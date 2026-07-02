#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_os.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

constexpr int kParentPid = 1;

void forkIsolationAndWaitpid(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "process test kernel boots");
    Process* parent = kernel.process(kParentPid);
    ctx.check(parent != nullptr, "parent process exists");
    if (!parent) return;
    parent->memory.resize(16, 0);
    parent->memory[3] = 777;

    const StatusResult forked = kernel.sysFork(kParentPid);
    ctx.check(forked.ok() && forked.payload > kParentPid,
              "fork returns child pid");
    const int child_pid = forked.payload;
    parent = kernel.process(kParentPid);
    Process* child = kernel.process(child_pid);
    ctx.check(parent != nullptr && child != nullptr, "forked child exists");
    if (!parent || !child) return;
    parent->memory[3] = 888;
    ctx.equal(child->memory[3], 777LL,
              "fork copies process memory independently");
    ctx.equal(child->fork_return_payload, 0,
              "child fork return payload is zero");

    ProcessInfo info;
    ctx.check(kernel.sysGetProc(child_pid, info).ok(),
              "getproc reports forked child");
    ctx.equal(info.parent_pid, kParentPid, "getproc reports parent pid");
    ctx.equal(info.state, sandbox::vm::PROC_STATE_RUNNABLE,
              "getproc reports runnable child");

    ctx.check(kernel.sysExit(child_pid, 17).ok(), "child exits");
    child = kernel.process(child_pid);
    ctx.check(child != nullptr &&
                  child->state == sandbox::vm::PROC_STATE_ZOMBIE,
              "exited child remains zombie for parent wait");
    const StatusResult waited = kernel.sysWaitPid(kParentPid, child_pid);
    ctx.check(waited.ok() && waited.payload == 17,
              "waitpid returns child exit status");
    ctx.check(kernel.process(child_pid) == nullptr,
              "waitpid reaps zombie child");
}

void execImageHandoff(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "exec handoff kernel boots");
    ctx.check(kernel.fs().createFile("/bin", InodeKind::Directory).ok(),
              "bin directory creates");

    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 7;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 40;
    const std::vector<long long> image = {700, 701, 702};
    ctx.check(kernel.installExecutable("/bin/task", image, header).ok(),
              "executable task installs");

    const StatusResult exec = kernel.sysExec(kParentPid, "/bin/task");
    Process* proc = kernel.process(kParentPid);
    ctx.check(exec.ok() && exec.payload == 7,
              "exec reports task entry point");
    ctx.check(proc != nullptr, "exec process remains live");
    if (!proc) return;
    ctx.check(proc->memory == image, "exec replaces process image");
    ctx.equal(proc->state, sandbox::vm::PROC_STATE_RUNNABLE,
              "exec leaves process runnable");
    ctx.equal(proc->heap_start, sandbox::vm::MMU_PAGE_WORDS,
              "exec data pages define heap base");
    ctx.equal(proc->heap_break, proc->heap_start,
              "exec resets heap break to heap base");
}

void ipcCapabilityGatedDelivery(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "IPC test kernel boots");
    const StatusResult forked = kernel.sysFork(kParentPid);
    ctx.check(forked.ok(), "fork creates IPC receiver");
    const int child_pid = forked.payload;

    ctx.check(kernel.setProcessCapabilities(child_pid, CAP_FILE_READ).ok(),
              "child capabilities restricted without IPC");
    ctx.equal(kernel.sysIpcSend(child_pid, kParentPid, 33).detail,
              ERR_ACCESS, "sender without IPC capability is denied");
    long long payload = 0;
    ctx.equal(kernel.sysIpcRecv(child_pid, payload).detail,
              ERR_ACCESS, "receiver without IPC capability is denied");

    ctx.check(kernel.setProcessCapabilities(child_pid, CAP_FILE_READ | CAP_IPC).ok(),
              "child receives IPC capability");
    const StatusResult empty = kernel.sysIpcRecv(child_pid, payload);
    ctx.check(empty.status == T1_PENDING && empty.detail == ERR_AGAIN,
              "empty IPC receive reports pending");
    ctx.check(kernel.sysIpcSend(kParentPid, child_pid, 5150).ok(),
              "parent sends IPC payload");
    ctx.equal(kernel.ipcMessageCount(), 1, "IPC queue records one message");
    const StatusResult received = kernel.sysIpcRecv(child_pid, payload);
    ctx.check(received.ok() && received.payload == kParentPid && payload == 5150,
              "child receives queued IPC payload");
    ctx.equal(kernel.ipcMessageCount(), 0, "IPC receive consumes message");
}

