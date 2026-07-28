#include "test_multiwidth_vm_common.h"
#include "ternary_compiler.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int g_failures = 0;

namespace {

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

std::string formatDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << "  " << diagnostic.format() << "\n";
    }
    return out.str();
}

long long compileAndRunReturn(const std::string& name,
                              const std::string& source,
                              int steps,
                              int imem_words,
                              int dmem_words) {
    using namespace sandbox::compiler;

    CompilerOptions options;
    options.optimization = OptimizationLevel::None;
    CompileResult compiled = compileSource(name, source, options);
    if (!compiled.success) {
        std::cout << "COMPILE FAIL [" << name << "]\n"
                  << formatDiagnostics(compiled.diagnostics);
        expect(false, name + " compiles");
        return -999999;
    }

    LinkResult linked = linkModules({compiled.object});
    if (!linked.success) {
        std::cout << "LINK FAIL [" << name << "]\n"
                  << formatDiagnostics(linked.diagnostics);
        expect(false, name + " links");
        return -999999;
    }

    sandbox::vm::VMState vm(imem_words, dmem_words);
    if (!sandbox::vm::assembler::loadAndReset(vm, linked.assembled)) {
        expect(false, name + " loads");
        return -999999;
    }

    const auto result = sandbox::vm::run(vm, steps);
    if (!result.halted()) {
        std::cout << "RUN FAIL [" << name << "] status="
                  << static_cast<int>(result.status)
                  << " pc=" << vm.pc
                  << " r13=" << regLong(vm, 13)
                  << " heap_break=" << vm.standalone_heap_break << "\n";
        expect(false, name + " halts");
        return -999999;
    }

    return regLong(vm, 13);
}

std::string compactAllocatorSource(bool backward_header_math) {
    const std::string header_mapping = backward_header_math
        ? R"(
            var base: t40 = sys_sbrk(sz + 1);
            var hdr: t40 = base - sz - 1;
          )"
        : R"(
            var base: t40 = sys_sbrk(sz + 1);
            var hdr: t40 = base;
          )";

    return R"(
        fn heap_init() -> t40 {
            unsafe {
                match load(20) {
                    zero => { store(20, 1); store(22, 0); }
                    neg => {}
                    pos => {}
                }
            }
            return 1;
        }

        fn malloc_raw(words: t40) -> t40 {
            heap_init();
            var sz: t40 = words;
            match sz - 1 {
                neg => { sz = 1; }
                zero => {}
                pos => {}
            }
    )" + header_mapping + R"(
            unsafe {
                store(100, base);
                store(101, hdr);
                store(hdr, 0 - sz);
            }
            return hdr + 1;
        }

        fn main() -> t40 {
            var ptr: t40 = malloc_raw(2);
            var base: t40 = 0;
            var hdr: t40 = 0;
            unsafe {
                base = load(100);
                hdr = load(101);
            }
            match hdr - base {
                zero => { return 1; }
                neg => { return 0 - 1; }
                pos => { return 0 - 2; }
            }
        }
    )";
}

