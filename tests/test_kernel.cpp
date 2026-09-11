#include "test_multiwidth_vm_common.h"

void testPhase35Infrastructure() {
    std::cout << "[8] Phase 3.5 VM hooks, data sections, and run limits\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;
    const auto assembleOrThrow = [](const std::string& source) {
        return assembleV2TestOrThrow(source);
    };

    {
        auto assembled = assemble(R"(
            .data
        value: .word 42
        next:  .word -7, value
            .text
        start:
            mov  r1, value
            load r2, r1
            load r3, zero, next
            mov  r4, next
            load r5, zero, 2
            halt
        )");
        expect(assembled.success, "assembler accepts .data/.text with .word");
        if (assembled.success) {
            expect(assembled.labels.count("start") && assembled.labels.at("start") == 0,
                   "text label maps to IMEM address");
            expect(assembled.data_labels.count("value") && assembled.data_labels.at("value") == 0,
                   "data label maps to first DMEM address");
            expect(assembled.data_labels.count("next") && assembled.data_labels.at("next") == 1,
                   "data label maps to second DMEM address");
            expect(assembled.data.size() == 3, ".word emits each data word");
            expect(sandbox::vm::ops::toLong(assembled.data[0]) == 42, ".word stores numeric literal");
            expect(sandbox::vm::ops::toLong(assembled.data[1]) == -7, ".word stores signed literal");
            expect(sandbox::vm::ops::toLong(assembled.data[2]) == 0, ".word resolves data label");

            VMState vm(32, 16);
            expect(loadAndReset(vm, assembled), "assembled text/data image loads");
            auto result = sandbox::vm::run(vm, 16);
            expect(result.halted(), "text/data program halts");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R2)) == 42,
                   "LOAD reads value through data label address");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R3)) == -7,
                   "LOAD immediate resolves data label absolutely");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R4)) == 1,
                   "MOV resolves data label absolutely");
            expect(sandbox::vm::ops::toLong(vm.regfile.read(R5)) == 0,
                   ".word label operand stores absolute data address");
        }
    }

    {
        auto mixed = assemble(R"(
            .text
        entry: mov r1, payload
               jmp done
            .data
        payload: .word 17
            .text
        done:  halt
        )");
        expect(mixed.success, "assembler accepts mixed text/data/text sections");
        if (mixed.success) {
            expect(mixed.labels.count("entry") && mixed.labels.at("entry") == 0,
                   "entry text label survives section switch");
            expect(mixed.labels.count("done") && mixed.labels.at("done") == 2,
                   "done text label address ignores data words");
            expect(mixed.data_labels.count("payload") && mixed.data_labels.at("payload") == 0,
                   "payload data label address ignores text words");
            auto jmp = InstructionWord::decodeSemantic(mixed.program[1]);
            expect(jmp.opcode == Opcode::JMP && jmp.offset == 1,
                   "branch labels remain PC-relative text offsets");
        }
    }

    expect(!assemble("foo: halt\n.data\nfoo: .word 1\n").success,
           "duplicate labels across text and data are rejected");
    expect(!assemble(".data\nx: .word 1\n.text\njmp x\n").success,
           "data labels cannot be branch targets");
    expect(!assemble(".word 1\nhalt\n").success,
           ".word outside .data is rejected");

    {
        auto placed = assemble(R"(
            .org 3
        start:
            halt
            .data
        first: .word 11
            .org 4
        pte:   .pte 8, 1, 0, 0, 1
        )");
        expect(placed.success, "assembler accepts .org and .pte directives");
        if (placed.success) {
            expect(placed.labels.count("start") && placed.labels.at("start") == 3,
                   ".org advances text addresses");
            expect(placed.program.size() == 4,
                   ".org pads text image with NOPs");
            expect(InstructionWord::decodeSemantic(placed.program[0]).opcode == Opcode::NOP &&
                   InstructionWord::decodeSemantic(placed.program[3]).opcode == Opcode::HALT,
                   ".org text padding executes as NOPs before placed code");
            expect(placed.data_labels.count("pte") && placed.data_labels.at("pte") == 4,
                   ".org advances data addresses");
            expect(placed.data.size() == 5,
                   ".org pads data image with zero words");
            PageTableEntry decoded;
            expect(decodePageTableEntry(placed.data[4], decoded),
                   ".pte emits a decodable raw PTE");
            expect(decoded.ppn == 8 && decoded.present && decoded.user &&
                   !decoded.read && !decoded.write && decoded.execute,
                   ".pte stores ppn and permission flags");
        }
    }

    expect(!assemble(".org -1\nhalt\n").success,
           ".org rejects negative addresses");
    expect(!assemble(".data\n.pte 1, 2, 0, 0, 1\n").success,
           ".pte rejects non-boolean flags");
    expect(!assemble(".pte 1, 1, 0, 0, 1\n").success,
           ".pte outside .data is rejected");

    {
        auto image = assemble(R"(
            .isa 2
            .text
        entry:
            halt
            .data
        app: .execheader2 0, 1, 0, 27, 1, 2, 0
        )");
        expect(image.success, "assembler accepts executable header directive");
        if (image.success) {
            expect(image.data_labels.count("app") && image.data_labels.at("app") == 0,
                   ".execheader2 defines a data label");
            expect(image.data.size() == EXEC_V2_HEADER_WORDS,
                   ".execheader2 emits fixed-size header words");
            expect(image.executable_headers_v2.count("app"),
                   ".execheader2 records executable metadata");
            const ExecutableImageHeaderV2 header =
                image.executable_headers_v2.at("app");
            expect(header.entry_pc == 0 &&
                   header.text_words == 1 &&
                   header.data_words == 0 &&
                   header.stack_words == 27 &&
                   header.syscall_abi_version == 2,
                   "executable metadata decodes header fields");
            VMState vm(64, 64);
            expect(initializeTaskContext(vm.dmem, 8, header, 1, 2),
                   "loader helper initializes a task context from executable metadata");
            expect(loadPhysLong(vm, 8 + TASK_CONTEXT_EPC) == 0 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_IMEM_PTBR) == 1 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_DMEM_PTBR) == 2 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 27,
                   "loader helper writes context PC, page tables, and SP");
        }

        expect(!assemble(
                    ".isa 2\n.data\nbad: .execheader2 0, 0, 0, 27, 1, 2, 0\n").success,
               ".execheader2 rejects an empty text image");
        expect(!assemble(
                    ".isa 2\n.execheader2 0, 1, 0, 27, 1, 2, 0\n").success,
               ".execheader2 outside .data is rejected");
        expect(!assemble(
                    ".isa 2\n.data\n.execheader2 0, 1, 0, 27, 1, 2, 0\n").success,
               ".execheader2 requires a label");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow(R"(
            nop
            nop
            nop
            halt
        )");
        expect(loadAndReset(vm, program), "hooked halt program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onTrap = [&](const VMState&, int pc) {
            events.push_back("trap:" + std::to_string(pc));
        };
        hooks.onHalt = [&](const VMState&, int pc) {
            events.push_back("halt:" + std::to_string(pc));
        };
        auto result = sandbox::vm::run(vm, 16, &hooks);
        expect(result.halted() && result.steps == 4, "hooked halt program runs four steps");
        const std::vector<std::string> want = {
            "step:0", "step:1", "step:2", "step:3", "halt:3"
        };
        expect(events == want, "hooks fire onStep per instruction then onHalt");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow(R"(
            mov r1, 1
            mov r2, 0
            div.t20 r3, r1, r2
            halt
        )");
        expect(loadAndReset(vm, program), "hooked trap program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onTrap = [&](const VMState&, int pc) {
            events.push_back("trap:" + std::to_string(pc));
        };
        auto result = sandbox::vm::run(vm, 16, &hooks);
        expect(result.trapped() && result.trap_code == TrapCode::TRAP_DIV_ZERO,
               "hooked trap program reports divide by zero");
        const std::vector<std::string> want = {
            "step:0", "step:1", "step:2", "trap:2"
        };
        expect(events == want, "hooks fire onStep per instruction then onTrap");
    }

    {
        VMState vm(16, 16);
        auto program = assembleOrThrow("nop\nhalt\n");
        expect(loadAndReset(vm, program), "run-limit program loads");
        std::vector<std::string> events;
        VMHooks hooks;
        hooks.onStep = [&](const VMState&, int pc) {
            events.push_back("step:" + std::to_string(pc));
        };
        hooks.onHalt = [&](const VMState&, int pc) {
            events.push_back("halt:" + std::to_string(pc));
        };
        auto first = sandbox::vm::run(vm, 1, &hooks);
        expect(first.timeout() && first.steps == 1 && vm.isRunning() && vm.pc == 1,
               "step limit stops after exact instruction count without trap/halt");
        expect(events.size() == 1 && events[0] == "step:0",
               "timeout fires only onStep for executed instruction");
        auto second = sandbox::vm::run(vm, 1, &hooks);
        expect(second.halted() && second.steps == 1,
               "continuing after timeout can reach HALT");
        expect(events.size() == 3 && events[1] == "step:1" && events[2] == "halt:1",
               "HALT hook fires when second run executes terminal instruction");
    }

    {
        const LongTriple* lnA = &native_ops::cachedLn3();
        const LongTriple* lnB = &native_ops::cachedLn3();
        const LongTriple* piA = &native_ops::cachedPi();
        const LongTriple* piB = &native_ops::cachedPi();
        expect(lnA == lnB, "cachedLn3 returns stable cached object");
        expect(piA == piB, "cachedPi returns stable cached object");
        expect(native_ops::compare(native_ops::ln3(), *lnA) == 0,
               "ln3 preserves public value through cache");
        expect(native_ops::compare(native_ops::pi(), *piA) == 0,
               "pi preserves public value through cache");
    }
}

