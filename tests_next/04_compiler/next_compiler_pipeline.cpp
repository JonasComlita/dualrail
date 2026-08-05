#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <algorithm>
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
    if (!ctx.failures()) {
        ctx.check(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
                  label + " image loads");
    }
    const auto result = sandbox::vm::run(vm, max_steps);
    if (result.halted()) return true;

    std::ostringstream out;
    out << label << " did not halt; status=" << static_cast<int>(result.status)
        << " pc=" << vm.pc << " cause=" << vm.cause;
    ctx.fail(out.str());
    return false;
}

void frontendLexerParserRoundtrip(TestContext& ctx) {
    std::vector<Diagnostic> diagnostics;
    Lexer lexer("frontend.trit", "fn main() -> t40 { return 1; }\n");
    std::vector<Token> tokens = lexer.lex(diagnostics);

    ctx.check(diagnostics.empty(), "lexer accepts a minimal function");
    ctx.check(tokens.size() >= 9, "lexer emits structural tokens");
    ctx.check(tokens.front().kind == TokenKind::Identifier &&
                  tokens.front().text == "fn",
              "first token is fn keyword text");
    ctx.check(tokens.back().kind == TokenKind::End, "lexer appends End token");

    Parser parser("frontend.trit", tokens);
    ModuleAst ast = parser.parse(diagnostics);
    ctx.check(diagnostics.empty(), "parser accepts a minimal function");
    ctx.equal(static_cast<int>(ast.functions.size()), 1,
              "parser emits one function");
    if (!ast.functions.empty()) {
        ctx.equal(ast.functions.front().name, std::string("main"),
                  "parser preserves function name");
        ctx.equal(static_cast<int>(ast.functions.front().body.size()), 1,
                  "parser emits one return statement");
    }
}

void pureMatchTselLowering(TestContext& ctx) {
    const std::string src = R"(
        fn main() -> t40 {
          let x = 3;
          match x {
            neg => { return -1; }
            zero => { return 0; }
            pos => { return x + 4; }
          }
        }
    )";

    CompileResult compiled = compileSource("next_match_pure.trit", src);
    if (!expectCompileOk(ctx, compiled, "pure match source compiles")) return;
    ctx.equal(static_cast<int>(compiled.ssa_module.functions.size()), 1,
              "compile result includes one SSA function");
    ctx.check(compiled.allocation.success, "register allocation succeeds");
    ctx.equal(compiled.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "pure match target section is emitted from optimized SSA");
    ctx.check(std::any_of(
                  compiled.optimized_module.functions.front().blocks.begin(),
                  compiled.optimized_module.functions.front().blocks.end(),
                  [](const BasicBlock& block) {
                      return std::any_of(
                          block.instructions.begin(),
                          block.instructions.end(),
                          [](const Instr& instr) {
                              return instr.opcode == InstrOpcode::Tsel;
                          });
                  }),
              "optimized SSA retains the TSEL operation");
    ctx.contains(compiled.assembly, "tsel",
                 "pure match return lowers to TSEL");
    ctx.check(!contains(compiled.assembly, "brn"),
              "pure match avoids negative branch");
    ctx.check(!contains(compiled.assembly, "brz"),
              "pure match avoids zero branch");
    ctx.check(!contains(compiled.assembly, "brp"),
              "pure match avoids positive branch");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "pure match executable links")) return;
    const int expected_text_pages = std::max(
        1, (linked.instruction_count + sandbox::vm::MMU_PAGE_WORDS - 1) /
               sandbox::vm::MMU_PAGE_WORDS);
    ctx.equal(
        sandbox::vm::executableTextPages(linked.executable_header_v2),
        expected_text_pages,
              "linker reports exact text pages");

    sandbox::vm::VMState vm(256, 256);
    const bool optimized_ran =
        loadAndRun(ctx, vm, linked, 256, "pure match");
    if (optimized_ran) {
        ctx.equal(regLong(vm, 13), 7LL, "main return value reaches r13");
        ctx.equal(vm.branch_instructions_count, 0LL,
                  "pure match executes without conditional branches");
    }

    CompilerOptions unoptimized_options;
    unoptimized_options.optimization = OptimizationLevel::None;
    CompileResult unoptimized = compileSource(
        "next_match_pure_o0.trit", src, unoptimized_options);
    if (expectCompileOk(ctx, unoptimized,
                        "pure match O0 source compiles")) {
        LinkResult unoptimized_link = linkModules({unoptimized.object});
        if (expectLinkOk(ctx, unoptimized_link,
                         "pure match O0 executable links")) {
            sandbox::vm::VMState unoptimized_vm(256, 256);
            const bool unoptimized_ran =
                loadAndRun(ctx, unoptimized_vm, unoptimized_link, 256,
                           "pure match O0");
            if (optimized_ran && unoptimized_ran) {
                ctx.equal(regLong(unoptimized_vm, 13), regLong(vm, 13),
                          "optimized and O0 pure match results agree");
                ctx.equal(unoptimized_vm.branch_instructions_count,
                          vm.branch_instructions_count,
                          "optimized and O0 pure match branch counts agree");
            }
        }
    }
}

