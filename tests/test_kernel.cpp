#include "test_multiwidth_vm_common.h"

void testPhase35Infrastructure() {
    std::cout << "[8] Phase 3.5 VM hooks, data sections, and run limits\n";
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;
    using namespace sandbox::vm::assembler;

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
            auto jmp = InstructionWord::decode(mixed.program[1]);
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
            expect(InstructionWord::decode(placed.program[0]).opcode == Opcode::NOP &&
                   InstructionWord::decode(placed.program[3]).opcode == Opcode::HALT,
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
            .text
        entry:
            halt
            .data
        app: .execheader 0, 1, 1, 24, 1, 0
        )");
        expect(image.success, "assembler accepts executable header directive");
        if (image.success) {
            expect(image.data_labels.count("app") && image.data_labels.at("app") == 0,
                   ".execheader defines a data label");
            expect(image.data.size() == EXEC_HEADER_WORDS,
                   ".execheader emits fixed-size header words");
            expect(image.executable_headers.count("app"),
                   ".execheader records executable metadata");
            const ExecutableImageHeader header = image.executable_headers.at("app");
            expect(header.entry_virtual_pc == 0 &&
                   header.text_pages == 1 &&
                   header.data_pages == 1 &&
                   header.stack_words == 24 &&
                   header.syscall_abi_version == EXEC_SYSCALL_ABI_VERSION_V1,
                   "executable metadata decodes header fields");
            VMState vm(64, 64);
            expect(initializeTaskContext(vm.dmem, 8, header, 1, 2),
                   "loader helper initializes a task context from executable metadata");
            expect(loadPhysLong(vm, 8 + TASK_CONTEXT_EPC) == 0 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_IMEM_PTBR) == 1 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_DMEM_PTBR) == 2 &&
                   loadPhysLong(vm, 8 + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "loader helper writes context PC, page tables, and SP");
        }

        expect(!assemble(".data\nbad: .execheader 0, 0, 1, 24, 1, 0\n").success,
               ".execheader rejects invalid text page count");
        expect(!assemble(".execheader 0, 1, 1, 24, 1, 0\n").success,
               ".execheader outside .data is rejected");
        expect(!assemble(".data\n.execheader 0, 1, 1, 24, 1, 0\n").success,
               ".execheader requires a label");
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
        VMState vm(96, 96);
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
        VMState vm(96, 128);
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
        VMState vm(96, 96);
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
        VMState vm(96, 128);
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
        VMState vm(96, 96);
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
        VMState vm(320, 224);
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

    {
        std::cerr << "DEBUG: OS sub-test 15 - bringup" << std::endl;
        const std::string source = readTextFile("OS3/minimal_kernel_bringup.tasm");
        expect(!source.empty(), "minimal kernel bring-up artifact is readable");
        auto assembled = assemble(source);
        expect(assembled.success, "minimal kernel bring-up artifact assembles");
        if (assembled.success) {
            std::cerr << "DEBUG: executable_headers keys:";
            for (auto const& [key, val] : assembled.executable_headers) std::cerr << " " << key;
            std::cerr << "\nDEBUG: data_labels keys:";
            for (auto const& [key, val] : assembled.data_labels) std::cerr << " " << key;
            std::cerr << std::endl;
            expect(assembled.labels.count("boot") && assembled.labels.at("boot") == 0,
                   "minimal kernel boots at PC zero");
            expect(assembled.labels.count("shell_loop") && assembled.labels.at("shell_loop") == 50 * MMU_PAGE_WORDS,
                   "minimal kernel places shell code on mapped physical page");
            expect(assembled.labels.count("prog_a") && assembled.labels.at("prog_a") == 56 * MMU_PAGE_WORDS,
                   "minimal kernel places static program A on mapped physical page");
            expect(assembled.labels.count("prog_b") && assembled.labels.at("prog_b") == 57 * MMU_PAGE_WORDS,
                   "minimal kernel places static program B on mapped physical page");
            expect(assembled.labels.count("idle_loop") && assembled.labels.at("idle_loop") == 58 * MMU_PAGE_WORDS,
                   "minimal kernel places idle task code on mapped physical page");
            expect(assembled.data_labels.count("shell_data") &&
                   assembled.data_labels.at("shell_data") == 16 * MMU_PAGE_WORDS,
                   "minimal kernel maps shell data page");
            expect(assembled.data_labels.count("prog_a_counter") &&
                   assembled.data_labels.at("prog_a_counter") == 17 * MMU_PAGE_WORDS,
                   "minimal kernel maps program A data page");
            expect(assembled.data_labels.count("prog_b_counter") &&
                   assembled.data_labels.at("prog_b_counter") == 18 * MMU_PAGE_WORDS,
                   "minimal kernel maps program B data page");
            expect(assembled.data_labels.count("idle_counter") &&
                   assembled.data_labels.at("idle_counter") == 19 * MMU_PAGE_WORDS,
                   "minimal kernel maps idle counter page");
            expect(assembled.executable_headers.count("exec_shell") &&
                   assembled.executable_headers.count("exec_prog_a") &&
                   assembled.executable_headers.count("exec_prog_b"),
                   "minimal kernel defines executable image metadata");
            expect(assembled.executable_headers.at("exec_shell").text_pages == 6,
                   "minimal kernel maps expanded Trit OS shell text");
            expect(assembled.data_labels.count("proc_count") &&
                   assembled.data_labels.count("user_proc_count") &&
                   assembled.data_labels.count("idle_proc") &&
                   assembled.data_labels.count("current_proc") &&
                   assembled.data_labels.count("ready_head") &&
                   assembled.data_labels.count("ready_tail") &&
                   assembled.data_labels.count("proc_table"),
                   "minimal kernel defines process table metadata");
            expect(assembled.data_labels.count("proc_state") &&
                   assembled.data_labels.count("proc_parent_pid") &&
                   assembled.data_labels.count("proc_exit_status") &&
                   assembled.data_labels.count("proc_ticks") &&
                   assembled.data_labels.count("proc_quantum_remaining") &&
                   assembled.data_labels.count("proc_preemptions") &&
                   assembled.data_labels.count("proc_wakeup_tick") &&
                   assembled.data_labels.count("proc_wait_channel") &&
                   assembled.data_labels.count("proc_wait_target") &&
                   assembled.data_labels.count("proc_ready_next") &&
                   assembled.data_labels.count("proc_wait_next") &&
                   assembled.data_labels.count("proc_yields") &&
                   assembled.data_labels.count("proc_sleeps") &&
                   assembled.data_labels.count("proc_exits") &&
                   assembled.data_labels.count("proc_spawns") &&
                   assembled.data_labels.count("proc_waits") &&
                   assembled.data_labels.count("proc_read_blocks") &&
                   assembled.data_labels.count("proc_input_reads"),
                   "minimal kernel defines scheduler lifecycle metadata");
            expect(assembled.data_labels.count("proc_heap_start") &&
                   assembled.data_labels.count("proc_heap_break") &&
                   assembled.data_labels.count("proc_heap_limit") &&
                   assembled.data_labels.count("proc_forks") &&
                   assembled.data_labels.count("proc_execs") &&
                   assembled.data_labels.count("os_fd_open") &&
                   assembled.data_labels.count("os_file_size") &&
                   assembled.data_labels.count("spare_data"),
                   "minimal kernel defines Trit OS syscall metadata");

            VMState vm(2048, 768);
            expect(loadAndReset(vm, assembled), "minimal kernel image loads");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_count")) == 5,
                   "minimal kernel process table declares four user slots plus idle");
            expect(loadPhysLong(vm, assembled.data_labels.at("user_proc_count")) == 4,
                   "minimal kernel process table declares four user slots");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_proc")) == 4,
                   "minimal kernel records idle process index");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_table")) ==
                       assembled.data_labels.at("ctx_shell") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 1) ==
                       assembled.data_labels.at("ctx_a") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 2) ==
                       assembled.data_labels.at("ctx_b") &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_table") + 4) ==
                       assembled.data_labels.at("idle_ctx"),
                   "minimal kernel process table points at task and idle contexts");
            auto result = sandbox::vm::run(vm, 1000);
            expect(result.timeout() && vm.isRunning(),
                   "minimal kernel idles while shell blocks for input");
            expect(vm.trap_routing_enabled && vm.mmu_enable,
                   "minimal kernel boot enabled routed traps and MMU");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_state")) == PROC_STATE_BLOCKED &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_wait_channel")) == PROC_WAIT_CONSOLE_INPUT,
                   "shell blocks on console input without polling");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_read_blocks")) > 0,
                   "console input wait is accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_counter")) > 0,
                   "idle task runs while shell waits for input");

            vm.enqueueConsoleAscii("awbu x");
            result = sandbox::vm::run(vm, 20000);
            expect(result.timeout() && vm.isRunning(),
                   "minimal kernel services image keeps running under timer preemption");
            expect(loadPhysLong(vm, assembled.data_labels.at("current_proc")) >= 0 &&
                   loadPhysLong(vm, assembled.data_labels.at("current_proc")) <
                       loadPhysLong(vm, assembled.data_labels.at("proc_count")),
                   "minimal kernel scheduler keeps current process index in range");
            expect(loadPhysLong(vm, assembled.data_labels.at("prog_a_counter")) == 1,
                   "spawned program A runs once and exits");
            expect(loadPhysLong(vm, assembled.data_labels.at("prog_b_counter")) == 1,
                   "spawned program B runs once and exits");
            expect(loadPhysLong(vm, assembled.data_labels.at("shell_data")) == 3,
                   "shell records last spawned child PID");
            expect(loadPhysLong(vm, assembled.data_labels.at("shell_data") + 1) == 11,
                   "waitpid returns program A exit status to shell memory");
            expect(!vm.syscall_buffer.empty() &&
                   vm.syscall_buffer.find("2\n") != std::string::npos &&
                   vm.syscall_buffer.find("11\n") != std::string::npos &&
                   vm.syscall_buffer.find("3\n") != std::string::npos,
                   "shell prints spawn and wait results through console CSR");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_shell") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved shell user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_a") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved program A user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("ctx_b") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved program B user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("idle_ctx") + TASK_CONTEXT_REG_BASE + R26_SP - 1) == 24,
                   "minimal kernel saved idle user stack pointer");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_spawns")) == 2,
                   "spawn syscall accounts shell-created children");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_waits")) == 1,
                   "waitpid blocking path is accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_input_reads")) >= 5,
                   "console input reads are accounted");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_exits")) == 1 &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_exits") + 1) == 1 &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_exits") + 2) == 1,
                   "exit syscall accounts shell and children");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_state")) == PROC_STATE_EXITED &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_state") + 1) == PROC_STATE_FREE &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_state") + 2) == PROC_STATE_EXITED,
                   "waited child is freed and un-waited child remains exited");
            expect(loadPhysLong(vm, assembled.data_labels.at("ready_head")) == -1 &&
                   loadPhysLong(vm, assembled.data_labels.at("ready_tail")) == -1,
                   "ready queue drains when only idle remains runnable");
            expect(loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining")) <= PROC_DEFAULT_QUANTUM &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining") + 1) <= PROC_DEFAULT_QUANTUM &&
                   loadPhysLong(vm, assembled.data_labels.at("proc_quantum_remaining") + 4) <= PROC_DEFAULT_QUANTUM,
                   "minimal kernel tracks per-process quantum remaining");

            std::cerr << "DEBUG: OS sub-test 15 - spawn exhaustion" << std::endl;
            VMState exhaustedVm(2048, 768);
            expect(loadAndReset(exhaustedVm, assembled), "spawn exhaustion image loads");
            exhaustedVm.enqueueConsoleAscii("aa x");
            auto exhaustedResult = sandbox::vm::run(exhaustedVm, 20000);
            expect(exhaustedResult.timeout() && exhaustedVm.isRunning(),
                   "kernel keeps running through spawn exhaustion");
            expect(exhaustedVm.syscall_buffer.find("-1\n") != std::string::npos,
                   "spawn returns -1 when the static slot is not free");
            expect(loadPhysLong(exhaustedVm, assembled.data_labels.at("proc_spawns")) == 1,
                   "failed spawn is not counted as a created process");

            std::cerr << "DEBUG: OS sub-test 15 - syscall probe" << std::endl;
            VMState osVm(2048, 768);
            expect(loadAndReset(osVm, assembled), "Trit OS syscall probe image loads");
            osVm.enqueueConsoleAscii("p x");
            auto osResult = sandbox::vm::run(osVm, 30000);
            expect(osResult.timeout() && osVm.isRunning(),
                   "kernel keeps running after Trit OS file and heap syscalls");
            const int shellBase = assembled.data_labels.at("shell_data");
            expect(loadPhysLong(osVm, shellBase + 2) == 1 &&
                   loadPhysLong(osVm, shellBase + 3) == 3 &&
                   loadPhysLong(osVm, shellBase + 4) == 0,
                   "open syscall returns T1 success, fd payload, and clear detail");
            expect(loadPhysLong(osVm, shellBase + 5) == 1 &&
                   loadPhysLong(osVm, shellBase + 6) == 1,
                   "read syscall returns success and word count");
            expect(loadPhysLong(osVm, shellBase + 24) == 101,
                   "read syscall copies file data into the shell user buffer");
            expect(loadPhysLong(osVm, shellBase + 8) == 1 &&
                   loadPhysLong(osVm, shellBase + 9) == 2,
                   "write syscall returns success and written word count");
            expect(loadPhysLong(osVm, assembled.data_labels.at("os_file_words") + 3) == 404 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_file_words") + 4) == 505,
                   "write syscall copies shell user buffer words into the kernel file image");
            expect(loadPhysLong(osVm, shellBase + 11) == 1 &&
                   loadPhysLong(osVm, shellBase + 12) == 5,
                   "stat syscall reports updated file size");
            expect(loadPhysLong(osVm, shellBase + 14) == 1 &&
                   loadPhysLong(osVm, shellBase + 15) == 2,
                   "readdir syscall reports directory entry count");
            expect(loadPhysLong(osVm, shellBase + 25) == 47 &&
                   loadPhysLong(osVm, shellBase + 26) == 102,
                   "readdir syscall copies directory words into the shell user buffer");
            expect(loadPhysLong(osVm, shellBase + 17) == 1 &&
                   loadPhysLong(osVm, shellBase + 18) == 7,
                   "sbrk syscall grows the shell heap break");
            expect(loadPhysLong(osVm, shellBase + 20) == -1 &&
                   loadPhysLong(osVm, shellBase + 22) == 3,
                   "brk syscall rejects out-of-range heap break");
            expect(loadPhysLong(osVm, shellBase + 23) == 1,
                   "close syscall returns success");
            expect(loadPhysLong(osVm, assembled.data_labels.at("os_open_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_read_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_write_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_stat_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_readdir_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_close_count")) == 1 &&
                   loadPhysLong(osVm, assembled.data_labels.at("os_sbrk_count")) == 1,
                   "kernel accounts Trit OS routed file and heap syscalls");

            std::cerr << "DEBUG: OS sub-test 15 - bad path" << std::endl;
            VMState badPathVm(2048, 768);
            expect(loadAndReset(badPathVm, assembled), "Trit OS bad path image loads");
            badPathVm.enqueueConsoleAscii("n x");
            auto badPathResult = sandbox::vm::run(badPathVm, 12000);
            expect(badPathResult.timeout() && badPathVm.isRunning(),
                   "kernel keeps running after bad path open");
            expect(loadPhysLong(badPathVm, shellBase + 24) == -1 &&
                   loadPhysLong(badPathVm, shellBase + 25) == 0 &&
                   loadPhysLong(badPathVm, shellBase + 26) == 1,
                   "open syscall rejects missing user path with T1 error detail");
            expect(loadPhysLong(badPathVm, assembled.data_labels.at("os_open_count")) == 0,
                   "failed open is not counted as an opened file");

            std::cerr << "DEBUG: OS sub-test 15 - bad ptr" << std::endl;
            VMState badPtrVm(2048, 768);
            expect(loadAndReset(badPtrVm, assembled), "Trit OS bad pointer image loads");
            badPtrVm.enqueueConsoleAscii("v x");
            auto badPtrResult = sandbox::vm::run(badPtrVm, 12000);
            expect(badPtrResult.timeout() && badPtrVm.isRunning(),
                   "kernel keeps running after bad pointer open");
            expect(loadPhysLong(badPtrVm, shellBase + 24) == -1 &&
                   loadPhysLong(badPtrVm, shellBase + 25) == 0 &&
                   loadPhysLong(badPtrVm, shellBase + 26) == 5,
                   "open syscall rejects invalid user pointer span with T1 error detail");

            std::cerr << "DEBUG: OS sub-test 15 - fork" << std::endl;
            VMState forkVm(2048, 768);
            expect(loadAndReset(forkVm, assembled), "Trit OS fork image loads");
            forkVm.enqueueConsoleAscii("f");
            auto forkResult = sandbox::vm::run(forkVm, 12000);
            expect(forkResult.timeout() && forkVm.isRunning(),
                   "kernel keeps running after routed fork syscall");
            expect(loadPhysLong(forkVm, shellBase + 24) == 1 &&
                   loadPhysLong(forkVm, shellBase + 25) == 4 &&
                   loadPhysLong(forkVm, shellBase + 26) == 0,
                   "fork parent sees success and child pid payload");
            expect(loadPhysLong(forkVm, assembled.data_labels.at("proc_parent_pid") + 3) == 1 &&
                   loadPhysLong(forkVm, assembled.data_labels.at("proc_state") + 3) != PROC_STATE_FREE &&
                   loadPhysLong(forkVm, assembled.data_labels.at("proc_forks")) == 1,
                   "fork populates spare process metadata and accounting");
            expect(loadPhysLong(forkVm, assembled.data_labels.at("spare_data") + 24) == 1 &&
                   loadPhysLong(forkVm, assembled.data_labels.at("spare_data") + 25) == 0,
                   "fork child sees zero payload in copied user memory");

            std::cerr << "DEBUG: OS sub-test 15 - exec" << std::endl;
            VMState execVm(2048, 768);
            expect(loadAndReset(execVm, assembled), "Trit OS exec image loads");
            execVm.enqueueConsoleAscii("e");
            auto execResult = sandbox::vm::run(execVm, 20000);
            expect(execResult.timeout() && execVm.isRunning(),
                   "kernel keeps running after routed exec syscall");
            expect(loadPhysLong(execVm, assembled.data_labels.at("prog_b_counter")) == 1,
                   "exec replaces shell image with executable program B");
            expect(loadPhysLong(execVm, assembled.data_labels.at("proc_execs")) == 1 &&
                   loadPhysLong(execVm, assembled.data_labels.at("proc_exit_status")) == 22,
                   "exec accounting is recorded and executed image exits with status");
        }
    }

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
