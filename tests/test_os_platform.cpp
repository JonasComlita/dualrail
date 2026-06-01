#include "ternary_os.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool hasDiagnostic(
    const std::vector<sandbox::compiler::Diagnostic>& diagnostics,
    const std::string& needle) {

    for (const auto& diagnostic : diagnostics) {
        if (contains(diagnostic.message, needle) || contains(diagnostic.format(), needle)) {
            return true;
        }
    }
    return false;
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

long long readCsrLong(sandbox::vm::VMState& vm, int csr) {
    sandbox::vm::TernaryValue value;
    if (!vm.readCSR(csr, value)) return -999999;
    return sandbox::vm::ops::toLong(value);
}

bool writeCsrLong(sandbox::vm::VMState& vm, int csr, long long value) {
    return vm.writeCSR(csr, sandbox::vm::ops::fromLong(value));
}

void testDeviceTreeAndBlockDevice() {
    std::cout << "[1] Trit OS device tree and block storage\n";
    using namespace sandbox::os;

    DeviceTree tree = defaultDeviceTree(32);
    std::vector<std::string> errors;
    expect(tree.validate(&errors).ok(), "default device tree validates");
    expect(tree.find("block0") != nullptr, "block device node is discoverable");

    tree.add(DeviceNode{"console", "trit,console-v1", {}});
    errors.clear();
    expect(!tree.validate(&errors).ok(), "duplicate device node fails validation");
    expect(!errors.empty() && contains(errors[0], "duplicate"), "duplicate diagnostic is clear");

    DeviceTree badGeometry;
    badGeometry.add(DeviceNode{"console", "trit,console-v1", {}});
    badGeometry.add(DeviceNode{"timer", "trit,timer-v1", {}});
    badGeometry.add(DeviceNode{"block0", "trit,block-v1",
                               {{"block_words", 9}, {"block_count", 4}}});
    expect(!badGeometry.validate().ok(), "invalid block geometry fails validation");

    BlockDevice device(4);
    std::vector<long long> block(BLOCK_WORDS, 0);
    block[0] = 17;
    block[26] = -5;
    expect(device.writeBlock(2, block).ok(), "block write succeeds");
    expect(device.dirty(2), "block write marks dirty state");
    std::vector<long long> readback;
    expect(device.readBlock(2, readback).ok(), "block read succeeds");
    expect(readback == block, "block read returns written payload");
    expect(!device.readBlock(9, readback).ok(), "out-of-range block read fails");
    expect(device.serialize() == device.serialize(), "disk serialization is deterministic");

    sandbox::vm::VMState vm(64, 512);
    expect(readCsrLong(vm, sandbox::isa::CSR_BLOCK_COUNT) >= 141,
           "VM block device exposes enough default blocks for native VFS persistence");
    expect(readCsrLong(vm, sandbox::isa::CSR_BLOCK_WORDS) == sandbox::vm::MMU_PAGE_WORDS,
           "VM block device exposes ternary page-sized blocks");
    for (int i = 0; i < sandbox::vm::MMU_PAGE_WORDS; ++i) {
        expect(vm.dmem.store(200 + i, sandbox::vm::ops::fromLong(500 + i)) ==
                   sandbox::vm::MemFaultCode::OK,
               "VM block write seed stores to DMEM");
    }
    expect(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_INDEX, 3), "block index CSR writes");
    expect(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_ADDR, 200), "block addr CSR writes");
    expect(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_CMD, 2), "block write command accepts");
    expect(readCsrLong(vm, sandbox::isa::CSR_BLOCK_STATUS) == 1,
           "block write command succeeds");
    expect(vm.block_dirty[3], "block write marks VM block dirty");
    for (int i = 0; i < sandbox::vm::MMU_PAGE_WORDS; ++i) {
        (void)vm.dmem.store(240 + i, sandbox::vm::ops::fromLong(0));
    }
    expect(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_ADDR, 240), "block read addr CSR writes");
    expect(writeCsrLong(vm, sandbox::isa::CSR_BLOCK_CMD, 1), "block read command accepts");
    expect(readCsrLong(vm, sandbox::isa::CSR_BLOCK_STATUS) == 1,
           "block read command succeeds");
    auto [loaded, fault] = vm.dmem.load(240 + 26);
    expect(fault == sandbox::vm::MemFaultCode::OK &&
               sandbox::vm::ops::toLong(loaded) == 526,
           "block read command transfers persisted words into DMEM");
    std::vector<long long> vmDisk = vm.blockImage();
    sandbox::vm::VMState rebooted(64, 512);
    expect(rebooted.loadBlockImage(vmDisk), "VM block image reloads into new VM");
    expect(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_INDEX, 3), "reboot block index writes");
    expect(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_ADDR, 260), "reboot block addr writes");
    expect(writeCsrLong(rebooted, sandbox::isa::CSR_BLOCK_CMD, 1), "reboot block read accepts");
    auto [reloaded, reloadFault] = rebooted.dmem.load(260);
    expect(reloadFault == sandbox::vm::MemFaultCode::OK &&
               sandbox::vm::ops::toLong(reloaded) == 500,
           "VM block image survives VM reboot");
}

