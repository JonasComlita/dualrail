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

void expectFsckOk(const sandbox::os::FsConsistencyReport& report,
                  const std::string& message) {
    if (report.ok()) return;
    ++g_failures;
    std::cout << "FAIL: " << message;
    if (!report.errors.empty()) {
        std::cout << " first_error='" << report.errors.front() << "'";
    }
    std::cout << "\n";
}

bool containsWord(const std::vector<long long>& words, long long needle) {
    for (long long word : words) {
        if (word == needle) return true;
    }
    return false;
}

void testFilesystemCheckerAndBootRecovery() {
    std::cout << "[1] Filesystem checker and boot recovery\n";
    using namespace sandbox::os;

    OSKernel kernel(160);
    expect(kernel.boot().ok(), "kernel boots before fsck");
    expect(kernel.fs().createFile("/etc", InodeKind::Directory).ok(),
           "etc directory creates");
    expect(kernel.fs().createFile("/etc/config", InodeKind::File).ok(),
           "config file creates");
    expect(kernel.fs().writeFile("/etc/config", {84, 82, 73, 84}).ok(),
           "config file writes");
    expect(kernel.fs().createFile("/bin", InodeKind::Directory).ok(),
           "bin directory creates");

    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 2;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 32;
    expect(kernel.installExecutable("/bin/init", {900, 901, 902}, header).ok(),
           "executable installs with metadata before fsck");

    expectFsckOk(kernel.checkFilesystemConsistency(),
                 "filesystem consistency checker accepts a clean image");

    std::vector<long long> image = kernel.diskImage();
    expect(!image.empty(), "disk image exists before corruption");
    image[0] = 0;
    OSKernel broken(image);
    expect(!broken.boot().ok(), "corrupt superblock fails normal boot");

    BootRecoveryReport recovered = broken.bootWithRecovery();
    expect(recovered.mode == BootMode::Recovery, "boot recovery mode is selected");
    expect(recovered.status.ok(), "boot recovery formats a rescue root");
    expectFsckOk(recovered.fsck, "recovered root passes fsck");
    std::vector<long long> recoveryLog;
    expect(broken.fs().readFile("/var/log/recovery", recoveryLog).ok(),
           "recovery mode writes a recovery log");
    expect(!recoveryLog.empty(), "recovery log records the boot failure reason");
}

void testJournalReplayEveryWritePhase() {
    std::cout << "[2] Journal replay for every write phase\n";
    using namespace sandbox::os;

    const std::vector<JournalWritePhase> phases = {
        JournalWritePhase::BeforeBegin,
        JournalWritePhase::AfterBegin,
        JournalWritePhase::AfterRecord,
        JournalWritePhase::AfterCommit,
        JournalWritePhase::AfterApply,
        JournalWritePhase::AfterCheckpoint,
    };

    for (JournalWritePhase phase : phases) {
        JournalReplayHarness journal(4);
        expect(journal.seed(1, 10).ok(), "journal seed succeeds");
        StatusResult crashed = journal.simulateWriteCrash(1, 99, phase);
        expect(crashed.ok() || crashed.status == T1_PENDING,
               "journal crash phase is represented");
        expect(journal.recover().ok(), "journal recovery succeeds");
        const bool committed =
            static_cast<int>(phase) >= static_cast<int>(JournalWritePhase::AfterCommit);
        expect(journal.read(1) == (committed ? 99 : 10),
               "journal recovery returns the correct phase result");
        expect(journal.pendingRecords() == 0, "journal recovery drains records");
    }
}

