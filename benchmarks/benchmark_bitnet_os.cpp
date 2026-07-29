#include "ternary_asm.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

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
    for (int index = 0; index < 27; ++index)
        state[static_cast<std::size_t>(index)] = (index % 5) - 2;

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

bool runVectorPortfolio(const std::vector<long long>& model,
                        long long& dynamic_instructions) {
    using namespace sandbox;
    using namespace sandbox::vm;
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
    if (!assembled.success) return false;
    VMState vm(128, 3 * 729 + 64);
    vm.vector_length = 27;
    if (!assembler::loadAndReset(vm, assembled)) return false;
    for (int index = 0; index < 729; ++index) {
        if (vm.dmem.store(index, sandbox::vm::ops::fromLong(
                model[static_cast<std::size_t>(index)])) !=
            MemFaultCode::OK) {
            return false;
        }
        if (vm.dmem.store(729 + index,
                          sandbox::vm::ops::fromLong((index % 7) - 3)) !=
            MemFaultCode::OK) {
            return false;
        }
    }
    const RunResult result = run(vm, 10000);
    dynamic_instructions = result.steps;
    if (!result.halted()) return false;
    for (int index = 0; index < 729; ++index) {
        const auto [value, fault] = vm.dmem.load(1458 + index);
        if (fault != MemFaultCode::OK ||
            sandbox::vm::ops::toLong(value) !=
                model[static_cast<std::size_t>(index)] + (index % 7) - 3) {
            return false;
        }
    }
    return true;
}

bool writeReport(const std::filesystem::path& path,
                 bool passed,
                 double package_ms,
                 double load_ms,
                 double compute_ms,
                 long long vector_instructions,
                 std::uint64_t model_checksum,
                 std::uint64_t token_checksum,
                 std::size_t tokens,
                 long long memory_high_water,
                 long long disk_words) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    const double tokens_per_second =
        compute_ms > 0.0 ? static_cast<double>(tokens) * 1000.0 / compute_ms : 0.0;
    out << "{\n"
        << "  \"schema\": \"trit.benchmark_result.v1\",\n"
        << "  \"source\": {\"identity\": \"synthetic-bitnet-v1\"},\n"
        << "  \"host\": {\"profile\": \"portable\"},\n"
        << "  \"build\": {\"profile\": \"current\"},\n"
        << "  \"workload\": {\"name\": \"bitnet-class-os\", "
           "\"model_words\": 729, \"tokens\": 27},\n"
        << "  \"correctness\": {\"passed\": "
        << (passed ? "true" : "false")
        << ", \"model_checksum\": " << model_checksum
        << ", \"token_checksum\": " << token_checksum << "},\n"
        << "  \"timing\": {\"unit\": \"milliseconds\", "
           "\"package\": " << package_ms << ", \"model_load\": " << load_ms
        << ", \"kernel_compute\": " << compute_ms
        << ", \"warmups\": 0, \"iterations\": 1, "
           "\"coefficient_of_variation\": 0.0},\n"
        << "  \"instruction_mix\": {\"vector_kernel_dynamic\": "
        << vector_instructions << "},\n"
        << "  \"memory\": {\"high_water_words\": " << memory_high_water << "},\n"
        << "  \"tlb\": {},\n"
        << "  \"scheduler\": {},\n"
        << "  \"wal\": {},\n"
        << "  \"disk\": {\"read_words\": " << disk_words << "},\n"
        << "  \"inference\": {\"tokens_generated\": " << tokens
        << ", \"tokens_per_second\": " << tokens_per_second << "}\n"
        << "}\n";
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();
    const std::filesystem::path report =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("build/benchmarks/bitnet-os.json");
    const std::vector<long long> model = makeModel();
    const std::uint64_t model_checksum = checksum(model);

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
    const long long memory_high_water =
        pressure.address + 4 * sandbox::vm::MMU_PAGE_WORDS;
    const auto load_end = Clock::now();

    long long vector_instructions = 0;
    const auto compute_begin = Clock::now();
    passed = passed && runVectorPortfolio(loaded_model, vector_instructions);
    const InferenceResult inference = runTinyInference(loaded_model);
    const auto compute_end = Clock::now();

    // This value is the reference output of synthetic-bitnet-v1. A change
    // requires an intentional workload-version bump.
    static constexpr std::uint64_t kExpectedTokenChecksum =
        3733740397087088829ULL;
    passed = passed && inference.tokens.size() == 27 &&
             inference.checksum == kExpectedTokenChecksum;

    if (!writeReport(report, passed,
                     milliseconds(package_begin, package_end),
                     milliseconds(load_begin, load_end),
                     milliseconds(compute_begin, compute_end),
                     vector_instructions, model_checksum, inference.checksum,
                     inference.tokens.size(), memory_high_water,
                     static_cast<long long>(loaded_model.size()))) {
        std::cerr << "benchmark_bitnet_os: failed to write " << report << "\n";
        return 1;
    }
    std::cout << "benchmark_bitnet_os: " << (passed ? "PASS" : "FAIL")
              << " report=" << report.string()
              << " token_checksum=" << inference.checksum << "\n";
    return passed ? 0 : 1;
}
