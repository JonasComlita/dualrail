// Deterministic SSA-only compiler corpus acceptance gate.
//
// This is deliberately a guest-execution benchmark rather than an assembly
// size proxy.  Each workload is compiled twice through compileSource(): once
// with OptimizationLevel::None and once with the default CompilerOptions.  A
// fresh interpreter VM executes each linked image twice; RunResult::steps is
// the architectural guest-instruction count used by the gate.

#include "ternary_compiler.h"
#include "ternary_vm.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using sandbox::compiler::CompileResult;
using sandbox::compiler::CompilerOptions;
using sandbox::compiler::LinkResult;
using sandbox::compiler::OptimizationLevel;

constexpr double kMinimumMedianReduction = 0.15;
constexpr double kMaximumWorkloadRegression = 0.05;
constexpr int kCountRepetitions = 2;
constexpr int kMaximumGuestSteps = 1000000;

struct Workload {
    const char* id;
    const char* source;
    std::vector<const char*> categories;
    long long expected_return;
    const char* expected_syscall;
};

struct RunObservation {
    bool compiled = false;
    bool linked = false;
    bool loaded = false;
    bool halted = false;
    long long returned = 0;
    int steps = 0;
    std::string syscall_buffer;
    std::vector<std::string> diagnostics;
};

struct ModeResult {
    CompilerOptions options;
    bool compiled = false;
    bool linked = false;
    bool deterministic = false;
    bool correct = false;
    std::array<RunObservation, kCountRepetitions> runs{};
    int median_steps = 0;
};

struct WorkloadResult {
    const Workload* workload = nullptr;
    std::string source_hash;
    ModeResult o0;
    ModeResult optimized;
    bool equivalent = false;
    double reduction = 0.0;
    bool passed = false;
};

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(c)
                        << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string jsonString(const std::string& value) {
    return "\"" + jsonEscape(value) + "\"";
}

std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::uint64_t fnv1a64(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string commandOutput(const char* command) {
#ifdef _WIN32
    FILE* pipe = _popen(command, "r");
#else
    FILE* pipe = popen(command, "r");
#endif
    if (!pipe) return {};
    std::string output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r' ||
            output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    return output;
}

std::string currentCommit() {
#ifdef _WIN32
    return commandOutput("git rev-parse HEAD 2>NUL");
#else
    return commandOutput("git rev-parse HEAD 2>/dev/null");
#endif
}

bool currentTreeDirty() {
#ifdef _WIN32
    return !commandOutput("git status --porcelain 2>NUL").empty();
#else
    return !commandOutput("git status --porcelain 2>/dev/null").empty();
#endif
}

std::string diagnosticSummary(const CompileResult& compiled) {
    std::ostringstream out;
    const std::size_t limit = std::min<std::size_t>(compiled.diagnostics.size(), 4);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) out << " | ";
        out << compiled.diagnostics[i].message;
    }
    return out.str();
}

RunObservation runImage(const LinkResult& linked,
                         long long expected_return,
                         const char* expected_syscall) {
    RunObservation observation;
    observation.linked = linked.success;
    if (!linked.success) {
        for (const auto& diagnostic : linked.diagnostics) {
            observation.diagnostics.push_back(diagnostic.message);
        }
        return observation;
    }

    sandbox::vm::VMState vm(4096, 4096);
    vm.setExecutionBackend(sandbox::vm::VMExecutionBackend::Interpreter);
    observation.loaded = sandbox::vm::assembler::loadAndReset(vm, linked.assembled);
    if (!observation.loaded) return observation;

    const auto result = sandbox::vm::run(vm, kMaximumGuestSteps);
    observation.halted = result.halted();
    observation.steps = result.steps;
    observation.returned = sandbox::vm::ops::toLong(vm.regfile.read(13));
    observation.syscall_buffer = vm.syscall_buffer;
    observation.compiled = observation.halted;
    if (!observation.halted) observation.diagnostics.push_back(result.description);
    if (observation.halted && observation.returned != expected_return) {
        observation.diagnostics.push_back("unexpected return value");
    }
    if (observation.halted && observation.syscall_buffer != expected_syscall) {
        observation.diagnostics.push_back("unexpected syscall output");
    }
    return observation;
}

