#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << message << "\n";
}

bool hasDiagnostic(
    const std::vector<sandbox::compiler::Diagnostic>& diagnostics,
    const std::string& needle) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos ||
            diagnostic.format().find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

long long runReturn(const sandbox::compiler::CompileResult& compiled,
                    sandbox::compiler::LinkOptions options = {}) {
    using namespace sandbox::compiler;
    const LinkResult linked = linkModules({compiled.object}, options);
    expect(linked.success, "compiler ABI contract image links");
    if (!linked.success) return -999999;
    sandbox::vm::VMState vm(4096, 4096);
    vm.setExecutionBackend(sandbox::vm::VMExecutionBackend::Interpreter);
    expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
           "compiler ABI contract image loads");
    const auto result = sandbox::vm::run(vm, 1000000);
    expect(result.halted(), "compiler ABI contract image halts");
    return sandbox::vm::ops::toLong(vm.regfile.read(13));
}

void testAggregatePointerAbiAcrossRegistersAndStack() {
    using namespace sandbox::compiler;
    const std::string source = R"TRIT(
        struct Pair { a: t40; b: t40; }
        fn combine(a: Pair, bias: t40, b: Pair, tail: t40,
                   c: Pair, extra: t40, d: Pair, last: t40) -> t40 {
            return a.a + a.b + bias + b.a + b.b + tail +
                   c.a + c.b + extra + d.a + d.b + last;
        }
        fn main() -> t40 {
            var left: Pair = Pair { a: 1, b: 2 };
            var right: Pair = Pair { a: 3, b: 4 };
            var third: Pair = Pair { a: 5, b: 6 };
            var fourth: Pair = Pair { a: 7, b: 8 };
            return combine(left, 9, right, 10, third, 11, fourth, 12);
        }
    )TRIT";
    for (const OptimizationLevel level :
         {OptimizationLevel::None, OptimizationLevel::Basic}) {
        CompilerOptions options;
        options.optimization = level;
        const CompileResult compiled =
            compileSource("aggregate_pointer_abi.trit", source, options);
        expect(compiled.success,
               "aggregate pointer ABI compiles in optimized and unoptimized modes");
        if (!compiled.success) continue;
        expect(compiled.object.metadata.at("target.ast_replay_functions") == "0",
               "aggregate pointer ABI emits solely from optimized SSA IR");
        expect(runReturn(compiled) == 78,
               "aggregate pointer ABI preserves following register and stack arguments");
    }
}

void testLiveAliasCarrierAcrossCall() {
    using namespace sandbox::compiler;
    const std::string source = R"TRIT(
        fn increment(x: t40) -> t40 { return x + 1; }
        fn main() -> t40 {
            var raw: t40 = 120;
            unsafe { store(raw, 41); }
            let delta = increment(1);
            unsafe { return load(raw) + delta; }
        }
    )TRIT";
    const CompileResult compiled = compileSource("alias_call_carrier.trit", source);
    expect(compiled.success, "live external address carrier across call compiles");
    if (!compiled.success) return;
    expect(compiled.object.metadata.at("target.memory_alias_model") ==
               "ordered-effects-with-call-carrier-proof",
           "target records the ordered alias/call-carrier proof");
    expect(compiled.object.metadata.at("target.caller_saved_live_across_calls") == "0",
           "live alias carrier is not allocated in a caller-saved register");
    expect(runReturn(compiled) == 43,
           "call clobbering preserves the live external address and ordered load");
}

