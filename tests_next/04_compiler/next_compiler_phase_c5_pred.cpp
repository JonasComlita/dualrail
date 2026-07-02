#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::compiler;

struct GoldenCase {
    std::string name;
    std::string source;
    std::string expected_output;
    int imem_words = 4096;
    int dmem_words = 4096;
    int steps = 4096;
};

std::string formatDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(
        vm.regfile.read(static_cast<uint8_t>(reg)));
}

std::string readTritFile(const std::string& name) {
    for (const std::string& candidate : {name, "../" + name, "../../" + name}) {
        std::ifstream file(candidate);
        if (!file.is_open()) continue;
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }
    return "";
}

std::string runBootstrapProducer(TestContext& ctx,
                                 const GoldenCase& test_case,
                                 OptimizationLevel optimization,
                                 const std::string& producer_name) {
    CompilerOptions options;
    options.optimization = optimization;
    CompileResult compiled = compileSource(
        "next_c5_" + test_case.name + ".trit", test_case.source, options);
    if (!compiled.success) {
        ctx.fail(producer_name + " compile diagnostics for " + test_case.name +
                 ":\n" + formatDiagnostics(compiled.diagnostics));
        return "";
    }

    LinkResult linked = linkModules({compiled.object});
    if (!linked.success) {
        ctx.fail(producer_name + " link diagnostics for " + test_case.name +
                 ":\n" + formatDiagnostics(linked.diagnostics));
        return "";
    }

    sandbox::vm::VMState vm(test_case.imem_words, test_case.dmem_words);
    if (!sandbox::vm::loadAndReset(vm, linked.assembled.program)) {
        ctx.fail(producer_name + " image loads for " + test_case.name);
        return "";
    }

    const auto result = sandbox::vm::run(vm, test_case.steps);
    if (!result.halted()) {
        std::ostringstream out;
        out << producer_name << " runtime did not halt for " << test_case.name
            << "; status=" << static_cast<int>(result.status)
            << " pc=" << vm.pc
            << " r13=" << regLong(vm, 13)
            << " buffer='" << vm.syscall_buffer << "'";
        ctx.fail(out.str());
        return vm.syscall_buffer;
    }

    ctx.equal(vm.syscall_buffer, test_case.expected_output,
              producer_name + " output matches oracle for " + test_case.name);
    return vm.syscall_buffer;
}

void runO0O1Golden(TestContext& ctx, const GoldenCase& test_case) {
    const std::string o0 = runBootstrapProducer(
        ctx, test_case, OptimizationLevel::None, "bootstrap-O0");
    const std::string o1 = runBootstrapProducer(
        ctx, test_case, OptimizationLevel::Basic, "bootstrap-O1");
    ctx.equal(o0, o1, "O0 and O1 outputs agree for " + test_case.name);
}

void bootstrapArithmetic(TestContext& ctx) {
    runO0O1Golden(ctx, GoldenCase{
        "arithmetic",
        R"(
            fn main() -> t40 {
                sys_write_int((2 + 3) * 4 - 6);
                sys_newline();
                return 0;
            }
        )",
        "14\n"});
}

void bootstrapRecursion(TestContext& ctx) {
    runO0O1Golden(ctx, GoldenCase{
        "recursion",
        R"(
            fn fact(n: t40) -> t40 {
                match n - 1 {
                    neg => { return 1; }
                    zero => { return 1; }
                    pos => { return n * fact(n - 1); }
                }
            }
            fn main() -> t40 {
                sys_write_int(fact(5));
                sys_newline();
                return 0;
            }
        )",
        "120\n", 4096, 4096, 12000});
}