void testOsSubstrate() {
    std::cout << "[9] Phase 3 OS substrate VM machine contract\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;
    const auto assembleOrThrow = [](const std::string& source) {
        return assembleV2TestOrThrow(source);
    };

    auto asLong = [](const VMState& vm, int reg) {
        return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
    };
    auto loadPhysLong = [](VMState& vm, int addr) {
        auto [value, fault] = vm.dmem.load(addr);
        expect(fault == MemFaultCode::OK, "physical DMEM load succeeds in test helper");
        return sandbox::vm::ops::toLong(value);
    };

    {
        std::cerr << "DEBUG: OS sub-test 1" << std::endl;
        VMState vm(64, 64);
        auto assembled = assemble(R"(
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
        expect(assembled.success, "routed div-zero program assembles");
        if (assembled.success) {
            std::cerr << "DEBUG: success=" << assembled.success << " count(fault_div)=" << assembled.labels.count("fault_div") << std::endl;
            expect(loadAndReset(vm, assembled), "routed div-zero program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "routed div-zero handler returns to halt");
            expect(asLong(vm, R4) == OS_CAUSE_DIV_ZERO, "routed div-zero stores cause");
            expect(asLong(vm, R5) == assembled.labels.at("fault_div"),
                   "routed div-zero stores faulting PC in EPC");
            expect(vm.privilege == PrivilegeMode::User,
                   "ERET restores user mode before resumed HALT");
        }
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov sp, 11
            mov r1, 22
            csrw scratch, r1
            csrrw sp, scratch, sp
            csrr r2, scratch
            halt
        )");
        expect(loadAndReset(vm, program), "CSRRW scratch swap program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "CSRRW scratch swap program halts");
        expect(asLong(vm, R26_SP) == 22, "CSRRW writes old scratch to rd");
        expect(asLong(vm, R2) == 11, "CSRRW writes source value into CSR");
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r1, 99
            csrrw r2, scratch, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user CSRRW trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user CSRRW trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user CSRRW routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user CSRRW routes protection fault");
        }
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r1, 99
            csrw scratch, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user CSRW trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user CSRW trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user CSRW routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user CSRW routes protection fault");
        }
    }

    {
        std::cerr << "DEBUG: OS sub-test 5" << std::endl;
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            eret
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "user ERET trap program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "user ERET trap program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "user ERET routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT,
                   "user ERET routes protection fault");
        }
    }

    {
        VMState vm(64, 64);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 123
            syscall 9
        after_syscall:
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            mov r13, 777
            mov r1, after_syscall
            csrw epc, r1
            eret
        )");
        expect(assembled.success, "routed syscall program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed syscall program loads");
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "routed syscall handler returns");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "SYSCALL routes ECALL cause");
            expect(asLong(vm, R5) == 9, "SYSCALL stores immediate in syscall_id CSR");
            expect(asLong(vm, 13) == 777, "syscall return value uses r13");
        }
    }

    {
        VMState vm(96, 96);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 123
            syscall 1
            syscall 2
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            mov r1, 1
            tcmp r2, r5, r1
            brz r2, write_value
            mov r1, 2
            tcmp r2, r5, r1
            brz r2, write_newline
            mov r13, -1
            jmp syscall_return
        write_value:
            csrw console_out, r13
            mov r13, 0
            jmp syscall_return
        write_newline:
            mov r1, 1
            csrw console_ctrl, r1
            mov r13, 0
        syscall_return:
            csrr r1, epc
            mov r2, 1
            add r1, r1, r2
            csrw epc, r1
            eret
        )");
        expect(assembled.success, "routed syscall console program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed syscall console program loads");
            auto result = sandbox::vm::run(vm, 96);
            expect(result.halted(), "routed syscall console program returns");
            expect(vm.syscall_buffer == "123\n",
                   "kernel console CSR writes routed syscall output");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "console syscall routes ECALL cause");
            expect(asLong(vm, R5) == 2, "console syscall records final syscall id");
            expect(asLong(vm, 13) == 0, "console syscall returns success in r13");
        }
    }

    {
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 88
            syscall 22
        after_syscall:
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            csrr r6, status
            mov r1, 2
            csrw console_ctrl, r1
            csrw console_out, r13
            mov r1, 3
            csrw console_ctrl, r1
            mov r13, 0
            mov r1, after_syscall
            csrw epc, r1
            eret
        )");
        expect(assembled.success, "routed sys_write_char program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "routed sys_write_char program loads");
            auto result = sandbox::vm::run(vm, 96);
            expect(result.halted(), "routed sys_write_char handler returns");
            expect(vm.syscall_buffer == "X", "kernel character-mode CSR writes routed char output");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "sys_write_char routes ECALL cause");
            expect(asLong(vm, R5) == SYSCALL_WRITE_CHAR, "sys_write_char records syscall id");
            expect(VMState::tritAt(asLong(vm, R6), 0) == T_NEG,
                   "trap handler observes kernel privilege in status CSR");
            expect(VMState::tritAt(asLong(vm, R6), 2) == T_POS,
                   "trap handler status records previous user privilege");
            expect(vm.privilege == PrivilegeMode::User,
                   "ERET restores user privilege after sys_write_char");
            expect(asLong(vm, 13) == 0, "sys_write_char returns success in r13");
        }
    }

    {
        VMState vm(96, 96);
        auto assembled = assemble(R"(
            mov r1, handler
            csrw tvec, r1
            mov r1, 48
            csrw scratch, r1
            mov sp, 19
            mov r1, -8
            csrw status, r1
            mov r13, 65
        trap_from_user:
            syscall 22
        after_syscall:
            halt
        handler:
            csrrw sp, scratch, sp
            csrr r4, cause
            csrr r5, epc
            csrr r6, status
            csrr r7, syscall_id
            csrr r8, scratch
            mov r1, 4242
            store r1, sp, 0
            mov r1, 2
            csrw console_ctrl, r1
            csrw console_out, r13
            mov r1, 3
            csrw console_ctrl, r1
            mov r13, 0
            mov r1, after_syscall
            csrw epc, r1
            csrrw sp, scratch, sp
            eret
        )");
        expect(assembled.success, "D1-style user trap entry program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "D1-style user trap entry program loads");
            auto result = sandbox::vm::run(vm, 128);
            expect(result.halted(), "D1-style trap handler returns to user halt");
            expect(vm.syscall_buffer == "A", "D1-style handler writes syscall character");
            expect(asLong(vm, R4) == OS_CAUSE_SYSCALL, "D1-style handler observes syscall cause");
            expect(asLong(vm, R5) == assembled.labels.at("trap_from_user"),
                   "D1-style handler captures user EPC");
            expect(asLong(vm, R7) == SYSCALL_WRITE_CHAR,
                   "D1-style handler captures syscall id");
            expect(asLong(vm, R8) == 19,
                   "D1-style CSRRW exposes user stack pointer in scratch");
            expect(loadPhysLong(vm, 48) == 4242,
                   "D1-style handler stores through kernel stack pointer");
            expect(asLong(vm, R26_SP) == 19,
                   "D1-style handler restores user stack pointer before ERET");
            expect(vm.scratch == 48,
                   "D1-style handler restores kernel stack pointer to scratch");
            expect(VMState::tritAt(asLong(vm, R6), 0) == T_NEG,
                   "D1-style handler runs in kernel privilege");
            expect(VMState::tritAt(asLong(vm, R6), 2) == T_POS,
                   "D1-style handler records previous user privilege");
            expect(vm.privilege == PrivilegeMode::User,
                   "D1-style ERET restores user privilege");
        }
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, -1
            csrw console_ctrl, r1
            mov r1, 55
            csrw console_out, r1
            mov r1, 1
            csrw console_ctrl, r1
            csrr r2, console_ctrl
            halt
        )");
        expect(loadAndReset(vm, program), "kernel console CSR program loads");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "kernel console CSR program halts");
        expect(vm.syscall_buffer == "55\n", "console CSRs update output buffer");
        expect(asLong(vm, R2) == 3, "console_ctrl reads output buffer length");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            csrr r1, console_in_ctrl
            csrr r2, console_in
            mov r3, 1
            csrw console_in_ctrl, r3
            csrr r4, console_in_ctrl
            csrr r5, console_in
            mov r3, -1
            csrw console_in_ctrl, r3
            csrr r6, console_in_ctrl
            halt
        )");
        expect(loadAndReset(vm, program), "kernel console input CSR program loads");
        vm.enqueueConsoleAscii("az");
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "kernel console input CSR program halts");
        expect(asLong(vm, R1) == 2 &&
               asLong(vm, R2) == 97 &&
               asLong(vm, R4) == 1 &&
               asLong(vm, R5) == 122 &&
               asLong(vm, R6) == 0,
               "console input CSRs peek, consume, and clear host-fed input");
    }

    {
        std::cerr << "DEBUG: OS sub-test 10" << std::endl;
        VMState vm(64, 64);
        auto assembled = assemble(R"(
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
        expect(assembled.success, "timer IRQ program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "timer IRQ program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.interrupt_enable = true;
            vm.timer_counter = 3;
            vm.timer_reload = 0;
            vm.timer_enable = true;
            auto result = sandbox::vm::run(vm, 64);
            expect(result.halted(), "timer IRQ handler resumes program");
            expect(asLong(vm, R6) == OS_CAUSE_TIMER_IRQ, "timer IRQ routes interrupt cause");
            expect(asLong(vm, R7) == assembled.labels.at("timer_after"),
                   "timer IRQ EPC is next PC after exact instruction count");
            expect(asLong(vm, R8) == 99, "program resumes after timer ERET");
        }
    }

    {
        VMState vm(96, 64);
        auto assembled = assemble(R"(
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
        expect(assembled.success, "critical section timer program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "critical section timer program loads");
            auto result = sandbox::vm::run(vm, 96);
            expect(result.halted(), "critical section timer program halts");
            expect(asLong(vm, R2) == 1,
                   "timer interrupt remains pending while interrupts are disabled");
            expect(asLong(vm, R4) == OS_CAUSE_TIMER_IRQ,
                   "pending timer routes after interrupts are re-enabled");
            expect(asLong(vm, R5) == assembled.labels.at("after_enable"),
                   "pending timer EPC is the first instruction after critical section");
            expect(asLong(vm, R8) == 44,
                   "program resumes after deferred timer interrupt");
        }
    }

    {
        VMState vm(16, 16);
        auto assembled = assemble(R"(
            nop
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "fetch protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "fetch protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_imem_base = 1;
            vm.user_imem_limit = 2;
            auto result = sandbox::vm::run(vm, 16);
            expect(result.halted(), "fetch protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_FETCH_FAULT,
                   "user fetch outside IMEM range routes fetch fault");
        }
    }

    {
        VMState vm(32, 64);
        auto assembled = assemble(R"(
            mov r1, 9
            load r2, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "load protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "load protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_dmem_base = 10;
            vm.user_dmem_limit = 12;
            auto result = sandbox::vm::run(vm, 32);
            expect(result.halted(), "load protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_LOAD_FAULT,
                   "user load outside DMEM range routes load fault");
        }
    }

    {
        VMState vm(32, 64);
        auto assembled = assemble(R"(
            mov r1, 9
            mov r2, 77
            store r2, r1
            halt
        handler:
            csrr r4, cause
            halt
        )");
        expect(assembled.success, "store protection program assembles");
        if (assembled.success) {
            expect(loadAndReset(vm, assembled), "store protection program loads");
            vm.trap_routing_enabled = true;
            vm.tvec = assembled.labels.at("handler");
            vm.privilege = PrivilegeMode::User;
            vm.user_dmem_base = 10;
            vm.user_dmem_limit = 12;
            auto result = sandbox::vm::run(vm, 32);
            expect(result.halted(), "store protection routes to handler");
            expect(asLong(vm, R4) == OS_CAUSE_STORE_FAULT,
                   "user store outside DMEM range routes store fault");
        }
    }

    {
        std::cerr << "DEBUG: OS sub-test 15" << std::endl;
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto user = assembleOrThrow(R"(
            mov r1, 42
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "MMU fetch user program loads at physical page");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "MMU translates user fetches");
        expect(asLong(vm, R1) == 42, "MMU fetch executes mapped physical IMEM page");
    }

    {
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto user = assembleOrThrow(R"(
            load r1, zero, 0
            mov r2, 1
            add.t20 r1, r1, r2
            store r1, zero, 0
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "MMU data user program loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, true, false));
        vm.dmem.store(2 * MMU_PAGE_WORDS, sandbox::vm::ops::fromLong(5));
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "MMU translates user load/store");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) == 6,
               "MMU store updates mapped physical data page");
    }

    {
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "fetch page fault handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true, false));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "absent IMEM PTE routes page fault");
        expect(asLong(vm, R4) == OS_CAUSE_FETCH_PAGE_FAULT, "absent fetch PTE cause");
        expect(asLong(vm, R5) == 0, "fetch page fault records virtual address");
        expect(asLong(vm, R6) == OS_PAGE_ACCESS_FETCH, "fetch page fault records access type");
    }

    {
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto user = assembleOrThrow(R"(
            load r1, zero, 0
            store r1, zero, 0
            halt
        )");
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_addr
            csrr r6, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(user, MMU_PAGE_WORDS), "read-only data user program loads");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "read-only data handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, false, false));
        vm.dmem.store(2 * MMU_PAGE_WORDS, sandbox::vm::ops::fromLong(33));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "read-only page rejects user store");
        expect(asLong(vm, R1) == 33, "read-only page allows user load");
        expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT, "read-only store protection cause");
        expect(asLong(vm, R5) == 0, "store protection records virtual address");
        expect(asLong(vm, R6) == OS_PAGE_ACCESS_STORE, "store protection records access type");
    }

    {
        VMState vm(4 * MMU_PAGE_WORDS, 4 * MMU_PAGE_WORDS);
        auto handler = assembleOrThrow(R"(
            csrr r4, cause
            csrr r5, page_fault_access
            halt
        )");
        expect(vm.imem.loadProgram(handler, 2 * MMU_PAGE_WORDS), "NX handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, true, false, false));
        vm.trap_routing_enabled = true;
        vm.tvec = 2 * MMU_PAGE_WORDS;
        vm.privilege = PrivilegeMode::User;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "non-executable PTE routes protection fault");
        expect(asLong(vm, R4) == OS_CAUSE_PROTECTION_FAULT, "NX fetch protection cause");
        expect(asLong(vm, R5) == OS_PAGE_ACCESS_FETCH, "NX fetch records access type");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 9
            load r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "kernel MMU bypass program loads");
        vm.mmu_enable = true;
        vm.user_dmem_ptbr = 0;
        vm.user_dmem_pages = 0;
        vm.dmem.store(9, sandbox::vm::ops::fromLong(66));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "kernel bypasses MMU translation");
        expect(asLong(vm, R2) == 66, "kernel load succeeds with MMU enabled");
    }

    {
        VMState vm(6 * MMU_PAGE_WORDS, 5 * MMU_PAGE_WORDS);
        auto user = assembleOrThrow(R"(
        loop:
            load r1, zero, 0
            mov r2, 1
            add r1, r1, r2
            store r1, zero, 0
            jmp loop
        )");
        auto handler = assembleOrThrow(R"(
            csrrw sp, scratch, sp
            store r1, sp, 6
            store r2, sp, 7
            store r3, sp, 8
            store r4, sp, 9
            store r5, sp, 10
            store r6, sp, 11
            store r7, sp, 12
            store r8, sp, 13
            store r9, sp, 14
            store r10, sp, 15
            store r11, sp, 16
            store r12, sp, 17
            store r13, sp, 18
            store r14, sp, 19
            store r15, sp, 20
            store r16, sp, 21
            store r17, sp, 22
            store r18, sp, 23
            store r19, sp, 24
            store r20, sp, 25
            store r21, sp, 26
            store r22, sp, 27
            store r23, sp, 28
            store r24, sp, 29
            store r25, sp, 30
            csrr r1, scratch
            store r1, sp, 31
            csrr r1, epc
            store r1, sp, 0
            csrr r1, status
            store r1, sp, 1
            csrr r1, user_imem_ptbr
            store r1, sp, 2
            csrr r1, user_imem_pages
            store r1, sp, 3
            csrr r1, user_dmem_ptbr
            store r1, sp, 4
            csrr r1, user_dmem_pages
            store r1, sp, 5
            mov r1, 0
            csrw timer_pending, r1
            csrw timer_enable, r1
            mov r1, 120
            tcmp r2, sp, r1
            brz r2, use_ctx1
            mov r3, 120
            jmp restore_next
        use_ctx1:
            mov r3, 152
        restore_next:
            copy sp, r3
            load r1, sp, 0
            csrw epc, r1
            load r1, sp, 1
            csrw status, r1
            load r1, sp, 2
            csrw user_imem_ptbr, r1
            load r1, sp, 3
            csrw user_imem_pages, r1
            load r1, sp, 4
            csrw user_dmem_ptbr, r1
            load r1, sp, 5
            csrw user_dmem_pages, r1
            load r1, sp, 31
            csrw scratch, r1
            mov r1, 180
            csrw timer_counter, r1
            mov r1, 1
            csrw timer_enable, r1
            load r1, sp, 6
            load r2, sp, 7
            load r3, sp, 8
            load r4, sp, 9
            load r5, sp, 10
            load r6, sp, 11
            load r7, sp, 12
            load r8, sp, 13
            load r9, sp, 14
            load r10, sp, 15
            load r11, sp, 16
            load r12, sp, 17
            load r13, sp, 18
            load r14, sp, 19
            load r15, sp, 20
            load r16, sp, 21
            load r17, sp, 22
            load r18, sp, 23
            load r19, sp, 24
            load r20, sp, 25
            load r21, sp, 26
            load r22, sp, 27
            load r23, sp, 28
            load r24, sp, 29
            load r25, sp, 30
            csrrw sp, scratch, sp
            eret
        )");
        constexpr int kUserPhys = MMU_PAGE_WORDS;
        constexpr int kHandlerPhys = 4 * MMU_PAGE_WORDS;
        constexpr int kTask0Context = 120;
        constexpr int kTask1Context = 152;
        constexpr int kTaskStatus = -1 + 9 + 27; // kernel current, user previous, previous IE set.

        expect(vm.imem.loadProgram(user, kUserPhys), "two-task user program loads");
        expect(vm.imem.loadProgram(handler, kHandlerPhys), "two-task timer handler loads");
        vm.dmem.store(0, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(4, encodePageTableEntry(2, true, true, true, false));
        vm.dmem.store(8, encodePageTableEntry(1, true, false, false, true));
        vm.dmem.store(12, encodePageTableEntry(3, true, true, true, false));

        vm.dmem.store(kTask1Context + TASK_CONTEXT_EPC, sandbox::vm::ops::fromLong(0));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_STATUS, sandbox::vm::ops::fromLong(kTaskStatus));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_IMEM_PTBR, sandbox::vm::ops::fromLong(8));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_IMEM_PAGES, sandbox::vm::ops::fromLong(1));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_DMEM_PTBR, sandbox::vm::ops::fromLong(12));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_DMEM_PAGES, sandbox::vm::ops::fromLong(1));
        vm.dmem.store(kTask1Context + TASK_CONTEXT_REG_BASE + R26_SP - 1,
                      sandbox::vm::ops::fromLong(24));

        vm.trap_routing_enabled = true;
        vm.tvec = kHandlerPhys;
        vm.privilege = PrivilegeMode::User;
        vm.interrupt_enable = true;
        vm.mmu_enable = true;
        vm.user_imem_ptbr = 0;
        vm.user_imem_pages = 1;
        vm.user_dmem_ptbr = 4;
        vm.user_dmem_pages = 1;
        vm.scratch = kTask0Context;
        vm.regfile.write(R26_SP, sandbox::vm::ops::fromLong(24));
        vm.timer_counter = 40;
        vm.timer_enable = true;

        auto result = sandbox::vm::run(vm, 900);
        expect(result.timeout() && vm.isRunning(), "two-task timer proof keeps VM running");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) > 0,
               "task 0 physical counter advances");
        expect(loadPhysLong(vm, 3 * MMU_PAGE_WORDS) > 0,
               "task 1 physical counter advances");
        expect(loadPhysLong(vm, 2 * MMU_PAGE_WORDS) !=
                   loadPhysLong(vm, 3 * MMU_PAGE_WORDS),
               "tasks retain independent physical counters");
        expect(loadPhysLong(vm, kTask0Context + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
               "task 0 saved user stack pointer");
        expect(loadPhysLong(vm, kTask1Context + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
               "task 1 saved user stack pointer");
    }

    // Retired with the v2 clean cutover. The 27-word-page bring-up remains
    // reproducible from tag trit-v1-final and its golden fixtures; it is not
    // part of the production runtime or regression matrix.

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 9
            load r2, r1
            halt
        )");
        expect(loadAndReset(vm, program), "kernel bypass program loads");
        vm.user_dmem_base = 10;
        vm.user_dmem_limit = 12;
        vm.dmem.store(9, sandbox::vm::ops::fromLong(55));
        auto result = sandbox::vm::run(vm, 16);
        expect(result.halted(), "kernel bypasses user DMEM bounds");
        expect(asLong(vm, R2) == 55, "kernel load succeeds outside user window");
    }
}

