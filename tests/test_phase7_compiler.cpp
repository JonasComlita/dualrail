#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

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

bool hasDiagnostic(
    const std::vector<sandbox::compiler::Diagnostic>& diagnostics,
    const std::string& needle) {

    for (const auto& diagnostic : diagnostics) {
        if (contains(diagnostic.message, needle) ||
            contains(diagnostic.format(), needle)) {
            return true;
        }
    }
    return false;
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

void testCompileAndRunMatchProgram() {
    std::cout << "[1] Phase 7 source -> SSA -> link -> VM\n";
    using namespace sandbox::compiler;

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

    CompileResult compiled = compileSource("phase7_match.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS:" << std::endl;
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << std::endl;
        }
    }
    expect(compiled.success, "match source compiles");
    expect(compiled.ssa_module.functions.size() == 1, "compile result includes SSA function");
    expect(compiled.object.metadata.at("object.ssa_is_optimized") == "true",
           "object publishes optimized SSA as its code-generation source");
    expect(contains(compiled.assembly, "tsel"),
           "constant pure match retains branch-free TSEL lowering");
    expect(compiled.optimizer_stats.constant_folds > 0,
           "constant pure match is folded by the SSA optimizer");
    expect(!contains(compiled.assembly, "brn"), "pure match avoids negative branch");
    expect(!contains(compiled.assembly, "brz"), "pure match avoids zero branch");
    expect(!contains(compiled.assembly, "brp"), "pure match avoids positive branch");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "linked match executable assembles");
    expect(linked.executable_header_v2.text_words ==
               linked.instruction_count,
           "linker emits accurate executable text word count");
    expect(linked.executable_header_v2.isa_version ==
               sandbox::architecture::v2::ISA_VERSION,
           "linker emits ISA v2 metadata by default");
    expect(linked.instruction_count > 0, "linker reports instruction count");

    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "linked image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "linked image halts through standalone _start");
        expect(regLong(vm, 13) == 7, "main return value is preserved in r13");
        expect(vm.branch_instructions_count == 0, "pure match executes without conditional branches");
    }
}

void testSideEffectfulMatchKeepsBranchLowering() {
    std::cout << "[1b] Side-effectful match keeps branch lowering\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn main(x: t40) -> t40 {
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

    CompileResult compiled = compileSource("phase7_match_side_effects.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR SIDE-EFFECTFUL MATCH:" << std::endl;
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << std::endl;
        }
    }
    expect(compiled.success, "side-effectful match compiles");
    expect(compiled.object.metadata.at(
               "target.ast_replay_functions") == "0",
           "dynamic side-effectful match emits solely from optimized IR");
    expect(contains(compiled.assembly, "brn"), "side-effectful match emits negative branch");
    expect(contains(compiled.assembly, "brz"), "side-effectful match emits zero branch");
    expect(!contains(compiled.assembly, "brp"), "side-effectful match uses positive fallthrough");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "side-effectful match executable assembles");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "side-effectful match image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "side-effectful match image halts");
        expect(regLong(vm, 13) == 0, "side-effectful match returns selected arm value");
        expect(vm.syscall_buffer == "0", "side-effectful match executes only selected arm");
        expect(vm.branch_instructions_count > 0, "side-effectful match executes conditional branches");
    }
}

void testRuntimeSyscallWrapperAndTupleSwap() {
    std::cout << "[2] Runtime wrappers and tuple-swap lowering\n";
    using namespace sandbox::compiler;

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

    CompileResult compiled = compileSource("phase7_runtime.trit", src);
    expect(compiled.success, "runtime source compiles");
    expect(compiled.object.metadata.at(
               "target.ast_replay_functions") == "0",
           "tuple swap and runtime wrappers emit solely from optimized IR");
    expect(contains(compiled.assembly, "swap"),
           "tuple swap lowers through the SSA Swap memory operation");
    expect(contains(compiled.assembly, "syscall 1"), "sys_write_int wrapper emits syscall 1");
    expect(contains(compiled.assembly, "copy r13"), "syscall wrapper uses r13 ABI");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "runtime executable links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "runtime image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "runtime image halts");
        expect(vm.syscall_buffer == "22\n", "legacy console oracle sees write/newline");
        expect(regLong(vm, 13) == 11, "tuple swap changed returned value");
    }
}

void testFunctionCallAndWhileLoop() {
    std::cout << "[3] Direct function calls and while loops\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn inc(x: t40) -> t40 {
          return x + 1;
        }

        fn sum7(a: t40, b: t40, c: t40, d: t40, e: t40, f: t40, g: t40) -> t40 {
          return a + b + c + d + e + f + g;
        }

        fn main() -> t40 {
          var i: t40 = 0;
          while (3 - i > 0) {
            i = inc(i);
          }
          return i + sum7(1, 2, 3, 4, 5, 6, 7);
        }
    )";

    CompileResult compiled = compileSource("phase7_call_loop.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR CALL_LOOP:" << std::endl;
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << std::endl;
        }
    }
    expect(compiled.success, "function call and while source compiles");
    expect(compiled.object.metadata.at(
               "target.ast_replay_functions") == "0",
           "ordinary scalar calls and loop phis emit solely from optimized IR");
        expect(contains(compiled.assembly, "call inc"), "direct function call lowers to CALL");
    expect(contains(compiled.assembly, "call sum7"),
           "stack-passed function call lowers to CALL");
    expect(contains(compiled.assembly, "brn"), "while lowers cold negative exit branch");
    expect(contains(compiled.assembly, "brz"), "while lowers cold zero exit branch");
    expect(!contains(compiled.assembly, "brp"), "while positive path falls through");
    const auto main_ir = std::find_if(
        compiled.ssa_module.functions.begin(),
        compiled.ssa_module.functions.end(),
        [](const Function& fn) { return fn.name == "main"; });
    expect(main_ir != compiled.ssa_module.functions.end() &&
               main_ir->blocks.size() >= 4,
           "while lowering creates entry, condition, body, and exit IR blocks");
    if (main_ir != compiled.ssa_module.functions.end()) {
        const ControlFlowGraph cfg = buildControlFlowGraph(*main_ir);
        expect(cfg.invalid_targets.empty(), "while IR has closed CFG edges");
        expect(!computeDominance(*main_ir, cfg).immediate_dominator.empty(),
               "while IR computes dominators");
    }

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "function call and while executable links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "call/loop image loads");
        const auto result = sandbox::vm::run(vm, 512);
        if (!result.halted()) {
            std::cout << "DEBUG: call/loop failed. status=" << static_cast<int>(result.status)
                      << ", cause=" << vm.cause << ", pc=" << vm.pc
                      << ", trap_reg=" << sandbox::vm::ops::toLong(vm.trap_reg) << "\n";
            std::cout << "Assembly:\n" << compiled.assembly << "\n";
        }
        expect(result.halted(), "call/loop image halts");
        expect(regLong(vm, 13) == 31,
               "while loop and stack-passed direct call produce expected result");
    }
}

void testDirectRawMemoryAddressLowering() {
    std::cout << "[3c] Direct raw-pointer memory lowering\n";
    using namespace sandbox::compiler;

    // The pointer is first held in a local scalar, then used as an address.
    // This must not be mistaken for a stack-frame address by the target
    // emitter: the store/load pair is an observable external-memory effect.
    const std::string src = R"(
        fn main() -> t40 {
          var ptr: t40 = 120;
          unsafe {
            store(ptr, 41);
            return load(ptr);
          }
        }
    )";

    CompileResult compiled = compileSource("phase7_raw_memory.trit", src);
    expect(compiled.success, "raw-pointer memory source compiles");
    expect(compiled.object.metadata.at(
               "target.ir_emitted_functions") == "1" &&
               compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
           "raw-pointer memory emits solely from optimized IR");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "raw-pointer memory executable links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(
                   vm, linked.assembled),
               "raw-pointer memory image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "raw-pointer memory image halts");
        expect(regLong(vm, 13) == 41,
               "raw-pointer store/load preserves external memory value");
    }
}

