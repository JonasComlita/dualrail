#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

constexpr int kAsmSourceLenAddr = 1599999;
constexpr int kAsmSourceBase = 1600000;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

std::string readRepoText(const std::string& name) {
    for (const std::string& path : {name, "../" + name, "../../" + name}) {
        std::ifstream in(path);
        if (!in.good()) continue;
        std::string out;
        std::string line;
        while (std::getline(in, line)) {
            out += line;
            out += '\n';
        }
        return out;
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
                          sandbox::vm::ops::fromLong(static_cast<unsigned char>(source[i]))) !=
            sandbox::vm::MemFaultCode::OK) {
            return false;
        }
    }
    return vm.dmem.store(kAsmSourceBase + static_cast<int>(source.size()),
                         sandbox::vm::ops::fromLong(0)) == sandbox::vm::MemFaultCode::OK;
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

std::vector<long long> assembleWithNativeTcl(const std::string& tasm) {
    using namespace sandbox::compiler;

    CompilerOptions options;
    options.optimization = OptimizationLevel::None;
    CompileResult compiled =
        compileSource("native_tcl_assembler_harness.trit",
                      nativeAssemblerHarnessSource(),
                      options);
    if (!compiled.success) {
        std::cout << "COMPILE FAIL [native-tcl-assembler]\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            std::cout << "  " << diagnostic.format() << "\n";
        }
        expect(false, "native TCL assembler harness compiles");
        return {};
    }

    LinkResult linked = linkModules({compiled.object});
    if (!linked.success) {
        std::cout << "LINK FAIL [native-tcl-assembler]\n";
        for (const auto& diagnostic : linked.diagnostics) {
            std::cout << "  " << diagnostic.format() << "\n";
        }
        expect(false, "native TCL assembler harness links");
        return {};
    }

    sandbox::vm::VMState vm(262144, 2097152);
    vm.block_cache_enabled = false;
    if (!sandbox::vm::assembler::loadAndReset(vm, linked.assembled)) {
        expect(false, "native TCL assembler image loads");
        return {};
    }
    if (!seedAsmSource(vm, tasm)) {
        expect(false, "TASM source seeds into native assembler VM");
        return {};
    }

    const auto result = sandbox::vm::run(vm, 20000000);
    if (!result.halted()) {
        std::cout << "RUN FAIL [native-tcl-assembler]"
                  << " status=" << static_cast<int>(result.status)
                  << " steps=" << result.steps
                  << " pc=" << vm.pc
                  << " buffer='" << vm.syscall_buffer.substr(0, 400) << "'\n";
        expect(false, "native TCL assembler halts");
        return {};
    }

    return parsePrintedWords(vm.syscall_buffer);
}

void expectNativeAssemblerMatchesCpp(const std::string& name,
                                     const std::string& tasm) {
    std::cout << "[tcl_asm] " << name << "\n";

    auto cpp = sandbox::vm::assembler::assemble(tasm);
    if (!cpp.success) {
        std::cout << "C++ ASSEMBLY FAIL " << name << "\n";
        for (const auto& error : cpp.errors) std::cout << "  " << error.format() << "\n";
        expect(false, "C++ assembler accepts " + name);
        return;
    }

    const std::vector<long long> native = assembleWithNativeTcl(tasm);
    expect(native.size() == cpp.program.size(), "native word count matches " + name);
    const std::size_t n = std::min(native.size(), cpp.program.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto want = semanticTritWord(cpp.program[i]);
        expect(native[i] == want,
               name + " word " + std::to_string(i) +
                   " got " + std::to_string(native[i]) +
                   " want " + std::to_string(want));
    }
}

void runTests() {
    expect(!readRepoText("tcl_asm.trit").empty(), "tcl_asm.trit is present");

    expectNativeAssemblerMatchesCpp("scalar branch loop", R"(
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

    expectNativeAssemblerMatchesCpp("typed scalar suffixes and mov word counts", R"(
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

    expectNativeAssemblerMatchesCpp("calls memory and comments", R"(
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

    expectNativeAssemblerMatchesCpp("three way branch", R"(
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
    runTests();
    if (g_failures == 0) {
        std::cout << "OK\n";
    } else {
        std::cout << g_failures << " failure(s)\n";
    }
    return g_failures == 0 ? 0 : 1;
}
