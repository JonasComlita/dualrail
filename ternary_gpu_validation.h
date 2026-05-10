// =============================================================================
// ternary_gpu_validation.h - shared CUDA/SYCL raw lane conformance helpers
// =============================================================================
//
// Test-only utilities for Phase 5A. Production lane semantics remain in
// ternary_backend.h and ternary_kernel.h; this header builds deterministic
// corpora, CPU reference outputs, checksums, and tuning_results.md rows.

#pragma once
#ifndef TERNARY_GPU_VALIDATION_H
#define TERNARY_GPU_VALIDATION_H

#include "ternary_backend.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {
namespace backend {
namespace gpu_validation {

enum class Operation : uint8_t {
    Neg,
    Add,
    Sub,
    Compare,
    Min,
    Max,
};

struct LaneCase {
    const char* name;
    int trits;
    bool raw128;
};

struct RunRecord {
    std::string backend;
    std::string device;
    std::string compiler;
    std::string operation;
    std::string width;
    std::size_t count = 0;
    std::string launchSize;
    std::string checksum;
    long long runtimeUs = 0;
    std::string execution;
    std::string result;
};

inline const std::vector<Operation>& operations() {
    static const std::vector<Operation> ops = {
        Operation::Neg,
        Operation::Add,
        Operation::Sub,
        Operation::Compare,
        Operation::Min,
        Operation::Max,
    };
    return ops;
}

inline const std::vector<LaneCase>& laneCases() {
    static const std::vector<LaneCase> cases = {
        {"L1", 1, false},
        {"L5", 5, false},
        {"L10", 10, false},
        {"L20", 20, false},
        {"L40", 40, true},
        {"L50", 50, true},
    };
    return cases;
}

inline const char* operationName(Operation op) {
    switch (op) {
        case Operation::Neg: return "neg";
        case Operation::Add: return "add";
        case Operation::Sub: return "sub";
        case Operation::Compare: return "compare";
        case Operation::Min: return "min";
        case Operation::Max: return "max";
    }
    return "unknown";
}

inline bool compareOutput(Operation op) {
    return op == Operation::Compare;
}

inline uint32_t mixSeed(uint32_t state) {
    return state * 1664525u + 1013904223u;
}

inline uint64_t validRaw64(int trits, std::size_t index, uint32_t salt) {
    uint64_t raw = 0;
    uint32_t state = static_cast<uint32_t>(index + 1U) ^ salt;
    for (int i = 0; i < trits; ++i) {
        state = mixSeed(state);
        raw = setPair64(raw, i, static_cast<uint8_t>(state % 3U));
    }
    return raw;
}

inline RawUInt128 validRaw128(int trits, std::size_t index, uint32_t salt) {
    RawUInt128 raw{};
    uint32_t state = static_cast<uint32_t>(index + 1U) ^ salt;
    for (int i = 0; i < trits; ++i) {
        state = mixSeed(state);
        raw = setPair128(raw, i, static_cast<uint8_t>(state % 3U));
    }
    return raw;
}

inline uint64_t withPaddingBit64(uint64_t raw, int trits) {
    const int usedBits = 2 * trits;
    if (usedBits >= 64) return raw;
    return raw | (1ULL << usedBits);
}

inline RawUInt128 withPaddingBit128(RawUInt128 raw, int trits) {
    const int usedBits = 2 * trits;
    if (usedBits >= 128) return raw;
    if (usedBits < 64) {
        raw.lo |= (1ULL << usedBits);
    } else {
        raw.hi |= (1ULL << (usedBits - 64));
    }
    return raw;
}

inline void fillRaw64Inputs(
    std::vector<uint64_t>& a,
    std::vector<uint64_t>& b,
    int trits) {

    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = validRaw64(trits, i, 0x13579BDFu);
        b[i] = validRaw64(trits, i, 0x2468ACE0u);
    }

    if (a.size() > 2) a[a.size() / 3] = invalidLane64(trits);
    if (a.size() > 3) a[a.size() - 1] = withPaddingBit64(a[a.size() - 1], trits);
    if (b.size() > 4) b[b.size() / 2] = setPair64(b[b.size() / 2], 0, 0x3U);
    if (b.size() > 5) b[b.size() - 2] = withPaddingBit64(b[b.size() - 2], trits);
}

inline void fillRaw128Inputs(
    std::vector<RawUInt128>& a,
    std::vector<RawUInt128>& b,
    int trits) {

    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = validRaw128(trits, i, 0x13579BDFu);
        b[i] = validRaw128(trits, i, 0x2468ACE0u);
    }

    if (a.size() > 2) a[a.size() / 3] = invalidLane128(trits);
    if (a.size() > 3) a[a.size() - 1] = withPaddingBit128(a[a.size() - 1], trits);
    if (b.size() > 4) b[b.size() / 2] = setPair128(b[b.size() / 2], 0, 0x3U);
    if (b.size() > 5) b[b.size() - 2] = withPaddingBit128(b[b.size() - 2], trits);
}

inline std::vector<uint64_t> expectedRaw64(
    Operation op,
    const std::vector<uint64_t>& a,
    const std::vector<uint64_t>& b,
    int trits) {

    std::vector<uint64_t> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        switch (op) {
            case Operation::Neg: out[i] = negLane64(a[i], trits); break;
            case Operation::Add: out[i] = addLane64(a[i], b[i], trits); break;
            case Operation::Sub: out[i] = subLane64(a[i], b[i], trits); break;
            case Operation::Min: out[i] = minLane64(a[i], b[i], trits); break;
            case Operation::Max: out[i] = maxLane64(a[i], b[i], trits); break;
            case Operation::Compare: out[i] = 0; break;
        }
    }
    return out;
}