void sideEffectMatchBranchLowering(TestContext& ctx) {
    const std::string src = R"(
        fn main() -> t40 {
          let x = 0;
          match x {
            neg => {
              sys_write_char(45);
              return -1;
            }
            zero => {
              sys_write_char(48);
              return 0;
            }
            pos => {
              sys_write_char(43);
              return 1;
            }
          }
        }
    )";

    CompileResult compiled = compileSource("next_match_side_effects.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "side-effectful match source compiles")) {
        return;
    }
    ctx.equal(compiled.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "side-effectful match target section is emitted from optimized SSA");
    ctx.contains(compiled.assembly, "brn",
                 "side-effectful match emits negative branch");
    ctx.contains(compiled.assembly, "brz",
                 "side-effectful match emits zero branch");
    ctx.check(!contains(compiled.assembly, "brp"),
              "side-effectful match uses positive fallthrough");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "side-effectful match executable links")) {
        return;
    }
    sandbox::vm::VMState vm(256, 256);
    const bool optimized_ran =
        loadAndRun(ctx, vm, linked, 256, "side-effectful match");
    if (optimized_ran) {
        ctx.equal(regLong(vm, 13), 0LL,
                  "selected match arm return value reaches r13");
        ctx.equal(vm.syscall_buffer, std::string("0"),
                  "only selected arm performs its side effect");
        ctx.check(vm.branch_instructions_count > 0,
                  "side-effectful match executes conditional branches");
    }

    CompilerOptions unoptimized_options;
    unoptimized_options.optimization = OptimizationLevel::None;
    CompileResult unoptimized = compileSource(
        "next_match_side_effects_o0.trit", src, unoptimized_options);
    if (expectCompileOk(ctx, unoptimized,
                        "side-effectful match O0 source compiles")) {
        LinkResult unoptimized_link = linkModules({unoptimized.object});
        if (expectLinkOk(ctx, unoptimized_link,
                         "side-effectful match O0 executable links")) {
            sandbox::vm::VMState unoptimized_vm(256, 256);
            const bool unoptimized_ran =
                loadAndRun(ctx, unoptimized_vm, unoptimized_link, 256,
                           "side-effectful match O0");
            if (optimized_ran && unoptimized_ran) {
                ctx.equal(regLong(unoptimized_vm, 13), regLong(vm, 13),
                          "optimized and O0 side-effectful match results agree");
                ctx.equal(unoptimized_vm.syscall_buffer, vm.syscall_buffer,
                          "optimized and O0 side-effectful match effects agree");
            }
        }
    }
}

