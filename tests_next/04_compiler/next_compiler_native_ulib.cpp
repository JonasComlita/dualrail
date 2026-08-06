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

std::string formatDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return "<none>";
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.format() << "\n";
    }
    return out.str();
}

bool hasDiagnostic(const std::vector<Diagnostic>& diagnostics,
                   const std::string& needle) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(needle) != std::string::npos ||
            diagnostic.format().find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(
        vm.regfile.read(static_cast<uint8_t>(reg)));
}

bool expectCompileOk(TestContext& ctx,
                     const CompileResult& compiled,
                     const std::string& label) {
    if (compiled.success) return true;
    ctx.fail(label + " diagnostics:\n" + formatDiagnostics(compiled.diagnostics));
    return false;
}

bool expectLinkOk(TestContext& ctx,
                  const LinkResult& linked,
                  const std::string& label) {
    if (linked.success) return true;
    ctx.fail(label + " diagnostics:\n" + formatDiagnostics(linked.diagnostics));
    return false;
}

bool loadAndRun(TestContext& ctx,
                sandbox::vm::VMState& vm,
                const LinkResult& linked,
                int max_steps,
                const std::string& label) {
    ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
              label + " image loads");
    const auto result = sandbox::vm::run(vm, max_steps);
    if (result.halted()) return true;

    std::ostringstream out;
    out << label << " did not halt; status=" << static_cast<int>(result.status)
        << " pc=" << vm.pc << " cause=" << vm.cause;
    ctx.fail(out.str());
    return false;
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

std::string nativeParserSource() {
    return readTritFile("ulib.trit") + "\n" +
           readTritFile("tcl_token.trit") + "\n" +
           readTritFile("tcl_lexer.trit") + "\n" +
           readTritFile("tcl_ast.trit") + "\n" +
           readTritFile("tcl_type.trit") + "\n" +
           readTritFile("tcl_parser.trit") + "\n";
}

void compilerModulesAreSsaOnlyAtBothOptimizationLevels(TestContext& ctx) {
    const std::vector<std::string> modules = {
        "kernel.trit", "apps/os_sdk.trit", "ulib.trit"};
    const std::vector<OptimizationLevel> levels = {
        OptimizationLevel::None, OptimizationLevel::Aggressive};
    for (const std::string& module : modules) {
        const std::string source = readTritFile(module);
        ctx.check(!source.empty(), module + " loads for SSA-only validation");
        if (source.empty()) continue;
        for (const OptimizationLevel level : levels) {
            CompilerOptions options;
            options.optimization = level;
            // This must not re-enable a production AST assembly path.
            options.allow_ast_replay = true;
            const std::string level_name =
                level == OptimizationLevel::None ? "O0" : "O1";
            const std::string label = module + " " + level_name;
            CompileResult compiled = compileSource(
                "next_ssa_only_" + level_name + "_" + module,
                source,
                options);
            if (!expectCompileOk(ctx, compiled,
                                 label + " compiles from optimized SSA")) {
                continue;
            }
            ctx.equal(compiled.object.metadata.at(
                          "target.ast_replay_functions"),
                      std::string("0"),
                      label + " has zero AST replay functions");
            ctx.equal(compiled.object.metadata.at(
                          "target.ast_replay_function_names"),
                      std::string(),
                      label + " has no AST replay function names");
            ctx.equal(compiled.object.metadata.at(
                          "target.rejected_functions"),
                      std::string("0"),
                      label + " has no target-rejected functions");
            ctx.equal(compiled.object.metadata.at(
                          "ssa.rejected_functions"),
                      std::string("0"),
                      label + " admits every function into SSA");
            ctx.equal(compiled.object.metadata.at(
                          "target.ir_emitted_functions"),
                      compiled.object.metadata.at("ssa.admitted_functions"),
                      label + " emits every admitted function from IR");
            if (module == "ulib.trit") {
                ctx.contains(compiled.object.metadata.at(
                                 "target.ir_emitted_function_names"),
                             "tst_find",
                             label + " emits tst_find from optimized SSA");
            }
        }
    }
}

