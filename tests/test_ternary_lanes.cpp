#include "ternary_lanes.h"
#include "ternary_kernel.h"
#include "ternary_simd.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

static int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

int signOf(long long value) {
    if (value < 0) return -1;
    if (value > 0) return 1;
    return 0;
}

sandbox::TritLane1 laneFromInt1(int value) {
    return sandbox::toLane(sandbox::native_ops::fromIntT1(value));
}

sandbox::TritLane5 laneFromInt5(int value) {
    return sandbox::toLane(sandbox::native_ops::fromIntT5(value));
}

sandbox::TritLane10 laneFromInt10(int value) {
    return sandbox::toLane(sandbox::native_ops::fromIntT10(value));
}

sandbox::TritLane20 laneFromInt20(int value) {
    return sandbox::toLane(sandbox::native_ops::fromIntT20(value));
}

sandbox::TritLane40 laneFromInt40(int value) {
    return sandbox::toLane(sandbox::native_ops::fromIntT40(value));
}

sandbox::TritLane50 laneFromInt50(int value) {
    return sandbox::toLane(sandbox::native_ops::fromInt(value));
}

template<typename Lane>
bool sameLane(Lane a, Lane b) {
    return a.rawForKernel() == b.rawForKernel();
}

template<typename Lane>
Lane expectedMin(Lane a, Lane b) {
    if (!a.isValid() || !b.isValid()) return Lane::invalid();
    return tritwiseCompare(a, b) <= 0 ? a : b;
}

template<typename Lane>
Lane expectedMax(Lane a, Lane b) {
    if (!a.isValid() || !b.isValid()) return Lane::invalid();
    return tritwiseCompare(a, b) >= 0 ? a : b;
}

sandbox::TritLane1 compareLane(int8_t value) {
    sandbox::TritLane1 out;
    out.setTrit(0, value);
    return out;
}

sandbox::backend::RawUInt128 raw128FromUInt(sandbox::UInt128 value) {
    return sandbox::backend::makeRawUInt128(value.hi, value.lo);
}

sandbox::UInt128 uintFromRaw128(sandbox::backend::RawUInt128 value) {
    return sandbox::UInt128{value.hi, value.lo};
}

template<typename FloatT, typename LaneT>
void expectFloatRoundtrip(FloatT value, LaneT lane, FloatT back, int mantissaTrits,
                          const std::string& label) {
    expect(lane.isValid(), label + " lane valid");
    if (value.isZero()) {
        expect(back.isZero(), label + " zero canonical roundtrip");
        return;
    }

    const auto trits = value.unpack();
    bool mantissaZero = true;
    for (int i = 0; i < mantissaTrits; ++i) {
        if (trits[i] != 0) {
            mantissaZero = false;
            break;
        }
    }

    if (mantissaZero) {
        expect(back.isZero(), label + " noncanonical zero canonicalizes");
    } else {
        expect(back.data == value.data, label + " exact payload roundtrip");
    }
}

void testBackendPrimitiveContract() {
    std::cout << "[0] backend primitive encoding and invalid-state contract\n";
    using namespace sandbox;

    for (int trit = -1; trit <= 1; ++trit) {
        const uint8_t raw = backend::encodeTritPair(static_cast<int8_t>(trit));
        expect(backend::validTritPair(raw), "encoded balanced trit is valid");
        expect(backend::decodeTritPair(raw) == trit, "valid trit encode/decode roundtrip");
        expect(backend::decodeTritPair(backend::negateTritPair(raw)) == -trit,
               "valid trit negation");
    }

    expect(backend::encodeTritPair(-2) == 0x3U,
           "out-of-range negative trit canonicalizes to invalid");
    expect(backend::encodeTritPair(2) == 0x3U,
           "out-of-range positive trit canonicalizes to invalid");
    expect(backend::decodeTritPair(0x3U) == 0,
           "invalid pair decodes to the HDL fallback value");
    expect(backend::decodeTritPair(0xFFU) == 0,
           "non-pair input decodes to the HDL fallback value");
    expect(backend::negateTritPair(0x3U) == 0x3U,
           "invalid pair remains canonically invalid under negation");
    expect(backend::negateTritPair(0xFFU) == 0x3U,
           "non-pair input canonicalizes to invalid under negation");

    for (int original = -10; original <= 10; ++original) {
        int normalized = original;
        const int8_t result = backend::normalizeTritSum(normalized);
        expect(result >= -1 && result <= 1, "normalized trit sum is balanced");
        expect(normalized == result, "normalizer mutates sum to returned digit");
        expect((original - normalized) % 3 == 0, "normalizer preserves value modulo three");
    }
}

