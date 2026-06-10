#include "../ternary_asm.h"
#include "../ternary_vm.h"

#include <chrono>
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
    if (backend == VMExecutionBackend::TraceJit) {
        vm.setTraceJitHotThreshold(4);
    }

    const int max_steps = iterations * 4 + 32;
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

BenchmarkRun bestOf(
    const std::vector<TritWord27>& program,
    VMExecutionBackend backend,
    int iterations,
    int repeats) {

    BenchmarkRun best;
    best.micros = std::numeric_limits<long long>::max();
    for (int i = 0; i < repeats; ++i) {
        BenchmarkRun run = runHotLoop(program, backend, iterations);
        if (run.micros < best.micros) best = run;
    }
    return best;
}

} // namespace

int main() {
    LongTriple::initPowTable();

    constexpr int kIterations = 20000;
    const auto program = assembleOrThrow(R"(
        mov r1, 0
        mov r2, 1
        mov r3, 20000
    loop:
        add r1, r1, r2
        sub r3, r3, r2
        brp r3, loop
        halt
    )");

    const BenchmarkRun interpreter =
        bestOf(program, VMExecutionBackend::Interpreter, kIterations, 3);
    const BenchmarkRun trace_jit =
        bestOf(program, VMExecutionBackend::TraceJit, kIterations, 3);

    require(interpreter.result == kIterations, "interpreter result matches iteration count");
    require(trace_jit.result == kIterations, "trace JIT result matches iteration count");
    require(interpreter.steps == trace_jit.steps, "trace JIT step count matches interpreter");
    require(trace_jit.trace_instructions > kIterations,
            "trace JIT executes the hot loop through traces");
    require(trace_jit.decode_count * 10 < interpreter.decode_count,
            "trace JIT sharply reduces hot-loop decode work");

    const double speedup = trace_jit.micros == 0
        ? static_cast<double>(interpreter.micros)
        : static_cast<double>(interpreter.micros) /
              static_cast<double>(trace_jit.micros);
    const double decode_speedup = trace_jit.decode_count == 0
        ? static_cast<double>(interpreter.decode_count)
        : static_cast<double>(interpreter.decode_count) /
              static_cast<double>(trace_jit.decode_count);

    std::cout << "trace_jit_hot_loop iterations=" << kIterations
              << " interpreter_us=" << interpreter.micros
              << " trace_jit_us=" << trace_jit.micros
              << " speedup=" << speedup
              << " decode_speedup=" << decode_speedup
              << " interpreter_decodes=" << interpreter.decode_count
              << " trace_jit_decodes=" << trace_jit.decode_count
              << " trace_jit_instructions=" << trace_jit.trace_instructions
              << "\n";

    require(decode_speedup >= 100.0, "trace JIT hot-loop decode speedup is clear");

    return EXIT_SUCCESS;
}
