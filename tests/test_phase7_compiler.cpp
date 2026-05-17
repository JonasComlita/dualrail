#include "ternary_compiler.h"
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
          match sign(x) {
            neg => { return -1; }
            zero => { return 0; }
            pos => { return x + 4; }
          }
        }
    )";

    CompileResult compiled = compileSource("phase7_match.trit", src);
    expect(compiled.success, "match source compiles");
    expect(compiled.ssa_module.functions.size() == 1, "compile result includes SSA function");
    expect(contains(compiled.assembly, "brn"), "match lowers negative branch");
    expect(contains(compiled.assembly, "brz"), "match lowers zero branch");
    expect(contains(compiled.assembly, "brp"), "match lowers positive branch");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "linked match executable assembles");
    expect(linked.executable_header.text_pages == 1, "linker emits executable header");
    expect(linked.instruction_count > 0, "linker reports instruction count");

    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "linked image loads");
        const auto result = sandbox::vm::run(vm, 256);
        expect(result.halted(), "linked image halts through standalone _start");
        expect(regLong(vm, 13) == 7, "main return value is preserved in r13");
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
    expect(contains(compiled.assembly, "swap"), "tuple swap lowers to SWAP");
    expect(contains(compiled.assembly, "syscall 1"), "sys_write_int wrapper emits syscall 1");
    expect(contains(compiled.assembly, "copy r13"), "syscall wrapper uses r13 ABI");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "runtime executable links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "runtime image loads");
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

        fn main() -> t40 {
          var i: t40 = 0;
          while pos(3 - i) {
            i = inc(i);
          }
          return i;
        }
    )";

    CompileResult compiled = compileSource("phase7_call_loop.trit", src);
    expect(compiled.success, "function call and while source compiles");
    expect(contains(compiled.assembly, "call inc"), "direct function call lowers to CALL");
    expect(contains(compiled.assembly, "brp"), "while pos lowers to positive branch");

    LinkResult linked = linkModules({compiled.object});
    expect(linked.success, "function call and while executable links");
    sandbox::vm::VMState vm(256, 256);
    if (linked.success) {
        expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "call/loop image loads");
        const auto result = sandbox::vm::run(vm, 512);
        expect(result.halted(), "call/loop image halts");
        expect(regLong(vm, 13) == 3, "while loop and direct call produce expected result");
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
              match sign(x) {
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
                let cause_value = csr_read(cause);
                fence(1);
              }
              return 0;
            }
        )";
        CompileResult compiled = compileSource("phase7_unsafe_ok.trit", src);
        expect(compiled.success, "raw CSR intrinsic inside unsafe compiles");
        expect(contains(compiled.assembly, "csrr"), "unsafe csr_read lowers to CSRR");
        expect(contains(compiled.assembly, "fence.+1"), "unsafe fence lowers memory order");
    }

    {
        const std::string src = R"(
            fn main() -> t40 {
              var addr: t40 = 12;
              unsafe {
                let old = tldr(addr, 1);
                let status = tstr(addr, 33, old, -1);
              }
              return 0;
            }
        )";
        CompileResult compiled = compileSource("phase7_atomic.trit", src);
        expect(compiled.success, "raw atomic intrinsics inside unsafe compile");
        expect(contains(compiled.assembly, "tldr.+1"), "unsafe tldr lowers memory order");
        expect(contains(compiled.assembly, "tstr.-1"), "unsafe tstr lowers memory order");
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
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "struct image loads");
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
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "array image loads");
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
        LinkResult linked = linkModules({compiled.object});
        sandbox::vm::VMState vm(256, 256);
        if (linked.success) {
            expect(sandbox::vm::loadAndReset(vm, linked.assembled.program), "aggregate param image loads");
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
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testCompileAndRunMatchProgram();
    testRuntimeSyscallWrapperAndTupleSwap();
    testFunctionCallAndWhileLoop();
    testTypeDiagnostics();
    testVerifierAllocatorAndDuplicateSymbols();
    testHMGeneralizationAndLayouts();
    testAggregatesEndToEnd();
    testOptimizerAndGraphColoringDetails();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " Phase 7 compiler test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll Phase 7 compiler tests passed\n";
    return EXIT_SUCCESS;
}
