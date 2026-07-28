#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_asm.h"
#include "ternary_vm.h"

#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox;
using namespace sandbox::isa;
using namespace sandbox::vm;
using namespace sandbox::vm::assembler;

long long regLong(const VMState& vm, uint8_t reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(reg));
}

long long loadLong(VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

void compareTraceJitEquivalent(TestContext& ctx,
                               const std::string& label,
                               const std::vector<TritWord27>& program,
                               const std::vector<int>& memory_addrs,
                               int max_steps) {
    VMState interpreter(128, 256);
    VMState jit(128, 256);
    ctx.check(loadAndReset(interpreter, program),
              label + " interpreter program loads");
    ctx.check(loadAndReset(jit, program), label + " trace-JIT program loads");

    interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
    jit.setExecutionBackend(VMExecutionBackend::TraceJit);
    jit.setTraceJitHotThreshold(1);

    const RunResult interpreter_result = run(interpreter, max_steps);
    const RunResult jit_result = run(jit, max_steps);
    ctx.check(interpreter_result.status == jit_result.status,
              label + " status matches");
    ctx.equal(interpreter_result.steps, jit_result.steps,
              label + " step count matches");
    ctx.equal(interpreter.pc, jit.pc, label + " final PC matches");
    ctx.equal(interpreter.cycle_count, jit.cycle_count,
              label + " cycle count matches");

    for (int reg = 0; reg < REG_COUNT; ++reg) {
        ctx.check(interpreter.regfile.read(static_cast<uint8_t>(reg)) ==
                      jit.regfile.read(static_cast<uint8_t>(reg)),
                  label + " register r" + std::to_string(reg) + " matches");
    }
    for (int addr : memory_addrs) {
        const auto [interpreter_value, interpreter_fault] = interpreter.dmem.load(addr);
        const auto [jit_value, jit_fault] = jit.dmem.load(addr);
        ctx.check(interpreter_fault == jit_fault,
                  label + " memory fault @" + std::to_string(addr) + " matches");
        ctx.check(interpreter_value == jit_value,
                  label + " memory @" + std::to_string(addr) + " matches");
    }

    ctx.check(jit.trace_jit_stats.traces_built > 0,
              label + " builds at least one trace");
    ctx.check(jit.trace_jit_stats.instructions_executed > 0,
              label + " executes trace instructions");
}

long long vectorLong(const VMState& vm, int vreg, int lane) {
    return sandbox::vm::ops::toLong(
        vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane));
}

TernaryMode vectorMode(const VMState& vm, int vreg, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane).mode;
}

TernaryValue predicateLane(int8_t trit) {
    TritLane1 lane;
    lane.setTrit(0, trit);
    return TernaryValue::fromL1(lane);
}

void widthSanity(TestContext& ctx) {
    VMState vm(16, 64);
    const auto program = assembleOrThrow(R"(
        mov.t5 r1, 120
        mov.t5 r2, 1
        add.t5 r3, r1, r2
        mov.t20 r4, 40
        add.t20 r5, r4, r3
        halt
    )");
    ctx.check(loadAndReset(vm, program), "width program loads");
    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "width program halts");
    ctx.check(vm.regfile.read(R3).mode == TernaryMode::T5, "T5 result tag");
    ctx.equal(regLong(vm, R3), 121LL, "T5 add value");
    ctx.check(vm.regfile.read(R5).mode == TernaryMode::T20, "T20 result tag");
    ctx.equal(regLong(vm, R5), 161LL, "T20 add converts source value");
}

void loadStoreTags(TestContext& ctx) {
    VMState vm(16, 64);
    const auto program = assembleOrThrow(R"(
        mov.t20 r1, 42
        store r1, sp, 0
        load r2, sp, 0
        add.t20 r3, r2, r2
        halt
    )");
    ctx.check(loadAndReset(vm, program), "load/store program loads");
    const RunResult result = run(vm, 64);
    ctx.check(result.halted(), "load/store program halts");
    ctx.check(vm.regfile.read(R2).mode == TernaryMode::T20, "LOAD preserves T20 tag");
    ctx.check(vm.regfile.read(R3).mode == TernaryMode::T20, "ADD writes T20 tag");
    ctx.equal(regLong(vm, R3), 84LL, "loaded value participates in arithmetic");
}

