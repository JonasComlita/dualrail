#include "ternary_host_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kCurrentPidAddr = 3020;

int currentPidFromKernelWord(const sandbox::vm::VMState& machine) {
    auto [word, fault] = machine.dmem.load(kCurrentPidAddr);
    if (fault == sandbox::vm::MemFaultCode::OK) {
        return static_cast<int>(sandbox::vm::ops::toLong(word));
    }
    return sandbox::vm::currentProcessForProfile(machine);
}

void usage() {
    std::cerr
        << "usage: trit_profile_tos [--workload os|syscall-probe]\n"
        << "                        [--steps N] [--top N] [--format text|json]\n"
        << "                        [--output PATH] [--profile minimum|compact]\n"
        << "                        [--disk PATH] BOOT_IMAGE\n";
}

bool writeReport(const std::string& path, const std::string& report, std::string& error) {
    if (path.empty()) {
        std::cout << report;
        return true;
    }
    const std::filesystem::path out_path(path);
    std::error_code ec;
    if (!out_path.parent_path().empty()) {
        std::filesystem::create_directories(out_path.parent_path(), ec);
        if (ec) {
            error = "failed to create output directory: " + ec.message();
            return false;
        }
    }
    std::ofstream out(out_path, std::ios::trunc);
    if (!out.good()) {
        error = "failed to open profile output: " + path;
        return false;
    }
    out << report;
    return static_cast<bool>(out);
}

std::string formatReport(const std::string& workload,
                         const std::string& subject,
                         int steps,
                         int top_limit,
                         const sandbox::vm::RunResult& result,
                         const sandbox::vm::VMExecutionProfile& profile,
                         const std::string& format) {
    std::ostringstream report;
    if (format == "json") {
        profile.writeJson(report, top_limit);
    } else {
        report << "workload: " << workload << "\n";
        if (!subject.empty()) report << "subject: " << subject << "\n";
        report << "steps_requested: " << steps << "\n";
        report << "result: " << result.description << "\n\n";
        profile.writeText(report, top_limit);
    }
    return report.str();
}

int runSyscallProbe(int steps,
                    int top_limit,
                    const std::string& format,
                    const std::string& output_path) {
    auto assembled = sandbox::vm::assembler::assemble(R"(
        .text
        start:
            mov r1, handler
            csrw tvec, r1
            mov r1, -8
            csrw status, r1
            mov r13, 123
            syscall 1
            syscall 2
            mov r13, 65
            syscall 22
            halt
        handler:
            csrr r4, cause
            csrr r5, syscall_id
            mov r1, 1
            tcmp r2, r5, r1
            brz r2, write_value
            mov r1, 2
            tcmp r2, r5, r1
            brz r2, write_newline
            mov r1, 22
            tcmp r2, r5, r1
            brz r2, write_char
            mov r13, -1
            jmp syscall_return
        write_value:
            csrw console_out, r13
            mov r13, 0
            jmp syscall_return
        write_newline:
            mov r1, 1
            csrw console_ctrl, r1
            mov r13, 0
            jmp syscall_return
        write_char:
            mov r1, 2
            csrw console_ctrl, r1
            csrw console_out, r13
            mov r1, 3
            csrw console_ctrl, r1
            mov r13, 0
        syscall_return:
            csrr r1, epc
            mov r2, 1
            add r1, r1, r2
            csrw epc, r1
            eret
    )");
    if (!assembled.success) {
        std::cerr << "failed to assemble syscall profile probe\n";
        return EXIT_FAILURE;
    }

    sandbox::vm::VMState machine(256, 1024);
    machine.coldReset();
    if (!machine.imem.loadProgram(assembled.program)) {
        std::cerr << "failed to load syscall profile probe\n";
        return EXIT_FAILURE;
    }
    machine.setCoreCurrentProcess(0, 99);

    sandbox::vm::VMExecutionProfile profile;
    sandbox::vm::VMHooks hooks = sandbox::vm::makeProfilerHooks(profile);
    const sandbox::vm::RunResult result = sandbox::vm::run(machine, steps, &hooks);
    std::string error;
    const std::string report =
        formatReport("syscall-probe", "routed syscall trap probe", steps, top_limit, result, profile, format);
    if (!writeReport(output_path, report, error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    return result.trapped() ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();

    int steps = 200000;
    int top_limit = 20;
    std::string format = "text";
    std::string output_path;
    std::string profile_override;
    std::string disk_path;
    std::string workload = "os";
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else if (arg == "--steps" && i + 1 < argc) {
            steps = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--top" && i + 1 < argc) {
            top_limit = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--profile" && i + 1 < argc) {
            profile_override = argv[++i];
        } else if (arg == "--disk" && i + 1 < argc) {
            disk_path = argv[++i];
        } else if (arg == "--workload" && i + 1 < argc) {
            workload = argv[++i];
        } else {
            positional.push_back(arg);
        }
    }

    if (format != "text" && format != "json") {
        std::cerr << "profile format must be text or json\n";
        return EXIT_FAILURE;
    }
    if (workload == "syscall-probe") {
        return runSyscallProbe(steps, top_limit, format, output_path);
    }
    if (workload != "os") {
        std::cerr << "profile workload must be os or syscall-probe\n";
        return EXIT_FAILURE;
    }
    if (positional.empty()) {
        usage();
        return EXIT_FAILURE;
    }

    const std::string boot_path = positional.front();
    sandbox::host::TosBootImage image;
    std::string error;
    if (!sandbox::host::readBootImageFile(boot_path, image, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    if (!profile_override.empty()) {
        image.manifest.profile_name = profile_override;
    }

    sandbox::vm::ProductionProfile production_profile =
        sandbox::host::profileForManifest(image.manifest);
    sandbox::vm::VMState machine(production_profile);
    if (!sandbox::host::loadBootImageIntoVm(machine, image, disk_path, &error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }

    sandbox::vm::VMExecutionProfile profile;
    sandbox::vm::VMHooks hooks =
        sandbox::vm::makeProfilerHooks(profile, currentPidFromKernelWord);
    const sandbox::vm::RunResult result = sandbox::vm::run(machine, steps, &hooks);

    const std::string report =
        formatReport("os", boot_path, steps, top_limit, result, profile, format);
    if (!writeReport(output_path, report, error)) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    return result.trapped() ? EXIT_FAILURE : EXIT_SUCCESS;
}