void testMemoryPressureAndProfilerHooks() {
    std::cout << "[3] Memory pressure and profiler hooks\n";
    using namespace sandbox;

    vm::ProductionProfile profile = vm::ProductionProfile::minimum();
    vm::VMState machine(profile);
    const int high_dmem = profile.ram_words - 3;
    const int middle_dmem = profile.ram_words / 2;
    expect(machine.dmem.store(0, vm::ops::fromLong(1)) == vm::MemFaultCode::OK,
           "memory pressure stores low sparse page");
    expect(machine.dmem.store(middle_dmem, vm::ops::fromLong(2)) == vm::MemFaultCode::OK,
           "memory pressure stores middle sparse page");
    expect(machine.dmem.store(high_dmem, vm::ops::fromLong(3)) == vm::MemFaultCode::OK,
           "memory pressure stores high sparse page");
    expect(machine.dmem.allocatedPages() <= 3,
           "memory pressure leaves untouched RAM unallocated");

    sandbox::isa::TritWord27 marker{};
    marker.bits = 1234;
    expect(machine.imem.write(profile.instruction_words - 4, marker) ==
               vm::MemFaultCode::OK,
           "instruction memory pressure writes near the production limit");
    expect(machine.imem.allocatedPages() <= 1,
           "instruction pressure keeps IMEM sparse");

    auto assembled = vm::assembler::assemble(R"(
        .text
        start:
            mov r1, 65
            syscall 22
            syscall 2
            halt
    )");
    expect(assembled.success, "profiler test program assembles");
    vm::VMState profiled(64, 512);
    expect(profiled.imem.loadProgram(assembled.program), "profiler program loads");
    vm::PerformanceCounters counters;
    vm::VMHooks hooks = vm::makeProfilerHooks(counters);
    vm::RunResult result = vm::run(profiled, 32, &hooks);
    expect(result.halted(), "profiled program halts");
    expect(counters.steps == 4 && counters.syscalls == 2 && counters.halts == 1,
           "profiler hooks count steps, syscalls, and halt");
    expect(profiled.syscall_buffer == "A\n", "profiled syscalls still execute normally");

    auto loopProgram = vm::assembler::assemble(R"(
        .text
        start:
            mov r1, 65
            syscall 22
            mov r4, 3
            mov r5, 1
        loop:
            sub r4, r4, r5
            brp r4, loop
            syscall 2
            halt
    )");
    expect(loopProgram.success, "execution profile loop program assembles");
    vm::VMState loopVm(64, 512);
    expect(loopVm.imem.loadProgram(loopProgram.program), "execution profile loop program loads");
    loopVm.setCoreCurrentProcess(0, 42);
    vm::VMExecutionProfile execProfile;
    vm::VMHooks profileHooks = vm::makeProfilerHooks(execProfile);
    result = vm::run(loopVm, 64, &profileHooks);
    expect(result.halted(), "execution profile loop program halts");
    expect(execProfile.total_instructions == 12, "execution profile counts every executed instruction");
    expect(execProfile.opcodeCount(sandbox::isa::Opcode::SYSCALL) == 2,
           "execution profile counts syscalls per opcode");
    expect(execProfile.pcCount(4) == 3 && execProfile.pcCount(5) == 3,
           "execution profile records hot PCs");
    const vm::BranchProfileCounts branch =
        execProfile.branchCounts(5, sandbox::isa::Opcode::BRP, 4);
    expect(branch.taken == 2 && branch.fallthrough == 1,
           "execution profile records branch taken and fallthrough counts");
    expect(execProfile.syscallCount(22, 0, 42) == 1 && execProfile.syscallCount(2, 0, 42) == 1,
           "execution profile records syscall id and process id");
    const std::string json = execProfile.toJson(20);
    expect(json == execProfile.toJson(20), "execution profile JSON output is deterministic");
    expect(json.find("\"top_pcs\"") != std::string::npos &&
               json.find("\"top_branches\"") != std::string::npos &&
               json.find("\"top_syscalls\"") != std::string::npos,
           "execution profile JSON reports hot PCs, branches, and syscalls");
    const std::string text = execProfile.toText(20);
    expect(text.find("hot PCs") != std::string::npos &&
               text.find("hot branches") != std::string::npos &&
               text.find("hot syscalls") != std::string::npos,
           "execution profile text report names hot sections");

    auto trapProgram = vm::assembler::assemble(R"(
        .text
        start:
            syscall 99
            halt
    )");
    expect(trapProgram.success, "execution profile trap program assembles");
    vm::VMState trapVm(64, 512);
    expect(trapVm.imem.loadProgram(trapProgram.program), "execution profile trap program loads");
    trapVm.setCoreCurrentProcess(0, 7);
    vm::VMExecutionProfile trapProfile;
    vm::VMHooks trapHooks = vm::makeProfilerHooks(trapProfile);
    result = vm::run(trapVm, 8, &trapHooks);
    expect(result.trapped(), "execution profile trap program traps");
    expect(trapProfile.syscallCount(99, vm::OS_CAUSE_ILLEGAL_INSTRUCTION, 7) == 1,
           "execution profile records trapping syscall cause");
    expect(trapProfile.trapCount(99, vm::OS_CAUSE_ILLEGAL_INSTRUCTION, 7) == 1,
           "execution profile records trap cause and process id");
}

