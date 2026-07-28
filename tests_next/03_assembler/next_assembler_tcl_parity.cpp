#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;

constexpr int kAsmSourceLenAddr = 1599999;
constexpr int kAsmSourceBase = 1600000;

std::string readRepoText(const std::string& name) {
    for (const std::string& path : {name, "../" + name, "../../" + name}) {
        std::ifstream in(path);
        if (!in.good()) continue;
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }
    return {};
}

std::string nativeAssemblerHarnessSource() {
    const std::string driver = R"(
        const ASM_SOURCE_LEN_ADDR: t40 = 1599999;
        const ASM_SOURCE_BASE: t40 = 1600000;

        fn main() -> t40 {
            var len: t40 = 0;
            unsafe { len = load(ASM_SOURCE_LEN_ADDR); }
            var image: t40 = tcl_asm_assemble_to_words(ASM_SOURCE_BASE, len);
            var n: t40 = vec_len(image);
            var i: t40 = 0;
            while n - i > 0 {
                sys_write_int(tcl_asm_words_get(image, i));
                sys_newline();
                i = i + 1;
            }
            return n;
        }
    )";

    return readRepoText("ulib_mini.trit") + "\n" +
           readRepoText("tcl_asm.trit") + "\n" +
           driver;
}

bool seedAsmSource(sandbox::vm::VMState& vm, const std::string& source) {
    if (kAsmSourceBase + static_cast<int>(source.size()) + 1 >= vm.dmem.size()) {
        return false;
    }
    if (vm.dmem.store(kAsmSourceLenAddr,
                      sandbox::vm::ops::fromLong(static_cast<long long>(source.size()))) !=
        sandbox::vm::MemFaultCode::OK) {
        return false;
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (vm.dmem.store(kAsmSourceBase + static_cast<int>(i),
                          sandbox::vm::ops::fromLong(
                              static_cast<unsigned char>(source[i]))) !=
            sandbox::vm::MemFaultCode::OK) {
            return false;
        }
    }
    return vm.dmem.store(kAsmSourceBase + static_cast<int>(source.size()),
                         sandbox::vm::ops::fromLong(0)) ==
           sandbox::vm::MemFaultCode::OK;
}

std::vector<long long> parsePrintedWords(const std::string& text) {
    std::vector<long long> words;
    std::istringstream in(text);
    long long value = 0;
    while (in >> value) words.push_back(value);
    return words;
}

long long semanticTritWord(const sandbox::isa::TritWord27& word) {
    long long out = 0;
    long long weight = 1;
    for (int i = 0; i < sandbox::isa::ISA_WORD_TRITS; ++i) {
        out += static_cast<long long>(word.getTrit(i)) * weight;
        weight *= 3;
    }
    return out;
}

std::vector<long long> assembleWithNativeTcl(TestContext& ctx,
                                             const std::string& tasm) {
    using namespace sandbox::compiler;

    CompilerOptions options;
    options.optimization = OptimizationLevel::None;
    CompileResult compiled =
        compileSource("next_native_tcl_assembler_harness.trit",
                      nativeAssemblerHarnessSource(),
                      options);
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics) {
            ctx.fail("native TCL assembler compile diagnostic: " +
                     diagnostic.format());
        }
        return {};
    }

    LinkResult linked = linkModules({compiled.object});
    if (!linked.success) {
        for (const auto& diagnostic : linked.diagnostics) {
            ctx.fail("native TCL assembler link diagnostic: " +
                     diagnostic.format());
        }
        return {};
    }

    sandbox::vm::VMState vm(262144, 2097152);
    vm.block_cache_enabled = false;
    if (!sandbox::vm::assembler::loadAndReset(vm, linked.assembled)) {
        ctx.fail("native TCL assembler image loads");
        return {};
    }
    if (!seedAsmSource(vm, tasm)) {
        ctx.fail("TASM source seeds into native TCL assembler VM");
        return {};
    }

    const auto result = sandbox::vm::run(vm, 20000000);
    if (!result.halted()) {
        ctx.fail("native TCL assembler halts status=" +
                 std::to_string(static_cast<int>(result.status)) +
                 " steps=" + std::to_string(result.steps) +
                 " pc=" + std::to_string(vm.pc));
        return {};
    }

    return parsePrintedWords(vm.syscall_buffer);
}