void testIfElseStatements() {
    std::cout << "[3b] If/else statement lowering\n";
    using namespace sandbox::compiler;

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

    CompileResult compiled = compileSource("phase7_if_else.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR IF/ELSE:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
    }
    expect(compiled.success, "if/else source compiles");
    expect(std::stoi(compiled.object.metadata.at(
               "target.ir_emitted_functions")) >= 1,
           "verified multi-block scalar function emits from SSA IR");
    expect(contains(compiled.assembly, "if_then"), "if lowering emits then label");
    expect(contains(compiled.assembly, "if_else"), "if lowering emits else label");
    const auto classify_ir = std::find_if(
        compiled.ssa_module.functions.begin(),
        compiled.ssa_module.functions.end(),
        [](const Function& fn) { return fn.name == "classify"; });
    expect(classify_ir != compiled.ssa_module.functions.end() &&
               classify_ir->blocks.size() >= 4,
           "if/else lowering creates structural CFG blocks");
    if (classify_ir != compiled.ssa_module.functions.end()) {
        const ControlFlowGraph cfg = buildControlFlowGraph(*classify_ir);
        expect(cfg.invalid_targets.empty(), "if/else IR has closed CFG edges");
        expect(!computeDominance(*classify_ir, cfg).frontier.empty(),
               "if/else IR computes dominance frontiers");
    }

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "if/else executable links");
    sandbox::vm::VMState vm(512, 512);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "if/else image loads");
        const auto result = sandbox::vm::run(vm, 1024);
        expect(result.halted(), "if/else image halts");
        expect(regLong(vm, 13) == 60, "if/else chain selects expected branches");
    }
}

void testTypeDiagnostics() {
    std::cout << "[4] Phase 7 type and safety diagnostics\n";
    using namespace sandbox::compiler;

    {
        const std::string src = R"(
            fn main() -> t5 {
              let wide: t40 = 12;
              return wide;
            }
        )";
        CompileResult compiled = compileSource("phase7_narrow.trit", src);
        expect(!compiled.success, "implicit narrowing fails");
        expect(hasDiagnostic(compiled.diagnostics, "implicit narrowing"),
               "narrowing diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              let p: ptr<t40, stack, unknown> = 0;
              return *p;
            }
        )";
        CompileResult compiled = compileSource("phase7_ptr.trit", src);
        expect(!compiled.success, "unknown pointer dereference fails");
        expect(hasDiagnostic(compiled.diagnostics, "proving it is valid"),
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
        CompileResult compiled = compileSource("phase7_match_diag.trit", src);
        expect(!compiled.success, "non-exhaustive sign match fails");
        expect(hasDiagnostic(compiled.diagnostics, "neg, zero, and pos"),
               "match exhaustiveness diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              csr_read(cause);
              return 0;
            }
        )";
        CompileResult compiled = compileSource("phase7_unsafe.trit", src);
        expect(!compiled.success, "raw CSR intrinsic outside unsafe fails");
        expect(hasDiagnostic(compiled.diagnostics, "unsafe block"),
               "unsafe intrinsic diagnostic is clear");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              unsafe {
                csr_read(cause);
                fence(1);
              }
              return 0;
            }
        )";
        CompileResult compiled = compileSource("phase7_unsafe_ok.trit", src);
        expect(compiled.success, "raw CSR intrinsic inside unsafe compiles");
        expect(compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
               "CSR and fence intrinsics emit solely from optimized IR");
        expect(contains(compiled.assembly, "csrr"), "unsafe csr_read lowers to CSRR");
        expect(contains(compiled.assembly, "fence.+1"), "unsafe fence lowers memory order");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "CSR/fence optimized IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "CSR/fence optimized IR image loads");
            expect(sandbox::vm::run(vm, 256).halted(),
                   "CSR/fence optimized IR image halts");
        }
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              unsafe {
                let old = tldr(12, 1);
                tstr(12, 33, old, -1);
              }
              return 0;
            }
        )";
        CompileResult compiled = compileSource("phase7_atomic.trit", src);
        expect(compiled.success, "raw atomic intrinsics inside unsafe compile");
        expect(compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
               "ternary atomic intrinsics emit solely from optimized IR");
        expect(contains(compiled.assembly, "tldr.+1"), "unsafe tldr lowers memory order");
        expect(contains(compiled.assembly, "tstr.-1"), "unsafe tstr lowers memory order");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "ternary atomic optimized IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "ternary atomic optimized IR image loads");
            expect(sandbox::vm::run(vm, 256).halted(),
                   "ternary atomic optimized IR image halts");
            const auto [stored, fault] =
                vm.dmem.load(12);
            expect(fault ==
                       sandbox::vm::MemFaultCode::OK &&
                       sandbox::vm::ops::toLong(stored) ==
                           33,
                   "ternary atomic optimized IR updates memory");
        }
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              return sys_write(1, 2, 3, 4, 5);
            }
        )";
        CompileResult compiled =
            compileSource("phase7_syscall_arity.trit", src);
        expect(!compiled.success,
               "syscall ABI v2 rejects a fifth argument");
        expect(hasDiagnostic(
                   compiled.diagnostics,
                   "at most four arguments"),
               "syscall ABI v2 arity diagnostic is clear");
    }
}

void testVerifierAllocatorAndDuplicateSymbols() {
    std::cout << "[5] Verifier, allocator, and linker errors\n";
    using namespace sandbox::compiler;

    Module module;
    module.name = "bad";
    Function fn;
    fn.name = "f";
    fn.blocks.push_back(BasicBlock{"entry", {}, {}});
    module.functions.push_back(fn);
    auto diagnostics = verifyModule(module);
    expect(!diagnostics.empty(), "verifier rejects unterminated block");

    Module allocModule;
    allocModule.name = "alloc";
    Function afn;
    afn.name = "alloc_fn";
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
    afn.blocks.push_back(block);
    allocModule.functions.push_back(afn);
    AllocationResult allocation = allocateRegisters(allocModule);
    expect(allocation.success, "graph-color allocator assigns structural values");
    expect(!allocation.scalar_registers.empty(), "allocator produces scalar register map");

    ObjectModule a;
    a.name = "a";
    a.symbols["main"] = 0;
    ObjectModule b;
    b.name = "b";
    b.symbols["main"] = 0;
    LinkResult linked = linkModules({a, b});
    expect(!linked.success, "linker rejects duplicate symbols");
    expect(hasDiagnostic(linked.diagnostics, "duplicate symbol"),
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
    CompileResult dead_compiled = compileSource("dead_strip.trit", dead_source);
    expect(dead_compiled.success, "dead-strip fixture compiles");
    LinkOptions strip_options;
    strip_options.dead_strip_functions = true;
    LinkResult stripped = linkModules({dead_compiled.object}, strip_options);
    expect(stripped.success, "dead-strip fixture links");
    expect(contains(stripped.assembly, "helper:"), "reachable helper survives dead strip");
    expect(!contains(stripped.assembly, "unused:"), "unreferenced function is removed");
}

void testHMGeneralizationAndLayouts() {
    std::cout << "[6] HM generalization and aggregate layouts\n";
    using namespace sandbox::compiler;

    TypeEnv env;
    TypeRef var = TypeRef::typeVar(1);
    Scheme id = generalize(env, functionType({var}, var), false);
    expect(id.generalized, "immutable let can generalize a type variable");
    expect(id.quantified.size() == 1, "generalized scheme quantifies free variable");

    TypeVarId next = 10;
    TypeRef first = instantiate(id, next);
    TypeRef second = instantiate(id, next);
    expect(first.kind == TypeKind::Function && second.kind == TypeKind::Function,
           "instantiation preserves function type");
    expect(first.params[0].type_var != second.params[0].type_var,
           "instantiation creates fresh type variables");

    Scheme restricted = generalize(env, TypeRef::typeVar(99), true);
    expect(!restricted.generalized && restricted.quantified.empty(),
           "value restriction keeps mutable/effectful bindings monomorphic");

    Substitution subst;
    std::vector<Diagnostic> diagnostics;
    TypeRef recursive = TypeRef::pointer(TypeRef::typeVar(42));
    expect(!unify(TypeRef::typeVar(42), recursive, subst, diagnostics, SourceSpan{"hm", 1, 1, 1}),
           "occurs-check rejects recursive type variable binding");

    TypeRef widened = commonNumericType(TypeRef::numeric(sandbox::ir::Type::T5),
                                        TypeRef::numeric(sandbox::ir::Type::T40));
    expect(widened.scalar == sandbox::ir::Type::T40, "numeric lattice widens to least common width");

    const std::string src = R"(
        struct Inner { z: t40; }
        struct Pair { a: t40; b: Inner; }
        fn main() -> t40 { return 0; }
    )";
    CompileResult compiled = compileSource("phase7_layout.trit", src);
    expect(compiled.success, "layout source compiles");
    expect(compiled.layout_table["Pair"].size_words == 2, "nested struct layout is flattened");
    expect(compiled.layout_table["Pair"].fields[1].offset_words == 1,
           "field offsets follow declaration order");
}