void schedulerSignalCleanup(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "scheduler cleanup kernel boots");
    ctx.check(kernel.fs().createFile("/tmp", InodeKind::Directory).ok(),
              "tmp directory creates");
    ctx.check(kernel.fs().createFile("/tmp/data", InodeKind::File).ok(),
              "data file creates");
    ctx.check(kernel.fs().writeFile("/tmp/data", {8, 9}).ok(),
              "data file seeds");

    const StatusResult forked = kernel.sysFork(kParentPid);
    ctx.check(forked.ok(), "parent forks child");
    const int child_pid = forked.payload;
    ctx.check(kernel.sysSuspend(child_pid).ok(),
              "suspend signal succeeds");
    ctx.equal(kernel.process(child_pid)->state,
              sandbox::vm::PROC_STATE_STOPPED,
              "suspend moves child to stopped state");
    ctx.check(kernel.sysResume(child_pid).ok(), "resume signal succeeds");
    ctx.equal(kernel.process(child_pid)->state,
              sandbox::vm::PROC_STATE_RUNNABLE,
              "resume returns child to runnable state");

    ctx.check(kernel.createWindow(child_pid, 3, 3).ok(),
              "child owns a window before kill");
    ctx.check(kernel.sysOpen(child_pid, "/tmp/data", true).ok(),
              "child owns an fd before kill");
    const StatusResult grandchild = kernel.sysFork(child_pid);
    ctx.check(grandchild.ok(), "child can fork grandchild");

    ctx.check(kernel.sysKill(child_pid, sandbox::vm::SIGNAL_KILL).ok(),
              "kill signal exits child");
    const Process* child = kernel.process(child_pid);
    ctx.check(child != nullptr &&
                  child->state == sandbox::vm::PROC_STATE_ZOMBIE,
              "killed child is waitable zombie");
    ctx.equal(kernel.windowCount(), 0, "kill removes child windows");
    const Process* orphan = kernel.process(grandchild.payload);
    ctx.check(orphan != nullptr && orphan->parent_pid == -1,
              "live grandchildren are orphaned");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "isolation checker accepts orphaned runnable process");

    ctx.check(kernel.sysKill(grandchild.payload,
                             sandbox::vm::SIGNAL_KILL).ok(),
              "orphan kill succeeds");
    ctx.check(kernel.process(grandchild.payload) == nullptr,
              "orphaned killed process is reaped immediately");
    ctx.check(kernel.sysWaitPid(kParentPid, child_pid).ok(),
              "parent waits killed child");
    ctx.check(kernel.process(child_pid) == nullptr,
              "wait removes killed child");
    ctx.check(kernel.checkProcessIsolation().ok(),
              "isolation checker accepts final cleanup");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"process.fork.copy_isolation_waitpid", "process.ipc.scheduler_contract",
         forkIsolationAndWaitpid},
        {"process.exec.image_handoff", "process.ipc.scheduler_contract",
         execImageHandoff},
        {"process.ipc.capability_gated_delivery", "process.ipc.scheduler_contract",
         ipcCapabilityGatedDelivery},
        {"process.scheduler.signal_cleanup", "process.ipc.scheduler_contract",
         schedulerSignalCleanup},
    };
    return tests_next::runCases("next_process_ipc_scheduler", cases);
}