void expectNativeAssemblerMatchesCpp(TestContext& ctx,
                                     const std::string& name,
                                     const std::string& tasm) {
    const auto cpp = sandbox::vm::assembler::assemble(tasm);
    ctx.check(cpp.success, "C++ assembler accepts " + name);
    if (!cpp.success) return;

    const std::vector<long long> native = assembleWithNativeTcl(ctx, tasm);
    ctx.equal(native.size(), cpp.program.size(),
              "native word count matches " + name);
    const std::size_t n = std::min(native.size(), cpp.program.size());
    for (std::size_t i = 0; i < n; ++i) {
        ctx.equal(native[i], semanticTritWord(cpp.program[i]),
                  name + " semantic word " + std::to_string(i));
    }
}

void scalarBranchLoopParity(TestContext& ctx) {
    ctx.check(!readRepoText("tcl_asm.trit").empty(), "tcl_asm.trit is present");
    expectNativeAssemblerMatchesCpp(ctx, "scalar branch loop", R"(
        .text
        _start:
            MOV r1, 3
            MOV r2, 0
            MOV r3, 1
        loop:
            ADD r2, r2, r1
            SUB r1, r1, r3
            TCMP r4, r1, r0
            BRP r4, loop
            SYSCALL 1
            HALT
    )");
}

void typedSuffixMovParity(TestContext& ctx) {
    expectNativeAssemblerMatchesCpp(ctx, "typed suffixes and mov word counts", R"(
        .text
        _start:
            mov.t5 r1, 3
            mov.t5 r2, 0
            mov.t5 r3, 1
        loop:
            add.t5 r2, r2, r1
            sub.t5 r1, r1, r3
            tcmp.t5 r4, r1, r0
            BRP r4, loop
            mov.t20 r5, 1000
            add.t20 r5, r5, r3
            mov.t40 r8, 42
            add.t40 r8, r8, r5
            mov.t10 r6, 9
            neg.t10 r6, r6
            mov.t50 r7, 2
            mul.t50 r7, r7, r7
            HALT
    )");
}

void callsMemoryCommentsParity(TestContext& ctx) {
    expectNativeAssemblerMatchesCpp(ctx, "calls memory and comments", R"(
        .text
        _start:
            MOV sp, 100
            CALL fn_1 ; call with mixed case labels
            HALT
        fn_1:
            STORE r25, sp, 0
            MOV r13, -42
            LOAD r1, sp, 0
            COPY lr, r1
            RET
    )");
}

void threeWayBranchParity(TestContext& ctx) {
    expectNativeAssemblerMatchesCpp(ctx, "three way branch", R"(
        .text
        _start:
            MOV r1, 0
            BRN r1, neg_label
            BRZ r1, zero_label
            BRP r1, pos_label
        neg_label:
            MOV r2, -1
            JMP done
        zero_label:
            MOV r2, 0
            JMP done
        pos_label:
            MOV r2, 1
        done:
            HALT
    )");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"assembler.tcl.scalar_branch_loop_parity", "assembler.golden_contract", scalarBranchLoopParity},
        {"assembler.tcl.typed_suffix_mov_parity", "assembler.golden_contract", typedSuffixMovParity},
        {"assembler.tcl.calls_memory_comments_parity", "assembler.golden_contract", callsMemoryCommentsParity},
        {"assembler.tcl.three_way_branch_parity", "assembler.golden_contract", threeWayBranchParity},
    };
    return tests_next::runCases("next_assembler_tcl_parity", cases);
}