void testAggregatesEndToEnd() {
    std::cout << "[7] Structs, arrays, and aggregate parameters\n";
    using namespace sandbox::compiler;

    {
        const std::string src = R"(
            struct Pair { a: t40; b: t40; }
            fn main() -> t40 {
              var p: Pair = Pair { a: 4, b: 5 };
              p.b = p.a + p.b;
              return p.b;
            }
        )";
        CompileResult compiled = compileSource("phase7_struct.trit", src);
        expect(compiled.success, "struct literal and field assignment compile");
        expect(compiled.object.metadata.at("target.ast_replay_functions") == "0",
               "struct field access emits solely from optimized SSA IR");
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "struct image loads");
            const auto result = sandbox::vm::run(vm, 256);
            expect(result.halted(), "struct image halts");
            expect(regLong(vm, 13) == 9, "struct field assignment produces expected result");
        }
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              var xs: [t40; 3] = [1, 2, 3];
              xs[1] = xs[0] + xs[2];
              return xs[1];
            }
        )";
        CompileResult compiled = compileSource("phase7_array.trit", src);
        expect(compiled.success, "array literal and index assignment compile");
        expect(compiled.object.metadata.at("target.ast_replay_functions") == "0",
               "array index access emits solely from optimized SSA IR");
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "array image loads");
            const auto result = sandbox::vm::run(vm, 256);
            expect(result.halted(), "array image halts");
            expect(regLong(vm, 13) == 4, "array index assignment produces expected result");
        }
    }

    {
        const std::string src = R"(
            struct Pair { a: t40; b: t40; }
            fn sum(p: Pair) -> t40 { return p.a + p.b; }
            fn main() -> t40 {
              var p: Pair = Pair { a: 6, b: 7 };
              return sum(p);
            }
        )";
        CompileResult compiled = compileSource("phase7_aggregate_param.trit", src);
        expect(compiled.success, "aggregate parameter by pointer compiles");
        expect(compiled.object.metadata.at("target.ast_replay_functions") == "0",
               "aggregate parameter field access emits solely from optimized SSA IR");
        expect(compiled.object.metadata.at("target.ir_emitted_function_names").find("sum") != std::string::npos,
               "aggregate parameter callee is emitted from optimized SSA IR");
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "aggregate param image loads");
            const auto result = sandbox::vm::run(vm, 256);
            expect(result.halted(), "aggregate param image halts");
            expect(regLong(vm, 13) == 13, "aggregate parameter is read through pointer");
        }
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              var xs: [t40; 2] = [1, 2];
              return xs[2];
            }
        )";
        CompileResult compiled = compileSource("phase7_bounds.trit", src);
        expect(!compiled.success, "constant out-of-bounds array index fails");
        expect(hasDiagnostic(compiled.diagnostics, "out of bounds"),
               "bounds diagnostic is clear");
    }
}