void testTinyFileSystem() {
    std::cout << "[2] Trit OS tiny filesystem\n";
    using namespace sandbox::os;

    BlockDevice device(96);
    TinyFileSystem fs;
    expect(fs.format(device).ok(), "filesystem formats a disk image");
    expect(fs.mount(device).ok(), "filesystem mounts formatted disk image");
    expect(fs.createFile("/bin", InodeKind::Directory).ok(), "directory creation succeeds");
    expect(fs.createFile("/bin/app", InodeKind::Executable, true).ok(),
           "executable inode creation succeeds");

    std::vector<long long> app = {1, 2, 3, 4, 5};
    expect(fs.writeFile("/bin/app", app).ok(), "file write succeeds");
    std::vector<long long> out;
    expect(fs.readFile("/bin/app", out).ok(), "file read succeeds");
    expect(out == app, "file read returns stored words");

    std::vector<DirectoryEntry> entries;
    expect(fs.readdir("/bin", entries).ok(), "readdir succeeds");
    bool sawApp = false;
    for (const auto& entry : entries) sawApp = sawApp || entry.name == "app";
    expect(sawApp, "directory lists child executable");

    FileStat stat;
    expect(fs.stat("/bin/app", stat).ok(), "stat succeeds");
    expect(stat.kind == InodeKind::Executable && stat.size_words == 5,
           "stat reports executable file metadata");
    expect(!fs.createFile("/bin/app").ok(), "duplicate file creation fails");
    expect(!fs.lookup("/missing").ok(), "missing path lookup fails");

    expect(fs.createFile("/large").ok(), "large file inode creation succeeds");
    std::vector<long long> large(static_cast<std::size_t>(DIRECT_BLOCKS * BLOCK_WORDS + 3), 7);
    expect(fs.writeFile("/large", large).ok(), "large file write succeeds");
    expect(fs.stat("/large", stat).ok(), "large file stat succeeds");
    expect(stat.direct_blocks == DIRECT_BLOCKS && stat.indirect_block >= 0,
           "large file allocates direct blocks and an indirect block");
}

void testSyscallsHeapForkAndExec() {
    std::cout << "[3] Trit OS syscall facade, heap, fork, and exec\n";
    using namespace sandbox::os;

    OSKernel kernel(128);
    expect(kernel.boot().ok(), "OS kernel facade boots from device tree");
    constexpr int kPid = 1;
    expect(kernel.fs().createFile("/tmp", InodeKind::Directory).ok(), "tmp directory creates");
    expect(kernel.fs().createFile("/tmp/data").ok(), "data file creates");
    expect(kernel.fs().writeFile("/tmp/data", {10, 20, 30}).ok(), "data file seeds");

    StatusResult open = kernel.sysOpen(kPid, "/tmp/data", true);
    expect(open.ok() && open.payload >= 3, "open returns fd payload");
    std::vector<long long> read;
    expect(kernel.sysRead(kPid, open.payload, 2, read).ok(), "read syscall succeeds");
    expect(read.size() == 2 && read[0] == 10 && read[1] == 20, "read returns fd data");
    expect(kernel.sysWrite(kPid, open.payload, {44, 55}).ok(), "write syscall succeeds");
    expect(kernel.sysClose(kPid, open.payload).ok(), "close syscall succeeds");
    expect(!kernel.sysRead(kPid, open.payload, 1, read).ok(), "closed fd cannot be read");

    Process* proc = kernel.process(kPid);
    expect(proc != nullptr, "initial process exists");
    const int oldBreak = proc->heap_break;
    expect(kernel.sysSbrk(kPid, 6).ok(), "sbrk grows heap");
    expect(proc->heap_break == oldBreak + 6, "sbrk updates heap break");
    expect(!kernel.sysBrk(kPid, proc->heap_limit + 1).ok(), "brk beyond limit fails");
    expect(proc->heap_break == oldBreak + 6, "failed brk leaves heap unchanged");

    UserPtr<long long> allocation;
    expect(kernel.mallocWords(kPid, 3, allocation).ok(), "malloc wrapper uses sbrk");
    expect(allocation.canDeref(), "malloc exposes a valid user pointer");

    proc->memory.resize(16, 0);
    proc->memory[3] = 777;
    StatusResult forked = kernel.sysFork(kPid);
    expect(forked.ok() && forked.payload > kPid, "fork returns child pid to parent");
    proc = kernel.process(kPid);
    Process* child = kernel.process(forked.payload);
    expect(child != nullptr && child->parent_pid == kPid, "fork creates child process");
    proc->memory[3] = 888;
    expect(child->memory[3] == 777 && child->fork_return_payload == 0,
           "fork copies memory and records child return payload");

    const int childPid = forked.payload;
    StatusResult grandchild = kernel.sysFork(childPid);
    expect(grandchild.ok(), "child can fork a grandchild for orphan cleanup");
    ProcessInfo info;
    expect(kernel.sysGetProc(childPid, info).ok() &&
               info.parent_pid == kPid &&
               info.state == sandbox::vm::PROC_STATE_RUNNABLE,
           "getproc reports parent and runnable state");
    expect(kernel.sysSuspend(childPid).ok() &&
               kernel.process(childPid)->state == sandbox::vm::PROC_STATE_STOPPED,
           "suspend moves a process to stopped");
    expect(kernel.sysResume(childPid).ok() &&
               kernel.process(childPid)->state == sandbox::vm::PROC_STATE_RUNNABLE,
           "resume makes a stopped process runnable");
    expect(kernel.createWindow(childPid, 2, 2).ok(), "child window creates before kill");
    expect(kernel.windowCount() == 1, "window table records child window");
    StatusResult childFd = kernel.sysOpen(childPid, "/tmp/data", true);
    expect(childFd.ok(), "child opens fd before kill");
    expect(kernel.sysKill(childPid, sandbox::vm::SIGNAL_KILL).ok(),
           "kill signal succeeds");
    child = kernel.process(childPid);
    expect(child != nullptr &&
               child->state == sandbox::vm::PROC_STATE_ZOMBIE &&
               child->fds.empty() &&
               child->memory.empty() &&
               kernel.windowCount() == 0,
           "killed child releases files, memory, and windows while waiting for parent");
    Process* orphan = kernel.process(grandchild.payload);
    expect(orphan != nullptr && orphan->parent_pid == -1,
           "killing a parent orphans live children");
    expect(kernel.sysKill(grandchild.payload, sandbox::vm::SIGNAL_KILL).ok() &&
               kernel.process(grandchild.payload) == nullptr,
           "orphaned killed child is reaped without a zombie leak");
    expect(kernel.sysWaitPid(kPid, childPid).ok() &&
               kernel.process(childPid) == nullptr,
           "parent wait reaps killed zombie child");

    expect(kernel.fs().createFile("/bin", InodeKind::Directory).ok(), "bin directory creates");
    expect(kernel.fs().createFile("/bin/app", InodeKind::Executable, true).ok(),
           "exec file creates");
    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 2;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 24;
    expect(kernel.fs().writeFile("/bin/app", {99, 100}).ok(), "exec file payload writes");
    expect(kernel.fs().markExecutable("/bin/app", header).ok(), "executable metadata attaches");
    StatusResult exec = kernel.sysExec(kPid, "/bin/app");
    proc = kernel.process(kPid);
    expect(exec.ok() && exec.payload == 2, "exec returns new entry pc");
    expect(proc->parent_pid == -1 && proc->exec_header.entry_virtual_pc == 2,
           "exec preserves pid lineage and installs executable header");
}

