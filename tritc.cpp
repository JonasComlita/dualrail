#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <iomanip>

namespace {

void showUsage() {
    std::cout << "\033[1;35mTritC - High-Level Ternary Compiler Toolchain CLI Driver\033[0m\n";
    std::cout << "\033[1;36m========================================================\033[0m\n\n";
    std::cout << "\033[1mUsage:\033[0m\n";
    std::cout << "  tritc <source_files...> [options]       Compile and link source files\n";
    std::cout << "  tritc run <source_file.trit> [options]  Compile, link, and run immediately on the VM\n\n";
    std::cout << "\033[1mOptions:\033[0m\n";
    std::cout << "  \033[1;32m-o <file>\033[0m               Output compiled ternary executable image\n";
    std::cout << "  \033[1;32m-S\033[0m                      Output assembly text (.tasm) instead of binary executable\n";
    std::cout << "  \033[1;32m-O0 / -O1 / -O2\033[0m         Set optimization level (None, Basic, Aggressive)\n";
    std::cout << "  \033[1;32m--stack <words>\033[0m         Set stack allocation size hint (default: 24)\n";
    std::cout << "  \033[1;32m--dump-ir\033[0m               Print structural SSA IR before lowering\n";
    std::cout << "  \033[1;32m--dump-passes\033[0m           Dump internal compiler optimizer pass telemetry\n";
    std::cout << "  \033[1;32m--steps <count>\033[0m         Set VM execution step limit (default: 1000000)\n";
    std::cout << "  \033[1;32m--input <string>\033[0m        Feed ASCII console input string to the VM in run mode\n";
    std::cout << "  \033[1;32m--input-file <file>\033[0m     Feed console input from a file to the VM in run mode\n";
    std::cout << "  \033[1;32m--imem <size>\033[0m           Set VM instruction memory size (default: 32768)\n";
    std::cout << "  \033[1;32m--dmem <size>\033[0m           Set VM data memory size (default: 65536)\n";
    std::cout << "  \033[1;32m--seed-file <p> <addr>\033[0m  Seed VM data memory with file contents starting at addr\n";
    std::cout << "  \033[1;32m--no-ansi\033[0m               Disable ANSI coloring in terminal outputs\n";
    std::cout << "  \033[1;32m--help / -h\033[0m             Display this help documentation\n\n";
    std::cout << "\033[1mExamples:\033[0m\n";
    std::cout << "  tritc main.trit lib.trit -o app.exe\n";
    std::cout << "  tritc run test.trit -O2\n\n";
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.good()) return {};
    std::string out;
    in.seekg(0, std::ios::end);
    out.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(&out[0], out.size());
    return out;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::out | std::ios::binary);
    if (!out.good()) return false;
    out.write(data.data(), data.size());
    return out.good();
}

struct TritFileHeader {
    char magic[4] = {'T', 'X', 'E', '4'};
    uint32_t version = 2;
    uint32_t endianness = 0x12345678;
    uint32_t header_size = 0;
    uint32_t isa_version = sandbox::architecture::v2::ISA_VERSION;
    uint64_t required_features = 0;
    uint32_t instruction_count = 0;
    uint32_t data_count = 0;
    uint32_t stack_words = 0;
    uint32_t entry_pc = 0;
    uint32_t abi_version =
        sandbox::architecture::v2::FUNCTION_ABI_VERSION;
    uint32_t syscall_abi_version =
        sandbox::architecture::v2::SYSCALL_ABI_VERSION;
    uint32_t scalar_word_trits =
        sandbox::architecture::v2::SCALAR_WORD_TRITS;
    uint32_t base_page_words =
        sandbox::architecture::v2::BASE_PAGE_WORDS;
    uint32_t flags = 0;
};

