#include "ternary_ir.h"
#include "ternary_vm.h"

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

bool hasDiagnostic(const std::vector<std::string>& diagnostics, const std::string& needle) {
    for (const std::string& diagnostic : diagnostics) {
        if (contains(diagnostic, needle)) return true;
    }
    return false;
}

long long scalarLong(const sandbox::vm::VMState& vm, sandbox::ir::Value value) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(value.reg)));
}

long long vectorLong(const sandbox::vm::VMState& vm, sandbox::ir::Value value, int lane) {
    return sandbox::vm::ops::toLong(vm.vregfile.reg[static_cast<std::size_t>(value.reg)].read(lane));
}

int8_t vectorPredicate(const sandbox::vm::VMState& vm, sandbox::ir::Value value, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(value.reg)].read(lane).asL1().tritAt(0);
}

bool loadAndRun(
    sandbox::vm::VMState& vm,
    const sandbox::ir::LowerResult& lowered,
    int maxSteps = 128) {

    if (!lowered.success) return false;
    if (!sandbox::vm::loadAndReset(vm, lowered.assembled.program)) return false;
    const auto result = sandbox::vm::run(vm, maxSteps);
    return result.halted();
}

sandbox::vm::TernaryValue l1Value(int8_t trit) {
    sandbox::TritLane1 lane;
    lane.setTrit(0, trit);
    return sandbox::vm::TernaryValue::fromL1(lane);
}

void testScalarMathAndAssembly() {
    std::cout << "[1] IR scalar math and assembly lowering\n";
    using namespace sandbox::ir;

    Program program;
    Value a = program.constant(Type::T20, 7);
    Value b = program.constant(Type::T20, 5);
    Value product = program.mul(a, b);
    Value delta = program.sub(a, b);
    Value result = program.add(product, delta);
    Value same = program.cvt(result, Type::T20);
    program.halt();

    auto lowered = program.lower();
    expect(lowered.success, "scalar IR assembles");
    expect(contains(lowered.assembly, "mov.t20"), "typed constants lower with suffix");
    expect(contains(lowered.assembly, "mul.t20"), "typed mul lowers with suffix");
    expect(contains(lowered.assembly, "add.t20"), "typed add lowers with suffix");
    expect(!contains(lowered.assembly, "add r"), "bare arithmetic is not emitted");
    expect(!contains(lowered.assembly, "cvt.t20.t20"), "no-op cvt is removed");
    expect(same.reg == result.reg, "no-op cvt returns same value");

    sandbox::vm::VMState vm(64, 64);
    expect(loadAndRun(vm, lowered), "scalar IR program runs");
    expect(scalarLong(vm, result) == 37, "scalar IR result value");
}

void testControlConversionAndMemory() {
    std::cout << "[2] IR tsel, cvt, branches, load/store\n";
    using namespace sandbox::ir;

    {
        Program program;
        Value left = program.constant(Type::T20, 3);
        Value right = program.constant(Type::T20, 1);
        Value cond = program.cmp(left, right);
        Value negArm = program.constant(Type::T10, -7);
        Value zeroArm = program.constant(Type::T10, 0);
        Value posArm = program.constant(Type::T10, 9);
        Value selected = program.tsel(cond, negArm, zeroArm, posArm);
        Value widened = program.cvt(selected, Type::T20);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "tsel/cvt IR assembles");
        expect(contains(lowered.assembly, "tcmp.t20"), "cmp emits typed compare");
        expect(contains(lowered.assembly, "tsel"), "tsel emits ternary select");
        expect(contains(lowered.assembly, "cvt.t10.t20"), "cvt emits source and destination suffix");

        sandbox::vm::VMState vm(64, 64);
        expect(loadAndRun(vm, lowered), "tsel/cvt program runs");
        expect(scalarLong(vm, widened) == 9, "tsel selected positive arm");
    }

    {
        Program program;
        Value base = program.constant(Type::T40, 20);
        Value value = program.constant(Type::T20, 42);
        program.store(value, base, 0);
        Value loaded = program.load(Type::T20, base, 0);
        Value one = program.constant(Type::T20, 1);
        Value result = program.add(loaded, one);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "load/store IR assembles");
        expect(contains(lowered.assembly, "store"), "store emitted");
        expect(contains(lowered.assembly, "load"), "load emitted");

        sandbox::vm::VMState vm(64, 64);
        expect(loadAndRun(vm, lowered), "load/store program runs");
        expect(scalarLong(vm, result) == 43, "load/store result value");
    }

    {
        Program program;
        Value zero = program.constant(Type::T20, 0);
        Value input = program.constant(Type::T20, 5);
        Value cond = program.cmp(input, zero);
        program.brp(cond, "positive");
        Value fail = program.constant(Type::T20, 999);
        program.jmp("done");
        program.label("positive");
        Value ok = program.constant(Type::T20, 123);
        program.label("done");
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "branch IR assembles");
        expect(contains(lowered.assembly, "brp"), "positive branch emitted");

        sandbox::vm::VMState vm(64, 64);
        expect(loadAndRun(vm, lowered), "branch program runs");
        expect(scalarLong(vm, ok) == 123, "branch reached positive block");
        expect(scalarLong(vm, fail) == 0, "branch skipped fail block");
    }
}

