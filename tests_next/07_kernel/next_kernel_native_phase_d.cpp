#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using sandbox::compiler::CompileResult;
using sandbox::compiler::LinkResult;

std::string formatDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

long long wordAt(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

std::string nativePhaseDDriver() {
    return R"(
        fn expect_eq(actual: t40, expected: t40, ok: t40) -> t40 {
            kstore(35103, kload(35103) + 1);
            if actual - expected == 0 {
                return ok;
            }
            if kload(35100) == 0 {
                kstore(35100, 1);
                kstore(35101, actual);
                kstore(35102, expected);
                kstore(35104, kload(35103));
            }
            return 0;
        }

        fn expect_pos(actual: t40, ok: t40) -> t40 {
            kstore(35103, kload(35103) + 1);
            if actual > 0 {
                return ok;
            }
            if kload(35100) == 0 {
                kstore(35100, 1);
                kstore(35101, actual);
                kstore(35102, 1);
                kstore(35104, kload(35103));
            }
            return 0;
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            ok = expect_eq(kernel_init(), 1, ok);

            // kernel_init publishes the kernel process slot immediately.
            ok = expect_eq(tier1_dequeue(), 0, ok);
            macro_stage_thread(9);
            macro_stage_thread(10);
            var epoch: t40 = scheduler_epoch();
            ok = expect_eq(macro_publish(), 2, ok);
            ok = expect_eq(scheduler_epoch(), epoch + 1, ok);
            ok = expect_eq(tier1_dequeue(), 9, ok);
            ok = expect_eq(tier1_dequeue(), 10, ok);

            ok = expect_eq(process_state(0), PROC_RUNNABLE, ok);
            quota_set_limit(0, 3);
            ok = expect_eq(quota_remaining(0), 3, ok);
            process_create(1, 2, 0, 1, 0, 120);
            process_wait(1, 44);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            ok = expect_eq(scheduler_find_runnable_pid(), 1, ok);
            process_wait(0, 45);
            ok = expect_eq(scheduler_find_runnable_pid(), ERR_NOT_FOUND, ok);
            ok = expect_eq(process_wake_channel(45), 1, ok);
            ok = expect_eq(scheduler_find_runnable_pid(), 1, ok);
            ok = expect_eq(process_wake_channel(44), 1, ok);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(scheduler_find_runnable_pid(), 1, ok);
            ok = expect_eq(macro_reconcile_processes(), 2, ok);
            ok = expect_eq(macro_publish(), 2, ok);
            var staged_a: t40 = tier1_dequeue();
            var staged_b: t40 = tier1_dequeue();
            ok = expect_eq(staged_a + staged_b, 1, ok);
            ok = expect_eq(staged_a * staged_b, 0, ok);
            ok = expect_eq(quota_remaining(0), 1, ok);

            var futex_addr: t40 = USER_MEM_BASE + 10;
            kstore(futex_addr, 7);
            kstore(120 + TASK_CONTEXT_EPC, 200);
            kernel_syscall_dispatch(2, 52, futex_addr, 7, 5, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            ok = expect_eq(wait_wake_futex(2, futex_addr, 1), 1, ok);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 201, ok);
            ok = expect_eq(kload(120 + 19), 1, ok);

            process_create(6, 7, 0, 1, 0, 180);
            process_create(7, 8, 0, 1, 0, 220);
            var futex_va: t40 = 10;
            var futex_ppn_a: t40 = exec_process_dmem_ppn(6);
            var futex_ppn_b: t40 = exec_process_dmem_ppn(7);
            kstore(exec_hw_dmem_ptbr(6), exec_encode_pte(futex_ppn_a, 1, 1, 1, 0));
            kstore(exec_hw_dmem_ptbr(7), exec_encode_pte(futex_ppn_b, 1, 1, 1, 0));
            kstore(180 + TASK_CONTEXT_DMEM_PTBR, exec_hw_dmem_ptbr(6));
            kstore(180 + TASK_CONTEXT_DMEM_PAGES, 1);
            kstore(220 + TASK_CONTEXT_DMEM_PTBR, exec_hw_dmem_ptbr(7));
            kstore(220 + TASK_CONTEXT_DMEM_PAGES, 1);
            kstore(futex_ppn_a * MMU_PAGE_WORDS + futex_va, 42);
            kstore(futex_ppn_b * MMU_PAGE_WORDS + futex_va, 42);
            ok = expect_eq(user_validate_span(7, futex_va, 1, 0), 1, ok);
            ok = expect_pos(kload(METRIC_TLB_MISSES_ADDR), ok);
            kstore(180 + TASK_CONTEXT_EPC, 600);
            kernel_syscall_dispatch(7, 52, futex_va, 42, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(6), PROC_BLOCKED, ok);
            ok = expect_pos(kload(METRIC_TLB_HITS_ADDR), ok);
            ok = expect_pos(kload(METRIC_SPAN_CACHE_HITS_ADDR), ok);
            var saved_futex_pte: t40 = kload(exec_hw_dmem_ptbr(6));
            var slow_faults_before: t40 = kload(METRIC_SLOW_PATH_FAULTS_ADDR);
            kstore(exec_hw_dmem_ptbr(6), 0);
            ok = expect_eq(user_validate_span(7, futex_va, 1, 0), 0, ok);
            ok = expect_pos(kload(METRIC_SLOW_PATH_FAULTS_ADDR) - slow_faults_before, ok);
            kstore(exec_hw_dmem_ptbr(6), saved_futex_pte);
            translation_cache_invalidate(6);
            kernel_syscall_dispatch(8, 53, futex_va, 1, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 0, ok);
            ok = expect_eq(process_state(6), PROC_BLOCKED, ok);
            kernel_syscall_dispatch(7, 53, futex_va, 1, 0, 0);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), 1, ok);
            ok = expect_eq(process_state(6), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(180 + TASK_CONTEXT_EPC), 601, ok);
            process_reap_slot(6);
            process_reap_slot(7);

            kstore(120 + TASK_CONTEXT_EPC, 300);
            kernel_syscall_dispatch(2, 56, 2, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_SLEEPING, ok);
            kstore(KERNEL_TICK_ADDR, kload(KERNEL_TICK_ADDR) + 1);
            wait_expire_timeouts();
            ok = expect_eq(process_state(1), PROC_SLEEPING, ok);
            kstore(KERNEL_TICK_ADDR, kload(KERNEL_TICK_ADDR) + 1);
            wait_expire_timeouts();
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 301, ok);

            ipc_open(2, 0, 0);
            kstore(120 + TASK_CONTEXT_EPC, 400);
            kernel_syscall_dispatch(2, 54, 2, USER_MEM_BASE + 820, 10, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            kernel_syscall_dispatch(1, 23, 2, 999, 0, 0);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(USER_MEM_BASE + 820), 999, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 401, ok);

            kernel_syscall_dispatch(2, 27, 0, 0, 2, 2);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            var wait_window: t40 = kload(SYS_PAYLOAD_ADDR);
            kstore(120 + TASK_CONTEXT_EPC, 500);
            kernel_syscall_dispatch(2, 55, wait_window, USER_MEM_BASE + 840, 10, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), SYS_BLOCKED, ok);
            ok = expect_eq(process_state(1), PROC_BLOCKED, ok);
            ok = expect_eq(window_route_input(EVENT_KIND_MOUSE, 1, 1, 0), wait_window, ok);
            ok = expect_eq(process_state(1), PROC_RUNNABLE, ok);
            ok = expect_eq(kload(USER_MEM_BASE + 840 + EVENT_KIND), EVENT_KIND_MOUSE, ok);
            ok = expect_eq(kload(USER_MEM_BASE + 840 + EVENT_X_OR_KEY), 1, ok);
            ok = expect_eq(kload(USER_MEM_BASE + 840 + EVENT_Y), 1, ok);
            ok = expect_eq(kload(120 + TASK_CONTEXT_EPC), 501, ok);
            kernel_syscall_dispatch(2, 32, wait_window, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);

            return ok;
        }
    )";
}