void bootstrapStructAndArray(TestContext& ctx) {
    runO0O1Golden(ctx, GoldenCase{
        "struct_param",
        R"(
            struct Pair { a: t40; b: t40; }
            fn sum(p: Pair) -> t40 { return p.a + p.b; }
            fn main() -> t40 {
                var p: Pair = Pair { a: 6, b: 7 };
                sys_write_int(sum(p));
                sys_newline();
                return 0;
            }
        )",
        "13\n"});

    runO0O1Golden(ctx, GoldenCase{
        "array_update",
        R"(
            fn main() -> t40 {
                var xs: [t40; 4] = [3, 1, 4, 1];
                xs[1] = xs[0] + xs[2];
                sys_write_int(xs[1]);
                sys_newline();
                return 0;
            }
        )",
        "7\n"});
}

void bootstrapAtomicWord(TestContext& ctx) {
    runO0O1Golden(ctx, GoldenCase{
        "atomic_word",
        R"(
            fn main() -> t40 {
                let counter: shared<t40, ACQ_REL> = shared_alloc(5);
                let value: t40 = atomic_load(counter, ACQ_REL);
                atomic_store(counter, value + 2, ACQ_REL);
                sys_write_int(atomic_load(counter, ACQ_REL));
                sys_newline();
                return 0;
            }
        )",
        "7\n", 4096, 4096, 12000});
}

void bootstrapPointerStateMatch(TestContext& ctx) {
    runO0O1Golden(ctx, GoldenCase{
        "pointer_state_match",
        R"(
            fn main() -> t40 {
                unsafe { store(40, 88); }
                let p: ptr<t40, unknown> = 40;
                match p {
                    null => { sys_write_int(0); }
                    unknown => { sys_write_int(1); }
                    valid(q) => { sys_write_int(*q); }
                }
                sys_newline();
                return 0;
            }
        )",
        "88\n"});
}

void bootstrapUlibMiniPrintString(TestContext& ctx) {
    const std::string ulib_mini = readTritFile("ulib_mini.trit");
    ctx.check(!ulib_mini.empty(), "ulib_mini.trit loads");
    if (ulib_mini.empty()) return;

    runO0O1Golden(ctx, GoldenCase{
        "ulib_mini_print_string",
        ulib_mini + R"(
            fn main() -> t40 {
                unsafe {
                    store(100, 72);
                    store(101, 105);
                    store(102, 10);
                    store(103, 0);
                }
                print_string(100);
                return 0;
            }
        )",
        "Hi\n", 65536, 1000000, 50000});
}

void bootstrapUlibMiniVector(TestContext& ctx) {
    const std::string ulib_mini = readTritFile("ulib_mini.trit");
    ctx.check(!ulib_mini.empty(), "ulib_mini.trit loads");
    if (ulib_mini.empty()) return;

    runO0O1Golden(ctx, GoldenCase{
        "ulib_mini_vector",
        ulib_mini + R"(
            fn main() -> t40 {
                var v: t40 = vec_new();
                vec_push(v, 3);
                vec_push(v, 42);
                sys_write_int(vec_get(v, 1));
                sys_newline();
                vec_free(v);
                return 0;
            }
        )",
        "42\n", 65536, 1000000, 50000});
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"compiler.phase_c5.bootstrap_arithmetic_o0_o1",
         "compiler.pipeline_contract", bootstrapArithmetic},
        {"compiler.phase_c5.bootstrap_recursion_o0_o1",
         "compiler.pipeline_contract", bootstrapRecursion},
        {"compiler.phase_c5.bootstrap_struct_array_o0_o1",
         "compiler.pipeline_contract", bootstrapStructAndArray},
        {"compiler.phase_c5.bootstrap_atomic_word_o0_o1",
         "compiler.pipeline_contract", bootstrapAtomicWord},
        {"compiler.phase_c5.bootstrap_pointer_state_o0_o1",
         "compiler.pipeline_contract", bootstrapPointerStateMatch},
        {"compiler.phase_c5.ulib_mini_print_string_o0_o1",
         "compiler.pipeline_contract", bootstrapUlibMiniPrintString},
        {"compiler.phase_c5.ulib_mini_vector_o0_o1",
         "compiler.pipeline_contract", bootstrapUlibMiniVector},
    };
    return tests_next::runCases("next_compiler_phase_c5_pred", cases);
}