bool writeBinaryFile(const std::string& path, const sandbox::compiler::LinkResult& linked) {
    std::ofstream out(path, std::ios::out | std::ios::binary);
    if (!out.good()) return false;
    
    TritFileHeader header;
    header.header_size = sizeof(TritFileHeader);
    header.instruction_count = static_cast<uint32_t>(linked.assembled.program.size());
    header.data_count = static_cast<uint32_t>(linked.assembled.data.size());
    header.required_features =
        linked.executable_header_v2.required_features;
    header.stack_words = static_cast<uint32_t>(
        linked.executable_header_v2.stack_words);
    header.entry_pc = static_cast<uint32_t>(
        linked.executable_header_v2.entry_pc);
    header.flags =
        static_cast<uint32_t>(linked.executable_header_v2.flags);
    
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    if (!linked.assembled.program.empty()) {
        out.write(reinterpret_cast<const char*>(linked.assembled.program.data()),
                  linked.assembled.program.size() * sizeof(sandbox::isa::TritWord27));
    }
    
    if (!linked.assembled.data.empty()) {
        out.write(reinterpret_cast<const char*>(linked.assembled.data.data()),
                  linked.assembled.data.size() * sizeof(sandbox::vm::TernaryValue));
    }
    
    return out.good();
}

bool readBinaryFile(const std::string& path, std::vector<sandbox::isa::TritWord27>& program, std::vector<sandbox::vm::TernaryValue>& data, TritFileHeader& header) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.good()) return false;
    
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good()) return false;
    
    const bool v2_header =
        header.magic[0] == 'T' && header.magic[1] == 'X' &&
        header.magic[2] == 'E' && header.magic[3] == '4' &&
        header.version == 2 &&
        header.header_size == sizeof(TritFileHeader) &&
        header.isa_version == sandbox::architecture::v2::ISA_VERSION &&
        header.abi_version ==
            sandbox::architecture::v2::FUNCTION_ABI_VERSION &&
        header.syscall_abi_version ==
            sandbox::architecture::v2::SYSCALL_ABI_VERSION &&
        header.scalar_word_trits ==
            sandbox::architecture::v2::SCALAR_WORD_TRITS &&
        header.base_page_words ==
            sandbox::architecture::v2::BASE_PAGE_WORDS &&
        (header.required_features &
         sandbox::isa::featureBit(
             sandbox::architecture::v2::FEATURE_BASE_V2)) != 0;
    if (!v2_header) {
        std::cerr
            << "Error: executable is not TXE4/ISA v2; rebuild source or "
               "use the offline artifact migrator: "
            << path << "\n";
        return false;
    }
    
    program.resize(header.instruction_count);
    if (header.instruction_count > 0) {
        in.read(reinterpret_cast<char*>(program.data()), header.instruction_count * sizeof(sandbox::isa::TritWord27));
    }
    
    data.resize(header.data_count);
    if (header.data_count > 0) {
        in.read(reinterpret_cast<char*>(data.data()), header.data_count * sizeof(sandbox::vm::TernaryValue));
    }
    
    return in.good() || in.eof();
}

bool loadAndResetBinary(sandbox::vm::VMState& vm, const std::vector<sandbox::isa::TritWord27>& program, const std::vector<sandbox::vm::TernaryValue>& data, const TritFileHeader& header) {
    if (static_cast<int>(program.size()) > vm.imem.size()) return false;
    if (static_cast<int>(data.size()) > vm.dmem.size()) return false;

    vm.coldReset();
    if (!vm.configureArchitecture(
            sandbox::isa::IsaEncodingVersion::V2,
            header.required_features)) {
        return false;
    }
    if (!vm.imem.loadProgram(program, 0)) return false;
    for (int i = 0; i < static_cast<int>(data.size()); ++i) {
        if (vm.dmem.store(i, data[static_cast<std::size_t>(i)]) != sandbox::vm::MemFaultCode::OK) {
            return false;
        }
    }
    
    int sp = vm.dmem.size() - 1;
    vm.regfile.write(26, sandbox::vm::ops::fromLong(sp));
    
    vm.standalone_heap_break =
        std::max<long long>(vm.standalone_heap_break,
                            static_cast<long long>(data.size()) + 16);
    vm.pc = static_cast<int>(header.entry_pc);
    return true;
}


void printDiagnostics(const std::vector<sandbox::compiler::Diagnostic>& diagnostics, bool use_ansi) {
    for (const auto& diag : diagnostics) {
        if (use_ansi) {
            std::cerr << "\033[1m" << diag.span.file << ":" << diag.span.line << ":" << diag.span.column << ": ";
            if (diag.severity == sandbox::compiler::DiagnosticSeverity::Error) {
                std::cerr << "\033[1;31merror: \033[0m\033[1m";
            } else if (diag.severity == sandbox::compiler::DiagnosticSeverity::Warning) {
                std::cerr << "\033[1;33mwarning: \033[0m\033[1m";
            } else {
                std::cerr << "\033[1;36minfo: \033[0m\033[1m";
            }
            std::cerr << diag.message << "\033[0m\n";
        } else {
            std::cerr << diag.format() << "\n";
        }
    }
}