std::string nativeProcessControlDriver() {
    return R"(
        fn expect_eq(actual: t40, expected: t40, ok: t40) -> t40 {
            kstore(35203, kload(35203) + 1);
            if actual - expected == 0 {
                return ok;
            }
            if kload(35200) == 0 {
                kstore(35200, 1);
                kstore(35201, actual);
                kstore(35202, expected);
                kstore(35204, kload(35203));
            }
            return 0;
        }

        fn expect_pos(actual: t40, ok: t40) -> t40 {
            kstore(35203, kload(35203) + 1);
            if actual > 0 {
                return ok;
            }
            if kload(35200) == 0 {
                kstore(35200, 1);
                kstore(35201, actual);
                kstore(35202, 1);
                kstore(35204, kload(35203));
            }
            return 0;
        }

        fn seed_alpha(addr: t40) -> t40 {
            kstore(addr + 0, 47);
            kstore(addr + 1, 97);
            kstore(addr + 2, 108);
            kstore(addr + 3, 112);
            kstore(addr + 4, 104);
            kstore(addr + 5, 97);
            kstore(addr + 6, 0);
            return addr;
        }

        fn main() -> t40 {
            var ok: t40 = 1;
            ok = expect_eq(kernel_init(), 1, ok);

            var path: t40 = USER_MEM_BASE;
            var ipc_out: t40 = USER_MEM_BASE + 800;
            seed_alpha(path);
            ok = expect_pos(vfs_create(0, path, KIND_FILE), ok);

            process_create(4, 5, 0, 1, 0, 160);
            process_set_parent(4, 1);
            process_create(5, 6, 0, 1, 0, 170);
            process_set_parent(5, 5);

            kernel_syscall_dispatch(6, 48, 5, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);
            kernel_syscall_dispatch(6, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);
            kernel_syscall_dispatch(6, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), -1, ok);
            ok = expect_eq(kload(SYS_DETAIL_ADDR), ERR_INVALID, ok);

            process_set_caps(5, APP_CAP_PROCESS);
            kernel_syscall_dispatch(6, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_STOPPED, ok);
            kernel_syscall_dispatch(6, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);

            process_set_caps(5, 0);
            kernel_syscall_dispatch(5, 49, 6, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(5), PROC_STOPPED, ok);
            kernel_syscall_dispatch(5, 50, 6, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(5), PROC_RUNNABLE, ok);

            var kill_fd: t40 = vfs_open(5, path, 0);
            ok = expect_eq(fd_is_open_for_pid(5, kill_fd), 1, ok);
            ok = expect_eq(ipc_open_owned(1, 5, 0, 1), 1, ok);
            var ppn: t40 = alloc_bootstrap_page();
            ok = expect_pos(ppn, ok);
            ok = expect_eq(pte_map(4, 0, ppn, 0), 1, ok);
            process_wait(4, 777);
            kernel_syscall_dispatch(1, 49, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_STOPPED, ok);
            ok = expect_eq(kload(WAIT_CHANNEL_BASE + 4), 0, ok);
            kernel_syscall_dispatch(1, 50, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_RUNNABLE, ok);

            kernel_syscall_dispatch(5, 27, 0, 0, 2, 2);
            var killed_window: t40 = kload(SYS_PAYLOAD_ADDR);
            var killed_window_slot: t40 = window_find(killed_window);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(window_addr(killed_window_slot) + WIN_OWNER_PID), 5, ok);

            process_wait(4, 888);
            kernel_syscall_dispatch(1, 48, 5, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_state(4), PROC_ZOMBIE, ok);
            ok = expect_eq(kload(fd_addr(kill_fd) + FD_REFCOUNT), 0, ok);
            ok = expect_eq(kload(window_addr(killed_window_slot) + WIN_ACTIVE), 0, ok);
            ok = expect_eq(kload(ipc_channel_addr(1) + IPC_VERSION), 0, ok);
            ok = expect_eq(kload(IPC_OWNER_BASE + 1), -1, ok);
            ok = expect_eq(kload(WAIT_CHANNEL_BASE + 4), 0, ok);
            ok = expect_eq(pte_entry_faults(4, 0), 1, ok);
            ok = expect_eq(kload(PROC_PARENT_PID_BASE + 5), -1, ok);

            kernel_syscall_dispatch(1, 51, 5, ipc_out, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(ipc_out + PROCINFO_STATE), PROC_ZOMBIE, ok);
            ok = expect_eq(kload(ipc_out + PROCINFO_PARENT_PID), 1, ok);
            ok = expect_eq(signal_has(kload(ipc_out + PROCINFO_PENDING_SIGNALS), SIGNAL_KILL), 1, ok);

            kernel_syscall_dispatch(1, 48, 6, SIGNAL_KILL, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(process_find_pid(6), ERR_NOT_FOUND, ok);
            kernel_syscall_dispatch(1, 43, 5, 0, 0, 0);
            ok = expect_eq(kload(SYS_STATUS_ADDR), 1, ok);
            ok = expect_eq(kload(SYS_PAYLOAD_ADDR), SIGNAL_KILL, ok);
            ok = expect_eq(process_find_pid(5), ERR_NOT_FOUND, ok);

            return ok;
        }
    )";
}