void testLaneRawValidation() {
    std::cout << "[1] lane raw validation and spare-state rejection\n";
    using namespace sandbox;

    int validCount1 = 0;
    for (uint32_t raw = 0; raw <= UINT8_MAX; ++raw) {
        TritLane1 lane = TritLane1::fromRawForKernel(static_cast<uint8_t>(raw));
        bool expected = (raw >> TritLane1::used_bits) == 0;
        if ((raw & 0x3U) == 0x3U) expected = false;
        if (expected) ++validCount1;
        expect(lane.isValid() == expected, "TritLane1 raw validity");
    }
    expect(validCount1 == 3, "TritLane1 has exactly 3 valid encodings");

    int validCount = 0;
    for (uint32_t raw = 0; raw <= UINT16_MAX; ++raw) {
        TritLane5 lane = TritLane5::fromRawForKernel(static_cast<uint16_t>(raw));
        bool expected = (raw >> TritLane5::used_bits) == 0;
        for (int i = 0; i < TritLane5::trits; ++i) {
            if (((raw >> (2 * i)) & 0x3U) == 0x3U) expected = false;
        }
        if (expected) ++validCount;
        expect(lane.isValid() == expected, "TritLane5 raw validity");
    }
    expect(validCount == 243, "TritLane5 has exactly 3^5 valid encodings");

    expect(!TritLane10::fromRawForKernel(0xFFFFFFFFu).isValid(), "TritLane10 rejects invalid trit pairs");
    expect(!TritLane10::fromRawForKernel(1u << TritLane10::used_bits).isValid(), "TritLane10 rejects padding bits");
    expect(!TritLane20::fromRawForKernel(UINT64_MAX).isValid(), "TritLane20 rejects invalid trit pairs");
    expect(!TritLane20::fromRawForKernel(1ULL << TritLane20::used_bits).isValid(), "TritLane20 rejects padding bits");

    UInt128 invalid40{};
    invalid40.hi = 1ULL << (TritLane40::used_bits - 64);
    expect(!TritLane40::fromRawForKernel(invalid40).isValid(), "TritLane40 rejects padding bits");

    TritLane40 badPair40;
    badPair40.setTrit(0, 0);
    UInt128 raw40 = badPair40.rawForKernel();
    raw40.lo |= 0x3ULL;
    expect(!TritLane40::fromRawForKernel(raw40).isValid(), "TritLane40 rejects invalid trit pair");

    UInt128 invalid50{};
    invalid50.hi = 1ULL << (TritLane50::used_bits - 64);
    expect(!TritLane50::fromRawForKernel(invalid50).isValid(), "TritLane50 rejects padding bits");

    TritLane50 badPair50;
    badPair50.setTrit(0, 0);
    UInt128 raw = badPair50.rawForKernel();
    raw.lo |= 0x3ULL;
    expect(!TritLane50::fromRawForKernel(raw).isValid(), "TritLane50 rejects invalid trit pair");
}

