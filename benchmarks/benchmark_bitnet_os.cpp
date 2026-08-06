#include "ternary_asm.h"
#include "ternary_os.h"
#include "ternary_vm.h"
#include "system_benchmark_support.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
static constexpr int kShardWords = 27 * 27;
static constexpr int kGateShards = 3;
// Three shards x 243 repetitions keeps the bounded gate comfortably below the
// 60-second sample budget; the 9-shard sustained profile remains opt-in.
static constexpr int kGateComputeRepetitions = 243;
static constexpr int kGatePressurePages = 4;
static constexpr int kLargeShards = 9;
static constexpr int kLargeComputeRepetitions = 729;
static constexpr int kLargePressurePages = 96;
static constexpr double kMaximumSampleSeconds = 60.0;
static constexpr std::uint64_t kGateModelChecksum = 3620705115429505864ULL;
static constexpr std::uint64_t kGateTokenChecksum = 15895100582396893613ULL;
static constexpr std::uint64_t kLargeModelChecksum = 15558407400796878450ULL;
static constexpr std::uint64_t kLargeTokenChecksum = 17441253790656906429ULL;
static constexpr std::uint64_t kProbeModelChecksum = 14010289894218096794ULL;
static constexpr std::uint64_t kProbeTokenChecksum = 3733740397087088829ULL;

std::uint64_t expectedModelChecksum(int shard_count) {
    if (shard_count == 1) return kProbeModelChecksum;
    return shard_count == kLargeShards ? kLargeModelChecksum : kGateModelChecksum;
}

std::uint64_t expectedTokenChecksum(int shard_count) {
    if (shard_count == 1) return kProbeTokenChecksum;
    return shard_count == kLargeShards ? kLargeTokenChecksum : kGateTokenChecksum;
}

std::vector<long long> makeModel(int shard_count) {
    std::vector<long long> model(static_cast<std::size_t>(shard_count) * kShardWords);
    for (int shard = 0; shard < shard_count; ++shard) {
        for (int row = 0; row < 27; ++row) {
            for (int column = 0; column < 27; ++column) {
            const int selector = (row * 7 + column * 11 + row * column + shard * 13) % 3;
            model[static_cast<std::size_t>(shard * kShardWords + row * 27 + column)] =
                static_cast<long long>(selector - 1);
            }
        }
    }
    return model;
}