struct KernelBuild {
    std::string kernel;
    std::string trap_stub;
    std::string boot;
    CompileResult compiled;
};

const KernelBuild& nativeBuild() {
    static const KernelBuild build = [] {
        KernelBuild out;
        out.kernel = tests_next::readText("kernel.trit");
        out.trap_stub = tests_next::readText("native_kernel_trap_stub.tasm");
        out.boot = tests_next::readText("native_kernel_boot.tasm");
        out.compiled = sandbox::compiler::compileSource(
            "next_kernel_native_phase_d.trit",
            out.kernel + "\n" + nativePhaseDDriver());
        return out;
    }();
    return build;
}

const KernelBuild& nativeProcessControlBuild() {
    static const KernelBuild build = [] {
        KernelBuild out;
        out.kernel = tests_next::readText("kernel.trit");
        out.trap_stub = tests_next::readText("native_kernel_trap_stub.tasm");
        out.boot = tests_next::readText("native_kernel_boot.tasm");
        out.compiled = sandbox::compiler::compileSource(
            "next_kernel_native_process_control.trit",
            out.kernel + "\n" + nativeProcessControlDriver());
        return out;
    }();
    return build;
}

bool requireCompiled(TestContext& ctx, const KernelBuild& build) {
    ctx.check(!build.kernel.empty(), "kernel.trit is present");
    if (build.compiled.success) return true;
    ctx.fail("kernel.trit native Phase D driver diagnostics:\n" +
             formatDiagnostics(build.compiled.diagnostics));
    return false;
}