ModeResult measureMode(const Workload& workload, OptimizationLevel level) {
    ModeResult result;
    result.options.optimization = level;
    if (level == OptimizationLevel::None) {
        result.options.optimization = OptimizationLevel::None;
    }

    const CompileResult compiled =
        sandbox::compiler::compileSource(workload.id, workload.source, result.options);
    result.compiled = compiled.success;
    if (!compiled.success) {
        result.runs[0].diagnostics.push_back(diagnosticSummary(compiled));
        result.runs[1].diagnostics.push_back(diagnosticSummary(compiled));
        return result;
    }

    const LinkResult linked = sandbox::compiler::linkModules({compiled.object});
    result.linked = linked.success;
    if (!linked.success) {
        for (auto& run : result.runs) {
            run.linked = false;
            for (const auto& diagnostic : linked.diagnostics) {
                run.diagnostics.push_back(diagnostic.message);
            }
        }
        return result;
    }

    for (auto& run : result.runs) {
        run = runImage(linked, workload.expected_return, workload.expected_syscall);
    }
    result.deterministic = result.runs[0].halted && result.runs[1].halted &&
                           result.runs[0].steps == result.runs[1].steps &&
                           result.runs[0].returned == result.runs[1].returned &&
                           result.runs[0].syscall_buffer == result.runs[1].syscall_buffer;
    result.correct = result.deterministic;
    for (const auto& run : result.runs) {
        result.correct = result.correct && run.halted &&
                         run.returned == workload.expected_return &&
                         run.syscall_buffer == workload.expected_syscall;
    }
    if (result.correct) result.median_steps = result.runs[0].steps;
    return result;
}

double median(std::vector<int> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 != 0) return static_cast<double>(values[middle]);
    return (static_cast<double>(values[middle - 1]) +
            static_cast<double>(values[middle])) /
           2.0;
}

double reductionFor(int o0, int optimized) {
    if (o0 <= 0) return 0.0;
    return static_cast<double>(o0 - optimized) / static_cast<double>(o0);
}

std::string modeJson(const ModeResult& mode) {
    std::ostringstream out;
    out << "{\"compiled\":" << (mode.compiled ? "true" : "false")
        << ",\"linked\":" << (mode.linked ? "true" : "false")
        << ",\"correct\":" << (mode.correct ? "true" : "false")
        << ",\"deterministic\":" << (mode.deterministic ? "true" : "false")
        << ",\"repetitions\":[";
    for (int i = 0; i < kCountRepetitions; ++i) {
        if (i != 0) out << ',';
        out << mode.runs[i].steps;
    }
    out << "]"
        << ",\"median\":" << mode.median_steps
        << ",\"returns\":[";
    for (int i = 0; i < kCountRepetitions; ++i) {
        if (i != 0) out << ',';
        out << mode.runs[i].returned;
    }
    out << "]"
        << ",\"syscalls\":[";
    for (int i = 0; i < kCountRepetitions; ++i) {
        if (i != 0) out << ',';
        out << jsonString(mode.runs[i].syscall_buffer);
    }
    out << "]";
    std::vector<std::string> diagnostics;
    for (const auto& run : mode.runs) {
        diagnostics.insert(diagnostics.end(), run.diagnostics.begin(), run.diagnostics.end());
    }
    out << ",\"diagnostics\":[";
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0) out << ',';
        out << jsonString(diagnostics[i]);
    }
    out << "]}";
    return out.str();
}

void writeWorkloadJson(std::ostringstream& out, const WorkloadResult& result) {
    out << "{\"id\":" << jsonString(result.workload->id)
        << ",\"source_hash\":" << jsonString(result.source_hash)
        << ",\"categories\":[";
    for (std::size_t i = 0; i < result.workload->categories.size(); ++i) {
        if (i != 0) out << ',';
        out << jsonString(result.workload->categories[i]);
    }
    out << "]"
        << ",\"expected_return\":" << result.workload->expected_return
        << ",\"expected_syscall\":" << jsonString(result.workload->expected_syscall)
        << ",\"correctness\":{\"equivalent\":"
        << (result.equivalent ? "true" : "false")
        << ",\"o0\":{\"compiled\":"
        << (result.o0.compiled ? "true" : "false")
        << ",\"linked\":" << (result.o0.linked ? "true" : "false")
        << ",\"correct\":" << (result.o0.correct ? "true" : "false")
        << "},\"optimized\":{\"compiled\":"
        << (result.optimized.compiled ? "true" : "false")
        << ",\"linked\":" << (result.optimized.linked ? "true" : "false")
        << ",\"correct\":" << (result.optimized.correct ? "true" : "false")
        << "}}"
        << ",\"counts\":{\"o0\": " << modeJson(result.o0)
        << ",\"optimized\": " << modeJson(result.optimized) << "}"
        << ",\"reduction\":" << std::setprecision(17) << result.reduction
        << ",\"passed\":" << (result.passed ? "true" : "false") << "}";
}

