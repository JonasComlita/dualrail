#define TERNARY_ENABLE_SYCL
#include "ternary_gpu_kernels.h"
#include "ternary_gpu_validation.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sycl/sycl.hpp>

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
T* syclAlloc(sycl::queue& queue, std::size_t count) {
    const std::size_t allocCount = std::max<std::size_t>(count, 1);
    return sycl::malloc_shared<T>(allocCount, queue);
}

template<typename T>
void copyToDevice(T* dst, const std::vector<T>& src) {
    for (std::size_t i = 0; i < src.size(); ++i) dst[i] = src[i];
}

template<typename T>
void copyFromDevice(std::vector<T>& dst, const T* src) {
    for (std::size_t i = 0; i < dst.size(); ++i) dst[i] = src[i];
}

sycl::event launch64(
    sycl::queue& queue,
    Operation op,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int localSize) {

    switch (op) {
        case Operation::Neg: return syclLaunchTritwiseNegRaw64(queue, a, out, count, trits, localSize);
        case Operation::Add: return syclLaunchTritwiseAddRaw64(queue, a, b, out, count, trits, localSize);
        case Operation::Sub: return syclLaunchTritwiseSubRaw64(queue, a, b, out, count, trits, localSize);
        case Operation::Compare: return syclLaunchTritwiseCompareRaw64(queue, a, b, cmp, count, trits, localSize);
        case Operation::Min: return syclLaunchTritwiseMinRaw64(queue, a, b, out, count, trits, localSize);
        case Operation::Max: return syclLaunchTritwiseMaxRaw64(queue, a, b, out, count, trits, localSize);
    }
    return syclNoop(queue);
}

sycl::event launch128(
    sycl::queue& queue,
    Operation op,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int localSize) {

    switch (op) {
        case Operation::Neg: return syclLaunchTritwiseNegRaw128(queue, a, out, count, trits, localSize);
        case Operation::Add: return syclLaunchTritwiseAddRaw128(queue, a, b, out, count, trits, localSize);
        case Operation::Sub: return syclLaunchTritwiseSubRaw128(queue, a, b, out, count, trits, localSize);
        case Operation::Compare: return syclLaunchTritwiseCompareRaw128(queue, a, b, cmp, count, trits, localSize);
        case Operation::Min: return syclLaunchTritwiseMinRaw128(queue, a, b, out, count, trits, localSize);
        case Operation::Max: return syclLaunchTritwiseMaxRaw128(queue, a, b, out, count, trits, localSize);
    }
    return syclNoop(queue);
}