void testDiskBackedSystemStateSurvivesReboot() {
    std::cout << "[4] Trit OS disk-backed system state\n";
    using namespace sandbox::os;

    OSKernel kernel(128);
    expect(kernel.boot().ok(), "fresh kernel boots mounted root filesystem");
    expect(kernel.fs().createFile("/home", InodeKind::Directory).ok(), "home directory creates");
    expect(kernel.fs().createFile("/home/note").ok(), "note file creates");
    expect(kernel.fs().writeFile("/home/note", {84, 82, 73, 84}).ok(), "note file writes");

    expect(kernel.fs().createFile("/large").ok(), "large persisted file creates");
    std::vector<long long> large(static_cast<std::size_t>(DIRECT_BLOCKS * BLOCK_WORDS + 11), 0);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<long long>(1000 + i);
    }
    expect(kernel.fs().writeFile("/large", large).ok(),
           "large file spanning indirect blocks writes");

    expect(kernel.fs().createFile("/bin", InodeKind::Directory).ok(), "bin directory creates");
    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 7;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 32;
    std::vector<long long> app = {9001, 9002, 9003};
    expect(kernel.installExecutable("/bin/app", app, header).ok(),
           "executable image installs into root filesystem");
    expect(kernel.shutdownSync().ok(), "kernel syncs filesystem before shutdown");
    std::vector<long long> image = kernel.diskImage();
    expect(!image.empty(), "disk image snapshot is non-empty");

    OSKernel rebooted(image);
    expect(rebooted.boot().ok(), "rebooted kernel mounts previous disk image");
    std::vector<long long> note;
    expect(rebooted.fs().readFile("/home/note", note).ok(), "note survives reboot");
    expect(note == std::vector<long long>({84, 82, 73, 84}), "note payload survives reboot");
    std::vector<long long> largeRead;
    expect(rebooted.fs().readFile("/large", largeRead).ok(), "large file survives reboot");
    expect(largeRead == large, "indirect file payload survives reboot");
    FileStat stat;
    expect(rebooted.fs().stat("/large", stat).ok(), "large file stat survives reboot");
    expect(stat.direct_blocks == DIRECT_BLOCKS && stat.indirect_block >= 0,
           "large file keeps direct and indirect metadata");

    expect(rebooted.sysExec(1, "/bin/app").ok(), "installed executable execs after reboot");
    const Process* proc = rebooted.process(1);
    expect(proc != nullptr && proc->exec_header.entry_virtual_pc == 7,
           "exec metadata survives disk image reboot");
    expect(proc != nullptr && proc->memory == app, "exec payload survives disk image reboot");

    expect(rebooted.fs().createFile("/home/after").ok(), "post-reboot file creates");
    expect(rebooted.fs().writeFile("/home/after", {1, 2, 3}).ok(), "post-reboot file writes");
    expect(rebooted.shutdownSync().ok(), "rebooted kernel syncs cleanly");
    OSKernel rebootedAgain(rebooted.diskImage());
    expect(rebootedAgain.boot().ok(), "second reboot mounts disk image");
    std::vector<long long> after;
    expect(rebootedAgain.fs().readFile("/home/after", after).ok(),
           "post-reboot write survives second reboot");
    expect(after == std::vector<long long>({1, 2, 3}), "post-reboot payload persists");
    largeRead.clear();
    expect(rebootedAgain.fs().readFile("/large", largeRead).ok(),
           "large file remains readable after allocating new file");
    expect(largeRead == large, "block allocation does not reuse persisted file blocks");
}

