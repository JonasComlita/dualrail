#include "test_multiwidth_vm_common.h"
#include "ternary_compiler.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>

int g_failures = 0;

namespace {

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

std::string readRepoText(const std::string& name) {
    for (const std::string& path : {name, "../" + name, "../../" + name}) {
        std::string text = readTextFile(path);
        if (!text.empty()) return text;
    }
    return {};
}

std::string formatDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << "  " << diagnostic.format() << "\n";
    }
    return out.str();
}

struct GoldenCase {
    std::string name;
    std::string source;
    std::string expected_output;
    int imem_words = 4096;
    int dmem_words = 4096;
    int steps = 4096;
};

constexpr int kNativeSourceLenAddr = 1599999;
constexpr int kNativeSourceBase = 1600000;

std::string runBootstrapCase(const GoldenCase& test_case,
                             sandbox::compiler::OptimizationLevel optimization,
                             const std::string& producer_name) {
    using namespace sandbox::compiler;

    CompilerOptions options;
    options.optimization = optimization;
    CompileResult compiled = compileSource(test_case.name + ".trit", test_case.source, options);
    if (!compiled.success) {
        std::cout << "COMPILE FAIL [" << producer_name << "] " << test_case.name << "\n"
                  << formatDiagnostics(compiled.diagnostics);
        expect(false, producer_name + " compiles " + test_case.name);
        return {};
    }

    LinkResult linked = linkModules({compiled.object});
    if (!linked.success) {
        std::cout << "LINK FAIL [" << producer_name << "] " << test_case.name << "\n"
                  << formatDiagnostics(linked.diagnostics);
        expect(false, producer_name + " links " + test_case.name);
        return {};
    }

    sandbox::vm::VMState vm(test_case.imem_words, test_case.dmem_words);
    if (!sandbox::vm::loadAndReset(vm, linked.assembled.program)) {
        expect(false, producer_name + " loads " + test_case.name);
        return {};
    }

    const int native_steps = std::max(test_case.steps, 50000);
    const auto result = sandbox::vm::run(vm, native_steps);
    if (!result.halted()) {
        std::cout << "RUN FAIL [" << producer_name << "] " << test_case.name
                  << " status=" << static_cast<int>(result.status)
                  << " pc=" << vm.pc
                  << " r13=" << regLong(vm, 13)
                  << " buffer='" << vm.syscall_buffer << "'\n";
        expect(false, producer_name + " halts " + test_case.name);
        return vm.syscall_buffer;
    }

    expect(vm.syscall_buffer == test_case.expected_output,
           producer_name + " output matches " + test_case.name);
    return vm.syscall_buffer;
}

std::string nativeCompilerHarnessSource() {
    const std::string driver = R"(
        const C5_SOURCE_LEN_ADDR: t40 = 1599999;
        const C5_SOURCE_BASE: t40 = 1600000;

        fn main() -> t40 {
            var len: t40 = 0;
            unsafe { len = load(C5_SOURCE_LEN_ADDR); }
            var text: t40 = tcl_native_compile_to_tasm(C5_SOURCE_BASE, len);
            var n: t40 = vec_len(text);
            var i: t40 = 0;
            while n - i > 0 {
                sys_write_char(vec_get(text, i));
                i = i + 1;
            }
            tcl_backend_text_free(text);
            return 0;
        }
    )";

    return readRepoText("ulib.trit") + "\n" +
           readRepoText("tcl_token.trit") + "\n" +
           readRepoText("tcl_lexer.trit") + "\n" +
           readRepoText("tcl_ast.trit") + "\n" +
           readRepoText("tcl_type.trit") + "\n" +
           readRepoText("tcl_parser.trit") + "\n" +
           readRepoText("tcl_ir.trit") + "\n" +
           readRepoText("tcl_backend.trit") + "\n" +
           driver;
}

const sandbox::compiler::LinkResult& nativeCompilerImage() {
    using namespace sandbox::compiler;

    static LinkResult linked;
    static bool built = false;
    if (built) return linked;
    built = true;

    CompilerOptions options;
    options.optimization = OptimizationLevel::None;
    const std::string source = nativeCompilerHarnessSource();
    CompileResult compiled = compileSource("phase_c5_native_compiler.trit", source, options);
    if (!compiled.success) {
        std::cout << "COMPILE FAIL [native-compiler-harness]\n"
                  << formatDiagnostics(compiled.diagnostics);
        expect(false, "native compiler harness compiles");
        return linked;
    }

    linked = linkModules({compiled.object});
    if (!linked.success) {
        std::cout << "LINK FAIL [native-compiler-harness]\n"
                  << formatDiagnostics(linked.diagnostics);
        expect(false, "native compiler harness links");
    }
    return linked;
}

