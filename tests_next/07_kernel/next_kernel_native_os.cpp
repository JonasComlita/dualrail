#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_os.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

constexpr int kParentPid = 1;

void syscallFileAndHeapFacade(TestContext& ctx) {
    OSKernel kernel(128);
    ctx.check(kernel.boot().ok(), "kernel facade boots mounted root filesystem");
    ctx.check(kernel.fs().createFile("/tmp", InodeKind::Directory).ok(),
              "tmp directory creates");
    ctx.check(kernel.fs().createFile("/tmp/data", InodeKind::File).ok(),
              "data file creates");
    ctx.check(kernel.fs().writeFile("/tmp/data", {10, 20, 30}).ok(),
              "data file seeds");

    const StatusResult opened = kernel.sysOpen(kParentPid, "/tmp/data", true);
    ctx.check(opened.ok() && opened.payload >= 3, "writable open returns user fd");

    std::vector<long long> read;
    ctx.check(kernel.sysRead(kParentPid, opened.payload, 2, read).ok(),
              "read syscall succeeds");
    ctx.check(read.size() == 2 && read[0] == 10 && read[1] == 20,
              "read syscall advances fd data");
    ctx.check(kernel.sysWrite(kParentPid, opened.payload, {44, 55}).ok(),
              "write syscall appends at current fd offset");
    ctx.check(kernel.sysClose(kParentPid, opened.payload).ok(),
              "close syscall succeeds");
    ctx.equal(kernel.sysRead(kParentPid, opened.payload, 1, read).detail,
              ERR_BAD_FD, "closed fd is rejected");

    std::vector<long long> stored;
    ctx.check(kernel.fs().readFile("/tmp/data", stored).ok(),
              "written file reads back through VFS");
    ctx.check(stored == std::vector<long long>({10, 20, 44, 55}),
              "write syscall persists merged file words");

    Process* proc = kernel.process(kParentPid);
    ctx.check(proc != nullptr, "initial process exists");
    if (!proc) return;
    const int old_break = proc->heap_break;
    ctx.check(kernel.sysSbrk(kParentPid, 6).ok(), "sbrk grows heap");
    ctx.equal(proc->heap_break, old_break + 6, "heap break advances");
    const int before_bad_brk = proc->heap_break;
    ctx.equal(kernel.sysBrk(kParentPid, proc->heap_limit + 1).detail,
              ERR_NO_SPACE, "brk beyond limit is rejected");
    ctx.equal(proc->heap_break, before_bad_brk,
              "failed brk leaves heap unchanged");

    UserPtr<long long> allocation;
    ctx.check(kernel.mallocWords(kParentPid, 3, allocation).ok(),
              "malloc wrapper uses process heap");
    ctx.check(allocation.canDeref(), "malloc returns a dereferenceable user pointer");
}

void execInstallsImageMetadata(TestContext& ctx) {
    OSKernel kernel(128);
    ctx.check(kernel.boot().ok(), "exec test kernel boots");
    ctx.check(kernel.fs().createFile("/bin", InodeKind::Directory).ok(),
              "bin directory creates");

    const std::vector<long long> image = {90, 91, 92, 93};
    const auto header = sandbox::vm::makeExecutableHeaderV2(
        5, static_cast<int>(image.size()),
        2 * sandbox::vm::MMU_PAGE_WORDS, 36);
    ctx.check(kernel.installExecutable("/bin/app", image, header).ok(),
              "executable installs through kernel helper");

    const StatusResult exec = kernel.sysExec(kParentPid, "/bin/app");
    Process* proc = kernel.process(kParentPid);
    ctx.check(exec.ok() && exec.payload == header.entry_pc,
              "exec returns executable entry point");
    ctx.check(proc != nullptr, "exec leaves process alive");
    if (!proc) return;
    ctx.check(proc->memory == image, "exec loads executable words into process memory");
    ctx.equal(proc->exec_header.entry_pc, header.entry_pc,
              "exec header entry pc is installed");
    ctx.equal(proc->heap_start,
              sandbox::vm::executableDataPages(header) *
                  sandbox::vm::MMU_PAGE_WORDS,
              "exec resets heap start from image metadata");
    ctx.equal(proc->heap_break, proc->heap_start, "exec resets heap break");
    ctx.check(proc->heap_limit > proc->heap_break, "exec establishes heap limit");
}

