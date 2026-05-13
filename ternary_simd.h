// =============================================================================
// ternary_simd.h - optional SIMD batch operations for lane types
// =============================================================================
//
// Batch APIs operate on lane/wire types, not numeric positional formats. Scalar
// batch paths are the correctness oracle; optimized backends may be selected at
// runtime when available.

#pragma once
#ifndef TERNARY_SIMD_H
#define TERNARY_SIMD_H

#include "ternary_lanes.h"

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#define TERNARY_SIMD_GNU_X86 1
#include <immintrin.h>
#else
#define TERNARY_SIMD_GNU_X86 0
#endif

namespace sandbox {
namespace simd {

enum class BatchBackend : uint8_t {
    Scalar,
    Auto,
    Avx2
};

[[nodiscard]] inline bool avx2Available() {
#if TERNARY_SIMD_GNU_X86
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

[[nodiscard]] inline bool backendAvailable(BatchBackend backend) {
    switch (backend) {
        case BatchBackend::Scalar: return true;
        case BatchBackend::Auto:   return true;
        case BatchBackend::Avx2:   return avx2Available();
    }
    return true;
}

[[nodiscard]] inline BatchBackend resolveBackend(BatchBackend requested) {
    if (requested == BatchBackend::Scalar) return BatchBackend::Scalar;
    if (requested == BatchBackend::Avx2 && !avx2Available()) return BatchBackend::Scalar;
    if (requested == BatchBackend::Auto) return avx2Available() ? BatchBackend::Avx2
                                                                : BatchBackend::Scalar;
    return requested;
}

[[nodiscard]] inline const char* backendName(BatchBackend backend = BatchBackend::Auto) {
    switch (resolveBackend(backend)) {
        case BatchBackend::Scalar: return "scalar";
        case BatchBackend::Avx2:   return "avx2";
        case BatchBackend::Auto:   return "auto";
    }
    return "scalar";
}

namespace detail {

template<typename Lane>
[[nodiscard]] inline Lane invalidLane();

template<>
[[nodiscard]] inline TritLane1 invalidLane<TritLane1>() { return TritLane1::invalid(); }
template<>
[[nodiscard]] inline TritLane5 invalidLane<TritLane5>() { return TritLane5::invalid(); }
template<>
[[nodiscard]] inline TritLane10 invalidLane<TritLane10>() { return TritLane10::invalid(); }
template<>
[[nodiscard]] inline TritLane20 invalidLane<TritLane20>() { return TritLane20::invalid(); }
template<>
[[nodiscard]] inline TritLane40 invalidLane<TritLane40>() { return TritLane40::invalid(); }
template<>
[[nodiscard]] inline TritLane50 invalidLane<TritLane50>() { return TritLane50::invalid(); }

template<typename Lane>
[[nodiscard]] inline TritLane1 compareResult(int8_t value) {
    (void)sizeof(Lane);
    TritLane1 out;
    out.setTrit(0, value);
    return out;
}

template<typename Lane>
[[nodiscard]] inline Lane tritwiseMin(Lane a, Lane b) {
    if (!a.isValid() || !b.isValid()) return invalidLane<Lane>();
    return tritwiseCompare(a, b) <= 0 ? a : b;
}

template<typename Lane>
[[nodiscard]] inline Lane tritwiseMax(Lane a, Lane b) {
    if (!a.isValid() || !b.isValid()) return invalidLane<Lane>();
    return tritwiseCompare(a, b) >= 0 ? a : b;
}

} // namespace detail

template<typename Lane>
inline void batchTritwiseNegScalar(const Lane* in, Lane* out, std::size_t count) {
    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = tritwiseNeg(in[i]);
}

template<typename Lane>
inline void batchTritwiseAddScalar(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count) {

    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = tritwiseAdd(a[i], b[i]);
}

template<typename Lane>
inline void batchTritwiseSubScalar(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count) {

    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = tritwiseSubtract(a[i], b[i]);
}

template<typename Lane>
inline void batchTritwiseCompareScalar(
    const Lane* a,
    const Lane* b,
    TritLane1* out,
    std::size_t count) {

    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = detail::compareResult<Lane>(tritwiseCompare(a[i], b[i]));
    }
}

template<typename Lane>
inline void batchTritwiseMinScalar(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count) {

    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = detail::tritwiseMin(a[i], b[i]);
}

template<typename Lane>
inline void batchTritwiseMaxScalar(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count) {

    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) out[i] = detail::tritwiseMax(a[i], b[i]);
}

inline void batchTritwiseAddT20Scalar(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count) {

    batchTritwiseAddScalar(a, b, out, count);
}

#if TERNARY_SIMD_GNU_X86
__attribute__((target("avx2")))
inline void batchTritwiseNegT20Avx2(
    const TritLane20* in,
    TritLane20* out,
    std::size_t count) {

    if (count == 0) return;
    const __m256i two = _mm256_set1_epi64x(2);
    const __m256i three = _mm256_set1_epi64x(3);
    const uint64_t invalidRaw = TritLane20::invalid().rawForKernel();

    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        if (!in[i].isValid() || !in[i + 1].isValid() ||
            !in[i + 2].isValid() || !in[i + 3].isValid()) {
            out[i] = tritwiseNeg(in[i]);
            out[i + 1] = tritwiseNeg(in[i + 1]);
            out[i + 2] = tritwiseNeg(in[i + 2]);
            out[i + 3] = tritwiseNeg(in[i + 3]);
            continue;
        }

        const __m256i value = _mm256_set_epi64x(
            static_cast<long long>(in[i + 3].rawForKernel()),
            static_cast<long long>(in[i + 2].rawForKernel()),
            static_cast<long long>(in[i + 1].rawForKernel()),
            static_cast<long long>(in[i].rawForKernel()));

        __m256i result = _mm256_setzero_si256();
        for (int pos = 0; pos < TritLane20::trits; ++pos) {
            const __m256i shift = _mm256_set1_epi64x(2 * pos);
            const __m256i raw = _mm256_and_si256(_mm256_srlv_epi64(value, shift), three);
            const __m256i rawOut = _mm256_sub_epi64(two, raw);
            result = _mm256_or_si256(result, _mm256_sllv_epi64(rawOut, shift));
        }

        alignas(32) uint64_t rawResult[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(rawResult), result);
        for (int lane = 0; lane < 4; ++lane) {
            auto candidate = TritLane20::fromRawForKernel(rawResult[lane]);
            out[i + static_cast<std::size_t>(lane)] =
                candidate.isValid() ? candidate : TritLane20::fromRawForKernel(invalidRaw);
        }
    }