void testRootFilesystemImageBuilder() {
    std::cout << "[5] Trit OS root filesystem image builder\n";
    using namespace sandbox::os;

    RootFsImageBuilder builder(160, 40);
    expect(builder.status().ok(), "root filesystem builder formats a disk image");
    expect(builder.installBaseLayout().ok(), "base filesystem layout installs");

    sandbox::vm::ExecutableImageHeader calcHeader;
    calcHeader.entry_virtual_pc = 12;
    calcHeader.text_pages = 2;
    calcHeader.data_pages = 1;
    calcHeader.stack_words = 64;
    std::vector<long long> calcImage = {700, 701, 702, 703};
    expect(builder.addExecutable("/bin/calculator", calcImage, calcHeader).ok(),
           "calculator executable installs into image");
    expect(builder.addFile("/etc/motd", {84, 114, 105, 116}).ok(),
           "configuration file installs into image");
    expect(builder.addUserRecord("root", 333667, "/home/root", "/bin/calculator").ok(),
           "user account record installs into image");

    std::vector<long long> image = builder.image();
    OSKernel kernel(image);
    expect(kernel.boot().ok(), "kernel boots from built root filesystem image");

    std::vector<DirectoryEntry> entries;
    expect(kernel.fs().readdir("/bin", entries).ok(), "booted image lists /bin");
    bool sawCalculator = false;
    for (const auto& entry : entries) {
        sawCalculator = sawCalculator || entry.name == "calculator";
    }
    expect(sawCalculator, "rootfs image contains calculator app");

    std::vector<long long> motd;
    expect(kernel.fs().readFile("/etc/motd", motd).ok(), "configuration file reads");
    expect(motd == std::vector<long long>({84, 114, 105, 116}),
           "configuration payload survives image build");

    std::vector<long long> users;
    expect(kernel.fs().readFile("/etc/users", users).ok(), "user database reads");
    expect(!users.empty() && users[0] == 4, "user database stores length-prefixed records");
    expect(kernel.fs().lookup("/home/root").ok(), "user home directory exists");

    expect(kernel.sysExec(1, "/bin/calculator").ok(),
           "app installed by image builder execs after boot");
    const Process* proc = kernel.process(1);
    expect(proc != nullptr && proc->exec_header.entry_virtual_pc == 12,
           "image-built executable metadata is available to exec");
    expect(proc != nullptr && proc->memory == calcImage,
           "image-built executable payload is available to exec");
}

void testNativeBioReadsRootFilesystemImage() {
    std::cout << "[6] Native BIO reads root filesystem image\n";
    using namespace sandbox::os;
    using namespace sandbox::compiler;

    RootFsImageBuilder builder(96, 32);
    expect(builder.installBaseLayout().ok(), "native BIO test rootfs layout installs");
    std::vector<long long> image = builder.image();

    const std::string bio = readTextFile("kernel/bio.trit");
    expect(!bio.empty(), "bio.trit is available to native BIO test");
    const std::string driver = R"(
        fn main() -> t40 {
            if bio_device_block_words() - 27 != 0 {
                return -1;
            }
            if bio_device_block_count() <= 0 {
                return -2;
            }
            if bio_device_read(0, 3000) - 1 != 0 {
                return -3;
            }
            if bio_load(3000) - 80808 != 0 {
                return -4;
            }
            if bio_load(3001) - 1 != 0 {
                return -5;
            }
            if bio_load(3002) - 27 != 0 {
                return -6;
            }
            return 1;
        }
    )";
    CompileResult compiled = compileSource("native_bio_rootfs.trit", bio + "\n" + driver);
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(compiled.success, "native BIO rootfs reader compiles");
    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "native BIO rootfs reader links");
    sandbox::vm::VMState vm(4096, 65536);
    expect(vm.loadBlockImage(image), "rootfs image loads into VM block device");
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program),
               "native BIO rootfs reader loads into VM");
        const auto result = sandbox::vm::run(vm, 1000000);
        expect(result.halted(), "native BIO rootfs reader halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(13)) == 1,
               "native BIO rootfs reader validates disk superblock");
    }
}