void testOptimizerAndGraphColoringDetails() {
    std::cout << "[8] Optimizer passes and graph-coloring metadata\n";
    using namespace sandbox::compiler;

    Module opt;
    opt.name = "opt";
    Function fn;
    fn.name = "f";
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
    fn.blocks.push_back(block);
    opt.functions.push_back(fn);

    CompilerOptions options;
    OptimizerStats stats = optimizeModule(opt, OptimizationLevel::Basic, options);
    expect(stats.constant_folds >= 1, "optimizer folds constants");
    expect(stats.cse_hits >= 1, "optimizer performs pure CSE");
    expect(stats.branch_simplifications >= 1, "optimizer simplifies constant branches");

    Module sccp;
    sccp.name = "sccp_reachability";
    Function sccp_fn;
    sccp_fn.name = "reachable";
    BasicBlock sccp_entry;
    sccp_entry.name = "entry";
    sccp_entry.instructions.push_back(
        Instr{1, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    sccp_entry.terminator.kind = TerminatorKind::Branch3;
    sccp_entry.terminator.condition = 1;
    sccp_entry.terminator.target_neg = "dead";
    sccp_entry.terminator.target_zero = "dead";
    sccp_entry.terminator.target_pos = "live";
    BasicBlock sccp_dead;
    sccp_dead.name = "dead";
    sccp_dead.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 99});
    sccp_dead.terminator.kind = TerminatorKind::Jump;
    sccp_dead.terminator.target = "merge";
    BasicBlock sccp_live;
    sccp_live.name = "live";
    sccp_live.instructions.push_back(
        Instr{3, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 7});
    sccp_live.terminator.kind = TerminatorKind::Jump;
    sccp_live.terminator.target = "merge";
    BasicBlock sccp_merge;
    sccp_merge.name = "merge";
    Instr sccp_phi;
    sccp_phi.def = 4;
    sccp_phi.opcode = InstrOpcode::Phi;
    sccp_phi.type = TypeRef::numeric(sandbox::ir::Type::T40);
    sccp_phi.phi_incoming = {{"dead", 2}, {"live", 3}};
    sccp_merge.instructions.push_back(sccp_phi);
    sccp_merge.terminator.kind = TerminatorKind::Return;
    sccp_fn.blocks = {sccp_entry, sccp_dead, sccp_live, sccp_merge};
    sccp.functions.push_back(sccp_fn);
    expect(runSparseConditionalConstantPropagation(sccp.functions[0]) == 0,
           "path-aware SCCP keeps structural phi nodes explicit");
    expect(sccp.functions[0].blocks[0].terminator.kind == TerminatorKind::Jump &&
               sccp.functions[0].blocks[0].terminator.target == "live",
           "path-aware SCCP folds only the executable branch");
    expect(sccp.functions[0].blocks[1].instructions.front().opcode ==
               InstrOpcode::Const,
           "path-aware SCCP does not rewrite unreachable blocks");

    Function critical;
    critical.name = "critical";
    critical.return_type =
        TypeRef::numeric(sandbox::ir::Type::T40);
    critical.ir_value_ceiling = 6;
    BasicBlock critical_entry;
    critical_entry.name = "entry";
    critical_entry.instructions.push_back(
        Instr{1, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40),
              {}, 1});
    critical_entry.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40),
              {}, -1});
    critical_entry.terminator.kind =
        TerminatorKind::Branch3;
    critical_entry.terminator.condition = 2;
    critical_entry.terminator.target_neg = "merge";
    critical_entry.terminator.target_zero = "merge";
    critical_entry.terminator.target_pos = "other";
    BasicBlock critical_other;
    critical_other.name = "other";
    critical_other.instructions.push_back(
        Instr{3, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40),
              {}, 10});
    critical_other.terminator.kind =
        TerminatorKind::Jump;
    critical_other.terminator.target = "merge";
    BasicBlock critical_merge;
    critical_merge.name = "merge";
    Instr critical_phi;
    critical_phi.def = 4;
    critical_phi.opcode = InstrOpcode::Phi;
    critical_phi.type =
        TypeRef::numeric(sandbox::ir::Type::T40);
    critical_phi.phi_incoming = {
        {"entry", 1}, {"other", 3}};
    critical_merge.instructions.push_back(critical_phi);
    critical_merge.instructions.push_back(
        Instr{-1, InstrOpcode::Ret,
              TypeRef::numeric(sandbox::ir::Type::T40),
              {4}});
    critical_merge.terminator.kind =
        TerminatorKind::Return;
    critical.blocks = {
        critical_entry, critical_other, critical_merge};
    expect(splitCriticalPhiEdges(critical) == 1,
           "phi lowering splits one critical CFG edge");
    const ControlFlowGraph split_cfg =
        buildControlFlowGraph(critical);
    expect(split_cfg.invalid_targets.empty() &&
               split_cfg.predecessors.at("merge").size() == 2,
           "critical-edge split preserves a closed CFG");
    const auto split_edge = std::find_if(
        critical.blocks.begin(),
        critical.blocks.end(),
        [](const BasicBlock& candidate) {
            return candidate.name.find(
                       "_critical_phi_edge_") !=
                   std::string::npos;
        });
    expect(split_edge != critical.blocks.end() &&
               split_edge->terminator.kind ==
                   TerminatorKind::Jump &&
               split_edge->terminator.target == "merge",
           "critical-edge split creates a concrete phi-copy block");
    bool phi_uses_split_predecessor = false;
    for (const auto& incoming :
         critical.blocks[2].instructions[0].phi_incoming) {
        phi_uses_split_predecessor =
            phi_uses_split_predecessor ||
            incoming.first.find(
                "_critical_phi_edge_") !=
                std::string::npos;
    }
    expect(phi_uses_split_predecessor,
           "critical-edge split rewrites phi predecessor identity");

    Module alloc;
    alloc.name = "alloc_pressure";
    Function afn;
    afn.name = "pressure";
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
    afn.blocks.push_back(pressure);
    alloc.functions.push_back(afn);

    AllocationResult allocation = allocateRegisters(alloc);
    expect(allocation.success, "allocator succeeds with spilling enabled");
    expect(allocation.interference_edges > 0, "allocator builds interference graph");
    expect(allocation.spills > 0, "allocator reports stack spills under pressure");

    Module frame_index_module;
    frame_index_module.name = "frame_index";
    Function frame_index_function;
    frame_index_function.name = "frame_index";
    BasicBlock frame_index_block;
    frame_index_block.name = "entry";
    frame_index_block.instructions.push_back(
        Instr{1, InstrOpcode::Alloca,
              TypeRef::numeric(sandbox::ir::Type::T40)});
    frame_index_block.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40),
              {}, 7});
    frame_index_block.instructions.push_back(
        Instr{-1, InstrOpcode::Store,
              TypeRef::voidType(), {1, 2}});
    frame_index_block.terminator.kind =
        TerminatorKind::Return;
    frame_index_function.blocks.push_back(
        frame_index_block);
    frame_index_module.functions.push_back(
        frame_index_function);
    const AllocationResult frame_index_allocation =
        allocateRegisters(frame_index_module);
    expect(frame_index_allocation.success &&
               !frame_index_allocation.scalar_registers.count(1),
           "virtual frame indices consume no physical graph color");

    Module rewrite_pressure;
    rewrite_pressure.name = "rewrite_pressure";
    Function rewrite_fn;
    rewrite_fn.name = "rewrite";
    BasicBlock rewrite_block;
    rewrite_block.name = "entry";
    for (int i = 0; i < 30; ++i) {
        Instr constant;
        constant.def = i + 1;
        constant.opcode = InstrOpcode::Const;
        constant.type =
            TypeRef::numeric(sandbox::ir::Type::T40);
        constant.imm = i + 1;
        rewrite_block.instructions.push_back(constant);
    }
    ValueId sum = 31;
    rewrite_block.instructions.push_back(
        Instr{sum, InstrOpcode::Add,
              TypeRef::numeric(sandbox::ir::Type::T40), {1, 2}});
    for (int value = 3; value <= 30; ++value) {
        const ValueId next = sum + 1;
        rewrite_block.instructions.push_back(
            Instr{next, InstrOpcode::Add,
                  TypeRef::numeric(sandbox::ir::Type::T40),
                  {sum, value}});
        sum = next;
    }
    Instr rewrite_sink;
    rewrite_sink.opcode = InstrOpcode::Call;
    rewrite_sink.args = {sum};
    rewrite_sink.symbol = "sink";
    rewrite_sink.effect = Effect::Control;
    rewrite_block.instructions.push_back(rewrite_sink);
    rewrite_block.terminator.kind = TerminatorKind::Return;
    rewrite_fn.blocks.push_back(rewrite_block);
    rewrite_fn.ir_value_ceiling = sum + 1;
    rewrite_pressure.functions.push_back(rewrite_fn);

    AllocationResult rewritten_allocation =
        allocateRegistersWithSpillRewrite(rewrite_pressure);
    if (!rewritten_allocation.success) {
        std::cout << "spill rewrite rounds="
                  << rewritten_allocation.spill_rewrite_rounds
                  << " spills=" << rewritten_allocation.spills << "\n";
        for (const Diagnostic& diagnostic :
             rewritten_allocation.diagnostics) {
            std::cout << diagnostic.format() << "\n";
        }
    }
    expect(rewritten_allocation.success,
           "iterative spill rewrite reaches a colorable module");
    expect(rewritten_allocation.spill_rewrite_rounds > 0 &&
               rewritten_allocation.spill_loads > 0 &&
               rewritten_allocation.spill_stores > 0,
           "spill rewrite materializes stack loads and stores");
    bool saw_spill_load = false;
    bool saw_spill_store = false;
    for (const Instr& instr :
         rewrite_pressure.functions[0].blocks[0].instructions) {
        saw_spill_load =
            saw_spill_load || instr.opcode == InstrOpcode::SpillLoad;
        saw_spill_store =
            saw_spill_store || instr.opcode == InstrOpcode::SpillStore;
    }
    expect(saw_spill_load && saw_spill_store,
           "rewritten IR contains explicit spill operations");
    expect(verifyModule(rewrite_pressure).empty(),
           "spill-rewritten IR passes SSA verification");

    Module moves;
    moves.name = "moves";
    Function mfn;
    mfn.name = "coalesce";
    BasicBlock mb;
    mb.name = "entry";
    mb.instructions.push_back(Instr{1, InstrOpcode::Const, TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    mb.instructions.push_back(Instr{2, InstrOpcode::Copy, TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    mb.terminator.kind = TerminatorKind::Return;
    mfn.blocks.push_back(mb);
    moves.functions.push_back(mfn);
    AllocationResult coalesced = allocateRegisters(moves);
    expect(coalesced.coalesced_moves >= 1, "allocator coalesces non-interfering moves");

    Module mem2reg;
    mem2reg.name = "mem2reg";
    Function ssa_fn;
    ssa_fn.name = "diamond";
    ssa_fn.ir_value_ceiling = 7;
    BasicBlock entry;
    entry.name = "entry";
    entry.instructions.push_back(
        Instr{1, InstrOpcode::Alloca,
              TypeRef::numeric(sandbox::ir::Type::T40)});
    entry.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 1});
    entry.instructions.push_back(
        Instr{-1, InstrOpcode::Store, TypeRef::voidType(), {1, 2}});
    entry.terminator.kind = TerminatorKind::Branch3;
    entry.terminator.condition = 2;
    entry.terminator.target_neg = "left";
    entry.terminator.target_zero = "right";
    entry.terminator.target_pos = "right";
    BasicBlock left;
    left.name = "left";
    left.instructions.push_back(
        Instr{3, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 10});
    left.instructions.push_back(
        Instr{-1, InstrOpcode::Store, TypeRef::voidType(), {1, 3}});
    left.terminator.kind = TerminatorKind::Jump;
    left.terminator.target = "merge";
    BasicBlock right;
    right.name = "right";
    right.instructions.push_back(
        Instr{4, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 20});
    right.instructions.push_back(
        Instr{-1, InstrOpcode::Store, TypeRef::voidType(), {1, 4}});
    right.terminator.kind = TerminatorKind::Jump;
    right.terminator.target = "merge";
    BasicBlock merge;
    merge.name = "merge";
    merge.instructions.push_back(
        Instr{5, InstrOpcode::Load,
              TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    Instr sink;
    sink.opcode = InstrOpcode::Call;
    sink.args = {5};
    sink.symbol = "sink";
    sink.effect = Effect::Control;
    merge.instructions.push_back(sink);
    merge.terminator.kind = TerminatorKind::Return;
    ssa_fn.blocks = {entry, left, right, merge};
    mem2reg.functions.push_back(ssa_fn);

    const OptimizerStats mem2reg_stats =
        optimizeModule(mem2reg, OptimizationLevel::Basic, options);
    expect(mem2reg_stats.mem2reg_promotions == 1,
           "mem2reg promotes a proven non-escaping scalar alloca");
    int phi_count = 0;
    bool memory_op_survived = false;
    for (const BasicBlock& ssa_block : mem2reg.functions[0].blocks) {
        for (const Instr& instr : ssa_block.instructions) {
            if (instr.opcode == InstrOpcode::Phi) {
                ++phi_count;
                expect(instr.phi_incoming.size() == 2,
                       "mem2reg phi records both predecessor/value pairs");
            }
            if (instr.opcode == InstrOpcode::Alloca ||
                instr.opcode == InstrOpcode::Load ||
                instr.opcode == InstrOpcode::Store) {
                memory_op_survived = true;
            }
        }
    }
    expect(phi_count == 1, "mem2reg inserts one dominance-frontier phi");
    expect(!memory_op_survived,
           "mem2reg removes promoted alloca/load/store operations");
    expect(verifyModule(mem2reg).empty(),
           "mem2reg output passes SSA dominance verification");

    Module escaped_mem2reg;
    escaped_mem2reg.name = "escaped_mem2reg";
    Function escaped_fn;
    escaped_fn.name = "escaped";
    BasicBlock escaped_entry;
    escaped_entry.name = "entry";
    escaped_entry.instructions.push_back(
        Instr{1, InstrOpcode::Alloca,
              TypeRef::numeric(sandbox::ir::Type::T40)});
    escaped_entry.instructions.push_back(
        Instr{2, InstrOpcode::AddrOf,
              TypeRef::pointer(
                  TypeRef::numeric(
                      sandbox::ir::Type::T40)),
              {1}});
    Instr escaped_sink;
    escaped_sink.opcode = InstrOpcode::Call;
    escaped_sink.args = {2};
    escaped_sink.symbol = "sink";
    escaped_sink.effect = Effect::Control;
    escaped_entry.instructions.push_back(
        escaped_sink);
    escaped_entry.terminator.kind =
        TerminatorKind::Return;
    escaped_fn.blocks.push_back(escaped_entry);
    escaped_mem2reg.functions.push_back(escaped_fn);
    const OptimizerStats escaped_stats =
        optimizeModule(
            escaped_mem2reg,
            OptimizationLevel::Basic,
            options);
    expect(escaped_stats.mem2reg_promotions == 0,
           "address-taken allocas remain in memory");
    expect(std::any_of(
               escaped_mem2reg.functions[0]
                   .blocks[0].instructions.begin(),
               escaped_mem2reg.functions[0]
                   .blocks[0].instructions.end(),
               [](const Instr& instr) {
                   return instr.opcode ==
                          InstrOpcode::Alloca;
               }),
           "alias-blocked promotion retains the escaped frame object");

    Module loop_mem2reg;
    loop_mem2reg.name = "loop_mem2reg";
    Function loop_fn;
    loop_fn.name = "loop";
    loop_fn.ir_value_ceiling = 8;
    BasicBlock loop_entry;
    loop_entry.name = "entry";
    loop_entry.instructions.push_back(
        Instr{1, InstrOpcode::Alloca,
              TypeRef::numeric(sandbox::ir::Type::T40)});
    loop_entry.instructions.push_back(
        Instr{2, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, 0});
    loop_entry.instructions.push_back(
        Instr{-1, InstrOpcode::Store, TypeRef::voidType(), {1, 2}});
    loop_entry.terminator.kind = TerminatorKind::Jump;
    loop_entry.terminator.target = "header";
    BasicBlock loop_header;
    loop_header.name = "header";
    loop_header.instructions.push_back(
        Instr{3, InstrOpcode::Load,
              TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    loop_header.terminator.kind = TerminatorKind::Branch3;
    loop_header.terminator.condition = 3;
    loop_header.terminator.target_neg = "exit";
    loop_header.terminator.target_zero = "body";
    loop_header.terminator.target_pos = "body";
    BasicBlock loop_body;
    loop_body.name = "body";
    loop_body.instructions.push_back(
        Instr{4, InstrOpcode::Load,
              TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    loop_body.instructions.push_back(
        Instr{5, InstrOpcode::Const,
              TypeRef::numeric(sandbox::ir::Type::T40), {}, -1});
    loop_body.instructions.push_back(
        Instr{6, InstrOpcode::Add,
              TypeRef::numeric(sandbox::ir::Type::T40), {4, 5}});
    loop_body.instructions.push_back(
        Instr{-1, InstrOpcode::Store, TypeRef::voidType(), {1, 6}});
    loop_body.terminator.kind = TerminatorKind::Jump;
    loop_body.terminator.target = "header";
    BasicBlock loop_exit;
    loop_exit.name = "exit";
    loop_exit.instructions.push_back(
        Instr{7, InstrOpcode::Load,
              TypeRef::numeric(sandbox::ir::Type::T40), {1}});
    Instr loop_sink;
    loop_sink.opcode = InstrOpcode::Call;
    loop_sink.args = {7};
    loop_sink.symbol = "sink";
    loop_sink.effect = Effect::Control;
    loop_exit.instructions.push_back(loop_sink);
    loop_exit.terminator.kind = TerminatorKind::Return;
    loop_fn.blocks = {
        loop_entry, loop_header, loop_body, loop_exit};
    loop_mem2reg.functions.push_back(loop_fn);
    const OptimizerStats loop_promotions =
        optimizeModule(
            loop_mem2reg, OptimizationLevel::Basic, options);
    expect(loop_promotions.mem2reg_promotions == 1,
           "mem2reg promotes a loop-carried scalar");
    const Instr& loop_phi =
        loop_mem2reg.functions[0].blocks[1].instructions.front();
    expect(loop_phi.opcode == InstrOpcode::Phi &&
               loop_phi.phi_incoming.size() == 2,
           "mem2reg creates entry/backedge loop phi");
    expect(verifyModule(loop_mem2reg).empty(),
           "loop-phi SSA passes dominance verification");

    {
        const std::string source = R"(
            fn main() -> t40 {
              var value: t40 = 1;
              let before: t40 = value;
              value = 2;
              let after: t40 = value;
              return before + after;
            }
        )";
        CompileResult compiled =
            compileSource("source_mem2reg.trit", source);
        expect(compiled.success,
               "source-level scalar CFG compiles through SSA admission");
        expect(!compiled.optimized_module.functions.empty() &&
                   compiled.optimized_module.functions[0].cfg_complete,
               "verified source CFG is admitted to global SSA passes");
        expect(compiled.optimizer_stats.mem2reg_promotions >= 3,
               "source locals are promoted by real mem2reg");
        bool scalar_stack_op = false;
        bool return_has_value = false;
        for (const BasicBlock& source_block :
             compiled.optimized_module.functions[0].blocks) {
            for (const Instr& instr : source_block.instructions) {
                scalar_stack_op =
                    scalar_stack_op ||
                    instr.opcode == InstrOpcode::Alloca ||
                    instr.opcode == InstrOpcode::Load ||
                    instr.opcode == InstrOpcode::Store;
                if (instr.opcode == InstrOpcode::Ret)
                    return_has_value =
                        instr.args.size() == 1 &&
                        instr.args.front() >= 0;
            }
        }
        expect(!scalar_stack_op,
               "optimized source IR contains no promoted stack operations");
        expect(return_has_value,
               "source return value is explicit in structural IR");
        expect(verifyModule(compiled.optimized_module).empty(),
               "optimized source-level SSA passes dominance verification");
        expect(
            compiled.object.metadata.at(
                "target.ir_emitted_functions") == "1" &&
            compiled.object.metadata.at(
                "target.ast_replay_functions") == "0",
            "verified straight-line source emits solely from optimized IR");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source-level mem2reg differential program links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source-level mem2reg differential image loads");
            const auto run = sandbox::vm::run(vm, 512);
            expect(run.halted(),
                   "source-level mem2reg differential image halts");
            expect(regLong(vm, 13) == 3,
                   "optimized-IR source retains AST replay semantics");
        }
    }

    {
        const std::string source = R"(
            fn main() -> t40 {
              var i: t40 = 0;
              while 3 - i > 0 {
                i = i + 1;
              }
              return i;
            }
        )";
        CompileResult compiled =
            compileSource("source_loop_phi.trit", source);
        expect(compiled.success,
               "source loop-phi program compiles");
        bool saw_phi = false;
        for (const BasicBlock& block :
             compiled.optimized_module.functions[0].blocks) {
            for (const Instr& instr : block.instructions)
                saw_phi =
                    saw_phi ||
                    instr.opcode == InstrOpcode::Phi;
        }
        expect(saw_phi,
               "source loop produces an optimized SSA phi");
        expect(
            compiled.object.metadata.at(
                "target.ir_emitted_functions") == "1" &&
            compiled.object.metadata.at(
                "target.ast_replay_functions") == "0",
            "loop phi lowers through edge-local IR copies");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source loop-phi IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source loop-phi IR image loads");
            const auto run = sandbox::vm::run(vm, 512);
            expect(run.halted(),
                   "source loop-phi IR image halts");
            expect(regLong(vm, 13) == 3,
                   "edge-local phi copies preserve loop semantics");
        }
    }

    {
        const std::string source = R"(
            fn main() -> t40 {
              var value: t40 = 0;
              var i: t40 = 0;
              let p = &value;
              while 3 - i > 0 {
                *p = *p + 1;
                i = i + 1;
              }
              return *p;
            }
        )";
        CompileResult compiled =
            compileSource("source_loop_frame_memory.trit", source);
        expect(compiled.success,
               "source loop with address-taken local compiles");
        expect(compiled.object.metadata.at(
                   "target.ir_emitted_functions") == "1" &&
                   compiled.object.metadata.at(
                       "target.ast_replay_functions") == "0",
               "loop-carried frame memory lowers through optimized SSA");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source loop frame-memory image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source loop frame-memory image loads");
            const auto run = sandbox::vm::run(vm, 512);
            expect(run.halted(),
                   "source loop frame-memory image halts");
            expect(regLong(vm, 13) == 3,
                   "loop-carried frame memory preserves value semantics");
        }
    }

    {
        const std::string source = R"(
            fn choose(x: t40) -> t40 {
              var a: t40 = 1;
              var b: t40 = 2;
              if x > 0 {
                a = 10;
                b = 20;
              } else {
                a = 30;
                b = 40;
              }
              return a + b;
            }

            fn main() -> t40 {
              return choose(1) + choose(0);
            }
        )";
        CompileResult compiled =
            compileSource("source_multi_phi.trit", source);
        expect(compiled.success,
               "source multi-phi program compiles");
        const auto choose = std::find_if(
            compiled.optimized_module.functions.begin(),
            compiled.optimized_module.functions.end(),
            [](const Function& fn) {
                return fn.name == "choose";
            });
        int phi_count = 0;
        if (choose !=
            compiled.optimized_module.functions.end()) {
            for (const BasicBlock& block : choose->blocks)
                for (const Instr& instr :
                     block.instructions)
                    if (instr.opcode ==
                        InstrOpcode::Phi) {
                        ++phi_count;
                    }
        }
        expect(phi_count == 2,
               "block-local CSE preserves distinct simultaneous phis");
        expect(compiled.object.metadata.at(
                   "target.ir_emitted_function_names")
                   .find("choose") != std::string::npos,
               "multi-phi function emits from optimized SSA");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source multi-phi IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source multi-phi IR image loads");
            const auto run = sandbox::vm::run(vm, 1024);
            expect(run.halted(),
                   "source multi-phi IR image halts");
            expect(regLong(vm, 13) == 100,
                   "parallel phi copies preserve both merged values");
        }
    }

    {
        const std::string source = R"(
            fn choose_positive_or_one(x: t40) -> t40 {
              var value: t40 = 1;
              if x > 0 {
                value = 10;
              }
              return value;
            }

            fn main() -> t40 {
              return choose_positive_or_one(1) +
                     choose_positive_or_one(0);
            }
        )";
        CompileResult compiled =
            compileSource(
                "source_critical_phi_edge.trit",
                source);
        expect(compiled.success,
               "source critical-phi-edge program compiles");
        expect(compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
               "conditional phi merges and their callers emit solely from optimized IR");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source critical-phi-edge IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source critical-phi-edge IR image loads");
            const auto run = sandbox::vm::run(vm, 1024);
            expect(run.halted(),
                   "source critical-phi-edge IR image halts");
            expect(regLong(vm, 13) == 11,
                   "synthetic phi edge preserves branch merge semantics");
        }
    }

    {
        const std::string source = R"(
            fn identity(x: t40) -> t40 {
              return x;
            }

            fn main() -> t40 {
              let v01 = identity(1);
              let v02 = identity(2);
              let v03 = identity(3);
              let v04 = identity(4);
              let v05 = identity(5);
              let v06 = identity(6);
              let v07 = identity(7);
              let v08 = identity(8);
              let v09 = identity(9);
              let v10 = identity(10);
              let v11 = identity(11);
              let v12 = identity(12);
              let v13 = identity(13);
              let v14 = identity(14);
              let v15 = identity(15);
              let v16 = identity(16);
              let v17 = identity(17);
              let v18 = identity(18);
              let v19 = identity(19);
              let v20 = identity(20);
              return v01 + v02 + v03 + v04 + v05 +
                     v06 + v07 + v08 + v09 + v10 +
                     v11 + v12 + v13 + v14 + v15 +
                     v16 + v17 + v18 + v19 + v20;
            }
        )";
        CompileResult compiled =
            compileSource(
                "source_spill_rewrite.trit",
                source);
        expect(compiled.success,
               "source spill-pressure program compiles");
        expect(compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
               "spill-pressure functions emit solely from optimized IR");
        expect(std::stoi(compiled.object.metadata.at(
                   "target.spill_rewrite_rounds")) > 0 &&
               std::stoi(compiled.object.metadata.at(
                   "target.spill_loads")) > 0 &&
               std::stoi(compiled.object.metadata.at(
                   "target.spill_stores")) > 0,
               "target emission performs iterative spill rewriting");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "source spill-pressure IR image links");
        sandbox::vm::VMState vm(1024, 1024);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "source spill-pressure IR image loads");
            const auto run = sandbox::vm::run(vm, 4096);
            expect(run.halted(),
                   "source spill-pressure IR image halts");
            expect(regLong(vm, 13) == 210,
                   "spill rewrite preserves all live values");
        }
    }
}

