#define TERNARY_ENABLE_CUDA
#include "ternary_gpu_kernels.h"
#include "ternary_gpu_validation.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace sandbox::backend;
using namespace sandbox::backend::gpu;
using namespace sandbox::backend::gpu_validation;

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

template<typename T>
T* cudaAlloc(std::size_t count) {
    T* ptr = nullptr;
    const std::size_t allocCount = std::max<std::size_t>(count, 1);
    cudaError_t err = cudaMallocManaged(&ptr, allocCount * sizeof(T));
    if (err != cudaSuccess) return nullptr;
    return ptr;
}

template<typename T>
void copyToDevice(T* dst, const std::vector<T>& src) {
    for (std::size_t i = 0; i < src.size(); ++i) dst[i] = src[i];
}

template<typename T>
void copyFromDevice(std::vector<T>& dst, const T* src) {
    for (std::size_t i = 0; i < dst.size(); ++i) dst[i] = src[i];
}

void launch64(
    Operation op,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int blockSize) {

    switch (op) {
        case Operation::Neg: cudaLaunchTritwiseNegRaw64(a, out, count, trits, blockSize); break;
        case Operation::Add: cudaLaunchTritwiseAddRaw64(a, b, out, count, trits, blockSize); break;
        case Operation::Sub: cudaLaunchTritwiseSubRaw64(a, b, out, count, trits, blockSize); break;
        case Operation::Compare: cudaLaunchTritwiseCompareRaw64(a, b, cmp, count, trits, blockSize); break;
        case Operation::Min: cudaLaunchTritwiseMinRaw64(a, b, out, count, trits, blockSize); break;
        case Operation::Max: cudaLaunchTritwiseMaxRaw64(a, b, out, count, trits, blockSize); break;
    }
}

void launch128(
    Operation op,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int blockSize) {

    switch (op) {
        case Operation::Neg: cudaLaunchTritwiseNegRaw128(a, out, count, trits, blockSize); break;
        case Operation::Add: cudaLaunchTritwiseAddRaw128(a, b, out, count, trits, blockSize); break;
        case Operation::Sub: cudaLaunchTritwiseSubRaw128(a, b, out, count, trits, blockSize); break;
        case Operation::Compare: cudaLaunchTritwiseCompareRaw128(a, b, cmp, count, trits, blockSize); break;
        case Operation::Min: cudaLaunchTritwiseMinRaw128(a, b, out, count, trits, blockSize); break;
        case Operation::Max: cudaLaunchTritwiseMaxRaw128(a, b, out, count, trits, blockSize); break;
    }
}

long long timeLaunch64(
    Operation op,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int blockSize) {

    if (count == 0) return 0;
    launch64(op, a, b, out, cmp, count, trits, blockSize);
    cudaDeviceSynchronize();

    cudaEvent_t start{};
    cudaEvent_t stop{};
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start);
    launch64(op, a, b, out, cmp, count, trits, blockSize);
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float milliseconds = 0.0f;
    cudaEventElapsedTime(&milliseconds, start, stop);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    return static_cast<long long>(milliseconds * 1000.0f);
}

long long timeLaunch128(
    Operation op,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int blockSize) {

    if (count == 0) return 0;
    launch128(op, a, b, out, cmp, count, trits, blockSize);
    cudaDeviceSynchronize();

    cudaEvent_t start{};
    cudaEvent_t stop{};
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start);
    launch128(op, a, b, out, cmp, count, trits, blockSize);
    cudaEventRecord(stop);
    cudaEventSynchronize(stop);
    float milliseconds = 0.0f;
    cudaEventElapsedTime(&milliseconds, start, stop);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    return static_cast<long long>(milliseconds * 1000.0f);
}

void runRaw64Case(
    std::ofstream& md,
    const std::string& deviceName,
    const LaneCase& laneCase,
    Operation op,
    std::size_t count,
    int blockSize) {

    std::vector<uint64_t> hostA(count);
    std::vector<uint64_t> hostB(count);
    std::vector<uint64_t> hostOut(count);
    std::vector<uint8_t> hostCmp(count);
    fillRaw64Inputs(hostA, hostB, laneCase.trits);

    std::vector<uint64_t> expected = expectedRaw64(op, hostA, hostB, laneCase.trits);
    std::vector<uint8_t> expectedCmp = expectedCompare64(hostA, hostB, laneCase.trits);

    uint64_t* devA = cudaAlloc<uint64_t>(count);
    uint64_t* devB = cudaAlloc<uint64_t>(count);
    uint64_t* devOut = cudaAlloc<uint64_t>(count);
    uint8_t* devCmp = cudaAlloc<uint8_t>(count);
    expect(devA && devB && devOut && devCmp, "CUDA managed allocation raw64");
    if (!devA || !devB || !devOut || !devCmp) return;

    copyToDevice(devA, hostA);
    copyToDevice(devB, hostB);
    const long long runtimeUs = timeLaunch64(op, devA, devB, devOut, devCmp, count, laneCase.trits, blockSize);
    cudaError_t err = cudaDeviceSynchronize();
    expect(err == cudaSuccess, std::string("CUDA launch raw64 ") + cudaGetErrorString(err));

    copyFromDevice(hostOut, devOut);
    copyFromDevice(hostCmp, devCmp);

    std::string message;
    const bool ok = verifyRaw64(op, hostOut, hostCmp, expected, expectedCmp, message);
    expect(ok, std::string(laneCase.name) + " " + operationName(op) + " raw64 " + message);

    writeMarkdownRow(md, RunRecord{
        "CUDA",
        deviceName,
        compilerDescription(),
        operationName(op),
        laneCase.name,
        count,
        std::to_string(blockSize),
        hex64(checksumRaw64(op, hostOut, hostCmp)),
        runtimeUs,
        "real GPU",
        ok ? "pass" : message,
    });

    cudaFree(devA);
    cudaFree(devB);
    cudaFree(devOut);
    cudaFree(devCmp);
}