void runtimeTupleSwapSyscallRuntime(TestContext& ctx) {
    const std::string src = R"(
        fn main() -> t40 {
          var a: t40 = 11;
          var b: t40 = 22;
          (a, b) = (b, a);
          sys_write_int(a);
          sys_newline();
          return b;
        }
    )";

    CompileResult compiled = compileSource("next_runtime_swap.trit", src);
    if (!expectCompileOk(ctx, compiled, "runtime source compiles")) return;
    ctx.equal(compiled.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "tuple swap target section is emitted from optimized SSA");
    ctx.check(std::any_of(
                  compiled.optimized_module.functions.front().blocks.begin(),
                  compiled.optimized_module.functions.front().blocks.end(),
                  [](const BasicBlock& block) {
                      return std::any_of(
                          block.instructions.begin(),
                          block.instructions.end(),
                          [](const Instr& instr) {
                              return instr.opcode == InstrOpcode::Swap &&
                                  instr.args.size() == 2 &&
                                  instr.def < 0;
                          });
                  }),
              "optimized SSA retains Swap as an address-based memory operation");
    ctx.contains(compiled.assembly, "swap", "tuple swap lowers to SWAP");
    ctx.contains(compiled.assembly, "syscall 1",
                 "sys_write_int wrapper emits syscall 1");
    ctx.contains(compiled.assembly, "copy r13",
                 "syscall wrapper uses the r13 ABI");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "runtime executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    const bool optimized_ran =
        loadAndRun(ctx, vm, linked, 256, "runtime wrapper");
    if (optimized_ran) {
        ctx.equal(vm.syscall_buffer, std::string("22\n"),
                  "console oracle observes write/newline");
        ctx.equal(regLong(vm, 13), 11LL, "tuple swap changes returned value");
    }

    CompilerOptions unoptimized_options;
    unoptimized_options.optimization = OptimizationLevel::None;
    CompileResult unoptimized = compileSource(
        "next_runtime_swap_o0.trit", src, unoptimized_options);
    if (expectCompileOk(ctx, unoptimized,
                        "runtime O0 source compiles")) {
        LinkResult unoptimized_link = linkModules({unoptimized.object});
        if (expectLinkOk(ctx, unoptimized_link,
                         "runtime O0 executable links")) {
            sandbox::vm::VMState unoptimized_vm(256, 256);
            const bool unoptimized_ran =
                loadAndRun(ctx, unoptimized_vm, unoptimized_link, 256,
                           "runtime wrapper O0");
            if (optimized_ran && unoptimized_ran) {
                ctx.equal(regLong(unoptimized_vm, 13), regLong(vm, 13),
                          "optimized and O0 tuple swap results agree");
                ctx.equal(unoptimized_vm.syscall_buffer, vm.syscall_buffer,
                          "optimized and O0 tuple swap effects agree");
            }
        }
    }
}

void callWhileExecution(TestContext& ctx) {
    const std::string src = R"(
        fn inc(x: t40) -> t40 {
          return x + 1;
        }

        fn main() -> t40 {
          var i: t40 = 0;
          while (3 - i > 0) {
            i = inc(i);
          }
          return i;
        }
    )";

    CompileResult compiled = compileSource("next_call_loop.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "function call and while source compiles")) {
        return;
    }
    ctx.contains(compiled.assembly, "call inc",
                 "direct function call lowers to CALL");
    ctx.contains(compiled.assembly, "brn",
                 "while emits negative exit branch");
    ctx.contains(compiled.assembly, "brz",
                 "while emits zero exit branch");
    ctx.check(!contains(compiled.assembly, "brp"),
              "while positive path falls through");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "function call and while executable links")) {
        return;
    }
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 512, "call/while")) {
        ctx.equal(regLong(vm, 13), 3LL,
                  "while loop and direct call produce expected result");
    }
}

void typeSafetyDiagnostics(TestContext& ctx) {
    {
        const std::string src = R"(
            fn main() -> t5 {
              let wide: t40 = 12;
              return wide;
            }
        )";
        CompileResult compiled = compileSource("next_narrow.trit", src);
        ctx.check(!compiled.success, "implicit narrowing fails");
        ctx.check(hasDiagnostic(compiled.diagnostics, "implicit narrowing"),
                  "narrowing diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              let p: ptr<t40, stack, unknown> = 0;
              return *p;
            }
        )";
        CompileResult compiled = compileSource("next_ptr_unknown.trit", src);
        ctx.check(!compiled.success, "unknown pointer dereference fails");
        ctx.check(hasDiagnostic(compiled.diagnostics, "proving it is valid"),
                  "pointer proof diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              let x = 0;
              match x {
                zero => { return 0; }
                pos => { return 1; }
              }
            }
        )";
        CompileResult compiled = compileSource("next_match_diag.trit", src);
        ctx.check(!compiled.success, "non-exhaustive sign match fails");
        ctx.check(hasDiagnostic(compiled.diagnostics, "neg, zero, and pos"),
                  "match exhaustiveness diagnostic names required arms");
    }
}

void unsafeIntrinsicGate(TestContext& ctx) {
    {
        const std::string src = R"(
            fn main() -> t40 {
              csr_read(cause);
              return 0;
            }
        )";
        CompileResult compiled = compileSource("next_unsafe_reject.trit", src);
        ctx.check(!compiled.success, "raw CSR intrinsic outside unsafe fails");
        ctx.check(hasDiagnostic(compiled.diagnostics, "unsafe block"),
                  "unsafe intrinsic diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              unsafe {
                let cause_value = csr_read(cause);
                fence(1);
              }
              return 0;
            }
        )";
        CompileResult compiled = compileSource("next_unsafe_ok.trit", src);
        if (!expectCompileOk(ctx, compiled,
                             "raw CSR intrinsic inside unsafe compiles")) {
            return;
        }
        ctx.contains(compiled.assembly, "csrr",
                     "unsafe csr_read lowers to CSRR");
        ctx.contains(compiled.assembly, "fence.+1",
                     "unsafe fence lowers memory order");
    }
}