void testConcurrencyFeatures() {
    std::cout << "[9] Concurrency features (shared, atomic_load, atomic_store)\n";
    using namespace sandbox::compiler;

    // Test 1: Successful compilation and execution of a valid atomic count increment
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

        CompileResult compiled = compileSource("phase7_concurrency_ok.trit", src);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS:" << std::endl;
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << std::endl;
            }
        }
        expect(compiled.success, "compiling atomic increment program succeeds");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "linked concurrency executable assembles");

        sandbox::vm::VMState vm(256, 4096);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "linked image loads");
            const auto result = sandbox::vm::run(vm, 2000);
            if (!result.halted() || regLong(vm, 13) != 6) {
                std::cout << "VM execution failed! Halted: " << result.halted() 
                          << ", Status: " << static_cast<int>(result.status)
                          << ", r13: " << regLong(vm, 13) << "\n";
                std::cout << "Trap reg: " << sandbox::vm::ops::toLong(vm.trap_reg) << "\n";
                std::cout << "Trap cause CSR: " << vm.cause << "\n";
                std::cout << "PC at trap: " << vm.pc << "\n";
                std::cout << "Assembled instructions:\n";
                for (size_t i = 0; i < linked.assembled.program.size(); ++i) {
                    auto iw = sandbox::isa::VersionedInstructionCodec::decode(linked.assembled.program[i], linked.assembled.isa_version);
                    std::cout << "  PC " << i << ": opcode=" << static_cast<int>(iw.opcode)
                              << " (" << sandbox::isa::opcodeToString(iw.opcode) << ")"
                              << ", rd=" << static_cast<int>(iw.rd)
                              << ", rs1=" << static_cast<int>(iw.rs1)
                              << ", rs2=" << static_cast<int>(iw.rs2)
                              << ", rs3=" << static_cast<int>(iw.rs3)
                              << ", imm=" << iw.imm
                              << ", offset=" << iw.offset << "\n";
                }
                if (vm.pc >= 0 && vm.pc < (int)linked.assembled.program.size()) {
                    auto iw = sandbox::isa::VersionedInstructionCodec::decode(linked.assembled.program[vm.pc], linked.assembled.isa_version);
                    std::cout << "Instruction at PC: opcode=" << static_cast<int>(iw.opcode) 
                              << ", fmt=" << static_cast<int>(iw.fmt) 
                              << ", rd=" << static_cast<int>(iw.rd)
                              << ", rs1=" << static_cast<int>(iw.rs1)
                              << ", rs2=" << static_cast<int>(iw.rs2)
                              << ", rs3=" << static_cast<int>(iw.rs3) << "\n";
                }
                std::cout << "Registers:\n";
                for (int r = 0; r < 27; ++r) {
                    auto val = vm.regfile.read(r);
                    std::cout << "  r" << r << ": val=" << sandbox::vm::ops::toLong(val) 
                              << ", mode=" << static_cast<int>(val.mode) << "\n";
                }
                std::cout << "Generated assembly:\n" << compiled.assembly << "\n";
            }
            expect(result.halted(), "linked image halts successfully");
            expect(regLong(vm, 13) == 6, "atomic increment returned correct value");
        }
    }

    // Test 2: Type checking diagnosis of order mismatch (weaker order passed)
    {
        const std::string src = R"(
            fn main() -> t40 {
                let counter: shared<t40, SEQ_CST> = shared_alloc(5);
                let val: t40 = atomic_load(counter, ACQ_REL);
                return val;
            }
        )";

        CompileResult compiled = compileSource("phase7_concurrency_err.trit", src);
        expect(!compiled.success, "weak memory order load is rejected");
        expect(hasDiagnostic(compiled.diagnostics, "weaker than declared order"),
               "diagnostics contain weaker order message");
    }
}

