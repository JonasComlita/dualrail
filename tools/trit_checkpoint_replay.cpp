#include "ternary_host_runtime.h"

#include <iostream>
#include <string>

namespace {

struct Options {
    std::string bundle;
    int steps = 100000;
    bool require_halt = false;
    bool json = false;
};

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " --bundle <checkpoint-directory> [--steps <count>]"
                 " [--require-halt] [--json]\n";
}

bool parseInt(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(text, &consumed, 10);
        if (consumed != text.size() || parsed < 0 || parsed > 100000000) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return false;
        }
        if (argument == "--bundle") {
            if (++index >= argc) return false;
            options.bundle = argv[index];
        } else if (argument == "--steps") {
            if (++index >= argc || !parseInt(argv[index], options.steps)) return false;
        } else if (argument == "--require-halt") {
            options.require_halt = true;
        } else if (argument == "--json") {
            options.json = true;
        } else if (options.bundle.empty() && argument.rfind("--", 0) != 0) {
            options.bundle = argument;
        } else {
            return false;
        }
    }
    return !options.bundle.empty();
}

void jsonString(std::ostream& out, const std::string& value) {
    out << '"';
    for (const char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    out << '"';
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        usage(argv[0]);
        return 2;
    }

    sandbox::host::TosRuntime runtime;
    std::string error;
    if (!runtime.restoreCheckpointBundle(options.bundle, &error)) {
        if (options.json) {
            std::cout << "{\"ok\":false,\"error\":";
            jsonString(std::cout, error);
            std::cout << "}\n";
        } else {
            std::cerr << "checkpoint restore failed: " << error << "\n";
        }
        return 3;
    }

    const sandbox::vm::RunResult result =
        runtime.replayFromCheckpoint(options.steps, &error);
    const sandbox::host::TosRuntimeSnapshot snapshot = runtime.snapshot();
    const bool halted = result.status == sandbox::vm::VMStatus::HALTED;
    const bool trapped = result.status == sandbox::vm::VMStatus::TRAPPED;
    const bool ok = !trapped && (!options.require_halt || halted);

    if (options.json) {
        std::cout << "{\"ok\":" << (ok ? "true" : "false")
                  << ",\"status\":";
        jsonString(std::cout, sandbox::vm::vmStatusToString(result.status));
        std::cout << ",\"steps\":" << result.steps
                  << ",\"pc\":" << result.final_pc
                  << ",\"cycles\":" << snapshot.cycles
                  << ",\"require_halt\":"
                  << (options.require_halt ? "true" : "false")
                  << ",\"description\":";
        jsonString(std::cout, result.description);
        if (!error.empty()) {
            std::cout << ",\"error\":";
            jsonString(std::cout, error);
        }
        std::cout << "}\n";
    } else {
        std::cout << "checkpoint replay " << (ok ? "passed" : "failed")
                  << ": status=" << sandbox::vm::vmStatusToString(result.status)
                  << " steps=" << result.steps << " pc=" << result.final_pc
                  << " cycles=" << snapshot.cycles << "\n";
        if (!error.empty()) std::cerr << error << "\n";
    }
    return ok ? 0 : 4;
}