std::vector<Workload> corpus() {
    return {
        {
            "loop_phi",
            R"TRIT(
                fn inc(x: t40) -> t40 { return x + 1; }
                fn main() -> t40 {
                    var i: t40 = 0;
                    var acc: t40 = 0;
                    while 9 - i > 0 {
                        acc = acc + i;
                        i = inc(i);
                    }
                    return acc;
                }
            )TRIT",
            {"loops", "phis", "calls"},
            36,
            "",
        },
        {
            "calls",
            R"TRIT(
                fn mix(a: t40, b: t40, c: t40, d: t40, e: t40, f: t40, g: t40) -> t40 {
                    return a + b + c + d + e + f + g;
                }
                fn main() -> t40 {
                    let a = mix(1, 2, 3, 4, 5, 6, 7);
                    let b = mix(8, 9, 10, 11, 12, 13, 14);
                    let c = mix(15, 16, 17, 18, 19, 20, 21);
                    return a + b + c;
                }
            )TRIT",
            {"calls"},
            231,
            "",
        },
        {
            "spills",
            R"TRIT(
                fn identity(x: t40) -> t40 { return x; }
                fn main() -> t40 {
                    let v01 = identity(1); let v02 = identity(2);
                    let v03 = identity(3); let v04 = identity(4);
                    let v05 = identity(5); let v06 = identity(6);
                    let v07 = identity(7); let v08 = identity(8);
                    let v09 = identity(9); let v10 = identity(10);
                    let v11 = identity(11); let v12 = identity(12);
                    let v13 = identity(13); let v14 = identity(14);
                    let v15 = identity(15); let v16 = identity(16);
                    let v17 = identity(17); let v18 = identity(18);
                    let v19 = identity(19); let v20 = identity(20);
                    let v21 = identity(21); let v22 = identity(22);
                    let v23 = identity(23); let v24 = identity(24);
                    return v01 + v02 + v03 + v04 + v05 + v06 +
                           v07 + v08 + v09 + v10 + v11 + v12 +
                           v13 + v14 + v15 + v16 + v17 + v18 +
                           v19 + v20 + v21 + v22 + v23 + v24;
                }
            )TRIT",
            {"spills", "calls"},
            300,
            "",
        },
        {
            "aggregates",
            R"TRIT(
                struct Pair { a: t40; b: t40; }
                fn sum_pair(p: Pair) -> t40 { return p.a + p.b; }
                fn main() -> t40 {
                    var p: Pair = Pair { a: 4, b: 5 };
                    var xs: [t40; 3] = [1, 2, 3];
                    p.b = p.a + xs[2];
                    xs[1] = sum_pair(p);
                    return xs[1] + p.b;
                }
            )TRIT",
            {"aggregates", "memory_alias"},
            18,
            "",
        },
        {
            "aggregate_abi",
            R"TRIT(
                struct Pair { a: t40; b: t40; }
                fn combine(a: Pair, bias: t40, b: Pair, tail: t40,
                           c: Pair, extra: t40, d: Pair, last: t40) -> t40 {
                    return a.a + a.b + bias + b.a + b.b + tail +
                           c.a + c.b + extra + d.a + d.b + last;
                }
                fn main() -> t40 {
                    var left: Pair = Pair { a: 1, b: 2 };
                    var right: Pair = Pair { a: 3, b: 4 };
                    var third: Pair = Pair { a: 5, b: 6 };
                    var fourth: Pair = Pair { a: 7, b: 8 };
                    return combine(left, 9, right, 10,
                                   third, 11, fourth, 12);
                }
            )TRIT",
            {"aggregates", "calls", "abi"},
            78,
            "",
        },
        {
            "ownership",
            R"TRIT(
                fn alloc(words: t40) -> own<ptr<t40, unknown>> {
                    return 44 + words;
                }
                fn free(ptr: borrow<ptr<t40, unknown>>) -> t40 { return 0; }
                fn main() -> t40 {
                    let x: own<ptr<t40, unknown>> = alloc(12);
                    return 7;
                }
            )TRIT",
            {"ownership", "calls"},
            7,
            "",
        },
        {
            "atomics",
            R"TRIT(
                fn main() -> t40 {
                    unsafe {
                        let old = tldr(120, 1);
                        tstr(120, 33, old, -1);
                        return tldr(120, 1);
                    }
                }
            )TRIT",
            {"atomics", "memory_alias"},
            33,
            "",
        },
        {
            "branches",
            R"TRIT(
                fn classify(x: t40) -> t40 {
                    match x {
                        neg => { return -3; }
                        zero => { return 0; }
                        pos => { return 5; }
                    }
                }
                fn main() -> t40 {
                    let a = classify(-4);
                    let b = classify(0);
                    let c = classify(9);
                    return a + b + c;
                }
            )TRIT",
            {"branches"},
            2,
            "",
        },
        {
            "memory_alias",
            R"TRIT(
                fn main() -> t40 {
                    var value: t40 = 4;
                    var raw: t40 = 120;
                    let p = &value;
                    unsafe {
                        store(raw, 9);
                        *p = *p + load(raw);
                        return load(raw) + *p;
                    }
                }
            )TRIT",
            {"memory_alias", "branches"},
            22,
            "",
        },
        {
            "memory_alias_call",
            R"TRIT(
                fn increment(x: t40) -> t40 { return x + 1; }
                fn main() -> t40 {
                    var raw: t40 = 120;
                    unsafe { store(raw, 41); }
                    let delta = increment(1);
                    unsafe { return load(raw) + delta; }
                }
            )TRIT",
            {"memory_alias", "calls", "call_clobber"},
            43,
            "",
        },
    };
}

} // namespace

