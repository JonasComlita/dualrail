#include "ternary_os.h"

#include <cstdlib>
#include <iostream>
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

void testSharedStatusAndCompilerWrappers() {
    std::cout << "[4] Trit OS T1 status and compiler syscall wrappers\n";
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
           runtime::sys_exec == SYSCALL_EXEC,
           "compiler runtime exports OS syscall ids");
    expect(sandbox::vm::SYSCALL_OPEN == SYSCALL_OPEN &&
           sandbox::vm::SYSCALL_FORK == SYSCALL_FORK &&
           sandbox::vm::SYSCALL_EXEC == SYSCALL_EXEC,
           "VM ABI constants reserve OS syscall ids");

    const std::string src = R"(
        fn main() -> t40 {
          let grown = sys_sbrk(9);
          let child = sys_fork();
          let status = sys_exec(0);
          return grown + child + status;
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
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testDeviceTreeAndBlockDevice();
    testTinyFileSystem();
    testSyscallsHeapForkAndExec();
    testSharedStatusAndCompilerWrappers();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " Trit OS platform test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll Trit OS platform tests passed\n";
    return EXIT_SUCCESS;
}