void testConversionBoundary() {
    std::cout << "[2] explicit numeric/lane conversion boundary\n";
    using namespace sandbox;

    for (int n = -1; n <= 1; ++n) {
        T1 value = native_ops::fromIntT1(n);
        TritLane1 lane = toLane(value);
        T1 back = fromLane(lane);
        expect(lane.isValid(), "T1 to lane valid");
        expect(back.data == value.data, "T1 exact lane roundtrip");
        expect(native_ops::toLongLong(back) == n, "T1 semantic lane roundtrip");
    }

    expect(!toLane(T1{T1::INVALID_DATA}).isValid(), "invalid T1 converts to invalid lane");
    expect(fromLane(TritLane1::invalid()).isInvalid(), "invalid lane converts to invalid T1");

    for (int n = -121; n <= 121; ++n) {
        T5 value = native_ops::fromIntT5(n);
        TritLane5 lane = toLane(value);
        T5 back = fromLane(lane);
        expect(lane.isValid(), "T5 to lane valid");
        expect(back.data == value.data, "T5 exact lane roundtrip");
        expect(native_ops::toLongLong(back) == n, "T5 semantic lane roundtrip");
    }

    expect(!toLane(T5{T5::INVALID_DATA}).isValid(), "invalid T5 converts to invalid lane");
    expect(fromLane(TritLane5::invalid()).isInvalid(), "invalid lane converts to invalid T5");

    for (uint32_t raw = 0; raw < 59049u; ++raw) {
        T10 value{static_cast<uint16_t>(raw)};
        TritLane10 lane = toLane(value);
        T10 back = fromLane(lane);
        expectFloatRoundtrip(value, lane, back, 6, "T10");
    }

    expect(!toLane(T10{T10::OVERFLOW_DATA}).isValid(), "T10 overflow converts to invalid lane");
    expect(fromLane(TritLane10::invalid()).isOverflow(), "invalid lane converts to T10 overflow");

    uint32_t seed = 0x12345678u;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u;
        T20 value{seed % 3486784401u};
        TritLane20 lane = toLane(value);
        T20 back = fromLane(lane);
        expectFloatRoundtrip(value, lane, back, 14, "T20 corpus");
    }

    expect(!toLane(T20{T20::OVERFLOW_DATA}).isValid(), "T20 overflow converts to invalid lane");
    expect(fromLane(TritLane20::invalid()).isOverflow(), "invalid lane converts to T20 overflow");

    const std::vector<long long> corpus = {
        -1000000, -59049, -729, -121, -1, 0, 1, 2, 3, 121, 729, 59049, 1000000
    };
    for (long long value : corpus) {
        Triple numeric = native_ops::fromIntT40(value);
        TritLane40 lane = toLane(numeric);
        Triple back = fromLane(lane);
        expect(lane.isValid(), "Triple to lane valid");
        expect(back.data == numeric.data, "Triple exact lane roundtrip");
    }

    {
        std::array<int8_t, 40> trits{};
        trits[0] = -1;
        trits[4] = 1;
        trits[16] = -1;
        trits[32] = 1;
        trits[33] = -1;
        trits[38] = 1;
        Triple numeric = Triple::pack(trits);
        TritLane40 lane = toLane(numeric);
        Triple back = fromLane(lane);
        expect(lane.isValid(), "patterned Triple lane valid");
        expect(back.data == numeric.data, "patterned Triple exact lane roundtrip");
    }

    expect(!toLane(Triple{Triple::OVERFLOW_DATA}).isValid(), "Triple overflow converts to invalid lane");
    expect(fromLane(TritLane40::invalid()).isOverflow(), "invalid lane converts to Triple overflow");

    for (long long value : corpus) {
        LongTriple numeric = native_ops::fromInt(value);
        TritLane50 lane = toLane(numeric);
        LongTriple back = fromLane(lane);
        expect(lane.isValid(), "LongTriple to lane valid");
        expect(back.data == numeric.data, "LongTriple exact lane roundtrip");
    }

    {
        std::array<int8_t, 50> trits{};
        trits[0] = 1;
        trits[3] = -1;
        trits[13] = 1;
        trits[40] = -1;
        trits[41] = 1;
        trits[48] = -1;
        LongTriple numeric = LongTriple::pack(trits);
        TritLane50 lane = toLane(numeric);
        LongTriple back = fromLane(lane);
        expect(lane.isValid(), "patterned LongTriple lane valid");
        expect(back.data == numeric.data, "patterned LongTriple exact lane roundtrip");
    }

    expect(!toLane(LongTriple{LongTriple::OVERFLOW_DATA}).isValid(), "LongTriple overflow converts to invalid lane");
    expect(fromLane(TritLane50::invalid()).isOverflow(), "invalid lane converts to LongTriple overflow");
}

