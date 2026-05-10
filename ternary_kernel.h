// =============================================================================
// ternary_kernel.h - kernel-callable raw lane batch wrappers
// =============================================================================
//
// These wrappers are intentionally below the C++ lane classes. They operate on
// raw 2-bit-per-trit payloads so CUDA/SYCL kernels can share the same semantics
// without pulling in STL-heavy host utilities.

#pragma once
#ifndef TERNARY_KERNEL_H
#define TERNARY_KERNEL_H

#include "ternary_backend.h"

#include <cstddef>
#include <cstdint>

namespace sandbox {
namespace backend {
namespace kernel {

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseNegRaw64(
    const uint64_t* in,
    uint64_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = negLane64(in[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseAddRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = addLane64(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseSubRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = subLane64(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseCompareRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint8_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane64(a[i], b[i], trits)));
    }
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseMinRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = minLane64(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseMaxRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = maxLane64(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseNegRaw128(
    const RawUInt128* in,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = negLane128(in[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseAddRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = addLane128(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseSubRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = subLane128(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseCompareRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    uint8_t* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane128(a[i], b[i], trits)));
    }
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseMinRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = minLane128(a[i], b[i], trits);
}

TERNARY_HOST_DEVICE TERNARY_FORCE_INLINE void batchTritwiseMaxRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    for (std::size_t i = 0; i < count; ++i) out[i] = maxLane128(a[i], b[i], trits);
}

} // namespace kernel
} // namespace backend
} // namespace sandbox

#endif // TERNARY_KERNEL_H