void processControlCleanup(TestContext& ctx) {
    const KernelBuild& build = nativeProcessControlBuild();
    if (!requireCompiled(ctx, build)) return;

    LinkResult linked = sandbox::compiler::linkModules({build.compiled.object});
    ctx.check(linked.success, "native process-control cleanup driver links");
    if (!linked.success) {
        ctx.fail("link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return;
    }

    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              "native process-control cleanup image loads");
    const auto result = sandbox::vm::run(vm, 50000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "native process-control cleanup driver did not halt; status="
            << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }

    ctx.equal(regLong(vm, 13), 1LL,
              "native process-control cleanup driver returns ok");
    if (regLong(vm, 13) != 1) {
        std::ostringstream out;
        out << "first native process-control assertion flag=" << wordAt(vm, 35200)
            << " actual=" << wordAt(vm, 35201)
            << " expected=" << wordAt(vm, 35202)
            << " assertion_index=" << wordAt(vm, 35204);
        ctx.fail(out.str());
    }
}

void schedulerFutexIpcWaits(TestContext& ctx) {
    const KernelBuild& build = nativeBuild();
    if (!requireCompiled(ctx, build)) return;

    LinkResult linked = sandbox::compiler::linkModules({build.compiled.object});
    ctx.check(linked.success, "native Phase D focused driver links");
    if (!linked.success) {
        ctx.fail("link diagnostics:\n" + formatDiagnostics(linked.diagnostics));
        return;
    }

    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              "native Phase D focused image loads");
    const auto result = sandbox::vm::run(vm, 50000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "native Phase D focused driver did not halt; status="
            << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }

    ctx.equal(regLong(vm, 13), 1LL, "native Phase D focused driver returns ok");
    if (regLong(vm, 13) != 1) {
        std::ostringstream out;
        out << "first native assertion flag=" << wordAt(vm, 35100)
            << " actual=" << wordAt(vm, 35101)
            << " expected=" << wordAt(vm, 35102)
            << " assertion_index=" << wordAt(vm, 35104);
        ctx.fail(out.str());
    }
}