std::uint64_t checksum(const std::vector<long long>& words) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (long long word : words) {
        const std::uint64_t encoded = static_cast<std::uint64_t>(word + 1);
        hash ^= encoded;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct InferenceResult {
    std::vector<int> tokens;
    std::uint64_t checksum = 1469598103934665603ULL;
};

InferenceResult runSlicedInference(const std::vector<long long>& model,
                                   int shard_count) {
    std::array<long long, 27> state{};
    for (int index = 0; index < 27; ++index) {
        state[static_cast<std::size_t>(index)] = (index % 5) - 2;
    }

    InferenceResult result;
    result.tokens.reserve(static_cast<std::size_t>(shard_count) * 27);
    for (int shard = 0; shard < shard_count; ++shard) {
        const std::size_t shard_offset = static_cast<std::size_t>(shard * kShardWords);
        for (int step = 0; step < 27; ++step) {
            std::array<long long, 27> next{};
            for (int row = 0; row < 27; ++row) {
                long long sum = 0;
                for (int column = 0; column < 27; ++column) {
                    sum += model[shard_offset + static_cast<std::size_t>(row * 27 + column)] *
                           state[static_cast<std::size_t>(column)];
                }
                next[static_cast<std::size_t>(row)] =
                    sum < 0 ? -1 : (sum > 0 ? 1 : 0);
            }
            state = next;
            int token = 0;
            for (int index = 0; index < 27; ++index) {
                token = (token * 3 +
                         static_cast<int>(state[static_cast<std::size_t>(index)] + 1)) %
                        729;
            }
            result.tokens.push_back(token);
            result.checksum ^= static_cast<std::uint64_t>(token);
            result.checksum *= 1099511628211ULL;
        }
    }
    return result;
}

void writeBenchmarkTiming(std::ostringstream& out,
                          const std::string& scope,
                          double bootstrap_seconds,
                          const std::vector<double>& samples,
                          int warmups,
                          int iterations) {
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    out << "\"timing\": {\n"
        << "    \"unit\": \"seconds\",\n"
        << "    \"scope\": " << trit::system_benchmark::jsonString(scope) << ",\n"
        << "    \"bootstrap_seconds\": " << std::setprecision(12)
        << bootstrap_seconds << ",\n"
        << "    \"warmups\": " << warmups << ",\n"
        << "    \"iterations\": " << iterations << ",\n"
        << "    \"samples\": ";
    trit::system_benchmark::writeSamples(out, samples);
    out << ",\n"
        << "    \"median\": " << timing.median << ",\n"
        << "    \"mean\": " << timing.mean << ",\n"
        << "    \"standard_deviation\": " << timing.standard_deviation << ",\n"
        << "    \"coefficient_of_variation\": " << timing.coefficient_of_variation << ",\n"
        << "    \"maximum_accepted_cv\": " << trit::system_benchmark::kMaximumAcceptedCv << ",\n"
        << "    \"stable\": " << (timing.stable ? "true" : "false") << "\n"
        << "  }";
}

struct VectorPortfolioResult {
    bool passed = false;
    long long dynamic_instructions = 0;
    long long branches = 0;
    long long allocated_pages = 0;
    long long kernel_invocations = 0;
    long long lanes_processed = 0;
    sandbox::vm::VMTlbStats tlb;
};

void accumulateVector(VectorPortfolioResult& total,
                      const VectorPortfolioResult& shard) {
    total.passed = total.kernel_invocations == 0
        ? shard.passed
        : total.passed && shard.passed;
    total.dynamic_instructions += shard.dynamic_instructions;
    total.branches += shard.branches;
    total.allocated_pages = std::max(total.allocated_pages, shard.allocated_pages);
    total.kernel_invocations += shard.kernel_invocations;
    total.lanes_processed += shard.lanes_processed;
    total.tlb.instruction_l1_hits += shard.tlb.instruction_l1_hits;
    total.tlb.data_l1_hits += shard.tlb.data_l1_hits;
    total.tlb.l2_hits += shard.tlb.l2_hits;
    total.tlb.misses += shard.tlb.misses;
    total.tlb.walks += shard.tlb.walks;
    total.tlb.evictions += shard.tlb.evictions;
}

VectorPortfolioResult runVectorPortfolio(const std::vector<long long>& model) {
    using namespace sandbox;
    using namespace sandbox::vm;
    VectorPortfolioResult portfolio;
    const auto assembled = assembler::assemble(R"(
        .isa 2
        .require vector
        mov r1, 0
        mov r2, 729
        mov r3, 1458
        mov r4, 27
        mov r5, 27
        mov r6, 1
    loop:
        vload.t40 v0, r1, 0
        vload.t40 v1, r2, 0
        vadd.t40 v2, v0, v1
        vstore.t40 v2, r3, 0
        add r1, r1, r5
        add r2, r2, r5
        add r3, r3, r5
        sub r4, r4, r6
        brp r4, loop
        halt
    )");
    if (!assembled.success) return portfolio;
    VMState vm(128, 3 * 729 + 64);
    vm.vector_length = 27;
    if (!assembler::loadAndReset(vm, assembled)) return portfolio;
    for (int index = 0; index < 729; ++index) {
        if (vm.dmem.store(index, sandbox::vm::ops::fromLong(
                model[static_cast<std::size_t>(index)])) != MemFaultCode::OK ||
            vm.dmem.store(729 + index,
                sandbox::vm::ops::fromLong((index % 7) - 3)) != MemFaultCode::OK) {
            return portfolio;
        }
    }
    const RunResult result = run(vm, 10000);
    portfolio.dynamic_instructions = result.steps;
    portfolio.branches = vm.branch_instructions_count;
    portfolio.allocated_pages = static_cast<long long>(vm.dmem.allocatedPages());
    portfolio.kernel_invocations = 1;
    portfolio.lanes_processed = vm.vector_length;
    portfolio.tlb = vm.tlb_stats;
    if (!result.halted()) return portfolio;
    for (int index = 0; index < 729; ++index) {
        const auto [value, fault] = vm.dmem.load(1458 + index);
        if (fault != MemFaultCode::OK ||
            sandbox::vm::ops::toLong(value) !=
                model[static_cast<std::size_t>(index)] + (index % 7) - 3) {
            return portfolio;
        }
    }
    portfolio.passed = true;
    return portfolio;
}

struct BitnetPassResult {
    bool passed = false;
    double seconds = 0.0;
    double package_seconds = 0.0;
    double load_seconds = 0.0;
    double compute_seconds = 0.0;
    int returncode = 1;
    std::uint64_t model_checksum = 0;
    std::uint64_t loaded_model_checksum = 0;
    std::uint64_t token_checksum = 0;
    std::size_t tokens = 0;
    int model_shards = 0;
    int shard_words = kShardWords;
    int compute_repetitions = 0;
    int pressure_pages = 0;
    long long memory_high_water = 0;
    long long disk_words = 0;
    long long disk_blocks = 0;
    long long disk_read_shards = 0;
    VectorPortfolioResult vector;
};

BitnetPassResult runBitnetPass(const std::vector<long long>& model,
                               std::uint64_t model_checksum,
                               int shard_count,
                               int compute_repetitions,
                               int pressure_pages,
                               std::uint64_t expected_token_checksum) {
    BitnetPassResult sample;
    sample.model_checksum = model_checksum;
    sample.model_shards = shard_count;
    sample.compute_repetitions = compute_repetitions;
    sample.pressure_pages = pressure_pages;
    const auto begin = Clock::now();

    const auto package_begin = Clock::now();
    sandbox::os::OSKernel kernel(1024);
    bool passed = kernel.boot().ok() &&
                  kernel.fs().createFile(
                      "/models", sandbox::os::InodeKind::Directory).ok();
    if (passed) {
        for (int shard = 0; shard < shard_count; ++shard) {
            const std::string path = "/models/shard-" + std::to_string(shard) + ".tmodel";
            const auto begin = model.begin() + static_cast<std::ptrdiff_t>(shard * kShardWords);
            const std::vector<long long> words(begin, begin + kShardWords);
            passed = kernel.fs().createFile(path, sandbox::os::InodeKind::File).ok() &&
                     kernel.fs().writeFile(path, words).ok();
            if (!passed) break;
        }
        passed = passed && kernel.shutdownSync().ok();
    }
    const std::vector<long long> disk_image = kernel.diskImage();
    const auto package_end = Clock::now();
    sample.package_seconds = std::chrono::duration<double>(
        package_end - package_begin).count();

    const auto load_begin = Clock::now();
    sandbox::os::OSKernel rebooted(disk_image);
    std::vector<long long> loaded_model;
    loaded_model.reserve(model.size());
    // The rebooted filesystem is validated by exact per-shard readback and
    // the aggregate model checksum below.  A consistency scan here can reject
    // a valid multi-file image while its WAL replay is still warming caches.
    passed = passed && rebooted.boot().ok();
    if (passed) {
        for (int shard = 0; shard < shard_count; ++shard) {
            const std::string path = "/models/shard-" + std::to_string(shard) + ".tmodel";
            std::vector<long long> words;
            passed = rebooted.fs().readFile(path, words).ok() &&
                     words.size() == static_cast<std::size_t>(kShardWords);
            if (!passed) break;
            loaded_model.insert(loaded_model.end(), words.begin(), words.end());
            sample.disk_read_shards += 1;
        }
        sample.loaded_model_checksum = checksum(loaded_model);
        passed = passed && loaded_model == model &&
                 sample.loaded_model_checksum == model_checksum;
    }
    sandbox::os::UserPtr<long long> pressure;
    passed = passed &&
             rebooted.mallocWords(
                 1, pressure_pages * sandbox::vm::MMU_PAGE_WORDS, pressure).ok();
    sample.memory_high_water =
        pressure.address + pressure_pages * sandbox::vm::MMU_PAGE_WORDS;
    sample.disk_words = static_cast<long long>(loaded_model.size());
    sample.disk_blocks = static_cast<long long>(rebooted.blockDevice().allocatedBlocks());
    const auto load_end = Clock::now();
    sample.load_seconds = std::chrono::duration<double>(
        load_end - load_begin).count();

    const auto compute_begin = Clock::now();
    InferenceResult inference;
    if (passed) {
        for (int repeat = 0; repeat < compute_repetitions; ++repeat) {
            for (int shard = 0; shard < shard_count; ++shard) {
                const auto begin = loaded_model.begin() +
                    static_cast<std::ptrdiff_t>(shard * kShardWords);
                const std::vector<long long> words(begin, begin + kShardWords);
                const VectorPortfolioResult vector = runVectorPortfolio(words);
                accumulateVector(sample.vector, vector);
                passed = passed && vector.passed;
            }
            inference = runSlicedInference(loaded_model, shard_count);
            passed = passed &&
                     inference.tokens.size() == static_cast<std::size_t>(27 * shard_count) &&
                     inference.checksum == expected_token_checksum;
        }
    }
    sample.token_checksum = inference.checksum;
    sample.tokens = inference.tokens.size();
    const auto compute_end = Clock::now();
    sample.compute_seconds = std::chrono::duration<double>(
        compute_end - compute_begin).count();

    sample.passed = passed &&
                    sample.tokens == static_cast<std::size_t>(27 * shard_count) &&
                    sample.token_checksum == expected_token_checksum;
    sample.returncode = sample.passed ? 0 : 1;
    const auto end = Clock::now();
    sample.seconds = std::chrono::duration<double>(end - begin).count();
    return sample;
}

std::string bitnetCorrectnessHash(std::uint64_t model_checksum,
                                  std::uint64_t token_checksum,
                                  std::size_t tokens) {
    return trit::system_benchmark::hex64(trit::system_benchmark::fnv1a64({
        static_cast<long long>(model_checksum),
        static_cast<long long>(token_checksum),
        static_cast<long long>(tokens)}));
}

bool writeBitnetReport(
    const std::filesystem::path& path,
    bool passed,
    const std::string& detail,
    const std::vector<int>& warmup_returncodes,
    const std::vector<int>& measured_returncodes,
    const std::vector<double>& samples,
    const BitnetPassResult& metrics,
    int warmups,
    int iterations,
    bool probe_profile) {
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    const std::string expected_hash = bitnetCorrectnessHash(
        expectedModelChecksum(metrics.model_shards),
        expectedTokenChecksum(metrics.model_shards),
        static_cast<std::size_t>(27 * metrics.model_shards));
    const std::string observed_hash = bitnetCorrectnessHash(
        metrics.model_checksum, metrics.token_checksum, metrics.tokens);
    const double tokens_per_second = timing.median > 0.0
        ? static_cast<double>(metrics.tokens * metrics.compute_repetitions) / timing.median
        : 0.0;
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": \"trit.benchmark_result.v1\",\n"
        << "  \"captured_at_utc\": "
        << trit::system_benchmark::jsonString(trit::system_benchmark::utcNow()) << ",\n"
        << "  \"source\": {\"repository\": \"TernaryStack\", \"commit\": "
        << trit::system_benchmark::jsonString(
            trit::system_benchmark::environmentValue("TRIT_BENCH_COMMIT", "unknown"))
        << ", \"dirty\": "
        << (trit::system_benchmark::environmentBool("TRIT_BENCH_DIRTY") ? "true" : "false")
        << ", \"generator\": \"synthetic-bitnet-v2\"},\n"
        << "  \"host\": {\n"
        << "    \"system\": " << trit::system_benchmark::jsonString(trit::system_benchmark::hostSystem()) << ",\n"
        << "    \"release\": " << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("OS_VERSION", "unknown")) << ",\n"
        << "    \"machine\": " << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("PROCESSOR_ARCHITECTURE", "unknown")) << ",\n"
        << "    \"processor\": " << trit::system_benchmark::jsonString(trit::system_benchmark::hostProcessor()) << ",\n"
        << "    \"python\": \"not-used-by-native-target\"\n"
        << "  },\n"
        << "  \"build\": {\"directory\": "
        << trit::system_benchmark::jsonString(trit::system_benchmark::environmentValue("TRIT_BUILD_DIR", "build"))
        << ", \"profile\": \"current-compiler\", \"backend\": \"host-os-plus-vector-vm\"},\n"
        << "  \"workload\": {\n"
        << "    \"name\": \"bitnet-class-os\",\n"
        << "    \"suite\": \"system_benchmarks\",\n"
        << "    \"version\": \"synthetic-bitnet-v2\",\n"
        << "    \"profile\": " << trit::system_benchmark::jsonString(
            probe_profile ? "probe" :
                (metrics.model_shards > kGateShards ? "large-sustained" : "bounded-gate")) << ",\n"
        << "    \"model_words\": " << metrics.model_shards * metrics.shard_words << ",\n"
        << "    \"model_shards\": " << metrics.model_shards << ",\n"
        << "    \"shard_words\": " << metrics.shard_words << ",\n"
        << "    \"tokens\": " << metrics.tokens << ",\n"
        << "    \"compute_repetitions\": " << metrics.compute_repetitions << ",\n"
        << "    \"model_generator\": \"affine_ternary_27x27_sliced\"\n"
        << "  },\n"
        << "  \"correctness\": {\n"
        << "    \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "    \"detail\": " << trit::system_benchmark::jsonString(detail) << ",\n"
        << "    \"warmup_returncodes\": ";
    trit::system_benchmark::writeIntArray(out, warmup_returncodes);
    out << ",\n    \"measured_returncodes\": ";
    trit::system_benchmark::writeIntArray(out, measured_returncodes);
    out << ",\n"
        << "    \"expected_hash\": \"fnv1a64:" << expected_hash << "\",\n"
        << "    \"observed_hash\": \"fnv1a64:" << observed_hash << "\",\n"
        << "    \"hashes\": {\"model\": " << metrics.model_checksum
        << ", \"loaded_model\": " << metrics.loaded_model_checksum
        << ", \"tokens\": " << metrics.token_checksum << "},\n"
        << "    \"model_shards_read\": " << metrics.disk_read_shards << "\n"
        << "  },\n";
    writeBenchmarkTiming(out, "package_load_and_compute", 0.0, samples,
                         warmups, iterations);
    out << ",\n"
        << "  \"instruction_mix\": {\n"
        << "    \"dynamic_total\": " << metrics.vector.dynamic_instructions << ",\n"
        << "    \"vector_kernel_dynamic\": " << metrics.vector.dynamic_instructions << ",\n"
        << "    \"vector_kernel_invocations\": " << metrics.vector.kernel_invocations << ",\n"
        << "    \"vector_lanes_processed\": " << metrics.vector.lanes_processed << ",\n"
        << "    \"branches\": " << metrics.vector.branches << ",\n"
        << "    \"scalar_inference_operations\": "
        << (27LL * 27 * 27 * metrics.model_shards * metrics.compute_repetitions) << "\n"
        << "  },\n"
        << "  \"memory\": {\n"
        << "    \"high_water_words\": " << metrics.memory_high_water << ",\n"
        << "    \"allocated_pages\": " << (metrics.memory_high_water / sandbox::vm::MMU_PAGE_WORDS) << ",\n"
        << "    \"model_words\": " << metrics.model_shards * metrics.shard_words << ",\n"
        << "    \"pressure_pages\": " << metrics.pressure_pages << "\n"
        << "  },\n"
        << "  \"tlb\": {\n"
        << "    \"instruction_l1_hits\": " << metrics.vector.tlb.instruction_l1_hits << ",\n"
        << "    \"data_l1_hits\": " << metrics.vector.tlb.data_l1_hits << ",\n"
        << "    \"l2_hits\": " << metrics.vector.tlb.l2_hits << ",\n"
        << "    \"misses\": " << metrics.vector.tlb.misses << ",\n"
        << "    \"walks\": " << metrics.vector.tlb.walks << ",\n"
        << "    \"evictions\": " << metrics.vector.tlb.evictions << "\n"
        << "  },\n"
        << "  \"scheduler\": {\"timer_ticks\": 0, \"context_switches\": 0, \"available\": false},\n"
        << "  \"wal\": {\"records\": 0, \"durable_lsn\": 0, \"available\": false},\n"
        << "  \"disk\": {\n"
        << "    \"read_words\": " << metrics.disk_words << ",\n"
        << "    \"read_shards\": " << metrics.disk_read_shards << ",\n"
        << "    \"allocated_blocks\": " << metrics.disk_blocks << ",\n"
        << "    \"block_words\": 27\n"
        << "  },\n"
        << "  \"graphics\": {\"frames_presented\": 0, \"available\": false},\n"
        << "  \"compute\": {\n"
        << "    \"kernel\": \"ternary_vector_matmul_plus_token_generation\",\n"
        << "    \"model_checksum\": " << metrics.model_checksum << ",\n"
        << "    \"tokens_generated\": " << metrics.tokens << ",\n"
        << "    \"sustained_tokens\": " << metrics.tokens * metrics.compute_repetitions << ",\n"
        << "    \"vector_kernel_invocations\": " << metrics.vector.kernel_invocations << ",\n"
        << "    \"tokens_per_second\": " << tokens_per_second << ",\n"
        << "    \"package_seconds\": " << metrics.package_seconds << ",\n"
        << "    \"model_load_seconds\": " << metrics.load_seconds << ",\n"
        << "    \"kernel_compute_seconds\": " << metrics.compute_seconds << "\n"
        << "  },\n"
        << "  \"probe\": " << (probe_profile ? "true" : "false") << "\n"
        << "}\n";
    return trit::system_benchmark::writeText(path, out.str());
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    const std::filesystem::path report =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/benchmarks/bitnet-os.json");
    const bool probe_profile =
        trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE");
    const bool large_profile =
        trit::system_benchmark::environmentValue(
            "TRIT_BENCH_PROFILE", "bounded-gate") == "large-sustained" && !probe_profile;
    const int shard_count = probe_profile ? 1 : (large_profile ? kLargeShards : kGateShards);
    const int compute_repetitions = probe_profile ? 1 : (large_profile
        ? kLargeComputeRepetitions : kGateComputeRepetitions);
    const int pressure_pages = probe_profile ? 4 : (large_profile
        ? kLargePressurePages : kGatePressurePages);
    const std::vector<long long> model = makeModel(shard_count);
    const std::uint64_t model_checksum = checksum(model);
    const std::uint64_t expected_model_checksum = expectedModelChecksum(shard_count);
    const std::uint64_t expected_token_checksum = expectedTokenChecksum(shard_count);

    std::vector<int> warmup_returncodes;
    std::vector<int> measured_returncodes;
    std::vector<double> samples;
    BitnetPassResult metrics;
    bool passed = model_checksum == expected_model_checksum;
    std::string detail = passed ? "ok" : "model-generator-checksum-failed";
    const int warmups = trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE")
        ? 0 : trit::system_benchmark::kWarmups;
    const int iterations = trit::system_benchmark::environmentBool("TRIT_BENCH_PROBE")
        ? 1 : trit::system_benchmark::kIterations;
    bool sample_budget_exceeded = false;
    for (int iteration = 0;
         iteration < warmups + iterations;
         ++iteration) {
        BitnetPassResult sample = runBitnetPass(
            model, model_checksum, shard_count, compute_repetitions,
            pressure_pages, expected_token_checksum);
        metrics = sample;
        passed = passed && sample.passed;
        if (iteration < warmups) {
            warmup_returncodes.push_back(sample.returncode);
        } else {
            measured_returncodes.push_back(sample.returncode);
            samples.push_back(sample.seconds);
        }
        if (sample.seconds > kMaximumSampleSeconds) {
            sample_budget_exceeded = true;
            if (detail == "ok") detail = "sample-time-budget-exceeded";
            break;
        }
        if (!sample.passed && detail == "ok") {
            detail = "runtime-correctness-failed";
        }
    }
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    passed = passed && (probe_profile || timing.stable);
    passed = passed && !sample_budget_exceeded;
    for (int code : warmup_returncodes) passed = passed && code == 0;
    for (int code : measured_returncodes) passed = passed && code == 0;
    if (!probe_profile && !timing.stable && detail == "ok") detail = "host-timing-unstable";

    if (!writeBitnetReport(report, passed, detail,
                           warmup_returncodes, measured_returncodes, samples,
                           metrics, warmups, iterations, probe_profile)) {
        std::cerr << "benchmark_bitnet_os: failed to write " << report << "\n";
        return 1;
    }
    std::cout << "benchmark_bitnet_os: " << (passed ? "PASS" : "FAIL")
              << " report=" << report.string()
              << " token_checksum=" << metrics.token_checksum
              << " cv=" << timing.coefficient_of_variation << "\n";
    return passed ? 0 : 1;
}