void testIsolationCapabilitiesAndSyscallFuzzing() {
    std::cout << "[4] Isolation, capabilities, and syscall fuzzing\n";
    using namespace sandbox::os;

    OSKernel kernel(192);
    expect(kernel.boot().ok(), "capability test kernel boots");
    expect(kernel.fs().createFile("/tmp", InodeKind::Directory).ok(),
           "tmp directory creates");
    expect(kernel.fs().createFile("/tmp/data", InodeKind::File).ok(),
           "data file creates");
    expect(kernel.fs().writeFile("/tmp/data", {1, 2, 3}).ok(),
           "data file writes");

    constexpr int kParentPid = 1;
    Process* parent = kernel.process(kParentPid);
    expect(parent != nullptr, "parent process exists");
    parent->memory.resize(16, 0);
    parent->memory[4] = 111;
    StatusResult forked = kernel.sysFork(kParentPid);
    expect(forked.ok(), "fork creates child for isolation test");
    const int childPid = forked.payload;
    parent = kernel.process(kParentPid);
    Process* child = kernel.process(childPid);
    expect(child != nullptr, "child process exists");
    parent->memory[4] = 222;
    expect(child->memory[4] == 111, "forked process memory is isolated");
    expect(kernel.checkProcessIsolation().ok(), "process isolation checker accepts fork state");

    expect(kernel.setProcessCapabilities(childPid, CAP_FILE_READ).ok(),
           "child capabilities can be restricted");
    StatusResult readOnly = kernel.sysOpen(childPid, "/tmp/data", false);
    expect(readOnly.ok(), "read capability allows opening a file read-only");
    expect(kernel.sysOpen(childPid, "/tmp/data", true).detail == ERR_ACCESS,
           "missing write capability blocks writable open");
    expect(kernel.createWindow(childPid, 8, 8).detail == ERR_ACCESS,
           "missing window capability blocks window creation");
    expect(kernel.sysIpcSend(childPid, kParentPid, 99).detail == ERR_ACCESS,
           "missing IPC capability blocks send");
    expect(kernel.sysKillFrom(childPid, kParentPid, sandbox::vm::SIGNAL_TERM).detail == ERR_ACCESS,
           "missing process-control capability blocks cross-process kill");

    expect(kernel.setProcessCapabilities(childPid, CAP_FILE_READ | CAP_IPC).ok(),
           "child receives explicit IPC capability");
    expect(kernel.sysIpcSend(kParentPid, childPid, 4242).ok(),
           "parent sends IPC message");
    long long payload = 0;
    StatusResult received = kernel.sysIpcRecv(childPid, payload);
    expect(received.ok() && received.payload == kParentPid && payload == 4242,
           "child receives IPC only after capability grant");

    std::vector<long long> out;
    for (int i = 0; i < 96; ++i) {
        expect(!kernel.sysRead(-1000 - i, i, 1, out).ok(),
               "syscall fuzzing rejects invalid read pid");
        expect(!kernel.sysClose(kParentPid, -1 - i).ok(),
               "syscall fuzzing rejects invalid fd");
        expect(!kernel.sysBrk(kParentPid, -10 - i).ok(),
               "syscall fuzzing rejects invalid brk target");
        expect(!kernel.sysKillFrom(-2000 - i, kParentPid, sandbox::vm::SIGNAL_TERM).ok(),
               "syscall fuzzing rejects invalid process-control caller");
    }

    expect(kernel.sysKillFrom(kParentPid, childPid, sandbox::vm::SIGNAL_KILL).ok(),
           "authorized parent can kill child");
    expect(kernel.sysWaitPid(kParentPid, childPid).ok(),
           "parent reaps killed child");
    expect(kernel.checkProcessIsolation().ok(),
           "process isolation checker accepts post-fuzz cleanup");
}