void testTernaryAtomicsAndLockAbi() {
    std::cout << "[10] ternary atomics and lock ABI\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;
    const auto assembleOrThrow = [](const std::string& source) {
        return assembleV2TestOrThrow(source);
    };

    auto asLong = [](const VMState& vm, int reg) {
        return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
    };
    auto loadPhysLong = [](VMState& vm, int addr) {
        auto [value, fault] = vm.dmem.load(addr);
        expect(fault == MemFaultCode::OK, "physical DMEM load succeeds in atomics helper");
        return sandbox::vm::ops::toLong(value);
    };

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 42
            tldr.+1 r3, r1
            tstr.+1 r4, r1, r2, r3
            load r5, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TLDR/TSTR success program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TLDR/TSTR success program halts");
        expect(asLong(vm, R3) == 7, "TLDR reads reserved word");
        expect(asLong(vm, R4) == 1, "TSTR returns +1 on success");
        expect(asLong(vm, R5) == 42, "TSTR writes desired value");
        expect(loadPhysLong(vm, 5) == 42, "successful TSTR updates memory");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 42
            mov r6, 99
            tldr r3, r1
            tstr r4, r1, r2, r6
            load r5, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TSTR mismatch program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TSTR mismatch program halts");
        expect(asLong(vm, R4) == 0, "TSTR returns 0 on value mismatch");
        expect(asLong(vm, R5) == 7, "TSTR mismatch leaves memory unchanged");
    }

    {
        VMState vm(32, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 8
            mov r5, 42
            tldr r3, r1
            store r2, r1
            tstr r4, r1, r5, r3
            load r6, r1
            halt
        )");
        expect(loadAndReset(vm, program), "TSTR collision program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(7));
        auto result = sandbox::vm::run(vm, 32);
        expect(result.halted(), "TSTR collision program halts");
        expect(asLong(vm, R4) == -1, "TSTR returns -1 when reservation is lost");
        expect(asLong(vm, R6) == 8, "ordinary store invalidates reservation before TSTR");
    }

    {
        VMState vm(64, 64);
        auto program = assembleOrThrow(R"(
            mov r1, 5
            mov r2, 1
            mov r3, 0
            tldr.0 r4, r1
            tstr.+1 r5, r1, r2, r3
            brp r5, acquired
            halt
        acquired:
            mov r6, 6
            load r7, r6
            mov r8, 1
            add.t40 r9, r7, r8
            store r9, r6
            store r3, r1
            fence.+1
            halt
        )");
        expect(loadAndReset(vm, program), "lock ABI acquire/release program loads");
        vm.dmem.store(5, sandbox::vm::ops::fromLong(0));
        vm.dmem.store(6, sandbox::vm::ops::fromLong(10));
        auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "lock ABI acquire/release program halts");
        expect(asLong(vm, R5) == 1, "lock acquire TSTR succeeds");
        expect(loadPhysLong(vm, 5) == 0, "lock release stores zero");
        expect(loadPhysLong(vm, 6) == 11, "critical section updates protected counter");
    }
}