void acquireReleaseAndTrapStub(TestContext& ctx) {
    const KernelBuild& build = nativeBuild();
    if (!requireCompiled(ctx, build)) return;

    ctx.contains(build.compiled.assembly, "tldr.+1",
                 "scheduler/VFS native paths use acquire loads");
    ctx.contains(build.compiled.assembly, "tstr.+1",
                 "scheduler/VFS native paths use release CAS stores");
    ctx.check(contains(build.trap_stub, "call kernel_dispatch"),
              "D1 trap stub enters compiled kernel_dispatch");
    ctx.check(contains(build.boot, "call kernel_init"),
              "native boot prelude initializes compiled kernel");
    ctx.check(contains(build.boot, "syscall 16"),
              "native boot prelude exercises syscall dispatch");

    auto assembled_stub = sandbox::vm::assembler::assemble(
        build.trap_stub + "\n" + build.compiled.assembly);
    if (!assembled_stub.success) {
        std::ostringstream out;
        for (const auto& error : assembled_stub.errors) {
            out << "line " << error.line << ": " << error.message << "\n";
        }
        ctx.fail("D1 trap stub assembly errors:\n" + out.str());
    }
    ctx.check(assembled_stub.success,
              "D1 trap stub assembles with compiled kernel.trit output");
}

void bootTrapDispatchResume(TestContext& ctx) {
    const KernelBuild& build = nativeBuild();
    if (!requireCompiled(ctx, build)) return;

    auto boot_image = sandbox::vm::assembler::assemble(
        build.boot + "\n" + build.trap_stub + "\n" + build.compiled.assembly);
    if (!boot_image.success) {
        std::ostringstream out;
        for (const auto& error : boot_image.errors) {
            out << "line " << error.line << ": " << error.message << "\n";
        }
        ctx.fail("native Phase D boot assembly errors:\n" + out.str());
        return;
    }

    sandbox::vm::VMState vm(sandbox::vm::ProductionProfile::minimum());
    vm.resetBlockDevice(192);
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, boot_image),
              "native Phase D boot image loads");
    const auto result = sandbox::vm::run(vm, 50000000);
    if (!result.halted()) {
        std::ostringstream out;
        out << "native Phase D boot image did not halt; status="
            << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg);
        ctx.fail(out.str());
        return;
    }

    ctx.equal(regLong(vm, 13), 1LL,
              "booted native kernel returns syscall success status");
    ctx.equal(regLong(vm, 14), 0LL,
              "booted native kernel stats root with zero size");
    ctx.equal(regLong(vm, 15), 0LL,
              "booted native kernel returns zero syscall detail");
    auto after_it = boot_image.labels.find("after_user_stat");
    ctx.check(after_it != boot_image.labels.end(),
              "native boot image exposes after_user_stat label");
    if (after_it != boot_image.labels.end()) {
        ctx.equal(static_cast<int>(vm.pc), after_it->second,
                  "ERET resumes after the user syscall");
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"kernel.native.acquire_release_trap_stub", "kernel.native_os_contract",
         acquireReleaseAndTrapStub},
        {"kernel.native.boot_trap_dispatch_resume", "kernel.native_os_contract",
         bootTrapDispatchResume},
        {"process.native.scheduler_futex_ipc_waits",
         "process.ipc.scheduler_contract", schedulerFutexIpcWaits},
        {"process.native.process_control_cleanup",
         "process.ipc.scheduler_contract", processControlCleanup},
    };
    return tests_next::runCases("next_kernel_native_phase_d", cases);
}