void testVersionedFunctionAbiContract() {
    using namespace sandbox::compiler;
    const CompileResult scalar = compileSource(
        "versioned_scalar_abi.trit",
        "fn main() -> t40 { return 7; }");
    expect(scalar.success, "versioned scalar ABI contract compiles");
    if (!scalar.success) return;
    expect(scalar.object.metadata.at("target.function_abi_contract") ==
               FunctionAbiContract::id(),
           "object records the versioned function ABI contract id");
    expect(scalar.object.metadata.at("target.function_abi_version") ==
               std::to_string(FunctionAbiContract::version),
           "object records the function ABI version");
    expect(scalar.object.metadata.at("target.aggregate_parameter_abi") ==
               FunctionAbiContract::aggregateParameter(),
           "object records one-word caller-owned aggregate parameters");
    expect(scalar.object.metadata.at("target.aggregate_return_abi") ==
               FunctionAbiContract::aggregateReturn(),
           "object records aggregate returns as unsupported");
    expect(scalar.object.metadata.at("target.vector_boundary_abi") ==
               FunctionAbiContract::vectorBoundary(),
           "object records first-class vector boundaries as unsupported");

    LinkOptions bad_link;
    bad_link.function_abi_version = FunctionAbiContract::version - 1;
    const LinkResult rejected_link = linkModules({scalar.object}, bad_link);
    expect(!rejected_link.success,
           "linker rejects an object/image function ABI version mismatch");
    expect(hasDiagnostic(rejected_link.diagnostics,
                         "unsupported function ABI version"),
           "linker ABI version diagnostic is explicit");

    CompilerOptions bad_options;
    bad_options.target_abi_version = FunctionAbiContract::version - 1;
    const CompileResult rejected_compile = compileSource(
        "unsupported_function_abi.trit",
        "fn main() -> t40 { return 0; }", bad_options);
    expect(!rejected_compile.success,
           "compiler rejects an unsupported function ABI version");
    expect(hasDiagnostic(rejected_compile.diagnostics,
                         "unsupported function ABI version"),
           "compiler ABI version diagnostic is explicit");
}

