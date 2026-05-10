// Baseline benchmark harness.
//
// Build:
//   g++ -std=c++20 -O3 -DNDEBUG benchmark.cpp -o benchmark.exe
//
// Run:
//   ./benchmark.exe          // default scale = 1
//   ./benchmark.exe 5        // 5x longer run for steadier numbers

#include "ternary_asm.h"
#include "ternary_kernel.h"
#include "ternary_lanes.h"
#include "ternary_simd.h"
#include "ternary_vm.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile uint64_t g_sink = 0;

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

uint64_t payload(sandbox::T1 value) { return value.data; }
uint64_t payload(sandbox::T5 value) { return value.data; }
uint64_t payload(sandbox::T10 value) { return value.data; }
uint64_t payload(sandbox::T20 value) { return value.data; }
uint64_t payload(sandbox::Triple value) { return value.data; }
uint64_t payload(sandbox::LongTriple value) {
    return value.data.lo ^ (value.data.hi * 0x9E3779B97F4A7C15ULL);
}

uint64_t payload(sandbox::UInt128 value) {
    return value.lo ^ (value.hi * 0xD6E8FEB86659FD93ULL);
}

uint64_t payload(sandbox::backend::RawUInt128 value) {
    return value.lo ^ (value.hi * 0xD6E8FEB86659FD93ULL);
}

template<int Trits, typename Storage>
uint64_t payload(sandbox::TritLane<Trits, Storage> lane) {
    return payload(lane.rawForKernel());
}

template<int Trits>
uint64_t payload(sandbox::TritLane<Trits, uint8_t> lane) {
    return lane.rawForKernel();
}

template<int Trits>
uint64_t payload(sandbox::TritLane<Trits, uint16_t> lane) {
    return lane.rawForKernel();
}

template<int Trits>
uint64_t payload(sandbox::TritLane<Trits, uint32_t> lane) {
    return lane.rawForKernel();
}

template<int Trits>
uint64_t payload(sandbox::TritLane<Trits, uint64_t> lane) {
    return lane.rawForKernel();
}

struct Result {
    std::string group;
    std::string name;
    std::string unit;
    uint64_t units = 0;
    double seconds = 0.0;
    uint64_t checksum = 0;
};

template<typename F>
Result measure(const std::string& group,
               const std::string& name,
               const std::string& unit,
               uint64_t units,
               F&& fn) {
    auto start = Clock::now();
    uint64_t checksum = fn();
    auto stop = Clock::now();
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(stop - start).count();
    g_sink ^= checksum;
    return Result{group, name, unit, units, seconds, checksum};
}

void printResult(const Result& r) {
    const double nsPerUnit = r.units == 0 ? 0.0 : (r.seconds * 1.0e9) / r.units;
    const double unitsPerSec = r.seconds == 0.0 ? 0.0 : r.units / r.seconds;

    std::cout << std::left << std::setw(13) << r.group
              << std::setw(30) << r.name
              << std::right << std::setw(14) << r.units
              << "  " << std::setw(8) << std::fixed << std::setprecision(3)
              << (r.seconds * 1000.0)
              << "  " << std::setw(10) << std::fixed << std::setprecision(2)
              << nsPerUnit
              << "  " << std::setw(13) << std::fixed << std::setprecision(0)
              << unitsPerSec
              << "  " << r.unit
              << "  0x" << std::hex << r.checksum << std::dec << "\n";
}

template<typename T, typename Op>
uint64_t benchBinary(const std::vector<T>& a,
                     const std::vector<T>& b,
                     uint64_t iterations,
                     Op&& op) {
    uint64_t h = 0;
    const uint64_t mask = static_cast<uint64_t>(a.size() - 1);
    for (uint64_t i = 0; i < iterations; ++i) {
        T out = op(a[i & mask], b[(i * 7U) & mask]);
        h = mix(h, payload(out));
    }
    return h;
}