void eretDivZeroRoute(TestContext& ctx) {
    VMState vm(64, 64);
    const AssemblyResult assembled = assemble(R"(
        mov r1, handler
        csrw tvec, r1
        mov r1, -8
        csrw status, r1
        mov r1, 1
        mov r2, 0
    fault_div:
        div.t20 r3, r1, r2
    after_fault:
        halt
    handler:
        csrr r4, cause
        csrr r5, epc
        mov r6, after_fault
        csrw epc, r6
        eret
    )");
    ctx.check(assembled.success, "routed trap program assembles");
    if (!assembled.success) return;
    ctx.check(loadAndReset(vm, assembled), "routed trap program loads");
    const RunResult result = run(vm, 64);
    ctx.check(result.halted(), "routed trap handler returns to halt");
    ctx.equal(regLong(vm, R4), static_cast<long long>(OS_CAUSE_DIV_ZERO),
              "handler observes div-zero cause");
    ctx.equal(regLong(vm, R5), static_cast<long long>(assembled.labels.at("fault_div")),
              "handler observes faulting EPC");
    ctx.check(vm.privilege == PrivilegeMode::User, "ERET restores user mode");
}

void scratchCsrrwRoundtrip(TestContext& ctx) {
    VMState vm(32, 64);
    const auto program = assembleOrThrow(R"(
        mov sp, 11
        mov r1, 22
        csrw scratch, r1
        csrrw sp, scratch, sp
        csrr r2, scratch
        halt
    )");
    ctx.check(loadAndReset(vm, program), "CSRRW program loads");
    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "CSRRW program halts");
    ctx.equal(regLong(vm, R26_SP), 22LL, "CSRRW writes old CSR value to rd");
    ctx.equal(regLong(vm, R2), 11LL, "CSRRW writes source value into CSR");
}

void zeroPteFaults(TestContext& ctx) {
    {
        VMState vm(96, 96);
        const auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        ctx.check(vm.imem.loadProgram(handler, 2 * LEGACY_MMU_PAGE_WORDS),
                  "fetch zero-PTE handler loads");
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * LEGACY_MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;

        const RunResult result = run(vm, 16);
        ctx.check(result.halted(), "zero fetch PTE routes to handler");
        ctx.equal(regLong(vm, R4), static_cast<long long>(OS_CAUSE_FETCH_PAGE_FAULT),
                  "zero fetch PTE reports page fault");
        ctx.equal(regLong(vm, R5), 0LL, "zero fetch PTE records virtual PC");
        ctx.equal(regLong(vm, R6), static_cast<long long>(OS_PAGE_ACCESS_FETCH),
                  "zero fetch PTE records fetch access");
    }

    {
        VMState vm(128, 128);
        const auto user = assembleOrThrow("load r1, zero, 0\nhalt\n");
        const auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        ctx.check(vm.imem.loadProgram(user, LEGACY_MMU_PAGE_WORDS),
                  "load zero-PTE user program loads");
        ctx.check(vm.imem.loadProgram(handler, 3 * LEGACY_MMU_PAGE_WORDS),
                  "load zero-PTE handler loads");
        ctx.check(vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true)) ==
                      MemFaultCode::OK,
                  "valid user IMEM PTE stores");
        vm.trap_routing_enabled = true;
        vm.tvec = 3 * LEGACY_MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;

        const RunResult result = run(vm, 16);
        ctx.check(result.halted(), "zero load PTE routes to handler");
        ctx.equal(regLong(vm, R4), static_cast<long long>(OS_CAUSE_LOAD_PAGE_FAULT),
                  "zero load PTE reports page fault");
        ctx.equal(regLong(vm, R5), 0LL, "zero load PTE records virtual address");
        ctx.equal(regLong(vm, R6), static_cast<long long>(OS_PAGE_ACCESS_LOAD),
                  "zero load PTE records load access");
    }
}

void sparseHighAddressAccess(TestContext& ctx) {
    constexpr int kLargeWords = SPARSE_MEMORY_DENSE_LIMIT_WORDS + 10000;
    const int high_addr = kLargeWords - 17;

    TernaryMemory dmem(kLargeWords);
    ctx.check(dmem.isSparse(), "large DMEM uses sparse backing");
    ctx.check(dmem.store(high_addr, sandbox::vm::ops::fromLong(123456)) == MemFaultCode::OK,
              "sparse DMEM high store succeeds");
    auto [loaded, load_fault] = dmem.load(high_addr);
    ctx.check(load_fault == MemFaultCode::OK, "sparse DMEM high load succeeds");
    ctx.equal(sandbox::vm::ops::toLong(loaded), 123456LL, "sparse DMEM high load value");
    ctx.check(dmem.allocatedPages() <= 1, "sparse DMEM allocates only touched page");

    TernaryInstructionMemory imem(kLargeWords);
    TritWord27 marker = InstructionWord::encodeB(Opcode::HALT, R0_ZERO, 0);
    marker.bits ^= 0x155ULL;
    ctx.check(imem.isSparse(), "large IMEM uses sparse backing");
    ctx.check(imem.write(high_addr, marker) == MemFaultCode::OK,
              "sparse IMEM high write succeeds");
    auto [fetched, fetch_fault] = imem.fetch(high_addr);
    ctx.check(fetch_fault == MemFaultCode::OK, "sparse IMEM high fetch succeeds");
    ctx.equal(fetched.bits, marker.bits, "sparse IMEM high fetch value");
    ctx.check(imem.allocatedPages() <= 1, "sparse IMEM allocates only touched page");
}

