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

long long runReturn(const sandbox::compiler::CompileResult& compiled) {
    using namespace sandbox::compiler;
    const LinkResult linked = linkModules({compiled.object});
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
    testUnsupportedValuesFailClosed();
    if (failures != 0) {
        std::cout << failures << " compiler ABI contract failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Compiler ABI and alias contracts passed\n";
    return EXIT_SUCCESS;
}