long long timeLaunch64(
    sycl::queue& queue,
    Operation op,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int localSize) {

    launch64(queue, op, a, b, out, cmp, count, trits, localSize).wait();
    const auto start = std::chrono::high_resolution_clock::now();
    launch64(queue, op, a, b, out, cmp, count, trits, localSize).wait();
    const auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

long long timeLaunch128(
    sycl::queue& queue,
    Operation op,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    uint8_t* cmp,
    std::size_t count,
    int trits,
    int localSize) {

    launch128(queue, op, a, b, out, cmp, count, trits, localSize).wait();
    const auto start = std::chrono::high_resolution_clock::now();
    launch128(queue, op, a, b, out, cmp, count, trits, localSize).wait();
    const auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

std::string executionKind(const sycl::device& device) {
    if (device.is_gpu()) return "real GPU";
    if (device.is_cpu()) return "CPU fallback";
    return "accelerator/unknown";
}

std::string localSizeLabel(int localSize) {
    return localSize == 0 ? "auto" : std::to_string(localSize);
}

void runRaw64Case(
    sycl::queue& queue,
    std::ofstream& md,
    const std::string& deviceName,
    const std::string& execution,
    const LaneCase& laneCase,
    Operation op,
    std::size_t count,
    int localSize) {

    std::vector<uint64_t> hostA(count);
    std::vector<uint64_t> hostB(count);
    std::vector<uint64_t> hostOut(count);
    std::vector<uint8_t> hostCmp(count);
    fillRaw64Inputs(hostA, hostB, laneCase.trits);

    std::vector<uint64_t> expected = expectedRaw64(op, hostA, hostB, laneCase.trits);
    std::vector<uint8_t> expectedCmp = expectedCompare64(hostA, hostB, laneCase.trits);

    uint64_t* devA = syclAlloc<uint64_t>(queue, count);
    uint64_t* devB = syclAlloc<uint64_t>(queue, count);
    uint64_t* devOut = syclAlloc<uint64_t>(queue, count);
    uint8_t* devCmp = syclAlloc<uint8_t>(queue, count);
    expect(devA && devB && devOut && devCmp, "SYCL shared allocation raw64");
    if (!devA || !devB || !devOut || !devCmp) return;

    copyToDevice(devA, hostA);
    copyToDevice(devB, hostB);
    const long long runtimeUs = timeLaunch64(queue, op, devA, devB, devOut, devCmp, count, laneCase.trits, localSize);
    queue.wait();

    copyFromDevice(hostOut, devOut);
    copyFromDevice(hostCmp, devCmp);

    std::string message;
    const bool ok = verifyRaw64(op, hostOut, hostCmp, expected, expectedCmp, message);
    expect(ok, std::string(laneCase.name) + " " + operationName(op) + " raw64 " + message);

    writeMarkdownRow(md, RunRecord{
        "SYCL",
        deviceName,
        compilerDescription(),
        operationName(op),
        laneCase.name,
        count,
        localSizeLabel(localSize),
        hex64(checksumRaw64(op, hostOut, hostCmp)),
        runtimeUs,
        execution,
        ok ? "pass" : message,
    });

    sycl::free(devA, queue);
    sycl::free(devB, queue);
    sycl::free(devOut, queue);
    sycl::free(devCmp, queue);
}

void runRaw128Case(
    sycl::queue& queue,
    std::ofstream& md,
    const std::string& deviceName,
    const std::string& execution,
    const LaneCase& laneCase,
    Operation op,
    std::size_t count,
    int localSize) {

    std::vector<RawUInt128> hostA(count);
    std::vector<RawUInt128> hostB(count);
    std::vector<RawUInt128> hostOut(count);
    std::vector<uint8_t> hostCmp(count);
    fillRaw128Inputs(hostA, hostB, laneCase.trits);

    std::vector<RawUInt128> expected = expectedRaw128(op, hostA, hostB, laneCase.trits);
    std::vector<uint8_t> expectedCmp = expectedCompare128(hostA, hostB, laneCase.trits);

    RawUInt128* devA = syclAlloc<RawUInt128>(queue, count);
    RawUInt128* devB = syclAlloc<RawUInt128>(queue, count);
    RawUInt128* devOut = syclAlloc<RawUInt128>(queue, count);
    uint8_t* devCmp = syclAlloc<uint8_t>(queue, count);
    expect(devA && devB && devOut && devCmp, "SYCL shared allocation raw128");
    if (!devA || !devB || !devOut || !devCmp) return;

    copyToDevice(devA, hostA);
    copyToDevice(devB, hostB);
    const long long runtimeUs = timeLaunch128(queue, op, devA, devB, devOut, devCmp, count, laneCase.trits, localSize);
    queue.wait();

    copyFromDevice(hostOut, devOut);
    copyFromDevice(hostCmp, devCmp);

    std::string message;
    const bool ok = verifyRaw128(op, hostOut, hostCmp, expected, expectedCmp, message);
    expect(ok, std::string(laneCase.name) + " " + operationName(op) + " raw128 " + message);

    writeMarkdownRow(md, RunRecord{
        "SYCL",
        deviceName,
        compilerDescription(),
        operationName(op),
        laneCase.name,
        count,
        localSizeLabel(localSize),
        hex64(checksumRaw128(op, hostOut, hostCmp)),
        runtimeUs,
        execution,
        ok ? "pass" : message,
    });

    sycl::free(devA, queue);
    sycl::free(devB, queue);
    sycl::free(devOut, queue);
    sycl::free(devCmp, queue);
}

} // namespace

int main() {
    std::cout << "Starting SYCL raw lane conformance and timing harness...\n";

    std::ofstream md("tuning_results.md", std::ios_base::app);
    if (!md.is_open()) {
        std::cerr << "Failed to open tuning_results.md for writing\n";
        return EXIT_FAILURE;
    }
    writeMarkdownHeader(md, "Phase 5A SYCL Raw Lane Conformance Results");

    try {
        sycl::queue queue(sycl::default_selector_v);
        const sycl::device device = queue.get_device();
        const std::string deviceName = device.get_info<sycl::info::device::name>();
        const std::string execution = executionKind(device);

        std::cout << "Running on: " << deviceName << " (" << execution << ")\n";

        const std::vector<std::size_t> counts = {0, 1, 127, 256, 1009, 65536};
        const std::vector<int> localSizes = {0, 64, 256};

        for (const LaneCase& laneCase : laneCases()) {
            for (Operation op : operations()) {
                for (std::size_t count : counts) {
                    for (int localSize : localSizes) {
                        std::cout << laneCase.name << " " << operationName(op)
                                  << " count=" << count
                                  << " local=" << localSizeLabel(localSize) << "\n";
                        if (laneCase.raw128) {
                            runRaw128Case(queue, md, deviceName, execution, laneCase, op, count, localSize);
                        } else {
                            runRaw64Case(queue, md, deviceName, execution, laneCase, op, count, localSize);
                        }
                    }
                }
            }
        }
    } catch (const sycl::exception& e) {
        writeMarkdownRow(md, RunRecord{
            "SYCL",
            "none",
            compilerDescription(),
            "all",
            "all",
            0,
            "n/a",
            "n/a",
            0,
            "not available",
            std::string("skipped: ") + e.what(),
        });
        std::cerr << "SYCL exception: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " SYCL raw lane failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nSYCL raw lane conformance passed\n";
    return EXIT_SUCCESS;
}