std::string asciiStores(const std::string& text) {
    std::ostringstream out;
    out << "                unsafe {\n"
        << "                    var s: ptr<t1, user, valid> = src;\n";
    for (std::size_t i = 0; i < text.size(); ++i) {
        out << "                    store(s + " << i << ", "
            << static_cast<int>(static_cast<unsigned char>(text[i])) << ");\n";
    }
    out << "                }\n";
    return out.str();
}

void ulibRawHeapAutodropRuntime(TestContext& ctx) {
    const std::string src = R"(
        fn malloc_raw(words: t40) -> t40 {
            return 40 + words;
        }

        fn free_raw(ptr: t40) -> t40 {
            match ptr {
                neg => { return 0; }
                zero => { return 0; }
                pos => { return 0; }
            }
        }

        fn malloc(words: t40) -> own<ptr<t40, unknown>> {
            return malloc_raw(words);
        }

        fn alloc(words: t40) -> own<ptr<t40, unknown>> {
            return malloc_raw(words);
        }

        fn free(ptr: borrow<ptr<t40, unknown>>) -> t40 {
            return free_raw(ptr);
        }

        fn vec_new() -> t40 {
            var vec: t40 = malloc_raw(3);
            match vec {
                neg => { return 0; }
                zero => { return 0; }
                pos => { return vec; }
            }
        }

        fn vec_free(vec: t40) -> t40 {
            match vec {
                neg => { return 0; }
                zero => { return 0; }
                pos => { return free_raw(vec); }
            }
        }

        fn main() -> t40 {
            let owned: own<ptr<t40, unknown>> = alloc(4);
            var vec: t40 = vec_new();
            vec_free(vec);
            return 3;
        }
    )";

    CompileResult compiled = compileSource("next_ulib_heap_snippet.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "ulib raw heap ownership snippet compiles")) {
        return;
    }
    ctx.contains(compiled.assembly, "call malloc_raw",
                 "internal code calls raw allocation helper");
    ctx.contains(compiled.assembly, "call free_raw",
                 "internal code calls raw free helper");
    ctx.contains(compiled.assembly, "call free",
                 "owned public value auto-drops through public free");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "ulib raw heap executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 512, "ulib raw heap runtime")) {
        ctx.equal(regLong(vm, 13), 3LL,
                  "ulib raw heap snippet preserves explicit return");
    }
}

void nativeParserGuardAcceptance(TestContext& ctx) {
    const std::string native_src = nativeParserSource();
    ctx.check(!native_src.empty(), "native parser sources load");
    if (native_src.empty()) return;

    const std::string parsed = "fn f() -> void { if pos(5) { return; } }";
    std::string driver = R"(
            fn main() -> t40 {
                var src: t40 = malloc_raw(128);
)" + asciiStores(parsed) + R"(
                var tokens: t40 = tcl_lex(src, )" + std::to_string(parsed.size()) + R"();
                var p: t40 = tcl_parser_new(tokens, src);
                var ast: t40 = tcl_ast_new(0);
                tcl_parser_parse(p, ast);

                var errs: t40 = tcl_parser_errors(p);

                tcl_parser_free(p);
                tcl_ast_free(ast);
                tcl_token_free(tokens);
                free_raw(src);

                match errs {
                    neg => { return 1; }
                    zero => { return 1; }
                    pos => { return 0; }
                }
            }
        )";

    CompileResult compiled = compileSource("next_native_guard.trit",
                                           native_src + driver);
    if (!expectCompileOk(ctx, compiled,
                         "native parser guard fixture compiles")) {
        return;
    }
    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "native parser guard fixture links")) return;
    sandbox::vm::VMState vm(65536, 1000000);
    if (loadAndRun(ctx, vm, linked, 500000, "native parser guard")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "native parser accepts if pos guard syntax");
    }
}

