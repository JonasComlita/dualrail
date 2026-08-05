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

    CompilerOptions o0_options;
    o0_options.optimization = OptimizationLevel::None;
    o0_options.allow_ast_replay = false;
    CompileResult o0 = compileSource("next_aggregates_o0.trit", src, o0_options);
    if (!expectCompileOk(ctx, o0, "aggregate O0 source compiles")) return;
    ctx.equal(o0.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "aggregate O0 emits from optimized SSA");
    LinkResult o0_link = linkModules({o0.object});
    if (!expectLinkOk(ctx, o0_link, "aggregate O0 executable links")) return;
    sandbox::vm::VMState o0_vm(256, 256);
    if (loadAndRun(ctx, o0_vm, o0_link, 512, "aggregate O0 runtime")) {
        ctx.equal(regLong(o0_vm, 13), regLong(vm, 13),
                  "aggregate optimized and O0 results agree");
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

void optimizerAdvancedDifferential(TestContext& ctx) {
    const std::string branch_src = R"(
        fn main() -> t40 {
            let selector: t40 = 1;
            var result: t40 = 0;
            if selector > 0 {
                result = 4;
            } else {
                result = 9;
            }
            return result;
        }
    )";
    CompilerOptions o0_options;
    o0_options.optimization = OptimizationLevel::None;
    o0_options.allow_ast_replay = false;
    CompileResult o0 = compileSource(
        "next_optimizer_branch_o0.trit", branch_src, o0_options);
    if (!expectCompileOk(ctx, o0,
                         "branch O0 source compiles from SSA")) return;
    CompilerOptions o1_options = o0_options;
    o1_options.optimization = OptimizationLevel::Aggressive;
    CompileResult o1 = compileSource(
        "next_optimizer_branch_o1.trit", branch_src, o1_options);
    if (!expectCompileOk(ctx, o1,
                         "branch optimized source compiles from SSA")) return;
    LinkResult o0_link = linkModules({o0.object});
    LinkResult o1_link = linkModules({o1.object});
    if (!expectLinkOk(ctx, o0_link, "branch O0 links") ||
        !expectLinkOk(ctx, o1_link, "branch optimized links")) return;
    sandbox::vm::VMState o0_vm(256, 256);
    sandbox::vm::VMState o1_vm(256, 256);
    const bool o0_ran = loadAndRun(ctx, o0_vm, o0_link, 512,
                                   "branch O0 runtime");
    const bool o1_ran = loadAndRun(ctx, o1_vm, o1_link, 512,
                                   "branch optimized runtime");
    if (o0_ran && o1_ran) {
        ctx.equal(regLong(o0_vm, 13), 4LL,
                  "branch O0 selects the positive arm");
        ctx.equal(regLong(o1_vm, 13), regLong(o0_vm, 13),
                  "branch O0 and optimized results agree");
    }

    Module tsel;
    tsel.name = "tsel_diamond";
    Function tsel_fn;
    tsel_fn.name = "tsel_diamond";
    BasicBlock tsel_entry;
    tsel_entry.name = "entry";
    tsel_entry.instructions.push_back(
        Instr{1, InstrOpcode::Const, TypeRef::trit(), {}, 1});
    tsel_entry.terminator.kind = TerminatorKind::Branch3;
    tsel_entry.terminator.condition = 1;
    tsel_entry.terminator.target_neg = "neg";
    tsel_entry.terminator.target_zero = "zero";
    tsel_entry.terminator.target_pos = "pos";
    BasicBlock tsel_neg;
    tsel_neg.name = "neg";
    tsel_neg.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, -1});
    tsel_neg.terminator.kind = TerminatorKind::Jump;
    tsel_neg.terminator.target = "merge";
    BasicBlock tsel_zero;
    tsel_zero.name = "zero";
    tsel_zero.instructions.push_back(
        Instr{3, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 0});
    tsel_zero.terminator.kind = TerminatorKind::Jump;
    tsel_zero.terminator.target = "merge";
    BasicBlock tsel_pos;
    tsel_pos.name = "pos";
    tsel_pos.instructions.push_back(
        Instr{4, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    tsel_pos.terminator.kind = TerminatorKind::Jump;
    tsel_pos.terminator.target = "merge";
    BasicBlock tsel_merge;
    tsel_merge.name = "merge";
    Instr tsel_phi;
    tsel_phi.def = 5;
    tsel_phi.opcode = InstrOpcode::Phi;
    tsel_phi.type = TypeRef::numeric(sandbox::ir::Type::T40);
    tsel_phi.phi_incoming = {{"neg", 2}, {"zero", 3}, {"pos", 4}};
    tsel_merge.instructions.push_back(tsel_phi);
    tsel_merge.instructions.push_back(
        Instr{-1, InstrOpcode::Ret,
              TypeRef::numeric(sandbox::ir::Type::T40), {5}});
    tsel_merge.terminator.kind = TerminatorKind::Return;
    tsel_fn.blocks = {tsel_entry, tsel_neg, tsel_zero, tsel_pos, tsel_merge};
    tsel.functions.push_back(tsel_fn);
    const OptimizerStats tsel_stats =
        optimizeModule(tsel, OptimizationLevel::Aggressive, o1_options);
    ctx.check(tsel_stats.tsel_conversions == 1,
              "cost table selects a three-way pure TSEL conversion");
    ctx.check(verifyModule(tsel).empty(),
              "converted TSEL diamond passes SSA verification");
    ctx.check(std::any_of(
                  tsel.functions[0].blocks.front().instructions.begin(),
                  tsel.functions[0].blocks.front().instructions.end(),
                  [](const Instr& instr) {
                      return instr.opcode == InstrOpcode::Tsel;
                  }),
              "converted diamond contains a target TSEL");

    Module redundant_iv;
    redundant_iv.name = "redundant_iv";
    Function redundant_iv_fn;
    redundant_iv_fn.name = "redundant_iv";
    redundant_iv_fn.return_type =
        TypeRef::numeric(sandbox::ir::Type::T40);
    BasicBlock redundant_entry;
    redundant_entry.name = "entry";
    redundant_entry.instructions.push_back(
        Instr{1, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 0});
    redundant_entry.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    redundant_entry.terminator.kind = TerminatorKind::Jump;
    redundant_entry.terminator.target = "header";
    BasicBlock redundant_header;
    redundant_header.name = "header";
    Instr iv_a;
    iv_a.def = 3;
    iv_a.opcode = InstrOpcode::Phi;
    iv_a.type = TypeRef::numeric(sandbox::ir::Type::T40);
    iv_a.phi_incoming = {{"entry", 1}, {"body", 6}};
    redundant_header.instructions.push_back(iv_a);
    Instr iv_b = iv_a;
    iv_b.def = 4;
    iv_b.phi_incoming = {{"entry", 1}, {"body", 7}};
    redundant_header.instructions.push_back(iv_b);
    redundant_header.instructions.push_back(
        Instr{5, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T1), {}, -1});
    redundant_header.terminator.kind = TerminatorKind::Branch3;
    redundant_header.terminator.condition = 5;
    redundant_header.terminator.target_neg = "exit";
    redundant_header.terminator.target_zero = "exit";
    redundant_header.terminator.target_pos = "body";
    BasicBlock redundant_body;
    redundant_body.name = "body";
    redundant_body.instructions.push_back(
        Instr{6, InstrOpcode::Add,
              TypeRef::numeric(sandbox::ir::Type::T40), {3, 2}});
    redundant_body.instructions.push_back(
        Instr{7, InstrOpcode::Add,
              TypeRef::numeric(sandbox::ir::Type::T40), {4, 2}});
    redundant_body.terminator.kind = TerminatorKind::Jump;
    redundant_body.terminator.target = "header";
    BasicBlock redundant_exit;
    redundant_exit.name = "exit";
    Instr redundant_ret;
    redundant_ret.opcode = InstrOpcode::Ret;
    redundant_ret.type = TypeRef::numeric(sandbox::ir::Type::T40);
    redundant_ret.args = {3};
    redundant_ret.effect = Effect::Control;
    redundant_exit.instructions.push_back(redundant_ret);
    redundant_exit.terminator.kind = TerminatorKind::Return;
    redundant_iv_fn.blocks = {
        redundant_entry, redundant_header, redundant_body, redundant_exit};
    redundant_iv.functions.push_back(redundant_iv_fn);
    const OptimizerStats redundant_stats = optimizeModule(
        redundant_iv, OptimizationLevel::Aggressive, o1_options);
    ctx.check(redundant_stats.induction_simplifications >= 1,
              "optimizer eliminates an equivalent nonzero-step induction variable");
    ctx.check(verifyModule(redundant_iv).empty(),
              "redundant-IV rewrite passes SSA verification");
    bool redundant_phi_survived = false;
    for (const Instr& instr : redundant_iv.functions[0].blocks[1].instructions)
        redundant_phi_survived = redundant_phi_survived || instr.def == 4;
    ctx.check(!redundant_phi_survived,
              "redundant induction phi is removed after canonicalization");

    const std::string equivalent_iv_src = R"(
        fn main() -> t40 {
            var i: t40 = 0;
            var j: t40 = 0;
            while 5 - i > 0 {
                i = i + 1;
                j = j + 1;
            }
            return i * 10 + j;
        }
    )";
    CompileResult equivalent_iv_o0 = compileSource(
        "next_optimizer_equivalent_iv_o0.trit", equivalent_iv_src,
        o0_options);
    CompileResult equivalent_iv_o1 = compileSource(
        "next_optimizer_equivalent_iv_o1.trit", equivalent_iv_src,
        o1_options);
    if (!expectCompileOk(ctx, equivalent_iv_o0,
                         "equivalent-IV O0 source compiles") ||
        !expectCompileOk(ctx, equivalent_iv_o1,
                         "equivalent-IV optimized source compiles")) return;
    ctx.check(equivalent_iv_o1.optimizer_stats.induction_simplifications >= 1,
              "optimized source removes a redundant nonzero-step IV");
    LinkResult equivalent_iv_o0_link = linkModules({equivalent_iv_o0.object});
    LinkResult equivalent_iv_o1_link = linkModules({equivalent_iv_o1.object});
    if (!expectLinkOk(ctx, equivalent_iv_o0_link,
                      "equivalent-IV O0 links") ||
        !expectLinkOk(ctx, equivalent_iv_o1_link,
                      "equivalent-IV optimized links")) return;
    sandbox::vm::VMState equivalent_iv_o0_vm(256, 256);
    sandbox::vm::VMState equivalent_iv_o1_vm(256, 256);
    const bool equivalent_iv_o0_ran = loadAndRun(
        ctx, equivalent_iv_o0_vm, equivalent_iv_o0_link, 4096,
        "equivalent-IV O0 runtime");
    const bool equivalent_iv_o1_ran = loadAndRun(
        ctx, equivalent_iv_o1_vm, equivalent_iv_o1_link, 4096,
        "equivalent-IV optimized runtime");
    if (equivalent_iv_o0_ran && equivalent_iv_o1_ran) {
        ctx.equal(regLong(equivalent_iv_o0_vm, 13), 55LL,
                  "equivalent-IV O0 loop executes five backedges");
        ctx.equal(regLong(equivalent_iv_o1_vm, 13),
                  regLong(equivalent_iv_o0_vm, 13),
                  "equivalent-IV O0 and optimized results agree");
    }

    const std::string induction_src = R"(
        fn main() -> t40 {
            var i: t40 = 0;
            while i > 0 {
                i = i + 0;
            }
            return i;
        }
    )";
    CompileResult induction_o0 = compileSource(
        "next_optimizer_induction_o0.trit", induction_src, o0_options);
    CompileResult induction_o1 = compileSource(
        "next_optimizer_induction_o1.trit", induction_src, o1_options);
    if (!expectCompileOk(ctx, induction_o0,
                         "induction O0 source compiles") ||
        !expectCompileOk(ctx, induction_o1,
                         "induction optimized source compiles")) return;
    ctx.check(induction_o1.optimizer_stats.induction_simplifications >= 1,
              "optimizer simplifies a redundant zero-step induction variable");
    LinkResult induction_o0_link = linkModules({induction_o0.object});
    LinkResult induction_o1_link = linkModules({induction_o1.object});
    if (!expectLinkOk(ctx, induction_o0_link, "induction O0 links") ||
        !expectLinkOk(ctx, induction_o1_link,
                      "induction optimized links")) return;
    sandbox::vm::VMState induction_o0_vm(256, 256);
    sandbox::vm::VMState induction_o1_vm(256, 256);
    const bool induction_o0_ran = loadAndRun(
        ctx, induction_o0_vm, induction_o0_link, 512,
        "induction O0 runtime");
    const bool induction_o1_ran = loadAndRun(
        ctx, induction_o1_vm, induction_o1_link, 512,
        "induction optimized runtime");
    if (induction_o0_ran && induction_o1_ran) {
        ctx.equal(regLong(induction_o1_vm, 13), 0LL,
                  "zero-step induction loop preserves its initial value");
        ctx.equal(regLong(induction_o1_vm, 13),
                  regLong(induction_o0_vm, 13),
                  "induction O0 and optimized results agree");
    }

    Module call_live;
    call_live.name = "call_live";
    Function call_live_fn;
    call_live_fn.name = "call_live";
    BasicBlock call_live_block;
    call_live_block.name = "entry";
    call_live_block.instructions.push_back(
        Instr{1, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 7});
    Instr call;
    call.opcode = InstrOpcode::Call;
    call.symbol = "sink";
    call.effect = Effect::Control;
    call_live_block.instructions.push_back(call);
    call_live_block.instructions.push_back(
        Instr{2, InstrOpcode::Copy,
              TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    call_live_block.instructions.push_back(
        Instr{-1, InstrOpcode::Ret,
              TypeRef::numeric(sandbox::ir::Type::T40), {2}});
    call_live_block.terminator.kind = TerminatorKind::Return;
    call_live_fn.blocks.push_back(call_live_block);
    call_live.functions.push_back(call_live_fn);
    AllocationResult call_live_allocation = allocateRegisters(call_live);
    ctx.check(call_live_allocation.success,
              "allocator colors values live across a call");
    ctx.check(call_live_allocation.scalar_registers.count(1) != 0,
              "call-live value receives a physical register");
    if (call_live_allocation.scalar_registers.count(1)) {
        const int reg = call_live_allocation.scalar_registers.at(1);
        ctx.check(reg >= 1 && reg <= 12,
                  "call-live scalar uses a callee-saved register");
        ctx.check(!call_live_allocation.caller_saved_live_across_calls.count(reg),
                  "call-live scalar is not reported caller-saved");
    }

    Module wide;
    wide.name = "wide_pair";
    Function wide_fn;
    wide_fn.name = "wide_pair";
    BasicBlock wide_block;
    wide_block.name = "entry";
    wide_block.instructions.push_back(
        Instr{1, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T50), {}, 7});
    wide_block.instructions.push_back(call);
    wide_block.instructions.push_back(
        Instr{-1, InstrOpcode::Ret,
              TypeRef::numeric(sandbox::ir::Type::T50), {1}});
    wide_block.terminator.kind = TerminatorKind::Return;
    wide_fn.blocks.push_back(wide_block);
    wide.functions.push_back(wide_fn);
    AllocationResult wide_allocation = allocateRegisters(wide);
    ctx.check(wide_allocation.success,
              "allocator colors T50 values live across a call");
    ctx.check(wide_allocation.scalar_registers.count(1) != 0,
              "T50 call-live value receives a physical register");
    if (wide_allocation.scalar_registers.count(1)) {
        const int reg = wide_allocation.scalar_registers.at(1);
        ctx.check(reg >= 1 && reg <= 11 && reg + 1 <= 12,
                  "T50 call-live value receives an adjacent callee pair");
    }
    ctx.check(verifyModule(call_live).empty() && verifyModule(wide).empty(),
              "allocator call-live and T50 fixtures pass SSA verification");
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

void externalMemoryCallDifferential(TestContext& ctx) {
    const std::string src = R"(
        fn identity(x: t40) -> t40 {
            return x;
        }

        fn main() -> t40 {
            var ptr: t40 = 120;
            unsafe {
                store(ptr, 41);
                let first: t40 = load(ptr);
                let through_call: t40 = identity(first);
                store(ptr, through_call + 1);
                let second: t40 = load(ptr);
                return second;
            }
        }
    )";

    CompilerOptions optimized_options;
    optimized_options.allow_ast_replay = false;
    CompileResult optimized = compileSource(
        "next_external_memory_call_o1.trit", src, optimized_options);
    if (!expectCompileOk(ctx, optimized,
                         "external-memory call O1 source compiles")) {
        return;
    }
    ctx.equal(optimized.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "external-memory call O1 emits from optimized SSA");
    LinkResult optimized_link = linkModules({optimized.object});
    if (!expectLinkOk(ctx, optimized_link,
                      "external-memory call O1 links")) {
        return;
    }
    sandbox::vm::VMState optimized_vm(256, 256);
    if (!loadAndRun(ctx, optimized_vm, optimized_link, 512,
                    "external-memory call O1 runtime")) {
        return;
    }

    CompilerOptions unoptimized_options = optimized_options;
    unoptimized_options.optimization = OptimizationLevel::None;
    CompileResult unoptimized = compileSource(
        "next_external_memory_call_o0.trit", src, unoptimized_options);
    if (!expectCompileOk(ctx, unoptimized,
                         "external-memory call O0 source compiles")) {
        return;
    }
    ctx.equal(unoptimized.object.metadata.at("target.ast_replay_functions"),
              std::string("0"),
              "external-memory call O0 emits from optimized SSA");
    LinkResult unoptimized_link = linkModules({unoptimized.object});
    if (!expectLinkOk(ctx, unoptimized_link,
                      "external-memory call O0 links")) {
        return;
    }
    sandbox::vm::VMState unoptimized_vm(256, 256);
    if (loadAndRun(ctx, unoptimized_vm, unoptimized_link, 512,
                   "external-memory call O0 runtime")) {
        ctx.equal(regLong(optimized_vm, 13), 42LL,
                  "external-memory call O1 preserves ordered alias effects");
        ctx.equal(regLong(unoptimized_vm, 13), 42LL,
                  "external-memory call O0 preserves ordered alias effects");
        ctx.equal(regLong(optimized_vm, 13), regLong(unoptimized_vm, 13),
                  "external-memory call optimized and O0 results agree");
    }
}

void vectorTargetBoundary(TestContext& ctx) {
    const std::string src = R"(
        fn identity(v: vec<t20>) -> vec<t20> {
            return v;
        }

        fn main() -> t40 {
            return 0;
        }
    )";
    CompilerOptions options;
    options.allow_ast_replay = false;
    CompileResult compiled = compileSource(
        "next_vector_target_boundary.trit", src, options);
    ctx.check(!compiled.success,
              "strict SSA rejects vector lowering without an ABI");
    ctx.check(hasDiagnostic(compiled.diagnostics,
                            "optimized SSA target lowering failed"),
              "vector strict-SSA diagnostic identifies target boundary");
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
        {"compiler.optimizer.advanced_differential",
         "compiler.pipeline_contract", optimizerAdvancedDifferential},
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
        {"compiler.memory.external_call_differential",
         "compiler.pipeline_contract", externalMemoryCallDifferential},
        {"compiler.vector.target_boundary",
         "compiler.pipeline_contract", vectorTargetBoundary},
    };
    return tests_next::runCases("next_compiler_pipeline", cases);
}
