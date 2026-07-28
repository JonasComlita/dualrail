#include "../ternary_asm.h"
#include "../ternary_vm.h"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using sandbox::LongTriple;
using namespace sandbox::isa;
using namespace sandbox::vm;
using namespace sandbox::vm::assembler;

struct BenchmarkRun {
    long long micros = 0;
    int steps = 0;
    long long decode_count = 0;
    long long trace_instructions = 0;
    long long result = 0;
};

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(EXIT_FAILURE);
}

BenchmarkRun runHotLoop(
    const std::vector<TritWord27>& program,
    VMExecutionBackend backend,
    int iterations) {

    VMState vm(64, 256);
    require(loadAndReset(vm, program), "benchmark program loads");
    vm.setExecutionBackend(backend);
    if (backend == VMExecutionBackend::DecodedTraceExecutor ||
        backend == VMExecutionBackend::NativeX64Jit) {
        vm.setDecodedTraceHotThreshold(4);
    }

    const int max_steps = iterations * 8 + 64;
    const auto start = std::chrono::steady_clock::now();
    const auto result = sandbox::vm::run(vm, max_steps);
    const auto stop = std::chrono::steady_clock::now();
    require(result.halted(), "benchmark run halts");

    BenchmarkRun run;
    run.micros = std::chrono::duration_cast<std::chrono::microseconds>(
        stop - start).count();
    run.steps = result.steps;
    run.decode_count = vm.decode_instructions_count;
    run.trace_instructions = vm.trace_jit_stats.instructions_executed;
    run.result = ops::toLong(vm.regfile.read(R1));
    return run;
}

BenchmarkRun medianOf(
    const std::vector<TritWord27>& program,
    VMExecutionBackend backend,
    int iterations,
    int warmups,
    int repeats) {

    for (int i = 0; i < warmups; ++i)
        (void)runHotLoop(program, backend, iterations);
    std::vector<BenchmarkRun> runs;
    for (int i = 0; i < repeats; ++i) {
        runs.push_back(runHotLoop(program, backend, iterations));
    }
    std::sort(runs.begin(), runs.end(),
        [](const BenchmarkRun& lhs, const BenchmarkRun& rhs) {
            return lhs.micros < rhs.micros;
        });
    return runs[runs.size() / 2];
}

} // namespace

int main() {
    LongTriple::initPowTable();

    constexpr int kIterations = 20000;
    struct Workload {
        const char* name;
        std::vector<TritWord27> program;
    };
    const std::vector<Workload> workloads = {
        {"arithmetic", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 20000
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )")},
        {"guarded_memory", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 20000
            mov r4, 20
        loop:
            store r1, r4, 0
            load r5, r4, 0
            add r1, r5, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )")},
        {"branch_exit", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 20000
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brz r3, done
            brp r3, loop
        done:
            halt
        )")},
    };

    int native_speedup_wins = 0;
    bool native_third_regression_ok = true;
    for (const Workload& workload : workloads) {
        const BenchmarkRun interpreter = medianOf(
            workload.program, VMExecutionBackend::Interpreter,
            kIterations, 2, 7);
        const BenchmarkRun decoded = medianOf(
            workload.program, VMExecutionBackend::DecodedTraceExecutor,
            kIterations, 2, 7);
        require(interpreter.result == kIterations,
                std::string(workload.name) + " interpreter result matches");
        require(decoded.result == kIterations,
                std::string(workload.name) + " decoded trace result matches");
        require(interpreter.steps == decoded.steps,
                std::string(workload.name) + " decoded trace step parity");
        require(decoded.trace_instructions > kIterations,
                std::string(workload.name) +
                    " executes through decoded traces");

        const double decoded_speedup = decoded.micros == 0
            ? static_cast<double>(interpreter.micros)
            : static_cast<double>(interpreter.micros) /
                  static_cast<double>(decoded.micros);
        std::cout << "decoded_trace workload=" << workload.name
                  << " iterations=" << kIterations
                  << " interpreter_median_us=" << interpreter.micros
                  << " decoded_median_us=" << decoded.micros
                  << " wall_speedup=" << decoded_speedup
                  << " interpreter_decodes=" << interpreter.decode_count
                  << " decoded_decodes=" << decoded.decode_count
                  << "\n";

        if (!nativeX64HostAvailable()) continue;
        const BenchmarkRun native = medianOf(
            workload.program, VMExecutionBackend::NativeX64Jit,
            kIterations, 2, 7);
        require(native.result == kIterations,
                std::string(workload.name) + " native result matches");
        require(native.steps == interpreter.steps,
                std::string(workload.name) + " native step parity");
        const double native_speedup = native.micros == 0
            ? static_cast<double>(interpreter.micros)
            : static_cast<double>(interpreter.micros) /
                  static_cast<double>(native.micros);
        if (native_speedup >= 1.15) ++native_speedup_wins;
        if (native_speedup < 0.97) native_third_regression_ok = false;
        std::cout << "native_x64_jit workload=" << workload.name
                  << " native_median_us=" << native.micros
                  << " wall_speedup=" << native_speedup
                  << "\n";
    }

    const bool native_gate_passed =
        nativeX64HostAvailable() &&
        native_speedup_wins >= 2 &&
        native_third_regression_ok;
    VMState default_vm(8, 8);
    require(!default_vm.nativeX64JitEnabled(),
            "native JIT remains disabled by default");
    std::cout << "native_x64_default_gate passed="
              << (native_gate_passed ? "true" : "false")
              << " wins_1_15=" << native_speedup_wins
              << " third_within_3_percent="
              << (native_third_regression_ok ? "true" : "false")
              << "\n";

    return EXIT_SUCCESS;
}