void verifierAllocatorContract(TestContext& ctx) {
    Module module;
    module.name = "bad";
    Function fn;
    fn.name = "f";
    fn.blocks.push_back(BasicBlock{"entry", {}, {}});
    module.functions.push_back(fn);
    const auto diagnostics = verifyModule(module);
    ctx.check(!diagnostics.empty(),
              "verifier rejects an unterminated block");

    Module alloc_module;
    alloc_module.name = "alloc";
    Function alloc_fn;
    alloc_fn.name = "alloc_fn";
    BasicBlock block;
    block.name = "entry";
    for (int i = 0; i < 32; ++i) {
        Instr instr;
        instr.def = i + 1;
        instr.opcode = InstrOpcode::Const;
        instr.type = TypeRef::numeric(sandbox::ir::Type::T40);
        block.instructions.push_back(instr);
    }
    block.terminator.kind = TerminatorKind::Return;
    alloc_fn.blocks.push_back(block);
    alloc_module.functions.push_back(alloc_fn);
    AllocationResult allocation = allocateRegisters(alloc_module);
    ctx.check(allocation.success,
              "graph-color allocator assigns structural values");
    ctx.check(!allocation.scalar_registers.empty(),
              "allocator produces a scalar register map");
}

void linkerDuplicateAndDeadStrip(TestContext& ctx) {
    ObjectModule a;
    a.name = "a";
    a.symbols["main"] = 0;
    ObjectModule b;
    b.name = "b";
    b.symbols["main"] = 0;
    LinkResult duplicate = linkModules({a, b});
    ctx.check(!duplicate.success, "linker rejects duplicate symbols");
    ctx.check(hasDiagnostic(duplicate.diagnostics, "duplicate symbol"),
              "duplicate symbol diagnostic is clear");

    const std::string dead_source = R"(
        fn helper() -> t40 {
            return 7;
        }

        fn unused() -> t40 {
            return 99;
        }

        fn main() -> t40 {
            return helper();
        }
    )";
    CompileResult compiled = compileSource("next_dead_strip.trit", dead_source);
    if (!expectCompileOk(ctx, compiled, "dead-strip fixture compiles")) {
        return;
    }
    LinkOptions strip_options;
    strip_options.dead_strip_functions = true;
    LinkResult stripped = linkModules({compiled.object}, strip_options);
    if (!expectLinkOk(ctx, stripped, "dead-strip fixture links")) return;
    ctx.contains(stripped.assembly, "helper:",
                 "reachable helper survives dead strip");
    ctx.check(!contains(stripped.assembly, "unused:"),
              "unreferenced function is removed");
}

void typeLayoutHmRules(TestContext& ctx) {
    TypeEnv env;
    TypeRef var = TypeRef::typeVar(1);
    Scheme id = generalize(env, functionType({var}, var), false);
    ctx.check(id.generalized, "immutable let generalizes a type variable");
    ctx.equal(static_cast<int>(id.quantified.size()), 1,
              "generalized scheme quantifies one variable");

    TypeVarId next = 10;
    TypeRef first = instantiate(id, next);
    TypeRef second = instantiate(id, next);
    ctx.check(first.kind == TypeKind::Function &&
                  second.kind == TypeKind::Function,
              "instantiation preserves function type");
    ctx.check(first.params[0].type_var != second.params[0].type_var,
              "instantiation creates fresh type variables");

    Scheme restricted = generalize(env, TypeRef::typeVar(99), true);
    ctx.check(!restricted.generalized && restricted.quantified.empty(),
              "value restriction keeps mutable bindings monomorphic");

    Substitution subst;
    std::vector<Diagnostic> diagnostics;
    TypeRef recursive = TypeRef::pointer(TypeRef::typeVar(42));
    ctx.check(!unify(TypeRef::typeVar(42), recursive, subst, diagnostics,
                     SourceSpan{"hm", 1, 1, 1}),
              "occurs-check rejects recursive type-variable binding");

    TypeRef widened = commonNumericType(
        TypeRef::numeric(sandbox::ir::Type::T5),
        TypeRef::numeric(sandbox::ir::Type::T40));
    ctx.check(widened.scalar == sandbox::ir::Type::T40,
              "numeric lattice widens to least common width");

    const std::string src = R"(
        struct Inner { z: t40; }
        struct Pair { a: t40; b: Inner; }
        fn main() -> t40 { return 0; }
    )";
    CompileResult compiled = compileSource("next_layout.trit", src);
    if (!expectCompileOk(ctx, compiled, "layout source compiles")) return;
    ctx.equal(compiled.layout_table["Pair"].size_words, 2,
              "nested struct layout is flattened");
    ctx.equal(compiled.layout_table["Pair"].fields[1].offset_words, 1,
              "field offsets follow declaration order");
}