template<typename T, typename Op>
uint64_t benchUnary(const std::vector<T>& values,
                    uint64_t iterations,
                    Op&& op) {
    uint64_t h = 0;
    const uint64_t mask = static_cast<uint64_t>(values.size() - 1);
    for (uint64_t i = 0; i < iterations; ++i) {
        T out = op(values[i & mask]);
        h = mix(h, payload(out));
    }
    return h;
}

template<typename T, typename FromInt>
std::vector<T> makeCorpus(FromInt&& fromInt, int low, int high, bool nonzero = false) {
    std::vector<T> values;
    for (int n = low; n <= high; ++n) {
        if (nonzero && n == 0) continue;
        values.push_back(fromInt(n));
    }
    while ((values.size() & (values.size() - 1)) != 0) {
        values.push_back(values.back());
    }
    return values;
}

uint64_t benchVmLoop(uint64_t repeats, uint64_t& totalSteps) {
    using namespace sandbox;
    using namespace sandbox::vm;

    const std::string source = R"(
        mov.t5   r1, 100
        mov.t5   r2, 1
        mov.t5   r3, 0
    loop:
        sub.t5   r1, r1, r2
        tcmp.t5  r4, r1, r3
        brn      r4, done
        jmp      loop
    done:
        halt
    )";

    const auto program = assembler::assembleOrThrow(source);
    VMState vm(64, 64);
    if (!loadAndReset(vm, program)) return 0;

    uint64_t h = 0;
    totalSteps = 0;
    for (uint64_t i = 0; i < repeats; ++i) {
        vm.reset();
        const RunResult result = run(vm, 10000);
        totalSteps += static_cast<uint64_t>(result.steps);
        h = mix(h, static_cast<uint64_t>(result.steps));
        h = mix(h, static_cast<uint64_t>(vm::ops::toLong(vm.regfile.read(isa::R1))));
    }
    return h;
}


void addUInt128Benches(std::vector<Result>& results, int scale) {
    using namespace sandbox;

    const uint64_t iters = 4'000'000ULL * scale;
    const uint64_t wideIters = 400'000ULL * scale;
    auto mk = [](int n) { return UInt128{static_cast<uint64_t>(n * 1337), static_cast<uint64_t>(n * 42)}; };
    auto corpusA = makeCorpus<UInt128>(mk, 1, 16);
    auto corpusB = makeCorpus<UInt128>(mk, 1, 16);

    std::vector<std::array<int8_t, 50>> tritCorpus;
    std::vector<LongTriple> longCorpus;
    for (int n = 0; n < 16; ++n) {
        std::array<int8_t, 50> trits{};
        for (int i = 0; i < 50; ++i) {
            trits[i] = static_cast<int8_t>(((i + n) % 3) - 1);
        }
        tritCorpus.push_back(trits);
        longCorpus.push_back(LongTriple::pack(trits));
    }

    auto addOp = [](UInt128 a, UInt128 b) { return a + b; };
    auto subOp = [](UInt128 a, UInt128 b) { return a - b; };
    auto mulOp = [](UInt128 a, UInt128 b) { return a * b; };
    auto divOp = [](UInt128 a, UInt128 b) { return a / b; };
    auto modOp = [](UInt128 a, UInt128 b) { return a % b; };
    auto divSmallOp = [](UInt128 a) { return a.divSmall(3); };
    auto modSmallOp = [](UInt128 a) { return UInt128{a.modSmall(3), 0}; };
    auto divModSmallOp = [](UInt128 a) { uint32_t r; return a.divModSmall(3, r); };

    results.push_back(measure("uint128", "add", "ops/s", iters,
        [&] { return benchBinary(corpusA, corpusB, iters, addOp); }));
    results.push_back(measure("uint128", "sub", "ops/s", iters,
        [&] { return benchBinary(corpusA, corpusB, iters, subOp); }));
    results.push_back(measure("uint128", "mul", "ops/s", iters,
        [&] { return benchBinary(corpusA, corpusB, iters, mulOp); }));
    results.push_back(measure("uint128", "div", "ops/s", iters,
        [&] { return benchBinary(corpusA, corpusB, iters, divOp); }));
    results.push_back(measure("uint128", "mod", "ops/s", iters,
        [&] { return benchBinary(corpusA, corpusB, iters, modOp); }));
    results.push_back(measure("uint128", "divSmall", "ops/s", iters,
        [&] { return benchUnary(corpusA, iters, divSmallOp); }));
    results.push_back(measure("uint128", "modSmall", "ops/s", iters,
        [&] { return benchUnary(corpusA, iters, modSmallOp); }));
    results.push_back(measure("uint128", "divModSmall", "ops/s", iters,
        [&] { return benchUnary(corpusA, iters, divModSmallOp); }));
    results.push_back(measure("uint128", "pow3UInt128", "ops/s", iters,
        [&] {
            uint64_t h = 0;
            for (uint64_t i = 0; i < iters; ++i) {
                h = mix(h, payload(native_ops::detail::pow3UInt128(static_cast<int>(i % 50))));
            }
            return h;
        }));
    results.push_back(measure("uint128", "LongTriple pack", "ops/s", wideIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(tritCorpus.size() - 1);
            for (uint64_t i = 0; i < wideIters; ++i) {
                h = mix(h, payload(LongTriple::pack(tritCorpus[i & mask])));
            }
            return h;
        }));
    results.push_back(measure("uint128", "LongTriple unpack", "ops/s", wideIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(longCorpus.size() - 1);
            for (uint64_t i = 0; i < wideIters; ++i) {
                const auto trits = longCorpus[i & mask].unpack();
                uint64_t packed = 0;
                for (int t = 0; t < 16; ++t) {
                    packed = (packed << 2) | static_cast<uint64_t>(trits[t] + 1);
                }
                h = mix(h, packed);
            }
            return h;
        }));
}