void printOptimizerStats(const sandbox::compiler::OptimizerStats& stats, bool use_ansi) {
    if (use_ansi) {
        std::cout << "\033[1;35m--- Optimizer Pipeline Telemetry ---\033[0m\n";
        std::cout << "  Promoted to SSA Registers (mem2reg): \033[1;32m" << stats.mem2reg_promotions << "\033[0m\n";
        std::cout << "  Constant Folding Operations:         \033[1;32m" << stats.constant_folds << "\033[0m\n";
        std::cout << "  Common Subexpressions Eliminated:    \033[1;32m" << stats.cse_hits << "\033[0m\n";
        std::cout << "  Copy Propagations:                   \033[1;32m" << stats.copy_props << "\033[0m\n";
        std::cout << "  Dead Instructions Removed:           \033[1;32m" << stats.dead_instrs << "\033[0m\n";
        std::cout << "  Constant Branch Simplifications:    \033[1;32m" << stats.branch_simplifications << "\033[0m\n";
        std::cout << "  Zero-Cycle Swap Conversions:         \033[1;32m" << stats.swaps << "\033[0m\n";
    } else {
        std::cout << "--- Optimizer Pipeline Telemetry ---\n";
        std::cout << "  Promoted to SSA Registers (mem2reg): " << stats.mem2reg_promotions << "\n";
        std::cout << "  Constant Folding Operations:         " << stats.constant_folds << "\n";
        std::cout << "  Common Subexpressions Eliminated:    " << stats.cse_hits << "\n";
        std::cout << "  Copy Propagations:                   " << stats.copy_props << "\n";
        std::cout << "  Dead Instructions Removed:           " << stats.dead_instrs << "\n";
        std::cout << "  Constant Branch Simplifications:    " << stats.branch_simplifications << "\n";
        std::cout << "  Zero-Cycle Swap Conversions:         " << stats.swaps << "\n";
    }
}