void runRaw128Case(
    std::ofstream& md,
    const std::string& deviceName,
    const LaneCase& laneCase,
    Operation op,
    std::size_t count,
    int blockSize) {

    std::vector<RawUInt128> hostA(count);
    std::vector<RawUInt128> hostB(count);
    std::vector<RawUInt128> hostOut(count);
    std::vector<uint8_t> hostCmp(count);
    fillRaw128Inputs(hostA, hostB, laneCase.trits);

    std::vector<RawUInt128> expected = expectedRaw128(op, hostA, hostB, laneCase.trits);
    std::vector<uint8_t> expectedCmp = expectedCompare128(hostA, hostB, laneCase.trits);

    RawUInt128* devA = cudaAlloc<RawUInt128>(count);
    RawUInt128* devB = cudaAlloc<RawUInt128>(count);
    RawUInt128* devOut = cudaAlloc<RawUInt128>(count);
    uint8_t* devCmp = cudaAlloc<uint8_t>(count);
    expect(devA && devB && devOut && devCmp, "CUDA managed allocation raw128");
    if (!devA || !devB || !devOut || !devCmp) return;

    copyToDevice(devA, hostA);
    copyToDevice(devB, hostB);
    const long long runtimeUs = timeLaunch128(op, devA, devB, devOut, devCmp, count, laneCase.trits, blockSize);
    cudaError_t err = cudaDeviceSynchronize();
    expect(err == cudaSuccess, std::string("CUDA launch raw128 ") + cudaGetErrorString(err));

    copyFromDevice(hostOut, devOut);
    copyFromDevice(hostCmp, devCmp);

    std::string message;
    const bool ok = verifyRaw128(op, hostOut, hostCmp, expected, expectedCmp, message);
    expect(ok, std::string(laneCase.name) + " " + operationName(op) + " raw128 " + message);

    writeMarkdownRow(md, RunRecord{
        "CUDA",
        deviceName,
        compilerDescription(),
        operationName(op),
        laneCase.name,
        count,
        std::to_string(blockSize),
        hex64(checksumRaw128(op, hostOut, hostCmp)),
        runtimeUs,
        "real GPU",
        ok ? "pass" : message,
    });

    cudaFree(devA);
    cudaFree(devB);
    cudaFree(devOut);
    cudaFree(devCmp);
}

} // namespace

int main() {
    std::cout << "Starting CUDA raw lane conformance and timing harness...\n";

    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    std::ofstream md("tuning_results.md", std::ios_base::app);
    if (!md.is_open()) {
        std::cerr << "Failed to open tuning_results.md for writing\n";
        return EXIT_FAILURE;
    }
    writeMarkdownHeader(md, "Phase 5A CUDA Raw Lane Conformance Results");

    if (err != cudaSuccess || deviceCount == 0) {
        writeMarkdownRow(md, RunRecord{
            "CUDA",
            "none",
            compilerDescription(),
            "all",
            "all",
            0,
            "n/a",
            "n/a",
            0,
            "not available",
            "skipped: no CUDA device",
        });
        std::cout << "No CUDA device found; CUDA conformance skipped.\n";
        return EXIT_SUCCESS;
    }

    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    const std::string deviceName = prop.name;
    std::cout << "Running on: " << deviceName << "\n";

    const std::vector<std::size_t> counts = {0, 1, 127, 256, 1009, 65536};
    const std::vector<int> blockSizes = {64, 256};

    for (const LaneCase& laneCase : laneCases()) {
        for (Operation op : operations()) {
            for (std::size_t count : counts) {
                for (int blockSize : blockSizes) {
                    std::cout << laneCase.name << " " << operationName(op)
                              << " count=" << count
                              << " block=" << blockSize << "\n";
                    if (laneCase.raw128) {
                        runRaw128Case(md, deviceName, laneCase, op, count, blockSize);
                    } else {
                        runRaw64Case(md, deviceName, laneCase, op, count, blockSize);
                    }
                }
            }
        }
    }

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " CUDA raw lane failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nCUDA raw lane conformance passed\n";
    return EXIT_SUCCESS;
}