void testNativeKernelVfsMountsDiskBackedState() {
    std::cout << "[7] Native kernel VFS mounts disk-backed state\n";
    using namespace sandbox::compiler;

    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "kernel.trit is available to native VFS persistence test");

    const std::string writer = R"(
        fn seed_persist_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 112);
            kstore(addr + 2, 101);
            kstore(addr + 3, 114);
            kstore(addr + 4, 115);
            kstore(addr + 5, 105);
            kstore(addr + 6, 115);
            kstore(addr + 7, 116);
            kstore(addr + 8, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var path: t40 = USER_MEM_BASE;
            var src: t40 = USER_MEM_BASE + 32;
            seed_persist_path(path);
            kstore(src + 0, 11);
            kstore(src + 1, 22);
            kstore(src + 2, -33);
            kstore(src + 3, 44);
            var fd: t40 = vfs_open(1, path, 2);
            if fd < 0 { return -2; }
            if vfs_write(1, fd, src, 4) - 4 != 0 { return -3; }
            if kernel_syscall_dispatch(1, 47, fd, 0, 0, 0) - 1 != 0 { return -4; }
            if kload(SYS_PAYLOAD_ADDR) - 1 != 0 { return -5; }
            vfs_close(1, fd);
            return 1;
        }
    )";

    CompileResult writerCompiled = compileSource("native_vfs_persist_writer.trit", kernel + "\n" + writer);
    if (!writerCompiled.success) {
        for (const auto& diagnostic : writerCompiled.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(writerCompiled.success, "native VFS persistence writer compiles");
    LinkResult writerLinked = linkModules({writerCompiled.object});
    expect(writerLinked.success, "native VFS persistence writer links");
    sandbox::vm::VMState writerVm(262144, 1000000);
    if (writerLinked.success) {
        expect(sandbox::vm::loadAndReset(writerVm, writerLinked.assembled.program),
               "native VFS persistence writer loads");
        const auto writerResult = sandbox::vm::run(writerVm, 5000000);
        expect(writerResult.halted(), "native VFS persistence writer halts");
        expect(sandbox::vm::ops::toLong(writerVm.regfile.read(13)) == 1,
               "native VFS persistence writer syncs file to disk");
    }

    const std::vector<long long> diskImage = writerVm.blockImage();
    const std::string reader = R"(
        fn seed_persist_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 112);
            kstore(addr + 2, 101);
            kstore(addr + 3, 114);
            kstore(addr + 4, 115);
            kstore(addr + 5, 105);
            kstore(addr + 6, 115);
            kstore(addr + 7, 116);
            kstore(addr + 8, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var path: t40 = USER_MEM_BASE;
            var dst: t40 = USER_MEM_BASE + 64;
            seed_persist_path(path);
            var fd: t40 = vfs_open(1, path, 0);
            if fd < 0 { return -2; }
            if vfs_read(1, fd, dst, 4) - 4 != 0 { return -3; }
            if kload(dst + 0) - 11 != 0 { return -4; }
            if kload(dst + 1) - 22 != 0 { return -5; }
            if kload(dst + 2) + 33 != 0 { return -6; }
            if kload(dst + 3) - 44 != 0 { return -7; }
            return 1;
        }
    )";

    CompileResult readerCompiled = compileSource("native_vfs_persist_reader.trit", kernel + "\n" + reader);
    if (!readerCompiled.success) {
        for (const auto& diagnostic : readerCompiled.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(readerCompiled.success, "native VFS persistence reader compiles");
    LinkResult readerLinked = linkModules({readerCompiled.object});
    expect(readerLinked.success, "native VFS persistence reader links");
    sandbox::vm::VMState readerVm(262144, 1000000);
    expect(readerVm.loadBlockImage(diskImage), "native VFS disk image loads into rebooted VM");
    if (readerLinked.success) {
        expect(sandbox::vm::loadAndReset(readerVm, readerLinked.assembled.program),
               "native VFS persistence reader loads");
        const auto readerResult = sandbox::vm::run(readerVm, 5000000);
        expect(readerResult.halted(), "native VFS persistence reader halts");
        expect(sandbox::vm::ops::toLong(readerVm.regfile.read(13)) == 1,
               "native VFS persistence reader mounts and reads disk-backed file");
    }

    const std::string pending = R"(
        fn seed_persist_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 112);
            kstore(addr + 2, 101);
            kstore(addr + 3, 114);
            kstore(addr + 4, 115);
            kstore(addr + 5, 105);
            kstore(addr + 6, 115);
            kstore(addr + 7, 116);
            kstore(addr + 8, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var path: t40 = USER_MEM_BASE;
            seed_persist_path(path);
            var inode: t40 = vfs_lookup(0, path);
            if inode < 0 { return -2; }
            var slot: t40 = vfs_find_extent_covering(inode, 0);
            if slot < 0 { return -3; }
            var payload: t40 = kload(extent_addr(slot) + EXTENT_DATA_ADDR);
            var tx: t40 = log_begin();
            log_write(tx, payload, kload(payload), 99);
            return 1;
        }
    )";

    CompileResult pendingCompiled = compileSource("native_vfs_pending_wal.trit", kernel + "\n" + pending);
    expect(pendingCompiled.success, "native pending WAL crash writer compiles");
    LinkResult pendingLinked = linkModules({pendingCompiled.object});
    expect(pendingLinked.success, "native pending WAL crash writer links");
    sandbox::vm::VMState pendingVm(262144, 1000000);
    expect(pendingVm.loadBlockImage(diskImage), "pending WAL writer starts from synced disk");
    if (pendingLinked.success) {
        expect(sandbox::vm::loadAndReset(pendingVm, pendingLinked.assembled.program),
               "pending WAL writer loads");
        const auto pendingResult = sandbox::vm::run(pendingVm, 5000000);
        expect(pendingResult.halted(), "pending WAL writer halts");
        expect(sandbox::vm::ops::toLong(pendingVm.regfile.read(13)) == 1,
               "pending WAL writer persists an uncommitted journal record");
    }

    sandbox::vm::VMState pendingReaderVm(262144, 1000000);
    expect(pendingReaderVm.loadBlockImage(pendingVm.blockImage()),
           "pending WAL disk image loads into rebooted VM");
    if (readerLinked.success) {
        expect(sandbox::vm::loadAndReset(pendingReaderVm, readerLinked.assembled.program),
               "pending WAL recovery reader loads");
        const auto pendingReaderResult = sandbox::vm::run(pendingReaderVm, 5000000);
        expect(pendingReaderResult.halted(), "pending WAL recovery reader halts");
        expect(sandbox::vm::ops::toLong(pendingReaderVm.regfile.read(13)) == 1,
               "pending WAL recovery rolls back to synced file contents");
    }

    const std::string committed = R"(
        fn seed_persist_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 112);
            kstore(addr + 2, 101);
            kstore(addr + 3, 114);
            kstore(addr + 4, 115);
            kstore(addr + 5, 105);
            kstore(addr + 6, 115);
            kstore(addr + 7, 116);
            kstore(addr + 8, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var path: t40 = USER_MEM_BASE;
            seed_persist_path(path);
            var inode: t40 = vfs_lookup(0, path);
            if inode < 0 { return -2; }
            var slot: t40 = vfs_find_extent_covering(inode, 0);
            if slot < 0 { return -3; }
            var payload: t40 = kload(extent_addr(slot) + EXTENT_DATA_ADDR);
            var tx: t40 = log_begin();
            log_write(tx, payload, kload(payload), 77);
            if log_commit(tx) <= 0 { return -4; }
            return 1;
        }
    )";

    CompileResult committedCompiled = compileSource("native_vfs_committed_wal.trit", kernel + "\n" + committed);
    expect(committedCompiled.success, "native committed WAL crash writer compiles");
    LinkResult committedLinked = linkModules({committedCompiled.object});
    expect(committedLinked.success, "native committed WAL crash writer links");
    sandbox::vm::VMState committedVm(262144, 1000000);
    expect(committedVm.loadBlockImage(diskImage), "committed WAL writer starts from synced disk");
    if (committedLinked.success) {
        expect(sandbox::vm::loadAndReset(committedVm, committedLinked.assembled.program),
               "committed WAL writer loads");
        const auto committedResult = sandbox::vm::run(committedVm, 5000000);
        expect(committedResult.halted(), "committed WAL writer halts");
        expect(sandbox::vm::ops::toLong(committedVm.regfile.read(13)) == 1,
               "committed WAL writer persists committed journal without fsyncing VFS");
    }

    const std::string committedReader = R"(
        fn seed_persist_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 112);
            kstore(addr + 2, 101);
            kstore(addr + 3, 114);
            kstore(addr + 4, 115);
            kstore(addr + 5, 105);
            kstore(addr + 6, 115);
            kstore(addr + 7, 116);
            kstore(addr + 8, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var path: t40 = USER_MEM_BASE;
            var dst: t40 = USER_MEM_BASE + 64;
            seed_persist_path(path);
            var fd: t40 = vfs_open(1, path, 0);
            if fd < 0 { return -2; }
            if vfs_read(1, fd, dst, 4) - 4 != 0 { return -3; }
            if kload(dst + 0) - 77 != 0 { return -4; }
            if kload(dst + 1) - 22 != 0 { return -5; }
            if kload(dst + 2) + 33 != 0 { return -6; }
            if kload(dst + 3) - 44 != 0 { return -7; }
            return 1;
        }
    )";

    CompileResult committedReaderCompiled = compileSource("native_vfs_committed_wal_reader.trit", kernel + "\n" + committedReader);
    expect(committedReaderCompiled.success, "native committed WAL recovery reader compiles");
    LinkResult committedReaderLinked = linkModules({committedReaderCompiled.object});
    expect(committedReaderLinked.success, "native committed WAL recovery reader links");
    sandbox::vm::VMState committedReaderVm(262144, 1000000);
    expect(committedReaderVm.loadBlockImage(committedVm.blockImage()),
           "committed WAL disk image loads into rebooted VM");
    if (committedReaderLinked.success) {
        expect(sandbox::vm::loadAndReset(committedReaderVm, committedReaderLinked.assembled.program),
               "committed WAL recovery reader loads");
        const auto committedReaderResult = sandbox::vm::run(committedReaderVm, 5000000);
        expect(committedReaderResult.halted(), "committed WAL recovery reader halts");
        const long long committedReaderRet = sandbox::vm::ops::toLong(committedReaderVm.regfile.read(13));
        expect(committedReaderRet == 1,
               "committed WAL recovery replays committed journal into VFS image");
    }
}