    batchTritwiseNegScalar(in + i, out + i, count - i);
}

__attribute__((target("avx2")))
inline void batchTritwiseAddSubT20Avx2(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count,
    bool subtractB) {

    if (count == 0) return;
    const uint64_t invalidRaw = TritLane20::invalid().rawForKernel();
    const __m256i zero = _mm256_setzero_si256();
    const __m256i one = _mm256_set1_epi64x(1);
    const __m256i negOne = _mm256_set1_epi64x(-1);
    const __m256i three = _mm256_set1_epi64x(3);

    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        if (!a[i].isValid() || !a[i + 1].isValid() || !a[i + 2].isValid() || !a[i + 3].isValid() ||
            !b[i].isValid() || !b[i + 1].isValid() || !b[i + 2].isValid() || !b[i + 3].isValid()) {
            out[i] = subtractB ? tritwiseSubtract(a[i], b[i]) : tritwiseAdd(a[i], b[i]);
            out[i + 1] = subtractB ? tritwiseSubtract(a[i + 1], b[i + 1]) : tritwiseAdd(a[i + 1], b[i + 1]);
            out[i + 2] = subtractB ? tritwiseSubtract(a[i + 2], b[i + 2]) : tritwiseAdd(a[i + 2], b[i + 2]);
            out[i + 3] = subtractB ? tritwiseSubtract(a[i + 3], b[i + 3]) : tritwiseAdd(a[i + 3], b[i + 3]);
            continue;
        }

        const __m256i va = _mm256_set_epi64x(
            static_cast<long long>(a[i + 3].rawForKernel()),
            static_cast<long long>(a[i + 2].rawForKernel()),
            static_cast<long long>(a[i + 1].rawForKernel()),
            static_cast<long long>(a[i].rawForKernel()));
        const __m256i vb = _mm256_set_epi64x(
            static_cast<long long>(b[i + 3].rawForKernel()),
            static_cast<long long>(b[i + 2].rawForKernel()),
            static_cast<long long>(b[i + 1].rawForKernel()),
            static_cast<long long>(b[i].rawForKernel()));

        __m256i result = zero;
        __m256i carry = zero;

        for (int pos = 0; pos < TritLane20::trits; ++pos) {
            const __m256i shift = _mm256_set1_epi64x(2 * pos);
            const __m256i rawA = _mm256_and_si256(_mm256_srlv_epi64(va, shift), three);
            const __m256i rawB = _mm256_and_si256(_mm256_srlv_epi64(vb, shift), three);

            const __m256i av = _mm256_sub_epi64(rawA, one);
            __m256i bv = _mm256_sub_epi64(rawB, one);
            if (subtractB) bv = _mm256_sub_epi64(zero, bv);

            __m256i sum = _mm256_add_epi64(_mm256_add_epi64(av, bv), carry);
            const __m256i gt = _mm256_cmpgt_epi64(sum, one);
            const __m256i lt = _mm256_cmpgt_epi64(negOne, sum);

            sum = _mm256_sub_epi64(sum, _mm256_and_si256(gt, three));
            sum = _mm256_add_epi64(sum, _mm256_and_si256(lt, three));

            const __m256i rawOut = _mm256_add_epi64(sum, one);
            result = _mm256_or_si256(result, _mm256_sllv_epi64(rawOut, shift));

            carry = _mm256_blendv_epi8(zero, one, gt);
            carry = _mm256_blendv_epi8(carry, negOne, lt);
        }

        alignas(32) uint64_t rawResult[4];
        alignas(32) int64_t rawCarry[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(rawResult), result);
        _mm256_store_si256(reinterpret_cast<__m256i*>(rawCarry), carry);

        for (int lane = 0; lane < 4; ++lane) {
            if (rawCarry[lane] != 0) {
                out[i + static_cast<std::size_t>(lane)] = TritLane20::fromRawForKernel(invalidRaw);
            } else {
                auto candidate = TritLane20::fromRawForKernel(rawResult[lane]);
                out[i + static_cast<std::size_t>(lane)] =
                    candidate.isValid() ? candidate : TritLane20::fromRawForKernel(invalidRaw);
            }
        }
    }

    if (subtractB) batchTritwiseSubScalar(a + i, b + i, out + i, count - i);
    else batchTritwiseAddScalar(a + i, b + i, out + i, count - i);
}

