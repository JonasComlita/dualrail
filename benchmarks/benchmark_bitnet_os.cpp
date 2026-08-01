#include "ternary_asm.h"
#include "ternary_os.h"
#include "ternary_vm.h"
#include "system_benchmark_support.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
static constexpr std::uint64_t kExpectedTokenChecksum =
    3733740397087088829ULL;
static constexpr int kComputeRepetitions = 729;

std::vector<long long> makeModel() {
    std::vector<long long> model(27 * 27);
    for (int row = 0; row < 27; ++row) {
        for (int column = 0; column < 27; ++column) {
            const int selector = (row * 7 + column * 11 + row * column) % 3;
            model[static_cast<std::size_t>(row * 27 + column)] =
                static_cast<long long>(selector - 1);
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

InferenceResult runTinyInference(const std::vector<long long>& model) {
    std::array<long long, 27> state{};
    for (int index = 0; index < 27; ++index) {
        state[static_cast<std::size_t>(index)] = (index % 5) - 2;
    }

    InferenceResult result;
    result.tokens.reserve(27);
    for (int step = 0; step < 27; ++step) {
        std::array<long long, 27> next{};
        for (int row = 0; row < 27; ++row) {
            long long sum = 0;
            for (int column = 0; column < 27; ++column) {
                sum += model[static_cast<std::size_t>(row * 27 + column)] *
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
    return result;
}

struct VectorPortfolioResult {
    bool passed = false;
    long long dynamic_instructions = 0;
    long long branches = 0;
    long long allocated_pages = 0;
    sandbox::vm::VMTlbStats tlb;
};

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
    std::uint64_t token_checksum = 0;
    std::size_t tokens = 0;
    long long memory_high_water = 0;
    long long disk_words = 0;
    long long disk_blocks = 0;
    VectorPortfolioResult vector;
};

BitnetPassResult runBitnetPass(const std::vector<long long>& model,
                               std::uint64_t model_checksum) {
    BitnetPassResult sample;
    sample.model_checksum = model_checksum;
    const auto begin = Clock::now();

    const auto package_begin = Clock::now();
    sandbox::os::OSKernel kernel(1024);
    bool passed = kernel.boot().ok() &&
                  kernel.fs().createFile(
                      "/models", sandbox::os::InodeKind::Directory).ok() &&
                  kernel.fs().createFile(
                      "/models/tiny-bitnet.tmodel",
                      sandbox::os::InodeKind::File).ok() &&
                  kernel.fs().writeFile(
                      "/models/tiny-bitnet.tmodel", model).ok() &&
                  kernel.shutdownSync().ok();
    const std::vector<long long> disk_image = kernel.diskImage();
    const auto package_end = Clock::now();
    sample.package_seconds = std::chrono::duration<double>(
        package_end - package_begin).count();

    const auto load_begin = Clock::now();
    sandbox::os::OSKernel rebooted(disk_image);
    std::vector<long long> loaded_model;
    passed = passed && rebooted.boot().ok() &&
             rebooted.fs().readFile(
                 "/models/tiny-bitnet.tmodel", loaded_model).ok() &&
             loaded_model == model && checksum(loaded_model) == model_checksum;
    sandbox::os::UserPtr<long long> pressure;
    passed = passed &&
             rebooted.mallocWords(1, 4 * sandbox::vm::MMU_PAGE_WORDS, pressure).ok();
    sample.memory_high_water =
        pressure.address + 4 * sandbox::vm::MMU_PAGE_WORDS;
    sample.disk_words = static_cast<long long>(loaded_model.size());
    sample.disk_blocks = static_cast<long long>(rebooted.blockDevice().allocatedBlocks());
    const auto load_end = Clock::now();
    sample.load_seconds = std::chrono::duration<double>(
        load_end - load_begin).count();

    const auto compute_begin = Clock::now();
    InferenceResult inference;
    if (passed) {
        for (int repeat = 0; repeat < kComputeRepetitions; ++repeat) {
            sample.vector = runVectorPortfolio(loaded_model);
            inference = runTinyInference(loaded_model);
            passed = passed && sample.vector.passed &&
                     inference.tokens.size() == 27 &&
                     inference.checksum == kExpectedTokenChecksum;
        }
    }
    sample.token_checksum = inference.checksum;
    sample.tokens = inference.tokens.size();
    const auto compute_end = Clock::now();
    sample.compute_seconds = std::chrono::duration<double>(
        compute_end - compute_begin).count();

    sample.passed = passed && sample.tokens == 27 &&
                    sample.token_checksum == kExpectedTokenChecksum;
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
    const BitnetPassResult& metrics) {
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    const std::string expected_hash = bitnetCorrectnessHash(
        checksum(makeModel()), kExpectedTokenChecksum, 27);
    const std::string observed_hash = bitnetCorrectnessHash(
        metrics.model_checksum, metrics.token_checksum, metrics.tokens);
    const double tokens_per_second = timing.median > 0.0
        ? static_cast<double>(metrics.tokens) / timing.median
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
        << ", \"generator\": \"synthetic-bitnet-v1\"},\n"
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
        << "    \"version\": \"synthetic-bitnet-v1\",\n"
        << "    \"model_words\": 729,\n"
        << "    \"model_shards\": 1,\n"
        << "    \"tokens\": 27,\n"
        << "    \"compute_repetitions\": " << kComputeRepetitions << ",\n"
        << "    \"model_generator\": \"affine_ternary_27x27\"\n"
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
        << ", \"tokens\": " << metrics.token_checksum << "}\n"
        << "  },\n";
    trit::system_benchmark::writeTiming(out, "package_load_and_compute", 0.0, samples);
    out << ",\n"
        << "  \"instruction_mix\": {\n"
        << "    \"dynamic_total\": " << metrics.vector.dynamic_instructions * kComputeRepetitions << ",\n"
        << "    \"vector_kernel_dynamic\": " << metrics.vector.dynamic_instructions * kComputeRepetitions << ",\n"
        << "    \"branches\": " << metrics.vector.branches * kComputeRepetitions << ",\n"
        << "    \"scalar_inference_operations\": " << (27 * 27 * 27 * kComputeRepetitions) << "\n"
        << "  },\n"
        << "  \"memory\": {\n"
        << "    \"high_water_words\": " << metrics.memory_high_water << ",\n"
        << "    \"allocated_pages\": " << (metrics.memory_high_water / sandbox::vm::MMU_PAGE_WORDS) << ",\n"
        << "    \"model_words\": 729\n"
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
        << "    \"allocated_blocks\": " << metrics.disk_blocks << ",\n"
        << "    \"block_words\": 27\n"
        << "  },\n"
        << "  \"graphics\": {\"frames_presented\": 0, \"available\": false},\n"
        << "  \"compute\": {\n"
        << "    \"kernel\": \"ternary_vector_matmul_plus_token_generation\",\n"
        << "    \"model_checksum\": " << metrics.model_checksum << ",\n"
        << "    \"tokens_generated\": " << metrics.tokens << ",\n"
        << "    \"tokens_per_second\": " << tokens_per_second << ",\n"
        << "    \"package_seconds\": " << metrics.package_seconds << ",\n"
        << "    \"model_load_seconds\": " << metrics.load_seconds << ",\n"
        << "    \"kernel_compute_seconds\": " << metrics.compute_seconds << "\n"
        << "  }\n"
        << "}\n";
    return trit::system_benchmark::writeText(path, out.str());
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    const std::filesystem::path report =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/benchmarks/bitnet-os.json");
    const std::vector<long long> model = makeModel();
    const std::uint64_t model_checksum = checksum(model);

    std::vector<int> warmup_returncodes;
    std::vector<int> measured_returncodes;
    std::vector<double> samples;
    BitnetPassResult metrics;
    bool passed = true;
    std::string detail = "ok";
    for (int iteration = 0;
         iteration < trit::system_benchmark::kWarmups +
                     trit::system_benchmark::kIterations;
         ++iteration) {
        BitnetPassResult sample = runBitnetPass(model, model_checksum);
        metrics = sample;
        if (iteration < trit::system_benchmark::kWarmups) {
            warmup_returncodes.push_back(sample.returncode);
        } else {
            measured_returncodes.push_back(sample.returncode);
            samples.push_back(sample.seconds);
        }
        if (!sample.passed && detail == "ok") {
            detail = "runtime-correctness-failed";
        }
    }
    const auto timing = trit::system_benchmark::summarizeTiming(samples);
    passed = timing.stable;
    for (int code : warmup_returncodes) passed = passed && code == 0;
    for (int code : measured_returncodes) passed = passed && code == 0;
    if (!timing.stable && detail == "ok") detail = "host-timing-unstable";

    if (!writeBitnetReport(report, passed, detail,
                           warmup_returncodes, measured_returncodes, samples,
                           metrics)) {
        std::cerr << "benchmark_bitnet_os: failed to write " << report << "\n";
        return 1;
    }
    std::cout << "benchmark_bitnet_os: " << (passed ? "PASS" : "FAIL")
              << " report=" << report.string()
              << " token_checksum=" << metrics.token_checksum
              << " cv=" << timing.coefficient_of_variation << "\n";
    return passed ? 0 : 1;
}