void vectorDivideLaneFaults(TestContext& ctx) {
    VMState vm(16, 64);
    vm.vector_length = 4;
    const auto program = assembleOrThrow("vdiv.t20 v2, v0, v1\nhalt\n");
    ctx.check(loadAndReset(vm, program), "vector divide program loads");
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(9)));
        vm.vregfile.reg[1].write(
            lane, TernaryValue::fromT20(native_ops::fromIntT20(lane == 2 ? 0 : 3)));
    }

    const RunResult result = run(vm, 16);
    ctx.check(result.halted(), "VDIV lane fault program still halts");
    ctx.equal(vectorLong(vm, 2, 0), 3LL, "VDIV valid lane result");
    ctx.check(vectorMode(vm, 2, 2) == TernaryMode::T20, "VDIV fault lane keeps result type");
    ctx.equal(vectorLong(vm, 2, 2), 0LL, "VDIV fault lane writes typed zero");
    ctx.check(vm.vector_faults.fault_valid[2], "VDIV records faulting lane");
    ctx.check(vm.vector_faults.fault_class[2] == TrapCode::TRAP_DIV_ZERO,
              "VDIV records divide-by-zero lane class");
    ctx.check(!vm.vector_faults.fault_valid[0], "VDIV leaves valid lane fault clear");
}

void vectorWrongTagLaneFaults(TestContext& ctx) {
    VMState vm(16, 64);
    vm.vector_length = 3;
    const auto program = assembleOrThrow("vadd.t20 v2, v0, v1\nhalt\n");
    ctx.check(loadAndReset(vm, program), "vector wrong-tag program loads");
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(1)));
        vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(2)));
    }
    vm.vregfile.reg[1].write(1, TernaryValue::fromL20(toLane(native_ops::fromIntT20(2))));

    const RunResult result = run(vm, 16);
    ctx.check(result.halted(), "wrong-tag lane program still halts");
    ctx.equal(vectorLong(vm, 2, 0), 3LL, "VADD valid lane survives wrong-tag neighbor");
    ctx.equal(vectorLong(vm, 2, 1), 0LL, "VADD wrong-tag lane writes typed zero");
    ctx.check(vm.vector_faults.fault_valid[1], "VADD records wrong-tag lane");
    ctx.check(vm.vector_faults.fault_class[1] == TrapCode::TRAP_ILLEGAL_OP,
              "VADD records illegal-op lane class");
}

void vectorPackUnpackAndSwapRuntime(TestContext& ctx) {
    VMState vm(32, 64);
    vm.vector_length = 3;
    const auto program = assembleOrThrow(R"(
        vpack.t20.t10   v1, v0
        vunpack.t10.t20 v2, v1
        vswap           v1, v2
        halt
    )");
    ctx.check(loadAndReset(vm, program), "vector pack/unpack/swap program loads");
    const long long values[] = {10, 20, 30};
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(
            lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
    }

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "vector pack/unpack/swap program halts");
    ctx.check(vectorMode(vm, 1, 0) == TernaryMode::T20,
              "VSWAP leaves unpacked T20 payload in v1");
    ctx.check(vectorMode(vm, 2, 0) == TernaryMode::T10,
              "VSWAP leaves packed T10 payload in v2");
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        ctx.equal(vectorLong(vm, 1, lane), values[lane],
                  "unpacked lane value survives swap");
        ctx.equal(vectorLong(vm, 2, lane), values[lane],
                  "packed lane value survives swap");
    }
    ctx.check(!vm.vector_faults.any(), "vector conversion/swap has no lane faults");
}