void testSysWriteChar() {
    std::cout << "[10] sys_write_char syscall 22 test\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn main() -> t40 {
            sys_write_char(65); // 'A'
            sys_write_char(66); // 'B'
            sys_write_char(10); // '\n'
            return 0;
        }
    )";

    CompileResult compiled = compileSource("phase7_sys_write_char.trit", src);
    expect(compiled.success, "sys_write_char compiles");
    expect(compiled.object.metadata.at(
               "target.ast_replay_functions") == "0",
           "syscall-only scalar function emits solely from optimized IR");
    expect(contains(compiled.assembly, "syscall 22"), "sys_write_char wrapper emits syscall 22");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "sys_write_char links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "sys_write_char image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "sys_write_char image halts");
        expect(vm.syscall_buffer == "AB\n", "console buffer has the correct characters");
    }
}

void testStrictSsaLowersExternalMemory() {
    std::cout << "[10b] Strict SSA lowers external memory beside a syscall\n";
    using namespace sandbox::compiler;
    const std::string src = R"(
        fn main() -> t40 {
            var ptr: t40 = 120;
            unsafe { store(ptr, 41); }
            sys_write_char(65);
            return 0;
        }
    )";
    CompilerOptions options;
    options.allow_ast_replay = false;
    CompileResult compiled = compileSource("phase7_strict_ssa.trit", src, options);
    expect(compiled.success,
           "strict SSA lowers external memory without replaying the AST");
    expect(compiled.object.metadata.at("target.ast_replay_functions") == "0",
           "external memory plus syscall emits solely from optimized SSA");
    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "strict SSA external-memory image links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled),
               "strict SSA external-memory image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "strict SSA external-memory image halts");
        expect(vm.syscall_buffer == "A",
               "strict SSA preserves ordered memory and syscall effects");
    }
}