int main() {
    const std::vector<Workload> workloads = corpus();
    std::vector<WorkloadResult> results;
    results.reserve(workloads.size());
    std::string corpus_text;
    for (const auto& workload : workloads) {
        corpus_text += workload.id;
        corpus_text.push_back('\0');
        corpus_text += workload.source;
        WorkloadResult result;
        result.workload = &workload;
        result.source_hash = "fnv1a64:" + hex64(fnv1a64(workload.source));
        result.o0 = measureMode(workload, OptimizationLevel::None);
        result.optimized = measureMode(workload, CompilerOptions{}.optimization);
        result.equivalent = result.o0.correct && result.optimized.correct &&
                            result.o0.runs[0].returned == result.optimized.runs[0].returned &&
                            result.o0.runs[0].syscall_buffer == result.optimized.runs[0].syscall_buffer;
        result.reduction = reductionFor(result.o0.median_steps, result.optimized.median_steps);
        result.passed = result.equivalent &&
                        result.reduction >= -kMaximumWorkloadRegression;
        results.push_back(std::move(result));
    }

    std::vector<int> o0_medians;
    std::vector<int> optimized_medians;
    o0_medians.reserve(results.size());
    optimized_medians.reserve(results.size());
    std::vector<std::string> failures;
    double max_regression = 0.0;
    for (const auto& result : results) {
        if (result.o0.correct) o0_medians.push_back(result.o0.median_steps);
        if (result.optimized.correct) optimized_medians.push_back(result.optimized.median_steps);
        max_regression = std::max(max_regression, -result.reduction);
        if (!result.equivalent) failures.push_back(std::string(result.workload->id) + ": correctness");
        if (result.reduction < -kMaximumWorkloadRegression) {
            failures.push_back(std::string(result.workload->id) + ": regression");
        }
    }
    const double o0_median = median(o0_medians);
    const double optimized_median = median(optimized_medians);
    const double aggregate_reduction = o0_median > 0.0
        ? (o0_median - optimized_median) / o0_median
        : 0.0;
    if (aggregate_reduction < kMinimumMedianReduction) {
        failures.push_back("aggregate: median reduction below threshold");
    }
    const bool passed = failures.empty() &&
                        results.size() == workloads.size() &&
                        aggregate_reduction >= kMinimumMedianReduction &&
                        max_regression <= kMaximumWorkloadRegression;

    const std::string commit = currentCommit();
    std::ostringstream out;
    out << "{\"schema\":\"trit.compiler_corpus_gate.v1\""
        << ",\"source\":{\"repository\":\"trit\",\"commit\":\""
        << jsonEscape(commit.empty() ? "unknown" : commit)
        << "\",\"dirty\":" << (currentTreeDirty() ? "true" : "false")
        << ",\"corpus_id\":\"compiler-corpus.v1\",\"corpus_hash\":"
        << jsonString("fnv1a64:" + hex64(fnv1a64(corpus_text)))
        << ",\"pipeline\":\"typed-ast,address-cfg-ir,verify,optimize,allocate,target-ir-only-fail-closed\"}"
        << ",\"policy\":{\"o0\":\"none\",\"optimized\":\"default\",\"count_repetitions\":"
        << kCountRepetitions << ",\"minimum_median_reduction\":"
        << std::setprecision(17) << kMinimumMedianReduction
        << ",\"maximum_workload_regression\":" << kMaximumWorkloadRegression << "}"
        << ",\"workloads\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (i != 0) out << ',';
        writeWorkloadJson(out, results[i]);
    }
    out << "]"
        << ",\"aggregate\":{\"o0_median\":" << std::setprecision(17) << o0_median
        << ",\"optimized_median\":" << optimized_median
        << ",\"median_reduction\":" << aggregate_reduction
        << ",\"maximum_workload_regression\":" << max_regression << "}"
        << ",\"gate\":{\"passed\":" << (passed ? "true" : "false")
        << ",\"verdict\":\"" << (passed ? "pass" : "fail") << "\",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i != 0) out << ',';
        out << jsonString(failures[i]);
    }
    out << "]}}\n";
    std::cout << out.str();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