void testV3AggregateSretBoundary() {
    using namespace sandbox::compiler;
    const std::string source = R"TRIT(
        struct Pair { a: t40; b: t40; }
        fn make_pair(seed: t40) -> Pair {
            var value: Pair = Pair { a: seed, b: seed + 1 };
            return value;
        }
        fn main() -> t40 {
            var result: Pair = make_pair(40);
            return result.a + result.b;
        }
    )TRIT";
    CompilerOptions options;
    options.target_abi_version = FunctionAbiContract::version_v3;
    const CompileResult compiled = compileSource(
        "aggregate_sret_v3.trit", source, options);
    expect(compiled.success,
           "ABI v3 aggregate-return source compiles with caller-owned sret");
    if (!compiled.success) return;
    expect(compiled.object.metadata.at("target.function_abi_contract") ==
               FunctionAbiContract::idForVersion(
                   FunctionAbiContract::version_v3),
           "ABI v3 object records its versioned function contract");
    expect(compiled.object.metadata.at("target.aggregate_return_abi") ==
               FunctionAbiContract::aggregateReturnForVersion(
                   FunctionAbiContract::version_v3),
           "ABI v3 object records first-word sret representation");
    bool saw_hidden_sret = false;
    bool saw_aggregate_call = false;
    for (const auto& function : compiled.object.ssa.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Param &&
                    instr.symbol == "$sret") {
                    saw_hidden_sret = true;
                }
                if (instr.opcode == InstrOpcode::Call &&
                    instr.symbol == "make_pair" &&
                    instr.type.kind == TypeKind::Struct) {
                    saw_aggregate_call = instr.def < 0 &&
                        !instr.args.empty();
                }
            }
        }
    }
    expect(saw_hidden_sret,
           "ABI v3 SSA exposes the hidden aggregate sret parameter");
    expect(saw_aggregate_call,
           "ABI v3 SSA models aggregate calls as side-effecting sret writes");
    LinkOptions link_options;
    link_options.function_abi_version = FunctionAbiContract::version_v3;
    const LinkResult linked = linkModules({compiled.object}, link_options);
    expect(linked.success,
           "ABI v3 object links when the matching compiler profile is selected");
    expect(linked.function_abi_version == FunctionAbiContract::version_v3 &&
               linked.function_abi_contract ==
                   FunctionAbiContract::idForVersion(
                       FunctionAbiContract::version_v3),
           "link result preserves the selected ABI v3 profile");
    if (linked.success) {
        const long long result = runReturn(compiled, link_options);
        expect(result == 81,
               "ABI v3 sret caller and callee preserve aggregate payload");
    }

    const std::string shifted_source = R"TRIT(
        struct Pair { a: t40; b: t40; }
        fn make_pair(seed: t40) -> Pair {
            var value: Pair = Pair { a: seed, b: seed + 1 };
            return value;
        }
        fn combine(base: Pair, p1: Pair, a: t40, p2: Pair, b: t40,
                   p3: Pair, c: t40) -> Pair {
            var nested: Pair = make_pair(a);
            var result: Pair = Pair {
                a: base.a + nested.a + p1.a + p2.a + p3.a + c,
                b: base.b + nested.b + p1.b + p2.b + p3.b + c + b
            };
            return result;
        }
        fn main() -> t40 {
            var base: Pair = Pair { a: 1, b: 2 };
            var p1: Pair = Pair { a: 3, b: 4 };
            var p2: Pair = Pair { a: 5, b: 6 };
            var p3: Pair = Pair { a: 7, b: 8 };
            var result: Pair = combine(base, p1, 2, p2, 3, p3, 4);
            return result.a + result.b;
        }
    )TRIT";
    const CompileResult shifted = compileSource(
        "aggregate_sret_v3_shifted.trit", shifted_source, options);
    expect(shifted.success,
           "ABI v3 shifts hidden sret and aggregate arguments across the stack");
    if (shifted.success) {
        expect(runReturn(shifted, link_options) == 52,
               "ABI v3 nested sret calls preserve register and stack words");
    }

    LinkOptions mismatched;
    mismatched.function_abi_version = FunctionAbiContract::version_v2;
    const LinkResult rejected = linkModules({compiled.object}, mismatched);
    expect(!rejected.success,
           "linker rejects mixing an ABI v3 object into the v2 profile");
    expect(hasDiagnostic(rejected.diagnostics,
                         "declares function ABI version 3"),
           "v3/v2 link rejection identifies the object profile mismatch");
}

void testUnsupportedValuesFailClosed() {
    using namespace sandbox::compiler;
    const CompileResult aggregate_return = compileSource(
        "aggregate_return_rejected.trit", R"TRIT(
            struct Pair { a: t40; b: t40; }
            fn make_pair() -> Pair {
                var value: Pair = Pair { a: 1, b: 2 };
                return value;
            }
            fn main() -> t40 { return 0; }
        )TRIT");
    expect(!aggregate_return.success,
           "aggregate-valued return fails closed under function ABI v2");
    expect(hasDiagnostic(aggregate_return.diagnostics,
                         "aggregate-valued function return has no ABI v2 representation"),
           "aggregate-return rejection names the missing ABI representation");

    const CompileResult vector_value = compileSource(
        "vector_abi_rejected.trit", R"TRIT(
            fn preserve(value: vec<t20>) -> vec<t20> { return value; }
            fn main() -> t40 { return 0; }
        )TRIT");
    expect(!vector_value.success,
           "first-class vector function boundary fails closed under ABI v2");
    expect(hasDiagnostic(vector_value.diagnostics,
                         "authoritative vector call/return ABI"),
           "vector rejection names the missing call/return ABI");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    testAggregatePointerAbiAcrossRegistersAndStack();
    testLiveAliasCarrierAcrossCall();
    testVersionedFunctionAbiContract();
    testV3AggregateSretBoundary();
    testUnsupportedValuesFailClosed();
    if (failures != 0) {
        std::cout << failures << " compiler ABI contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compiler ABI and alias contracts passed\n";
    return EXIT_SUCCESS;
}