void aggregateStructArrayRuntime(TestContext& ctx) {
    const std::string src = R"(
        struct Pair { a: t40; b: t40; }

        fn sum(p: Pair) -> t40 {
          return p.a + p.b;
        }

        fn main() -> t40 {
          var p: Pair = Pair { a: 4, b: 5 };
          p.b = p.a + p.b;
          var xs: [t40; 3] = [1, 2, 3];
          xs[1] = xs[0] + xs[2];
          var q: Pair = Pair { a: 6, b: 7 };
          return p.b + xs[1] + sum(q);
        }
    )";
    CompileResult compiled = compileSource("next_aggregates.trit", src);
    if (!expectCompileOk(ctx, compiled, "aggregate source compiles")) return;

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "aggregate executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 512, "aggregate runtime")) {
        ctx.equal(regLong(vm, 13), 26LL,
                  "struct fields, array stores, and aggregate params execute");
    }

    const std::string bounds_src = R"(
        fn main() -> t40 {
          var xs: [t40; 2] = [1, 2];
          return xs[2];
        }
    )";
    CompileResult bounds = compileSource("next_bounds.trit", bounds_src);
    ctx.check(!bounds.success, "constant out-of-bounds array index fails");
    ctx.check(hasDiagnostic(bounds.diagnostics, "out of bounds"),
              "bounds diagnostic is clear");
}

void constantsFoldedRuntime(TestContext& ctx) {
    const std::string src = R"(
        const FILE_CONST: t40 = 5 * 2;

        fn main() -> t40 {
            const LOCAL_CONST: t10 = FILE_CONST + 1;
            let x: t40 = LOCAL_CONST;
            return x;
        }
    )";

    CompileResult compiled = compileSource("next_constants.trit", src);
    if (!expectCompileOk(ctx, compiled, "constants source compiles")) return;
    ctx.contains(compiled.assembly, "11",
                 "constant expression is folded into assembly");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "constants executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 256, "constants runtime")) {
        ctx.equal(regLong(vm, 13), 11LL, "constants return folded value");
    }
}

void widthParametricRuntime(TestContext& ctx) {
    const std::string src = R"(
        fn accumulate<W: TritWidth>(val: T<W>) -> T<W> {
            return val + 1;
        }

        fn main() -> t40 {
            let a: t40 = 5;
            let b: t20 = 10;
            let ra: t40 = accumulate(a);
            let rb: t20 = accumulate(b);
            return ra + rb;
        }
    )";

    CompileResult compiled = compileSource("next_width_parametric.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "width-parametric source compiles")) {
        return;
    }
    ctx.contains(compiled.assembly, "accumulate__Wt40",
                 "t40 width instantiation is emitted");
    ctx.contains(compiled.assembly, "accumulate__Wt20",
                 "t20 width instantiation is emitted");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "width-parametric executable links")) {
        return;
    }
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 512, "width-parametric runtime")) {
        ctx.equal(regLong(vm, 13), 17LL,
                  "width-parametric calls return expected value");
    }
}