void nativeParserInvalidGuardDiagnostic(TestContext& ctx) {
    const std::string native_src = nativeParserSource();
    ctx.check(!native_src.empty(), "native parser sources load");
    if (native_src.empty()) return;

    const std::string parsed = "fn f() -> void { if other(5) { return; } }";
    std::string driver = R"(
            fn main() -> t40 {
                var src: t40 = malloc_raw(128);
)" + asciiStores(parsed) + R"(
                var tokens: t40 = tcl_lex(src, )" + std::to_string(parsed.size()) + R"();
                var p: t40 = tcl_parser_new(tokens, src);
                var ast: t40 = tcl_ast_new(0);
                tcl_parser_parse(p, ast);

                var errs: t40 = tcl_parser_errors(p);
                var has_invalid_guard_err: t40 = 0;

                match errs {
                    neg => {}
                    zero => {}
                    pos => {
                        var diags: t40 = tcl_parser_diagnostics(p);
                        var len: t40 = vec_len(diags);
                        var i: t40 = 0;
                        while len - i > 0 {
                            var err_code: t40 = vec_get(diags, i + 1);
                            match err_code - 29 {
                                zero => { has_invalid_guard_err = 1; }
                                neg => {}
                                pos => {}
                            }
                            i = i + 2;
                        }
                    }
                }

                tcl_parser_free(p);
                tcl_ast_free(ast);
                tcl_token_free(tokens);
                free_raw(src);

                return has_invalid_guard_err;
            }
        )";

    CompileResult compiled = compileSource("next_native_bad_guard.trit",
                                           native_src + driver);
    if (!expectCompileOk(ctx, compiled,
                         "native parser invalid guard fixture compiles")) {
        return;
    }
    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "native parser invalid guard fixture links")) {
        return;
    }
    sandbox::vm::VMState vm(65536, 1000000);
    if (loadAndRun(ctx, vm, linked, 500000, "native parser invalid guard")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "native parser reports invalid guard diagnostic code 29");
    }
}

void ulibVecSortRuntime(TestContext& ctx) {
    const std::string ulib = readTritFile("ulib.trit");
    ctx.check(!ulib.empty(), "ulib.trit loads");
    if (ulib.empty()) return;

    const std::string src = ulib + R"(
        fn check(actual: t40, expected: t40, ok: t40) -> t40 {
            match ok {
                pos => {}
                zero => { return 0; }
                neg => { return 0; }
            }
            match actual - expected {
                zero => { return ok; }
                neg => { return 0; }
                pos => { return 0; }
            }
        }

        fn check_slot(data: t40, index: t40, expected: t40, ok: t40) -> t40 {
            var value: t40 = 0;
            unsafe { value = load(data + index); }
            return check(value, expected, ok);
        }

        fn main() -> t40 {
            var v: t40 = vec_new();
            vec_push(v, 15);
            vec_push(v, 0 - 3);
            vec_push(v, 42);
            vec_push(v, 0);
            vec_push(v, 0 - 3);
            vec_push(v, 8);
            vec_push(v, 100);
            vec_push(v, 0 - 50);
            vec_push(v, 15);
            vec_push(v, 4);

            vec_sort(v);

            var data: t40 = 0;
            var len: t40 = 0;
            unsafe {
                data = load(v);
                len = load(v + 1);
            }

            var ok: t40 = 1;
            ok = check(len, 10, ok);
            ok = check_slot(data, 0, 0 - 50, ok);
            ok = check_slot(data, 1, 0 - 3, ok);
            ok = check_slot(data, 2, 0 - 3, ok);
            ok = check_slot(data, 3, 0, ok);
            ok = check_slot(data, 4, 4, ok);
            ok = check_slot(data, 5, 8, ok);
            ok = check_slot(data, 6, 15, ok);
            ok = check_slot(data, 7, 15, ok);
            ok = check_slot(data, 8, 42, ok);
            ok = check_slot(data, 9, 100, ok);
            vec_free(v);
            return ok;
        }
    )";

    CompileResult compiled = compileSource("next_ulib_vec_sort.trit", src);
    if (!expectCompileOk(ctx, compiled, "ulib vec_sort source compiles")) {
        return;
    }
    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "ulib vec_sort executable links")) return;
    sandbox::vm::VMState vm(65536, 1000000);
    if (loadAndRun(ctx, vm, linked, 50000, "ulib vec_sort runtime")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "vec_sort orders negative, zero, duplicate, and positive values");
    }
}