void printAllocationResult(const sandbox::compiler::AllocationResult& alloc, bool use_ansi) {
    if (use_ansi) {
        std::cout << "\033[1;35m--- Register Allocator Telemetry ---\033[0m\n";
        std::cout << "  \033[1;32m[Allocator colors are wired into assembly emission]\033[0m\n";
        std::cout << "  Interference Graph Edges Resolved:   \033[1;32m" << alloc.interference_edges << "\033[0m\n";
        std::cout << "  Coalesced Copy Operations:           \033[1;32m" << alloc.coalesced_moves << "\033[0m\n";
        std::cout << "  Callee-Saved Registers Used:         \033[1;32m" << alloc.callee_saved_used.size() << "\033[0m\n";
        std::cout << "  Caller-Saved Spill-Reloads:          \033[1;32m" << alloc.caller_saved_live_across_calls.size() << "\033[0m\n";
        if (alloc.spills > 0) {
            std::cout << "  Active Stack Spill Slots Allocated:  \033[1;31m" << alloc.spills << " [REGISTER PRESSURE SPILL]\033[0m\n";
        } else {
            std::cout << "  Active Stack Spill Slots Allocated:  \033[1;32m0\033[0m\n";
        }
    } else {
        std::cout << "--- Register Allocator Telemetry ---\n";
        std::cout << "  [Allocator colors are wired into assembly emission]\n";
        std::cout << "  Interference Graph Edges Resolved:   " << alloc.interference_edges << "\n";
        std::cout << "  Coalesced Copy Operations:           " << alloc.coalesced_moves << "\n";
        std::cout << "  Callee-Saved Registers Used:         " << alloc.callee_saved_used.size() << "\n";
        std::cout << "  Caller-Saved Spill-Reloads:          " << alloc.caller_saved_live_across_calls.size() << "\n";
        std::cout << "  Active Stack Spill Slots Allocated:  " << alloc.spills << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace sandbox;
    using namespace sandbox::compiler;

    LongTriple::initPowTable();

    if (argc < 2) {
        showUsage();
        return 1;
    }

    std::vector<std::string> source_paths;
    std::string output_path;
    bool assembly_only = false;
    bool run_mode = false;
    bool dump_ir = false;
    bool dump_passes = false;
    bool dump_registers = false;
    bool use_ansi = true;
    int step_limit = 1000000;
    std::string console_input_str;

    int imem_size = 32768;
    int dmem_size = 1000000;
    struct SeedFile {
        std::string path;
        int address;
    };
    std::vector<SeedFile> seed_files;
    
    CompilerOptions comp_options;
    LinkOptions link_options;

    // Command-line parsing
    std::string mode = argv[1];
    int arg_start = 1;
    if (mode == "run") {
        run_mode = true;
        arg_start = 2;
    } else if (mode == "--help" || mode == "-h") {
        showUsage();
        return 0;
    }

    for (int i = arg_start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 < argc) {
                output_path = argv[++i];
            } else {
                std::cerr << "Error: Missing output file path after -o\n";
                return 1;
            }
        } else if (arg == "-S") {
            assembly_only = true;
        } else if (arg == "-O0") {
            comp_options.optimization = OptimizationLevel::None;
        } else if (arg == "-O1") {
            comp_options.optimization = OptimizationLevel::Basic;
        } else if (arg == "-O2") {
            comp_options.optimization = OptimizationLevel::Aggressive;
        } else if (arg == "--stack") {
            if (i + 1 < argc) {
                link_options.stack_hint_words = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: Missing stack size after --stack\n";
                return 1;
            }
        } else if (arg == "--steps") {
            if (i + 1 < argc) {
                step_limit = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: Missing step limit after --steps\n";
                return 1;
            }
        } else if (arg == "--input") {
            if (i + 1 < argc) {
                console_input_str = argv[++i];
            } else {
                std::cerr << "Error: Missing input string after --input\n";
                return 1;
            }
        } else if (arg == "--input-file") {
            if (i + 1 < argc) {
                std::string path = argv[++i];
                std::ifstream check(path, std::ios::in | std::ios::binary);
                if (!check.good()) {
                    std::cerr << "Error: Could not open or read input file: " << path << "\n";
                    return 1;
                }
                check.close();
                console_input_str = readFile(path);
            } else {
                std::cerr << "Error: Missing file path after --input-file\n";
                return 1;
            }
        } else if (arg == "--dump-ir") {
            dump_ir = true;
        } else if (arg == "--dump-passes") {
            dump_passes = true;
        } else if (arg == "--dump-registers") {
            dump_registers = true;
        } else if (arg == "--imem") {
            if (i + 1 < argc) {
                imem_size = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: Missing size after --imem\n";
                return 1;
            }
        } else if (arg == "--dmem") {
            if (i + 1 < argc) {
                dmem_size = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: Missing size after --dmem\n";
                return 1;
            }
        } else if (arg == "--seed-file") {
            if (i + 2 < argc) {
                std::string path = argv[++i];
                int address = std::stoi(argv[++i]);
                seed_files.push_back({path, address});
            } else {
                std::cerr << "Error: Missing path or address after --seed-file\n";
                return 1;
            }
        } else if (arg == "--no-ansi") {
            use_ansi = false;
            comp_options.diagnostics_mode = DiagnosticsMode::Machine;
        } else if (arg == "--help" || arg == "-h") {
            showUsage();
            return 0;
        } else {
            if (arg[0] == '-') {
                std::cerr << "Error: Unknown compiler option: " << arg << "\n";
                return 1;
            }
            source_paths.push_back(arg);
        }
    }

    if (source_paths.empty()) {
        std::cerr << "Error: No source files specified.\n";
        return 1;
    }



    bool is_tasm = false;
    bool is_txe = false;
    for (const auto& path : source_paths) {
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".tasm") {
            is_tasm = true;
            break;
        } else if (path.size() >= 4 && path.substr(path.size() - 4) == ".txe") {
            is_txe = true;
            break;
        }
    }

    if (is_txe) {
        if (source_paths.size() > 1) {
            std::cerr << "Error: Cannot run multiple .txe files together.\n";
            return 1;
        }

        std::vector<isa::TritWord27> program;
        std::vector<vm::TernaryValue> data;
        TritFileHeader header;
        if (!readBinaryFile(source_paths[0], program, data, header)) {
            std::cerr << "Error: Failed to read binary executable file: " << source_paths[0] << "\n";
            return 1;
        }

        if (!run_mode) {
            std::cout << "File is already a compiled binary executable: " << source_paths[0] << "\n";
            std::cout << "  Instruction Words:        " << header.instruction_count << "\n";
            std::cout << "  Static Data Memory Words: " << header.data_count << "\n";
            std::cout << "  ABI Version:              " << header.abi_version << "\n";
            return 0;
        }

        // Run immediately on the VM (run mode)
        std::cout << (use_ansi ? "\033[1;32mBooting binary executable in Ternary VM...\033[0m\n\n" : "Booting binary executable in Ternary VM...\n\n");
        
        vm::VMState vm(imem_size, dmem_size);
        if (!loadAndResetBinary(vm, program, data, header)) {
            std::cerr << "Error: Failed to load binary executable into VM memory.\n";
            return 1;
        }

        for (const auto& sf : seed_files) {
            std::string seed_data = readFile(sf.path);
            int len = static_cast<int>(seed_data.size());
            if (sf.address - 1 < 0 || sf.address + len > dmem_size) {
                std::cerr << "Error: Seed file " << sf.path << " (len " << len 
                          << ") at address " << sf.address 
                          << " exceeds dmem boundaries (0.." << dmem_size - 1 << ")\n";
                return 1;
            }
            vm.dmem.store(sf.address - 1, sandbox::vm::ops::fromLong(len));
            for (int j = 0; j < len; ++j) {
                vm.dmem.store(sf.address + j, sandbox::vm::ops::fromLong(static_cast<unsigned char>(seed_data[j])));
            }
        }

        if (!console_input_str.empty()) {
            vm.enqueueConsoleAscii(console_input_str);
        }

        const auto run_result = vm::run(vm, step_limit);
        
        if (use_ansi) {
            std::cout << "\n\033[1;36m========================================================\033[0m\n";
            std::cout << "\033[1;35m            Ternary VM Execution Terminated            \033[0m\n";
            std::cout << "\033[1;36m========================================================\033[0m\n";
            std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
            if (vm.status == vm::VMStatus::TRAPPED || dump_registers) {
                if (vm.status == vm::VMStatus::TRAPPED) {
                    std::cout << "  Trap Cause:         \033[1;31m" << vm.cause << "\033[0m (PC: " << vm.pc << ")\n";
                }
                std::cout << "  Register File State:\n";
                for (int r = 0; r <= 26; ++r) {
                    std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                              << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
                }
            }
            std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
            if (!vm.syscall_buffer.empty()) {
                std::cout << "  Console Output:\n\033[1;32m" << vm.syscall_buffer << "\033[0m\n";
            }
            long long return_val = vm::ops::toLong(vm.regfile.read(13)); // ABI return register r13
            std::cout << "  Return Register \033[1mr13\033[0m: \033[1;33m" << return_val << "\033[0m\n";
            std::cout << "\033[1;36m========================================================\033[0m\n";
        } else {
            std::cout << "\n========================================================\n";
            std::cout << "            Ternary VM Execution Terminated            \n";
            std::cout << "========================================================\n";
            std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
            if (vm.status == vm::VMStatus::TRAPPED || dump_registers) {
                if (vm.status == vm::VMStatus::TRAPPED) {
                    std::cout << "  Trap Cause:         " << vm.cause << " (PC: " << vm.pc << ")\n";
                }
                std::cout << "  Register File State:\n";
                for (int r = 0; r <= 26; ++r) {
                    std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                              << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
                }
            }
            std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
            if (!vm.syscall_buffer.empty()) {
                std::cout << "  Console Output:\n" << vm.syscall_buffer << "\n";
            }
            long long return_val = vm::ops::toLong(vm.regfile.read(13));
            std::cout << "  Return Register r13: " << return_val << "\n";
            std::cout << "========================================================\n";
        }
        return 0;
    }

    // Read and concatenate all source files into a single translation unit
    std::string combined_src;
    std::string combined_filename;
    for (size_t i = 0; i < source_paths.size(); ++i) {
        std::string src = readFile(source_paths[i]);
        if (src.empty()) {
            std::cerr << "Error: Could not read source file: " << source_paths[i] << "\n";
            return 1;
        }
        if (i > 0) {
            combined_src += "\n\n// =============================================================================\n";
            combined_src += "// Concatenated Source: " + source_paths[i] + "\n";
            combined_src += "// =============================================================================\n\n";
            combined_filename += " + ";
        }
        combined_src += src;
        combined_filename += source_paths[i];
    }

    if (is_tasm) {
        LinkResult linked;
        linked.assembly = combined_src;
        linked.assembled = vm::assembler::assemble(linked.assembly);
        linked.success = linked.assembled.success;
        linked.text_words = static_cast<int>(linked.assembled.program.size());
        linked.data_words = static_cast<int>(linked.assembled.data.size());
        linked.instruction_count = linked.text_words;
        
        if (!linked.assembled.executable_headers_v2.empty()) {
            linked.executable_header_v2 =
                linked.assembled.executable_headers_v2.begin()->second;
        } else {
            std::cerr
                << "Error: raw assembly executable output requires a "
                   "validated .execheader2\n";
            return 1;
        }

        if (!linked.success) {
            std::cerr << "\033[1;31mAssembly failed for translation unit: " << combined_filename << "\033[0m\n";
            for (const auto& error : linked.assembled.errors) {
                std::cerr << "  " << error.format() << "\n";
            }
            return 1;
        }

        if (assembly_only) {
            if (output_path.empty()) {
                std::cout << combined_src;
            } else {
                if (!writeFile(output_path, combined_src)) {
                    std::cerr << "Error: Failed to write assembly file to: " << output_path << "\n";
                    return 1;
                }
                std::cout << "Assembly successfully written to: " << output_path << "\n";
            }
            return 0;
        }

        // Save final executable image
        if (!run_mode) {
            if (output_path.empty()) {
                output_path = "app.exe";
            }
            if (!writeBinaryFile(output_path, linked)) {
                std::cerr << "Error: Failed to write executable image to: " << output_path << "\n";
                return 1;
            }
            std::cout << (use_ansi ? "\033[1;32mAssembly Succeeded!\033[0m\n" : "Assembly Succeeded!\n");
            std::cout << "  Output Executable Image:  " << output_path << "\n";
            std::cout << "  Total Instruction Words:  " << linked.text_words << "\n";
            std::cout << "  Static Data Memory Words: " << linked.data_words << "\n";
            std::cout << "  Emitted ABI Version:      "
                      << linked.executable_header_v2.function_abi_version
                      << "\n";
            return 0;
        }

        // Run immediately on the VM (run mode)
        std::cout << (use_ansi ? "\033[1;32mBooting compiled executable in Ternary VM...\033[0m\n\n" : "Booting compiled executable in Ternary VM...\n\n");
        
        vm::VMState vm(imem_size, dmem_size);
        if (!vm::assembler::loadAndReset(vm, linked.assembled)) {
            std::cerr << "Error: Failed to load executable image into VM memory.\n";
            return 1;
        }

        for (const auto& sf : seed_files) {
            std::string seed_data = readFile(sf.path);
            int len = static_cast<int>(seed_data.size());
            if (sf.address - 1 < 0 || sf.address + len > dmem_size) {
                std::cerr << "Error: Seed file " << sf.path << " (len " << len 
                          << ") at address " << sf.address 
                          << " exceeds dmem boundaries (0.." << dmem_size - 1 << ")\n";
                return 1;
            }
            vm.dmem.store(sf.address - 1, sandbox::vm::ops::fromLong(len));
            for (int j = 0; j < len; ++j) {
                vm.dmem.store(sf.address + j, sandbox::vm::ops::fromLong(static_cast<unsigned char>(seed_data[j])));
            }
        }

        if (!console_input_str.empty()) {
            vm.enqueueConsoleAscii(console_input_str);
        }

        const auto run_result = vm::run(vm, step_limit);
        
        if (use_ansi) {
            std::cout << "\n\033[1;36m========================================================\033[0m\n";
            std::cout << "\033[1;35m            Ternary VM Execution Terminated            \033[0m\n";
            std::cout << "\033[1;36m========================================================\033[0m\n";
            std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
            if (vm.status == vm::VMStatus::TRAPPED) {
                std::cout << "  Trap Cause:         \033[1;31m" << vm.cause << "\033[0m (PC: " << vm.pc << ")\n";
                std::cout << "  Register File State:\n";
                for (int r = 0; r <= 26; ++r) {
                    std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                              << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
                }
            }
            std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
            if (!vm.syscall_buffer.empty()) {
                std::cout << "  Console Output:\n\033[1;32m" << vm.syscall_buffer << "\033[0m\n";
            }
            long long return_val = vm::ops::toLong(vm.regfile.read(13)); // ABI return register r13
            std::cout << "  Return Register \033[1mr13\033[0m: \033[1;33m" << return_val << "\033[0m\n";
            std::cout << "\033[1;36m========================================================\033[0m\n";
        } else {
            std::cout << "\n========================================================\n";
            std::cout << "            Ternary VM Execution Terminated            \n";
            std::cout << "========================================================\n";
            std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
            if (vm.status == vm::VMStatus::TRAPPED) {
                std::cout << "  Trap Cause:         " << vm.cause << " (PC: " << vm.pc << ")\n";
                std::cout << "  Register File State:\n";
                for (int r = 0; r <= 26; ++r) {
                    std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                              << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
                }
            }
            std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
            if (!vm.syscall_buffer.empty()) {
                std::cout << "  Console Output:\n" << vm.syscall_buffer << "\n";
            }
            long long return_val = vm::ops::toLong(vm.regfile.read(13));
            std::cout << "  Return Register r13: " << return_val << "\n";
            std::cout << "========================================================\n";
        }
        return 0;
    }

    CompileResult compiled = compileSource(combined_filename, combined_src, comp_options);
    
    if (!compiled.diagnostics.empty()) {
        printDiagnostics(compiled.diagnostics, use_ansi);
    }

    if (!compiled.success) {
        std::cerr << "\033[1;31mCompilation failed for translation unit: " << combined_filename << "\033[0m\n";
        return 1;
    }

    if (dump_ir) {
        std::cout << "\n\033[1;36m=== Structural SSA IR Module for " << combined_filename << " ===\033[0m\n";
        for (const auto& fn : compiled.ssa_module.functions) {
            std::cout << "fn " << fn.name << "() {\n";
            for (const auto& block : fn.blocks) {
                std::cout << "  block " << block.name << ":\n";
                for (const auto& instr : block.instructions) {
                    std::cout << "    ";
                    if (instr.def >= 0) std::cout << "%" << instr.def << " = ";
                    std::cout << "instr_opcode_" << static_cast<int>(instr.opcode);
                    if (!instr.args.empty()) {
                        std::cout << " [";
                        for (size_t k = 0; k < instr.args.size(); ++k) {
                            if (k > 0) std::cout << ", ";
                            std::cout << "%" << instr.args[k];
                        }
                        std::cout << "]";
                    }
                    if (instr.imm != 0) std::cout << ", imm: " << instr.imm;
                    std::cout << "\n";
                }
                std::cout << "    terminator_" << static_cast<int>(block.terminator.kind) << "\n";
            }
            std::cout << "}\n";
        }
        std::cout << "\033[1;36m========================================================\033[0m\n\n";
    }

    if (dump_passes && comp_options.optimization != OptimizationLevel::None) {
        printOptimizerStats(compiled.optimizer_stats, use_ansi);
        printAllocationResult(compiled.allocation, use_ansi);
        std::cout << "\n";
    }

    if (assembly_only) {
        if (output_path.empty()) {
            std::cout << compiled.assembly;
        } else {
            if (!writeFile(output_path, compiled.assembly)) {
                std::cerr << "Error: Failed to write assembly file to: " << output_path << "\n";
                return 1;
            }
            std::cout << "Assembly successfully written to: " << output_path << "\n";
        }
        return 0;
    }

    std::vector<ObjectModule> object_modules = { compiled.object };

    // Linking stage
    LinkResult linked = linkModules(object_modules, link_options);
    if (!linked.diagnostics.empty()) {
        printDiagnostics(linked.diagnostics, use_ansi);
    }

    if (!linked.success) {
        std::cerr << "Linker failed!\n";
        return 1;
    }

    // Save final executable image
    if (!run_mode) {
        if (output_path.empty()) {
            output_path = "app.exe";
        }
        if (!writeBinaryFile(output_path, linked)) {
            std::cerr << "Error: Failed to write executable image to: " << output_path << "\n";
            return 1;
        }
        std::cout << (use_ansi ? "\033[1;32mBuild Succeeded!\033[0m\n" : "Build Succeeded!\n");
        std::cout << "  Output Executable Image:  " << output_path << "\n";
        std::cout << "  Total Instruction Words:  " << linked.text_words << "\n";
        std::cout << "  Static Data Memory Words: " << linked.data_words << "\n";
        std::cout << "  Emitted ABI Version:      "
                  << linked.executable_header_v2.function_abi_version
                  << "\n";
        return 0;
    }

    // Run immediately on the VM (run mode)
    std::cout << (use_ansi ? "\033[1;32mBooting compiled executable in Ternary VM...\033[0m\n\n" : "Booting compiled executable in Ternary VM...\n\n");
    
    vm::VMState vm(imem_size, dmem_size);
    if (!vm::assembler::loadAndReset(vm, linked.assembled)) {
        std::cerr << "Error: Failed to load executable image into VM memory.\n";
        return 1;
    }

    for (const auto& sf : seed_files) {
        std::string seed_data = readFile(sf.path);
        int len = static_cast<int>(seed_data.size());
        if (sf.address - 1 < 0 || sf.address + len > dmem_size) {
            std::cerr << "Error: Seed file " << sf.path << " (len " << len 
                      << ") at address " << sf.address 
                      << " exceeds dmem boundaries (0.." << dmem_size - 1 << ")\n";
            return 1;
        }
        vm.dmem.store(sf.address - 1, sandbox::vm::ops::fromLong(len));
        for (int j = 0; j < len; ++j) {
            vm.dmem.store(sf.address + j, sandbox::vm::ops::fromLong(static_cast<unsigned char>(seed_data[j])));
        }
    }

    if (!console_input_str.empty()) {
        vm.enqueueConsoleAscii(console_input_str);
    }

    const auto run_result = vm::run(vm, step_limit);
    
    if (use_ansi) {
        std::cout << "\n\033[1;36m========================================================\033[0m\n";
        std::cout << "\033[1;35m            Ternary VM Execution Terminated            \033[0m\n";
        std::cout << "\033[1;36m========================================================\033[0m\n";
        std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
        if (vm.status == vm::VMStatus::TRAPPED || dump_registers) {
            if (vm.status == vm::VMStatus::TRAPPED) {
                std::cout << "  Trap Cause:         \033[1;31m" << vm.cause << "\033[0m (PC: " << vm.pc << ")\n";
            }
            std::cout << "  Register File State:\n";
            for (int r = 0; r <= 26; ++r) {
                std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                          << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
            }
        }
        std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
        if (!vm.syscall_buffer.empty()) {
            std::cout << "  Console Output:\n\033[1;32m" << vm.syscall_buffer << "\033[0m\n";
        }
        long long return_val = vm::ops::toLong(vm.regfile.read(13)); // ABI return register r13
        std::cout << "  Return Register \033[1mr13\033[0m: \033[1;33m" << return_val << "\033[0m\n";
        std::cout << "\033[1;36m========================================================\033[0m\n";
    } else {
        std::cout << "\n========================================================\n";
        std::cout << "            Ternary VM Execution Terminated            \n";
        std::cout << "========================================================\n";
        std::cout << "  Final CPU Status:   " << vm::vmStatusToString(vm.status) << "\n";
        if (vm.status == vm::VMStatus::TRAPPED || dump_registers) {
            if (vm.status == vm::VMStatus::TRAPPED) {
                std::cout << "  Trap Cause:         " << vm.cause << " (PC: " << vm.pc << ")\n";
            }
            std::cout << "  Register File State:\n";
            for (int r = 0; r <= 26; ++r) {
                std::cout << "    r" << r << (r == 26 ? " (sp)" : "") << ": " 
                          << sandbox::vm::ops::toLong(vm.regfile.read(r)) << "\n";
            }
        }
        std::cout << "  Total CPU Cycles:   " << vm.cycle_count << "\n";
        if (!vm.syscall_buffer.empty()) {
            std::cout << "  Console Output:\n" << vm.syscall_buffer << "\n";
        }
        long long return_val = vm::ops::toLong(vm.regfile.read(13));
        std::cout << "  Return Register r13: " << return_val << "\n";
        std::cout << "========================================================\n";
    }

    return 0;
}