std::string allocatorInvariantMain() {
    return R"(
        fn check_eq(value: t40, expected: t40, code: t40) -> t40 {
            match value - expected {
                zero => { return 1; }
                neg => { return 0 - code; }
                pos => { return 0 - code; }
            }
        }

        fn check_positive(value: t40, code: t40) -> t40 {
            match value {
                pos => { return 1; }
                zero => { return 0 - code; }
                neg => { return 0 - code; }
            }
        }

        fn check_free_list(limit: t40) -> t40 {
            var curr: t40 = 0;
            unsafe { curr = load(22); }
            var count: t40 = 0;
            while curr > 0 {
                count = count + 1;
                match count - limit {
                    pos => { return 0 - 10; }
                    zero => {}
                    neg => {}
                }

                var size: t40 = 0;
                var next: t40 = 0;
                unsafe {
                    size = load(curr);
                    next = load(curr + 1);
                }
                match size {
                    pos => {}
                    zero => { return 0 - 11; }
                    neg => { return 0 - 11; }
                }
                match next - curr {
                    zero => { return 0 - 12; }
                    neg => {}
                    pos => {}
                }
                match next {
                    neg => { return 0 - 13; }
                    zero => {}
                    pos => {}
                }
                curr = next;
            }
            return 1;
        }

        fn main() -> t40 {
            var a: t40 = malloc_raw(2);
            var b: t40 = malloc_raw(3);
            var code: t40 = check_positive(a, 1);
            match code { neg => { return code; } zero => { return 0 - 20; } pos => {} }
            code = check_positive(b, 2);
            match code { neg => { return code; } zero => { return 0 - 21; } pos => {} }

            var ah: t40 = 0;
            var bh: t40 = 0;
            unsafe {
                ah = load(a - 1);
                bh = load(b - 1);
                store(a, 111);
                store(a + 1, 222);
                store(b, 333);
            }
            code = check_eq(ah, 0 - 2, 3);
            match code { neg => { return code; } zero => { return 0 - 22; } pos => {} }
            code = check_eq(bh, 0 - 3, 4);
            match code { neg => { return code; } zero => { return 0 - 23; } pos => {} }

            free_raw(b);
            code = check_free_list(16);
            match code { neg => { return code; } zero => { return 0 - 24; } pos => {} }

            var c: t40 = malloc_raw(1);
            code = check_positive(c, 5);
            match code { neg => { return code; } zero => { return 0 - 25; } pos => {} }
            code = check_eq(c, b, 6);
            match code { neg => { return code; } zero => { return 0 - 26; } pos => {} }

            var av0: t40 = 0;
            var av1: t40 = 0;
            unsafe {
                av0 = load(a);
                av1 = load(a + 1);
            }
            code = check_eq(av0, 111, 7);
            match code { neg => { return code; } zero => { return 0 - 27; } pos => {} }
            code = check_eq(av1, 222, 8);
            match code { neg => { return code; } zero => { return 0 - 28; } pos => {} }

            free_raw(a);
            free_raw(c);
            code = check_free_list(16);
            match code { neg => { return code; } zero => { return 0 - 29; } pos => {} }
            return 1;
        }
    )";
}

void testStandaloneSbrkContract() {
    const std::string source = R"(
        .text
        _start:
            mov.t40 r13, 3
            syscall 19
            copy r2, r13
            mov.t40 r13, 2
            syscall 19
            copy r3, r13
            halt
    )";

    auto assembled = sandbox::vm::assembler::assemble(source);
    expect(assembled.success, "standalone sbrk contract assembly parses");
    if (!assembled.success) return;

    for (int run_idx = 0; run_idx < 2; ++run_idx) {
        sandbox::vm::VMState vm(128, 4096);
        expect(sandbox::vm::assembler::loadAndReset(vm, assembled),
               "standalone sbrk contract image loads");
        const auto result = sandbox::vm::run(vm, 64);
        expect(result.halted(), "standalone sbrk contract image halts");
        expect(regLong(vm, 2) == 2000,
               "first sbrk returns initial fresh-region base");
        expect(regLong(vm, 3) == 2003,
               "second sbrk returns next fresh-region base");
        expect(vm.standalone_heap_break == 2005,
               "standalone heap break advances per VM");
    }
}

void testHeaderMappingContract() {
    const long long buggy = compileAndRunReturn(
        "malloc_backward_header_contract.trit",
        compactAllocatorSource(true), 4096, 4096, 4096);
    expect(buggy < 0,
           "backward header math violates the sbrk base contract");

    const long long fixed = compileAndRunReturn(
        "malloc_forward_header_contract.trit",
        compactAllocatorSource(false), 4096, 4096, 4096);
    expect(fixed == 1,
           "forward header mapping satisfies the sbrk base contract");
}

void testActualUlibAllocatorInvariants() {
    const std::string ulib = readTextFile("ulib.trit");
    expect(!ulib.empty(), "ulib.trit is available for allocator invariant test");
    if (ulib.empty()) return;

    const long long result = compileAndRunReturn(
        "ulib_allocator_invariants.trit",
        ulib + "\n" + allocatorInvariantMain(),
        200000, 65536, 65536);
    expect(result == 1,
           "actual ulib allocator preserves headers, free-list shape, and live data");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testStandaloneSbrkContract();
    testHeaderMappingContract();
    testActualUlibAllocatorInvariants();

    if (g_failures == 0) {
        std::cout << "All malloc/sbrk micro-contract tests passed\n";
    } else {
        std::cout << g_failures << " malloc/sbrk micro-contract test failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