void testMatchWildcard() {
    std::cout << "[11] match statement wildcard arm test\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn main() -> t40 {
            let x = 0;
            match x {
                pos => { return 10; }
                _ => { return 20; }
            }
        }
    )";

    CompileResult compiled = compileSource("phase7_match_wildcard.trit", src);
    expect(compiled.success, "match wildcard compiles");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "match wildcard links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "match wildcard image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "match wildcard image halts");
        expect(regLong(vm, 13) == 20, "wildcard arm is executed for zero value");
    }
}

void testConstants() {
    std::cout << "[12] compile-time constants (const NAME: TYPE = EXPR) test\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        const FILE_CONST: t40 = 5 * 2;
        
        fn main() -> t40 {
            const LOCAL_CONST: t10 = FILE_CONST + 1;
            let x: t40 = LOCAL_CONST;
            return x;
        }
    )";

    CompileResult compiled = compileSource("phase7_constants.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR CONSTANTS:" << std::endl;
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << std::endl;
        }
    }
    expect(compiled.success, "constants compile");
    expect(contains(compiled.assembly, "11"), "constant is folded to 11");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "constants link");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "constants image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "constants image halts");
        expect(regLong(vm, 13) == 11, "constants return correct value");
    }
}

void testParametricWidthFunctions() {
    std::cout << "[13] width-parametric functions (fn f<W: TritWidth>) test\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn double<W: TritWidth>(x: T<W>) -> T<W> {
            return x + x;
        }

        fn main() -> t50 {
            let a: t40 = 5;
            let b: t50 = 10;
            let res_a: t40 = double(a);
            let res_b: t50 = double(b);
            return res_a + res_b;
        }
    )";

    CompileResult compiled = compileSource("phase7_parametric.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR PARAMETRIC:" << std::endl;
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << std::endl;
        }
    }
    expect(compiled.success, "parametric functions compile");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "parametric functions link");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "parametric image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "parametric image halts");
        expect(regLong(vm, 13) == 30,
               "parametric functions return correct value");
    }
}

void testWidthParametricFunctionsPhaseA() {
    std::cout << "[14] Phase A width-parametric monomorphization test\n";
    using namespace sandbox::compiler;

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

    CompileResult compiled = compileSource("phaseA_width.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR PHASE A WIDTH:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
    }
    expect(compiled.success, "width-parametric accumulate compiles");
    expect(contains(compiled.assembly, "accumulate__Wt40"),
           "t40 width instantiation is emitted");
    expect(contains(compiled.assembly, "accumulate__Wt20"),
           "t20 width instantiation is emitted");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "width-parametric accumulate links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "width-parametric image loads");
        const auto result = sandbox::vm::run(vm, 512);
        expect(result.halted(), "width-parametric image halts");
        expect(regLong(vm, 13) == 17, "width-parametric calls return expected value");
    }
}

void testPointerValidationPhaseA() {
    std::cout << "[15] Phase A pointer validation and valid-arm promotion test\n";
    using namespace sandbox::compiler;

    {
        const std::string src = R"(
            fn main() -> t40 {
                let p: ptr<t40, unknown> = 1;
                return *p;
            }
        )";
        CompileResult compiled = compileSource("phaseA_ptr_unknown.trit", src);
        expect(!compiled.success, "unknown pointer dereference is rejected");
        expect(hasDiagnostic(compiled.diagnostics, "proving it is valid"),
               "unknown pointer diagnostic requests validity proof");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
                let p: ptr<t40, null> = 0;
                return *p;
            }
        )";
        CompileResult compiled = compileSource("phaseA_ptr_null.trit", src);
        expect(!compiled.success, "null pointer dereference is rejected");
        expect(hasDiagnostic(compiled.diagnostics, "proving it is valid"),
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
        CompileResult compiled = compileSource("phaseA_ptr_valid_match.trit", src);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR VALID MATCH:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "valid(p) match arm promotes pointer to valid");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
                var value: t40 = 41;
                let p = &value;
                match p {
                    null => { return -1; }
                    unknown => { return -2; }
                    valid(q) => { return *q + 1; }
                }
            }
        )";
        CompilerOptions stack_options;
        stack_options.optimization =
            OptimizationLevel::None;
        CompileResult compiled =
            compileSource(
                "phaseA_stack_address_ir.trit",
                src,
                stack_options);
        expect(compiled.success,
               "address-taken scalar source compiles");
        expect(compiled.object.metadata.at(
                   "target.ast_replay_functions") == "0",
               "address-taken scalar emits solely from optimized IR");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success,
               "address-taken scalar IR image links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(
                       vm, linked.assembled),
                   "address-taken scalar IR image loads");
            const auto run = sandbox::vm::run(vm, 512);
            expect(run.halted(),
                   "address-taken scalar IR image halts");
            expect(regLong(vm, 13) == 42,
                   "virtual frame address preserves scalar dereference semantics");
        }
    }
}

void testOwnershipAndAutoDropPhaseA() {
    std::cout << "[16] Phase A ownership move tracking and auto-drop test\n";
    using namespace sandbox::compiler;

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
        CompileResult compiled = compileSource("phaseA_owned_move.trit", src);
        expect(!compiled.success, "using an owned value after move is rejected");
        expect(hasDiagnostic(compiled.diagnostics, "use of moved value 'x'"),
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
        CompileResult compiled = compileSource("phaseA_auto_drop.trit", src);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR AUTO DROP:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "live owned value at return compiles");
        expect(contains(compiled.assembly, "call free"),
               "auto-drop inserts a call to free before returning");

        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "auto-drop program links");
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "auto-drop image loads");
            const auto result = sandbox::vm::run(vm, 512);
            expect(result.halted(), "auto-drop image halts");
            expect(regLong(vm, 13) == 7, "auto-drop preserves the explicit return value");
        }
    }
}

void testRegisterAllocationWiringPhaseA() {
    std::cout << "[17] Phase A codegen register allocation wiring test\n";
    using namespace sandbox::compiler;

    const std::string src = R"(
        fn touch(x: t40) -> t40 {
            return x + 1;
        }

        fn main() -> t40 {
            return (((((1 + 2) + 3) + 4) + 5) + 6) + touch(10);
        }
    )";

    CompileResult compiled = compileSource("phaseA_regalloc.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR REGALLOC:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
        std::cerr << compiled.assembly << "\n";
    }
    expect(compiled.success, "register allocation wiring source compiles");
    bool savesCallee = false;
    bool restoresCallee = false;
    for (int reg = 1; reg <= 12; ++reg) {
        savesCallee = savesCallee ||
            contains(compiled.assembly, "store r" + std::to_string(reg) + ", sp");
        restoresCallee = restoresCallee ||
            contains(compiled.assembly, "load r" + std::to_string(reg) + ", sp");
    }
    expect(savesCallee, "callee-saved registers beyond r19-r23 are used and saved");
    expect(restoresCallee, "callee-saved registers are restored in the epilogue");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "register allocation wiring program links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "register allocation image loads");
        const auto result = sandbox::vm::run(vm, 512);
        expect(result.halted(), "register allocation image halts");
        expect(regLong(vm, 13) == 32, "register-colored function returns expected value");
    }
}

void testUlibOwnershipRawHeapSnippetPhaseA() {
    std::cout << "[18] Phase A ulib ownership/raw heap snippet test\n";
    using namespace sandbox::compiler;

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

    CompileResult compiled = compileSource("phaseA_ulib_heap_snippet.trit", src);
    if (!compiled.success) {
        std::cerr << "COMPILE FAIL DIAGNOSTICS FOR ULIB HEAP SNIPPET:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
    }
    expect(compiled.success, "ulib ownership/raw heap snippet compiles");
    expect(contains(compiled.assembly, "call malloc_raw"),
           "raw heap allocation helper is called by internal code");
    expect(contains(compiled.assembly, "call free_raw"),
           "raw heap free helper is called by internal code");
    expect(contains(compiled.assembly, "call free"),
           "owned public value is auto-dropped through public free");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "ulib ownership/raw heap snippet links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "ulib heap snippet image loads");
        const auto result = sandbox::vm::run(vm, 512);
        expect(result.halted(), "ulib heap snippet image halts");
        expect(regLong(vm, 13) == 3, "ulib heap snippet preserves explicit return");
    }
}