__attribute__((target("avx2")))
inline void batchTritwiseAddT20Avx2(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count) {

    batchTritwiseAddSubT20Avx2(a, b, out, count, false);
}

__attribute__((target("avx2")))
inline void batchTritwiseSubT20Avx2(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count) {

    batchTritwiseAddSubT20Avx2(a, b, out, count, true);
}

__attribute__((target("avx2")))
inline void batchTritwiseCompareT20Avx2(
    const TritLane20* a,
    const TritLane20* b,
    TritLane1* out,
    std::size_t count) {

    if (count == 0) return;
    const __m256i zero = _mm256_setzero_si256();
    const __m256i one = _mm256_set1_epi64x(1);
    const __m256i negOne = _mm256_set1_epi64x(-1);
    const __m256i three = _mm256_set1_epi64x(3);

    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        if (!a[i].isValid() || !a[i + 1].isValid() || !a[i + 2].isValid() || !a[i + 3].isValid() ||
            !b[i].isValid() || !b[i + 1].isValid() || !b[i + 2].isValid() || !b[i + 3].isValid()) {
            batchTritwiseCompareScalar(a + i, b + i, out + i, 4);
            continue;
        }

        const __m256i va = _mm256_set_epi64x(
            static_cast<long long>(a[i + 3].rawForKernel()),
            static_cast<long long>(a[i + 2].rawForKernel()),
            static_cast<long long>(a[i + 1].rawForKernel()),
            static_cast<long long>(a[i].rawForKernel()));
        const __m256i vb = _mm256_set_epi64x(
            static_cast<long long>(b[i + 3].rawForKernel()),
            static_cast<long long>(b[i + 2].rawForKernel()),
            static_cast<long long>(b[i + 1].rawForKernel()),
            static_cast<long long>(b[i].rawForKernel()));

        __m256i cmp = zero;
        for (int pos = TritLane20::trits - 1; pos >= 0; --pos) {
            const __m256i shift = _mm256_set1_epi64x(2 * pos);
            const __m256i av = _mm256_sub_epi64(
                _mm256_and_si256(_mm256_srlv_epi64(va, shift), three), one);
            const __m256i bv = _mm256_sub_epi64(
                _mm256_and_si256(_mm256_srlv_epi64(vb, shift), three), one);

            const __m256i unset = _mm256_cmpeq_epi64(cmp, zero);
            const __m256i gt = _mm256_and_si256(unset, _mm256_cmpgt_epi64(av, bv));
            const __m256i lt = _mm256_and_si256(unset, _mm256_cmpgt_epi64(bv, av));
            cmp = _mm256_blendv_epi8(cmp, one, gt);
            cmp = _mm256_blendv_epi8(cmp, negOne, lt);
        }

        alignas(32) int64_t rawCmp[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(rawCmp), cmp);
        for (int lane = 0; lane < 4; ++lane) {
            out[i + static_cast<std::size_t>(lane)].setTrit(0, static_cast<int8_t>(rawCmp[lane]));
        }
    }

    batchTritwiseCompareScalar(a + i, b + i, out + i, count - i);
}
#endif

