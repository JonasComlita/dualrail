// =============================================================================
// ternary_device_allocators.h - optional VMStateAllocator device backends
// =============================================================================
//
// Plain C++ builds see only the capability flags. CUDA/SYCL headers are included
// only when the corresponding opt-in macro is defined.

#pragma once
#ifndef TERNARY_DEVICE_ALLOCATORS_H
#define TERNARY_DEVICE_ALLOCATORS_H

#include "ternary_vm_state.h"

#include <cstddef>
#include <stdexcept>

#if defined(TERNARY_ENABLE_CUDA)
#include <cuda_runtime.h>
#endif

#if defined(TERNARY_ENABLE_SYCL)
#include <sycl/sycl.hpp>
#endif

namespace sandbox {
namespace vm {

#if defined(TERNARY_ENABLE_CUDA)
inline constexpr bool CUDA_VM_ALLOCATOR_ENABLED = true;

struct CudaManagedVMStateAllocator final : VMStateAllocator {
    TernaryValue* allocateDataWords(int capacity) override {
        if (capacity <= 0) return nullptr;
        TernaryValue* words = nullptr;
        const cudaError_t err = cudaMallocManaged(
            reinterpret_cast<void**>(&words),
            sizeof(TernaryValue) * static_cast<std::size_t>(capacity));
        if (err != cudaSuccess) throw std::runtime_error("cudaMallocManaged failed for VM data memory");
        return words;
    }

    void deallocateDataWords(TernaryValue* words) override {
        if (words != nullptr) cudaFree(words);
    }

    TritWord27* allocateInstructionWords(int capacity) override {
        if (capacity <= 0) return nullptr;
        TritWord27* words = nullptr;
        const cudaError_t err = cudaMallocManaged(
            reinterpret_cast<void**>(&words),
            sizeof(TritWord27) * static_cast<std::size_t>(capacity));
        if (err != cudaSuccess) throw std::runtime_error("cudaMallocManaged failed for VM instruction memory");
        return words;
    }

    void deallocateInstructionWords(TritWord27* words) override {
        if (words != nullptr) cudaFree(words);
    }
};
#else
inline constexpr bool CUDA_VM_ALLOCATOR_ENABLED = false;
#endif

#if defined(TERNARY_ENABLE_SYCL)
inline constexpr bool SYCL_VM_ALLOCATOR_ENABLED = true;

struct SyclSharedVMStateAllocator final : VMStateAllocator {
    explicit SyclSharedVMStateAllocator(sycl::queue& q) : queue(&q) {}

    TernaryValue* allocateDataWords(int capacity) override {
        if (capacity <= 0) return nullptr;
        TernaryValue* words = sycl::malloc_shared<TernaryValue>(
            static_cast<std::size_t>(capacity),
            *queue);
        if (words == nullptr) throw std::runtime_error("sycl::malloc_shared failed for VM data memory");
        return words;
    }

    void deallocateDataWords(TernaryValue* words) override {
        if (words != nullptr) sycl::free(words, *queue);
    }

    TritWord27* allocateInstructionWords(int capacity) override {
        if (capacity <= 0) return nullptr;
        TritWord27* words = sycl::malloc_shared<TritWord27>(
            static_cast<std::size_t>(capacity),
            *queue);
        if (words == nullptr) throw std::runtime_error("sycl::malloc_shared failed for VM instruction memory");
        return words;
    }

    void deallocateInstructionWords(TritWord27* words) override {
        if (words != nullptr) sycl::free(words, *queue);
    }

    sycl::queue* queue = nullptr;
};
#else
inline constexpr bool SYCL_VM_ALLOCATOR_ENABLED = false;
#endif

} // namespace vm
} // namespace sandbox

#endif // TERNARY_DEVICE_ALLOCATORS_H