void testNativeVfsImageBuilderBootsKernelRoot() {
    std::cout << "[8] Native VFS image builder boots kernel root\n";
    using namespace sandbox::os;
    using namespace sandbox::compiler;

    NativeVfsImageBuilder builder(192);
    expect(builder.status().ok(), "native VFS image builder formats a disk image");
    expect(builder.installBaseLayout().ok(), "native VFS base layout installs");
    expect(builder.addFile("/etc/motd", {84, 82, 73, 84}).ok(),
           "native VFS image carries configuration payload");

    sandbox::vm::ExecutableImageHeader appHeader;
    appHeader.entry_virtual_pc = 0;
    appHeader.text_pages = 1;
    appHeader.data_pages = 1;
    appHeader.stack_words = 64;
    constexpr int kDiskAppTextPpn = 720;
    auto appAssembly = sandbox::vm::assembler::assemble(R"(
        .text
        disk_app:
            mov r13, 123
            halt
    )");
    expect(appAssembly.success, "disk app text image assembles");
    expect(builder.addExecutableImage("/bin/disk_app",
                                      appAssembly.program,
                                      appHeader,
                                      kDiskAppTextPpn).ok(),
           "native VFS image carries executable descriptor and text pages");
    std::vector<long long> image = builder.image();
    expect(!image.empty() && image[0] == NATIVE_VFS_MAGIC,
           "native VFS image uses the kernel mount format");

    const std::string kernel = readTextFile("kernel.trit");
    expect(!kernel.empty(), "kernel.trit is available to native image boot test");
    const std::string driver = R"(
        fn seed_motd_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 101);
            kstore(addr + 2, 116);
            kstore(addr + 3, 99);
            kstore(addr + 4, 47);
            kstore(addr + 5, 109);
            kstore(addr + 6, 111);
            kstore(addr + 7, 116);
            kstore(addr + 8, 100);
            kstore(addr + 9, 0);
            return addr;
        }

        fn seed_app_path(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 98);
            kstore(addr + 2, 105);
            kstore(addr + 3, 110);
            kstore(addr + 4, 47);
            kstore(addr + 5, 100);
            kstore(addr + 6, 105);
            kstore(addr + 7, 115);
            kstore(addr + 8, 107);
            kstore(addr + 9, 95);
            kstore(addr + 10, 97);
            kstore(addr + 11, 112);
            kstore(addr + 12, 112);
            kstore(addr + 13, 0);
            return addr;
        }

        fn main() -> t40 {
            if kernel_init() - 1 != 0 { return -1; }
            var motd_path: t40 = USER_MEM_BASE;
            var app_path: t40 = USER_MEM_BASE + 32;
            var dst: t40 = USER_MEM_BASE + 96;
            seed_motd_path(motd_path);
            seed_app_path(app_path);
            var fd: t40 = vfs_open(1, motd_path, 0);
            if fd < 0 { return -2; }
            if vfs_read(1, fd, dst, 4) - 4 != 0 { return -3; }
            if kload(dst + 0) - 84 != 0 { return -4; }
            if kload(dst + 1) - 82 != 0 { return -5; }
            if kload(dst + 2) - 73 != 0 { return -6; }
            if kload(dst + 3) - 84 != 0 { return -7; }
            vfs_close(1, fd);
            if app_launch(1, app_path, 0) - EXEC_DESC_V2_WORDS != 0 { return -9; }
            if kload(exec_hw_imem_ptbr(0)) - exec_encode_pte(720, 1, 0, 0, 1) != 0 { return -10; }
            var ctx: t40 = kload(process_addr(0) + PROC_CONTEXT);
            if ctx <= 0 { return -11; }
            if kload(ctx + TASK_CONTEXT_EPC) != 0 { return -12; }
            if kload(ctx + TASK_CONTEXT_IMEM_PAGES) - 1 != 0 { return -13; }
            return 1;
        }
    )";

    CompileResult compiled = compileSource("native_vfs_image_builder_boot.trit", kernel + "\n" + driver);
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(compiled.success, "native VFS image boot driver compiles");
    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "native VFS image boot driver links");
    sandbox::vm::VMState vm(262144, 1000000);
    expect(vm.loadBlockImage(image), "native VFS image loads into VM block device");
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program),
               "native VFS image boot driver loads");
        const auto result = sandbox::vm::run(vm, 50000000);
        expect(result.halted(), "native VFS image boot driver halts");
        const long long bootRet = sandbox::vm::ops::toLong(vm.regfile.read(13));
        expect(bootRet == 123,
               "native kernel mounts image-built root and launches disk app image");
        expect(vm.imem.words[kDiskAppTextPpn * sandbox::vm::MMU_PAGE_WORDS] ==
                   appAssembly.program.front(),
               "native exec loads app text from disk into IMEM");
    }
}