void testLaneTritwiseOps() {
    std::cout << "[3] lane tritwise full-adder ops\n";
    using namespace sandbox;

    for (int a = -1; a <= 1; ++a) {
        TritLane1 la = toLane(native_ops::fromIntT1(a));
        TritLane1 neg = tritwiseNeg(la);
        expect(neg.isValid(), "TritLane1 neg valid");
        expect(native_ops::toLongLong(fromLane(neg)) == -a, "TritLane1 neg semantic");

        for (int b = -1; b <= 1; ++b) {
            TritLane1 lb = toLane(native_ops::fromIntT1(b));
            TritLane1 sum = tritwiseAdd(la, lb);
            const int expectedSum = a + b;
            if (expectedSum < -1 || expectedSum > 1) {
                expect(!sum.isValid(), "TritLane1 add overflow invalid");
            } else {
                expect(sum.isValid(), "TritLane1 add valid");
                expect(native_ops::toLongLong(fromLane(sum)) == expectedSum, "TritLane1 add semantic");
            }
            expect(tritwiseCompare(la, lb) == signOf(a - b), "TritLane1 compare semantic");

            TritLane1 carrylessSum = tritwiseAddCarryless(la, lb);
            int expectedCarryless = a + b;
            while (expectedCarryless > 1) expectedCarryless -= 3;
            while (expectedCarryless < -1) expectedCarryless += 3;
            expect(carrylessSum.isValid(), "TritLane1 carryless add always valid");
            expect(carrylessSum.tritAt(0) == expectedCarryless, "TritLane1 carryless add wraps");

            TritLane1 carrylessDiff = tritwiseSubtractCarryless(la, lb);
            int expectedCarrylessDiff = a - b;
            while (expectedCarrylessDiff > 1) expectedCarrylessDiff -= 3;
            while (expectedCarrylessDiff < -1) expectedCarrylessDiff += 3;
            expect(carrylessDiff.isValid(), "TritLane1 carryless sub always valid");
            expect(carrylessDiff.tritAt(0) == expectedCarrylessDiff, "TritLane1 carryless sub wraps");

            expect(tritwiseAnd(la, lb).tritAt(0) == std::min(a, b), "TritLane1 lattice min");
            expect(tritwiseOr(la, lb).tritAt(0) == std::max(a, b), "TritLane1 lattice max");
        }
    }

    for (int a = -121; a <= 121; ++a) {
        TritLane5 la = toLane(native_ops::fromIntT5(a));
        TritLane5 neg = tritwiseNeg(la);
        expect(neg.isValid(), "TritLane5 neg valid");
        expect(native_ops::toLongLong(fromLane(neg)) == -a, "TritLane5 neg semantic");

        for (int b = -121; b <= 121; ++b) {
            TritLane5 lb = toLane(native_ops::fromIntT5(b));

            TritLane5 sum = tritwiseAdd(la, lb);
            const int expectedSum = a + b;
            if (expectedSum < -121 || expectedSum > 121) {
                expect(!sum.isValid(), "TritLane5 add overflow invalid");
            } else {
                expect(sum.isValid(), "TritLane5 add valid");
                expect(native_ops::toLongLong(fromLane(sum)) == expectedSum, "TritLane5 add semantic");
            }

            TritLane5 diff = tritwiseSubtract(la, lb);
            const int expectedDiff = a - b;
            if (expectedDiff < -121 || expectedDiff > 121) {
                expect(!diff.isValid(), "TritLane5 sub overflow invalid");
            } else {
                expect(diff.isValid(), "TritLane5 sub valid");
                expect(native_ops::toLongLong(fromLane(diff)) == expectedDiff, "TritLane5 sub semantic");
            }

            expect(tritwiseCompare(la, lb) == signOf(a - b), "TritLane5 compare semantic");
        }
    }

    expect(!tritwiseNeg(TritLane5::invalid()).isValid(), "invalid lane neg propagates invalid");
    expect(!tritwiseAdd(TritLane5::invalid(), toLane(native_ops::fromIntT5(1))).isValid(),
           "invalid lane add propagates invalid");
}