inline std::vector<RawUInt128> expectedRaw128(
    Operation op,
    const std::vector<RawUInt128>& a,
    const std::vector<RawUInt128>& b,
    int trits) {

    std::vector<RawUInt128> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        switch (op) {
            case Operation::Neg: out[i] = negLane128(a[i], trits); break;
            case Operation::Add: out[i] = addLane128(a[i], b[i], trits); break;
            case Operation::Sub: out[i] = subLane128(a[i], b[i], trits); break;
            case Operation::Min: out[i] = minLane128(a[i], b[i], trits); break;
            case Operation::Max: out[i] = maxLane128(a[i], b[i], trits); break;
            case Operation::Compare: out[i] = RawUInt128{}; break;
        }
    }
    return out;
}

inline std::vector<uint8_t> expectedCompare64(
    const std::vector<uint64_t>& a,
    const std::vector<uint64_t>& b,
    int trits) {

    std::vector<uint8_t> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane64(a[i], b[i], trits)));
    }
    return out;
}

inline std::vector<uint8_t> expectedCompare128(
    const std::vector<RawUInt128>& a,
    const std::vector<RawUInt128>& b,
    int trits) {

    std::vector<uint8_t> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = static_cast<uint8_t>(compareResultLane1Raw(compareLane128(a[i], b[i], trits)));
    }
    return out;
}

inline bool sameRaw128(RawUInt128 a, RawUInt128 b) {
    return a.lo == b.lo && a.hi == b.hi;
}

inline bool verifyRaw64(
    Operation op,
    const std::vector<uint64_t>& out,
    const std::vector<uint8_t>& cmp,
    const std::vector<uint64_t>& expected,
    const std::vector<uint8_t>& expectedCmp,
    std::string& message) {

    if (compareOutput(op)) {
        for (std::size_t i = 0; i < expectedCmp.size(); ++i) {
            if (cmp[i] != expectedCmp[i]) {
                message = "mismatch at " + std::to_string(i);
                return false;
            }
        }
    } else {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (out[i] != expected[i]) {
                message = "mismatch at " + std::to_string(i);
                return false;
            }
        }
    }
    message = "pass";
    return true;
}

inline bool verifyRaw128(
    Operation op,
    const std::vector<RawUInt128>& out,
    const std::vector<uint8_t>& cmp,
    const std::vector<RawUInt128>& expected,
    const std::vector<uint8_t>& expectedCmp,
    std::string& message) {

    if (compareOutput(op)) {
        for (std::size_t i = 0; i < expectedCmp.size(); ++i) {
            if (cmp[i] != expectedCmp[i]) {
                message = "mismatch at " + std::to_string(i);
                return false;
            }
        }
    } else {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (!sameRaw128(out[i], expected[i])) {
                message = "mismatch at " + std::to_string(i);
                return false;
            }
        }
    }
    message = "pass";
    return true;
}

inline uint64_t fnv1a(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint64_t checksumRaw64(
    Operation op,
    const std::vector<uint64_t>& out,
    const std::vector<uint8_t>& cmp) {

    if (compareOutput(op)) {
        return fnv1a(cmp.data(), cmp.size() * sizeof(uint8_t));
    }
    return fnv1a(out.data(), out.size() * sizeof(uint64_t));
}

inline uint64_t checksumRaw128(
    Operation op,
    const std::vector<RawUInt128>& out,
    const std::vector<uint8_t>& cmp) {

    if (compareOutput(op)) {
        return fnv1a(cmp.data(), cmp.size() * sizeof(uint8_t));
    }
    return fnv1a(out.data(), out.size() * sizeof(RawUInt128));
}

inline std::string hex64(uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return oss.str();
}

inline std::string compilerDescription() {
    std::ostringstream oss;
#if defined(__CUDACC__)
    oss << "nvcc " << __CUDACC_VER_MAJOR__ << "." << __CUDACC_VER_MINOR__;
#elif defined(__INTEL_LLVM_COMPILER)
    oss << "IntelLLVM " << __INTEL_LLVM_COMPILER;
#elif defined(__clang__)
    oss << "clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#elif defined(__GNUC__)
    oss << "gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
    oss << "MSVC " << _MSC_VER;
#else
    oss << "unknown";
#endif
    return oss.str();
}

inline void writeMarkdownHeader(std::ofstream& out, const std::string& title) {
    out << "\n## " << title << "\n\n";
    out << "| Backend | Device | Compiler | Operation | Width | Count | Block/Local Size | Checksum | Runtime (us) | Execution | Result |\n";
    out << "| ------- | ------ | -------- | --------- | ----- | -----: | ---------------- | -------- | -----------: | --------- | ------ |\n";
}

inline void writeMarkdownRow(std::ofstream& out, const RunRecord& row) {
    out << "| " << row.backend
        << " | `" << row.device
        << "` | `" << row.compiler
        << "` | `" << row.operation
        << "` | `" << row.width
        << "` | " << row.count
        << " | `" << row.launchSize
        << "` | `" << row.checksum
        << "` | " << row.runtimeUs
        << " | " << row.execution
        << " | " << row.result
        << " |\n";
}

} // namespace gpu_validation
} // namespace backend
} // namespace sandbox

#endif // TERNARY_GPU_VALIDATION_H