bool seedNativeSource(sandbox::vm::VMState& vm, const std::string& source) {
    if (kNativeSourceBase + static_cast<int>(source.size()) + 1 >= vm.dmem.size()) {
        return false;
    }
    if (vm.dmem.store(kNativeSourceLenAddr,
                      sandbox::vm::ops::fromLong(static_cast<long long>(source.size()))) !=
        sandbox::vm::MemFaultCode::OK) {
        return false;
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (vm.dmem.store(kNativeSourceBase + static_cast<int>(i),
                          sandbox::vm::ops::fromLong(static_cast<unsigned char>(source[i]))) !=
            sandbox::vm::MemFaultCode::OK) {
            return false;
        }
    }
    return vm.dmem.store(kNativeSourceBase + static_cast<int>(source.size()),
                         sandbox::vm::ops::fromLong(0)) == sandbox::vm::MemFaultCode::OK;
}

std::string runNativeCompilerCase(const GoldenCase& test_case,
                                  const std::string& bootstrap_output) {
    const sandbox::compiler::LinkResult& native_image = nativeCompilerImage();
    if (!native_image.success) return {};

    sandbox::vm::VMState compiler_vm(262144, 2097152);
    if (!sandbox::vm::loadAndReset(compiler_vm, native_image.assembled.program)) {
        expect(false, "native compiler image loads for " + test_case.name);
        return {};
    }
    if (!seedNativeSource(compiler_vm, test_case.source)) {
        expect(false, "native compiler source seeds for " + test_case.name);
        return {};
    }

    const auto compile_run = sandbox::vm::run(compiler_vm, 20000000);
    if (!compile_run.halted()) {
        std::string faulting;
        if (compiler_vm.pc >= 0 && compiler_vm.pc < compiler_vm.imem.size()) {
            faulting = sandbox::isa::disassemble(compiler_vm.imem.words[compiler_vm.pc]);
        }
        std::string nearest_label;
        int nearest_label_pc = -1;
        for (const auto& [label, pc] : native_image.assembled.labels) {
            if (pc <= compiler_vm.pc && pc > nearest_label_pc) {
                nearest_label = label;
                nearest_label_pc = pc;
            }
        }
        std::cout << "NATIVE COMPILE RUN FAIL " << test_case.name
                  << " status=" << static_cast<int>(compile_run.status)
                  << " pc=" << compiler_vm.pc
                  << " label=" << nearest_label << "+" << (compiler_vm.pc - nearest_label_pc)
                  << " r13=" << regLong(compiler_vm, 13)
                  << " r19=" << regLong(compiler_vm, 19)
                  << " r20=" << regLong(compiler_vm, 20)
                  << " sp=" << regLong(compiler_vm, 26)
                  << " instr='" << faulting << "'"
                  << " emitted_prefix='" << compiler_vm.syscall_buffer.substr(0, 400) << "'\n";
        const int start_pc = std::max(0, compiler_vm.pc - 5);
        const int end_pc = std::min(compiler_vm.imem.size(), compiler_vm.pc + 6);
        for (int pc = start_pc; pc < end_pc; ++pc) {
            std::cout << "  " << pc << ": "
                      << sandbox::isa::disassemble(compiler_vm.imem.words[pc]) << "\n";
        }
        expect(false, "native compiler halts for " + test_case.name);
        return {};
    }

    const std::string native_assembly = compiler_vm.syscall_buffer;
    auto assembled = sandbox::vm::assembler::assemble(native_assembly);
    if (!assembled.success) {
        std::cout << "NATIVE ASSEMBLY FAIL " << test_case.name << "\n";
        for (const auto& error : assembled.errors) {
            std::cout << "  " << error.format() << "\n";
        }
        std::cout << native_assembly.substr(0, 2000) << "\n";
        expect(false, "native compiler assembly assembles for " + test_case.name);
        return {};
    }

    sandbox::vm::VMState vm(test_case.imem_words, test_case.dmem_words);
    if (!sandbox::vm::assembler::loadAndReset(vm, assembled)) {
        expect(false, "native compiler image loads for " + test_case.name);
        return {};
    }
    const auto result = sandbox::vm::run(vm, test_case.steps);
    if (!result.halted()) {
        std::cout << "RUN FAIL [native] " << test_case.name
                  << " status=" << static_cast<int>(result.status)
                  << " pc=" << vm.pc
                  << " r13=" << regLong(vm, 13)
                  << " buffer='" << vm.syscall_buffer << "'\n";
        std::cout << native_assembly.substr(0, 3000) << "\n";
        expect(false, "native compiler output halts " + test_case.name);
        return vm.syscall_buffer;
    }

    if (vm.syscall_buffer != bootstrap_output || vm.syscall_buffer != test_case.expected_output) {
        std::cout << "NATIVE OUTPUT MISMATCH " << test_case.name
                  << " got='" << vm.syscall_buffer
                  << "' bootstrap='" << bootstrap_output
                  << "' expected='" << test_case.expected_output << "'\n";
    }
    expect(vm.syscall_buffer == bootstrap_output,
           "native compiler output equals bootstrap for " + test_case.name);
    expect(vm.syscall_buffer == test_case.expected_output,
           "native compiler output matches oracle for " + test_case.name);
    return vm.syscall_buffer;
}

