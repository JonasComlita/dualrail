#include "../ternary_asm.h"
#include "../ternary_vm.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    long long cycles = 0;
    long long direct_instructions = 0;
    long long portable_side_exits = 0;
    long long result = 0;
    double coefficient_of_variation = 0.0;
    std::uint64_t correctness_fingerprint = 0;
    bool deterministic_repeatable = false;
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
    run.cycles = vm.cycle_count;
    run.direct_instructions = vm.native_x64_jit_stats.direct_instructions;
    run.portable_side_exits =
        vm.native_x64_jit_stats.portable_side_exits;
    run.result = ops::toLong(vm.regfile.read(R1));
    // Keep timing out of the repeatability contract.  This FNV-1a digest
    // captures the deterministic architectural and backend accounting that
    // should be identical across every fresh run on a controlled host.
    const TernaryValue final_r1 = vm.regfile.read(R1);
    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) {
        fingerprint ^= value;
        fingerprint *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(result.status));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(result.final_pc)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(result.steps)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.cycles)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.decode_count)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.trace_instructions)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.direct_instructions)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.portable_side_exits)));
    mix(final_r1.bits.lo);
    mix(final_r1.bits.hi);
    mix(static_cast<std::uint64_t>(final_r1.mode));
    run.correctness_fingerprint = fingerprint;
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
    double mean = 0.0;
    for (const BenchmarkRun& run : runs)
        mean += static_cast<double>(run.micros);
    mean /= static_cast<double>(runs.size());
    double variance = 0.0;
    for (const BenchmarkRun& run : runs) {
        const double delta = static_cast<double>(run.micros) - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(runs.size());
    std::sort(runs.begin(), runs.end(),
        [](const BenchmarkRun& lhs, const BenchmarkRun& rhs) {
            return lhs.micros < rhs.micros;
        });
    BenchmarkRun median = runs[runs.size() / 2];
    median.coefficient_of_variation = mean == 0.0
        ? 0.0
        : std::sqrt(variance) / mean;
    median.deterministic_repeatable = true;
    for (const BenchmarkRun& run : runs) {
        median.deterministic_repeatable =
            median.deterministic_repeatable &&
            run.correctness_fingerprint == runs.front().correctness_fingerprint;
    }
    return median;
}

} // namespace

int main() {
    LongTriple::initPowTable();

    // Use a long steady-state loop so the required seven-run CV reflects
    // backend work rather than one-time process/JIT noise.
    constexpr int kIterations = 200000;
    struct Workload {
        const char* name;
        std::vector<TritWord27> program;
    };
    const std::vector<Workload> workloads = {
        {"arithmetic", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 200000
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )")},
        {"guarded_memory", assembleOrThrow(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 200000
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
            mov r3, 200000
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
    bool measurements_stable = true;
    bool deterministic_repeats = true;
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
        measurements_stable = measurements_stable &&
            interpreter.coefficient_of_variation < 0.03 &&
            decoded.coefficient_of_variation < 0.03;
        deterministic_repeats = deterministic_repeats &&
            interpreter.deterministic_repeatable &&
            decoded.deterministic_repeatable;

        const double decoded_speedup = decoded.micros == 0
            ? static_cast<double>(interpreter.micros)
            : static_cast<double>(interpreter.micros) /
                  static_cast<double>(decoded.micros);
        std::cout << "decoded_trace workload=" << workload.name
                  << " iterations=" << kIterations
                  << " interpreter_median_us=" << interpreter.micros
                  << " decoded_median_us=" << decoded.micros
                  << " interpreter_cv="
                  << interpreter.coefficient_of_variation
                  << " decoded_cv=" << decoded.coefficient_of_variation
                  << " interpreter_repeatable="
                  << (interpreter.deterministic_repeatable ? "true" : "false")
                  << " decoded_repeatable="
                  << (decoded.deterministic_repeatable ? "true" : "false")
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
        measurements_stable = measurements_stable &&
            native.coefficient_of_variation < 0.03;
        deterministic_repeats = deterministic_repeats &&
            native.deterministic_repeatable;
        const double native_speedup = native.micros == 0
            ? static_cast<double>(interpreter.micros)
            : static_cast<double>(interpreter.micros) /
                  static_cast<double>(native.micros);
        if (native_speedup >= 1.15) ++native_speedup_wins;
        if (native_speedup < 0.97) native_third_regression_ok = false;
        std::cout << "native_x64_jit workload=" << workload.name
                  << " native_median_us=" << native.micros
                  << " native_cv=" << native.coefficient_of_variation
                  << " native_repeatable="
                  << (native.deterministic_repeatable ? "true" : "false")
                  << " wall_speedup=" << native_speedup
                  << "\n";
    }

    const bool native_gate_passed =
        nativeX64HostAvailable() &&
        measurements_stable &&
        deterministic_repeats &&
        native_speedup_wins >= 2 &&
        native_third_regression_ok;
    VMState default_vm(8, 8);
    require(!default_vm.nativeX64JitEnabled(),
            "native JIT remains disabled by default");
    std::cout << "native_x64_default_gate passed="
              << (native_gate_passed ? "true" : "false")
              << " wins_1_15=" << native_speedup_wins
              << " cv_under_3_percent="
              << (measurements_stable ? "true" : "false")
              << " deterministic_repeats="
              << (deterministic_repeats ? "true" : "false")
              << " third_within_3_percent="
              << (native_third_regression_ok ? "true" : "false")
              << "\n";

    // This compact line is intentionally key/value machine-readable for
    // controlled-host evidence collectors without making the benchmark's
    // human-facing timing output a JSON protocol.
    std::cout << "native_x64_repeatability_record"
              << " host_x64=" << (nativeX64HostAvailable() ? "true" : "false")
              << " deterministic_repeats="
              << (deterministic_repeats ? "true" : "false")
              << " cv_under_3_percent="
              << (measurements_stable ? "true" : "false")
              << " wins_1_15=" << native_speedup_wins
              << " third_within_3_percent="
              << (native_third_regression_ok ? "true" : "false")
              << " gate_passed=" << (native_gate_passed ? "true" : "false")
              << "\n";

    // A measured native backend gate is an acceptance test, not an advisory
    // log line.  Keep non-x86 hosts portable, but fail CI on x86-64 until the
    // two-of-three 1.15x speedup and third-workload regression contract is
    // actually met.
    if (nativeX64HostAvailable() && !native_gate_passed) {
        std::cerr << "native x86-64 wall-time gate failed\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
