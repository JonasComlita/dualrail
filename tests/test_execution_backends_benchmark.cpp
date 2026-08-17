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
    long long helper_instructions = 0;
    long long portable_side_exits = 0;
    long long blocks_built = 0;
    long long result = 0;
    int status = 0;
    int final_pc = 0;
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
    if (backend == VMExecutionBackend::NativeX64Jit) {
        for (const auto& cached : vm.native_x64_code_cache) {
            const auto block =
                std::static_pointer_cast<VMNativeX64CodeBlock>(cached.second);
            run.helper_instructions +=
                static_cast<long long>(block->helper_instruction_count);
        }
    }
    run.portable_side_exits =
        vm.native_x64_jit_stats.portable_side_exits;
    run.blocks_built = vm.native_x64_jit_stats.blocks_built;
    run.result = ops::toLong(vm.regfile.read(R1));
    run.status = static_cast<int>(result.status);
    run.final_pc = result.final_pc;
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
        static_cast<std::int64_t>(run.helper_instructions)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.portable_side_exits)));
    mix(static_cast<std::uint64_t>(
        static_cast<std::int64_t>(run.blocks_built)));
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
    int repeats,
    const char* record_workload = nullptr,
    const char* record_backend = nullptr) {

    for (int i = 0; i < warmups; ++i)
        (void)runHotLoop(program, backend, iterations);
    std::vector<BenchmarkRun> runs;
    for (int i = 0; i < repeats; ++i) {
        BenchmarkRun run = runHotLoop(program, backend, iterations);
        if (record_workload != nullptr && record_backend != nullptr) {
            std::cout << "native_x64_sample"
                      << " version=1"
                      << " workload=" << record_workload
                      << " backend=" << record_backend
                      << " sample=" << i
                      << " micros=" << run.micros
                      << " status=" << run.status
                      << " final_pc=" << run.final_pc
                      << " steps=" << run.steps
                      << " decode_count=" << run.decode_count
                      << " trace_instructions=" << run.trace_instructions
                      << " cycles=" << run.cycles
                      << " direct_instructions="
                      << run.direct_instructions
                      << " helper_instructions="
                      << run.helper_instructions
                      << " portable_side_exits="
                      << run.portable_side_exits
                      << " blocks_built=" << run.blocks_built
                      << " result=" << run.result
                      << " fingerprint=" << run.correctness_fingerprint
                      << "\n";
        }
        runs.push_back(run);
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
    // backend work rather than one-time process/JIT noise.  The source
    // fixtures use a textual count marker so the architectural result and
    // the measured work stay tied to this one acceptance constant.
    constexpr int kIterations = 1000000;
    const auto assembleBenchmarkProgram = [&](const char* source) {
        std::string expanded(source);
        const std::string marker = "200000";
        std::size_t position = 0;
        while ((position = expanded.find(marker, position)) !=
               std::string::npos) {
            expanded.replace(position, marker.size(),
                             std::to_string(kIterations));
            position += std::to_string(kIterations).size();
        }
        return assembleOrThrow(expanded);
    };
    struct Workload {
        const char* name;
        std::vector<TritWord27> program;
    };
    const std::vector<Workload> workloads = {
        {"arithmetic", assembleBenchmarkProgram(R"(
            mov r1, 0
            mov r2, 1
            mov r3, 200000
        loop:
            add r1, r1, r2
            sub r3, r3, r2
            brp r3, loop
            halt
        )")},
        {"guarded_memory", assembleBenchmarkProgram(R"(
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
        {"branch_exit", assembleBenchmarkProgram(R"(
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
            kIterations, 2, 7, workload.name, "interpreter");
        const BenchmarkRun decoded = medianOf(
            workload.program, VMExecutionBackend::DecodedTraceExecutor,
            kIterations, 2, 7, workload.name, "decoded");
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
            kIterations, 2, 7, workload.name, "native");
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
                  << " native_direct=" << native.direct_instructions
                  << " native_helper=" << native.helper_instructions
                  << " native_side_exits=" << native.portable_side_exits
                  << " wall_speedup=" << native_speedup
                  << "\n";
    }

    // Keep dynamic control as a separate acceptance workload so it proves
    // direct RET/CALLR/JMPR lowering without changing the historical
    // three-workload default-on speed gate.  The target is loaded in a T40
    // register, CALLR aliases a stable indirect target, RET returns to the
    // loop body before BRP re-enters the loop, and a final JMPR reaches HALT.
    const auto dynamic_control_program = assembleBenchmarkProgram(R"(
        mov r1, 0
        mov r2, 1
        mov r3, 200000
        mov r4, 11
    loop:
        callr r4
        sub r3, r3, r2
        brp r3, loop
        mov r5, 13
        jmpr r5
        halt
        nop
        add r1, r1, r2
        ret
        halt
    )");
    const BenchmarkRun dynamic_interpreter = medianOf(
        dynamic_control_program, VMExecutionBackend::Interpreter,
        kIterations, 2, 7, "dynamic_control", "interpreter");
    const BenchmarkRun dynamic_decoded = medianOf(
        dynamic_control_program, VMExecutionBackend::DecodedTraceExecutor,
        kIterations, 2, 7, "dynamic_control", "decoded");
    require(dynamic_interpreter.result == kIterations,
            "dynamic_control interpreter result matches");
    require(dynamic_decoded.result == kIterations,
            "dynamic_control decoded result matches");
    require(dynamic_decoded.steps == dynamic_interpreter.steps,
            "dynamic_control decoded step parity");
    require(dynamic_decoded.trace_instructions > kIterations,
            "dynamic_control executes through decoded traces");
    measurements_stable = measurements_stable &&
        dynamic_interpreter.coefficient_of_variation < 0.03 &&
        dynamic_decoded.coefficient_of_variation < 0.03;
    deterministic_repeats = deterministic_repeats &&
        dynamic_interpreter.deterministic_repeatable &&
        dynamic_decoded.deterministic_repeatable;

    BenchmarkRun dynamic_native;
    bool dynamic_control_gate_passed = !nativeX64HostAvailable();
    bool dynamic_control_repeatable = !nativeX64HostAvailable();
    bool dynamic_control_stable = !nativeX64HostAvailable();
    if (nativeX64HostAvailable()) {
        dynamic_native = medianOf(
            dynamic_control_program, VMExecutionBackend::NativeX64Jit,
            kIterations, 2, 7, "dynamic_control", "native");
        require(dynamic_native.result == kIterations,
                "dynamic_control native result matches");
        require(dynamic_native.steps == dynamic_interpreter.steps,
                "dynamic_control native step parity");
        dynamic_control_stable =
            dynamic_native.coefficient_of_variation < 0.03;
        dynamic_control_repeatable = dynamic_native.deterministic_repeatable;
        const double dynamic_native_speedup = dynamic_native.micros == 0
            ? static_cast<double>(dynamic_interpreter.micros)
            : static_cast<double>(dynamic_interpreter.micros) /
                  static_cast<double>(dynamic_native.micros);
        dynamic_control_gate_passed =
            dynamic_native.direct_instructions > 0 &&
            dynamic_native.helper_instructions == 0 &&
            dynamic_native.portable_side_exits == 0 &&
            dynamic_control_repeatable && dynamic_control_stable &&
            dynamic_native_speedup >= (1.0 / 1.03);
        std::cout << "native_x64_jit workload=dynamic_control"
                  << " native_median_us=" << dynamic_native.micros
                  << " native_cv=" << dynamic_native.coefficient_of_variation
                  << " native_repeatable="
                  << (dynamic_native.deterministic_repeatable ? "true" : "false")
                  << " native_direct=" << dynamic_native.direct_instructions
                  << " native_side_exits="
                  << dynamic_native.portable_side_exits
                  << " native_helper=" << dynamic_native.helper_instructions
                  << " native_blocks_built=" << dynamic_native.blocks_built
                  << " wall_speedup=" << dynamic_native_speedup
                  << " gate_passed="
                  << (dynamic_control_gate_passed ? "true" : "false")
                  << "\n";
    }
    std::cout << "native_x64_dynamic_control_record"
              << " host_x64=" << (nativeX64HostAvailable() ? "true" : "false")
              << " deterministic_repeats="
              << ((!nativeX64HostAvailable() ||
                   dynamic_control_repeatable)
                      ? "true" : "false")
              << " cv_under_3_percent="
              << (dynamic_control_stable ? "true" : "false")
              << " direct_commits="
              << (nativeX64HostAvailable()
                      ? dynamic_native.direct_instructions : 0)
              << " portable_side_exits="
              << (nativeX64HostAvailable()
                      ? dynamic_native.portable_side_exits : 0)
              << " helper_lowerings="
              << (nativeX64HostAvailable()
                      ? dynamic_native.helper_instructions : 0)
              << " gate_passed="
              << (dynamic_control_gate_passed ? "true" : "false")
              << "\n";

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
              << " dynamic_control_passed="
              << (dynamic_control_gate_passed ? "true" : "false")
              << " dynamic_control_max_slowdown_3_percent="
              << ((!nativeX64HostAvailable() ||
                   dynamic_native.micros == 0 ||
                   static_cast<double>(dynamic_interpreter.micros) /
                           static_cast<double>(dynamic_native.micros) >=
                       (1.0 / 1.03))
                      ? "true" : "false")
              << " gate_passed=" << (native_gate_passed ? "true" : "false")
              << "\n";

    // A measured native backend gate is an acceptance test, not an advisory
    // log line.  Keep non-x86 hosts portable, but fail CI on x86-64 until the
    // two-of-three 1.15x speedup and third-workload regression contract is
    // actually met.
    if (nativeX64HostAvailable() &&
        (!native_gate_passed || !dynamic_control_gate_passed)) {
        std::cerr << "native x86-64 acceptance gate failed\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