void ulibSplitBufferRuntime(TestContext& ctx) {
    const std::string ulib = readTritFile("ulib.trit");
    ctx.check(!ulib.empty(), "ulib.trit loads");
    if (ulib.empty()) return;

    const std::string src = ulib + R"(
        fn split_check(actual: t40, expected: t40, ok: t40) -> t40 {
            match ok {
                pos => {}
                zero => { return 0; }
                neg => { return 0; }
            }
            match actual - expected {
                zero => { return ok; }
                neg => { return 0; }
                pos => { return 0; }
            }
        }

        fn main() -> t40 {
            var sb: t40 = split_buf_new(6);
            match sb {
                neg => { return 0; }
                zero => { return 0; }
                pos => {}
            }

            var ok: t40 = 1;
            ok = split_check(split_gap_len(sb), 6, ok);
            ok = split_check(split_push_neg(sb, 0 - 10), 1, ok);
            ok = split_check(split_push_pos(sb, 10), 1, ok);
            ok = split_check(split_push_neg(sb, 5), 0 - 1, ok);
            ok = split_check(split_push_pos(sb, 0 - 5), 0 - 1, ok);
            ok = split_check(split_gap_start(sb), 1, ok);
            ok = split_check(split_gap_end(sb), 4, ok);
            ok = split_check(split_gap_len(sb), 4, ok);

            var neg_ticket: t40 = split_steal_neg(sb);
            var pos_ticket: t40 = split_steal_pos(sb);
            ok = split_check(neg_ticket, 2, ok);
            ok = split_check(pos_ticket, 5, ok);
            ok = split_check(split_slot_value(sb, neg_ticket), 0 - 1, ok);
            ok = split_check(split_slot_value(sb, pos_ticket), 1, ok);
            ok = split_check(split_gap_start(sb), 2, ok);
            ok = split_check(split_gap_end(sb), 3, ok);
            ok = split_check(split_gap_len(sb), 2, ok);

            ok = split_check(split_pop_neg(sb), 0 - 1, ok);
            ok = split_check(split_pop_pos(sb), 1, ok);
            ok = split_check(split_pop_neg(sb), 0 - 10, ok);
            ok = split_check(split_pop_pos(sb), 10, ok);
            ok = split_check(split_empty_neg(sb), 1, ok);
            ok = split_check(split_empty_pos(sb), 1, ok);
            return ok;
        }
    )";

    CompileResult compiled = compileSource("next_ulib_split_buffer.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "ulib split buffer source compiles")) {
        return;
    }
    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "ulib split buffer executable links")) {
        return;
    }
    sandbox::vm::VMState vm(65536, 1000000);
    if (loadAndRun(ctx, vm, linked, 500000, "ulib split buffer runtime")) {
        ctx.equal(regLong(vm, 13), 1LL,
                  "SplitBuf push/pop and zero-zone steal operations work");
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"compiler.pipeline.ssa_only_system_modules",
         "compiler.pipeline_contract",
         compilerModulesAreSsaOnlyAtBothOptimizationLevels},
        {"compiler.ulib.raw_heap_autodrop_runtime",
         "compiler.pipeline_contract", ulibRawHeapAutodropRuntime},
        {"compiler.native_parser.guard_acceptance",
         "compiler.pipeline_contract", nativeParserGuardAcceptance},
        {"compiler.native_parser.invalid_guard_diagnostic",
         "compiler.pipeline_contract", nativeParserInvalidGuardDiagnostic},
        {"compiler.ulib.vec_sort_runtime",
         "compiler.pipeline_contract", ulibVecSortRuntime},
        {"compiler.ulib.split_buffer_runtime",
         "compiler.pipeline_contract", ulibSplitBufferRuntime},
    };
    return tests_next::runCases("next_compiler_native_ulib", cases);
}