void ownershipMoveAutodrop(TestContext& ctx) {
    {
        const std::string src = R"(
            fn alloc(words: t40) -> own<ptr<t40, unknown>> {
                return 44;
            }

            fn free(ptr: borrow<ptr<t40, unknown>>) -> t40 {
                return 0;
            }

            fn take(x: own<ptr<t40, unknown>>) -> t40 {
                return 0;
            }

            fn main() -> t40 {
                let x: own<ptr<t40, unknown>> = alloc(12);
                take(x);
                return x;
            }
        )";
        CompileResult compiled = compileSource("next_owned_move.trit", src);
        ctx.check(!compiled.success,
                  "using an owned value after move is rejected");
        ctx.check(hasDiagnostic(compiled.diagnostics,
                                "use of moved value 'x'"),
                  "owned move diagnostic names the moved binding");
    }

    {
        const std::string src = R"(
            fn alloc(words: t40) -> own<ptr<t40, unknown>> {
                return 44;
            }

            fn free(ptr: borrow<ptr<t40, unknown>>) -> t40 {
                return 0;
            }

            fn main() -> t40 {
                let x: own<ptr<t40, unknown>> = alloc(12);
                return 7;
            }
        )";
        CompileResult compiled = compileSource("next_auto_drop.trit", src);
        if (!expectCompileOk(ctx, compiled,
                             "live owned value at return compiles")) {
            return;
        }
        ctx.contains(compiled.assembly, "call free",
                     "auto-drop inserts a free call before returning");

        LinkResult linked = linkModules({compiled.object});
        if (!expectLinkOk(ctx, linked, "auto-drop executable links")) return;
        sandbox::vm::VMState vm(256, 256);
        if (loadAndRun(ctx, vm, linked, 512, "auto-drop runtime")) {
            ctx.equal(regLong(vm, 13), 7LL,
                      "auto-drop preserves explicit return value");
        }
    }
}

void optimizerStatsSpillsCoalesce(TestContext& ctx) {
    Module opt;
    opt.name = "opt";
    Function opt_fn;
    opt_fn.name = "f";
    BasicBlock block;
    block.name = "entry";
    block.instructions.push_back(Instr{1, InstrOpcode::Const, TypeRef::numeric(sandbox::ir::Type::T40), {}, 2});
    block.instructions.push_back(Instr{2, InstrOpcode::Const, TypeRef::numeric(sandbox::ir::Type::T40), {}, 3});
    block.instructions.push_back(Instr{3, InstrOpcode::Add, TypeRef::numeric(sandbox::ir::Type::T40), {1, 2}});
    block.instructions.push_back(Instr{4, InstrOpcode::Cmp, TypeRef::numeric(sandbox::ir::Type::T40), {1, 2}});
    block.instructions.push_back(Instr{5, InstrOpcode::Cmp, TypeRef::numeric(sandbox::ir::Type::T40), {1, 2}});
    block.terminator.kind = TerminatorKind::Branch3;
    block.terminator.condition = 3;
    block.terminator.target_neg = "n";
    block.terminator.target_zero = "z";
    block.terminator.target_pos = "p";
    opt_fn.blocks.push_back(block);
    opt.functions.push_back(opt_fn);

    CompilerOptions options;
    OptimizerStats stats = optimizeModule(opt, OptimizationLevel::Basic, options);
    ctx.check(stats.constant_folds >= 1, "optimizer folds constants");
    ctx.check(stats.cse_hits >= 1, "optimizer performs pure CSE");
    ctx.check(stats.branch_simplifications >= 1,
              "optimizer simplifies constant branches");

    Module alloc;
    alloc.name = "alloc_pressure";
    Function alloc_fn;
    alloc_fn.name = "pressure";
    BasicBlock pressure;
    pressure.name = "entry";
    for (int i = 0; i < 30; ++i) {
        Instr instr;
        instr.def = i + 1;
        instr.opcode = InstrOpcode::Const;
        instr.type = TypeRef::numeric(sandbox::ir::Type::T40);
        pressure.instructions.push_back(instr);
    }
    Instr call;
    call.opcode = InstrOpcode::Call;
    call.symbol = "sink";
    for (int i = 0; i < 30; ++i) call.args.push_back(i + 1);
    pressure.instructions.push_back(call);
    pressure.terminator.kind = TerminatorKind::Return;
    alloc_fn.blocks.push_back(pressure);
    alloc.functions.push_back(alloc_fn);
    AllocationResult allocation = allocateRegisters(alloc);
    ctx.check(allocation.success, "allocator succeeds under pressure");
    ctx.check(allocation.interference_edges > 0,
              "allocator builds an interference graph");
    ctx.check(allocation.spills > 0,
              "allocator reports stack spills under pressure");

    Module moves;
    moves.name = "moves";
    Function moves_fn;
    moves_fn.name = "coalesce";
    BasicBlock move_block;
    move_block.name = "entry";
    move_block.instructions.push_back(Instr{1, InstrOpcode::Const, TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    move_block.instructions.push_back(Instr{2, InstrOpcode::Copy, TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    move_block.terminator.kind = TerminatorKind::Return;
    moves_fn.blocks.push_back(move_block);
    moves.functions.push_back(moves_fn);
    AllocationResult coalesced = allocateRegisters(moves);
    ctx.check(coalesced.coalesced_moves >= 1,
              "allocator coalesces non-interfering moves");
}

void concurrencyAtomicRuntime(TestContext& ctx) {
    {
        const std::string src = R"(
            fn main() -> t40 {
                let counter: shared<t40, ACQ_REL> = shared_alloc(5);
                let val: t40 = atomic_load(counter, ACQ_REL);
                atomic_store(counter, val + 1, ACQ_REL);
                let val2: t40 = atomic_load(counter, ACQ_REL);
                return val2;
            }
        )";
        CompileResult compiled = compileSource("next_concurrency_ok.trit", src);
        if (!expectCompileOk(ctx, compiled,
                             "atomic increment source compiles")) {
            return;
        }

        LinkResult linked = linkModules({compiled.object});
        if (!expectLinkOk(ctx, linked,
                          "atomic increment executable links")) {
            return;
        }
        sandbox::vm::VMState vm(256, 4096);
        if (loadAndRun(ctx, vm, linked, 2000, "atomic increment runtime")) {
            ctx.equal(regLong(vm, 13), 6LL,
                      "atomic increment returns loaded updated value");
        }
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
                let counter: shared<t40, SEQ_CST> = shared_alloc(5);
                let val: t40 = atomic_load(counter, ACQ_REL);
                return val;
            }
        )";
        CompileResult compiled = compileSource("next_concurrency_order.trit", src);
        ctx.check(!compiled.success, "weak memory order load is rejected");
        ctx.check(hasDiagnostic(compiled.diagnostics,
                                "weaker than declared order"),
                  "weak-order diagnostic is clear");
    }
}

void registerAllocationRuntime(TestContext& ctx) {
    const std::string src = R"(
        fn touch(x: t40) -> t40 {
            return x + 1;
        }

        fn main() -> t40 {
            return (((((1 + 2) + 3) + 4) + 5) + 6) + touch(10);
        }
    )";

    CompileResult compiled = compileSource("next_regalloc_runtime.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "register allocation source compiles")) {
        return;
    }
    bool saves_callee = false;
    bool restores_callee = false;
    for (int reg = 1; reg <= 12; ++reg) {
        saves_callee = saves_callee ||
            contains(compiled.assembly, "store r" + std::to_string(reg) + ", sp");
        restores_callee = restores_callee ||
            contains(compiled.assembly, "load r" + std::to_string(reg) + ", sp");
    }
    ctx.check(saves_callee,
              "callee-saved registers beyond r19-r23 are saved");
    ctx.check(restores_callee,
              "callee-saved registers are restored in the epilogue");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked,
                      "register allocation executable links")) {
        return;
    }
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 512, "register allocation runtime")) {
        ctx.equal(regLong(vm, 13), 32LL,
                  "register-colored function returns expected value");
    }
}