template<typename Lane>
inline void batchTritwiseNeg(
    const Lane* in,
    Lane* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseNegScalar(in, out, count);
}

inline void batchTritwiseNeg(
    const TritLane20* in,
    TritLane20* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

#if TERNARY_SIMD_GNU_X86
    if (resolveBackend(backend) == BatchBackend::Avx2) {
        batchTritwiseNegT20Avx2(in, out, count);
        return;
    }
#else
    (void)backend;
#endif
    batchTritwiseNegScalar(in, out, count);
}

template<typename Lane>
inline void batchTritwiseAdd(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseAddScalar(a, b, out, count);
}

inline void batchTritwiseAdd(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

#if TERNARY_SIMD_GNU_X86
    if (resolveBackend(backend) == BatchBackend::Avx2) {
        batchTritwiseAddT20Avx2(a, b, out, count);
        return;
    }
#else
    (void)backend;
#endif
    batchTritwiseAddScalar(a, b, out, count);
}

template<typename Lane>
inline void batchTritwiseSub(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseSubScalar(a, b, out, count);
}

inline void batchTritwiseSub(
    const TritLane20* a,
    const TritLane20* b,
    TritLane20* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

#if TERNARY_SIMD_GNU_X86
    if (resolveBackend(backend) == BatchBackend::Avx2) {
        batchTritwiseSubT20Avx2(a, b, out, count);
        return;
    }
#else
    (void)backend;
#endif
    batchTritwiseSubScalar(a, b, out, count);
}

template<typename Lane>
inline void batchTritwiseCompare(
    const Lane* a,
    const Lane* b,
    TritLane1* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseCompareScalar(a, b, out, count);
}

inline void batchTritwiseCompare(
    const TritLane20* a,
    const TritLane20* b,
    TritLane1* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

#if TERNARY_SIMD_GNU_X86
    if (resolveBackend(backend) == BatchBackend::Avx2) {
        batchTritwiseCompareT20Avx2(a, b, out, count);
        return;
    }
#else
    (void)backend;
#endif
    batchTritwiseCompareScalar(a, b, out, count);
}

template<typename Lane>
inline void batchTritwiseMin(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseMinScalar(a, b, out, count);
}

template<typename Lane>
inline void batchTritwiseMax(
    const Lane* a,
    const Lane* b,
    Lane* out,
    std::size_t count,
    BatchBackend backend = BatchBackend::Auto) {

    (void)backend;
    batchTritwiseMaxScalar(a, b, out, count);
}

} // namespace simd
} // namespace sandbox

#endif // TERNARY_SIMD_H