void testVectorAndAccumulatorOps() {
    std::cout << "[3] IR vector, accumulator, and T1 AI ops\n";
    using namespace sandbox::ir;

    {
        Program program;
        Value three = program.constant(Type::T20, 3);
        Value five = program.constant(Type::T20, 5);
        Value v3 = program.vbcast(Type::T20, three);
        Value v5 = program.vbcast(Type::T20, five);
        Value sum = program.vadd(v3, v5);
        Value pred = program.vcmp(v3, v5);
        Value selected = program.vsel(pred, v3, v5, sum);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "vector IR assembles");
        expect(contains(lowered.assembly, "vbcast.t20"), "vbcast emits suffix");
        expect(contains(lowered.assembly, "vadd.t20"), "vadd emits suffix");
        expect(contains(lowered.assembly, "vcmp.t20"), "vcmp emits suffix");
        expect(contains(lowered.assembly, "vsel.t20"), "vsel emits suffix");

        sandbox::vm::VMState vm(64, 64);
        vm.vector_length = 4;
        expect(loadAndRun(vm, lowered), "vector program runs");
        expect(vectorLong(vm, sum, 0) == 8, "vector add lane result");
        expect(vectorLong(vm, selected, 0) == 3, "vector select chose negative arm");
    }

    {
        Program program;
        Value weights = program.vparam(Type::L1);
        Value activations = program.vparam(Type::L1);
        Value signs = program.vparam(Type::T20);
        program.aclr(Type::T40);
        Value dot = program.vdotT1(weights, activations);
        program.vmacT1(weights, activations);
        Value acc = program.astore(Type::T40);
        Value activated = program.vactT1(signs);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "T1 AI IR assembles");
        expect(contains(lowered.assembly, "vdot.t1"), "vdot.t1 emitted");
        expect(contains(lowered.assembly, "vmac.t1"), "vmac.t1 emitted");
        expect(contains(lowered.assembly, "vact.t1"), "vact.t1 emitted");

        sandbox::vm::VMState vm(64, 64);
        vm.vector_length = 4;
        vm.coldReset();
        const int8_t a[] = {1, 1, 0, -1};
        const int8_t b[] = {1, -1, 1, -1};
        const long long s[] = {-5, 0, 7, -1};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[static_cast<std::size_t>(weights.reg)].write(lane, l1Value(a[lane]));
            vm.vregfile.reg[static_cast<std::size_t>(activations.reg)].write(lane, l1Value(b[lane]));
            vm.vregfile.reg[static_cast<std::size_t>(signs.reg)].write(
                lane,
                sandbox::vm::TernaryValue::fromT20(sandbox::native_ops::fromIntT20(s[lane])));
        }
        expect(vm.imem.loadProgram(lowered.assembled.program, 0), "T1 AI program loads");
        const auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "T1 AI program runs");
        expect(scalarLong(vm, dot) == 1, "vdot result");
        expect(scalarLong(vm, acc) == 1, "vmac accumulator result");
        expect(vectorPredicate(vm, activated, 0) == -1 &&
               vectorPredicate(vm, activated, 1) == 0 &&
               vectorPredicate(vm, activated, 2) == 1,
               "vact sign lanes");
    }
}

