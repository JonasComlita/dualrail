// =============================================================================
// ternary_gpu_kernels.h - optional CUDA/SYCL launch wrappers for raw lanes
// =============================================================================
//
// This header is inert in plain C++ builds. Define TERNARY_ENABLE_CUDA or
// TERNARY_ENABLE_SYCL when compiling with the corresponding toolchain to expose
// concrete kernel launch entry points over the raw lane batch API.

#pragma once
#ifndef TERNARY_GPU_KERNELS_H
#define TERNARY_GPU_KERNELS_H

#include "ternary_kernel.h"

#include <cstddef>
#include <cstdint>

#if defined(TERNARY_ENABLE_CUDA)
#include <cuda_runtime.h>
#endif

#if defined(TERNARY_ENABLE_SYCL)
#include <sycl/sycl.hpp>
#endif

namespace sandbox {
namespace backend {
namespace gpu {

#if defined(TERNARY_ENABLE_CUDA)
inline constexpr bool CUDA_LANE_KERNELS_ENABLED = true;

__global__ inline void cudaTritwiseNegRaw64Kernel(
    const uint64_t* in,
    uint64_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = negLane64(in[idx], trits);
}

__global__ inline void cudaTritwiseAddRaw64Kernel(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = addLane64(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseSubRaw64Kernel(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = subLane64(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseCompareRaw64Kernel(
    const uint64_t* a,
    const uint64_t* b,
    uint8_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        out[idx] = static_cast<uint8_t>(compareResultLane1Raw(compareLane64(a[idx], b[idx], trits)));
    }
}

__global__ inline void cudaTritwiseMinRaw64Kernel(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = minLane64(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseMaxRaw64Kernel(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = maxLane64(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseNegRaw128Kernel(
    const RawUInt128* in,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = negLane128(in[idx], trits);
}

__global__ inline void cudaTritwiseAddRaw128Kernel(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = addLane128(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseSubRaw128Kernel(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = subLane128(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseCompareRaw128Kernel(
    const RawUInt128* a,
    const RawUInt128* b,
    uint8_t* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) {
        out[idx] = static_cast<uint8_t>(compareResultLane1Raw(compareLane128(a[idx], b[idx], trits)));
    }
}

__global__ inline void cudaTritwiseMinRaw128Kernel(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = minLane128(a[idx], b[idx], trits);
}

__global__ inline void cudaTritwiseMaxRaw128Kernel(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits) {

    const std::size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < count) out[idx] = maxLane128(a[idx], b[idx], trits);
}

inline dim3 cudaLaneGrid(std::size_t count, int blockSize = 256) {
    return dim3(static_cast<unsigned>((count + static_cast<std::size_t>(blockSize) - 1) /
                                      static_cast<std::size_t>(blockSize)));
}

inline void cudaLaunchTritwiseNegRaw64(
    const uint64_t* in,
    uint64_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseNegRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(in, out, count, trits);
}

inline void cudaLaunchTritwiseAddRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseAddRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseSubRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseSubRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseCompareRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint8_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseCompareRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseMinRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseMinRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseMaxRaw64(
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseMaxRaw64Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseNegRaw128(
    const RawUInt128* in,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseNegRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(in, out, count, trits);
}

inline void cudaLaunchTritwiseAddRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseAddRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseSubRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseSubRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseCompareRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    uint8_t* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseCompareRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseMinRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseMinRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}

inline void cudaLaunchTritwiseMaxRaw128(
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int blockSize = 256,
    cudaStream_t stream = nullptr) {

    cudaTritwiseMaxRaw128Kernel<<<cudaLaneGrid(count, blockSize), blockSize, 0, stream>>>(a, b, out, count, trits);
}
#else
inline constexpr bool CUDA_LANE_KERNELS_ENABLED = false;
#endif

#if defined(TERNARY_ENABLE_SYCL)
inline constexpr bool SYCL_LANE_KERNELS_ENABLED = true;

inline sycl::event syclLaunchTritwiseNegRaw64(
    sycl::queue& queue,
    const uint64_t* in,
    uint64_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = negLane64(in[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = negLane64(in[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseAddRaw64(
    sycl::queue& queue,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = addLane64(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = addLane64(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseSubRaw64(
    sycl::queue& queue,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = subLane64(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = subLane64(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseCompareRaw64(
    sycl::queue& queue,
    const uint64_t* a,
    const uint64_t* b,
    uint8_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane64(a[i], b[i], trits)));
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane64(a[i], b[i], trits)));
        });
    }
}

inline sycl::event syclLaunchTritwiseMinRaw64(
    sycl::queue& queue,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = minLane64(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = minLane64(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseMaxRaw64(
    sycl::queue& queue,
    const uint64_t* a,
    const uint64_t* b,
    uint64_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = maxLane64(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = maxLane64(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseNegRaw128(
    sycl::queue& queue,
    const RawUInt128* in,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = negLane128(in[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = negLane128(in[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseAddRaw128(
    sycl::queue& queue,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = addLane128(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = addLane128(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseSubRaw128(
    sycl::queue& queue,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = subLane128(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = subLane128(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseCompareRaw128(
    sycl::queue& queue,
    const RawUInt128* a,
    const RawUInt128* b,
    uint8_t* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane128(a[i], b[i], trits)));
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane128(a[i], b[i], trits)));
        });
    }
}

inline sycl::event syclLaunchTritwiseMinRaw128(
    sycl::queue& queue,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = minLane128(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = minLane128(a[i], b[i], trits);
        });
    }
}

inline sycl::event syclLaunchTritwiseMaxRaw128(
    sycl::queue& queue,
    const RawUInt128* a,
    const RawUInt128* b,
    RawUInt128* out,
    std::size_t count,
    int trits,
    int localSize = 0) {

    if (localSize > 0) {
        std::size_t globalSize = ((count + static_cast<std::size_t>(localSize) - 1) / 
                                   static_cast<std::size_t>(localSize)) * static_cast<std::size_t>(localSize);
        return queue.parallel_for(sycl::nd_range<1>(sycl::range<1>(globalSize), sycl::range<1>(localSize)), [=](sycl::nd_item<1> item) {
            const std::size_t i = item.get_global_id(0);
            if (i < count) {
                out[i] = maxLane128(a[i], b[i], trits);
            }
        });
    } else {
        return queue.parallel_for(sycl::range<1>(count), [=](sycl::id<1> idx) {
            const std::size_t i = idx[0];
            out[i] = maxLane128(a[i], b[i], trits);
        });
    }
}
#else
inline constexpr bool SYCL_LANE_KERNELS_ENABLED = false;
#endif

} // namespace gpu
} // namespace backend
} // namespace sandbox

#endif // TERNARY_GPU_KERNELS_H