void vectorPermuteBlendRuntime(TestContext& ctx) {
    VMState vm(32, 64);
    vm.vector_length = 3;
    const auto program = assembleOrThrow(R"(
        vpermute.t20 v3, v0, v4
        vblend.t20   v6, v5, v0, v3
        halt
    )");
    ctx.check(loadAndReset(vm, program), "vector permute/blend program loads");
    const long long values[] = {10, 20, 30};
    const long long indices[] = {2, 0, 1};
    const int8_t cond[] = {-1, 0, 1};
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(
            lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
        vm.vregfile.reg[4].write(
            lane, TernaryValue::fromT5(native_ops::fromIntT5(indices[lane])));
        vm.vregfile.reg[5].write(lane, predicateLane(cond[lane]));
    }

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "vector permute/blend program halts");
    ctx.equal(vectorLong(vm, 3, 0), 30LL, "VPERMUTE lane 0 selects index 2");
    ctx.equal(vectorLong(vm, 3, 1), 10LL, "VPERMUTE lane 1 selects index 0");
    ctx.equal(vectorLong(vm, 3, 2), 20LL, "VPERMUTE lane 2 selects index 1");
    ctx.equal(vectorLong(vm, 6, 0), 10LL, "VBLEND negative condition selects false arm");
    ctx.equal(vectorLong(vm, 6, 1), 20LL, "VBLEND zero condition selects false arm");
    ctx.equal(vectorLong(vm, 6, 2), 20LL, "VBLEND positive condition selects true arm");
    ctx.check(!vm.vector_faults.any(), "vector permute/blend has no lane faults");
}

void vectorGatherScatterLaneFaults(TestContext& ctx) {
    VMState vm(32, 32);
    vm.vector_length = 3;
    const auto program = assembleOrThrow(R"(
        mov r1, 10
        vgather.t20  v2, r1, v0
        vscatter.t20 v2, r1, v1
        halt
    )");
    ctx.check(loadAndReset(vm, program), "vector gather/scatter program loads");
    const long long gather_index[] = {0, 2, 4};
    const long long scatter_index[] = {6, 7, 40};
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(
            lane, TernaryValue::fromT5(native_ops::fromIntT5(gather_index[lane])));
        vm.vregfile.reg[1].write(
            lane, TernaryValue::fromT5(native_ops::fromIntT5(scatter_index[lane])));
    }
    ctx.check(vm.dmem.store(10, TernaryValue::fromT5(native_ops::fromIntT5(11))) ==
                  MemFaultCode::OK,
              "gather seed 0 stores");
    ctx.check(vm.dmem.store(12, TernaryValue::fromT5(native_ops::fromIntT5(22))) ==
                  MemFaultCode::OK,
              "gather seed 1 stores");
    ctx.check(vm.dmem.store(14, TernaryValue::fromT5(native_ops::fromIntT5(33))) ==
                  MemFaultCode::OK,
              "gather seed 2 stores");

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "vector gather/scatter program halts with lane-local fault");
    ctx.equal(loadLong(vm, 16), 11LL, "VSCATTER stores lane 0 gathered value");
    ctx.equal(loadLong(vm, 17), 22LL, "VSCATTER stores lane 1 gathered value");
    ctx.check(!vm.vector_faults.fault_valid[0], "VSCATTER lane 0 remains clean");
    ctx.check(!vm.vector_faults.fault_valid[1], "VSCATTER lane 1 remains clean");
    ctx.check(vm.vector_faults.fault_valid[2], "VSCATTER records out-of-range lane");
    ctx.check(vm.vector_faults.fault_class[2] == TrapCode::TRAP_MEM_FAULT,
              "VSCATTER out-of-range lane records memory fault");
}

void vectorReductionRuntime(TestContext& ctx) {
    VMState vm(32, 64);
    vm.vector_length = 4;
    const auto program = assembleOrThrow(R"(
        vsum.t20  r3, v0
        vhmin.t20 r4, v0
        vhmax.t20 r5, v0
        halt
    )");
    ctx.check(loadAndReset(vm, program), "vector reduction program loads");
    const long long values[] = {5, -2, 7, 0};
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(
            lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
    }

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "vector reduction program halts");
    ctx.equal(regLong(vm, R3), 10LL, "VSUM reduces all lanes");
    ctx.equal(regLong(vm, R4), -2LL, "VHMIN finds minimum lane");
    ctx.equal(regLong(vm, R5), 7LL, "VHMAX finds maximum lane");
    ctx.check(!vm.vector_faults.any(), "vector reductions have no lane faults");
}

void blockCacheInvalidation(TestContext& ctx) {
    const auto program = assembleOrThrow(R"(
        mov r1, 0
        mov r2, 1
        mov r3, 6
    loop:
        add r1, r1, r2
        sub r3, r3, r2
        brp r3, loop
        halt
    )");

    VMState vm(32, 128);
    ctx.check(loadAndReset(vm, program), "block-cache loop loads");
    const RunResult result = run(vm, 64);
    ctx.check(result.halted(), "block-cache loop halts");
    ctx.equal(regLong(vm, R1), 6LL, "block-cache loop arithmetic result");
    ctx.check(vm.block_cache_stats.hits > 0, "block cache records hits");
    ctx.check(vm.block_cache_stats.misses > 0, "block cache records misses");
    ctx.check(vm.averageBlockCacheLength() > 1.0, "block cache records multi-instruction blocks");

    const auto generation = vm.imem.generation();
    ctx.check(vm.imem.write(0, program[0]) == MemFaultCode::OK, "IMEM write succeeds");
    ctx.check(vm.imem.generation() != generation, "IMEM write bumps generation");
    syncBlockCacheGeneration(vm);
    ctx.check(vm.basic_block_cache.empty(), "IMEM write invalidates basic block cache");
    ctx.check(vm.decoded_instruction_cache.empty(), "IMEM write invalidates decoded cache");
}

