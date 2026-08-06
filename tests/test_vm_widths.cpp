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
            expect(native.cycle_count == interpreter.cycle_count,
                   "native x86-64 JIT inline commits preserve cycle count");
            expect(native.branch_instructions_count ==
                       interpreter.branch_instructions_count,
                   "native x86-64 JIT direct branches preserve branch count");
            expect(native.regfile.read(R1) == interpreter.regfile.read(R1),
                   "native x86-64 JIT arithmetic matches interpreter");
            expect(native.native_x64_jit_stats.blocks_built > 0 &&
                       native.native_x64_jit_stats.blocks_executed > 0,
                   "native x86-64 JIT builds and executes code blocks");
            expect(native.native_x64_jit_stats.direct_instructions > 0,
                   "native x86-64 JIT accounts executed direct lowerings");
            bool all_wx = !native.native_x64_code_cache.empty();
            bool all_lowered = !native.native_x64_code_cache.empty();
            bool all_lowerings_accounted = !native.native_x64_code_cache.empty();
            bool saw_helper_backed_lowering = false;
            for (const auto& cached : native.native_x64_code_cache) {
                const auto block =
                    std::static_pointer_cast<VMNativeX64CodeBlock>(
                        cached.second);
                all_wx = all_wx && block->isWriteXorExecute();
                all_lowered = all_lowered &&
                              block->direct_instruction_count > 0;
                all_lowerings_accounted = all_lowerings_accounted &&
                    block->direct_instruction_count +
                            block->helper_instruction_count ==
                        block->lowered.size();
                saw_helper_backed_lowering = saw_helper_backed_lowering ||
                    block->helper_instruction_count > 0;
            }
            expect(all_wx,
                   "native x86-64 JIT code cache is RX and never left W+X");
            expect(all_lowered,
                   "native x86-64 JIT caches only emitted direct blocks");
            expect(all_lowerings_accounted,
                   "native x86-64 JIT accounts direct and helper lowerings");
            expect(!saw_helper_backed_lowering,
                   "native x86-64 JIT emits the arithmetic/control hot loop directly");
            expect(native.native_x64_jit_stats.direct_instructions ==
                       native.native_x64_jit_stats.instructions_executed,
                   "native x86-64 JIT direct count covers the inline hot loop");

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
        // The scalar native subset must execute the ordinary normalized T40
        // integer encodings directly.  In particular, fromIntT40(1) has a
        // non-32 exponent after normalization, so parity alone is not enough:
        // the cached Add/Sub/TCmp lowerings and their raw output bits are
        // checked for every boundary corpus entry.
        const auto arithmetic_program = assembleOrThrow(R"(
            add  r3, r1, r2
            sub  r4, r1, r2
            tcmp r5, r1, r2
            halt
        )");
        struct IntPair { long long a; long long b; };
        const long long mantissa_max = static_cast<long long>(
            (native_ops::detail::pow3(33).toUint64() - 1) / 2);
        std::vector<IntPair> cases = {
            {0, 0}, {1, 0}, {-1, 0}, {2, -2}, {27, -27},
            {-27, 2}, {1, 27}, {-2, -27},
            {mantissa_max, 0}, {-mantissa_max, 0},
            {mantissa_max, 1}, {mantissa_max, mantissa_max},
            {-mantissa_max, -1}, {-mantissa_max, -mantissa_max},
        };
        std::uint32_t random = 0xC001D00Du;
        for (int sample = 0; sample < 32; ++sample) {
            random = random * 1664525u + 1013904223u;
            const long long a = static_cast<long long>(random % 55u) - 27;
            random = random * 1664525u + 1013904223u;
            const long long b = static_cast<long long>(random % 55u) - 27;
            cases.push_back({a, b});
        }

        for (const IntPair values : cases) {
            VMState interpreter(32, 64);
            VMState native(32, 64);
            expect(loadAndReset(interpreter, arithmetic_program) &&
                       loadAndReset(native, arithmetic_program),
                   "native scalar boundary programs load");
            const Triple a_value = native_ops::fromIntT40(values.a);
            const Triple b_value = native_ops::fromIntT40(values.b);
            interpreter.regfile.write(
                R1, TernaryValue::fromTriple(a_value));
            interpreter.regfile.write(
                R2, TernaryValue::fromTriple(b_value));
            native.regfile.write(R1, TernaryValue::fromTriple(a_value));
            native.regfile.write(R2, TernaryValue::fromTriple(b_value));
            expect(native.regfile.reg[R1].bits.lo == a_value.data &&
                       native.regfile.reg[R2].bits.lo == b_value.data,
                   "native scalar inputs preserve fromIntT40 raw bits");
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto interpreter_result =
                sandbox::vm::run(interpreter, 32);
            const auto native_result = sandbox::vm::run(native, 32);
            expect(native_result.status == interpreter_result.status &&
                       native_result.trap_code == interpreter_result.trap_code &&
                       native_result.steps == interpreter_result.steps,
                   "native scalar status and accounting parity");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count,
                   "native scalar PC and cycle parity");
            for (const std::uint8_t reg : {R3, R4, R5}) {
                expect(native.regfile.reg[reg].bits.lo ==
                           interpreter.regfile.reg[reg].bits.lo &&
                           native.regfile.reg[reg].bits.hi ==
                           interpreter.regfile.reg[reg].bits.hi &&
                           native.regfile.view_mode[reg] ==
                           interpreter.regfile.view_mode[reg],
                       "native scalar raw result parity");
            }
            const Triple expected_add = native_ops::add(
                a_value, b_value);
            const Triple expected_sub = native_ops::subtract(
                a_value, b_value);
            const int8_t expected_cmp = native_ops::compare(
                a_value, b_value);
            expect(native.regfile.reg[R3].bits.lo == expected_add.data,
                   "native ADD matches authoritative T40 raw result");
            expect(native.regfile.reg[R4].bits.lo == expected_sub.data,
                   "native SUB matches authoritative T40 raw result");
            expect(native.regfile.reg[R5].bits.lo ==
                       native_ops::fromIntT40(expected_cmp).data,
                   "native TCMP matches authoritative T1 physical result");

            if (nativeX64HostAvailable()) {
                bool saw_add = false;
                bool saw_sub = false;
                bool saw_tcmp = false;
                for (const auto& cached : native.native_x64_code_cache) {
                    const auto block =
                        std::static_pointer_cast<VMNativeX64CodeBlock>(
                            cached.second);
                    for (const VMNativeX64Instruction& instruction :
                         block->lowered) {
                        const bool direct = nativeX64InstructionIsDirect(
                            instruction, block->lowered.size());
                        saw_add = saw_add ||
                            (instruction.op == VMMicroOpcode::Add && direct);
                        saw_sub = saw_sub ||
                            (instruction.op == VMMicroOpcode::Sub && direct);
                        saw_tcmp = saw_tcmp ||
                            (instruction.op == VMMicroOpcode::TCmp && direct);
                    }
                }
                expect(saw_add && saw_sub && saw_tcmp,
                       "native scalar cache marks ADD/SUB/TCMP as direct");
                expect(native.native_x64_jit_stats.portable_side_exits == 0,
                       "ordinary scalar values stay on direct native path");
            }
        }
    }

    {
        // Raw T40 NEG/ABS lowerings operate on the canonical positional
        // mantissa, so fractional values and exponent boundaries must remain
        // bit-for-bit identical to the authoritative native_ops result.
        const auto unary_program = assembleOrThrow(R"(
            neg r2, r1
            abs r3, r1
            halt
        )");
        const std::uint64_t pow3_mantissa =
            native_ops::detail::pow3(33).toUint64();
        const std::uint64_t pow3_t40 =
            native_ops::detail::pow3(40).toUint64();
        const auto rawT40 = [&](std::uint64_t mantissa, int exponent) {
            const std::uint64_t exponent_code =
                static_cast<std::uint64_t>(exponent + 1093);
            return TernaryValue::fromTriple(Triple{
                exponent_code * pow3_mantissa + mantissa});
        };

        std::vector<std::pair<std::string, TernaryValue>> unary_cases = {
            {"canonical +0", TernaryValue::zero(TernaryMode::T40)},
            {"canonical -0", TernaryValue::fromTriple(
                sandbox::ops::fromDouble(-0.0))},
            {"mantissa low", rawT40(1, -1093)},
            {"mantissa midpoint-1", rawT40(
                (pow3_mantissa - 1) / 2 - 1, 0)},
            {"mantissa midpoint", rawT40(
                (pow3_mantissa - 1) / 2, 0)},
            {"mantissa midpoint+1", rawT40(
                (pow3_mantissa - 1) / 2 + 1, 0)},
            {"mantissa high", rawT40(pow3_mantissa - 1, 1093)},
            {"minimum exponent", rawT40(
                (pow3_mantissa - 1) / 2 + 1, -1093)},
            {"maximum exponent", rawT40(
                (pow3_mantissa - 1) / 2 - 1, 1093)},
            {"maximum valid raw", TernaryValue::fromTriple(
                Triple{pow3_t40 - 1})},
            {"overflow sentinel", TernaryValue::fromTriple(Triple::Overflow)},
            {"underflow sentinel", TernaryValue::fromTriple(Triple::Underflow)},
            {"invalid raw", TernaryValue::fromTriple(Triple{pow3_t40})},
        };
        std::uint64_t random = 0xA11CE40D5EEDu;
        for (int sample = 0; sample < 48; ++sample) {
            random = random * 6364136223846793005ULL + 1442695040888963407ULL;
            const std::uint64_t mantissa = random % pow3_mantissa;
            random = random * 6364136223846793005ULL + 1442695040888963407ULL;
            const int exponent = static_cast<int>(random % 2187ULL) - 1093;
            unary_cases.push_back({
                "random raw " + std::to_string(sample),
                rawT40(mantissa, exponent)});
        }

        const auto compareUnary = [&](const std::string& label,
                                      const TernaryValue& input,
                                      bool hostile_mode) {
            VMState interpreter(32, 32);
            VMState native(32, 32);
            expect(loadAndReset(interpreter, unary_program) &&
                       loadAndReset(native, unary_program),
                   label + " unary programs load");
            // Seed the physical word directly for hostile-mode coverage;
            // ordinary writes canonicalize their physical mode to T40.
            if (hostile_mode) {
                const TernaryValue tagged = TernaryValue::fromT5(
                    native_ops::fromIntT5(3));
                interpreter.regfile.reg[R1] = tagged;
                native.regfile.reg[R1] = tagged;
            } else {
                interpreter.regfile.reg[R1] = input;
                native.regfile.reg[R1] = input;
            }
            interpreter.regfile.view_mode[R1] = TernaryMode::T40;
            native.regfile.view_mode[R1] = TernaryMode::T40;
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);

            const auto interpreter_result = sandbox::vm::run(interpreter, 16);
            const auto native_result = sandbox::vm::run(native, 16);
            expect(native_result.status == interpreter_result.status &&
                       native_result.trap_code == interpreter_result.trap_code &&
                       native_result.steps == interpreter_result.steps,
                   label + " unary status and accounting parity");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count,
                   label + " unary PC/cycle parity");
            for (const std::uint8_t reg : {R1, R2, R3}) {
                expect(native.regfile.reg[reg] == interpreter.regfile.reg[reg] &&
                           native.regfile.view_mode[reg] ==
                               interpreter.regfile.view_mode[reg],
                       label + " unary raw register parity");
            }

            if (nativeX64HostAvailable()) {
                bool saw_neg_direct = false;
                bool saw_abs_direct = false;
                for (const auto& cached : native.native_x64_code_cache) {
                    const auto block =
                        std::static_pointer_cast<VMNativeX64CodeBlock>(
                            cached.second);
                    for (const VMNativeX64Instruction& instruction :
                         block->lowered) {
                        const bool direct = nativeX64InstructionIsDirect(
                            instruction, block->lowered.size());
                        saw_neg_direct = saw_neg_direct ||
                            (instruction.op == VMMicroOpcode::Neg && direct);
                        saw_abs_direct = saw_abs_direct ||
                            (instruction.op == VMMicroOpcode::Abs && direct);
                    }
                }
                expect(saw_neg_direct && saw_abs_direct,
                       label + " cache classifies NEG/ABS as direct");
                const bool valid_raw =
                    !hostile_mode && input.mode == TernaryMode::T40 &&
                    input.bits.hi == 0 && input.bits.lo < pow3_t40;
                if (valid_raw) {
                    expect(native.native_x64_jit_stats.portable_side_exits == 0,
                           label + " valid raw NEG/ABS stays direct");
                    expect(native.native_x64_jit_stats.direct_instructions >= 2,
                           label + " valid raw NEG/ABS counts direct commits");
                } else {
                    expect(native.native_x64_jit_stats.portable_side_exits > 0,
                           label + " invalid/tagged/special raw side-exits");
                }
            }
        };

        for (const auto& test_case : unary_cases)
            compareUnary(test_case.first, test_case.second, false);
        compareUnary("hostile physical T5 tag", TernaryValue::zero(), true);
    }

    {
        // MUL directly lowers the exact integral subset and guards signed
        // host overflow, fractional operands, specials, and invalid words.
        const auto mul_program = assembleOrThrow(R"(
            mul r3, r1, r2
            halt
        )");
        const auto compareMul = [&](const std::string& label,
                                    const TernaryValue& lhs,
                                    const TernaryValue& rhs,
                                    bool expect_direct) {
            VMState interpreter(32, 32);
            VMState native(32, 32);
            expect(loadAndReset(interpreter, mul_program) &&
                       loadAndReset(native, mul_program),
                   label + " mul programs load");
            interpreter.regfile.write(R1, lhs);
            interpreter.regfile.write(R2, rhs);
            native.regfile.write(R1, lhs);
            native.regfile.write(R2, rhs);
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto interpreter_result = sandbox::vm::run(interpreter, 16);
            const auto native_result = sandbox::vm::run(native, 16);
            expect(native_result.status == interpreter_result.status &&
                       native_result.trap_code == interpreter_result.trap_code &&
                       native_result.steps == interpreter_result.steps,
                   label + " mul status and accounting parity");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count &&
                       native.regfile.reg[R3] == interpreter.regfile.reg[R3] &&
                       native.regfile.view_mode[R3] ==
                           interpreter.regfile.view_mode[R3],
                   label + " mul raw result parity");
            if (nativeX64HostAvailable()) {
                bool saw_mul_direct = false;
                for (const auto& cached : native.native_x64_code_cache) {
                    const auto block =
                        std::static_pointer_cast<VMNativeX64CodeBlock>(
                            cached.second);
                    for (const VMNativeX64Instruction& instruction :
                         block->lowered) {
                        saw_mul_direct = saw_mul_direct ||
                            (instruction.op == VMMicroOpcode::Mul &&
                             nativeX64InstructionIsDirect(
                                 instruction, block->lowered.size()));
                    }
                }
                expect(saw_mul_direct,
                       label + " cache classifies MUL as direct-capable");
                if (expect_direct) {
                    expect(native.native_x64_jit_stats.portable_side_exits == 0,
                           label + " exact integral MUL stays direct");
                    expect(native.native_x64_jit_stats.direct_instructions > 0,
                           label + " exact integral MUL counts direct commit");
                } else {
                    expect(native.native_x64_jit_stats.portable_side_exits > 0,
                           label + " guarded MUL side-exits to portable path");
                }
            }
        };

        const long long max_long = std::numeric_limits<long long>::max();
        const long long min_long = std::numeric_limits<long long>::min();
        compareMul("MUL zero", sandbox::vm::ops::fromLong(0),
                   sandbox::vm::ops::fromLong(-17), true);
        compareMul("MUL signed small", sandbox::vm::ops::fromLong(-27),
                   sandbox::vm::ops::fromLong(14), true);
        compareMul("MUL int64 max by one", TernaryValue::fromTriple(
                       native_ops::fromIntT40(max_long)),
                   TernaryValue::fromTriple(native_ops::fromIntT40(1)), true);
        compareMul("MUL int64 min by one", TernaryValue::fromTriple(
                       native_ops::fromIntT40(min_long)),
                   TernaryValue::fromTriple(native_ops::fromIntT40(1)), true);
        compareMul("MUL signed overflow", TernaryValue::fromTriple(
                       native_ops::fromIntT40(max_long)),
                   TernaryValue::fromTriple(native_ops::fromIntT40(2)), false);
        compareMul("MUL min times negative one", TernaryValue::fromTriple(
                       native_ops::fromIntT40(min_long)),
                   TernaryValue::fromTriple(native_ops::fromIntT40(-1)), true);
        compareMul("MUL min times negative two", TernaryValue::fromTriple(
                       native_ops::fromIntT40(min_long)),
                   TernaryValue::fromTriple(native_ops::fromIntT40(-2)), false);
        compareMul("MUL fractional fallback", TernaryValue::fromTriple(
                       native_ops::divide(native_ops::fromIntT40(1),
                                          native_ops::fromIntT40(2))),
                   TernaryValue::fromTriple(native_ops::fromIntT40(7)), false);
        compareMul("MUL special fallback", TernaryValue::fromTriple(
                       Triple::Overflow),
                   TernaryValue::fromTriple(native_ops::fromIntT40(1)), false);
        compareMul("MUL invalid fallback", TernaryValue::invalid(
                       TernaryMode::T40),
                   TernaryValue::fromTriple(native_ops::fromIntT40(1)), false);
    }

    {
        // A direct T40 write must side-exit before invalidating a live T50/L50
        // pair, matching TernaryRegisterFile::write for every direct scalar
        // arithmetic opcode (including the newly lowered NEG/ABS/MUL).
        const std::vector<std::string> wide_ops = {
            "add r1, r3, r4", "sub r1, r3, r4", "mul r1, r3, r4",
            "neg r1, r3", "abs r1, r3", "tcmp r1, r3, r4",
        };
        for (const std::string& operation : wide_ops) {
            VMState interpreter(32, 32);
            VMState native(32, 32);
            const auto program = assembleOrThrow(operation + "\nhalt\n");
            expect(loadAndReset(interpreter, program) &&
                       loadAndReset(native, program),
                   operation + " wide-pair program loads");
            for (VMState* vm : {&interpreter, &native}) {
                vm->regfile.write(R1, TernaryValue::fromLongTriple(
                    native_ops::fromInt(7)));
                vm->regfile.write(R3, TernaryValue::fromTriple(
                    native_ops::fromIntT40(-6)));
                vm->regfile.write(R4, TernaryValue::fromTriple(
                    native_ops::fromIntT40(2)));
            }
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto interpreter_result = sandbox::vm::run(interpreter, 8);
            const auto native_result = sandbox::vm::run(native, 8);
            expect(native_result.status == interpreter_result.status &&
                       native_result.steps == interpreter_result.steps &&
                       native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count,
                   operation + " wide-pair status/PC parity");
            for (const std::uint8_t reg : {R1, R2, R3, R4}) {
                expect(native.regfile.reg[reg] == interpreter.regfile.reg[reg] &&
                           native.regfile.view_mode[reg] ==
                               interpreter.regfile.view_mode[reg],
                       operation + " wide-pair register parity");
            }
            if (nativeX64HostAvailable()) {
                expect(native.native_x64_jit_stats.portable_side_exits > 0,
                       operation + " wide-pair side-exits before mutation");
            }
        }
    }

    {
        // Native blocks are tied to IMEM generation just like decoded traces:
        // an instruction write must retire the old RX allocation before the
        // next native dispatch can compile a replacement.
        VMState invalidation(64, 64);
        const auto program = assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
        loop:
            add r1, r1, r2
            jmp loop
        )");
        expect(loadAndReset(invalidation, program),
               "native invalidation fixture loads");
        invalidation.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        invalidation.setDecodedTraceHotThreshold(1);
        const auto warm_result = sandbox::vm::run(invalidation, 12);
        expect(warm_result.timeout(),
               "native invalidation fixture reaches the step limit");
        if (nativeX64HostAvailable()) {
            expect(!invalidation.native_x64_code_cache.empty() &&
                       invalidation.native_x64_jit_stats.blocks_built > 0,
                   "native invalidation fixture builds an RX block");
            const auto old_generation = invalidation.imem.generation();
            const auto old_invalidations =
                invalidation.native_x64_jit_stats.invalidations;
            expect(invalidation.imem.write(0, program[0]) == MemFaultCode::OK &&
                       invalidation.imem.generation() != old_generation,
                   "native invalidation write bumps IMEM generation");
            invalidation.pc = 0;
            invalidation.status = VMStatus::RUNNING;
            const auto rerun = sandbox::vm::run(invalidation, 1);
            expect(rerun.timeout() && rerun.steps == 1,
                   "native invalidation rerun dispatches one instruction");
            expect(invalidation.native_x64_jit_stats.invalidations >
                       old_invalidations,
                   "native invalidation retires stale code cache entries");
        }
    }

    {
        // Invalid and non-integral T40 payloads must take a precise guard
        // exit, then let the portable path own the architectural trap/result.
        const auto program = assembleOrThrow(R"(
            add r3, r1, r2
            halt
        )");
        auto compareGuardExit = [&](const std::string& label,
                                     const TernaryValue& lhs,
                                     const TernaryValue& rhs) {
            VMState interpreter(32, 32);
            VMState native(32, 32);
            expect(loadAndReset(interpreter, program) &&
                       loadAndReset(native, program),
                   label + " programs load");
            interpreter.regfile.write(R1, lhs);
            interpreter.regfile.write(R2, rhs);
            native.regfile.write(R1, lhs);
            native.regfile.write(R2, rhs);
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto interpreter_result =
                sandbox::vm::run(interpreter, 16);
            const auto native_result = sandbox::vm::run(native, 16);
            expect(native_result.status == interpreter_result.status &&
                       native_result.trap_code == interpreter_result.trap_code &&
                       native_result.steps == interpreter_result.steps,
                   label + " guard preserves status and steps");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count,
                   label + " guard preserves PC and cycle");
            if (nativeX64HostAvailable()) {
                expect(native.native_x64_jit_stats.portable_side_exits > 0,
                       label + " records precise native guard exit");
                expect(native.native_x64_jit_stats.direct_instructions == 0,
                       label + " does not count the guarded instruction committed");
            }
        };
        compareGuardExit(
            "invalid T40 scalar guard",
            TernaryValue::invalid(TernaryMode::T40),
            TernaryValue::fromTriple(native_ops::fromIntT40(1)));
        compareGuardExit(
            "fractional T40 scalar guard",
            TernaryValue::fromTriple(native_ops::divide(
                native_ops::fromIntT40(1), native_ops::fromIntT40(2))),
            TernaryValue::fromTriple(native_ops::fromIntT40(1)));
    }

    {
        // Exercise all three direct branch predicates with zero and ordinary
        // signed T40 values, plus an invalid guard-failure case.
        struct BranchCase {
            const char* mnemonic;
            long long value;
            bool taken;
        };
        const std::vector<BranchCase> branches = {
            {"brn", -1, true}, {"brn", 0, false}, {"brn", 1, false},
            {"brz", -1, false}, {"brz", 0, true}, {"brz", 1, false},
            {"brp", -1, false}, {"brp", 0, false}, {"brp", 1, true},
        };
        for (const BranchCase branch : branches) {
            const auto program = assembleOrThrow(
                std::string{ "loop:\n    nop\n    " } +
                branch.mnemonic + R"( r1, loop
                    halt
                )");
            VMState interpreter(32, 32);
            VMState native(32, 32);
            expect(loadAndReset(interpreter, program) &&
                       loadAndReset(native, program),
                   "native branch predicate programs load");
            const TernaryValue value = TernaryValue::fromTriple(
                native_ops::fromIntT40(branch.value));
            interpreter.regfile.write(R1, value);
            native.regfile.write(R1, value);
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto interpreter_result =
                sandbox::vm::run(interpreter, 8);
            const auto native_result = sandbox::vm::run(native, 8);
            expect(native_result.status == interpreter_result.status &&
                       native_result.steps == interpreter_result.steps,
                   "native branch status and steps parity");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count &&
                       native.branch_instructions_count ==
                           interpreter.branch_instructions_count,
                   "native branch PC/cycle/count parity");
            expect((native_result.status == VMStatus::RUNNING) ==
                       branch.taken,
                   "native branch taken outcome parity");
            if (nativeX64HostAvailable()) {
                bool saw_direct_branch = false;
                for (const auto& cached : native.native_x64_code_cache) {
                    const auto block =
                        std::static_pointer_cast<VMNativeX64CodeBlock>(
                            cached.second);
                    for (const VMNativeX64Instruction& instruction :
                         block->lowered) {
                        saw_direct_branch = saw_direct_branch ||
                            ((instruction.op == VMMicroOpcode::Brn ||
                              instruction.op == VMMicroOpcode::Brz ||
                              instruction.op == VMMicroOpcode::Brp) &&
                             nativeX64InstructionIsDirect(
                                 instruction, block->lowered.size()));
                    }
                }
                expect(saw_direct_branch,
                       "native branch predicate is emitted directly");
                expect(native.native_x64_jit_stats.portable_side_exits == 0,
                       "valid branch predicates stay on native path");
            }
        }

        const auto invalid_program = assembleOrThrow(R"(
        loop:
            nop
            brp r1, loop
            halt
        )");
        VMState interpreter(32, 32);
        VMState native(32, 32);
        expect(loadAndReset(interpreter, invalid_program) &&
                   loadAndReset(native, invalid_program),
               "native invalid branch programs load");
        const TernaryValue invalid = TernaryValue::invalid(TernaryMode::T40);
        interpreter.regfile.write(R1, invalid);
        native.regfile.write(R1, invalid);
        interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
        native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        native.setDecodedTraceHotThreshold(1);
        const auto interpreter_result = sandbox::vm::run(interpreter, 8);
        const auto native_result = sandbox::vm::run(native, 8);
        expect(native_result.status == interpreter_result.status &&
                   native_result.steps == interpreter_result.steps &&
                   native.pc == interpreter.pc &&
                   native.cycle_count == interpreter.cycle_count,
               "invalid branch guard preserves precise state");
        if (nativeX64HostAvailable()) {
            expect(native.native_x64_jit_stats.portable_side_exits > 0,
                   "invalid branch records precise guard exit");
        }
    }

    {
        // Direct arithmetic guards and guarded dense-memory lowerings both
        // side-exit before an unsafe commit.  The portable interpreter owns
        // the trap so PC, cycle, and step accounting stay identical to the
        // oracle.
        auto compareNativeGuardExit = [&](const std::string& label,
                                           const std::vector<TritWord27>& program,
                                           const std::function<void(VMState&)>& setup,
                                           bool expect_helper) {
            VMState interpreter(32, 16);
            VMState native(32, 16);
            expect(loadAndReset(interpreter, program) &&
                       loadAndReset(native, program),
                   label + " programs load");
            setup(interpreter);
            setup(native);
            interpreter.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);

            const auto interpreter_result = sandbox::vm::run(interpreter, 16);
            const auto native_result = sandbox::vm::run(native, 16);
            if (!nativeX64HostAvailable()) {
                expect(native.native_x64_jit_stats.blocks_built == 0,
                       label + " skips native code on non-x86 host");
                return;
            }
            expect(native_result.status == interpreter_result.status &&
                       native_result.trap_code == interpreter_result.trap_code &&
                       native_result.steps == interpreter_result.steps,
                   label + " preserves trap and instruction count");
            expect(native.pc == interpreter.pc &&
                       native.cycle_count == interpreter.cycle_count,
                   label + " preserves fault PC and cycle count");
            expect(native.native_x64_jit_stats.portable_side_exits > 0,
                   label + " records helper side exit");
            bool saw_helper = false;
            for (const auto& cached : native.native_x64_code_cache) {
                const auto block =
                    std::static_pointer_cast<VMNativeX64CodeBlock>(
                        cached.second);
                saw_helper = saw_helper || block->helper_instruction_count > 0;
            }
            expect(saw_helper == expect_helper,
                   label + " cache identifies the expected lowering");
            expect(native.native_x64_jit_stats.direct_instructions == 0,
                   label + " does not miscount helper instruction as direct");
        };

        compareNativeGuardExit(
            "native arithmetic helper trap",
            assembleOrThrow(R"(
                add r3, r1, r2
                halt
            )"),
            [](VMState& vm) {
                vm.regfile.write(R1, TernaryValue::invalid(TernaryMode::T40));
            },
            false);
        compareNativeGuardExit(
            "native memory guard trap",
            assembleOrThrow(R"(
                load r2, r1, 0
                halt
            )"),
            [](VMState& vm) {
                vm.regfile.write(R1, sandbox::vm::ops::fromLong(99));
            },
            false);
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
        // Scalar pointers are tagged values, not unchecked host integers.
        // Keep a fixed-seed corpus of invalid T40 payloads, representable
        // out-of-range addresses, and ordinary controls in lock-step across
        // the portable, decoded, and native fallback paths. A fault must
        // leave the destination and memory untouched at the faulting PC.
        struct PointerCase {
            std::string label;
            TernaryValue base;
            int immediate = 0;
            bool valid = false;
            long long expected_value = 0;
            TrapCode trap = TrapCode::TRAP_MEM_FAULT;
        };

        std::vector<PointerCase> cases;
        cases.push_back({"valid", sandbox::vm::ops::fromLong(4), 0, true, 4242,
                         TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"valid-negative-immediate",
                         sandbox::vm::ops::fromLong(5), -1, true, 4242,
                         TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"fractional-truncate",
                         TernaryValue::fromTriple(native_ops::divide(
                             native_ops::fromIntT40(1),
                             native_ops::fromIntT40(2))),
                         0, true, 111, TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"tagged-t5",
                         TernaryValue::fromT5(native_ops::fromIntT5(4)),
                         0, true, 4242, TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"invalid-t40", TernaryValue::invalid(TernaryMode::T40),
                         0, false, 0, TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"invalid-l40", TernaryValue::invalid(TernaryMode::L40),
                         0, false, 0, TrapCode::TRAP_ILLEGAL_OP});
        cases.push_back({"negative-oob", sandbox::vm::ops::fromLong(-1), 0, false, 0,
                         TrapCode::TRAP_MEM_FAULT});
        cases.push_back({"positive-oob", sandbox::vm::ops::fromLong(64), 0, false, 0,
                         TrapCode::TRAP_MEM_FAULT});

        std::uint32_t fuzz_seed = 0x5eed1234u;
        for (int sample = 0; sample < 36; ++sample) {
            fuzz_seed = fuzz_seed * 1664525u + 1013904223u;
            const bool high = (fuzz_seed & 1u) != 0;
            const long long magnitude =
                2147483648LL + static_cast<long long>(fuzz_seed % 1000000u);
            fuzz_seed = fuzz_seed * 1664525u + 1013904223u;
            const int immediate = high
                ? static_cast<int>(fuzz_seed % 97u) + 1
                : -static_cast<int>(fuzz_seed % 97u) - 1;
            cases.push_back({
                "seeded-" + std::to_string(sample),
                sandbox::vm::ops::fromLong(high ? magnitude : -magnitude), immediate, false,
                0, TrapCode::TRAP_MEM_FAULT});
        }

        struct PointerSnapshot {
            RunResult result;
            TernaryValue destination;
            long long memory_zero = 0;
            long long memory_four = 0;
            int cause = 0;
            int page_fault_addr = 0;
            int page_fault_access = 0;
        };

        auto runPointerCase = [&](const PointerCase& pointer,
                                  bool store,
                                  VMExecutionBackend backend) {
            const std::string source = store
                ? "mov r2, 321\nstore r2, r1, " +
                      std::to_string(pointer.immediate) + "\nhalt\n"
                : "load r2, r1, " + std::to_string(pointer.immediate) +
                      "\nhalt\n";
            VMState vm(32, 64);
            const auto program = assembleV2TestOrThrow(source);
            expect(loadAndReset(vm, program),
                   pointer.label + (store ? " store" : " load") +
                       " pointer program loads");
            vm.setExecutionBackend(backend);
            if (backend != VMExecutionBackend::Interpreter) {
                vm.setTraceJitHotThreshold(1);
            }
            vm.regfile.write(R1, pointer.base);
            vm.regfile.write(R2, sandbox::vm::ops::fromLong(321));
            expect(vm.dmem.store(0, sandbox::vm::ops::fromLong(111)) == MemFaultCode::OK,
                   pointer.label + " seeds zero sentinel");
            expect(vm.dmem.store(4, sandbox::vm::ops::fromLong(4242)) == MemFaultCode::OK,
                   pointer.label + " seeds valid sentinel");
            PointerSnapshot snapshot;
            snapshot.result = sandbox::vm::run(vm, 8);
            snapshot.destination = vm.regfile.read(R2);
            snapshot.memory_zero = loadPhysLong(vm, 0);
            snapshot.memory_four = loadPhysLong(vm, 4);
            snapshot.cause = vm.cause;
            snapshot.page_fault_addr = vm.page_fault_addr;
            snapshot.page_fault_access = vm.page_fault_access;
            return snapshot;
        };

        for (const PointerCase& pointer : cases) {
            for (const bool store : {false, true}) {
                const std::string label = pointer.label +
                    (store ? " STORE" : " LOAD");
                const PointerSnapshot oracle =
                    runPointerCase(pointer, store, VMExecutionBackend::Interpreter);
                const PointerSnapshot decoded =
                    runPointerCase(pointer, store,
                                   VMExecutionBackend::DecodedTraceExecutor);
                const PointerSnapshot native =
                    runPointerCase(pointer, store,
                                   VMExecutionBackend::NativeX64Jit);

                const bool expected_trap = !pointer.valid;
                expect(oracle.result.trapped() == expected_trap,
                       label + " interpreter trap classification");
                if (pointer.valid) {
                    expect(oracle.result.halted() &&
                               sandbox::vm::ops::toLong(oracle.destination) ==
                                   (store ? 321 : pointer.expected_value),
                           label + " interpreter valid result");
                } else {
                    expect(oracle.result.trap_code == pointer.trap,
                           label + " interpreter trap code");
                    expect(oracle.destination == sandbox::vm::ops::fromLong(321),
                           label + " interpreter leaves destination unchanged");
                    expect(oracle.memory_zero == 111 && oracle.memory_four == 4242,
                           label + " interpreter leaves sentinels unchanged");
                }

                for (const auto& candidate : {decoded, native}) {
                    expect(candidate.result.status == oracle.result.status &&
                               candidate.result.trap_code == oracle.result.trap_code &&
                               candidate.result.steps == oracle.result.steps,
                           label + " backend status/trap/steps parity");
                    expect(candidate.result.final_pc == oracle.result.final_pc,
                           label + " backend final PC parity");
                    expect(candidate.destination == oracle.destination &&
                               candidate.memory_zero == oracle.memory_zero &&
                               candidate.memory_four == oracle.memory_four,
                           label + " backend state/memory parity");
                    expect(candidate.cause == oracle.cause &&
                               candidate.page_fault_addr == oracle.page_fault_addr &&
                               candidate.page_fault_access == oracle.page_fault_access,
                           label + " backend fault metadata parity");
                }
            }
        }
    }

    {
        // Dense identity-mapped T40 memory is emitted directly on x86-64.
        // Compare both payloads and TernaryMemory page generations with the
        // interpreter, then exercise the same block across a fixed-seed
        // boundary corpus of valid addresses/values.
        const auto program = assembleOrThrow(R"(
            store r2, r1, 0
            load r3, r1, 0
            halt
        )");
        std::vector<std::pair<int, long long>> cases = {
            {0, 0}, {1, 1}, {27, -27}, {28, 42}, {63, -63},
            {728, 17}, {729, -18}, {1457, 19},
        };
        std::uint32_t seed = 0xD1CECAFEu;
        for (int sample = 0; sample < 27; ++sample) {
            seed = seed * 1664525u + 1013904223u;
            const int address = static_cast<int>(seed % 64u);
            seed = seed * 1664525u + 1013904223u;
            const long long value = static_cast<long long>(seed % 101u) - 50;
            cases.push_back({address, value});
        }
        for (const auto [address, value] : cases) {
            VMState oracle(32, 1458);
            VMState native(32, 1458);
            expect(loadAndReset(oracle, program) &&
                       loadAndReset(native, program),
                   "native direct memory differential programs load");
            oracle.regfile.write(R1, sandbox::vm::ops::fromLong(address));
            native.regfile.write(R1, sandbox::vm::ops::fromLong(address));
            oracle.regfile.write(R2, sandbox::vm::ops::fromLong(value));
            native.regfile.write(R2, sandbox::vm::ops::fromLong(value));
            expect(oracle.dmem.store(0, sandbox::vm::ops::fromLong(7)) ==
                       MemFaultCode::OK &&
                       native.dmem.store(0, sandbox::vm::ops::fromLong(7)) ==
                       MemFaultCode::OK,
                   "native direct memory seeds sentinel");
            oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
            native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
            native.setDecodedTraceHotThreshold(1);
            const auto oracle_result = sandbox::vm::run(oracle, 16);
            const auto native_result = sandbox::vm::run(native, 16);
            expect(native_result.status == oracle_result.status &&
                       native_result.steps == oracle_result.steps &&
                       native.pc == oracle.pc &&
                       native.cycle_count == oracle.cycle_count,
                   "native direct memory status/PC/cycle parity");
            expect(native.regfile.read(R3) == oracle.regfile.read(R3) &&
                       loadPhysLong(native, 0) == loadPhysLong(oracle, 0),
                   "native direct memory payload parity");
            expect(native.dmem.generation() == oracle.dmem.generation() &&
                       native.dmem.pageGenerationForAddress(0) ==
                           oracle.dmem.pageGenerationForAddress(0) &&
                       native.dmem.pageGenerationForAddress(address) ==
                           oracle.dmem.pageGenerationForAddress(address),
                   "native direct store preserves DMEM generations");
            if (nativeX64HostAvailable()) {
                bool saw_load = false;
                bool saw_store = false;
                bool saw_helper = false;
                bool all_wx = !native.native_x64_code_cache.empty();
                for (const auto& cached : native.native_x64_code_cache) {
                    const auto block =
                        std::static_pointer_cast<VMNativeX64CodeBlock>(
                            cached.second);
                    all_wx = all_wx && block->isWriteXorExecute();
                    saw_helper = saw_helper || block->helper_instruction_count > 0;
                    for (const auto& instruction : block->lowered) {
                        const bool direct = nativeX64InstructionIsDirect(
                            instruction, block->lowered.size());
                        saw_load = saw_load ||
                            (instruction.op == VMMicroOpcode::Load && direct);
                        saw_store = saw_store ||
                            (instruction.op == VMMicroOpcode::Store && direct);
                    }
                }
                expect(saw_load && saw_store,
                       "native direct memory cache marks LOAD/STORE direct");
                expect(!saw_helper,
                       "native direct memory fixture has no helper lowering");
                expect(all_wx,
                       "native direct memory blocks remain write-xor-execute");
                expect(native.native_x64_jit_stats.portable_side_exits == 0,
                       "native direct memory valid cases stay inline");
            }
        }
    }

    {
        // Sparse backing is deliberately outside the raw-pointer lowering;
        // the same trace must side-exit before touching a null dense pointer,
        // then let the portable path preserve sparse page materialization.
        const auto program = assembleOrThrow(R"(
            store r2, r1, 0
            load r3, r1, 0
            halt
        )");
        const int sparse_words = SPARSE_MEMORY_DENSE_LIMIT_WORDS + 1;
        VMState oracle(32, sparse_words);
        VMState native(32, sparse_words);
        expect(loadAndReset(oracle, program) &&
                   loadAndReset(native, program),
               "native sparse-memory programs load");
        for (VMState* vm : {&oracle, &native}) {
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(729));
            vm->regfile.write(R2, sandbox::vm::ops::fromLong(37));
        }
        expect(oracle.dmem.isSparse() && native.dmem.isSparse(),
               "native sparse-memory fixture uses sparse backing");
        oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        native.setDecodedTraceHotThreshold(1);
        const auto oracle_result = sandbox::vm::run(oracle, 8);
        const auto native_result = sandbox::vm::run(native, 8);
        expect(native_result.status == oracle_result.status &&
                   native_result.steps == oracle_result.steps &&
                   native.regfile.read(R3) == oracle.regfile.read(R3) &&
                   native.dmem.allocatedPages() == oracle.dmem.allocatedPages(),
               "native sparse-memory side-exit parity");
        if (nativeX64HostAvailable()) {
            expect(native.native_x64_jit_stats.portable_side_exits > 0,
                   "native sparse memory explicitly side-exits");
        }
    }

    {
        // Non-kernel identity mappings use the same dense inline lowering,
        // while MMU-enabled state is explicitly side-exited before memory
        // access.  The latter still resumes through the exact interpreter
        // translation/trap path.
        const auto program = assembleOrThrow(R"(
            store r2, r1, 0
            load r3, r1, 0
            halt
        )");

        VMState user_oracle(32, 64);
        VMState user_native(32, 64);
        expect(loadAndReset(user_oracle, program) &&
                   loadAndReset(user_native, program),
               "native user-memory programs load");
        for (VMState* vm : {&user_oracle, &user_native}) {
            vm->privilege = PrivilegeMode::User;
            vm->mmu_enable = false;
            vm->user_imem_base = 0;
            vm->user_imem_limit = static_cast<int>(program.size());
            vm->user_dmem_base = 4;
            vm->user_dmem_limit = 16;
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(8));
            vm->regfile.write(R2, sandbox::vm::ops::fromLong(17));
        }
        user_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        user_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        user_native.setDecodedTraceHotThreshold(1);
        const auto user_oracle_result = sandbox::vm::run(user_oracle, 16);
        const auto user_native_result = sandbox::vm::run(user_native, 16);
        expect(user_native_result.status == user_oracle_result.status &&
                   user_native_result.steps == user_oracle_result.steps &&
                   user_native.regfile.read(R3) == user_oracle.regfile.read(R3),
               "native user identity memory parity");
        if (nativeX64HostAvailable()) {
            expect(user_native.native_x64_jit_stats.portable_side_exits == 0,
                   "native user identity memory stays inline");
        }

        VMState mmu_oracle(32, 64);
        VMState mmu_native(32, 64);
        expect(loadAndReset(mmu_oracle, program) &&
                   loadAndReset(mmu_native, program),
               "native MMU-memory programs load");
        for (VMState* vm : {&mmu_oracle, &mmu_native}) {
            vm->privilege = PrivilegeMode::Kernel;
            vm->mmu_enable = true;
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(8));
            vm->regfile.write(R2, sandbox::vm::ops::fromLong(17));
        }
        mmu_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        mmu_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        mmu_native.setDecodedTraceHotThreshold(1);
        const auto mmu_oracle_result = sandbox::vm::run(mmu_oracle, 16);
        const auto mmu_native_result = sandbox::vm::run(mmu_native, 16);
        expect(mmu_native_result.status == mmu_oracle_result.status &&
                   mmu_native_result.steps == mmu_oracle_result.steps &&
                   mmu_native.regfile.read(R3) == mmu_oracle.regfile.read(R3),
               "native MMU-memory side-exit parity");
        if (nativeX64HostAvailable()) {
            expect(mmu_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native MMU memory explicitly side-exits");
        }

        VMState tagged_oracle(32, 64);
        VMState tagged_native(32, 64);
        expect(loadAndReset(tagged_oracle, program) &&
                   loadAndReset(tagged_native, program),
               "native tagged-source memory programs load");
        for (VMState* vm : {&tagged_oracle, &tagged_native}) {
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(8));
            // Bypass RegFile::write deliberately: this models a hostile
            // physical register payload whose tag is not canonical T40.
            vm->regfile.reg[R2] = TernaryValue::fromT5(
                native_ops::fromIntT5(23));
            vm->regfile.view_mode[R2] = TernaryMode::T5;
            expect(vm->dmem.store(8, sandbox::vm::ops::fromLong(7)) ==
                       MemFaultCode::OK,
                   "native tagged-source memory seeds value");
        }
        tagged_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        tagged_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        tagged_native.setDecodedTraceHotThreshold(1);
        const auto tagged_oracle_result = sandbox::vm::run(tagged_oracle, 8);
        const auto tagged_native_result = sandbox::vm::run(tagged_native, 8);
        expect(tagged_native_result.status == tagged_oracle_result.status &&
                   tagged_native_result.steps == tagged_oracle_result.steps &&
                   tagged_native.dmem.load(8).first ==
                       tagged_oracle.dmem.load(8).first &&
                   tagged_native.dmem.generation() ==
                       tagged_oracle.dmem.generation() &&
                   tagged_native.dmem.pageGenerationForAddress(8) ==
                       tagged_oracle.dmem.pageGenerationForAddress(8),
               "native tagged-source store preserves portable payload");
        if (nativeX64HostAvailable()) {
            expect(tagged_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native tagged-source store side-exits before mode rewrite");
        }

        const auto tagged_address_program = assembleOrThrow(R"(
            load r3, r1, 0
            halt
        )");
        VMState tagged_address_oracle(32, 64);
        VMState tagged_address_native(32, 64);
        expect(loadAndReset(tagged_address_oracle, tagged_address_program) &&
                   loadAndReset(tagged_address_native, tagged_address_program),
               "native tagged-physical-address programs load");
        for (VMState* vm : {&tagged_address_oracle, &tagged_address_native}) {
            // Keep the view tag T40 while making the physical payload hostile;
            // read() therefore returns the T5 payload and the portable path
            // interprets it with T5 semantics.
            vm->regfile.reg[R1] = TernaryValue::fromT5(
                native_ops::fromIntT5(4));
            vm->regfile.view_mode[R1] = TernaryMode::T40;
            expect(vm->dmem.store(4, sandbox::vm::ops::fromLong(31)) ==
                       MemFaultCode::OK,
                   "native tagged-physical-address seeds value");
        }
        tagged_address_oracle.setExecutionBackend(
            VMExecutionBackend::Interpreter);
        tagged_address_native.setExecutionBackend(
            VMExecutionBackend::NativeX64Jit);
        tagged_address_native.setDecodedTraceHotThreshold(1);
        const auto tagged_address_oracle_result = sandbox::vm::run(
            tagged_address_oracle, 8);
        const auto tagged_address_native_result = sandbox::vm::run(
            tagged_address_native, 8);
        expect(tagged_address_native_result.status ==
                   tagged_address_oracle_result.status &&
                   tagged_address_native_result.steps ==
                       tagged_address_oracle_result.steps &&
                   tagged_address_native.regfile.read(R3) ==
                       tagged_address_oracle.regfile.read(R3),
               "native tagged physical address preserves fallback semantics");
        if (nativeX64HostAvailable()) {
            expect(tagged_address_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native tagged physical address side-exits before decode");
        }

        const auto tagged_memory_program = assembleOrThrow(R"(
            load r3, r1, 0
            halt
        )");
        VMState tagged_memory_oracle(32, 64);
        VMState tagged_memory_native(32, 64);
        expect(loadAndReset(tagged_memory_oracle, tagged_memory_program) &&
                   loadAndReset(tagged_memory_native, tagged_memory_program),
               "native tagged-memory programs load");
        for (VMState* vm : {&tagged_memory_oracle, &tagged_memory_native}) {
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(4));
            expect(vm->dmem.store(4, TernaryValue::fromT5(
                       native_ops::fromIntT5(13))) == MemFaultCode::OK,
                   "native tagged-memory seeds value");
        }
        tagged_memory_oracle.setExecutionBackend(
            VMExecutionBackend::Interpreter);
        tagged_memory_native.setExecutionBackend(
            VMExecutionBackend::NativeX64Jit);
        tagged_memory_native.setDecodedTraceHotThreshold(1);
        const auto tagged_memory_oracle_result = sandbox::vm::run(
            tagged_memory_oracle, 8);
        const auto tagged_memory_native_result = sandbox::vm::run(
            tagged_memory_native, 8);
        expect(tagged_memory_native_result.status ==
                   tagged_memory_oracle_result.status &&
                   tagged_memory_native_result.steps ==
                       tagged_memory_oracle_result.steps &&
                   tagged_memory_native.regfile.read(R3) ==
                       tagged_memory_oracle.regfile.read(R3),
               "native tagged-memory load preserves fallback semantics");
        if (nativeX64HostAvailable()) {
            expect(tagged_memory_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native tagged-memory load side-exits before rewrite");
        }

        VMState bad_limit_oracle(32, 64);
        VMState bad_limit_native(32, 64);
        expect(loadAndReset(bad_limit_oracle, program) &&
                   loadAndReset(bad_limit_native, program),
               "native signed user-bound memory programs load");
        for (VMState* vm : {&bad_limit_oracle, &bad_limit_native}) {
            vm->privilege = PrivilegeMode::User;
            vm->mmu_enable = false;
            vm->user_imem_base = 0;
            vm->user_imem_limit = static_cast<int>(program.size());
            vm->user_dmem_base = 0;
            vm->user_dmem_limit = -1;
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(8));
            vm->regfile.write(R2, sandbox::vm::ops::fromLong(29));
            expect(vm->dmem.store(8, sandbox::vm::ops::fromLong(7)) ==
                       MemFaultCode::OK,
                   "native signed user-bound memory seeds value");
        }
        bad_limit_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        bad_limit_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        bad_limit_native.setDecodedTraceHotThreshold(1);
        const auto bad_limit_oracle_result = sandbox::vm::run(
            bad_limit_oracle, 8);
        const auto bad_limit_native_result = sandbox::vm::run(
            bad_limit_native, 8);
        expect(bad_limit_native_result.status ==
                   bad_limit_oracle_result.status &&
                   bad_limit_native_result.trap_code ==
                       bad_limit_oracle_result.trap_code &&
                   bad_limit_native_result.steps ==
                       bad_limit_oracle_result.steps &&
                   bad_limit_native.dmem.load(8).first ==
                       bad_limit_oracle.dmem.load(8).first,
               "native negative user limit preserves memory fault parity");
        if (nativeX64HostAvailable()) {
            expect(bad_limit_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native negative user limit side-exits before store");
        }

        VMState reservation_oracle(32, 64);
        VMState reservation_native(32, 64);
        expect(loadAndReset(reservation_oracle, program) &&
                   loadAndReset(reservation_native, program),
               "native reservation-memory programs load");
        for (VMState* vm : {&reservation_oracle, &reservation_native}) {
            vm->regfile.write(R1, sandbox::vm::ops::fromLong(8));
            vm->regfile.write(R2, sandbox::vm::ops::fromLong(23));
            vm->atomic_reservation_valid = true;
            vm->atomic_reservation_addr = 8;
        }
        reservation_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        reservation_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        reservation_native.setDecodedTraceHotThreshold(1);
        const auto reservation_oracle_result =
            sandbox::vm::run(reservation_oracle, 16);
        const auto reservation_native_result =
            sandbox::vm::run(reservation_native, 16);
        expect(reservation_native_result.status ==
                   reservation_oracle_result.status &&
                   reservation_native_result.steps ==
                   reservation_oracle_result.steps &&
                   !reservation_native.atomic_reservation_valid &&
                   !reservation_oracle.atomic_reservation_valid,
               "native reservation store preserves invalidation semantics");
        if (nativeX64HostAvailable()) {
            expect(reservation_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native reservation store side-exits before mutation");
        }

        const auto wide_program = assembleOrThrow(R"(
            load r1, r3, 0
            halt
        )");
        VMState wide_oracle(32, 64);
        VMState wide_native(32, 64);
        expect(loadAndReset(wide_oracle, wide_program) &&
                   loadAndReset(wide_native, wide_program),
               "native wide-destination memory programs load");
        for (VMState* vm : {&wide_oracle, &wide_native}) {
            vm->regfile.write(R3, sandbox::vm::ops::fromLong(0));
            vm->regfile.write(R1, TernaryValue::fromLongTriple(
                native_ops::fromInt(7)));
            expect(vm->dmem.store(0, sandbox::vm::ops::fromLong(19)) ==
                       MemFaultCode::OK,
                   "native wide-destination memory seeds value");
        }
        wide_oracle.setExecutionBackend(VMExecutionBackend::Interpreter);
        wide_native.setExecutionBackend(VMExecutionBackend::NativeX64Jit);
        wide_native.setDecodedTraceHotThreshold(1);
        const auto wide_oracle_result = sandbox::vm::run(wide_oracle, 8);
        const auto wide_native_result = sandbox::vm::run(wide_native, 8);
        expect(wide_native_result.status == wide_oracle_result.status &&
                   wide_native.regfile.reg[R1] == wide_oracle.regfile.reg[R1] &&
                   wide_native.regfile.reg[R2] == wide_oracle.regfile.reg[R2] &&
                   wide_native.regfile.view_mode[R1] ==
                       wide_oracle.regfile.view_mode[R1] &&
                   wide_native.regfile.view_mode[R2] ==
                       wide_oracle.regfile.view_mode[R2],
               "native wide destination preserves pair write semantics");
        if (nativeX64HostAvailable()) {
            expect(wide_native.native_x64_jit_stats.portable_side_exits > 0,
                   "native wide destination side-exits before pair mutation");
        }
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