void capabilityAndKillCleanup(TestContext& ctx) {
    OSKernel kernel(192);
    ctx.check(kernel.boot().ok(), "capability test kernel boots");
    ctx.check(kernel.fs().createFile("/tmp", InodeKind::Directory).ok(),
              "tmp directory creates");
    ctx.check(kernel.fs().createFile("/tmp/data", InodeKind::File).ok(),
              "data file creates");
    ctx.check(kernel.fs().writeFile("/tmp/data", {1, 2, 3}).ok(),
              "data file writes");

    Process* parent = kernel.process(kParentPid);
    ctx.check(parent != nullptr, "parent process exists");
    if (!parent) return;
    parent->memory.resize(12, 0);
    parent->memory[4] = 111;

    const StatusResult forked = kernel.sysFork(kParentPid);
    ctx.check(forked.ok(), "fork creates child for capability test");
    const int child_pid = forked.payload;
    parent = kernel.process(kParentPid);
    Process* child = kernel.process(child_pid);
    ctx.check(parent != nullptr && child != nullptr, "parent and child are live");
    if (!parent || !child) return;
    parent->memory[4] = 222;
    ctx.equal(child->memory[4], 111LL, "forked process memory is isolated");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "isolation checker accepts forked state");

    ctx.check(kernel.setProcessCapabilities(child_pid, CAP_FILE_READ).ok(),
              "child capabilities can be restricted");
    ctx.check(kernel.sysOpen(child_pid, "/tmp/data", false).ok(),
              "read capability permits read-only open");
    ctx.equal(kernel.sysOpen(child_pid, "/tmp/data", true).detail,
              ERR_ACCESS, "missing write capability blocks writable open");
    ctx.equal(kernel.createWindow(child_pid, 8, 8).detail,
              ERR_ACCESS, "missing window capability blocks windows");
    ctx.equal(kernel.sysIpcSend(child_pid, kParentPid, 99).detail,
              ERR_ACCESS, "missing IPC capability blocks send");
    ctx.equal(kernel.sysKillFrom(child_pid, kParentPid,
                                 sandbox::vm::SIGNAL_TERM).detail,
              ERR_ACCESS, "missing process control blocks cross-process kill");

    ctx.check(kernel.setProcessCapabilities(child_pid, CAP_FILE_READ | CAP_IPC).ok(),
              "child receives explicit IPC capability");
    ctx.check(kernel.sysIpcSend(kParentPid, child_pid, 4242).ok(),
              "parent sends IPC to child");
    long long payload = 0;
    const StatusResult received = kernel.sysIpcRecv(child_pid, payload);
    ctx.check(received.ok() && received.payload == kParentPid && payload == 4242,
              "child receives IPC after capability grant");

    ctx.check(kernel.setProcessCapabilities(child_pid, CAP_ALL).ok(),
              "child capabilities restore for cleanup witness");
    ctx.check(kernel.createWindow(child_pid, 2, 2).ok(),
              "child window creates before kill");
    ctx.check(kernel.sysOpen(child_pid, "/tmp/data", true).ok(),
              "child opens fd before kill");
    ctx.check(kernel.sysIpcSend(kParentPid, child_pid, 616).ok(),
              "queued IPC exists before kill cleanup");
    ctx.equal(kernel.windowCount(), 1, "window table records child window");
    ctx.equal(kernel.ipcMessageCount(), 1, "IPC queue records child message");

    ctx.check(kernel.sysKillFrom(kParentPid, child_pid,
                                 sandbox::vm::SIGNAL_KILL).ok(),
              "authorized parent kills child");
    child = kernel.process(child_pid);
    ctx.check(child != nullptr, "killed child remains waitable zombie");
    if (!child) return;
    ctx.equal(child->state, sandbox::vm::PROC_STATE_ZOMBIE,
              "killed child becomes zombie while parent is alive");
    ctx.check(child->fds.empty(), "kill cleanup closes child fds");
    ctx.check(child->memory.empty(), "kill cleanup releases child memory");
    ctx.equal(kernel.windowCount(), 0, "kill cleanup releases child windows");
    ctx.equal(kernel.ipcMessageCount(), 0, "kill cleanup drops child IPC messages");
    ctx.check(kernel.sysWaitPid(kParentPid, child_pid).ok(),
              "parent wait reaps killed zombie");
    ctx.check(kernel.process(child_pid) == nullptr, "reaped child is removed");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "isolation checker accepts post-cleanup state");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"kernel.syscall.file_heap_facade", "kernel.native_os_contract",
         syscallFileAndHeapFacade},
        {"kernel.exec.image_metadata_install", "kernel.native_os_contract",
         execInstallsImageMetadata},
        {"kernel.security.capability_cleanup", "kernel.native_os_contract",
         capabilityAndKillCleanup},
    };
    return tests_next::runCases("next_kernel_native_os", cases);
}