void testSignedPackagesAndReleaseImageBuilder() {
    std::cout << "[5] Signed package/update format and release image builder\n";
    using namespace sandbox::os;

    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 3;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 64;
    const std::vector<long long> app = {700, 701, 702, 703};
    const std::string secret = "production-signing-key";
    SignedExecutableMetadata metadata =
        signExecutableMetadata(app, header, "trit-release", secret);
    expect(verifySignedExecutableMetadata(app, header, metadata, secret),
           "signed executable metadata verifies");

    std::vector<long long> tampered = app;
    tampered[0] = 999;
    expect(!verifySignedExecutableMetadata(tampered, header, metadata, secret),
           "signed executable metadata rejects tampered payload");

    PackageImageBuilder packageBuilder("base", 42);
    expect(packageBuilder.addFile("/etc/motd", {84, 82, 73, 84}).ok(),
           "package records a configuration file");
    expect(packageBuilder.addExecutable("/bin/app", app, header, metadata).ok(),
           "package records a signed executable");
    PackageImage package = packageBuilder.build();
    std::vector<long long> manifest = encodePackageManifest(package);
    expect(manifest.size() > 4 &&
               manifest[0] == PACKAGE_MAGIC &&
               manifest[1] == PACKAGE_FORMAT_VERSION &&
               manifest[2] == 42,
           "package manifest encodes format and update epoch");

    OSKernel kernel(192);
    expect(kernel.boot().ok(), "package install kernel boots");
    expect(installPackage(kernel, package).ok(), "signed package installs");
    std::vector<long long> storedManifest;
    expect(kernel.fs().readFile("/var/packages/base.manifest", storedManifest).ok(),
           "package manifest persists in root filesystem");
    expect(storedManifest == manifest, "persisted package manifest matches encoded format");
    std::vector<long long> signatureSidecar;
    expect(kernel.fs().readFile("/bin/app.sig", signatureSidecar).ok(),
           "signed executable metadata sidecar persists");
    expect(kernel.sysExec(1, "/bin/app").ok(), "installed package executable execs");
    const Process* proc = kernel.process(1);
    expect(proc != nullptr && proc->memory == app,
           "installed package executable payload is loaded by exec");
    expectFsckOk(kernel.checkFilesystemConsistency(),
                 "package install leaves filesystem consistent");

    PackageImage tamperedPackage = package;
    tamperedPackage.entries[1].words[0] = 12345;
    OSKernel rejected(192);
    expect(rejected.boot().ok(), "tampered package test kernel boots");
    expect(installPackage(rejected, tamperedPackage).detail == ERR_SIGNATURE,
           "package installer rejects executable payload/signature mismatch");

    ReleaseImageBuilder release("trit-production", 42, 224);
    expect(release.addPackage(package).ok(), "release image builder accepts signed package");
    std::vector<long long> releaseImage = release.image();
    expect(!releaseImage.empty() && releaseImage[0] == NATIVE_VFS_MAGIC,
           "release image uses native VFS superblock");
    expect(containsWord(releaseImage, RELEASE_IMAGE_MAGIC),
           "release image embeds release metadata");
    expect(containsWord(releaseImage, PACKAGE_MAGIC),
           "release image embeds package manifest metadata");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testFilesystemCheckerAndBootRecovery();
    testJournalReplayEveryWritePhase();
    testMemoryPressureAndProfilerHooks();
    testIsolationCapabilitiesAndSyscallFuzzing();
    testSignedPackagesAndReleaseImageBuilder();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " production hardening failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nProduction hardening tests passed\n";
    return EXIT_SUCCESS;
}