void testSimdBatchOps() {
    std::cout << "[4] SIMD batch lane ops\n";
    using namespace sandbox;

    expect(sandbox::simd::backendAvailable(sandbox::simd::BatchBackend::Scalar),
           "SIMD scalar backend is always available");
    expect(std::string(sandbox::simd::backendName()).size() > 0, "SIMD backend name exists");

    auto run = [&](const char* label, auto maker) {
        using Lane = decltype(maker(0));
        const std::vector<std::size_t> counts = {0, 1, 2, 3, 4, 5, 7, 16, 17, 64, 257};

        for (std::size_t count : counts) {
            std::vector<Lane> a;
            std::vector<Lane> b;
            a.reserve(count);
            b.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                a.push_back(maker(static_cast<int>((i % 19) - 9)));
                b.push_back(maker(static_cast<int>(((i * 5) % 17) - 8)));
            }
            if (count > 2) a[count / 3] = Lane::invalid();
            if (count > 4) b[count / 2] = Lane::invalid();

            std::vector<Lane> expectedLane(count);
            std::vector<Lane> scalarLane(count);
            std::vector<Lane> autoLane(count);
            std::vector<Lane> avxLane(count);
            std::vector<TritLane1> expectedCmp(count);
            std::vector<TritLane1> scalarCmp(count);
            std::vector<TritLane1> autoCmp(count);
            std::vector<TritLane1> avxCmp(count);

            const Lane* ap = count == 0 ? nullptr : a.data();
            const Lane* bp = count == 0 ? nullptr : b.data();
            Lane* scalarOut = count == 0 ? nullptr : scalarLane.data();
            Lane* autoOut = count == 0 ? nullptr : autoLane.data();
            Lane* avxOut = count == 0 ? nullptr : avxLane.data();
            TritLane1* scalarCmpOut = count == 0 ? nullptr : scalarCmp.data();
            TritLane1* autoCmpOut = count == 0 ? nullptr : autoCmp.data();
            TritLane1* avxCmpOut = count == 0 ? nullptr : avxCmp.data();

            for (std::size_t i = 0; i < count; ++i) expectedLane[i] = tritwiseNeg(a[i]);
            sandbox::simd::batchTritwiseNegScalar(ap, scalarOut, count);
            sandbox::simd::batchTritwiseNeg(ap, autoOut, count);
            sandbox::simd::batchTritwiseNeg(ap, avxOut, count, sandbox::simd::BatchBackend::Avx2);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarLane[i], expectedLane[i]), std::string(label) + " scalar neg");
                expect(sameLane(autoLane[i], expectedLane[i]), std::string(label) + " auto neg");
                expect(sameLane(avxLane[i], expectedLane[i]), std::string(label) + " avx/fallback neg");
            }

            for (std::size_t i = 0; i < count; ++i) expectedLane[i] = tritwiseAdd(a[i], b[i]);
            sandbox::simd::batchTritwiseAddScalar(ap, bp, scalarOut, count);
            sandbox::simd::batchTritwiseAdd(ap, bp, autoOut, count);
            sandbox::simd::batchTritwiseAdd(ap, bp, avxOut, count, sandbox::simd::BatchBackend::Avx2);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarLane[i], expectedLane[i]), std::string(label) + " scalar add");
                expect(sameLane(autoLane[i], expectedLane[i]), std::string(label) + " auto add");
                expect(sameLane(avxLane[i], expectedLane[i]), std::string(label) + " avx/fallback add");
            }

            for (std::size_t i = 0; i < count; ++i) expectedLane[i] = tritwiseSubtract(a[i], b[i]);
            sandbox::simd::batchTritwiseSubScalar(ap, bp, scalarOut, count);
            sandbox::simd::batchTritwiseSub(ap, bp, autoOut, count);
            sandbox::simd::batchTritwiseSub(ap, bp, avxOut, count, sandbox::simd::BatchBackend::Avx2);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarLane[i], expectedLane[i]), std::string(label) + " scalar sub");
                expect(sameLane(autoLane[i], expectedLane[i]), std::string(label) + " auto sub");
                expect(sameLane(avxLane[i], expectedLane[i]), std::string(label) + " avx/fallback sub");
            }

            for (std::size_t i = 0; i < count; ++i) {
                expectedCmp[i] = compareLane(tritwiseCompare(a[i], b[i]));
            }
            sandbox::simd::batchTritwiseCompareScalar(ap, bp, scalarCmpOut, count);
            sandbox::simd::batchTritwiseCompare(ap, bp, autoCmpOut, count);
            sandbox::simd::batchTritwiseCompare(ap, bp, avxCmpOut, count, sandbox::simd::BatchBackend::Avx2);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarCmp[i], expectedCmp[i]), std::string(label) + " scalar compare");
                expect(sameLane(autoCmp[i], expectedCmp[i]), std::string(label) + " auto compare");
                expect(sameLane(avxCmp[i], expectedCmp[i]), std::string(label) + " avx/fallback compare");
            }

            for (std::size_t i = 0; i < count; ++i) expectedLane[i] = expectedMin(a[i], b[i]);
            sandbox::simd::batchTritwiseMinScalar(ap, bp, scalarOut, count);
            sandbox::simd::batchTritwiseMin(ap, bp, autoOut, count);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarLane[i], expectedLane[i]), std::string(label) + " scalar min");
                expect(sameLane(autoLane[i], expectedLane[i]), std::string(label) + " auto min");
            }

            for (std::size_t i = 0; i < count; ++i) expectedLane[i] = expectedMax(a[i], b[i]);
            sandbox::simd::batchTritwiseMaxScalar(ap, bp, scalarOut, count);
            sandbox::simd::batchTritwiseMax(ap, bp, autoOut, count);
            for (std::size_t i = 0; i < count; ++i) {
                expect(sameLane(scalarLane[i], expectedLane[i]), std::string(label) + " scalar max");
                expect(sameLane(autoLane[i], expectedLane[i]), std::string(label) + " auto max");
            }
        }
    };

    run("TritLane1", laneFromInt1);
    run("TritLane5", laneFromInt5);
    run("TritLane10", laneFromInt10);
    run("TritLane20", laneFromInt20);
    run("TritLane40", laneFromInt40);
    run("TritLane50", laneFromInt50);
}