std::string readTritFile(const std::string& name) {
    std::ifstream f(name);
    if (!f.is_open()) {
        f.open("../" + name);
    }
    if (!f.is_open()) {
        f.open("../../" + name);
    }
    if (!f.is_open()) {
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string readUlibTrit() {
    return readTritFile("ulib.trit");
}

void testTclTernaryErgonomicsExtensions() {
    std::cout << "[19] Phase B/C Ergonomics and stdlib extensions test\n";
    using namespace sandbox::compiler;

    std::string native_compiler_src = 
        readTritFile("ulib.trit") + "\n" +
        readTritFile("tcl_token.trit") + "\n" +
        readTritFile("tcl_lexer.trit") + "\n" +
        readTritFile("tcl_ast.trit") + "\n" +
        readTritFile("tcl_type.trit") + "\n" +
        readTritFile("tcl_parser.trit") + "\n";

    // 1. Test single-arm guards (if pos, if neg, if zero) in the native parser
    {
        expect(!native_compiler_src.empty(), "successfully read native parser source files from disk");
        std::string test_driver = R"(
            fn main() -> t40 {
                var src: t40 = malloc_raw(100);
                unsafe {
                    var s: ptr<t1, user, valid> = src;
                    store(s + 0, 102); store(s + 1, 110); store(s + 2, 32); store(s + 3, 102);
                    store(s + 4, 40); store(s + 5, 41); store(s + 6, 32); store(s + 7, 45);
                    store(s + 8, 62); store(s + 9, 32); store(s + 10, 118); store(s + 11, 111);
                    store(s + 12, 105); store(s + 13, 100); store(s + 14, 32); store(s + 15, 123);
                    store(s + 16, 32); store(s + 17, 105); store(s + 18, 102); store(s + 19, 32);
                    store(s + 20, 112); store(s + 21, 111); store(s + 22, 115); store(s + 23, 40);
                    store(s + 24, 53); store(s + 25, 41); store(s + 26, 32); store(s + 27, 123);
                    store(s + 28, 32); store(s + 29, 114); store(s + 30, 101); store(s + 31, 116);
                    store(s + 32, 117); store(s + 33, 114); store(s + 34, 110); store(s + 35, 59);
                    store(s + 36, 32); store(s + 37, 125); store(s + 38, 32); store(s + 39, 125);
                }
                var tokens: t40 = tcl_lex(src, 40);
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

        CompileResult compiled = compileSource("native_guard_test.trit", native_compiler_src + test_driver);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR NATIVE GUARD TEST:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "native guard test compiles successfully");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "native guard test links");
        sandbox::vm::VMState vm(65536, 1000000);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "native guard test VM image loads");
            const auto result = sandbox::vm::run(vm, 500000);
            expect(result.halted(), "native guard test halts");
            expect(regLong(vm, 13) == 1, "native parser successfully parses if pos block");
        }
    }

    // 2. Test guard syntax diagnostics for invalid guard keyword in the native parser
    {
        std::string test_driver = R"(
            fn main() -> t40 {
                var src: t40 = malloc_raw(100);
                unsafe {
                    var s: ptr<t1, user, valid> = src;
                    store(s + 0, 102); store(s + 1, 110); store(s + 2, 32); store(s + 3, 102);
                    store(s + 4, 40); store(s + 5, 41); store(s + 6, 32); store(s + 7, 45);
                    store(s + 8, 62); store(s + 9, 32); store(s + 10, 118); store(s + 11, 111);
                    store(s + 12, 105); store(s + 13, 100); store(s + 14, 32); store(s + 15, 123);
                    store(s + 16, 32); store(s + 17, 105); store(s + 18, 102); store(s + 19, 32);
                    store(s + 20, 111); store(s + 21, 116); store(s + 22, 104); store(s + 23, 101);
                    store(s + 24, 114); store(s + 25, 40); store(s + 26, 53); store(s + 27, 41);
                    store(s + 28, 32); store(s + 29, 123); store(s + 30, 32); store(s + 31, 114);
                    store(s + 32, 101); store(s + 33, 116); store(s + 34, 117); store(s + 35, 114);
                    store(s + 36, 110); store(s + 37, 59); store(s + 38, 32); store(s + 39, 125);
                    store(s + 40, 32); store(s + 41, 125);
                }
                var tokens: t40 = tcl_lex(src, 42);
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

        CompileResult compiled = compileSource("native_bad_guard_test.trit", native_compiler_src + test_driver);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR NATIVE BAD GUARD TEST:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "native bad guard test compiles successfully");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "native bad guard test links");
        sandbox::vm::VMState vm(65536, 1000000);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "native bad guard test VM image loads");
            const auto result = sandbox::vm::run(vm, 500000);
            expect(result.halted(), "native bad guard test halts");
            expect(regLong(vm, 13) == 1, "native parser flags invalid guard keyword with error code 29");
        }
    }

    // 3. Test vec_sort_3way using ulib.trit
    {
        std::string ulib_src = readUlibTrit();
        expect(!ulib_src.empty(), "successfully read ulib.trit from disk");

        std::string sort_test_src = ulib_src + R"(
            fn main() -> t40 {
                var v: t40 = vec_new();
                sys_write_int(v); sys_newline();
                
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

                var len: t40 = 0;
                unsafe { len = load(v + 1); }
                sys_write_int(len); sys_newline();

                vec_sort(v);

                var data: t40 = 0;
                unsafe {
                    data = load(v);
                    len = load(v + 1);
                }
                
                var i: t40 = 0;
                while len - i > 0 {
                    var val: t40 = 0;
                    unsafe { val = load(data + i); }
                    sys_write_int(val); sys_newline();
                    i = i + 1;
                }

                vec_free(v);
                return 1;
            }
        )";

        CompileResult compiled = compileSource("sort_test.trit", sort_test_src);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR SORT TEST:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "ulib + sort test compiles successfully");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "sort test links");
        sandbox::vm::VMState vm(65536, 1000000);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "sort test VM image loads");
            const auto result = sandbox::vm::run(vm, 50000);
            std::cout << "VM SYSCALL BUFFER FOR SORT:\n" << vm.syscall_buffer << "\n";
            expect(result.halted(), "sort test halts");
            std::cout << "DEBUG: sort test reg 13 = " << regLong(vm, 13) << "\n";
            expect(regLong(vm, 13) == 1, "vec_sort correctly sorts elements in ascending order");
        }
    }

    // 4. Test SplitBuf push/pop/gap/steal API using ulib.trit
    {
        std::string ulib_src = readUlibTrit();
        std::string split_test_src = ulib_src + R"(
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

        CompileResult compiled = compileSource("split_test.trit", split_test_src);
        if (!compiled.success) {
            std::cerr << "COMPILE FAIL DIAGNOSTICS FOR SPLIT BUFFER TEST:\n";
            for (const auto& diag : compiled.diagnostics) {
                std::cerr << "  " << diag.format() << "\n";
            }
        }
        expect(compiled.success, "ulib + split buffer test compiles successfully");
        LinkResult linked = linkModules({compiled.object});
        expect(linked.success, "split buffer test links");
        sandbox::vm::VMState vm(65536, 1000000);
        if (linked.success) {
            expect(sandbox::vm::assembler::loadAndReset(vm, linked.assembled), "split buffer test VM image loads");
            const auto result = sandbox::vm::run(vm, 500000);
            if (!result.halted()) {
                std::cout << "SPLIT DEBUG status="
                          << static_cast<int>(result.status)
                          << " pc=" << vm.pc
                          << " cause=" << vm.cause
                          << " ir_functions="
                          << compiled.object.metadata.at(
                                 "target.ir_emitted_function_names")
                          << "\n";
            }
            expect(result.halted(), "split buffer test halts");
            expect(regLong(vm, 13) == 1, "SplitBuf push/pop and zero-zone steal operations work correctly");
        }
    }
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    sandbox::LongTriple::initPowTable();

    testCompileAndRunMatchProgram();
    testSideEffectfulMatchKeepsBranchLowering();
    testRuntimeSyscallWrapperAndTupleSwap();
    testFunctionCallAndWhileLoop();
    testDirectRawMemoryAddressLowering();
    testIfElseStatements();
    testTypeDiagnostics();
    testVerifierAllocatorAndDuplicateSymbols();
    testHMGeneralizationAndLayouts();
    testAggregatesEndToEnd();
    testOptimizerAndGraphColoringDetails();
    testConcurrencyFeatures();
    testSysWriteChar();
    testStrictSsaLowersExternalMemory();
    testMatchWildcard();
    testConstants();
    testParametricWidthFunctions();
    testWidthParametricFunctionsPhaseA();
    testPointerValidationPhaseA();
    testOwnershipAndAutoDropPhaseA();
    testRegisterAllocationWiringPhaseA();
    testUlibOwnershipRawHeapSnippetPhaseA();
    testTclTernaryErgonomicsExtensions();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " Phase 7 compiler test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll Phase 7 compiler tests passed\n";
    return EXIT_SUCCESS;
}