void testSharedStatusAndCompilerWrappers() {
    std::cout << "[9] Trit OS T1 status and compiler syscall wrappers\n";
    using namespace sandbox::os;
    using namespace sandbox::compiler;

    SharedWord word;
    expect(word.tldr(SharedOrder::AcquireRelease) == 0, "shared word loads initial value");
    StatusResult mismatch = word.tstr(7, 1, SharedOrder::AcquireRelease);
    expect(mismatch.status == T1_PENDING, "tstr mismatch returns zero/pending status");
    StatusResult stored = word.tstr(7, 0, SharedOrder::AcquireRelease);
    expect(stored.status == T1_SUCCESS && word.value == 7, "tstr success returns positive status");

    expect(runtime::sys_open == SYSCALL_OPEN &&
           runtime::sys_sbrk == SYSCALL_SBRK &&
           runtime::sys_exec == SYSCALL_EXEC &&
           runtime::sys_fsync == SYSCALL_FSYNC &&
           runtime::sys_kill == SYSCALL_KILL &&
           runtime::sys_getproc == SYSCALL_GETPROC &&
           runtime::sys_futex_wait == SYSCALL_FUTEX_WAIT &&
           runtime::sys_wait_event == SYSCALL_WAIT_EVENT &&
           runtime::sys_sleep_ms == SYSCALL_SLEEP_MS,
           "compiler runtime exports OS syscall ids");
    expect(sandbox::vm::SYSCALL_OPEN == SYSCALL_OPEN &&
           sandbox::vm::SYSCALL_FORK == SYSCALL_FORK &&
           sandbox::vm::SYSCALL_EXEC == SYSCALL_EXEC &&
           sandbox::vm::SYSCALL_FSYNC == SYSCALL_FSYNC &&
           sandbox::vm::SYSCALL_KILL == SYSCALL_KILL &&
           sandbox::vm::SYSCALL_GETPROC == SYSCALL_GETPROC &&
           sandbox::vm::SYSCALL_FUTEX_WAKE == SYSCALL_FUTEX_WAKE &&
           sandbox::vm::SYSCALL_IPC_RECV_BLOCKING == SYSCALL_IPC_RECV_BLOCKING,
           "VM ABI constants reserve OS syscall ids");

    const std::string src = R"(
        fn main() -> t40 {
          let grown = sys_sbrk(9);
          let child = sys_fork();
          let status = sys_exec(0);
          let synced = sys_fsync(0);
          let killed = sys_kill(2, 2);
          let resumed = sys_resume(2);
          let info = sys_getproc(1, 10000);
          let slept = sys_sleep_ms(1);
          let woken = sys_futex_wake(10000, 1);
          let evented = sys_wait_event(-1, 10000, 1);
          return grown + child + status + synced + killed + resumed + info + slept + woken + evented;
        }
    )";
    CompileResult compiled = compileSource("os_wrappers.trit", src);
    expect(compiled.success, "OS syscall wrapper source compiles");
    if (!compiled.success) {
        expect(!hasDiagnostic(compiled.diagnostics, "unknown function"),
               "OS wrappers are known to type inference");
    }
    expect(contains(compiled.assembly, "syscall 19"), "sys_sbrk lowers to syscall 19");
    expect(contains(compiled.assembly, "syscall 20"), "sys_fork lowers to syscall 20");
    expect(contains(compiled.assembly, "syscall 21"), "sys_exec lowers to syscall 21");
    expect(contains(compiled.assembly, "syscall 48"), "sys_kill lowers to syscall 48");
    expect(contains(compiled.assembly, "syscall 50"), "sys_resume lowers to syscall 50");
    expect(contains(compiled.assembly, "syscall 51"), "sys_getproc lowers to syscall 51");
    expect(contains(compiled.assembly, "syscall 53"), "sys_futex_wake lowers to syscall 53");
    expect(contains(compiled.assembly, "syscall 55"), "sys_wait_event lowers to syscall 55");
    expect(contains(compiled.assembly, "syscall 56"), "sys_sleep_ms lowers to syscall 56");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testDeviceTreeAndBlockDevice();
    testTinyFileSystem();
    testSyscallsHeapForkAndExec();
    testDiskBackedSystemStateSurvivesReboot();
    testRootFilesystemImageBuilder();
    testNativeBioReadsRootFilesystemImage();
    testNativeKernelVfsMountsDiskBackedState();
    testNativeVfsImageBuilderBootsKernelRoot();
    testSharedStatusAndCompilerWrappers();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " Trit OS platform test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll Trit OS platform tests passed\n";
    return EXIT_SUCCESS;
}
