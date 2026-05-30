#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

long long regLong(const sandbox::vm::VMState& vm, int reg) {
    return sandbox::vm::ops::toLong(vm.regfile.read(static_cast<uint8_t>(reg)));
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();

    int step_limit = 500000;
    if (argc > 1) {
        step_limit = std::stoi(argv[1]);
    }

    const std::string kernel = readTextFile("kernel.trit");
    const std::string boot = readTextFile("OS3/native_kernel_boot.tasm");
    const std::string trap = readTextFile("OS3/native_kernel_trap_stub.tasm");
    if (kernel.empty() || boot.empty() || trap.empty()) {
        std::cerr << "Failed to read Phase D kernel sources.\n";
        return 1;
    }

    sandbox::compiler::CompileResult compiled =
        sandbox::compiler::compileSource("kernel.trit", kernel);
    if (!compiled.success) {
        std::cerr << "kernel.trit compilation failed:\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "  " << diag.format() << "\n";
        }
        return 1;
    }

    auto assembled = sandbox::vm::assembler::assemble(boot + "\n" + trap + "\n" + compiled.assembly);
    if (!assembled.success) {
        std::cerr << "Native kernel assembly failed:\n";
        for (const auto& error : assembled.errors) {
            std::cerr << "  line " << error.line << ": " << error.message << "\n";
        }
        return 1;
    }

    sandbox::vm::VMState vm(262144, 1000000);
    if (!sandbox::vm::assembler::loadAndReset(vm, assembled)) {
        std::cerr << "Failed to load native Phase D kernel image.\n";
        return 1;
    }

    const auto result = sandbox::vm::run(vm, step_limit);
    if (!result.halted()) {
        std::cerr << "Native Phase D kernel did not halt within " << step_limit
                  << " steps. pc=" << vm.pc
                  << " trap=" << sandbox::vm::ops::toLong(vm.trap_reg) << "\n";
        return 1;
    }

    std::cout << "Native Phase D kernel booted and returned from sys_stat(\"/\").\n";
    std::cout << "r13(status)=" << regLong(vm, 13)
              << " r14(payload)=" << regLong(vm, 14)
              << " r15(detail)=" << regLong(vm, 15) << "\n";
    return regLong(vm, 13) == 1 ? 0 : 1;
}