void testPhase2IsaIrOps() {
    std::cout << "[4] IR Phase 2 ISA ops\n";
    using namespace sandbox::ir;

    {
        Program program;
        Value value = program.constant(Type::T20, 11);
        Value low = program.constant(Type::T20, 3);
        Value high = program.constant(Type::T20, 9);
        Value four = program.constant(Type::T20, 4);
        Value one = program.constant(Type::T20, 1);
        Value scanSubject = program.constant(Type::T5, 9);
        Value window = program.twcmp(value, low, high);
        Value clamped = program.tclamp(value, low, high);
        Value rem = program.tmod(value, four);
        Value left = program.tlshift(value, one);
        Value right = program.trshift(left, one);
        Value count = program.tcount(scanSubject);
        Value scan = program.tscan(scanSubject);
        program.aclr(Type::T40);
        program.tmac(value, four);
        Value acc = program.astore(Type::T40);
        program.syscall(3);
        program.syscall(1);
        program.fence();
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "Phase 2 scalar IR assembles");
        expect(contains(lowered.assembly, "twcmp.t20"), "twcmp emitted");
        expect(contains(lowered.assembly, "tclamp.t20"), "tclamp emitted");
        expect(contains(lowered.assembly, "tmod.t20"), "tmod emitted");
        expect(contains(lowered.assembly, "tlshift.t20"), "tlshift emitted");
        expect(contains(lowered.assembly, "trshift.t20"), "trshift emitted");
        expect(contains(lowered.assembly, "tmac.t20"), "tmac emitted");
        expect(contains(lowered.assembly, "tcount.t5"), "tcount emitted");
        expect(contains(lowered.assembly, "tscan.t5"), "tscan emitted");
        expect(contains(lowered.assembly, "syscall 1"), "syscall emitted");
        expect(contains(lowered.assembly, "fence"), "fence emitted");

        sandbox::vm::VMState vm(128, 64);
        expect(loadAndRun(vm, lowered), "Phase 2 scalar IR program runs");
        expect(scalarLong(vm, window) == 1, "IR twcmp above-window result");
        expect(scalarLong(vm, clamped) == 9, "IR tclamp high result");
        expect(scalarLong(vm, rem) == 3, "IR tmod result");
        expect(scalarLong(vm, left) == 33, "IR tlshift result");
        expect(scalarLong(vm, right) == 11, "IR trshift result");
        expect(scalarLong(vm, count) == 1, "IR tcount result");
        expect(scalarLong(vm, scan) == 2, "IR tscan result");
        expect(scalarLong(vm, acc) == 44, "IR tmac accumulator result");
        expect(vm.syscall_buffer == "11", "IR syscall wrote value from r1");
    }

    {
        Program program;
        Value vector = program.vparam(Type::T20);
        Value sum = program.vsum(vector);
        Value min = program.vhmin(vector);
        Value max = program.vhmax(vector);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "Phase 2 vector reduction IR assembles");
        expect(contains(lowered.assembly, "vsum.t20"), "vsum emitted");
        expect(contains(lowered.assembly, "vhmin.t20"), "vhmin emitted");
        expect(contains(lowered.assembly, "vhmax.t20"), "vhmax emitted");

        sandbox::vm::VMState vm(64, 64);
        vm.vector_length = 3;
        vm.coldReset();
        vm.vector_length = 3;
        vm.vregfile.reset(3);
        vm.vector_faults.reset(3);
        const long long values[] = {3, -2, 5};
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[static_cast<std::size_t>(vector.reg)].write(
                lane,
                sandbox::vm::TernaryValue::fromT20(sandbox::native_ops::fromIntT20(values[lane])));
        }
        expect(vm.imem.loadProgram(lowered.assembled.program, 0), "vector reduction IR program loads");
        const auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "vector reduction IR program runs");
        expect(scalarLong(vm, sum) == 6, "IR vsum result");
        expect(scalarLong(vm, min) == -2, "IR vhmin result");
        expect(scalarLong(vm, max) == 5, "IR vhmax result");
    }

    {
        Program program;
        Value target = program.constant(Type::T40, 6);
        program.callr(target);
        program.jmpr(target);
        program.halt();

        auto lowered = program.lower();
        expect(lowered.success, "indirect control IR assembles");
        expect(contains(lowered.assembly, "callr"), "callr emitted");
        expect(contains(lowered.assembly, "jmpr"), "jmpr emitted");
    }
}

void testDiagnosticsAndRegisterExhaustion() {
    std::cout << "[5] IR diagnostics and register exhaustion\n";
    using namespace sandbox::ir;

    {
        Program program;
        for (int i = 0; i < 25; ++i) {
            (void)program.constant(Type::T20, i);
        }
        auto lowered = program.lower();
        expect(!lowered.success, "register exhaustion fails lowering");
        expect(hasDiagnostic(lowered.diagnostics, "scalar register exhausted"),
               "register exhaustion diagnostic is clear");
    }

    {
        Program program;
        Value a = program.constant(Type::T20, 1);
        Value b = program.constant(Type::T10, 2);
        (void)program.add(a, b);
        auto lowered = program.lower();
        expect(!lowered.success, "type mismatch fails lowering");
        expect(hasDiagnostic(lowered.diagnostics, "matching numeric scalar"),
               "type mismatch diagnostic is clear");
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testScalarMathAndAssembly();
    testControlConversionAndMemory();
    testVectorAndAccumulatorOps();
    testPhase2IsaIrOps();
    testDiagnosticsAndRegisterExhaustion();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " ternary IR test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll ternary IR tests passed\n";
    return EXIT_SUCCESS;
}