void ifElseChainRuntime(TestContext& ctx) {
    const std::string src = R"(
        fn classify(x: t40) -> t40 {
          if x - 5 > 0 {
            return 10;
          } else if x - 5 == 0 {
            return 20;
          } else {
            return 30;
          }
        }

        fn main() -> t40 {
          return classify(4) + classify(5) + classify(6);
        }
    )";

    CompileResult compiled = compileSource("next_if_else.trit", src);
    if (!expectCompileOk(ctx, compiled, "if/else source compiles")) return;
    ctx.contains(compiled.assembly, "if_then",
                 "if lowering emits then label");
    ctx.contains(compiled.assembly, "if_else",
                 "if lowering emits else label");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "if/else executable links")) return;
    sandbox::vm::VMState vm(512, 512);
    if (loadAndRun(ctx, vm, linked, 1024, "if/else runtime")) {
        ctx.equal(regLong(vm, 13), 60LL,
                  "if/else chain selects expected branches");
    }
}

void syscallWriteCharBuffering(TestContext& ctx) {
    const std::string src = R"(
        fn main() -> t40 {
            sys_write_char(65);
            sys_write_char(66);
            sys_write_char(10);
            return 0;
        }
    )";

    CompileResult compiled = compileSource("next_sys_write_char.trit", src);
    if (!expectCompileOk(ctx, compiled,
                         "sys_write_char source compiles")) {
        return;
    }
    ctx.contains(compiled.assembly, "syscall 22",
                 "sys_write_char wrapper emits syscall 22");

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "sys_write_char executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 256, "sys_write_char runtime")) {
        ctx.equal(vm.syscall_buffer, std::string("AB\n"),
                  "console buffer preserves character writes");
    }
}