void testCompilerGoldenPrograms() {
    std::cout << "[C.5] Pre-D compiler golden program suite\n";

    const std::string ulib = readRepoText("ulib.trit");
    expect(!ulib.empty(), "ulib.trit is available for C.5 golden cases");

    std::vector<GoldenCase> cases = {
        {"arithmetic",
         R"(
            fn main() -> t40 {
                sys_write_int((2 + 3) * 4 - 6);
                sys_newline();
                return 0;
            }
         )",
         "14\n"},
        {"while_sum",
         R"(
            fn main() -> t40 {
                var i: t40 = 1;
                var sum: t40 = 0;
                while 6 - i > 0 {
                    sum = sum + i;
                    i = i + 1;
                }
                sys_write_int(sum);
                sys_newline();
                return 0;
            }
         )",
         "15\n"},
        {"recursion",
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
         "120\n", 4096, 4096, 12000},
        {"match_arms",
         R"(
            fn emit(x: t40) -> t40 {
                match x {
                    neg => { sys_write_char(78); }
                    zero => { sys_write_char(90); }
                    pos => { sys_write_char(80); }
                }
                return 0;
            }
            fn main() -> t40 {
                emit(0 - 1);
                emit(0);
                emit(1);
                sys_write_char(10);
                return 0;
            }
         )",
         "NZP\n"},
        {"function_args",
         R"(
            fn mix(a: t40, b: t40, c: t40) -> t40 {
                return a * 100 + b * 10 + c;
            }
            fn main() -> t40 {
                sys_write_int(mix(1, 2, 3));
                sys_newline();
                return 0;
            }
         )",
         "123\n"},
        {"struct_param",
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
         "13\n"},
        {"array_update",
         R"(
            fn main() -> t40 {
                var xs: [t40; 4] = [3, 1, 4, 1];
                xs[1] = xs[0] + xs[2];
                sys_write_int(xs[1]);
                sys_newline();
                return 0;
            }
         )",
         "7\n"},
        {"constants",
         R"(
            const BASE: t40 = 5 * 2;
            fn main() -> t40 {
                const VALUE: t40 = BASE + 1;
                sys_write_int(VALUE);
                sys_newline();
                return 0;
            }
         )",
         "11\n"},
        {"width_parametric",
         R"(
            fn inc<W: TritWidth>(x: T<W>) -> T<W> {
                return x + 1;
            }
            fn main() -> t40 {
                let a: t20 = 8;
                let b: t40 = 9;
                sys_write_int(inc(a) + inc(b));
                sys_newline();
                return 0;
            }
         )",
         "19\n"},
        {"unsafe_load_store",
         R"(
            fn main() -> t40 {
                var value: t40 = 0;
                unsafe {
                    store(40, 77);
                    value = load(40);
                }
                sys_write_int(value);
                sys_newline();
                return 0;
            }
         )",
         "77\n"},
        {"char_output",
         R"(
            fn main() -> t40 {
                sys_write_char(65);
                sys_write_char(66);
                sys_write_char(67);
                sys_write_char(10);
                return 0;
            }
         )",
         "ABC\n"},
        {"wildcard_match",
         R"(
            fn main() -> t40 {
                match 0 {
                    pos => { sys_write_int(10); }
                    _ => { sys_write_int(20); }
                }
                sys_newline();
                return 0;
            }
         )",
         "20\n"},
        {"atomic_word",
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
         "7\n", 4096, 4096, 12000},
        {"ownership_auto_drop",
         R"(
            fn alloc(words: t40) -> own<ptr<t40, unknown>> {
                return 44 + words;
            }
            fn free(ptr: borrow<ptr<t40, unknown>>) -> t40 {
                return 0;
            }
            fn main() -> t40 {
                let owned: own<ptr<t40, unknown>> = alloc(1);
                sys_write_int(7);
                sys_newline();
                return 0;
            }
         )",
         "7\n"},
        {"pointer_state_match",
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
         "88\n"},
    };

    if (!ulib.empty()) {
        cases.push_back({"ulib_print_string",
                         ulib + R"(
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
                         "Hi\n", 65536, 65536, 50000});
        cases.push_back({"ulib_vector",
                         ulib + R"(
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
                         "42\n", 65536, 65536, 50000});
    }

    std::vector<std::string> bootstrap_outputs;
    const char* case_filter = std::getenv("C5_CASE");
    std::vector<GoldenCase> selected_cases;
    for (const auto& test_case : cases) {
        if (case_filter && test_case.name != case_filter) continue;
        selected_cases.push_back(test_case);
    }

    for (const auto& test_case : selected_cases) {
        const std::string o0 = runBootstrapCase(test_case,
                                                sandbox::compiler::OptimizationLevel::None,
                                                "bootstrap-O0");
        const std::string o1 = runBootstrapCase(test_case,
                                                sandbox::compiler::OptimizationLevel::Basic,
                                                "bootstrap-O1");
        expect(o0 == o1, "bootstrap producer outputs agree for " + test_case.name);
        bootstrap_outputs.push_back(o0);
    }

    for (std::size_t i = 0; i < selected_cases.size(); ++i) {
        runNativeCompilerCase(selected_cases[i], bootstrap_outputs[i]);
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();

    testCompilerGoldenPrograms();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " Phase C.5 predecessor test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll Phase C.5 predecessor tests passed\n";
    return EXIT_SUCCESS;
}