void traceJitSyscallFallback(TestContext& ctx) {
    VMState vm(32, 64);
    const auto program = assembleOrThrow(R"(
        mov r1, 3
        syscall 1
        mov r2, 7
        halt
    )");
    ctx.check(loadAndReset(vm, program), "trace-JIT syscall program loads");
    vm.setExecutionBackend(VMExecutionBackend::TraceJit);
    vm.setTraceJitHotThreshold(1);

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "trace-JIT syscall program halts");
    ctx.check(vm.trace_jit_stats.unsupported_fallbacks > 0,
              "trace JIT records unsupported syscall fallback");
    ctx.equal(vm.syscall_buffer, std::string("3"), "syscall side effect is preserved");
    ctx.equal(regLong(vm, R2), 7LL, "execution resumes after fallback");
}

void traceJitArithmeticLoopEquivalence(TestContext& ctx) {
    compareTraceJitEquivalent(ctx, "trace-JIT arithmetic loop", assembleOrThrow(R"(
        mov r1, 0
        mov r2, 1
        mov r3, 12
    loop:
        add r1, r1, r2
        sub r3, r3, r2
        brp r3, loop
        halt
    )"), {}, 128);
}

void traceJitLoadStoreEquivalence(TestContext& ctx) {
    compareTraceJitEquivalent(ctx, "trace-JIT load/store", assembleOrThrow(R"(
        mov r1, 10
        mov.t20 r2, 7
        store r2, r1, 0
        load r3, r1, 0
        add.t20 r4, r3, r2
        halt
    )"), {10}, 64);
}

void traceJitDirectBranchEquivalence(TestContext& ctx) {
    compareTraceJitEquivalent(ctx, "trace-JIT direct branch", assembleOrThrow(R"(
        mov r1, 1
        jmp done
        mov r1, 2
    done:
        add r2, r1, r1
        halt
    )"), {}, 32);
}

void traceJitMemoryBailout(TestContext& ctx) {
    VMState vm(32, 16);
    const auto program = assembleOrThrow(R"(
        mov r1, 99
    loop:
        load r2, r1, 0
        jmp loop
    )");
    ctx.check(loadAndReset(vm, program), "trace-JIT memory bailout program loads");
    vm.setExecutionBackend(VMExecutionBackend::TraceJit);
    vm.setTraceJitHotThreshold(1);

    const RunResult result = run(vm, 16);
    ctx.check(result.trapped(), "trace-JIT unsafe load traps through interpreter");
    ctx.check(result.trap_code == TrapCode::TRAP_MEM_FAULT,
              "trace-JIT unsafe load preserves memory trap class");
    ctx.check(vm.trace_jit_stats.interpreter_bailouts > 0,
              "trace JIT records unsafe memory bailout");
}

void timerIrqRoutesAndResumes(TestContext& ctx) {
    VMState vm(64, 64);
    const AssemblyResult assembled = assemble(R"(
        nop
        nop
        nop
    timer_after:
        mov r8, 99
        halt
    handler:
        csrr r6, cause
        csrr r7, epc
        mov r1, 0
        csrw timer_enable, r1
        eret
    )");
    ctx.check(assembled.success, "timer IRQ program assembles");
    if (!assembled.success) return;

    ctx.check(loadAndReset(vm, assembled), "timer IRQ program loads");
    vm.trap_routing_enabled = true;
    vm.tvec = assembled.labels.at("handler");
    vm.privilege = PrivilegeMode::User;
    vm.interrupt_enable = true;
    vm.timer_counter = 3;
    vm.timer_reload = 0;
    vm.timer_enable = true;

    const RunResult result = run(vm, 64);
    ctx.check(result.halted(), "timer IRQ handler resumes program");
    ctx.equal(regLong(vm, R6), static_cast<long long>(OS_CAUSE_TIMER_IRQ),
              "timer IRQ routes interrupt cause");
    ctx.equal(regLong(vm, R7),
              static_cast<long long>(assembled.labels.at("timer_after")),
              "timer IRQ EPC is next PC after exact instruction count");
    ctx.equal(regLong(vm, R8), 99LL, "program resumes after timer ERET");
}

void timerPendingDefersAcrossCriticalSection(TestContext& ctx) {
    VMState vm(96, 64);
    const AssemblyResult assembled = assemble(R"(
        mov r1, handler
        csrw tvec, r1
        mov r1, 1
        csrw timer_counter, r1
        csrw timer_enable, r1
        nop
        csrr r2, timer_pending
        mov r1, -7
        csrw status, r1
    after_enable:
        mov r8, 44
        halt
    handler:
        csrr r4, cause
        csrr r5, epc
        mov r1, 0
        csrw timer_enable, r1
        eret
    )");
    ctx.check(assembled.success, "critical-section timer program assembles");
    if (!assembled.success) return;

    ctx.check(loadAndReset(vm, assembled), "critical-section timer program loads");
    const RunResult result = run(vm, 96);
    ctx.check(result.halted(), "critical-section timer program halts");
    ctx.equal(regLong(vm, R2), 1LL,
              "timer interrupt remains pending while interrupts are disabled");
    ctx.equal(regLong(vm, R4), static_cast<long long>(OS_CAUSE_TIMER_IRQ),
              "pending timer routes after interrupts are re-enabled");
    ctx.equal(regLong(vm, R5),
              static_cast<long long>(assembled.labels.at("after_enable")),
              "pending timer EPC is the first instruction after critical section");
    ctx.equal(regLong(vm, R8), 44LL,
              "program resumes after deferred timer interrupt");
}

void multicoreIndependentRegisterFiles(TestContext& ctx) {
    const AssemblyResult assembled = assemble(R"(
        .text
        .org 0
    core0:
        mov r13, 11
        halt
        .org 8
    core1:
        mov r13, 22
        halt
    )");
    ctx.check(assembled.success, "two-core program assembles");
    if (!assembled.success) return;

    VMState machine(64, 512);
    ctx.check(machine.imem.loadProgram(assembled.program),
              "two-core program loads");
    machine.configureCores(2);
    machine.coreState(0).pc = 0;
    machine.coreState(1).pc = 8;

    const RunResult result = runMultiCore(machine, 16);
    ctx.check(result.halted(), "two-core VM halts both cores");
    ctx.equal(sandbox::vm::ops::toLong(machine.coreState(0).regfile.read(13)),
              11LL, "core 0 keeps an independent register file");
    ctx.equal(sandbox::vm::ops::toLong(machine.coreState(1).regfile.read(13)),
              22LL, "core 1 keeps an independent register file");
}

void multicoreRunQueueBalancing(TestContext& ctx) {
    VMState machine(64, 512);
    machine.configureCores(2);
    for (int pid = 1; pid <= 101; ++pid) {
        machine.enqueueProcess(pid);
    }

    machine.loadBalanceRunQueues();
    const std::size_t c0 = machine.runQueueDepth(0);
    const std::size_t c1 = machine.runQueueDepth(1);
    const std::size_t diff = c0 > c1 ? c0 - c1 : c1 - c0;
    ctx.equal(c0 + c1, static_cast<std::size_t>(101),
              "load balancing preserves all runnable processes");
    ctx.check(diff <= 1, "load balancing keeps two core queues within one process");
}

void multicoreMmuCsrStateIsolation(TestContext& ctx) {
    VMState machine(64, 512);
    machine.configureCores(2);

    machine.restoreCoreState(0);
    machine.mmu_enable = true;
    machine.user_imem_ptbr = 111;
    machine.user_imem_pages = 7;
    machine.user_dmem_ptbr = 222;
    machine.user_dmem_pages = 9;
    machine.captureCoreState(0);

    machine.restoreCoreState(1);
    machine.mmu_enable = false;
    machine.user_imem_ptbr = 333;
    machine.user_imem_pages = 11;
    machine.user_dmem_ptbr = 444;
    machine.user_dmem_pages = 13;
    machine.captureCoreState(1);

    machine.restoreCoreState(0);
    ctx.check(machine.mmu_enable, "core 0 restores MMU enabled state");
    ctx.equal(machine.user_imem_ptbr, 111, "core 0 restores IMEM PTBR");
    ctx.equal(machine.user_imem_pages, 7, "core 0 restores IMEM page count");
    ctx.equal(machine.user_dmem_ptbr, 222, "core 0 restores DMEM PTBR");
    ctx.equal(machine.user_dmem_pages, 9, "core 0 restores DMEM page count");

    machine.restoreCoreState(1);
    ctx.check(!machine.mmu_enable, "core 1 restores MMU disabled state");
    ctx.equal(machine.user_imem_ptbr, 333, "core 1 restores IMEM PTBR");
    ctx.equal(machine.user_imem_pages, 11, "core 1 restores IMEM page count");
    ctx.equal(machine.user_dmem_ptbr, 444, "core 1 restores DMEM PTBR");
    ctx.equal(machine.user_dmem_pages, 13, "core 1 restores DMEM page count");
}

void multicoreVectorStateIsolation(TestContext& ctx) {
    VMState machine(64, 512);
    machine.configureCores(2);

    machine.restoreCoreState(0);
    machine.vector_length = 3;
    machine.vregfile.reset(machine.vector_length);
    machine.vector_faults.reset(machine.vector_length);
    machine.vregfile.reg[0].write(0, sandbox::vm::ops::fromLong(101));
    machine.vector_faults.setLane(1, TrapCode::TRAP_DIV_ZERO);
    machine.captureCoreState(0);

    machine.restoreCoreState(1);
    machine.vector_length = 4;
    machine.vregfile.reset(machine.vector_length);
    machine.vector_faults.reset(machine.vector_length);
    machine.vregfile.reg[0].write(0, sandbox::vm::ops::fromLong(202));
    machine.captureCoreState(1);

    machine.restoreCoreState(0);
    ctx.equal(machine.vector_length, 3, "core 0 restores vector length");
    ctx.equal(vectorLong(machine, 0, 0), 101LL, "core 0 restores vector lane payload");
    ctx.check(machine.vector_faults.any(), "core 0 restores vector fault mask");
    ctx.check(machine.vector_faults.fault_valid[1], "core 0 restores faulting lane");
    ctx.check(machine.vector_faults.fault_class[1] == TrapCode::TRAP_DIV_ZERO,
              "core 0 restores fault class");

    machine.restoreCoreState(1);
    ctx.equal(machine.vector_length, 4, "core 1 restores vector length");
    ctx.equal(vectorLong(machine, 0, 0), 202LL, "core 1 restores vector lane payload");
    ctx.check(!machine.vector_faults.any(), "core 1 restores clean vector fault mask");
}

void multicoreDeviceStateIsolation(TestContext& ctx) {
    VMState machine(64, 512);
    machine.configureCores(2);

    machine.restoreCoreState(0);
    machine.accumulator = sandbox::vm::ops::fromLong(1001);
    machine.syscall_id = 33;
    machine.console_char_mode = true;
    machine.gpu_page = 1;
    machine.block_index = 44;
    machine.block_addr = 55;
    machine.captureCoreState(0);

    machine.restoreCoreState(1);
    machine.accumulator = sandbox::vm::ops::fromLong(2002);
    machine.syscall_id = 66;
    machine.console_char_mode = false;
    machine.gpu_page = 0;
    machine.block_index = 77;
    machine.block_addr = 88;
    machine.captureCoreState(1);

    machine.restoreCoreState(0);
    ctx.equal(sandbox::vm::ops::toLong(machine.accumulator), 1001LL,
              "core 0 restores accumulator");
    ctx.equal(machine.syscall_id, 33, "core 0 restores syscall id");
    ctx.check(machine.console_char_mode, "core 0 restores console char mode");
    ctx.equal(machine.gpu_page, 1, "core 0 restores GPU page staging");
    ctx.equal(machine.block_index, 44, "core 0 restores block index staging");
    ctx.equal(machine.block_addr, 55, "core 0 restores block address staging");

    machine.restoreCoreState(1);
    ctx.equal(sandbox::vm::ops::toLong(machine.accumulator), 2002LL,
              "core 1 restores accumulator");
    ctx.equal(machine.syscall_id, 66, "core 1 restores syscall id");
    ctx.check(!machine.console_char_mode, "core 1 restores console char mode");
    ctx.equal(machine.gpu_page, 0, "core 1 restores GPU page staging");
    ctx.equal(machine.block_index, 77, "core 1 restores block index staging");
    ctx.equal(machine.block_addr, 88, "core 1 restores block address staging");
}

void mmuReadOnlyStoreProtection(TestContext& ctx) {
    VMState vm(96, 128);
    const auto user = assembleOrThrow(R"(
        load r1, zero, 0
        store r1, zero, 0
        halt
    )");
    const auto handler = assembleOrThrow(R"(
        csrr r4, cause
        csrr r5, page_fault_addr
        csrr r6, page_fault_access
        halt
    )");
    ctx.check(vm.imem.loadProgram(user, LEGACY_MMU_PAGE_WORDS), "read-only user program loads");
    ctx.check(vm.imem.loadProgram(handler, 2 * LEGACY_MMU_PAGE_WORDS), "read-only handler loads");
    ctx.check(vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true)) ==
                  MemFaultCode::OK,
              "user IMEM PTE stores");
    ctx.check(vm.dmem.store(4, encodePageTableEntry(2, true, true, false, false)) ==
                  MemFaultCode::OK,
              "read-only DMEM PTE stores");
    ctx.check(vm.dmem.store(2 * LEGACY_MMU_PAGE_WORDS, sandbox::vm::ops::fromLong(33)) ==
                  MemFaultCode::OK,
              "physical read-only page seed stores");
    vm.trap_routing_enabled = true;
    vm.tvec = 2 * LEGACY_MMU_PAGE_WORDS;
    vm.privilege = PrivilegeMode::User;
    vm.mmu_enable = true;
    vm.user_imem_ptbr = 0;
    vm.user_imem_pages = 1;
    vm.user_dmem_ptbr = 4;
    vm.user_dmem_pages = 1;

    const RunResult result = run(vm, 32);
    ctx.check(result.halted(), "read-only page trap routes to handler");
    ctx.equal(regLong(vm, R1), 33LL, "read-only page permits user load before store");
    ctx.equal(regLong(vm, R4), static_cast<long long>(OS_CAUSE_PROTECTION_FAULT),
              "read-only store records protection fault");
    ctx.equal(regLong(vm, R5), 0LL, "read-only store records virtual address");
    ctx.equal(regLong(vm, R6), static_cast<long long>(OS_PAGE_ACCESS_STORE),
              "read-only store records store access");
    ctx.equal(loadLong(vm, 2 * LEGACY_MMU_PAGE_WORDS), 33LL,
              "read-only store does not mutate physical page");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"vm.arithmetic.width_sanity", "vm.execution_contract", widthSanity},
        {"vm.memory.load_store_tags", "vm.execution_contract", loadStoreTags},
        {"vm.trap.eret_div_zero_route", "vm.execution_contract", eretDivZeroRoute},
        {"vm.csr.scratch_csrrw_roundtrip", "vm.execution_contract", scratchCsrrwRoundtrip},
        {"vm.mmu.zero_pte_faults", "vm.execution_contract", zeroPteFaults},
        {"vm.memory.sparse_high_address_access", "vm.execution_contract", sparseHighAddressAccess},
        {"vm.vector.divide_lane_faults", "vm.execution_contract", vectorDivideLaneFaults},
        {"vm.vector.wrong_tag_lane_faults", "vm.execution_contract", vectorWrongTagLaneFaults},
        {"vm.vector.pack_unpack_swap_runtime", "vm.execution_contract", vectorPackUnpackAndSwapRuntime},
        {"vm.vector.permute_blend_runtime", "vm.execution_contract", vectorPermuteBlendRuntime},
        {"vm.vector.gather_scatter_lane_faults", "vm.execution_contract", vectorGatherScatterLaneFaults},
        {"vm.vector.reduction_runtime", "vm.execution_contract", vectorReductionRuntime},
        {"vm.cache.block_cache_invalidation", "vm.execution_contract", blockCacheInvalidation},
        {"vm.jit.syscall_fallback", "vm.execution_contract", traceJitSyscallFallback},
        {"vm.jit.arithmetic_loop_equivalence", "vm.execution_contract", traceJitArithmeticLoopEquivalence},
        {"vm.jit.load_store_equivalence", "vm.execution_contract", traceJitLoadStoreEquivalence},
        {"vm.jit.direct_branch_equivalence", "vm.execution_contract", traceJitDirectBranchEquivalence},
        {"vm.jit.memory_bailout_trap", "vm.execution_contract", traceJitMemoryBailout},
        {"vm.timer.irq_routes_and_resumes", "vm.execution_contract", timerIrqRoutesAndResumes},
        {"vm.timer.pending_defers_across_critical_section", "vm.execution_contract", timerPendingDefersAcrossCriticalSection},
        {"vm.multicore.independent_register_files", "vm.execution_contract", multicoreIndependentRegisterFiles},
        {"vm.multicore.run_queue_balancing", "vm.execution_contract", multicoreRunQueueBalancing},
        {"vm.multicore.mmu_csr_state_isolation", "vm.execution_contract", multicoreMmuCsrStateIsolation},
        {"vm.multicore.vector_state_isolation", "vm.execution_contract", multicoreVectorStateIsolation},
        {"vm.multicore.device_state_isolation", "vm.execution_contract", multicoreDeviceStateIsolation},
        {"vm.mmu.read_only_store_protection", "vm.execution_contract", mmuReadOnlyStoreProtection},
    };
    return tests_next::runCases("next_vm_core", cases);
}