void addNativeBenches(std::vector<Result>& results, int scale) {
    using namespace sandbox;
    using namespace sandbox::native_ops;

    auto addOp = [](auto x, auto y) { return sandbox::native_ops::add(x, y); };
    auto mulOp = [](auto x, auto y) { return sandbox::native_ops::multiply(x, y); };
    auto divOp = [](auto x, auto y) { return sandbox::native_ops::divide(x, y); };
    auto sqrtOp = [](auto x) { return sandbox::native_ops::sqrt(x); };

    const uint64_t intIters = 4'000'000ULL * scale;
    const uint64_t floatIters = 300'000ULL * scale;
    const uint64_t sqrtIters = 60'000ULL * scale;

    auto t1 = makeCorpus<T1>(fromIntT1, -1, 1);
    auto t5 = makeCorpus<T5>(fromIntT5, -16, 16);
    auto t5nz = makeCorpus<T5>(fromIntT5, -16, 16, true);
    auto t10 = makeCorpus<T10>(fromIntT10, -8, 8);
    auto t10nz = makeCorpus<T10>(fromIntT10, -8, 8, true);
    auto t20 = makeCorpus<T20>(fromIntT20, -8, 8);
    auto t20nz = makeCorpus<T20>(fromIntT20, -8, 8, true);
    auto t40 = makeCorpus<Triple>(fromIntT40, -8, 8);
    auto t40nz = makeCorpus<Triple>(fromIntT40, -8, 8, true);
    auto t50 = makeCorpus<LongTriple>(fromInt, -8, 8);
    auto t50nz = makeCorpus<LongTriple>(fromInt, -8, 8, true);
    auto sqrt10 = makeCorpus<T10>(fromIntT10, 1, 16, true);
    auto sqrt20 = makeCorpus<T20>(fromIntT20, 1, 16, true);
    auto sqrt40 = makeCorpus<Triple>(fromIntT40, 1, 16, true);
    auto sqrt50 = makeCorpus<LongTriple>(fromInt, 1, 16, true);

    results.push_back(measure("native-int", "T1 add", "ops/s", intIters,
        [&] { return benchBinary(t1, t1, intIters, addOp); }));
    results.push_back(measure("native-int", "T5 add", "ops/s", intIters,
        [&] { return benchBinary(t5, t5, intIters, addOp); }));
    results.push_back(measure("native-int", "T5 multiply", "ops/s", intIters,
        [&] { return benchBinary(t5, t5, intIters, mulOp); }));
    results.push_back(measure("native-int", "T5 divide", "ops/s", intIters,
        [&] { return benchBinary(t5, t5nz, intIters, divOp); }));

    results.push_back(measure("native-fp", "T10 add", "ops/s", floatIters,
        [&] { return benchBinary(t10, t10, floatIters, addOp); }));
    results.push_back(measure("native-fp", "T20 add", "ops/s", floatIters,
        [&] { return benchBinary(t20, t20, floatIters, addOp); }));
    results.push_back(measure("native-fp", "T40 add", "ops/s", floatIters,
        [&] { return benchBinary(t40, t40, floatIters, addOp); }));
    results.push_back(measure("native-fp", "T50 add", "ops/s", floatIters,
        [&] { return benchBinary(t50, t50, floatIters, addOp); }));

    results.push_back(measure("native-fp", "T10 multiply", "ops/s", floatIters,
        [&] { return benchBinary(t10, t10, floatIters, mulOp); }));
    results.push_back(measure("native-fp", "T20 multiply", "ops/s", floatIters,
        [&] { return benchBinary(t20, t20, floatIters, mulOp); }));
    results.push_back(measure("native-fp", "T40 multiply", "ops/s", floatIters,
        [&] { return benchBinary(t40, t40, floatIters, mulOp); }));
    results.push_back(measure("native-fp", "T50 multiply", "ops/s", floatIters,
        [&] { return benchBinary(t50, t50, floatIters, mulOp); }));

    results.push_back(measure("native-fp", "T10 divide", "ops/s", floatIters,
        [&] { return benchBinary(t10, t10nz, floatIters, divOp); }));
    results.push_back(measure("native-fp", "T20 divide", "ops/s", floatIters,
        [&] { return benchBinary(t20, t20nz, floatIters, divOp); }));
    results.push_back(measure("native-fp", "T40 divide", "ops/s", floatIters,
        [&] { return benchBinary(t40, t40nz, floatIters, divOp); }));
    results.push_back(measure("native-fp", "T50 divide", "ops/s", floatIters,
        [&] { return benchBinary(t50, t50nz, floatIters, divOp); }));

    results.push_back(measure("native-fp", "T10 sqrt", "ops/s", sqrtIters,
        [&] { return benchUnary(sqrt10, sqrtIters, sqrtOp); }));
    results.push_back(measure("native-fp", "T20 sqrt", "ops/s", sqrtIters,
        [&] { return benchUnary(sqrt20, sqrtIters, sqrtOp); }));
    results.push_back(measure("native-fp", "T40 sqrt", "ops/s", sqrtIters,
        [&] { return benchUnary(sqrt40, sqrtIters, sqrtOp); }));
    results.push_back(measure("native-fp", "T50 sqrt", "ops/s", sqrtIters,
        [&] { return benchUnary(sqrt50, sqrtIters, sqrtOp); }));
}