template<typename Lane, typename Maker>
void runRaw64KernelCase(const char* label, Maker maker) {
    using namespace sandbox;
    const int trits = Lane::trits;
    const std::vector<std::size_t> counts = {0, 1, 2, 5, 16, 17, 64, 129};

    backend::kernel::batchTritwiseNegRaw64(nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseAddRaw64(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseSubRaw64(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseCompareRaw64(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseMinRaw64(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseMaxRaw64(nullptr, nullptr, nullptr, 0, trits);

    for (std::size_t count : counts) {
        std::vector<Lane> laneA(count);
        std::vector<Lane> laneB(count);
        std::vector<uint64_t> a(count);
        std::vector<uint64_t> b(count);
        std::vector<uint64_t> out(count);
        std::vector<uint8_t> cmp(count);

        for (std::size_t i = 0; i < count; ++i) {
            laneA[i] = maker(static_cast<int>((i % 19) - 9));
            laneB[i] = maker(static_cast<int>(((i * 7) % 23) - 11));
        }
        if (count > 2) laneA[count / 3] = Lane::invalid();
        if (count > 4) laneB[count / 2] = Lane::invalid();

        for (std::size_t i = 0; i < count; ++i) {
            a[i] = static_cast<uint64_t>(laneA[i].rawForKernel());
            b[i] = static_cast<uint64_t>(laneB[i].rawForKernel());
        }

        const uint64_t* ap = count == 0 ? nullptr : a.data();
        const uint64_t* bp = count == 0 ? nullptr : b.data();
        uint64_t* outp = count == 0 ? nullptr : out.data();
        uint8_t* cmpp = count == 0 ? nullptr : cmp.data();

        backend::kernel::batchTritwiseNegRaw64(ap, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(static_cast<typename Lane::storage_type>(out[i]));
            expect(sameLane(got, tritwiseNeg(laneA[i])), std::string(label) + " raw64 neg");
        }

        backend::kernel::batchTritwiseAddRaw64(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(static_cast<typename Lane::storage_type>(out[i]));
            expect(sameLane(got, tritwiseAdd(laneA[i], laneB[i])), std::string(label) + " raw64 add");
        }

        backend::kernel::batchTritwiseSubRaw64(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(static_cast<typename Lane::storage_type>(out[i]));
            expect(sameLane(got, tritwiseSubtract(laneA[i], laneB[i])), std::string(label) + " raw64 sub");
        }

        backend::kernel::batchTritwiseCompareRaw64(ap, bp, cmpp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            TritLane1 got = TritLane1::fromRawForKernel(cmp[i]);
            expect(sameLane(got, compareLane(tritwiseCompare(laneA[i], laneB[i]))),
                   std::string(label) + " raw64 compare");
        }

        backend::kernel::batchTritwiseMinRaw64(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(static_cast<typename Lane::storage_type>(out[i]));
            expect(sameLane(got, expectedMin(laneA[i], laneB[i])), std::string(label) + " raw64 min");
        }

        backend::kernel::batchTritwiseMaxRaw64(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(static_cast<typename Lane::storage_type>(out[i]));
            expect(sameLane(got, expectedMax(laneA[i], laneB[i])), std::string(label) + " raw64 max");
        }
    }
}

template<typename Lane, typename Maker>
void runRaw128KernelCase(const char* label, Maker maker) {
    using namespace sandbox;
    const int trits = Lane::trits;
    const std::vector<std::size_t> counts = {0, 1, 2, 5, 16, 17, 64, 129};

    backend::kernel::batchTritwiseNegRaw128(nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseAddRaw128(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseSubRaw128(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseCompareRaw128(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseMinRaw128(nullptr, nullptr, nullptr, 0, trits);
    backend::kernel::batchTritwiseMaxRaw128(nullptr, nullptr, nullptr, 0, trits);

    for (std::size_t count : counts) {
        std::vector<Lane> laneA(count);
        std::vector<Lane> laneB(count);
        std::vector<backend::RawUInt128> a(count);
        std::vector<backend::RawUInt128> b(count);
        std::vector<backend::RawUInt128> out(count);
        std::vector<uint8_t> cmp(count);

        for (std::size_t i = 0; i < count; ++i) {
            laneA[i] = maker(static_cast<int>((i % 19) - 9));
            laneB[i] = maker(static_cast<int>(((i * 7) % 23) - 11));
        }
        if (count > 2) laneA[count / 3] = Lane::invalid();
        if (count > 4) laneB[count / 2] = Lane::invalid();

        for (std::size_t i = 0; i < count; ++i) {
            a[i] = raw128FromUInt(laneA[i].rawForKernel());
            b[i] = raw128FromUInt(laneB[i].rawForKernel());
        }

        const backend::RawUInt128* ap = count == 0 ? nullptr : a.data();
        const backend::RawUInt128* bp = count == 0 ? nullptr : b.data();
        backend::RawUInt128* outp = count == 0 ? nullptr : out.data();
        uint8_t* cmpp = count == 0 ? nullptr : cmp.data();

        backend::kernel::batchTritwiseNegRaw128(ap, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(uintFromRaw128(out[i]));
            expect(sameLane(got, tritwiseNeg(laneA[i])), std::string(label) + " raw128 neg");
        }

        backend::kernel::batchTritwiseAddRaw128(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(uintFromRaw128(out[i]));
            expect(sameLane(got, tritwiseAdd(laneA[i], laneB[i])), std::string(label) + " raw128 add");
        }

        backend::kernel::batchTritwiseSubRaw128(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(uintFromRaw128(out[i]));
            expect(sameLane(got, tritwiseSubtract(laneA[i], laneB[i])), std::string(label) + " raw128 sub");
        }

        backend::kernel::batchTritwiseCompareRaw128(ap, bp, cmpp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            TritLane1 got = TritLane1::fromRawForKernel(cmp[i]);
            expect(sameLane(got, compareLane(tritwiseCompare(laneA[i], laneB[i]))),
                   std::string(label) + " raw128 compare");
        }

        backend::kernel::batchTritwiseMinRaw128(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(uintFromRaw128(out[i]));
            expect(sameLane(got, expectedMin(laneA[i], laneB[i])), std::string(label) + " raw128 min");
        }

        backend::kernel::batchTritwiseMaxRaw128(ap, bp, outp, count, trits);
        for (std::size_t i = 0; i < count; ++i) {
            Lane got = Lane::fromRawForKernel(uintFromRaw128(out[i]));
            expect(sameLane(got, expectedMax(laneA[i], laneB[i])), std::string(label) + " raw128 max");
        }
    }
}

void testBackendKernelWrappers() {
    std::cout << "[5] backend raw lane kernel wrappers\n";
    using namespace sandbox;

    expect(backend::validLane64(laneFromInt20(1).rawForKernel(), TritLane20::trits),
           "raw64 valid lane accepts T20 payload");
    expect(!backend::validLane64(1ULL << TritLane20::used_bits, TritLane20::trits),
           "raw64 valid lane rejects padding");
    expect(!backend::validLane64(backend::invalidLane64(TritLane20::trits), TritLane20::trits),
           "raw64 valid lane rejects spare pair");

    backend::RawUInt128 raw40 = raw128FromUInt(laneFromInt40(1).rawForKernel());
    expect(backend::validLane128(raw40, TritLane40::trits),
           "raw128 valid lane accepts T40 payload");
    raw40.hi |= 1ULL << (TritLane40::used_bits - 64);
    expect(!backend::validLane128(raw40, TritLane40::trits),
           "raw128 valid lane rejects padding");
    expect(!backend::validLane128(backend::invalidLane128(TritLane50::trits), TritLane50::trits),
           "raw128 valid lane rejects spare pair");

    runRaw64KernelCase<TritLane1>("TritLane1", laneFromInt1);
    runRaw64KernelCase<TritLane5>("TritLane5", laneFromInt5);
    runRaw64KernelCase<TritLane10>("TritLane10", laneFromInt10);
    runRaw64KernelCase<TritLane20>("TritLane20", laneFromInt20);
    runRaw128KernelCase<TritLane40>("TritLane40", laneFromInt40);
    runRaw128KernelCase<TritLane50>("TritLane50", laneFromInt50);
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();

    testBackendPrimitiveContract();
    testLaneRawValidation();
    testConversionBoundary();
    testLaneTritwiseOps();
    testSimdBatchOps();
    testBackendKernelWrappers();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " lane test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll lane tests passed\n";
    return EXIT_SUCCESS;
}
