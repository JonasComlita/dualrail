#include "ternary_asm.h"
#include "ternary_vm.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <iomanip>
#include <exception>
#include <chrono>
#include <thread>

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

// Map process state integer to human-readable string
std::string stateToString(int state) {
    switch (state) {
        case 0: return "FREE     ";
        case 1: return "RUNNABLE ";
        case 2: return "RUNNING  ";
        case 3: return "BLOCKED  ";
        case 4: return "SLEEPING ";
        case 5: return "EXITED   ";
        default: return "UNKNOWN  ";
    }
}

// Map wait channel integer to human-readable string
std::string waitChannelToString(int chan) {
    switch (chan) {
        case 0: return "NONE  ";
        case 1: return "TIMER ";
        case 2: return "KEYBD ";
        case 3: return "CHILD ";
        default: return "OTHER ";
    }
}

// Helper to get process metadata names
std::string procName(int idx) {
    switch (idx) {
        case 0: return "Shell  ";
        case 1: return "Prog A ";
        case 2: return "Prog B ";
        case 3: return "Spare  ";
        case 4: return "Idle   ";
        default: return "Unknown";
    }
}

long long loadPhysLong(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

} // namespace

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::vm;

    std::cout << "\033[2J\033[H"; // Clear screen and reset cursor
    std::cout << "\033[1;36m===============================================================\033[0m\n";
    std::cout << "\033[1;35m      Ternary OS3 Microkernel Real-Time Visualizer             \033[0m\n";
    std::cout << "\033[1;36m===============================================================\033[0m\n\n";
    
    std::string kernel_path = "OS3/minimal_kernel_bringup.tasm";
    if (argc > 1) {
        kernel_path = argv[1];
    }

    std::string src = readTextFile(kernel_path);
    if (src.empty()) {
        std::cerr << "\033[1;31mError: Could not read " << kernel_path << "\033[0m\n";
        return 1;
    }

    try {
        std::cout << "Assembling " << kernel_path << "...\n";
        auto assembled = assembler::assemble(src);
        if (!assembled.success) {
            std::cerr << "\033[1;31mAssembly failed:\033[0m\n";
            for (const auto& err : assembled.errors) {
                std::cerr << "  Line " << err.line << ": " << err.message << "\n";
            }
            return 1;
        }
        
        VMState vm(8192, 65536); 
        if (!assembler::loadAndReset(vm, assembled)) {
            std::cerr << "\033[1;31mError: Failed to load program/data into VM.\033[0m\n";
            return 1;
        }

        // Extract metadata labels
        auto label = [&](const std::string& name) -> int {
            auto it = assembled.data_labels.find(name);
            if (it == assembled.data_labels.end()) {
                throw std::runtime_error("Required kernel label missing: " + name);
            }
            return it->second;
        };

        const int addr_current = label("current_proc");
        const int addr_state = label("proc_state");
        const int addr_pid = label("proc_pid");
        const int addr_ticks = label("proc_ticks");
        const int addr_preemptions = label("proc_preemptions");
        const int addr_wait_channel = label("proc_wait_channel");
        const int addr_yields = label("proc_yields");
        const int addr_sleeps = label("proc_sleeps");
        const int addr_exits = label("proc_exits");
        const int addr_spawns = label("proc_spawns");
        const int addr_waits = label("proc_waits");
        const int addr_read_blocks = label("proc_read_blocks");
        const int addr_input_reads = label("proc_input_reads");

        std::cout << "\033[1;32mKernel Booting...\033[0m\n";
        std::cout << "Press [Enter] to begin step-by-step interactive simulation...\n";
        std::cin.get();

        size_t last_pos = 0;
        int steps = 0;
        int next_input_step = 300;
        std::string script = "awbu x";
        size_t script_idx = 0;

        auto printStateTable = [&]() {
            // Read active process
            int current = static_cast<int>(loadPhysLong(vm, addr_current));

            std::cout << "\033[H"; // Reset cursor to top
            std::cout << "\033[1;36m===============================================================\033[0m\n";
            std::cout << "\033[1;35m      Ternary OS3 Microkernel Real-Time Visualizer             \033[0m\n";
            std::cout << "\033[1;36m===============================================================\033[0m\n";
            std::cout << " System Cycle: \033[1;33m" << std::setw(6) << vm.cycle_count << "\033[0m | Simulation Step: " << std::setw(6) << steps;
            std::cout << " | Privilege: " << (vm.privilege == PrivilegeMode::Kernel ? "\033[1;31mKERNEL\033[0m" : "\033[1;32mUSER\033[0m") << "\n";
            std::cout << "\033[1;34m---------------------------------------------------------------\033[0m\n";
            std::cout << " \033[1mSlot Name    PID  State     WaitCh  Ticks  Preempt  Yields  Spawns\033[0m\n";
            std::cout << "\033[1;34m---------------------------------------------------------------\033[0m\n";

            for (int i = 0; i < 5; ++i) {
                int pid = static_cast<int>(loadPhysLong(vm, addr_pid + i));
                int state = static_cast<int>(loadPhysLong(vm, addr_state + i));
                int wait_chan = static_cast<int>(loadPhysLong(vm, addr_wait_channel + i));
                int ticks = static_cast<int>(loadPhysLong(vm, addr_ticks + i));
                int preempt = static_cast<int>(loadPhysLong(vm, addr_preemptions + i));
                int yields = static_cast<int>(loadPhysLong(vm, addr_yields + i));
                int spawns = static_cast<int>(loadPhysLong(vm, addr_spawns + i));

                if (i == current) {
                    std::cout << "\033[1;32m * \033[0m"; // Active marker
                } else {
                    std::cout << "   ";
                }

                std::cout << procName(i) << "  " 
                          << std::setw(3) << pid << "  "
                          << stateToString(state) << "  "
                          << waitChannelToString(wait_chan) << "  "
                          << std::setw(5) << ticks << "  "
                          << std::setw(7) << preempt << "  "
                          << std::setw(6) << yields << "  "
                          << std::setw(6) << spawns << "\n";
            }
            std::cout << "\033[1;34m---------------------------------------------------------------\033[0m\n";
            std::cout << " \033[1mVirtual Console Output:\033[0m\n ";
            if (vm.syscall_buffer.empty()) {
                std::cout << "\033[90m(No output yet)\033[0m";
            } else {
                std::cout << "\033[1;32m" << vm.syscall_buffer << "\033[0m";
            }
            std::cout << "\n\033[1;34m---------------------------------------------------------------\033[0m\n";
            std::cout << " \033[1mSimulated Keyboard Input:\033[0m ";
            if (script_idx < script.size()) {
                std::cout << "Queued: \"" << script.substr(script_idx) << "\"\n";
            } else {
                std::cout << "All inputs processed.\n";
            }
            std::cout << "\033[1;36m===============================================================\033[0m\n";
        };

        // Run simulation loop
        while (vm.status == VMStatus::RUNNING && steps < 12000) {
            step(vm);
            steps++;

            // Periodically check if shell is blocked and feed next keybd character
            int shell_state = static_cast<int>(loadPhysLong(vm, addr_state + 0));
            int shell_chan = static_cast<int>(loadPhysLong(vm, addr_wait_channel + 0));
            
            if (shell_state == 3 && shell_chan == 2 && script_idx < script.size() && steps >= next_input_step) {
                char ch = script[script_idx++];
                vm.enqueueConsoleInput(ch);
                next_input_step = steps + 1000; // Queue next character in 1000 steps to let processes execute
            }

            // Update UI every 50 steps, or when console output changes, or during privilege transitions
            if (steps % 100 == 0 || vm.syscall_buffer.size() > last_pos) {
                last_pos = vm.syscall_buffer.size();
                printStateTable();
                std::this_thread::sleep_for(std::chrono::milliseconds(20)); // High quality smooth rate-limiting
            }
        }

        // Final print
        printStateTable();

        std::cout << "\n\033[1;32mSimulation Finished Successfully!\033[0m\n";
        std::cout << "Total execution steps: " << steps << "\n";
        std::cout << "Final VM Status: " << vmStatusToString(vm.status) << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\n\033[1;31mError during execution: " << e.what() << "\033[0m\n";
        return 1;
    }

    return 0;
}
