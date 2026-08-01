#include "test_multiwidth_vm_common.h"

struct CountingAllocator final : sandbox::vm::VMStateAllocator {
    int dataAllocations = 0;
    int dataFrees = 0;
    int instructionAllocations = 0;
    int instructionFrees = 0;

    sandbox::vm::TernaryValue* allocateDataWords(int capacity) override {
        ++dataAllocations;
        return capacity > 0 ? new sandbox::vm::TernaryValue[capacity] : nullptr;
    }

    void deallocateDataWords(sandbox::vm::TernaryValue* words) override {
        ++dataFrees;
        delete[] words;
    }

    sandbox::isa::TritWord27* allocateInstructionWords(int capacity) override {
        ++instructionAllocations;
        return capacity > 0 ? new sandbox::isa::TritWord27[capacity] : nullptr;
    }

    void deallocateInstructionWords(sandbox::isa::TritWord27* words) override {
        ++instructionFrees;
        delete[] words;
    }
};

void testVmWidths() {
    std::cout << "[7] VM width execution and tagged load/store\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;
    const auto assembleOrThrow = [](const std::string& source) {
        return assembleV2TestOrThrow(source);
    };

    {
        CountingAllocator allocator;
        {
            VMState vm(4, 8, allocator);
            expect(vm.imem.words != nullptr, "IMEM uses flat pointer storage");
            expect(vm.dmem.words != nullptr, "DMEM uses flat pointer storage");
            expect(vm.imem.capacity == 4 && vm.dmem.capacity == 8, "flat memory capacities");
            expect(vm.dmem.store(0, TernaryValue::fromT20(native_ops::fromIntT20(7))) == MemFaultCode::OK,
                   "flat DMEM store");
            auto [loaded, fault] = vm.dmem.load(0);
            expect(fault == MemFaultCode::OK && loaded.mode == TernaryMode::T20 &&
                   native_ops::toLongLong(loaded.asT20()) == 7,
                   "flat DMEM load preserves tag and payload");
        }
        expect(allocator.dataAllocations == 1 && allocator.instructionAllocations == 1,
               "custom VM allocator allocation count");
        expect(allocator.dataFrees == 1 && allocator.instructionFrees == 1,
               "custom VM allocator free count");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 120
            mov.t5 r2, 1
            add.t5 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T5 program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T5 add program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::T5, "T5 result tag");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 121, "T5 result value");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 121
            mov.t5 r2, 1
            add.t5 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T5 overflow program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "T5 overflow traps");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "T5 overflow trap code");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 42
            store r1, sp, 0
            load r2, sp, 0
            add.t20 r3, r2, r2
            halt
        )");
        expect(loadAndReset(vm, program), "T20 load/store program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "T20 load/store program halts");
        expect(vm.regfile.readPhysical(R2).mode == TernaryMode::T40,
               "LOAD produces a canonical physical T40 register word");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20, "ADD writes T20 tag");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 84, "T20 arithmetic value");
    }

    {
        auto program = assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 6
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )");

        VMState cached(32, 128);
        expect(loadAndReset(cached, program), "block-cache loop program loads");
        auto cachedResult = sandbox::vm::run(cached, 64);
        expect(cachedResult.halted(), "block-cache loop halts");
        expect(sandbox::vm::ops::toLong(cached.regfile.read(R1)) == 6,
               "cached loop preserves arithmetic result");
        expect(cached.block_cache_stats.hits > 0, "block cache records hits");
        expect(cached.block_cache_stats.misses > 0, "block cache records misses");
        expect(cached.block_cache_stats.instructions_executed > 0,
               "block cache executes straight-line instructions");
        expect(cached.averageBlockCacheLength() > 1.0,
               "block cache reports average block length");

        VMState uncached(32, 128);
        expect(loadAndReset(uncached, program), "uncached loop program loads");
        uncached.setBlockCacheEnabled(false);
        auto uncachedResult = sandbox::vm::run(uncached, 64);
        expect(uncachedResult.halted(), "uncached loop halts");
        expect(sandbox::vm::ops::toLong(uncached.regfile.read(R1)) == 6,
               "uncached loop preserves arithmetic result");
        expect(uncached.block_cache_stats.hits == 0 &&
                   uncached.block_cache_stats.misses == 0 &&
                   uncached.block_cache_stats.instructions_executed == 0,
               "block cache disable flag bypasses cache counters");
        expect(uncached.decode_instructions_count == uncachedResult.steps,
               "uncached run decodes once per step");
        expect(cached.decode_instructions_count < uncached.decode_instructions_count,
               "cached run reduces VM decode count");

        const auto oldGeneration = cached.imem.generation();
        expect(cached.imem.write(0, program[0]) == MemFaultCode::OK,
               "IMEM write succeeds after cached run");
        expect(cached.imem.generation() != oldGeneration,
               "IMEM write bumps cache generation");
        syncBlockCacheGeneration(cached);
        expect(cached.basic_block_cache.empty() && cached.decoded_instruction_cache.empty(),
               "IMEM write lazily invalidates decoded/block caches");

        const auto loadGeneration = cached.imem.generation();
        expect(cached.imem.loadProgram(program), "IMEM loadProgram succeeds after cache invalidation");
        expect(cached.imem.generation() != loadGeneration,
               "IMEM loadProgram bumps cache generation");

        const auto resetGeneration = cached.imem.generation();
        cached.imem.reset();
        expect(cached.imem.generation() != resetGeneration,
               "IMEM reset bumps cache generation");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 1
            mov r2, 2
            mov r3, 3
            halt
        )");
        expect(loadAndReset(vm, program), "block-cache step-limit program loads");
        auto result = sandbox::vm::run(vm, 2);
        expect(result.timeout() && result.steps == 2 && vm.pc == 2,
               "block cache honors max step limit inside a block");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 1 &&
                   sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 2 &&
                   sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 0,
               "partial cached block commits only executed instructions");
    }

    {
        VMState vm(16, 64);
        expect(vm.execution_backend == VMExecutionBackend::CachedBlockInterpreter,
               "decoded trace executor is disabled by default");
        expect(!vm.decodedTraceExecutorEnabled(),
               "decoded trace executor opt-in flag is false by default");
        expect(VMExecutionBackend::TraceJit ==
                   VMExecutionBackend::DecodedTraceExecutor,
               "transition TraceJit backend name aliases decoded trace executor");
    }

    {
        auto compareExecution = [&](const std::string& label,
                                    const std::vector<TritWord27>& program,
                                    const std::vector<int>& memory_addrs,
                                    int max_steps) {
            VMState interpreter(128, 256);
            VMState jit(128, 256);
            expect(loadAndReset(interpreter, program), label + " interpreter program loads");
            expect(loadAndReset(jit, program), label + " JIT program loads");
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            jit.setExecutionBackend(VMExecutionBackend::TraceJit);
            jit.setTraceJitHotThreshold(1);

            auto interpreterResult = sandbox::vm::run(interpreter, max_steps);
            auto jitResult = sandbox::vm::run(jit, max_steps);
            expect(interpreterResult.status == jitResult.status, label + " status matches");
            expect(interpreterResult.steps == jitResult.steps, label + " steps match");
            expect(interpreter.pc == jit.pc, label + " final PC matches");
            expect(interpreter.cycle_count == jit.cycle_count, label + " cycle count matches");
            for (int reg = 0; reg < REG_COUNT; ++reg) {
                expect(interpreter.regfile.read(static_cast<uint8_t>(reg)) ==
                           jit.regfile.read(static_cast<uint8_t>(reg)),
                       label + " register r" + std::to_string(reg) + " matches");
            }
            for (int addr : memory_addrs) {
                auto [interpreterValue, interpreterFault] = interpreter.dmem.load(addr);
                auto [jitValue, jitFault] = jit.dmem.load(addr);
                expect(interpreterFault == jitFault, label + " memory fault matches");
                expect(interpreterValue == jitValue,
                       label + " memory @" + std::to_string(addr) + " matches");
            }
            expect(jit.trace_jit_stats.traces_built > 0, label + " builds a trace");
            expect(jit.trace_jit_stats.instructions_executed > 0,
                   label + " executes trace instructions");
        };

        compareExecution("trace JIT arithmetic loop", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 12
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )"), {}, 128);

        compareExecution("trace JIT load/store", assembleOrThrow(R"(
            mov r1, 10
            mov.t20 r2, 7
            store r2, r1, 0
            load r3, r1, 0
            add.t20 r4, r3, r2
            halt
        )"), {10}, 64);

        compareExecution("trace JIT direct branch", assembleOrThrow(R"(
            mov r1, 1
            jmp done
            mov r1, 2
        done:
            add r2, r1, r1
            halt
        )"), {}, 32);

        VMState user_interpreter(64, 128);
        VMState user_trace(64, 128);
        auto user_program = assembleOrThrow(R"(
            mov r1, 12
            mov r2, 9
            store r2, r1, 0
            load r3, r1, 0
            halt
        )");
        expect(loadAndReset(user_interpreter, user_program) &&
                   loadAndReset(user_trace, user_program),
               "user-mode decoded trace fixture loads");
        user_interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
        user_trace.setExecutionBackend(
            VMExecutionBackend::DecodedTraceExecutor);
        user_trace.setDecodedTraceHotThreshold(1);
        user_interpreter.privilege = PrivilegeMode::User;
        user_trace.privilege = PrivilegeMode::User;
        user_interpreter.user_imem_base = user_trace.user_imem_base = 0;
        user_interpreter.user_imem_limit = user_trace.user_imem_limit = 64;
        user_interpreter.user_dmem_base = user_trace.user_dmem_base = 0;
        user_interpreter.user_dmem_limit = user_trace.user_dmem_limit = 128;
        const auto user_interpreter_result =
            sandbox::vm::run(user_interpreter, 32);
        const auto user_trace_result = sandbox::vm::run(user_trace, 32);
        expect(user_interpreter_result.status == user_trace_result.status &&
                   user_trace_result.halted(),
               "decoded trace executor supports guarded user-mode memory");
        expect(user_interpreter.regfile.read(R3) ==
                   user_trace.regfile.read(R3),
               "user-mode decoded trace load matches interpreter");
        expect(user_trace.decodedTraceStats().instructions_executed > 0,
               "user-mode fixture executes through decoded trace backend");
        bool saw_guarded_load = false;
        bool saw_guarded_store = false;
        for (const auto& cached_trace : user_trace.trace_jit_cache) {
            for (const VMMicroOp& micro_op :
                 cached_trace.second.instructions) {
                expect(micro_op.trap_point &&
                           micro_op.instruction_accounting == 1,
                       "decoded micro-op preserves trap and instruction accounting");
                if (micro_op.memory_effect == VMMicroMemoryEffect::Read) {
                    saw_guarded_load =
                        (micro_op.guards &
                         VM_MICRO_GUARD_ADDRESS_TRANSLATION) != 0;
                }
                if (micro_op.memory_effect == VMMicroMemoryEffect::Write) {
                    saw_guarded_store =
                        (micro_op.guards &
                         VM_MICRO_GUARD_ADDRESS_TRANSLATION) != 0;
                }
            }
        }
        expect(saw_guarded_load && saw_guarded_store,
               "micro-op IR declares guarded read/write memory effects");

        const std::size_t first_address_space_cache_size =
            user_trace.trace_jit_cache.size();
        user_trace.pc = 0;
        user_trace.status = VMStatus::RUNNING;
        user_trace.setCurrentAsid(7);
        const auto second_asid_result = sandbox::vm::run(user_trace, 32);
        expect(second_asid_result.halted(),
               "decoded trace reruns after ASID change");
        expect(user_trace.trace_jit_cache.size() >
                   first_address_space_cache_size,
               "decoded trace cache keys entries by ASID");
    }

    {
        VMState jit(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 3
            syscall 1
            mov r2, 7
            halt
        )");
        expect(loadAndReset(jit, program), "trace JIT syscall fallback program loads");
        jit.setExecutionBackend(VMExecutionBackend::TraceJit);
        jit.setTraceJitHotThreshold(1);
        auto result = sandbox::vm::run(jit, 32);
        expect(result.halted(), "trace JIT syscall fallback halts");
        expect(jit.trace_jit_stats.unsupported_fallbacks > 0,
               "trace JIT records unsupported syscall fallback");
        expect(jit.syscall_buffer == "3", "interpreter handles syscall side effect");
        expect(sandbox::vm::ops::toLong(jit.regfile.read(R2)) == 7,
               "interpreter handles unsupported syscall path");
    }

    {
        VMState interpreter(64, 128);
        VMState native(64, 128);
        auto program = assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 9
        native_loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, native_loop
            halt
        )");
        expect(loadAndReset(interpreter, program) &&
                   loadAndReset(native, program),
               "native x86-64 JIT fixture loads");
        interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
        native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        native.setDecodedTraceHotThreshold(1);
        const auto interpreter_result =
            sandbox::vm::run(interpreter, 128);
        const auto native_result = sandbox::vm::run(native, 128);
        if (nativeX64HostAvailable()) {
            expect(native_result.status == interpreter_result.status &&
                       native_result.steps == interpreter_result.steps,
                   "native x86-64 JIT preserves status and instruction count");
            expect(native.regfile.read(R1) == interpreter.regfile.read(R1),
                   "native x86-64 JIT arithmetic matches interpreter");
            expect(native.native_x64_jit_stats.blocks_built > 0 &&
                       native.native_x64_jit_stats.blocks_executed > 0,
                   "native x86-64 JIT builds and executes code blocks");
            expect(native.native_x64_jit_stats.direct_instructions > 0,
                   "native x86-64 JIT accounts executed direct lowerings");
            bool all_wx = !native.native_x64_code_cache.empty();
            bool all_lowered = !native.native_x64_code_cache.empty();
            for (const auto& cached : native.native_x64_code_cache) {
                const auto block =
                    std::static_pointer_cast<VMNativeX64CodeBlock>(
                        cached.second);
                all_wx = all_wx && block->isWriteXorExecute();
                all_lowered = all_lowered &&
                              block->direct_instruction_count > 0;
            }
            expect(all_wx,
                   "native x86-64 JIT code cache is RX and never left W+X");
            expect(all_lowered,
                   "native x86-64 JIT caches only emitted direct blocks");

            std::uint32_t random = 0x51A7u;
            for (int sample = 0; sample < 27; ++sample) {
                random = random * 1664525u + 1013904223u;
                const int a = static_cast<int>(random % 19u) - 9;
                random = random * 1664525u + 1013904223u;
                const int b = static_cast<int>(random % 9u) - 4;
                const std::string source =
                    "mov r1, " + std::to_string(a) + "\n" +
                    "mov r2, " + std::to_string(b) + "\n" +
                    "add r3, r1, r2\n"
                    "sub r4, r3, r2\n"
                    "mul r5, r4, r2\n"
                    "neg r6, r5\n"
                    "abs r7, r6\n"
                    "halt\n";
                const auto randomized_program =
                    assembleOrThrow(source);
                VMState oracle(32, 64);
                VMState generated(32, 64);
                expect(loadAndReset(oracle, randomized_program) &&
                           loadAndReset(generated, randomized_program),
                       "randomized native differential fixture loads");
                oracle.setExecutionBackend(
                    VMExecutionBackend::Interpreter);
                generated.setExecutionBackend(
                    VMExecutionBackend::NativeX64Jit);
                generated.setDecodedTraceHotThreshold(1);
                const auto oracle_result =
                    sandbox::vm::run(oracle, 32);
                const auto generated_result =
                    sandbox::vm::run(generated, 32);
                expect(oracle_result.status == generated_result.status &&
                           oracle_result.steps == generated_result.steps,
                       "randomized native status/step parity");
                for (int reg = 1; reg <= 7; ++reg) {
                    expect(oracle.regfile.read(
                               static_cast<std::uint8_t>(reg)) ==
                               generated.regfile.read(
                                   static_cast<std::uint8_t>(reg)),
                           "randomized native register parity");
                }
            }
        } else {
            expect(native.native_x64_jit_stats.blocks_built == 0,
                   "non-x86 hosts keep native backend unavailable");
        }
    }

    {
        VMState jit(32, 16);
        auto program = assembleOrThrow(R"(
            mov r1, 99
        loop:
            load r2, r1, 0
            jmp loop
        )");
        expect(loadAndReset(jit, program), "trace JIT memory bailout program loads");
        jit.setExecutionBackend(VMExecutionBackend::TraceJit);
        jit.setTraceJitHotThreshold(1);
        auto result = sandbox::vm::run(jit, 16);
        expect(result.trapped(), "trace JIT unsafe load falls back to interpreter trap");
        expect(result.trap_code == TrapCode::TRAP_MEM_FAULT,
               "trace JIT unsafe load preserves memory trap");
        expect(jit.trace_jit_stats.interpreter_bailouts > 0,
               "trace JIT records unsafe memory bailout");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t5  r1, -5
            mov.t20 r2, 20
            mov     r3, 50
            mov.t1  r4, -1
            tsel    r5, r4, r1, r2, r3
            mov.t1  r4, 0
            tsel    r6, r4, r1, r2, r3
            mov.t1  r4, 1
            tsel    r7, r4, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "TSEL program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "TSEL program halts");
        expect(vm.regfile.read(R5).mode == TernaryMode::T5 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R5)) == -5,
               "TSEL preserves negative arm tag/value");
        expect(vm.regfile.read(R6).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R6)) == 20,
               "TSEL preserves zero arm tag/value");
        expect(vm.regfile.read(R7).mode == TernaryMode::T40 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 50,
               "TSEL preserves positive arm tag/value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t1 r1, 0
            brz    r1, zero_path
            mov    r2, 999
zero_path:
            mov.t1 r1, 1
            brp    r1, pos_path
            mov    r2, 999
pos_path:
            mov    r2, 7
            halt
        )");
        expect(loadAndReset(vm, program), "BRZ/BRP program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "BRZ/BRP program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 7,
               "BRZ and BRP both branch on T1 predicates");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t5  r1, 11
            mov.t20 r2, 22
            swap    r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "SWAP program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "SWAP program halts");
        expect(vm.regfile.read(R1).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 22,
               "SWAP moves second value into first register");
        expect(vm.regfile.read(R2).mode == TernaryMode::T5 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 11,
               "SWAP moves first value into second register");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow(R"(
            mov.t10 r1, 42
            cvt.t10.t20 r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "cvt.src.dst program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "cvt.src.dst program halts");
        expect(vm.regfile.read(R2).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 42,
               "CVT source/destination suffix converts value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20     r1, 42
            cvt.t20.l20 r2, r1
            cvt.l20.t20 r3, r2
            halt
        )");
        expect(loadAndReset(vm, program), "numeric/lane CVT program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "numeric/lane CVT program halts");
        expect(vm.regfile.read(R2).mode == TernaryMode::L20, "CVT writes L20 tag");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 42,
               "CVT lane to matching numeric recovers value");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.l1    r1, 1
            mov.l1    r2, 1
            tladd.l1  r3, r1, r2
            tlsub.l1  r4, r1, r3
            tlneg.l1  r5, r3
            tland.l1  r6, r1, r3
            tlor.l1   r7, r1, r3
            halt
        )");
        expect(loadAndReset(vm, program), "scalar lane program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "scalar lane program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::L1 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -1,
               "TLADD is carryless modulo per trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R4)) == -1,
               "TLSUB is carryless modulo per trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 1,
               "TLNEG flips lane trit");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R6)) == -1,
               "TLAND is per-trit lattice min");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 1,
               "TLOR is per-trit lattice max");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.l20 r1, 7
            add.t20 r2, r1, r1
            halt
        )");
        expect(loadAndReset(vm, program), "numeric view over lane-written register loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "instruction-selected numeric view is not a register tag");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 14,
               "numeric view reinterprets the fixed physical scalar word");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("tlneg.l20 r2, r1\nhalt\n");
        expect(loadAndReset(vm, program), "invalid lane payload program loads");
        vm.regfile.write(R1, TernaryValue::fromL20(TritLane20::invalid()));
        auto result = sandbox::vm::run(vm, 8);
        expect(result.trapped(), "TLNEG rejects invalid lane payload");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "invalid lane trap code");
    }

    {
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 2
            mov.t20 r2, 3
            mov.t20 r3, 7
            twcmp.t20  r4, r1, r2, r3
            mov.t20 r1, 5
            twcmp.t20  r5, r1, r2, r3
            mov.t20 r1, 9
            twcmp.t20  r6, r1, r2, r3
            tclamp.t20 r7, r1, r2, r3
            mov.t20 r1, 1
            tclamp.t20 r8, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "window compare/clamp program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "window compare/clamp program halts");
        expect(readTrit0(vm.regfile.read(R4)) == T_NEG, "TWCMP reports below window");
        expect(readTrit0(vm.regfile.read(R5)) == T_ZER, "TWCMP reports inside window");
        expect(readTrit0(vm.regfile.read(R6)) == T_POS, "TWCMP reports above window");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R7)) == 7, "TCLAMP clamps high");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R8)) == 3, "TCLAMP clamps low");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 5
            mov.t20 r2, 9
            mov.t20 r3, 3
            twcmp.t20 r4, r1, r2, r3
            halt
        )");
        expect(loadAndReset(vm, program), "invalid window program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "TWCMP traps on inverted bounds");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "TWCMP inverted bounds trap code");
    }

    {
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, -8
            mov.t20 r2, 3
            tmod.t20 r3, r1, r2
            mov.t20 r4, 2
            tlshift.t20 r5, r4, r2
            trshift.t20 r6, r5, r2
            mov.t5 r7, 9
            tcount.t5 r8, r7
            tscan.t5  r9, r7
            mov.t5 r10, 0
            tscan.t5 r11, r10
            halt
        )");
        expect(loadAndReset(vm, program), "Phase 2 scalar numeric program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "Phase 2 scalar numeric program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -2,
               "TMOD remainder follows dividend sign");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 54,
               "TLSHIFT scales by powers of three");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R6)) == 2,
               "TRSHIFT inverse scales by powers of three");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R8)) == 1,
               "TCOUNT counts non-zero trits");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R9)) == 2,
               "TSCAN returns first non-zero trit position");
        expect(vm.regfile.read(R11).mode == TernaryMode::T1 &&
               readTrit0(vm.regfile.read(R11)) == T_NEG,
               "TSCAN all-zero returns T1 -1 sentinel");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 5
            mov.t20 r2, 0
            tmod.t20 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "TMOD divisor zero program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(), "TMOD traps on zero divisor");
        expect(result.trap_code == TrapCode::TRAP_DIV_ZERO, "TMOD zero divisor trap code");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 6
            mov.t20 r2, 7
            aclr.t40
            tmac.t20 r1, r2
            astore.t40 r3
            halt
        )");
        expect(loadAndReset(vm, program), "TMAC program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TMAC program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 42,
               "TMAC multiplies into accumulator");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            syscall 3
            mov.t20 r1, 42
            syscall 1
            syscall 2
            fence
            halt
        )");
        expect(loadAndReset(vm, program), "SYSCALL/FENCE program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "SYSCALL/FENCE program halts");
        expect(vm.syscall_buffer == "42\n", "SYSCALL writes sandbox output buffer");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 4
            callr r1
            mov r2, 999
            halt
            mov r2, 7
            mov r3, 3
            jmpr r3
        )");
        expect(loadAndReset(vm, program), "CALLR/JMPR program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "CALLR/JMPR program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 7,
               "CALLR jumps to absolute PC target");
        expect(sandbox::vm::ops::toLong(vm.regfile.readLR()) == 2,
               "CALLR writes link register with return PC");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            vsum.t20  r1, v0
            vhmin.t20 r2, v0
            vhmax.t20 r3, v0
            halt
        )");
        expect(loadAndReset(vm, program), "vector reduction program loads");
        vm.vector_length = 3;
        vm.vregfile.reset(3);
        vm.vector_faults.reset(3);
        const long long values[] = {3, -2, 5};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector reduction program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 6, "VSUM reduces lanes");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == -2, "VHMIN reduces lanes");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 5, "VHMAX reduces lanes");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("vlen r1\nhalt\n");
        expect(loadAndReset(vm, program), "VLEN program loads");
        auto result = sandbox::vm::run(vm, 8);
        expect(result.halted(), "VLEN program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == DEFAULT_VECTOR_LENGTH, "VLEN returns default length");
        expect(vm.vregfile.reg[0].lane.size() == DEFAULT_VECTOR_LENGTH, "vector registers allocate default lanes");
        expect(vm.vector_faults.fault_valid.size() == DEFAULT_VECTOR_LENGTH && !vm.vector_faults.any(),
               "vector fault masks reset to default length");
    }

    {
        VMState vm(64, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            mov.t20   r1, 3
            mov.t20   r2, 5
            vbcast.t20 v0, r1
            vbcast.t20 v1, r2
            vadd.t20   v2, v0, v1
            vsub.t20   v3, v1, v0
            vneg.t20   v4, v3
            vmul.t20   v5, v3, v1
            vcmp.t20   v6, v0, v1
            vsel.t20   v7, v6, v4, v0, v5
            halt
        )");
        expect(loadAndReset(vm, program), "vector arithmetic program loads");
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "vector arithmetic program halts");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            expect(vectorMode(vm, 2, lane) == TernaryMode::T20 && vectorLong(vm, 2, lane) == 8,
                   "VADD.t20 lane result");
            expect(vectorLong(vm, 3, lane) == 2, "VSUB.t20 lane result");
            expect(vectorLong(vm, 4, lane) == -2, "VNEG.t20 lane result");
            expect(vectorLong(vm, 5, lane) == 10, "VMUL.t20 lane result");
            expect(vectorMode(vm, 6, lane) == TernaryMode::L1 &&
                   vectorPredicateTrit(vm, 6, lane) == -1,
                   "VCMP.t20 writes L1 predicate");
            expect(vectorLong(vm, 7, lane) == -2, "VSEL.t20 chooses negative arm");
        }
        expect(!vm.vector_faults.any(), "vector arithmetic has no lane faults");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow("vsel.t20 v7, v6, v0, v1, v2\nhalt\n");
        expect(loadAndReset(vm, program), "three-arm VSEL program loads");
        TritLane1 neg; neg.setTrit(0, -1);
        TritLane1 zer; zer.setTrit(0, 0);
        TritLane1 pos; pos.setTrit(0, 1);
        vm.vregfile.reg[6].write(0, TernaryValue::fromL1(neg));
        vm.vregfile.reg[6].write(1, TernaryValue::fromL1(zer));
        vm.vregfile.reg[6].write(2, TernaryValue::fromL1(pos));
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(-10 - lane)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(20 + lane)));
            vm.vregfile.reg[2].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(30 + lane)));
        }
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "three-arm VSEL program halts");
        expect(vectorLong(vm, 7, 0) == -10, "VSEL chooses negative lane arm");
        expect(vectorLong(vm, 7, 1) == 21, "VSEL chooses zero lane arm");
        expect(vectorLong(vm, 7, 2) == 32, "VSEL chooses positive lane arm");
        expect(!vm.vector_faults.any(), "three-arm VSEL has no lane faults");
    }

    {
        VMState vm(64, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            mov r1, 10
            vload.t20  v0, r1, 0
            vstore.t20 v0, r1, 20
            halt
        )");
        expect(loadAndReset(vm, program), "vector load/store program loads");
        for (int i = 0; i < vm.vector_length; ++i) {
            vm.dmem.store(10 + i, TernaryValue::fromT5(native_ops::fromIntT5(2 + i)));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector load/store program halts");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            expect(vectorMode(vm, 0, lane) == TernaryMode::T20 &&
                   vectorLong(vm, 0, lane) == 2 + lane,
                   "VLOAD converts contiguous memory to suffix type");
            auto [stored, fc] = vm.dmem.load(30 + lane);
            expect(fc == MemFaultCode::OK && stored.mode == TernaryMode::T20 &&
                   sandbox::vm::ops::toLong(stored) == 2 + lane,
                   "VSTORE writes contiguous converted memory");
        }
        expect(!vm.vector_faults.any(), "vector load/store has no lane faults");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 2;
        auto program = assembleOrThrow(R"(
            mov.t5 r1, 5
            mov.t5 r2, 6
            vbcast.t5 v0, r1
            vbcast.t5 v1, r2
            vadd.t5 v2, v0, v1
            halt
        )");
        expect(loadAndReset(vm, program), "T5 vector arithmetic program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T5 vector arithmetic program halts");
        expect(vectorMode(vm, 2, 0) == TernaryMode::T5 && vectorLong(vm, 2, 0) == 11,
               "VADD.t5 executes");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow("vdiv.t20 v2, v0, v1\nhalt\n");
        expect(loadAndReset(vm, program), "vector divide fault program loads");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(9)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(lane == 2 ? 0 : 3)));
        }
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "VDIV lane fault program still halts");
        expect(vectorLong(vm, 2, 0) == 3, "VDIV valid lane result");
        expect(vectorLong(vm, 2, 2) == 0, "VDIV fault lane typed zero");
        expect(vm.vector_faults.fault_valid[2] &&
               vm.vector_faults.fault_class[2] == TrapCode::TRAP_DIV_ZERO,
               "VDIV records lane divide-by-zero");
        expect(!vm.vector_faults.fault_valid[0], "VDIV leaves valid lane fault clear");
    }

    {
        VMState vm(16, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow("vadd.t20 v2, v0, v1\nhalt\n");
        expect(loadAndReset(vm, program), "vector wrong-tag fault program loads");
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(1)));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(2)));
        }
        vm.vregfile.reg[1].write(1, TernaryValue::fromL20(toLane(native_ops::fromIntT20(2))));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "vector wrong-tag lane fault program still halts");
        expect(vectorLong(vm, 2, 0) == 3, "VADD valid lane survives wrong-tag neighbor");
        expect(vectorLong(vm, 2, 1) == 0, "VADD wrong-tag lane typed zero");
        expect(vm.vector_faults.fault_valid[1] &&
               vm.vector_faults.fault_class[1] == TrapCode::TRAP_ILLEGAL_OP,
               "VADD records wrong-tag lane fault");
    }

    {
        VMState vm(16, 12);
        vm.vector_length = 4;
        auto program = assembleOrThrow("mov r1, 10\nvload.t20 v0, r1, 0\nhalt\n");
        expect(loadAndReset(vm, program), "vector memory fault program loads");
        vm.dmem.store(10, TernaryValue::fromT20(native_ops::fromIntT20(10)));
        vm.dmem.store(11, TernaryValue::fromT20(native_ops::fromIntT20(11)));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.trapped(), "VLOAD faults precisely before commit");
        expect(vectorLong(vm, 0, 0) == 0 && vectorLong(vm, 0, 1) == 0 &&
                   vectorLong(vm, 0, 2) == 0 && vectorLong(vm, 0, 3) == 0,
               "VLOAD fault leaves the entire destination unchanged");
        expect(vm.vector_faults.fault_valid[2] &&
                   vm.vector_faults.fault_valid[3] &&
                   vm.vector_faults.first_failing_lane == 2 &&
                   vm.vector_faults.fault_class[2] ==
                       TrapCode::TRAP_MEM_FAULT,
               "VLOAD records the complete fault mask and first lane");
    }

    {
        VMState vm(16, 64);
        auto program = assembleOrThrow("mov.l20 r1, 7\nvbcast.t20 v0, r1\nhalt\n");
        expect(loadAndReset(vm, program), "VBCAST structural fault program loads");
        auto result = sandbox::vm::run(vm, 16);
        expect(result.trapped(), "VBCAST rejects lane-family scalar source structurally");
        expect(result.trap_code == TrapCode::TRAP_ILLEGAL_OP, "VBCAST structural trap code");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov.t20 r1, 3
            mov.t20 r2, 4
            aclr.t50
            aload.t20 r1
            aadd.t20  r2
            amul.t20  r2
            asub.t20  r1
            astore.t20 r3
            halt
        )");
        expect(loadAndReset(vm, program), "accumulator program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "accumulator program halts");
        expect(vm.regfile.read(R3).mode == TernaryMode::T20 &&
               sandbox::vm::ops::toLong(vm.regfile.read(R3)) == 25,
               "accumulator keeps T50 internal precision and stores selected width");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 4;
        auto program = assembleOrThrow(R"(
            vdot.t1 r1, v0, v1
            vmac.t1 v0, v1
            astore.t50 r4
            vact.t1 v2, v3
            halt
        )");
        expect(loadAndReset(vm, program), "T1 AI program loads");
        auto l1 = [](int8_t trit) {
            TritLane1 lane;
            lane.setTrit(0, trit);
            return TernaryValue::fromL1(lane);
        };
        const int8_t a[] = {1, 1, 0, -1};
        const int8_t b[] = {1, -1, 1, -1};
        const long long signs[] = {-5, 0, 7, -1};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, l1(a[lane]));
            vm.vregfile.reg[1].write(lane, l1(b[lane]));
            vm.vregfile.reg[3].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(signs[lane])));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "T1 AI program halts");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R1)) == 1,
               "VDOT.t1 writes T50 dot product to scalar register");
        expect(sandbox::vm::ops::toLong(vm.regfile.read(R4)) == 1,
               "VMAC.t1 accumulates T1 dot product into accumulator");
        expect(vectorPredicateTrit(vm, 2, 0) == -1 &&
               vectorPredicateTrit(vm, 2, 1) == 0 &&
               vectorPredicateTrit(vm, 2, 2) == 1 &&
               vectorPredicateTrit(vm, 2, 3) == -1,
               "VACT.t1 writes sign predicates");
        expect(!vm.vector_faults.any(), "T1 AI program has no lane faults");
    }

    {
        VMState vm(32, 64);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            vpack.t20.t10   v1, v0
            vunpack.t10.t20 v2, v1
            vpermute.t20    v3, v2, v4
            vblend.t20      v6, v5, v2, v3
            vswap           v1, v2
            halt
        )");
        expect(loadAndReset(vm, program), "vector plumbing program loads");
        const long long values[] = {10, 20, 30};
        const long long indices[] = {2, 0, 1};
        const int8_t cond[] = {-1, 0, 1};
        auto l1 = [](int8_t trit) {
            TritLane1 lane;
            lane.setTrit(0, trit);
            return TernaryValue::fromL1(lane);
        };
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT20(native_ops::fromIntT20(values[lane])));
            vm.vregfile.reg[4].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(indices[lane])));
            vm.vregfile.reg[5].write(lane, l1(cond[lane]));
        }
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "vector plumbing program halts");
        expect(vectorMode(vm, 1, 0) == TernaryMode::T20 &&
               vectorMode(vm, 2, 0) == TernaryMode::T10,
               "VSWAP exchanges whole vector register payloads");
        expect(vectorLong(vm, 3, 0) == 30 &&
               vectorLong(vm, 3, 1) == 10 &&
               vectorLong(vm, 3, 2) == 20,
               "VPERMUTE reorders lanes by index vector");
        expect(vectorLong(vm, 6, 0) == 10 &&
               vectorLong(vm, 6, 1) == 20 &&
               vectorLong(vm, 6, 2) == 20,
               "VBLEND selects false for non-positive and true for positive");
        expect(!vm.vector_faults.any(), "vector plumbing program has no lane faults");
    }

    {
        VMState vm(32, 32);
        vm.vector_length = 3;
        auto program = assembleOrThrow(R"(
            mov r1, 10
            vgather.t20  v2, r1, v0
            vscatter.t20 v2, r1, v1
            halt
        )");
        expect(loadAndReset(vm, program), "gather/scatter program loads");
        const long long gatherIdx[] = {0, 2, 4};
        const long long scatterIdx[] = {6, 7, 40};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(gatherIdx[lane])));
            vm.vregfile.reg[1].write(lane, TernaryValue::fromT5(native_ops::fromIntT5(scatterIdx[lane])));
        }
        vm.dmem.store(10, TernaryValue::fromT5(native_ops::fromIntT5(11)));
        vm.dmem.store(12, TernaryValue::fromT5(native_ops::fromIntT5(22)));
        vm.dmem.store(14, TernaryValue::fromT5(native_ops::fromIntT5(33)));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.trapped(),
               "scatter faults precisely after successful gather");
        auto [stored0, fc0] = vm.dmem.load(16);
        auto [stored1, fc1] = vm.dmem.load(17);
        expect(fc0 == MemFaultCode::OK &&
                   sandbox::vm::ops::toLong(stored0) == 0,
               "faulting VSCATTER writes no earlier lane");
        expect(fc1 == MemFaultCode::OK &&
                   sandbox::vm::ops::toLong(stored1) == 0,
               "faulting VSCATTER writes no lanes");
        expect(vm.vector_faults.fault_valid[2] &&
               vm.vector_faults.first_failing_lane == 2 &&
               vm.vector_faults.fault_class[2] == TrapCode::TRAP_MEM_FAULT,
               "VSCATTER records first out-of-range indexed lane");
    }

    {
        VMState vm;
        expect(!sandbox::vm::trapValid(vm.trap_reg), "reset trap record is invalid/no-fault");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 0) == sandbox::isa::FAULT_VALID_NONE,
               "reset trap_valid trit is zero");
        vm.trap(TrapCode::TRAP_ILLEGAL_OP);
        expect(sandbox::vm::trapValid(vm.trap_reg), "trap record valid bit set");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 0) == sandbox::isa::FAULT_VALID_SET,
               "trap_valid trit set");
        expect(sandbox::vm::readStoredTrit(vm.trap_reg, 1) == sandbox::isa::T_POS,
               "trap_class trit stores illegal op");
        expect(sandbox::vm::decodeTrap(vm.trap_reg) == TrapCode::TRAP_ILLEGAL_OP,
               "two-trit trap record decodes");
    }
}
