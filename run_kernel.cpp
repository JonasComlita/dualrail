#include "ternary_asm.h"
#include "ternary_vm.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <exception>

namespace {

std::string readTextFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) return {};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::vm;

    std::cout << "===========================================\n";
    std::cout << "   Ternary OS3 Minimal Kernel Runner\n";
    std::cout << "===========================================\n\n";
    
    std::string kernel_path = "OS3/minimal_kernel_bringup.tasm";
    if (argc > 1) {
        kernel_path = argv[1];
    }

    std::string src = readTextFile(kernel_path);
    if (src.empty()) {
        std::cerr << "Error: Could not read " << kernel_path << "\n";
        return 1;
    }

    try {
        std::cout << "Assembling " << kernel_path << "...\n";
        auto assembled = assembler::assemble(src);
        if (!assembled.success) {
            std::cerr << "Assembly failed:\n";
            for (const auto& err : assembled.errors) {
                std::cerr << "  Line " << err.line << ": " << err.message << "\n";
            }
            return 1;
        }
        
        VMState vm(8192, 65536); 
        if (!assembler::loadAndReset(vm, assembled)) {
            std::cerr << "Error: Failed to load program/data into VM.\n";
            return 1;
        }

        std::cout << "Kernel Booting...\n";
        std::cout << "-------------------------------------------\n";

        size_t last_pos = 0;
        int max_steps = 100000;
        int steps = 0;
        PrivilegeMode last_priv = PrivilegeMode::Kernel;

        while (vm.status == VMStatus::RUNNING && steps < max_steps) {
            step(vm);
            steps++;

            if (vm.privilege != last_priv) {
                std::cout << "[Privilege Change: " << (vm.privilege == PrivilegeMode::Kernel ? "Kernel" : "User") << "]\n" << std::flush;
                last_priv = vm.privilege;
            }

            // Check for new console output
            if (vm.syscall_buffer.size() > last_pos) {
                std::cout << vm.syscall_buffer.substr(last_pos) << std::flush;
                last_pos = vm.syscall_buffer.size();
            }
        }

        std::cout << "\n-------------------------------------------\n";
        std::cout << "Execution ended after " << steps << " steps.\n";
        std::cout << "Final Status: " << vmStatusToString(vm.status) << "\n";
        std::cout << "Final PC:     " << vm.pc << "\n";
        
        if (vm.status == VMStatus::TRAPPED) {
            TrapCode tc = decodeTrap(vm.trap_reg);
            std::cout << "Trap Code:    " << static_cast<int>(tc) << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "\nAssembler/VM Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