void matchWildcardFallback(TestContext& ctx) {
    const std::string src = R"(
        fn main() -> t40 {
            let x = 0;
            match x {
                pos => { return 10; }
                _ => { return 20; }
            }
        }
    )";

    CompileResult compiled = compileSource("next_match_wildcard.trit", src);
    if (!expectCompileOk(ctx, compiled, "match wildcard source compiles")) {
        return;
    }

    LinkResult linked = linkModules({compiled.object});
    if (!expectLinkOk(ctx, linked, "match wildcard executable links")) return;
    sandbox::vm::VMState vm(256, 256);
    if (loadAndRun(ctx, vm, linked, 256, "match wildcard runtime")) {
        ctx.equal(regLong(vm, 13), 20LL,
                  "wildcard arm executes for zero value");
    }
}

void pointerValidArmPromotion(TestContext& ctx) {
    {
        const std::string src = R"(
            fn main() -> t40 {
                let p: ptr<t40, null> = 0;
                return *p;
            }
        )";
        CompileResult compiled = compileSource("next_ptr_null.trit", src);
        ctx.check(!compiled.success, "null pointer dereference is rejected");
        ctx.check(hasDiagnostic(compiled.diagnostics, "proving it is valid"),
                  "null pointer diagnostic requests validity proof");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
                let p: ptr<t40, unknown> = 1;
                match p {
                    null => { return 0; }
                    unknown => { return 0; }
                    valid(q) => { return *q; }
                }
            }
        )";
        CompileResult compiled = compileSource("next_ptr_valid_match.trit", src);
        ctx.check(compiled.success,
                  "valid(p) match arm promotes pointer to valid");
        if (!compiled.success) {
            ctx.fail("valid-arm diagnostics:\n" +
                     formatDiagnostics(compiled.diagnostics));
        }
    }
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"compiler.frontend.lexer_parser_roundtrip",
         "compiler.pipeline_contract", frontendLexerParserRoundtrip},
        {"compiler.match.pure_tsel_lowering",
         "compiler.pipeline_contract", pureMatchTselLowering},
        {"compiler.match.side_effect_branch_lowering",
         "compiler.pipeline_contract", sideEffectMatchBranchLowering},
        {"compiler.runtime.tuple_swap_syscall_runtime",
         "compiler.pipeline_contract", runtimeTupleSwapSyscallRuntime},
        {"compiler.control.call_while_execution",
         "compiler.pipeline_contract", callWhileExecution},
        {"compiler.diagnostics.type_safety_rejections",
         "compiler.pipeline_contract", typeSafetyDiagnostics},
        {"compiler.diagnostics.unsafe_intrinsics_gate",
         "compiler.pipeline_contract", unsafeIntrinsicGate},
        {"compiler.ir.verifier_allocator_contract",
         "compiler.pipeline_contract", verifierAllocatorContract},
        {"compiler.link.duplicate_dead_strip",
         "compiler.pipeline_contract", linkerDuplicateAndDeadStrip},
        {"compiler.types.layout_hm_rules",
         "compiler.pipeline_contract", typeLayoutHmRules},
        {"compiler.aggregate.struct_array_runtime",
         "compiler.pipeline_contract", aggregateStructArrayRuntime},
        {"compiler.constants.folded_runtime",
         "compiler.pipeline_contract", constantsFoldedRuntime},
        {"compiler.generics.width_parametric_runtime",
         "compiler.pipeline_contract", widthParametricRuntime},
        {"compiler.ownership.move_auto_drop",
         "compiler.pipeline_contract", ownershipMoveAutodrop},
        {"compiler.optimizer.stats_spills_coalesce",
         "compiler.pipeline_contract", optimizerStatsSpillsCoalesce},
        {"compiler.concurrency.atomic_order_runtime",
         "compiler.pipeline_contract", concurrencyAtomicRuntime},
        {"compiler.codegen.register_allocation_runtime",
         "compiler.pipeline_contract", registerAllocationRuntime},
        {"compiler.control.if_else_chain_runtime",
         "compiler.pipeline_contract", ifElseChainRuntime},
        {"compiler.syscall.write_char_buffering",
         "compiler.pipeline_contract", syscallWriteCharBuffering},
        {"compiler.match.wildcard_fallback",
         "compiler.pipeline_contract", matchWildcardFallback},
        {"compiler.pointer.valid_arm_promotion",
         "compiler.pipeline_contract", pointerValidArmPromotion},
    };
    return tests_next::runCases("next_compiler_pipeline", cases);
}