void addLaneBenches(std::vector<Result>& results, int scale) {
    using namespace sandbox;
    using namespace sandbox::native_ops;

    const uint64_t laneIters = 2'000'000ULL * scale;
    const uint64_t wideLaneIters = 400'000ULL * scale;

    auto t1 = makeCorpus<T1>(fromIntT1, -1, 1);
    auto t5 = makeCorpus<T5>(fromIntT5, -16, 16);
    auto t10 = makeCorpus<T10>(fromIntT10, -8, 8);
    auto t20 = makeCorpus<T20>(fromIntT20, -8, 8);
    auto t40 = makeCorpus<Triple>(fromIntT40, -8, 8);
    auto t50 = makeCorpus<LongTriple>(fromInt, -8, 8);

    std::vector<TritLane1> lane1;
    std::vector<TritLane5> lane5;
    std::vector<TritLane10> lane10;
    std::vector<TritLane20> lane20;
    std::vector<TritLane40> lane40;
    std::vector<TritLane50> lane50;
    for (auto v : t1) lane1.push_back(toLane(v));
    for (auto v : t5) lane5.push_back(toLane(v));
    for (auto v : t10) lane10.push_back(toLane(v));
    for (auto v : t20) lane20.push_back(toLane(v));
    for (auto v : t40) lane40.push_back(toLane(v));
    for (auto v : t50) lane50.push_back(toLane(v));

    results.push_back(measure("lane-conv", "T1 roundtrip", "roundtrips/s", laneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t1.size() - 1);
            for (uint64_t i = 0; i < laneIters; ++i) h = mix(h, payload(fromLane(toLane(t1[i & mask]))));
            return h;
        }));
    results.push_back(measure("lane-conv", "T5 roundtrip", "roundtrips/s", laneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t5.size() - 1);
            for (uint64_t i = 0; i < laneIters; ++i) h = mix(h, payload(fromLane(toLane(t5[i & mask]))));
            return h;
        }));
    results.push_back(measure("lane-conv", "T10 roundtrip", "roundtrips/s", laneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t10.size() - 1);
            for (uint64_t i = 0; i < laneIters; ++i) h = mix(h, payload(fromLane(toLane(t10[i & mask]))));
            return h;
        }));
    results.push_back(measure("lane-conv", "T20 roundtrip", "roundtrips/s", laneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t20.size() - 1);
            for (uint64_t i = 0; i < laneIters; ++i) h = mix(h, payload(fromLane(toLane(t20[i & mask]))));
            return h;
        }));
    results.push_back(measure("lane-conv", "T40 roundtrip", "roundtrips/s", wideLaneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t40.size() - 1);
            for (uint64_t i = 0; i < wideLaneIters; ++i) h = mix(h, payload(fromLane(toLane(t40[i & mask]))));
            return h;
        }));
    results.push_back(measure("lane-conv", "T50 roundtrip", "roundtrips/s", wideLaneIters,
        [&] {
            uint64_t h = 0;
            const uint64_t mask = static_cast<uint64_t>(t50.size() - 1);
            for (uint64_t i = 0; i < wideLaneIters; ++i) h = mix(h, payload(fromLane(toLane(t50[i & mask]))));
            return h;
        }));

    results.push_back(measure("lane-op", "T1 tritwiseAdd", "ops/s", laneIters,
        [&] { return benchBinary(lane1, lane1, laneIters, tritwiseAdd<1, uint8_t>); }));
    results.push_back(measure("lane-op", "T5 tritwiseAdd", "ops/s", laneIters,
        [&] { return benchBinary(lane5, lane5, laneIters, tritwiseAdd<5, uint16_t>); }));
    results.push_back(measure("lane-op", "T20 tritwiseAdd", "ops/s", laneIters,
        [&] { return benchBinary(lane20, lane20, laneIters, tritwiseAdd<20, uint64_t>); }));
    results.push_back(measure("lane-op", "T40 tritwiseAdd", "ops/s", wideLaneIters,
        [&] { return benchBinary(lane40, lane40, wideLaneIters, tritwiseAdd<40, UInt128>); }));
    results.push_back(measure("lane-op", "T50 tritwiseAdd", "ops/s", wideLaneIters,
        [&] { return benchBinary(lane50, lane50, wideLaneIters, tritwiseAdd<50, UInt128>); }));
}

void addSimdBenches(std::vector<Result>& results, int scale) {
    using namespace sandbox;
    using namespace sandbox::native_ops;

    struct BatchSize {
        const char* name;
        std::size_t lanes;
        uint64_t repeats;
    };

    const BatchSize sizes[] = {
        {"small", 64, 50000ULL * static_cast<uint64_t>(scale)},
        {"hot", 4096, 2000ULL * static_cast<uint64_t>(scale)},
        {"large", 65536, 128ULL * static_cast<uint64_t>(scale)},
    };

    auto autoName = [](const char* op, bool hasAvx2Path, const char* sizeName) {
        const char* backend = hasAvx2Path ? sandbox::simd::backendName(sandbox::simd::BatchBackend::Auto)
                                          : "scalar";
        return std::string("T20 ") + op + " auto(" + backend + ") " + sizeName;
    };

    auto scalarName = [](const char* op, const char* sizeName) {
        return std::string("T20 ") + op + " scalar " + sizeName;
    };

    for (const BatchSize& size : sizes) {
        std::vector<TritLane20> a;
        std::vector<TritLane20> b;
        std::vector<TritLane20> out;
        std::vector<TritLane1> cmpOut;
        a.reserve(size.lanes);
        b.reserve(size.lanes);
        out.resize(size.lanes);
        cmpOut.resize(size.lanes);

        for (std::size_t i = 0; i < size.lanes; ++i) {
            a.push_back(toLane(fromIntT20(static_cast<int>((i % 37) - 18))));
            b.push_back(toLane(fromIntT20(static_cast<int>(((i * 5) % 29) - 14))));
        }

        const uint64_t ops = size.repeats * static_cast<uint64_t>(size.lanes);
        const std::size_t mask = size.lanes - 1;

        auto benchLaneOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < size.repeats; ++r) {
                fn();
                h = mix(h, payload(out[(r * 17U) & mask]));
            }
            return h;
        };

        auto benchCmpOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < size.repeats; ++r) {
                fn();
                h = mix(h, payload(cmpOut[(r * 17U) & mask]));
            }
            return h;
        };

        results.push_back(measure("simd", scalarName("neg", size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseNegScalar(a.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd", autoName("neg", true, size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseNeg(a.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd", scalarName("add", size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseAddScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd", autoName("add", true, size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseAdd(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd", scalarName("sub", size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseSubScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd", autoName("sub", true, size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseSub(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd", scalarName("compare", size.name), "ops/s", ops,
            [&] { return benchCmpOut([&] { sandbox::simd::batchTritwiseCompareScalar(a.data(), b.data(), cmpOut.data(), cmpOut.size()); }); }));
        results.push_back(measure("simd", autoName("compare", true, size.name), "ops/s", ops,
            [&] { return benchCmpOut([&] { sandbox::simd::batchTritwiseCompare(a.data(), b.data(), cmpOut.data(), cmpOut.size()); }); }));

        results.push_back(measure("simd", scalarName("min", size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMinScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd", autoName("min", false, size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMin(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd", scalarName("max", size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMaxScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd", autoName("max", false, size.name), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMax(a.data(), b.data(), out.data(), out.size()); }); }));
    }

    const std::size_t lanes = 4096;
    const uint64_t repeats = 1000ULL * static_cast<uint64_t>(scale);
    const uint64_t ops = repeats * lanes;

    auto addWidthBench = [&](const char* name, auto maker) {
        using Lane = decltype(maker(0));
        std::vector<Lane> a;
        std::vector<Lane> b;
        std::vector<Lane> out(lanes);
        std::vector<TritLane1> cmpOut(lanes);
        a.reserve(lanes);
        b.reserve(lanes);
        for (std::size_t i = 0; i < lanes; ++i) {
            a.push_back(maker(static_cast<int>((i % 19) - 9)));
            b.push_back(maker(static_cast<int>(((i * 5) % 17) - 8)));
        }

        auto benchLaneOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, payload(out[(r * 17U) & (lanes - 1)]));
            }
            return h;
        };

        auto benchCmpOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, payload(cmpOut[(r * 17U) & (lanes - 1)]));
            }
            return h;
        };

        auto scalarLabel = [&](const char* op) {
            return std::string(name) + " " + op + " scalar";
        };

        auto autoLabel = [&](const char* op) {
            const bool t20Avx2 =
                std::string(name) == "TritLane20" &&
                (std::string(op) == "neg" ||
                 std::string(op) == "add" ||
                 std::string(op) == "sub" ||
                 std::string(op) == "compare");
            const char* backend = t20Avx2
                ? sandbox::simd::backendName(sandbox::simd::BatchBackend::Auto)
                : "scalar";
            return std::string(name) + " " + op + " auto(" + backend + ")";
        };

        results.push_back(measure("simd-width", scalarLabel("neg"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseNegScalar(a.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("neg"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseNeg(a.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd-width", scalarLabel("add"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseAddScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("add"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseAdd(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd-width", scalarLabel("sub"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseSubScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("sub"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseSub(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd-width", scalarLabel("compare"), "ops/s", ops,
            [&] { return benchCmpOut([&] { sandbox::simd::batchTritwiseCompareScalar(a.data(), b.data(), cmpOut.data(), cmpOut.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("compare"), "ops/s", ops,
            [&] { return benchCmpOut([&] { sandbox::simd::batchTritwiseCompare(a.data(), b.data(), cmpOut.data(), cmpOut.size()); }); }));

        results.push_back(measure("simd-width", scalarLabel("min"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMinScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("min"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMin(a.data(), b.data(), out.data(), out.size()); }); }));

        results.push_back(measure("simd-width", scalarLabel("max"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMaxScalar(a.data(), b.data(), out.data(), out.size()); }); }));
        results.push_back(measure("simd-width", autoLabel("max"), "ops/s", ops,
            [&] { return benchLaneOut([&] { sandbox::simd::batchTritwiseMax(a.data(), b.data(), out.data(), out.size()); }); }));
    };

    addWidthBench("TritLane1", [](int n) { return toLane(fromIntT1(n < 0 ? -1 : (n > 0 ? 1 : 0))); });
    addWidthBench("TritLane5", [](int n) { return toLane(fromIntT5(n)); });
    addWidthBench("TritLane10", [](int n) { return toLane(fromIntT10(n)); });
    addWidthBench("TritLane20", [](int n) { return toLane(fromIntT20(n)); });
    addWidthBench("TritLane40", [](int n) { return toLane(fromIntT40(n)); });
    addWidthBench("TritLane50", [](int n) { return toLane(fromInt(n)); });
}

void addKernelRawBenches(std::vector<Result>& results, int scale) {
    using namespace sandbox;
    using namespace sandbox::native_ops;

    const std::size_t lanes = 4096;
    const uint64_t repeats = 500ULL * static_cast<uint64_t>(scale);
    const uint64_t ops = repeats * lanes;

    auto addRaw64Width = [&](const char* name, int trits, auto maker) {
        std::vector<uint64_t> a;
        std::vector<uint64_t> b;
        std::vector<uint64_t> out(lanes);
        std::vector<uint8_t> cmpOut(lanes);
        a.reserve(lanes);
        b.reserve(lanes);
        for (std::size_t i = 0; i < lanes; ++i) {
            a.push_back(static_cast<uint64_t>(maker(static_cast<int>((i % 19) - 9)).rawForKernel()));
            b.push_back(static_cast<uint64_t>(maker(static_cast<int>(((i * 5) % 17) - 8)).rawForKernel()));
        }

        auto label = [&](const char* op) {
            return std::string(name) + " " + op + " raw";
        };

        auto benchLaneOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, out[(r * 17U) & (lanes - 1)]);
            }
            return h;
        };

        auto benchCmpOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, cmpOut[(r * 17U) & (lanes - 1)]);
            }
            return h;
        };

        results.push_back(measure("kernel-raw", label("neg"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseNegRaw64(a.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("add"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseAddRaw64(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("sub"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseSubRaw64(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("compare"), "ops/s", ops,
            [&] { return benchCmpOut([&] { backend::kernel::batchTritwiseCompareRaw64(a.data(), b.data(), cmpOut.data(), cmpOut.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("min"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseMinRaw64(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("max"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseMaxRaw64(a.data(), b.data(), out.data(), out.size(), trits); }); }));
    };

    auto addRaw128Width = [&](const char* name, int trits, auto maker) {
        std::vector<backend::RawUInt128> a;
        std::vector<backend::RawUInt128> b;
        std::vector<backend::RawUInt128> out(lanes);
        std::vector<uint8_t> cmpOut(lanes);
        a.reserve(lanes);
        b.reserve(lanes);
        for (std::size_t i = 0; i < lanes; ++i) {
            const UInt128 ar = maker(static_cast<int>((i % 19) - 9)).rawForKernel();
            const UInt128 br = maker(static_cast<int>(((i * 5) % 17) - 8)).rawForKernel();
            a.push_back(backend::makeRawUInt128(ar.hi, ar.lo));
            b.push_back(backend::makeRawUInt128(br.hi, br.lo));
        }

        auto label = [&](const char* op) {
            return std::string(name) + " " + op + " raw";
        };

        auto benchLaneOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, payload(out[(r * 17U) & (lanes - 1)]));
            }
            return h;
        };

        auto benchCmpOut = [&](auto&& fn) {
            uint64_t h = 0;
            for (uint64_t r = 0; r < repeats; ++r) {
                fn();
                h = mix(h, cmpOut[(r * 17U) & (lanes - 1)]);
            }
            return h;
        };

        results.push_back(measure("kernel-raw", label("neg"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseNegRaw128(a.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("add"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseAddRaw128(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("sub"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseSubRaw128(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("compare"), "ops/s", ops,
            [&] { return benchCmpOut([&] { backend::kernel::batchTritwiseCompareRaw128(a.data(), b.data(), cmpOut.data(), cmpOut.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("min"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseMinRaw128(a.data(), b.data(), out.data(), out.size(), trits); }); }));
        results.push_back(measure("kernel-raw", label("max"), "ops/s", ops,
            [&] { return benchLaneOut([&] { backend::kernel::batchTritwiseMaxRaw128(a.data(), b.data(), out.data(), out.size(), trits); }); }));
    };

    addRaw64Width("TritLane1", TritLane1::trits,
        [](int n) { return toLane(fromIntT1(n < 0 ? -1 : (n > 0 ? 1 : 0))); });
    addRaw64Width("TritLane5", TritLane5::trits,
        [](int n) { return toLane(fromIntT5(n)); });
    addRaw64Width("TritLane10", TritLane10::trits,
        [](int n) { return toLane(fromIntT10(n)); });
    addRaw64Width("TritLane20", TritLane20::trits,
        [](int n) { return toLane(fromIntT20(n)); });
    addRaw128Width("TritLane40", TritLane40::trits,
        [](int n) { return toLane(fromIntT40(n)); });
    addRaw128Width("TritLane50", TritLane50::trits,
        [](int n) { return toLane(fromInt(n)); });
}

void addVmBenches(std::vector<Result>& results, int scale) {
    const uint64_t repeats = 20'000ULL * scale;
    uint64_t totalSteps = 0;
    Result r = measure("vm", "dispatch loop", "instr/s", 0,
        [&] { return benchVmLoop(repeats, totalSteps); });
    r.units = totalSteps;
    results.push_back(r);
}

} // namespace

int main(int argc, char** argv) {
    int scale = 1;
    if (argc > 1) {
        scale = std::atoi(argv[1]);
        if (scale < 1) scale = 1;
    }

    sandbox::LongTriple::initPowTable();

    std::vector<Result> results;
    results.reserve(120);

    addUInt128Benches(results, scale);
    addNativeBenches(results, scale);
    addLaneBenches(results, scale);
    addSimdBenches(results, scale);
    addKernelRawBenches(results, scale);
    addVmBenches(results, scale);

    std::cout << "Ternary baseline benchmark (scale=" << scale << ")\n";
    std::cout << "Build with -O3 -DNDEBUG for comparable numbers.\n";
    std::cout << std::left << std::setw(13) << "group"
              << std::setw(30) << "benchmark"
              << std::right << std::setw(14) << "units"
              << "  " << std::setw(8) << "ms"
              << "  " << std::setw(10) << "ns/unit"
              << "  " << std::setw(13) << "units/sec"
              << "  unit  checksum\n";
    std::cout << std::string(118, '-') << "\n";

    for (const auto& result : results) printResult(result);

    std::cout << "\nsink=0x" << std::hex << g_sink << std::dec << "\n";
    return 0;
}
