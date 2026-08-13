// =============================================================================
// ternary_vm_state.h  —  Ternary VM State: Register File and Memory Model
// =============================================================================
//
// Phase 2 of the Ternary VM build plan. Defines all mutable state containers
// that the Phase 3 dispatcher will operate on. No execution logic lives here.
//
// Dependencies:
//   ternary_math.h   — LongTriple, TrapCode (data type)
//   ternary_isa.h    — TritWord27, TrapCode, register constants, REG_COUNT
//
// Architecture: Harvard
//   Instruction memory (TernaryInstructionMemory) is physically separate from
//   data memory (TernaryMemory). The PC indexes into instruction memory only.
//   A LOAD/STORE instruction cannot read or write instruction memory.
//   This matches the eventual FPGA design where IMEM and DMEM are separate
//   block RAM instances with independent address buses.
//
// Word Addressing:
//   Both memories are word-addressed. One address = one LongTriple (50 trits,
//   128-bit host storage) for data memory, or one TritWord27 (27 trits,
//   64-bit host storage) for instruction memory. There is no byte addressing.
//
// Memory Fault Model:
//   Out-of-range LOAD/STORE returns {LongTriple{0}, MemFaultCode::OUT_OF_RANGE}
//   and sets a fault flag. The dispatcher reads the flag and raises
//   TRAP_MEM_FAULT. Memory itself does not touch the register file.
//
// Trap Architecture (Decision 3 from ternary_isa.h):
//   r27 (VMState::trap_reg) is the dedicated trap register, outside the
//   general 27-register file. When a fault occurs:
//     1. VMState::status is set to VMStatus::TRAPPED.
//     2. VMState::trap_reg is written as a T5 fault record:
//        trit[0] = fault_valid, trit[1] = fault_class.
//   Trap detection: check status == VMStatus::TRAPPED.
//   Trap identification: check fault_valid, then decode fault_class.
//   This separation keeps no-fault distinct from the zero-valued
//   fault_class; fault_valid is the explicit guard.
//   does not conflict with TRAP_MEM_FAULT detection — VMStatus is the guard.
//
// Stack Convention:
//   r26 (SP) is initialized by reset() to the top of data memory (DMEM_SIZE-1).
//   The stack grows downward: PUSH = STORE then SP--, POP = SP++ then LOAD.
//   The assembler and dispatcher both follow this convention.
//
// =============================================================================

#pragma once
#ifndef TERNARY_VM_STATE_H
#define TERNARY_VM_STATE_H

// Suppress unused-variable warning in ternary_math.h (not our code).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "ternary_math.h"
#pragma GCC diagnostic pop
#include "ternary_isa.h"
#include "ternary_lanes.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <cstdio>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sandbox {
namespace vm {

using namespace isa;  // bring in Opcode, TrapCode, REG_COUNT, R0_ZERO, etc.

namespace detail {

inline bool syncFileToStableStorage(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring native = path.wstring();
    const int fd = _wopen(native.c_str(), _O_RDWR | _O_BINARY);
    if (fd < 0) return false;
    const bool ok = _commit(fd) == 0;
    _close(fd);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

} // namespace detail

// =============================================================================
// SECTION 1 — Memory Size Constants
// =============================================================================

// Default instruction memory capacity: 4096 TritWord27 instruction words.
// Each word is 27 trits (64-bit host storage). 4096 words = sufficient for
// any realistic test program or benchmark.
static constexpr int DEFAULT_IMEM_SIZE = 4096;

// Default data memory capacity: 65536 LongTriple words.
// Each word is 50 trits (128-bit host storage).
// Range of I-type imm16 (±21,523,360) far exceeds this — intentional.
// The memory can be resized at construction for large simulations.
static constexpr int DEFAULT_DMEM_SIZE = 1000000;

static constexpr int STORAGE_BLOCK_WORDS =
    architecture::v2::STORAGE_BLOCK_WORDS;
static constexpr int MMU_PAGE_WORDS =
    architecture::v2::BASE_PAGE_WORDS;
static constexpr int MMU_SUPERPAGE_WORDS =
    architecture::v2::SUPERPAGE_WORDS;
static constexpr int OS_CLUSTER_WORDS = 4096;
static constexpr int SPARSE_VM_PAGE_WORDS = OS_CLUSTER_WORDS;
static constexpr int SPARSE_MEMORY_DENSE_LIMIT_WORDS = 4 * 1024 * 1024;
static constexpr std::int64_t PRODUCTION_RAM_WORD_BYTES = 16;

struct ProductionProfile {
    int cores = 2;
    int ram_words = 0;
    int instruction_words = 0;
    int disk_blocks = 0;
    int framebuffer_words = 0;
    int framebuffer_width = 1280;
    int framebuffer_height = 720;
    int max_processes = 100;
    int max_files = 2048;
    int max_windows = 100;
    int hardware_page_words = MMU_PAGE_WORDS;
    int cluster_words = OS_CLUSTER_WORDS;
    std::int64_t ram_bytes = 4LL * 1024LL * 1024LL * 1024LL;
    std::int64_t disk_bytes = 64LL * 1024LL * 1024LL * 1024LL;

    [[nodiscard]] static ProductionProfile minimum() {
        ProductionProfile profile;
        profile.ram_words = static_cast<int>(
            profile.ram_bytes / PRODUCTION_RAM_WORD_BYTES);
        profile.instruction_words = profile.ram_words;
        const std::int64_t block_bytes =
            static_cast<std::int64_t>(STORAGE_BLOCK_WORDS) *
            static_cast<std::int64_t>(sizeof(long long));
        profile.disk_blocks = static_cast<int>((profile.disk_bytes + block_bytes - 1) / block_bytes);
        profile.framebuffer_words = profile.framebuffer_width * profile.framebuffer_height;
        return profile;
    }
};

[[nodiscard]] inline int clustersForWords(int words, int cluster_words = OS_CLUSTER_WORDS) {
    if (words <= 0 || cluster_words <= 0) return 0;
    return (words + cluster_words - 1) / cluster_words;
}

// =============================================================================
// SECTION 2 — Memory Fault Code
// =============================================================================
// Returned inline by load/store to let the dispatcher decide how to handle
// faults without the memory model touching the register file.

enum class MemFaultCode : uint8_t {
    OK            = 0,
    OUT_OF_RANGE  = 1,  // Address outside [0, size-1]
};

// =============================================================================
// SECTION 3 — Trap Encoding Helpers
// =============================================================================
// A trap value stored in r27 is a T5 fault record.
// trit[0] is fault_valid; trit[1] is fault_class.

[[nodiscard]] inline bool isNumericMode(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::T1:
        case TernaryMode::T5:
        case TernaryMode::T10:
        case TernaryMode::T20:
        case TernaryMode::T40:
        case TernaryMode::T50:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline bool isLaneMode(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::L1:
        case TernaryMode::L5:
        case TernaryMode::L10:
        case TernaryMode::L20:
        case TernaryMode::L40:
        case TernaryMode::L50:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline int modeTritWidth(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::T1:  case TernaryMode::L1:  return 1;
        case TernaryMode::T5:  case TernaryMode::L5:  return 5;
        case TernaryMode::T10: case TernaryMode::L10: return 10;
        case TernaryMode::T20: case TernaryMode::L20: return 20;
        case TernaryMode::T40: case TernaryMode::L40: return 40;
        case TernaryMode::T50: case TernaryMode::L50: return 50;
    }
    return 0;
}

[[nodiscard]] inline TernaryMode matchingNumericMode(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::T1:  case TernaryMode::L1:  return TernaryMode::T1;
        case TernaryMode::T5:  case TernaryMode::L5:  return TernaryMode::T5;
        case TernaryMode::T10: case TernaryMode::L10: return TernaryMode::T10;
        case TernaryMode::T20: case TernaryMode::L20: return TernaryMode::T20;
        case TernaryMode::T40: case TernaryMode::L40: return TernaryMode::T40;
        case TernaryMode::T50: case TernaryMode::L50: return TernaryMode::T50;
    }
    return TernaryMode::T50;
}

[[nodiscard]] inline TernaryMode matchingLaneMode(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::T1:  case TernaryMode::L1:  return TernaryMode::L1;
        case TernaryMode::T5:  case TernaryMode::L5:  return TernaryMode::L5;
        case TernaryMode::T10: case TernaryMode::L10: return TernaryMode::L10;
        case TernaryMode::T20: case TernaryMode::L20: return TernaryMode::L20;
        case TernaryMode::T40: case TernaryMode::L40: return TernaryMode::L40;
        case TernaryMode::T50: case TernaryMode::L50: return TernaryMode::L50;
    }
    return TernaryMode::L50;
}

template<typename Lane>
[[nodiscard]] inline bool laneIsZero(Lane lane) {
    if (!lane.isValid()) return false;
    for (int i = 0; i < Lane::trits; ++i) {
        if (lane.tritAt(i) != 0) return false;
    }
    return true;
}

struct TernaryValue {
    TernaryMode mode = TernaryMode::T40;
    UInt128 bits = 0;

    [[nodiscard]] static TernaryValue fromT1(T1 v) { return {TernaryMode::T1, v.data}; }
    [[nodiscard]] static TernaryValue fromT5(T5 v) { return {TernaryMode::T5, v.data}; }
    [[nodiscard]] static TernaryValue fromT10(T10 v) { return {TernaryMode::T10, v.data}; }
    [[nodiscard]] static TernaryValue fromT20(T20 v) { return {TernaryMode::T20, v.data}; }
    [[nodiscard]] static TernaryValue fromTriple(Triple v) { return {TernaryMode::T40, v.data}; }
    [[nodiscard]] static TernaryValue fromLongTriple(LongTriple v) { return {TernaryMode::T50, v.data}; }
    // L-mode factories: store lane payload natively in 2-bit-per-trit format.
    [[nodiscard]] static TernaryValue fromL1(TritLane1 v) {
        return {TernaryMode::L1, UInt128{v.rawForKernel()}};
    }
    [[nodiscard]] static TernaryValue fromL5(TritLane5 v) {
        return {TernaryMode::L5, UInt128{v.rawForKernel()}};
    }
    [[nodiscard]] static TernaryValue fromL10(TritLane10 v) {
        return {TernaryMode::L10, UInt128{v.rawForKernel()}};
    }
    [[nodiscard]] static TernaryValue fromL20(TritLane20 v) {
        return {TernaryMode::L20, UInt128{v.rawForKernel()}};
    }
    [[nodiscard]] static TernaryValue fromL40(TritLane40 v) {
        return {TernaryMode::L40, v.rawForKernel()};
    }
    [[nodiscard]] static TernaryValue fromL50(TritLane50 v) {
        return {TernaryMode::L50, v.rawForKernel()};
    }

    [[nodiscard]] static TernaryValue invalid(TernaryMode m) {
        switch (m) {
            case TernaryMode::T1:  return fromT1(T1{T1::INVALID_DATA});
            case TernaryMode::T5:  return fromT5(T5{T5::INVALID_DATA});
            case TernaryMode::T10: return fromT10(T10{59049u});
            case TernaryMode::T20: return fromT20(T20{3486784401u});
            case TernaryMode::T40: return fromTriple(Triple{12157665459056928801ULL});
            case TernaryMode::T50: return fromLongTriple(LongTriple{native_ops::detail::pow3UInt128(50)});
            case TernaryMode::L1:  return fromL1(TritLane1::invalid());
            case TernaryMode::L5:  return fromL5(TritLane5::invalid());
            case TernaryMode::L10: return fromL10(TritLane10::invalid());
            case TernaryMode::L20: return fromL20(TritLane20::invalid());
            case TernaryMode::L40: return fromL40(TritLane40::invalid());
            case TernaryMode::L50: return fromL50(TritLane50::invalid());
        }
        return fromLongTriple(LongTriple{LongTriple::OVERFLOW_DATA});
    }

    [[nodiscard]] static TernaryValue zero(TernaryMode m = TernaryMode::T40) {
        switch (m) {
            case TernaryMode::T1:  return fromT1(native_ops::fromIntT1(0));
            case TernaryMode::T5:  return fromT5(native_ops::fromIntT5(0));
            case TernaryMode::T10: return fromT10(native_ops::fromIntT10(0));
            case TernaryMode::T20: return fromT20(native_ops::fromIntT20(0));
            case TernaryMode::T40: return fromTriple(native_ops::fromIntT40(0));
            case TernaryMode::T50: return fromLongTriple(native_ops::fromInt(0));
            case TernaryMode::L1:  { TritLane1 lane; lane.fill(0); return fromL1(lane); }
            case TernaryMode::L5:  { TritLane5 lane; lane.fill(0); return fromL5(lane); }
            case TernaryMode::L10: { TritLane10 lane; lane.fill(0); return fromL10(lane); }
            case TernaryMode::L20: { TritLane20 lane; lane.fill(0); return fromL20(lane); }
            case TernaryMode::L40: { TritLane40 lane; lane.fill(0); return fromL40(lane); }
            case TernaryMode::L50: { TritLane50 lane; lane.fill(0); return fromL50(lane); }
        }
        return fromLongTriple(LongTriple{0});
    }

    [[nodiscard]] T1 asT1() const { return T1{static_cast<uint8_t>(bits.toUint64())}; }
    [[nodiscard]] T5 asT5() const { return T5{static_cast<uint8_t>(bits.toUint64())}; }
    [[nodiscard]] T10 asT10() const { return T10{static_cast<uint16_t>(bits.toUint64())}; }
    [[nodiscard]] T20 asT20() const { return T20{static_cast<uint32_t>(bits.toUint64())}; }
    [[nodiscard]] Triple asTriple() const { return Triple{bits.toUint64()}; }
    [[nodiscard]] LongTriple asLongTripleRaw() const { return LongTriple{bits}; }
    // L-mode accessors: convert positional base-3 back to lane format.
    [[nodiscard]] TritLane1 asL1() const {
        return TritLane1::fromRawForKernel(static_cast<uint8_t>(bits.toUint64()));
    }
    [[nodiscard]] TritLane5 asL5() const {
        return TritLane5::fromRawForKernel(static_cast<uint16_t>(bits.toUint64()));
    }
    [[nodiscard]] TritLane10 asL10() const {
        return TritLane10::fromRawForKernel(static_cast<uint32_t>(bits.toUint64()));
    }
    [[nodiscard]] TritLane20 asL20() const {
        return TritLane20::fromRawForKernel(static_cast<uint64_t>(bits.toUint64()));
    }
    [[nodiscard]] TritLane40 asL40() const {
        return TritLane40::fromRawForKernel(bits);
    }
    [[nodiscard]] TritLane50 asL50() const {
        return TritLane50::fromRawForKernel(bits);
    }

    [[nodiscard]] bool isInvalid() const {
        switch (mode) {
            case TernaryMode::T1:  return native_ops::isInvalid(asT1());
            case TernaryMode::T5:  return native_ops::isInvalid(asT5());
            case TernaryMode::T10: return native_ops::isInvalid(asT10());
            case TernaryMode::T20: return native_ops::isInvalid(asT20());
            case TernaryMode::T40: return native_ops::isInvalid(asTriple());
            case TernaryMode::T50: return native_ops::isInvalid(asLongTripleRaw());
            // L-modes store native lane data. Check if lane is valid.
            case TernaryMode::L1:  return !asL1().isValid();
            case TernaryMode::L5:  return !asL5().isValid();
            case TernaryMode::L10: return !asL10().isValid();
            case TernaryMode::L20: return !asL20().isValid();
            case TernaryMode::L40: return !asL40().isValid();
            case TernaryMode::L50: return !asL50().isValid();
        }
        return true;
    }

    [[nodiscard]] bool isZero() const {
        switch (mode) {
            case TernaryMode::T1:  return asT1().isZero();
            case TernaryMode::T5:  return asT5().isZero();
            case TernaryMode::T10: return asT10().isZero();
            case TernaryMode::T20: return asT20().isZero();
            case TernaryMode::T40: return asTriple().isZero();
            case TernaryMode::T50: return asLongTripleRaw().isZero();
            // L-modes store native lane data. Check if all lane trits are zero.
            case TernaryMode::L1:  return laneIsZero(asL1());
            case TernaryMode::L5:  return laneIsZero(asL5());
            case TernaryMode::L10: return laneIsZero(asL10());
            case TernaryMode::L20: return laneIsZero(asL20());
            case TernaryMode::L40: return laneIsZero(asL40());
            case TernaryMode::L50: return laneIsZero(asL50());
        }
        return false;
    }

    [[nodiscard]] LongTriple toLongTriple() const {
        if (!isNumericMode(mode)) return LongTriple{LongTriple::OVERFLOW_DATA};
        if (isInvalid()) return LongTriple{LongTriple::OVERFLOW_DATA};
        switch (mode) {
            case TernaryMode::T1:  return native_ops::toLongTriple(asT1());
            case TernaryMode::T5:  return native_ops::toLongTriple(asT5());
            case TernaryMode::T10: return native_ops::toLongTriple(asT10());
            case TernaryMode::T20: return native_ops::toLongTriple(asT20());
            case TernaryMode::T40: return native_ops::toLongTriple(asTriple());
            case TernaryMode::T50: return asLongTripleRaw();
            default: break;
        }
        return LongTriple{LongTriple::OVERFLOW_DATA};
    }

    // Structural equality: same mode AND same bit representation.
    // Required by IR constant folding and CSE passes.
    [[nodiscard]] bool operator==(const TernaryValue& other) const {
        return mode == other.mode && bits == other.bits;
    }
    [[nodiscard]] bool operator!=(const TernaryValue& other) const {
        return !(*this == other);
    }
};

[[nodiscard]] inline TernaryValue laneToNumeric(TernaryValue val) {
    switch (val.mode) {
        case TernaryMode::L1:  return TernaryValue::fromT1(sandbox::fromLane(val.asL1()));
        case TernaryMode::L5:  return TernaryValue::fromT5(sandbox::fromLane(val.asL5()));
        case TernaryMode::L10: return TernaryValue::fromT10(sandbox::fromLane(val.asL10()));
        case TernaryMode::L20: return TernaryValue::fromT20(sandbox::fromLane(val.asL20()));
        case TernaryMode::L40: return TernaryValue::fromTriple(sandbox::fromLane(val.asL40()));
        case TernaryMode::L50: return TernaryValue::fromLongTriple(sandbox::fromLane(val.asL50()));
        default: return val;
    }
}

[[nodiscard]] inline TernaryValue numericToLane(TernaryValue val, TernaryMode targetMode) {
    switch (targetMode) {
        case TernaryMode::L1:  return TernaryValue::fromL1(sandbox::toLane(val.asT1()));
        case TernaryMode::L5:  return TernaryValue::fromL5(sandbox::toLane(val.asT5()));
        case TernaryMode::L10: return TernaryValue::fromL10(sandbox::toLane(val.asT10()));
        case TernaryMode::L20: return TernaryValue::fromL20(sandbox::toLane(val.asT20()));
        case TernaryMode::L40: return TernaryValue::fromL40(sandbox::toLane(val.asTriple()));
        case TernaryMode::L50: return TernaryValue::fromL50(sandbox::toLane(val.asLongTripleRaw()));
        default: return val;
    }
}

[[nodiscard]] inline TernaryValue convertValue(TernaryValue value, TernaryMode target) {
    if (value.mode == target) return value;
    if (value.isInvalid()) return TernaryValue::invalid(target);

    // If source is a lane mode, convert it to its matching numeric mode first.
    if (isLaneMode(value.mode)) {
        value = laneToNumeric(value);
        if (value.isInvalid()) return TernaryValue::invalid(target);
        if (value.mode == target) return value;
    }

    // If target is a lane mode, convert the numeric value to the matching numeric mode of target,
    // and then convert that to the target lane mode.
    if (isLaneMode(target)) {
        TernaryMode targetNumeric = matchingNumericMode(target);
        TernaryValue numericVal = convertValue(value, targetNumeric);
        if (numericVal.isInvalid()) return TernaryValue::invalid(target);
        return numericToLane(numericVal, target);
    }

    LongTriple canonical = value.toLongTriple();
    switch (target) {
        case TernaryMode::T1:
            return TernaryValue::fromT1(native_ops::fromIntT1(native_ops::toLongLong(canonical)));
        case TernaryMode::T5:
            return TernaryValue::fromT5(native_ops::fromIntT5(native_ops::toLongLong(canonical)));
        case TernaryMode::T10:
            return TernaryValue::fromT10(native_ops::toT10(canonical));
        case TernaryMode::T20:
            return TernaryValue::fromT20(native_ops::toT20(canonical));
        case TernaryMode::T40:
            return TernaryValue::fromTriple(native_ops::toT40(canonical));
        case TernaryMode::T50:
            return TernaryValue::fromLongTriple(canonical);
        default:
            break;
    }
    return TernaryValue::fromLongTriple(LongTriple{LongTriple::OVERFLOW_DATA});
}


[[nodiscard]] inline int8_t readStoredTrit(TernaryValue val, int pos) {
    if (pos < 0 || val.isInvalid()) return 0;
    switch (val.mode) {
        case TernaryMode::T1:
            return pos == 0 ? static_cast<int8_t>(native_ops::toLongLong(val.asT1())) : 0;
        case TernaryMode::T5: {
            if (pos >= T5::TRITS) return 0;
            uint8_t temp = val.asT5().data;
            for (int i = 0; i < pos; ++i) temp = static_cast<uint8_t>(temp / 3);
            return static_cast<int8_t>(temp % 3) - 1;
        }
        case TernaryMode::T10: {
            if (pos >= 10) return 0;
            return val.asT10().unpack()[pos];
        }
        case TernaryMode::T20: {
            if (pos >= 20) return 0;
            return val.asT20().unpack()[pos];
        }
        case TernaryMode::T40: {
            if (pos >= 40) return 0;
            return val.asTriple().unpack()[pos];
        }
        case TernaryMode::T50: {
            if (pos >= 50) return 0;
            return val.asLongTripleRaw().unpack()[pos];
        }
        // L-modes store native lane data — decode trit directly.
        case TernaryMode::L1: {
            if (pos >= 1) return 0;
            return val.asL1().tritAt(pos);
        }
        case TernaryMode::L5: {
            if (pos >= 5) return 0;
            return val.asL5().tritAt(pos);
        }
        case TernaryMode::L10: {
            if (pos >= 10) return 0;
            return val.asL10().tritAt(pos);
        }
        case TernaryMode::L20: {
            if (pos >= 20) return 0;
            return val.asL20().tritAt(pos);
        }
        case TernaryMode::L40: {
            if (pos >= 40) return 0;
            return val.asL40().tritAt(pos);
        }
        case TernaryMode::L50: {
            if (pos >= 50) return 0;
            return val.asL50().tritAt(pos);
        }
    }
    return 0;
}

[[nodiscard]] inline int8_t readStoredTrit(LongTriple val, int pos) {
    if (pos < 0 || pos >= 50 || val.isZero() || val.isSpecial()) return 0;
    return val.unpack()[pos];
}

[[nodiscard]] inline TernaryValue makeFaultRecord(int8_t fault_valid, int8_t fault_class) {
    assert(fault_valid == T_ZER || fault_valid == T_POS);
    assert(fault_class >= T_NEG && fault_class <= T_POS);
    const int encoded = fault_valid + 3 * fault_class;
    return TernaryValue::fromT5(native_ops::fromIntT5(encoded));
}

[[nodiscard]] inline TernaryValue encodeNoTrap() {
    return makeFaultRecord(FAULT_VALID_NONE, T_ZER);
}

[[nodiscard]] inline TernaryValue encodeTrap(TrapCode code) {
    return makeFaultRecord(FAULT_VALID_SET, static_cast<int8_t>(code));
}

[[nodiscard]] inline bool trapValid(TernaryValue val) {
    return readStoredTrit(val, 0) == FAULT_VALID_SET;
}

[[nodiscard]] inline bool trapValid(LongTriple val) {
    return readStoredTrit(val, 0) == FAULT_VALID_SET;
}

[[nodiscard]] inline int8_t readTrit0(TernaryValue val) {
    if (val.isInvalid()) return 0;
    if (isLaneMode(val.mode)) return readStoredTrit(val, 0);
    if (val.mode == TernaryMode::T1) {
        return static_cast<int8_t>(native_ops::toLongLong(val.asT1()));
    }
    return static_cast<int8_t>(native_ops::sign(val.toLongTriple()));
}

[[nodiscard]] inline int8_t readTrit0(LongTriple val) {
    if (val.isZero() || val.isSpecial()) return 0;
    auto trits = val.unpack();
    return trits[0];
}

[[nodiscard]] inline TrapCode decodeTrap(TernaryValue val) {
    if (!trapValid(val)) return TrapCode::TRAP_MEM_FAULT;
    const int8_t t = readStoredTrit(val, 1);
    if (t == T_NEG) return TrapCode::TRAP_DIV_ZERO;
    if (t == T_POS) return TrapCode::TRAP_ILLEGAL_OP;
    return TrapCode::TRAP_MEM_FAULT;
}

[[nodiscard]] inline TrapCode decodeTrap(LongTriple val) {
    if (!trapValid(val)) return TrapCode::TRAP_MEM_FAULT;
    const int8_t t = readStoredTrit(val, 1);
    if (t == T_NEG) return TrapCode::TRAP_DIV_ZERO;
    if (t == T_POS) return TrapCode::TRAP_ILLEGAL_OP;
    return TrapCode::TRAP_MEM_FAULT;
}

[[nodiscard]] inline TernaryValue makeTritResult(int8_t trit_val) {
    assert(trit_val >= -1 && trit_val <= 1);
    return TernaryValue::fromT1(native_ops::fromIntT1(trit_val));
}

// =============================================================================
// SECTION 4 — General-Purpose Register File
// =============================================================================

struct TernaryRegisterFile {
    // Registers r0..r26. r0 is hardwired zero — reads always return
    // a tagged zero; writes are silently discarded. All others are
    // general-purpose, initialized to T40 zero on reset.
    // Every stored word remains T40. view_mode is non-architectural simulator
    // metadata used to reconstruct the view selected by an instruction.
    std::array<TernaryValue, REG_COUNT> reg;
    std::array<TernaryMode, REG_COUNT> view_mode;

    TernaryRegisterFile() { reset(); }

    void reset() {
        for (std::size_t index = 0; index < reg.size(); ++index) {
            reg[index] = TernaryValue::zero(TernaryMode::T40);
            view_mode[index] = TernaryMode::T40;
        }
    }

    // Read register [idx]. r0 always returns exact zero regardless of
    // any prior attempted write. Out-of-range index returns zero silently.
    [[nodiscard]] TernaryValue readPhysical(uint8_t idx) const {
        if (idx == R0_ZERO || idx >= REG_COUNT)
            return TernaryValue::zero(TernaryMode::T40);
        return reg[idx];
    }

    [[nodiscard]] TernaryValue readView(
            uint8_t idx,
            TernaryMode view) const {
        if (idx == R0_ZERO || idx >= REG_COUNT)
            return TernaryValue::zero(view);
        if (view == TernaryMode::T50 || view == TernaryMode::L50) {
            if (idx + 1 >= REG_COUNT)
                return TernaryValue::invalid(view);
            const auto low = reg[idx].asTriple().unpack();
            const auto high = reg[idx + 1].asTriple().unpack();
            std::array<int8_t, 50> wide{};
            for (int trit = 0; trit < 40; ++trit)
                wide[static_cast<std::size_t>(trit)] =
                    low[static_cast<std::size_t>(trit)];
            for (int trit = 0; trit < 10; ++trit)
                wide[static_cast<std::size_t>(40 + trit)] =
                    high[static_cast<std::size_t>(trit)];
            TernaryValue numeric = TernaryValue::fromLongTriple(
                LongTriple::pack(wide));
            return view == TernaryMode::L50
                ? numericToLane(numeric, view)
                : numeric;
        }

        if (view == TernaryMode::T40) return reg[idx];
        return convertValue(reg[idx], view);
    }

    [[nodiscard]] TernaryValue read(uint8_t idx) const {
        if (idx == R0_ZERO || idx >= REG_COUNT)
            return TernaryValue::zero(TernaryMode::T40);
        return readView(idx, view_mode[idx]);
    }

    // Write register [idx]. Writes to r0 are discarded. Writes to
    // r27 or above are discarded (r27 is the separate trap register).
    void write(uint8_t idx, TernaryValue val) {
        if (idx == R0_ZERO) return;   // hardwired zero
        if (idx >= REG_COUNT) return;

        invalidatePairTouching(idx);
        const TernaryMode requested = val.mode;
        if (requested == TernaryMode::T50 ||
            requested == TernaryMode::L50) {
            if (idx + 1 >= REG_COUNT) {
                reg[idx] = TernaryValue::invalid(TernaryMode::T40);
                view_mode[idx] = TernaryMode::T40;
                return;
            }
            invalidatePairTouching(static_cast<uint8_t>(idx + 1));
            TernaryValue numeric = isLaneMode(requested)
                ? laneToNumeric(val)
                : val;
            if (numeric.isInvalid()) {
                reg[idx] = TernaryValue::invalid(TernaryMode::T40);
                reg[idx + 1] =
                    TernaryValue::invalid(TernaryMode::T40);
            } else {
                const auto wide = numeric.asLongTripleRaw().unpack();
                std::array<int8_t, 40> low{};
                std::array<int8_t, 40> high{};
                for (int trit = 0; trit < 40; ++trit)
                    low[static_cast<std::size_t>(trit)] =
                        wide[static_cast<std::size_t>(trit)];
                for (int trit = 0; trit < 10; ++trit)
                    high[static_cast<std::size_t>(trit)] =
                        wide[static_cast<std::size_t>(40 + trit)];
                reg[idx] =
                    TernaryValue::fromTriple(Triple::pack(low));
                reg[idx + 1] =
                    TernaryValue::fromTriple(Triple::pack(high));
            }
            view_mode[idx] = requested;
            view_mode[idx + 1] = TernaryMode::T40;
            return;
        }

        TernaryValue numeric = isLaneMode(requested)
            ? laneToNumeric(val)
            : val;
        reg[idx] = numeric.isInvalid()
            ? TernaryValue::invalid(TernaryMode::T40)
            : convertValue(numeric, TernaryMode::T40);
        view_mode[idx] = requested;
    }

    void write(uint8_t idx, LongTriple val) {
        write(idx, TernaryValue::fromLongTriple(val));
    }

    // Convenience: read the stack pointer and link register.
    [[nodiscard]] TernaryValue readSP() const { return read(R26_SP); }
    [[nodiscard]] TernaryValue readLR() const { return read(R25_LR); }
    void writeSP(TernaryValue val)            { write(R26_SP, val); }
    void writeLR(TernaryValue val)            { write(R25_LR, val); }
    void writeSP(LongTriple val)              { write(R26_SP, val); }
    void writeLR(LongTriple val)              { write(R25_LR, val); }

    // Debug: dump all non-zero registers to a string.
    [[nodiscard]] std::string dump() const {
        std::ostringstream oss;
        for (int i = 0; i < REG_COUNT; ++i) {
            if (!reg[i].isZero()) {
                oss << "  r" << i << ": [non-zero LongTriple]\n";
            }
        }
        return oss.str();
    }

private:
    void invalidatePairTouching(uint8_t idx) {
        if (idx < REG_COUNT &&
            (view_mode[idx] == TernaryMode::T50 ||
             view_mode[idx] == TernaryMode::L50)) {
            view_mode[idx] = TernaryMode::T40;
            if (idx + 1 < REG_COUNT) {
                reg[idx + 1] =
                    TernaryValue::zero(TernaryMode::T40);
                view_mode[idx + 1] = TernaryMode::T40;
            }
        }
        if (idx > 0 &&
            (view_mode[idx - 1] == TernaryMode::T50 ||
             view_mode[idx - 1] == TernaryMode::L50)) {
            view_mode[idx - 1] = TernaryMode::T40;
        }
    }
};

// =============================================================================
// SECTION 5 — Data Memory
// =============================================================================
// Word-addressed flat array of LongTriple values. The VM never addresses
// sub-word (there are no byte load/store instructions).

struct VMStateAllocator {
    virtual ~VMStateAllocator() = default;

    virtual TernaryValue* allocateDataWords(int capacity) = 0;
    virtual void deallocateDataWords(TernaryValue* words) = 0;

    virtual TritWord27* allocateInstructionWords(int capacity) = 0;
    virtual void deallocateInstructionWords(TritWord27* words) = 0;
};

struct HostVMStateAllocator final : VMStateAllocator {
    TernaryValue* allocateDataWords(int capacity) override {
        return capacity > 0 ? new TernaryValue[capacity] : nullptr;
    }

    void deallocateDataWords(TernaryValue* words) override {
        delete[] words;
    }

    TritWord27* allocateInstructionWords(int capacity) override {
        return capacity > 0 ? new TritWord27[capacity] : nullptr;
    }

    void deallocateInstructionWords(TritWord27* words) override {
        delete[] words;
    }
};

inline VMStateAllocator& defaultVMStateAllocator() {
    static HostVMStateAllocator host;
    return host;
}

enum class MemoryBacking : uint8_t {
    Auto,
    Dense,
    Sparse,
};

struct TernaryMemory {
    // Native x64 JITs may use this contract for a guarded dense-word store.
    // The pointers intentionally expose only the state needed to mirror the
    // ordinary dense `store()` bookkeeping; sparse pages and their cache stay
    // on the portable path.  Callers must validate `words`, `capacity`, and
    // `page_count` before writing, exactly as `store()` does.
    struct NativeDenseStoreAccess {
        TernaryValue* words = nullptr;
        int capacity = 0;
        std::uint64_t* write_generation = nullptr;
        std::uint64_t* page_generations = nullptr;
        int page_count = 0;
    };

    TernaryValue* words = nullptr; // Non-null only for dense compatibility mode.
    int capacity = 0;
    VMStateAllocator* allocator = &defaultVMStateAllocator();

    explicit TernaryMemory(int size = DEFAULT_DMEM_SIZE,
                           VMStateAllocator& alloc = defaultVMStateAllocator(),
                           MemoryBacking backing = MemoryBacking::Auto)
        : allocator(&alloc) {
        allocate(size, backing);
        reset();
    }

    ~TernaryMemory() {
        release();
    }

    TernaryMemory(const TernaryMemory& other)
        : allocator(other.allocator),
          write_generation_(other.write_generation_),
          clear_generation_(other.clear_generation_) {
        allocate(other.capacity, other.sparse_ ? MemoryBacking::Sparse : MemoryBacking::Dense);
        try {
            if (!sparse_ && capacity > 0) std::copy(other.words, other.words + capacity, words);
            sparse_pages_ = other.sparse_pages_;
            sparse_page_generations_ = other.sparse_page_generations_;
            dense_page_generations_ = other.dense_page_generations_;
        } catch (...) {
            release();
            throw;
        }
    }

    TernaryMemory& operator=(const TernaryMemory& other) {
        if (this == &other) return *this;
        TernaryMemory tmp(other);  // copy-and-swap for strong exception safety
        swap(tmp);
        return *this;
    }

    TernaryMemory(TernaryMemory&& other) noexcept
        : words(other.words),
          capacity(other.capacity),
          allocator(other.allocator),
          sparse_(other.sparse_),
          sparse_pages_(std::move(other.sparse_pages_)),
          sparse_page_generations_(std::move(other.sparse_page_generations_)),
          dense_page_generations_(std::move(other.dense_page_generations_)),
          write_generation_(other.write_generation_),
          clear_generation_(other.clear_generation_) {
        invalidateSparseCache();
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        other.sparse_ = false;
        other.write_generation_ = 1;
        other.clear_generation_ = 1;
        other.invalidateSparseCache();
    }

    TernaryMemory& operator=(TernaryMemory&& other) noexcept {
        if (this == &other) return *this;
        release();
        words = other.words;
        capacity = other.capacity;
        allocator = other.allocator;
        sparse_ = other.sparse_;
        sparse_pages_ = std::move(other.sparse_pages_);
        sparse_page_generations_ = std::move(other.sparse_page_generations_);
        dense_page_generations_ = std::move(other.dense_page_generations_);
        write_generation_ = other.write_generation_;
        clear_generation_ = other.clear_generation_;
        invalidateSparseCache();
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        other.sparse_ = false;
        other.write_generation_ = 1;
        other.clear_generation_ = 1;
        other.invalidateSparseCache();
        return *this;
    }

    void swap(TernaryMemory& other) noexcept {
        std::swap(words, other.words);
        std::swap(capacity, other.capacity);
        std::swap(allocator, other.allocator);
        std::swap(sparse_, other.sparse_);
        std::swap(sparse_pages_, other.sparse_pages_);
        std::swap(sparse_page_generations_, other.sparse_page_generations_);
        std::swap(dense_page_generations_, other.dense_page_generations_);
        std::swap(write_generation_, other.write_generation_);
        std::swap(clear_generation_, other.clear_generation_);
        invalidateSparseCache();
        other.invalidateSparseCache();
    }

    void allocate(int size, MemoryBacking backing = MemoryBacking::Auto) {
        if (size < 0) throw std::invalid_argument("negative TernaryMemory size");
        capacity = size;
        sparse_ = backing == MemoryBacking::Sparse ||
                  (backing == MemoryBacking::Auto && size > SPARSE_MEMORY_DENSE_LIMIT_WORDS);
        sparse_pages_.clear();
        sparse_page_generations_.clear();
        if (sparse_) {
            words = nullptr;
            dense_page_generations_.clear();
            return;
        }
        words = allocator->allocateDataWords(capacity);
        dense_page_generations_.assign(pageCountForCapacity(capacity), clear_generation_);
    }

    void release() {
        if (words != nullptr) {
            allocator->deallocateDataWords(words);
            words = nullptr;
        }
        sparse_pages_.clear();
        sparse_page_generations_.clear();
        dense_page_generations_.clear();
        invalidateSparseCache();
        capacity = 0;
        sparse_ = false;
    }

    void reset() {
        if (sparse_) {
            sparse_pages_.clear();
            sparse_page_generations_.clear();
            invalidateSparseCache();
        } else if (capacity > 0) {
            std::fill(words, words + capacity, TernaryValue::zero());
            dense_page_generations_.assign(pageCountForCapacity(capacity), clear_generation_);
        }
        noteClear();
    }

    void resize(int new_size, MemoryBacking backing = MemoryBacking::Auto) {
        release();
        allocate(new_size, backing);
        reset();
    }

    bool growTo(int min_size) {
        if (min_size <= capacity) return true;
        if (min_size < 0) return false;
        if (sparse_) {
            capacity = min_size;
            return true;
        }
        if (min_size > SPARSE_MEMORY_DENSE_LIMIT_WORDS) {
            convertDenseToSparse(min_size);
            return true;
        }

        TernaryValue* grown = nullptr;
        try {
            grown = allocator->allocateDataWords(min_size);
        } catch (...) {
            return false;
        }
        if (min_size > 0 && grown == nullptr) return false;

        std::fill(grown, grown + min_size, TernaryValue::zero());
        if (capacity > 0 && words != nullptr) {
            std::copy(words, words + capacity, grown);
            allocator->deallocateDataWords(words);
        }
        words = grown;
        capacity = min_size;
        dense_page_generations_.resize(pageCountForCapacity(capacity), clear_generation_);
        return true;
    }

    // Load one LongTriple word from address [addr].
    // Returns {value, MemFaultCode::OK} on success.
    // Returns {LongTriple{0}, MemFaultCode::OUT_OF_RANGE} on fault.
    [[nodiscard]] std::pair<TernaryValue, MemFaultCode> load(int addr) const {
        if (addr < 0 || addr >= capacity) {
            return {TernaryValue::zero(), MemFaultCode::OUT_OF_RANGE};
        }
        if (!sparse_) return {words[addr], MemFaultCode::OK};
        const int page_index = pageIndex(addr);
        if (cached_sparse_page_index_ == page_index && cached_sparse_page_ != nullptr) {
            return {(*cached_sparse_page_)[static_cast<std::size_t>(pageOffset(addr))],
                    MemFaultCode::OK};
        }
        const auto page = sparse_pages_.find(page_index);
        if (page == sparse_pages_.end()) {
            cached_sparse_page_index_ = -1;
            cached_sparse_page_ = nullptr;
            return {TernaryValue::zero(), MemFaultCode::OK};
        }
        cached_sparse_page_index_ = page_index;
        cached_sparse_page_ = &page->second;
        return {page->second[static_cast<std::size_t>(pageOffset(addr))], MemFaultCode::OK};
    }

    // Store one LongTriple word to address [addr].
    // Returns MemFaultCode::OK on success, OUT_OF_RANGE on fault.
    MemFaultCode store(int addr, TernaryValue val) {
        if (addr < 0 || addr >= capacity) {
            return MemFaultCode::OUT_OF_RANGE;
        }
        const int page_index = pageIndex(addr);
        if (!sparse_) {
            words[addr] = val;
            noteStore(page_index);
            return MemFaultCode::OK;
        }
        auto& page = sparse_pages_[page_index];
        if (page.empty()) page.assign(SPARSE_VM_PAGE_WORDS, TernaryValue::zero());
        page[static_cast<std::size_t>(pageOffset(addr))] = val;
        sparse_page_generations_[page_index] = nextGeneration();
        cached_sparse_page_index_ = page_index;
        cached_sparse_page_ = &page;
        return MemFaultCode::OK;
    }

    MemFaultCode store(int addr, LongTriple val) {
        return store(addr, TernaryValue::fromLongTriple(val));
    }

    [[nodiscard]] bool inRange(int addr) const {
        return addr >= 0 && addr < capacity;
    }

    [[nodiscard]] int size() const { return capacity; }
    [[nodiscard]] bool isSparse() const { return sparse_; }
    [[nodiscard]] std::size_t allocatedPages() const { return sparse_pages_.size(); }
    [[nodiscard]] std::uint64_t generation() const { return write_generation_; }

    // Return writable generation pointers only for dense memory.  The native
    // emitter updates one word and then applies `nextGeneration()` to the
    // corresponding dense page, matching `store()`/`noteStore()` without a
    // C++ helper call.  `densePageGenerations()` repairs a stale vector size
    // before its pointer is published; this is never used for sparse memory.
    [[nodiscard]] NativeDenseStoreAccess nativeDenseStoreAccess() {
        if (sparse_) return {};
        auto& generations = densePageGenerations();
        return {
            words,
            capacity,
            &write_generation_,
            generations.empty() ? nullptr : generations.data(),
            static_cast<int>(generations.size()),
        };
    }

    [[nodiscard]] std::uint64_t pageGenerationForAddress(int addr) const {
        if (addr < 0 || addr >= capacity) return 0;
        return pageGeneration(pageIndex(addr));
    }

    [[nodiscard]] std::uint64_t rangeGeneration(int start_addr, int word_count) const {
        if (word_count <= 0 || capacity <= 0) return generation();
        const int start = std::clamp(start_addr, 0, capacity - 1);
        const int end_exclusive = std::clamp(start_addr + word_count, 0, capacity);
        if (end_exclusive <= start) return generation();
        std::uint64_t newest = clear_generation_;
        const int first_page = pageIndex(start);
        const int last_page = pageIndex(end_exclusive - 1);
        for (int page = first_page; page <= last_page; ++page) {
            newest = std::max(newest, pageGeneration(page));
        }
        return newest;
    }

    // Enumerate only materialized, non-zero words.  This is intentionally
    // sparse-aware so checkpoint and diagnostic writers never scan a
    // production-sized virtual address space one word at a time.
    template <typename Callback>
    void forEachNonZero(Callback&& callback) const {
        const TernaryValue zero = TernaryValue::zero();
        if (!sparse_) {
            for (int address = 0; address < capacity; ++address) {
                if (words[address] != zero) callback(address, words[address]);
            }
            return;
        }
        for (const auto& entry : sparse_pages_) {
            const int base = entry.first * SPARSE_VM_PAGE_WORDS;
            const auto& page = entry.second;
            const int limit = std::min<int>(
                static_cast<int>(page.size()), capacity - base);
            for (int offset = 0; offset < limit; ++offset) {
                const TernaryValue& value = page[static_cast<std::size_t>(offset)];
                if (value != zero) callback(base + offset, value);
            }
        }
    }

private:
    bool sparse_ = false;
    std::unordered_map<int, std::vector<TernaryValue>> sparse_pages_;
    std::unordered_map<int, std::uint64_t> sparse_page_generations_;
    std::vector<std::uint64_t> dense_page_generations_;
    std::uint64_t write_generation_ = 1;
    std::uint64_t clear_generation_ = 1;
    mutable int cached_sparse_page_index_ = -1;
    mutable const std::vector<TernaryValue>* cached_sparse_page_ = nullptr;

    [[nodiscard]] static int pageIndex(int addr) { return addr / SPARSE_VM_PAGE_WORDS; }
    [[nodiscard]] static int pageOffset(int addr) { return addr % SPARSE_VM_PAGE_WORDS; }
    [[nodiscard]] static int pageCountForCapacity(int size) {
        return size <= 0 ? 0 : ((size - 1) / SPARSE_VM_PAGE_WORDS) + 1;
    }

    void invalidateSparseCache() const {
        cached_sparse_page_index_ = -1;
        cached_sparse_page_ = nullptr;
    }

    std::vector<std::uint64_t>& densePageGenerations() {
        if (static_cast<int>(dense_page_generations_.size()) != pageCountForCapacity(capacity)) {
            dense_page_generations_.assign(pageCountForCapacity(capacity), clear_generation_);
        }
        return dense_page_generations_;
    }

    [[nodiscard]] std::uint64_t pageGeneration(int page_index) const {
        if (sparse_) {
            const auto it = sparse_page_generations_.find(page_index);
            return it == sparse_page_generations_.end() ? clear_generation_ : it->second;
        }
        if (page_index < 0 ||
            page_index >= static_cast<int>(dense_page_generations_.size())) {
            return clear_generation_;
        }
        return dense_page_generations_[static_cast<std::size_t>(page_index)];
    }

    std::uint64_t nextGeneration() {
        if (write_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            write_generation_ = 1;
        }
        return ++write_generation_;
    }

    void noteStore(int page_index) {
        auto& generations = densePageGenerations();
        if (page_index >= 0 && page_index < static_cast<int>(generations.size())) {
            generations[static_cast<std::size_t>(page_index)] = nextGeneration();
        }
    }

    void noteClear() {
        clear_generation_ = nextGeneration();
        if (!sparse_) {
            dense_page_generations_.assign(pageCountForCapacity(capacity), clear_generation_);
        }
    }

    void convertDenseToSparse(int new_capacity) {
        std::unordered_map<int, std::vector<TernaryValue>> pages;
        std::unordered_map<int, std::uint64_t> generations;
        const TernaryValue zero = TernaryValue::zero();
        for (int addr = 0; addr < capacity; ++addr) {
            if (words[addr] == zero) continue;
            const int page_index = pageIndex(addr);
            auto& page = pages[page_index];
            if (page.empty()) page.assign(SPARSE_VM_PAGE_WORDS, zero);
            page[static_cast<std::size_t>(pageOffset(addr))] = words[addr];
            generations[page_index] = pageGeneration(page_index);
        }
        if (words != nullptr) allocator->deallocateDataWords(words);
        words = nullptr;
        sparse_pages_ = std::move(pages);
        sparse_page_generations_ = std::move(generations);
        dense_page_generations_.clear();
        sparse_ = true;
        capacity = new_capacity;
        invalidateSparseCache();
    }
};

// =============================================================================
// SECTION 6 — Instruction Memory
// =============================================================================
// Word-addressed flat array of TritWord27 instruction words.
// Separate from data memory (Harvard architecture).
// The PC is an index into this array.

struct TernaryInstructionMemory {
    TritWord27* words = nullptr; // Non-null only for dense compatibility mode.
    int capacity = 0;
    VMStateAllocator* allocator = &defaultVMStateAllocator();

    explicit TernaryInstructionMemory(int size = DEFAULT_IMEM_SIZE,
                                      VMStateAllocator& alloc = defaultVMStateAllocator(),
                                      MemoryBacking backing = MemoryBacking::Auto)
        : allocator(&alloc) {
        allocate(size, backing);
        reset();
    }

    ~TernaryInstructionMemory() {
        release();
    }

    TernaryInstructionMemory(const TernaryInstructionMemory& other)
        : allocator(other.allocator), sparse_(other.sparse_), generation_(other.generation_) {
        allocate(other.capacity, other.sparse_ ? MemoryBacking::Sparse : MemoryBacking::Dense);
        try {
            if (!sparse_ && capacity > 0) std::copy(other.words, other.words + capacity, words);
            if (sparse_) sparse_pages_ = other.sparse_pages_;
        } catch (...) {
            release();
            throw;
        }
    }

    TernaryInstructionMemory& operator=(const TernaryInstructionMemory& other) {
        if (this == &other) return *this;
        TernaryInstructionMemory tmp(other);  // copy-and-swap for strong exception safety
        swap(tmp);
        return *this;
    }

    TernaryInstructionMemory(TernaryInstructionMemory&& other) noexcept
        : words(other.words),
          capacity(other.capacity),
          allocator(other.allocator),
          sparse_(other.sparse_),
          sparse_pages_(std::move(other.sparse_pages_)),
          generation_(other.generation_) {
        invalidateSparseCache();
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        other.sparse_ = false;
        other.invalidateSparseCache();
    }

    TernaryInstructionMemory& operator=(TernaryInstructionMemory&& other) noexcept {
        if (this == &other) return *this;
        release();
        words = other.words;
        capacity = other.capacity;
        allocator = other.allocator;
        sparse_ = other.sparse_;
        sparse_pages_ = std::move(other.sparse_pages_);
        invalidateSparseCache();
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        other.sparse_ = false;
        other.invalidateSparseCache();
        return *this;
    }

    void swap(TernaryInstructionMemory& other) noexcept {
        std::swap(words, other.words);
        std::swap(capacity, other.capacity);
        std::swap(allocator, other.allocator);
        std::swap(sparse_, other.sparse_);
        std::swap(sparse_pages_, other.sparse_pages_);
        std::swap(generation_, other.generation_);
        invalidateSparseCache();
        other.invalidateSparseCache();
    }

    void allocate(int size, MemoryBacking backing = MemoryBacking::Auto) {
        if (size < 0) throw std::invalid_argument("negative TernaryInstructionMemory size");
        capacity = size;
        sparse_ = backing == MemoryBacking::Sparse ||
                  (backing == MemoryBacking::Auto && size > SPARSE_MEMORY_DENSE_LIMIT_WORDS);
        if (sparse_) {
            words = nullptr;
            return;
        }
        words = allocator->allocateInstructionWords(capacity);
    }

    void release() {
        if (words != nullptr) {
            allocator->deallocateInstructionWords(words);
            words = nullptr;
        }
        sparse_pages_.clear();
        invalidateSparseCache();
        capacity = 0;
        sparse_ = false;
        bumpGeneration();
    }

    void reset() {
        if (sparse_) {
            sparse_pages_.clear();
            invalidateSparseCache();
        } else if (capacity > 0) {
            std::fill(words, words + capacity, TritWord27{});
        }
        bumpGeneration();
    }

    void resize(int new_size, MemoryBacking backing = MemoryBacking::Auto) {
        release();
        allocate(new_size, backing);
        reset();
    }

    // Fetch the instruction word at PC address [addr].
    // Returns {word, OK} on success, {zero_word, OUT_OF_RANGE} on fault.
    [[nodiscard]] std::pair<TritWord27, MemFaultCode> fetch(int addr) const {
        if (addr < 0 || addr >= capacity) {
            return {TritWord27{}, MemFaultCode::OUT_OF_RANGE};
        }
        if (!sparse_) return {words[addr], MemFaultCode::OK};
        const int page_index = pageIndex(addr);
        if (cached_sparse_page_index_ == page_index && cached_sparse_page_ != nullptr) {
            return {(*cached_sparse_page_)[static_cast<std::size_t>(pageOffset(addr))],
                    MemFaultCode::OK};
        }
        const auto page = sparse_pages_.find(page_index);
        if (page == sparse_pages_.end()) {
            cached_sparse_page_index_ = -1;
            cached_sparse_page_ = nullptr;
            return {TritWord27{}, MemFaultCode::OK};
        }
        cached_sparse_page_index_ = page_index;
        cached_sparse_page_ = &page->second;
        return {page->second[static_cast<std::size_t>(pageOffset(addr))], MemFaultCode::OK};
    }

    // Write a single instruction word (used by the assembler / test harness).
    MemFaultCode write(int addr, TritWord27 iw) {
        if (addr < 0 || addr >= capacity) {
            return MemFaultCode::OUT_OF_RANGE;
        }
        if (!sparse_) {
            words[addr] = iw;
            bumpGeneration();
            return MemFaultCode::OK;
        }
        const int page_index = pageIndex(addr);
        auto& page = sparse_pages_[page_index];
        if (page.empty()) page.assign(SPARSE_VM_PAGE_WORDS, TritWord27{});
        page[static_cast<std::size_t>(pageOffset(addr))] = iw;
        cached_sparse_page_index_ = page_index;
        cached_sparse_page_ = &page;
        bumpGeneration();
        return MemFaultCode::OK;
    }

    // Load a full program from a vector of TritWord27 instruction words
    // starting at [start_addr]. Returns false and does not write if the
    // program would exceed capacity.
    bool loadProgram(const std::vector<TritWord27>& program, int start_addr = 0) {
        if (start_addr < 0) return false;
        if (start_addr + static_cast<int>(program.size()) > capacity) return false;
        for (int i = 0; i < static_cast<int>(program.size()); ++i) {
            if (write(start_addr + i, program[static_cast<std::size_t>(i)]) != MemFaultCode::OK) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool inRange(int addr) const {
        return addr >= 0 && addr < capacity;
    }

    [[nodiscard]] int size() const { return capacity; }
    [[nodiscard]] bool isSparse() const { return sparse_; }
    [[nodiscard]] std::size_t allocatedPages() const { return sparse_pages_.size(); }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    template <typename Callback>
    void forEachNonZero(Callback&& callback) const {
        const TritWord27 zero{};
        if (!sparse_) {
            for (int address = 0; address < capacity; ++address) {
                if (words[address].bits != zero.bits) callback(address, words[address]);
            }
            return;
        }
        for (const auto& entry : sparse_pages_) {
            const int base = entry.first * SPARSE_VM_PAGE_WORDS;
            const auto& page = entry.second;
            const int limit = std::min<int>(
                static_cast<int>(page.size()), capacity - base);
            for (int offset = 0; offset < limit; ++offset) {
                const TritWord27& value = page[static_cast<std::size_t>(offset)];
                if (value.bits != zero.bits) callback(base + offset, value);
            }
        }
    }

private:
    bool sparse_ = false;
    std::unordered_map<int, std::vector<TritWord27>> sparse_pages_;
    mutable int cached_sparse_page_index_ = -1;
    mutable const std::vector<TritWord27>* cached_sparse_page_ = nullptr;
    std::uint64_t generation_ = 1;

    [[nodiscard]] static int pageIndex(int addr) { return addr / SPARSE_VM_PAGE_WORDS; }
    [[nodiscard]] static int pageOffset(int addr) { return addr % SPARSE_VM_PAGE_WORDS; }

    void invalidateSparseCache() const {
        cached_sparse_page_index_ = -1;
        cached_sparse_page_ = nullptr;
    }

    void bumpGeneration() {
        ++generation_;
        if (generation_ == 0) generation_ = 1;
    }
};

// =============================================================================
// SECTION 7 — VM Execution Status
// =============================================================================

enum class VMStatus : uint8_t {
    RUNNING  = 0,  // Normal execution. PC will advance on next step.
    HALTED   = 1,  // Clean HALT instruction executed. PC points to HALT word.
    TRAPPED  = 2,  // Fault occurred. trap_reg written. Execution stopped.
    WAITING  = 3,  // Architectural idle; resumes only after an event.
};

[[nodiscard]] inline std::string vmStatusToString(VMStatus s) {
    switch (s) {
        case VMStatus::RUNNING: return "RUNNING";
        case VMStatus::HALTED:  return "HALTED";
        case VMStatus::TRAPPED: return "TRAPPED";
        case VMStatus::WAITING: return "WAITING";
        default:                return "UNKNOWN";
    }
}

// =============================================================================
// SECTION 8a — Integer <-> LongTriple Bridges (needed by VMState::reset)
// =============================================================================
// Declared before VMState because reset() calls ops::fromLong to init SP.

namespace ops {

[[nodiscard]] inline TernaryValue fromLong(long long n, TernaryMode mode = TernaryMode::T40) {
    switch (mode) {
        case TernaryMode::T1:  return TernaryValue::fromT1(native_ops::fromIntT1(n));
        case TernaryMode::T5:  return TernaryValue::fromT5(native_ops::fromIntT5(n));
        case TernaryMode::T10: return TernaryValue::fromT10(native_ops::fromIntT10(n));
        case TernaryMode::T20: return TernaryValue::fromT20(native_ops::fromIntT20(n));
        case TernaryMode::T40: return TernaryValue::fromTriple(native_ops::fromIntT40(n));
        case TernaryMode::T50: return TernaryValue::fromLongTriple(native_ops::fromInt(n));
        case TernaryMode::L1:  return convertValue(fromLong(n, TernaryMode::T1), TernaryMode::L1);
        case TernaryMode::L5:  return convertValue(fromLong(n, TernaryMode::T5), TernaryMode::L5);
        case TernaryMode::L10: return convertValue(fromLong(n, TernaryMode::T10), TernaryMode::L10);
        case TernaryMode::L20: return convertValue(fromLong(n, TernaryMode::T20), TernaryMode::L20);
        case TernaryMode::L40: return convertValue(fromLong(n, TernaryMode::T40), TernaryMode::L40);
        case TernaryMode::L50: return convertValue(fromLong(n, TernaryMode::T50), TernaryMode::L50);
    }
    return TernaryValue::fromLongTriple(native_ops::fromInt(n));
}

[[nodiscard]] inline long long toLong(TernaryValue t) {
    if (isLaneMode(t.mode)) {
        return toLong(convertValue(t, matchingNumericMode(t.mode)));
    }
    switch (t.mode) {
        case TernaryMode::T1:  return native_ops::toLongLong(t.asT1());
        case TernaryMode::T5:  return native_ops::toLongLong(t.asT5());
        case TernaryMode::T10: return native_ops::toLongLong(t.asT10());
        case TernaryMode::T20: return native_ops::toLongLong(t.asT20());
        case TernaryMode::T40: return native_ops::toLongLong(t.asTriple());
        case TernaryMode::T50: return native_ops::toLongLong(t.asLongTripleRaw());
        default: break;
    }
    return 0;
}

[[nodiscard]] inline long long toLong(LongTriple t) {
    return native_ops::toLongLong(t);
}

} // namespace ops

// =============================================================================
// SECTION 8b - Single-Level Page Table Helpers
// =============================================================================

static_assert(MMU_PAGE_WORDS == 729, "v2 base pages are 729 ternary words");
static_assert(STORAGE_BLOCK_WORDS == 27,
              "storage transfer blocks remain 27 ternary words");
static constexpr int PTE_FLAG_VALID = 0;
static constexpr int PTE_FLAG_USER = 1;
static constexpr int PTE_FLAG_READ = 2;
static constexpr int PTE_FLAG_WRITE = 3;
static constexpr int PTE_FLAG_EXECUTE = 4;
static constexpr int PTE_PPN_SHIFT = 5;
static constexpr int TASK_CONTEXT_WORDS = 32;
static constexpr int TASK_CONTEXT_EPC = 0;
static constexpr int TASK_CONTEXT_STATUS = 1;
static constexpr int TASK_CONTEXT_IMEM_PTBR = 2;
static constexpr int TASK_CONTEXT_IMEM_PAGES = 3;
static constexpr int TASK_CONTEXT_DMEM_PTBR = 4;
static constexpr int TASK_CONTEXT_DMEM_PAGES = 5;
static constexpr int TASK_CONTEXT_REG_BASE = 6;

static constexpr int PROC_STATE_FREE     = 0;
static constexpr int PROC_STATE_RUNNABLE = 1;
static constexpr int PROC_STATE_RUNNING  = 2;
static constexpr int PROC_STATE_BLOCKED  = 3;
static constexpr int PROC_STATE_SLEEPING = 4;
static constexpr int PROC_STATE_EXITED   = 5;
static constexpr int PROC_STATE_STOPPED  = 6;
static constexpr int PROC_STATE_ZOMBIE   = 7;
static constexpr int PROC_STATE_KILLING  = 8;
static constexpr int PROC_STATE_CRASHED  = 9;
static constexpr int PROC_DEFAULT_QUANTUM = 180;
static constexpr int PROC_WAIT_NONE          = 0;
static constexpr int PROC_WAIT_TIMER         = 1;
static constexpr int PROC_WAIT_CONSOLE_INPUT = 2;
static constexpr int PROC_WAIT_CHILD         = 3;
static constexpr int SIGNAL_TERM          = 1;
static constexpr int SIGNAL_KILL          = 2;
static constexpr int SIGNAL_STOP          = 4;
static constexpr int SIGNAL_CONT          = 8;
static constexpr int SIGNAL_CLOSE_REQUEST = 16;

static constexpr int SYSCALL_WRITE_INT        = 1;
static constexpr int SYSCALL_NEWLINE          = 2;
static constexpr int SYSCALL_CLEAR_CONSOLE    = 3;
static constexpr int SYSCALL_YIELD            = 4;
static constexpr int SYSCALL_SLEEP_UNTIL_TICK = 5;
static constexpr int SYSCALL_EXIT             = 6;
static constexpr int SYSCALL_GETPID           = 7;
static constexpr int SYSCALL_UPTIME           = 8;
static constexpr int SYSCALL_READ_INPUT       = 9;
static constexpr int SYSCALL_SPAWN            = 10;
static constexpr int SYSCALL_WAITPID          = 11;
static constexpr int SYSCALL_OPEN             = 12;
static constexpr int SYSCALL_CLOSE            = 13;
static constexpr int SYSCALL_READ             = 14;
static constexpr int SYSCALL_WRITE            = 15;
static constexpr int SYSCALL_STAT             = 16;
static constexpr int SYSCALL_READDIR          = 17;
static constexpr int SYSCALL_BRK              = 18;
static constexpr int SYSCALL_SBRK             = 19;
static constexpr int SYSCALL_FORK             = 20;
static constexpr int SYSCALL_EXEC             = 21;
static constexpr int SYSCALL_WRITE_CHAR       = 22;
static constexpr int SYSCALL_FSYNC            = 47;
static constexpr int SYSCALL_KILL             = 48;
static constexpr int SYSCALL_SUSPEND          = 49;
static constexpr int SYSCALL_RESUME           = 50;
static constexpr int SYSCALL_GETPROC          = 51;
static constexpr int SYSCALL_FUTEX_WAIT       = 52;
static constexpr int SYSCALL_FUTEX_WAKE       = 53;
static constexpr int SYSCALL_IPC_RECV_BLOCKING = 54;
static constexpr int SYSCALL_WAIT_EVENT       = 55;
static constexpr int SYSCALL_SLEEP_MS         = 56;
static constexpr int SYSCALL_APP_SPAWN        = 57;
static constexpr int SYSCALL_REBOOT           = 58;
static constexpr int SYSCALL_RENAME           = 59;

static constexpr int EXEC_MAGIC = 40404;

#include "executable_header_v2.h"

inline bool initializeTaskContext(
    TernaryMemory& dmem,
    int context_addr,
    const ExecutableImageHeaderV2& header,
    int imem_ptbr,
    int dmem_ptbr) {

    if (!validateExecutableHeaderV2(header)) return false;
    if (context_addr < 0 || context_addr + TASK_CONTEXT_WORDS > dmem.size()) return false;
    const int sp = header.stack_words;
    if (dmem.store(context_addr + TASK_CONTEXT_EPC,
                   ops::fromLong(header.entry_pc)) != MemFaultCode::OK) return false;
    if (dmem.store(context_addr + TASK_CONTEXT_STATUS,
                   ops::fromLong(35)) != MemFaultCode::OK) return false;
    if (dmem.store(context_addr + TASK_CONTEXT_IMEM_PTBR,
                   ops::fromLong(imem_ptbr)) != MemFaultCode::OK) return false;
    if (dmem.store(context_addr + TASK_CONTEXT_IMEM_PAGES,
                   ops::fromLong(executableTextPages(header))) != MemFaultCode::OK) return false;
    if (dmem.store(context_addr + TASK_CONTEXT_DMEM_PTBR,
                   ops::fromLong(dmem_ptbr)) != MemFaultCode::OK) return false;
    if (dmem.store(context_addr + TASK_CONTEXT_DMEM_PAGES,
                   ops::fromLong(executableDataPages(header))) != MemFaultCode::OK) return false;
    for (int i = TASK_CONTEXT_REG_BASE; i < TASK_CONTEXT_WORDS; ++i) {
        if (dmem.store(context_addr + i, TernaryValue::zero()) != MemFaultCode::OK) return false;
    }
    return dmem.store(context_addr + TASK_CONTEXT_REG_BASE + R26_SP - 1,
                      ops::fromLong(sp)) == MemFaultCode::OK;
}

struct PageTableEntry {
    int ppn = 0;
    bool present = false;
    bool user = false;
    bool read = false;
    bool write = false;
    bool execute = false;
    bool global = false;
    bool accessed = false;
    bool dirty = false;
    bool superpage = false;
};

[[nodiscard]] inline TernaryValue encodePageTableEntry(
    int ppn,
    bool user,
    bool read,
    bool write,
    bool execute,
    bool present = true) {

    // PTE v2 is one canonical numeric T40 word. Flag trits are zero or
    // positive; zero is the unique non-present encoding.
    if (!present) return TernaryValue::zero(TernaryMode::T40);
    if (ppn < 0) {
        return TernaryValue::invalid(TernaryMode::T40);
    }
    long long flags = 1;
    if (user) flags += 3;
    if (read) flags += 9;
    if (write) flags += 27;
    if (execute) flags += 81;
    return ops::fromLong(
        static_cast<long long>(ppn) * 19683 + flags,
        TernaryMode::T40);
}

[[nodiscard]] inline TernaryValue encodePageTableEntry(
    const PageTableEntry& entry) {
    if (!entry.present) return TernaryValue::zero(TernaryMode::T40);
    if (entry.ppn < 0 ||
        (entry.superpage && entry.ppn % 27 != 0)) {
        return TernaryValue::invalid(TernaryMode::T40);
    }
    long long flags = 1;
    if (entry.user) flags += 3;
    if (entry.read) flags += 9;
    if (entry.write) flags += 27;
    if (entry.execute) flags += 81;
    if (entry.global) flags += 243;
    if (entry.accessed) flags += 729;
    if (entry.dirty) flags += 2187;
    if (entry.superpage) flags += 6561;
    return ops::fromLong(
        static_cast<long long>(entry.ppn) * 19683 + flags,
        TernaryMode::T40);
}

[[nodiscard]] inline bool decodePageTableEntryV2(
        TernaryValue value,
        PageTableEntry& out) {
    if (!isNumericMode(value.mode) || value.isInvalid()) return false;
    const long long packed = ops::toLong(value);
    if (packed == 0) {
        out = PageTableEntry{};
        return true;
    }
    if (packed < 0) return false;
    long long flag_word = packed % 19683;
    std::array<bool*, 9> fields = {
        &out.present, &out.user, &out.read, &out.write, &out.execute,
        &out.global, &out.accessed, &out.dirty, &out.superpage};
    for (int trit = 0; trit < 9; ++trit) {
        const int digit = static_cast<int>(flag_word % 3);
        flag_word /= 3;
        if (digit == 2) return false;
        *fields[static_cast<std::size_t>(trit)] = digit == 1;
    }
    const long long ppn = packed / 19683;
    if (ppn < 0 || ppn > std::numeric_limits<int>::max()) return false;
    out.ppn = static_cast<int>(ppn);
    if (!out.present || (out.superpage && out.ppn % 27 != 0))
        return false;
    return true;
}

[[nodiscard]] inline bool decodePageTableEntry(
        TernaryValue value,
        PageTableEntry& out) {
    return decodePageTableEntryV2(value, out);
}

// =============================================================================
// SECTION 8c - Vector Register and Fault State
// =============================================================================

static constexpr int DEFAULT_VECTOR_LENGTH = 27;

struct TernaryVectorRegister {
    std::vector<TernaryValue> lane;

    void reset(int length) {
        lane.assign(std::max(0, length), TernaryValue::zero());
    }

    [[nodiscard]] TernaryValue read(int index) const {
        if (index < 0 || index >= static_cast<int>(lane.size())) {
            return TernaryValue::zero();
        }
        return lane[static_cast<std::size_t>(index)];
    }

    void write(int index, TernaryValue value) {
        if (index < 0 || index >= static_cast<int>(lane.size())) return;
        lane[static_cast<std::size_t>(index)] = value;
    }
};

struct TernaryVectorFile {
    std::array<TernaryVectorRegister, VECTOR_REGISTER_COUNT> reg;

    void reset(int length) {
        for (auto& r : reg) r.reset(length);
    }

    [[nodiscard]] bool validRegister(uint8_t idx) const {
        return idx < VECTOR_REGISTER_COUNT;
    }
};

struct VectorFaultState {
    std::vector<uint8_t> fault_valid;
    std::vector<TrapCode> fault_class;
    int first_failing_lane = -1;

    void reset(int length) {
        fault_valid.assign(std::max(0, length), 0);
        fault_class.assign(std::max(0, length), TrapCode::TRAP_MEM_FAULT);
        first_failing_lane = -1;
    }

    void clear() {
        std::fill(fault_valid.begin(), fault_valid.end(), 0);
        std::fill(fault_class.begin(), fault_class.end(), TrapCode::TRAP_MEM_FAULT);
        first_failing_lane = -1;
    }

    void setLane(int lane, TrapCode code) {
        if (lane < 0 || lane >= static_cast<int>(fault_valid.size())) return;
        fault_valid[static_cast<std::size_t>(lane)] = 1;
        fault_class[static_cast<std::size_t>(lane)] = code;
        if (first_failing_lane < 0) first_failing_lane = lane;
    }

    [[nodiscard]] bool any() const {
        for (uint8_t valid : fault_valid) {
            if (valid) return true;
        }
        return false;
    }
};

// =============================================================================
// SECTION 8 — Complete VM State
// =============================================================================
// The single aggregate that the Phase 3 dispatcher reads and mutates.
// One VMState instance = one running ternary machine.

class SparseDirtyBlocks {
public:
    SparseDirtyBlocks() = default;
    explicit SparseDirtyBlocks(int count, bool value = false) {
        assign(static_cast<std::size_t>(count), value);
    }

    void assign(std::size_t count, bool value) {
        count_ = static_cast<int>(count);
        dirty_.clear();
        if (value) {
            for (int i = 0; i < count_; ++i) dirty_.insert(i);
        }
    }

    [[nodiscard]] bool operator[](std::size_t index) const {
        return index < static_cast<std::size_t>(count_) &&
               dirty_.count(static_cast<int>(index)) != 0;
    }

    void set(int index, bool value = true) {
        if (index < 0 || index >= count_) return;
        if (value) dirty_.insert(index);
        else dirty_.erase(index);
    }

    [[nodiscard]] std::size_t size() const { return static_cast<std::size_t>(count_); }
    [[nodiscard]] std::size_t dirtyCount() const { return dirty_.size(); }

private:
    int count_ = 0;
    std::unordered_set<int> dirty_;
};

struct VMBlockDeviceStats {
    long long reads = 0;
    long long writes = 0;
    long long hits = 0;
    long long misses = 0;
    long long dirty_flushes = 0;
    long long flushes = 0;
    long long compactions = 0;
    long long read_ahead = 0;

    void reset() {
        reads = 0;
        writes = 0;
        hits = 0;
        misses = 0;
        dirty_flushes = 0;
        flushes = 0;
        compactions = 0;
        read_ahead = 0;
    }
};

class SparseBlockStorage {
public:
    explicit SparseBlockStorage(int block_count = 192) {
        reset(block_count);
    }

    void reset(int block_count) {
        block_count_ = std::max(1, block_count);
        blocks_.clear();
        read_cache_.clear();
        pending_blocks_.clear();
        compact_record_count_ = 0;
        backing_generation_ = 0;
        stats_.reset();
    }

    [[nodiscard]] int blockCount() const { return block_count_; }
    [[nodiscard]] int blockWords() const { return STORAGE_BLOCK_WORDS; }
    [[nodiscard]] std::size_t allocatedBlocks() const { return blocks_.size(); }
    [[nodiscard]] std::size_t pendingWriteCount() const { return pending_blocks_.size(); }
    [[nodiscard]] int backingRecordCount() const { return compact_record_count_; }
    [[nodiscard]] VMBlockDeviceStats stats() const { return stats_; }
    void resetStats() const { stats_.reset(); }

    [[nodiscard]] bool attachBackingFile(const std::string& path) {
        backing_path_ = path;
        blocks_.clear();
        read_cache_.clear();
        pending_blocks_.clear();
        compact_record_count_ = 0;
        backing_generation_ = 0;
        stats_.reset();
        return loadCompactBacking();
    }

    void detachBackingFile() {
        (void)flushBackingFile();
        backing_path_.clear();
        pending_blocks_.clear();
        read_cache_.clear();
    }

    [[nodiscard]] const std::string& backingPath() const { return backing_path_; }

    [[nodiscard]] bool readBlock(int index, std::vector<long long>& out) const {
        if (index < 0 || index >= block_count_) return false;
        ++stats_.reads;
        const auto cached = read_cache_.find(index);
        if (cached != read_cache_.end()) {
            ++stats_.hits;
            out = cached->second;
        } else {
            ++stats_.misses;
            out = materializeBlock(index);
            cacheBlock(index, out);
        }
        prefetchReadAhead(index + 1);
        return true;
    }

    [[nodiscard]] bool writeBlock(int index, const std::vector<long long>& data) {
        if (index < 0 || index >= block_count_) return false;
        if (static_cast<int>(data.size()) != STORAGE_BLOCK_WORDS) return false;

        ++stats_.writes;
        applyBlock(index, data);
        cacheBlock(index, data);
        if (!backing_path_.empty()) {
            pending_blocks_[index] = data;
            if (shouldCompactBacking() && !rewriteCompactBacking()) return false;
        }
        return true;
    }

    [[nodiscard]] std::vector<long long> serializeDense() const {
        std::vector<long long> out;
        out.reserve(static_cast<std::size_t>(block_count_) *
                    static_cast<std::size_t>(STORAGE_BLOCK_WORDS));
        for (int index = 0; index < block_count_; ++index) {
            const std::vector<long long> block = materializeBlock(index);
            out.insert(out.end(), block.begin(), block.end());
        }
        return out;
    }

    [[nodiscard]] bool loadSerialized(const std::vector<long long>& image) {
        if (image.empty() ||
            static_cast<int>(image.size()) % STORAGE_BLOCK_WORDS != 0)
            return false;
        const int blocks =
            static_cast<int>(image.size()) / STORAGE_BLOCK_WORDS;
        reset(blocks);
        for (int block = 0; block < blocks; ++block) {
            std::vector<long long> payload(STORAGE_BLOCK_WORDS, 0);
            for (int word = 0; word < STORAGE_BLOCK_WORDS; ++word) {
                payload[static_cast<std::size_t>(word)] =
                    image[static_cast<std::size_t>(
                        block * STORAGE_BLOCK_WORDS + word)];
            }
            if (!isZeroBlock(payload)) blocks_[block] = std::move(payload);
        }
        read_cache_.clear();
        return true;
    }

    [[nodiscard]] bool flushBackingFile() {
        if (backing_path_.empty()) {
            pending_blocks_.clear();
            return true;
        }
        if (pending_blocks_.empty()) return true;
        return appendPendingRecords();
    }

    [[nodiscard]] bool compactBackingFile(bool force = true) {
        if (backing_path_.empty()) return true;
        if (!force && !shouldCompactBacking()) return flushBackingFile();
        return rewriteCompactBacking();
    }

    // Write the current sparse block set to an independent canonical tDisk
    // v2 file.  Unlike compactBackingFile this never changes the live backing
    // path, so it is safe to use for an immutable VM checkpoint bundle.
    [[nodiscard]] bool writeSnapshotFile(const std::string& path) const {
        std::vector<int> indices;
        indices.reserve(blocks_.size());
        for (const auto& block : blocks_) indices.push_back(block.first);
        std::sort(indices.begin(), indices.end());
        return writeCompactBackingTo(std::filesystem::path(path), indices);
    }

private:
    int block_count_ = 1;
    std::unordered_map<int, std::vector<long long>> blocks_;
    mutable std::unordered_map<int, std::vector<long long>> read_cache_;
    std::unordered_map<int, std::vector<long long>> pending_blocks_;
    std::string backing_path_;
    int compact_record_count_ = 0;
    std::uint64_t backing_generation_ = 0;
    mutable VMBlockDeviceStats stats_;

    [[nodiscard]] static bool isZeroBlock(const std::vector<long long>& data) {
        for (long long value : data) {
            if (value != 0) return false;
        }
        return true;
    }

    [[nodiscard]] std::vector<long long> materializeBlock(int index) const {
        std::vector<long long> out(STORAGE_BLOCK_WORDS, 0);
        const auto found = blocks_.find(index);
        if (found != blocks_.end()) out = found->second;
        return out;
    }

    void cacheBlock(int index, const std::vector<long long>& data) const {
        if (index < 0 || index >= block_count_) return;
        if (read_cache_.find(index) == read_cache_.end() &&
            read_cache_.size() >= kReadCacheMaxBlocks) {
            read_cache_.erase(read_cache_.begin());
        }
        read_cache_[index] = data;
    }

    void prefetchReadAhead(int index) const {
        if (index < 0 || index >= block_count_) return;
        if (read_cache_.find(index) != read_cache_.end()) return;
        cacheBlock(index, materializeBlock(index));
        ++stats_.read_ahead;
    }

    void applyBlock(int index, const std::vector<long long>& data) {
        if (isZeroBlock(data)) {
            blocks_.erase(index);
        } else {
            blocks_[index] = data;
        }
    }

    [[nodiscard]] bool shouldCompactBacking() const {
        if (backing_path_.empty()) return false;
        const long long live_records =
            std::max<long long>(1, static_cast<long long>(blocks_.size()));
        const long long threshold =
            std::max<long long>(kCompactionMinRecords,
                                live_records * kCompactionRecordMultiplier +
                                    kCompactionSlackRecords);
        return static_cast<long long>(compact_record_count_) +
                   static_cast<long long>(pending_blocks_.size()) > threshold;
    }

    static void hashBytes(
        std::uint64_t& hash,
        const void* data,
        std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    }

    [[nodiscard]] static std::uint64_t canonicalRawWord(long long value) {
        return convertValue(ops::fromLong(value), TernaryMode::T40)
            .asTriple().data;
    }

    [[nodiscard]] static bool numericWordFromRaw(
        std::uint64_t raw,
        long long& value) {
        if (raw > 12157665459056928801ULL) return false;
        value = ops::toLong(TernaryValue::fromTriple(Triple{raw}));
        return true;
    }

    [[nodiscard]] bool writeCompactBackingTo(const std::filesystem::path& path,
                                             const std::vector<int>& indices) const {
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file.good()) return false;
            const std::uint64_t magic = kSparseDiskV2Magic;
            const std::uint32_t version = kSparseDiskVersion;
            const std::uint32_t block_words = STORAGE_BLOCK_WORDS;
            const std::uint64_t generation = backing_generation_ + 1;
            const int count = static_cast<int>(indices.size());
            std::uint64_t checksum = 1469598103934665603ULL;
            for (int index : indices) {
                const auto found = blocks_.find(index);
                if (found == blocks_.end()) continue;
                hashBytes(checksum, &index, sizeof(index));
                for (long long word : found->second) {
                    const std::uint64_t raw = canonicalRawWord(word);
                    hashBytes(checksum, &raw, sizeof(raw));
                }
            }
            file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));
            file.write(reinterpret_cast<const char*>(&block_words),
                       sizeof(block_words));
            file.write(reinterpret_cast<const char*>(&generation),
                       sizeof(generation));
            file.write(reinterpret_cast<const char*>(&checksum),
                       sizeof(checksum));
            file.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (int index : indices) {
                const auto found = blocks_.find(index);
                if (found == blocks_.end()) continue;
                file.write(reinterpret_cast<const char*>(&index), sizeof(index));
                for (long long word : found->second) {
                    const std::uint64_t raw = canonicalRawWord(word);
                    file.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
                }
            }
            file.flush();
            if (!file.good()) return false;
        }
        return detail::syncFileToStableStorage(path);
    }

    [[nodiscard]] bool rewriteCompactBacking() {
        if (backing_path_.empty()) return true;
        const std::size_t flushed_blocks = pending_blocks_.size();

        std::vector<int> indices;
        indices.reserve(blocks_.size());
        for (const auto& block : blocks_) indices.push_back(block.first);
        std::sort(indices.begin(), indices.end());

        const std::filesystem::path target(backing_path_);
        std::filesystem::path temp = target;
        temp += ".compact";
        if (!writeCompactBackingTo(temp, indices)) {
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            ec.clear();
            std::filesystem::copy_file(temp,
                                       target,
                                       std::filesystem::copy_options::overwrite_existing,
                                       ec);
            std::error_code cleanup_ec;
            std::filesystem::remove(temp, cleanup_ec);
            if (ec) return false;
        }
        if (!detail::syncFileToStableStorage(target)) return false;

        compact_record_count_ = static_cast<int>(indices.size());
        ++backing_generation_;
        pending_blocks_.clear();
        if (flushed_blocks != 0) {
            stats_.dirty_flushes +=
                static_cast<long long>(flushed_blocks);
            ++stats_.flushes;
        }
        ++stats_.compactions;
        return true;
    }

    [[nodiscard]] bool loadCompactBacking() {
        if (backing_path_.empty()) return true;
        std::ifstream file(backing_path_, std::ios::binary);
        if (!file.good()) return initializeCompactBacking();
        std::error_code size_ec;
        const std::uintmax_t file_size = std::filesystem::file_size(backing_path_, size_ec);
        if (size_ec || file_size < kSparseDiskV2HeaderBytes) return false;
        std::uint64_t magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (!file.good()) return false;

        int count = 0;
        std::uint64_t expected_checksum = 0;
        std::uintmax_t header_bytes = kSparseDiskV2HeaderBytes;
        std::uintmax_t record_bytes = kSparseDiskV2RecordBytes;
        if (magic == kSparseDiskV2Magic) {
            std::uint32_t version = 0;
            std::uint32_t block_words = 0;
            file.read(reinterpret_cast<char*>(&version), sizeof(version));
            file.read(reinterpret_cast<char*>(&block_words), sizeof(block_words));
            file.read(reinterpret_cast<char*>(&backing_generation_),
                      sizeof(backing_generation_));
            file.read(reinterpret_cast<char*>(&expected_checksum),
                      sizeof(expected_checksum));
            file.read(reinterpret_cast<char*>(&count), sizeof(count));
            if (!file.good() || version != kSparseDiskVersion ||
                block_words != STORAGE_BLOCK_WORDS || count < 0) {
                return false;
            }
            header_bytes = kSparseDiskV2HeaderBytes;
            record_bytes = kSparseDiskV2RecordBytes;
        } else {
            return false;
        }

        blocks_.clear();
        const int max_records =
            static_cast<int>((file_size - header_bytes) / record_bytes);
        const int records_to_read = std::min(count, max_records);
        const bool needs_repair = count > max_records;
        compact_record_count_ = records_to_read;
        std::uint64_t actual_checksum = 1469598103934665603ULL;
        for (int i = 0; i < records_to_read; ++i) {
            int index = -1;
            std::vector<long long> payload(STORAGE_BLOCK_WORDS, 0);
            file.read(reinterpret_cast<char*>(&index), sizeof(index));
            hashBytes(actual_checksum, &index, sizeof(index));
            for (int word = 0; word < STORAGE_BLOCK_WORDS; ++word) {
                std::uint64_t raw = 0;
                file.read(reinterpret_cast<char*>(&raw), sizeof(raw));
                if (!numericWordFromRaw(
                        raw, payload[static_cast<std::size_t>(word)])) {
                    return false;
                }
                hashBytes(actual_checksum, &raw, sizeof(raw));
            }
            if (!file.good()) return false;
            if (index >= 0 && index < block_count_) {
                if (isZeroBlock(payload)) {
                    blocks_.erase(index);
                } else {
                    blocks_[index] = std::move(payload);
                }
            }
        }
        if (actual_checksum != expected_checksum) return false;
        if (needs_repair) {
            return rewriteCompactBacking();
        }
        return true;
    }

    [[nodiscard]] bool initializeCompactBacking() {
        if (backing_path_.empty()) return true;
        backing_generation_ = 0;
        blocks_.clear();
        pending_blocks_.clear();
        return rewriteCompactBacking();
    }

    [[nodiscard]] bool appendPendingRecords() {
        if (backing_path_.empty() || pending_blocks_.empty()) return true;
        // tDisk v2 uses a checksummed generation. Rewriting the compact sparse
        // image atomically keeps the checksum and generation coherent.
        return rewriteCompactBacking();
    }

    static constexpr std::uint64_t kSparseDiskV2Magic =
        0x54524954535032ULL; // "TRITSP2"
    static constexpr std::uint32_t kSparseDiskVersion = 2;
    static constexpr std::uintmax_t kSparseDiskV2HeaderBytes =
        static_cast<std::uintmax_t>(
            sizeof(std::uint64_t) + sizeof(std::uint32_t) +
            sizeof(std::uint32_t) + sizeof(std::uint64_t) +
            sizeof(std::uint64_t) + sizeof(int));
    static constexpr std::uintmax_t kSparseDiskV2RecordBytes =
        static_cast<std::uintmax_t>(
            sizeof(int) + sizeof(std::uint64_t) * STORAGE_BLOCK_WORDS);
    static constexpr std::size_t kReadCacheMaxBlocks = 128;
    static constexpr int kCompactionMinRecords = 128;
    static constexpr int kCompactionSlackRecords = 32;
    static constexpr int kCompactionRecordMultiplier = 4;
};

enum class VMDecodedOp : uint8_t {
    Unsupported = 0,
    Nop,
    Mov,
    MovH,
    Copy,
    Swap,
    Add,
    Sub,
    Mul,
    Div,
    Sqrt,
    Neg,
    Abs,
    TCmp,
    TMin,
    TMax,
    TInv,
    TLAdd,
    TLSub,
    TLAnd,
    TLOr,
    TLNeg,
    TSel,
    Cvt,
    Load,
    Store,
};

enum class VMExecutionBackend : uint8_t {
    Interpreter,
    CachedBlockInterpreter,
    DecodedTraceExecutor,
    NativeX64Jit,
    // Transition source alias. This backend decodes and executes cached
    // traces; it does not emit native host code.
    TraceJit = DecodedTraceExecutor,
};

struct VMDecodedTraceCacheKey {
    std::uint64_t required_features = 0;
    int asid = 0;
    int pc = 0;
    int privilege = 0;
    bool mmu_enabled = false;
    int user_imem_base = 0;
    int user_imem_limit = 0;
    int user_dmem_base = 0;
    int user_dmem_limit = 0;
    int user_imem_ptbr = 0;
    int user_imem_pages = 0;
    int user_dmem_ptbr = 0;
    int user_dmem_pages = 0;
    std::uint64_t imem_generation = 0;
    std::uint64_t executable_mapping_generation = 0;
    std::uint64_t mmu_generation = 0;

    [[nodiscard]] bool operator==(
        const VMDecodedTraceCacheKey& other) const {
        return required_features == other.required_features &&
               asid == other.asid &&
               pc == other.pc &&
               privilege == other.privilege &&
               mmu_enabled == other.mmu_enabled &&
               user_imem_base == other.user_imem_base &&
               user_imem_limit == other.user_imem_limit &&
               user_dmem_base == other.user_dmem_base &&
               user_dmem_limit == other.user_dmem_limit &&
               user_imem_ptbr == other.user_imem_ptbr &&
               user_imem_pages == other.user_imem_pages &&
               user_dmem_ptbr == other.user_dmem_ptbr &&
               user_dmem_pages == other.user_dmem_pages &&
               imem_generation == other.imem_generation &&
               executable_mapping_generation ==
                   other.executable_mapping_generation &&
               mmu_generation == other.mmu_generation;
    }

    [[nodiscard]] bool operator!=(
        const VMDecodedTraceCacheKey& other) const {
        return !(*this == other);
    }
};

struct VMDecodedTraceCacheKeyHash {
    [[nodiscard]] std::size_t operator()(
        const VMDecodedTraceCacheKey& key) const noexcept {
        std::size_t hash = 0;
        auto mix = [&](std::uint64_t value) {
            hash ^= static_cast<std::size_t>(
                value + 0x9e3779b97f4a7c15ULL +
                (static_cast<std::uint64_t>(hash) << 6) +
                (static_cast<std::uint64_t>(hash) >> 2));
        };
        mix(key.required_features);
        mix(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(key.asid)));
        mix(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(key.pc)));
        mix(static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(key.privilege)));
        mix(key.mmu_enabled ? 1u : 0u);
        mix(static_cast<std::uint32_t>(key.user_imem_base));
        mix(static_cast<std::uint32_t>(key.user_imem_limit));
        mix(static_cast<std::uint32_t>(key.user_dmem_base));
        mix(static_cast<std::uint32_t>(key.user_dmem_limit));
        mix(static_cast<std::uint32_t>(key.user_imem_ptbr));
        mix(static_cast<std::uint32_t>(key.user_imem_pages));
        mix(static_cast<std::uint32_t>(key.user_dmem_ptbr));
        mix(static_cast<std::uint32_t>(key.user_dmem_pages));
        mix(key.imem_generation);
        mix(key.executable_mapping_generation);
        mix(key.mmu_generation);
        return hash;
    }
};

enum class VMMicroOpcode : uint8_t {
    Unsupported,
    Nop,
    Mov,
    MovH,
    Copy,
    TCmp,
    Add,
    Sub,
    Mul,
    Neg,
    Abs,
    Load,
    Store,
    Jmp,
    Brn,
    Brz,
    Brp,
    Call,
    Ret,
    CallR,
    Jmpr,
};

using VMDecodedTraceOp = VMMicroOpcode;
using VMTraceJitOp = VMMicroOpcode;

enum class VMMicroMemoryEffect : uint8_t {
    None,
    Read,
    Write,
};

enum VMMicroGuard : std::uint32_t {
    VM_MICRO_GUARD_NONE = 0,
    VM_MICRO_GUARD_RS1_NUMERIC = 1u << 0,
    VM_MICRO_GUARD_RS2_NUMERIC = 1u << 1,
    VM_MICRO_GUARD_RESULT_VALID = 1u << 2,
    VM_MICRO_GUARD_ADDRESS_TRANSLATION = 1u << 3,
    VM_MICRO_GUARD_MEMORY_ACCESS = 1u << 4,
    VM_MICRO_GUARD_TRACE_GENERATION = 1u << 5,
};

enum class VMMicroSideExit : uint8_t {
    None,
    Unsupported,
    InvalidOperand,
    InvalidResult,
    AddressTranslation,
    MemoryFault,
    BranchLeavesTrace,
    GenerationMismatch,
};

struct VMDecodedInstruction {
    int pc = 0;
    TritWord27 raw{};
    InstructionWord word{};
    VMDecodedOp op = VMDecodedOp::Unsupported;
    TernaryMode mode = TernaryMode::T40;
    bool supported = false;
    bool block_boundary = false;
};

struct VMDecodedCacheEntry {
    std::uint64_t imem_generation = 0;
    VMDecodedInstruction decoded{};
};

struct VMBasicBlock {
    int start_pc = 0;
    std::uint64_t imem_generation = 0;
    std::vector<VMDecodedInstruction> instructions;
};

struct VMMicroOp {
    int pc = 0;
    TritWord27 raw{};
    InstructionWord word{};
    VMMicroOpcode op = VMMicroOpcode::Unsupported;
    TernaryMode mode = TernaryMode::T40;
    VMMicroMemoryEffect memory_effect = VMMicroMemoryEffect::None;
    std::uint32_t guards = VM_MICRO_GUARD_TRACE_GENERATION;
    std::vector<VMMicroSideExit> side_exits;
    bool trap_point = true;
    int instruction_accounting = 1;
    int branch_target = -1;
    int branch_target_index = -1;
    bool ends_trace = false;
};

using VMDecodedTraceInstruction = VMMicroOp;
using VMTraceJitInstruction = VMMicroOp;

struct VMDecodedTrace {
    int start_pc = 0;
    std::uint64_t imem_generation = 0;
    VMDecodedTraceCacheKey cache_key{};
    std::vector<VMMicroOp> instructions;
};

using VMTraceJitTrace = VMDecodedTrace;

struct VMBlockCacheStats {
    long long hits = 0;
    long long misses = 0;
    long long blocks_built = 0;
    long long instructions_executed = 0;
    long long decoded_instructions = 0;
    long long fallback_steps = 0;
    long long invalidations = 0;
    long long total_block_length = 0;

    void reset() {
        hits = 0;
        misses = 0;
        blocks_built = 0;
        instructions_executed = 0;
        decoded_instructions = 0;
        fallback_steps = 0;
        invalidations = 0;
        total_block_length = 0;
    }

    [[nodiscard]] double averageBlockLength() const {
        return blocks_built == 0
            ? 0.0
            : static_cast<double>(total_block_length) / static_cast<double>(blocks_built);
    }
};

struct VMTraceJitStats {
    long long hot_pc_samples = 0;
    long long compilation_attempts = 0;
    long long traces_built = 0;
    long long traces_executed = 0;
    long long instructions_executed = 0;
    long long interpreter_bailouts = 0;
    long long unsupported_fallbacks = 0;
    long long invalidations = 0;

    void reset() {
        hot_pc_samples = 0;
        compilation_attempts = 0;
        traces_built = 0;
        traces_executed = 0;
        instructions_executed = 0;
        interpreter_bailouts = 0;
        unsupported_fallbacks = 0;
        invalidations = 0;
    }
};

// Accurate v2 stats name for the portable backend. The TraceJit spelling
// remains source-compatible during the transition release.
using VMDecodedTraceStats = VMTraceJitStats;

struct VMNativeJitStats {
    long long compilation_attempts = 0;
    long long blocks_built = 0;
    long long blocks_executed = 0;
    long long instructions_executed = 0;
    long long direct_instructions = 0;
    long long portable_side_exits = 0;
    long long invalidations = 0;
    long long wx_transitions = 0;

    void reset() { *this = VMNativeJitStats{}; }
};

struct VMCoreState {
    TernaryRegisterFile regfile;
    int pc = 0;
    VMStatus status = VMStatus::RUNNING;
    TernaryValue trap_reg = encodeNoTrap();
    int vector_length = DEFAULT_VECTOR_LENGTH;
    TernaryVectorFile vregfile;
    VectorFaultState vector_faults;
    TernaryValue accumulator = TernaryValue::zero();
    PrivilegeMode privilege = PrivilegeMode::Kernel;
    PrivilegeMode previous_privilege = PrivilegeMode::Kernel;
    bool interrupt_enable = false;
    bool previous_interrupt_enable = false;
    bool trap_routing_enabled = false;
    int epc = 0;
    int cause = 0;
    int tvec = 0;
    int scratch = 0;
    long long timer_reload = 0;
    long long timer_counter = 0;
    bool timer_enable = false;
    bool timer_pending = false;
    int user_imem_base = 0;
    int user_imem_limit = 0;
    int user_dmem_base = 0;
    int user_dmem_limit = 0;
    int syscall_id = 0;
    bool console_char_mode = false;
    bool mmu_enable = false;
    int user_imem_ptbr = 0;
    int user_imem_pages = 0;
    int user_dmem_ptbr = 0;
    int user_dmem_pages = 0;
    int page_fault_addr = 0;
    int page_fault_access = OS_PAGE_ACCESS_LOAD;
    int asid = 0;
    long long mouse_x = 0;
    long long mouse_y = 0;
    long long mouse_btn = 0;
    long long gpu_x1 = 0;
    long long gpu_y1 = 0;
    long long gpu_x2 = 0;
    long long gpu_y2 = 0;
    long long gpu_color = 0;
    long long gpu_page = 0;
    long long gpu_mode = 0;
    long long sprite_x = 0;
    long long sprite_y = 0;
    long long sprite_attr = 0;
    long long block_index = 0;
    long long block_addr = 0;
    long long block_status = 0;
    int current_process = -1;
};

struct TlbEntry {
    bool valid = false;
    bool global = false;
    int asid = 0;
    int translation_root = 0;
    bool instruction_space = false;
    int vpn = 0;
    int page_words = MMU_PAGE_WORDS;
    int ppn = 0;
    bool user = false;
    bool read = false;
    bool write = false;
    bool execute = false;
    bool accessed = false;
    bool dirty = false;
    std::uint64_t replacement_stamp = 0;
};

struct VMTlbStats {
    std::uint64_t instruction_l1_hits = 0;
    std::uint64_t data_l1_hits = 0;
    std::uint64_t l2_hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t walks = 0;
    std::uint64_t evictions = 0;
    std::uint64_t superpage_hits = 0;
    std::uint64_t shootdowns = 0;

    void reset() { *this = VMTlbStats{}; }
};

struct VMState {
    TernaryRegisterFile      regfile;   // r0..r26 general-purpose registers
    TernaryInstructionMemory imem;      // Instruction memory (Harvard IMEM)
    TernaryMemory            dmem;      // Data memory (Harvard DMEM)
    int                      pc = 0;   // Program counter (word-addressed into imem)
    VMStatus                 status = VMStatus::RUNNING;
    TernaryValue             trap_reg;  // r27: written on fault, read-only from ISA
    std::uint64_t            required_features = 0;
    std::uint64_t            supported_features =
        (std::uint64_t{1} <<
         (architecture::v2::FEATURE_WIDE_T50 + 1)) - 1;
    int                      asid = 0;
    std::array<TlbEntry, architecture::v2::ITLB_ENTRIES> instruction_tlb{};
    std::array<TlbEntry, architecture::v2::DTLB_ENTRIES> data_tlb{};
    std::array<TlbEntry, architecture::v2::L2_TLB_ENTRIES> unified_l2_tlb{};
    VMTlbStats               tlb_stats;
    std::uint64_t            tlb_replacement_clock = 0;
    int                      vector_length = DEFAULT_VECTOR_LENGTH;
    TernaryVectorFile        vregfile;
    VectorFaultState         vector_faults;
    TernaryValue             accumulator;
    std::string              syscall_buffer;
    std::vector<long long>    console_input;
    PrivilegeMode            privilege = PrivilegeMode::Kernel;
    PrivilegeMode            previous_privilege = PrivilegeMode::Kernel;
    bool                     interrupt_enable = false;
    bool                     previous_interrupt_enable = false;
    bool                     trap_routing_enabled = false;
    int                      epc = 0;
    int                      cause = 0;
    int                      tvec = 0;
    int                      scratch = 0;
    long long                cycle_count = 0;
    long long                branch_instructions_count = 0;
    long long                decode_instructions_count = 0;
    long long                timer_reload = 0;
    long long                timer_counter = 0;
    bool                     timer_enable = false;
    bool                     timer_pending = false;
    int                      user_imem_base = 0;
    int                      user_imem_limit = 0;
    int                      user_dmem_base = 0;
    int                      user_dmem_limit = 0;
    int                      syscall_id = 0;
    bool                     console_char_mode = false;
    bool                     mmu_enable = false;
    long long                mouse_x = 0;
    long long                mouse_y = 0;
    long long                mouse_btn = 0;
    long long                gpu_x1 = 0;
    long long                gpu_y1 = 0;
    long long                gpu_x2 = 0;
    long long                gpu_y2 = 0;
    long long                gpu_color = 0;
    long long                gpu_page = 0;
    long long                gpu_mode = 0;
    long long                sprite_x = 0;
    long long                sprite_y = 0;
    long long                sprite_attr = 0;
    long long                block_index = 0;
    long long                block_addr = 0;
    long long                block_status = 0;
    long long                power_control = 0;
    SparseBlockStorage      block_device = SparseBlockStorage(192);
    SparseDirtyBlocks       block_dirty = SparseDirtyBlocks(192, false);
    int                      user_imem_ptbr = 0;
    int                      user_imem_pages = 0;
    int                      user_dmem_ptbr = 0;
    int                      user_dmem_pages = 0;
    int                      page_fault_addr = 0;
    int                      page_fault_access = OS_PAGE_ACCESS_LOAD;
    bool                     atomic_reservation_valid = false;
    int                      atomic_reservation_addr = -1;
    long long                standalone_heap_break = 2000;
    ProductionProfile        profile;
    std::vector<VMCoreState> cores;
    int                      active_core = 0;
    std::vector<std::deque<int>> core_run_queues;
    std::deque<int>          global_run_queue;
    VMExecutionBackend       execution_backend = VMExecutionBackend::CachedBlockInterpreter;
    bool                     block_cache_enabled = true;
    VMBlockCacheStats        block_cache_stats;
    std::uint64_t            block_cache_observed_generation = 0;
    std::unordered_map<int, VMDecodedCacheEntry> decoded_instruction_cache;
    std::unordered_map<int, VMBasicBlock> basic_block_cache;
    int                      trace_jit_hot_threshold = 8;
    VMTraceJitStats          trace_jit_stats;
    std::uint64_t            trace_jit_observed_generation = 0;
    std::uint64_t            executable_mapping_generation = 1;
    std::uint64_t            mmu_generation = 1;
    std::unordered_map<
        VMDecodedTraceCacheKey,
        long long,
        VMDecodedTraceCacheKeyHash> hot_pc_counts;
    std::unordered_map<
        VMDecodedTraceCacheKey,
        VMTraceJitTrace,
        VMDecodedTraceCacheKeyHash> trace_jit_cache;
    std::unordered_set<
        VMDecodedTraceCacheKey,
        VMDecodedTraceCacheKeyHash> trace_jit_unsupported_pcs;
    VMNativeJitStats         native_x64_jit_stats;
    std::unordered_map<
        VMDecodedTraceCacheKey,
        std::shared_ptr<void>,
        VMDecodedTraceCacheKeyHash> native_x64_code_cache;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    VMState()
        : regfile(), imem(DEFAULT_IMEM_SIZE), dmem(DEFAULT_DMEM_SIZE),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        configureCores(1);
        resetControlState();
    }

    explicit VMState(VMStateAllocator& allocator)
        : regfile(), imem(DEFAULT_IMEM_SIZE, allocator), dmem(DEFAULT_DMEM_SIZE, allocator),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        configureCores(1);
        resetControlState();
    }

    VMState(int imem_size, int dmem_size,
            VMStateAllocator& allocator = defaultVMStateAllocator())
        : regfile(), imem(imem_size, allocator), dmem(dmem_size, allocator),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        configureCores(1);
        resetControlState();
    }

    explicit VMState(const ProductionProfile& production,
                     VMStateAllocator& allocator = defaultVMStateAllocator())
        : regfile(),
          imem(production.instruction_words, allocator, MemoryBacking::Sparse),
          dmem(production.ram_words, allocator, MemoryBacking::Sparse),
          pc(0),
          status(VMStatus::RUNNING),
          trap_reg(encodeNoTrap()),
          block_device(production.disk_blocks),
          block_dirty(production.disk_blocks, false),
          accumulator(TernaryValue::zero()),
          profile(production) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        configureCores(production.cores);
        resetControlState();
    }

    // -------------------------------------------------------------------------
    // Reset — returns the machine to power-on state.
    // SP (r26) is initialized to the top of data memory per the stack
    // convention. All other registers are zeroed. PC = 0.
    // Memory contents are NOT cleared — call clearMemory() separately if needed.
    // -------------------------------------------------------------------------
    void reset() {
        regfile.reset();
        pc         = 0;
        status     = VMStatus::RUNNING;
        trap_reg   = encodeNoTrap();
        accumulator = TernaryValue::zero();
        syscall_buffer.clear();
        console_input.clear();
        standalone_heap_break = 2000;
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        resetControlState();

        // Initialize SP to top of data memory.
        // native_ops::fromInt puts a small integer into LongTriple format.
        regfile.write(R26_SP, ops::fromLong(dmem.size() - 1));
        for (auto& core : cores) {
            core = VMCoreState{};
            core.vector_length = vector_length;
            core.vregfile.reset(vector_length);
            core.vector_faults.reset(vector_length);
            core.accumulator = TernaryValue::zero();
            core.user_imem_limit = imem.size();
            core.user_dmem_limit = dmem.size();
        }
        if (!cores.empty()) captureCoreState(0);
    }

    void enqueueConsoleInput(long long word) {
        console_input.push_back(word);
    }

    void enqueueConsoleAscii(const std::string& text) {
        for (unsigned char ch : text) {
            console_input.push_back(static_cast<long long>(ch));
        }
    }

    [[nodiscard]] int consoleInputAvailable() const {
        return static_cast<int>(console_input.size());
    }

    [[nodiscard]] long long peekConsoleInput() const {
        return console_input.empty() ? -1 : console_input.front();
    }

    bool consumeConsoleInput() {
        if (console_input.empty()) return false;
        console_input.erase(console_input.begin());
        return true;
    }

    void clearConsoleInput() {
        console_input.clear();
    }

    void configureCores(int count) {
        const int normalized = std::max(1, count);
        cores.assign(static_cast<std::size_t>(normalized), VMCoreState{});
        for (auto& core : cores) {
            core.vector_length = vector_length;
            core.vregfile.reset(vector_length);
            core.vector_faults.reset(vector_length);
            core.accumulator = TernaryValue::zero();
            core.user_imem_limit = imem.size();
            core.user_dmem_limit = dmem.size();
        }
        core_run_queues.assign(static_cast<std::size_t>(normalized), std::deque<int>{});
        active_core = 0;
        captureCoreState(0);
    }

    [[nodiscard]] int coreCount() const {
        return static_cast<int>(cores.size());
    }

    void captureCoreState(int core_id) {
        if (core_id < 0 || core_id >= coreCount()) return;
        VMCoreState& core = cores[static_cast<std::size_t>(core_id)];
        core.regfile = regfile;
        core.pc = pc;
        core.status = status;
        core.trap_reg = trap_reg;
        core.vector_length = vector_length;
        core.vregfile = vregfile;
        core.vector_faults = vector_faults;
        core.accumulator = accumulator;
        core.privilege = privilege;
        core.previous_privilege = previous_privilege;
        core.interrupt_enable = interrupt_enable;
        core.previous_interrupt_enable = previous_interrupt_enable;
        core.trap_routing_enabled = trap_routing_enabled;
        core.epc = epc;
        core.cause = cause;
        core.tvec = tvec;
        core.scratch = scratch;
        core.timer_reload = timer_reload;
        core.timer_counter = timer_counter;
        core.timer_enable = timer_enable;
        core.timer_pending = timer_pending;
        core.user_imem_base = user_imem_base;
        core.user_imem_limit = user_imem_limit;
        core.user_dmem_base = user_dmem_base;
        core.user_dmem_limit = user_dmem_limit;
        core.syscall_id = syscall_id;
        core.console_char_mode = console_char_mode;
        core.mmu_enable = mmu_enable;
        core.user_imem_ptbr = user_imem_ptbr;
        core.user_imem_pages = user_imem_pages;
        core.user_dmem_ptbr = user_dmem_ptbr;
        core.user_dmem_pages = user_dmem_pages;
        core.page_fault_addr = page_fault_addr;
        core.page_fault_access = page_fault_access;
        core.asid = asid;
        core.mouse_x = mouse_x;
        core.mouse_y = mouse_y;
        core.mouse_btn = mouse_btn;
        core.gpu_x1 = gpu_x1;
        core.gpu_y1 = gpu_y1;
        core.gpu_x2 = gpu_x2;
        core.gpu_y2 = gpu_y2;
        core.gpu_color = gpu_color;
        core.gpu_page = gpu_page;
        core.gpu_mode = gpu_mode;
        core.sprite_x = sprite_x;
        core.sprite_y = sprite_y;
        core.sprite_attr = sprite_attr;
        core.block_index = block_index;
        core.block_addr = block_addr;
        core.block_status = block_status;
    }

    void restoreCoreState(int core_id) {
        if (core_id < 0 || core_id >= coreCount()) return;
        const VMCoreState& core = cores[static_cast<std::size_t>(core_id)];
        regfile = core.regfile;
        pc = core.pc;
        status = core.status;
        trap_reg = core.trap_reg;
        vector_length = core.vector_length;
        vregfile = core.vregfile;
        vector_faults = core.vector_faults;
        accumulator = core.accumulator;
        privilege = core.privilege;
        previous_privilege = core.previous_privilege;
        interrupt_enable = core.interrupt_enable;
        previous_interrupt_enable = core.previous_interrupt_enable;
        trap_routing_enabled = core.trap_routing_enabled;
        epc = core.epc;
        cause = core.cause;
        tvec = core.tvec;
        scratch = core.scratch;
        timer_reload = core.timer_reload;
        timer_counter = core.timer_counter;
        timer_enable = core.timer_enable;
        timer_pending = core.timer_pending;
        user_imem_base = core.user_imem_base;
        user_imem_limit = core.user_imem_limit;
        user_dmem_base = core.user_dmem_base;
        user_dmem_limit = core.user_dmem_limit;
        syscall_id = core.syscall_id;
        console_char_mode = core.console_char_mode;
        mmu_enable = core.mmu_enable;
        user_imem_ptbr = core.user_imem_ptbr;
        user_imem_pages = core.user_imem_pages;
        user_dmem_ptbr = core.user_dmem_ptbr;
        user_dmem_pages = core.user_dmem_pages;
        page_fault_addr = core.page_fault_addr;
        page_fault_access = core.page_fault_access;
        asid = core.asid;
        mouse_x = core.mouse_x;
        mouse_y = core.mouse_y;
        mouse_btn = core.mouse_btn;
        gpu_x1 = core.gpu_x1;
        gpu_y1 = core.gpu_y1;
        gpu_x2 = core.gpu_x2;
        gpu_y2 = core.gpu_y2;
        gpu_color = core.gpu_color;
        gpu_page = core.gpu_page;
        gpu_mode = core.gpu_mode;
        sprite_x = core.sprite_x;
        sprite_y = core.sprite_y;
        sprite_attr = core.sprite_attr;
        block_index = core.block_index;
        block_addr = core.block_addr;
        block_status = core.block_status;
        active_core = core_id;
    }

    [[nodiscard]] VMCoreState& coreState(int core_id) {
        return cores[static_cast<std::size_t>(std::max(0, std::min(core_id, coreCount() - 1)))];
    }

    [[nodiscard]] const VMCoreState& coreState(int core_id) const {
        return cores[static_cast<std::size_t>(std::max(0, std::min(core_id, coreCount() - 1)))];
    }

    void setCoreCurrentProcess(int core_id, int pid) {
        if (core_id < 0 || core_id >= coreCount()) return;
        cores[static_cast<std::size_t>(core_id)].current_process = pid;
    }

    void enqueueProcess(int pid, int preferred_core = -1) {
        if (preferred_core >= 0 && preferred_core < coreCount()) {
            core_run_queues[static_cast<std::size_t>(preferred_core)].push_back(pid);
            return;
        }
        global_run_queue.push_back(pid);
    }

    [[nodiscard]] int dequeueProcess(int core_id) {
        if (core_id < 0 || core_id >= coreCount()) return -1;
        auto& queue = core_run_queues[static_cast<std::size_t>(core_id)];
        if (queue.empty()) loadBalanceRunQueues();
        if (queue.empty()) return -1;
        const int pid = queue.front();
        queue.pop_front();
        return pid;
    }

    void loadBalanceRunQueues() {
        while (!global_run_queue.empty()) {
            int target = 0;
            for (int core = 1; core < coreCount(); ++core) {
                if (core_run_queues[static_cast<std::size_t>(core)].size() <
                    core_run_queues[static_cast<std::size_t>(target)].size()) {
                    target = core;
                }
            }
            core_run_queues[static_cast<std::size_t>(target)].push_back(global_run_queue.front());
            global_run_queue.pop_front();
        }
    }

    [[nodiscard]] std::size_t runQueueDepth(int core_id) const {
        if (core_id < 0 || core_id >= coreCount()) return 0;
        return core_run_queues[static_cast<std::size_t>(core_id)].size();
    }

    void resetBlockDevice(int blocks = 192) {
        const int count = std::max(1, blocks);
        block_device.reset(count);
        block_dirty.assign(static_cast<std::size_t>(count), false);
        block_index = 0;
        block_addr = 0;
        block_status = 0;
        power_control = 0;
    }

    [[nodiscard]] bool loadBlockImage(const std::vector<long long>& image) {
        if (image.empty() ||
            static_cast<int>(image.size()) % STORAGE_BLOCK_WORDS != 0) {
            return false;
        }
        const int blocks =
            static_cast<int>(image.size()) / STORAGE_BLOCK_WORDS;
        resetBlockDevice(blocks);
        return block_device.loadSerialized(image);
    }

    [[nodiscard]] std::vector<long long> blockImage() const {
        return block_device.serializeDense();
    }

    [[nodiscard]] bool attachBlockBackingFile(const std::string& path) {
        return block_device.attachBackingFile(path);
    }

    [[nodiscard]] bool compactBlockBackingFile(bool force = true) {
        return block_device.compactBackingFile(force);
    }

    [[nodiscard]] bool flushBlockBackingFile() {
        return block_device.flushBackingFile();
    }

    [[nodiscard]] std::size_t allocatedDiskBlocks() const {
        return block_device.allocatedBlocks();
    }

    [[nodiscard]] std::size_t pendingDiskWrites() const {
        return block_device.pendingWriteCount();
    }

    [[nodiscard]] int sparseDiskRecordCount() const {
        return block_device.backingRecordCount();
    }

    [[nodiscard]] VMBlockDeviceStats blockDeviceStats() const {
        return block_device.stats();
    }

    void resetBlockDeviceStats() const {
        block_device.resetStats();
    }

    // Clear both memories (set all words to zero / NOP).
    void clearMemory() {
        imem.reset();
        dmem.reset();
        invalidateBlockCache();
    }

    // Full cold reset: register file, PC, status, and both memories.
    void coldReset() {
        reset();
        clearMemory();
    }

    void invalidateBlockCache() {
        const bool had_entries =
            !decoded_instruction_cache.empty() || !basic_block_cache.empty();
        decoded_instruction_cache.clear();
        basic_block_cache.clear();
        block_cache_observed_generation = imem.generation();
        if (had_entries) ++block_cache_stats.invalidations;
    }

    void invalidateTraceJit() {
        const bool had_entries =
            !hot_pc_counts.empty() || !trace_jit_cache.empty() ||
            !trace_jit_unsupported_pcs.empty() ||
            !native_x64_code_cache.empty();
        hot_pc_counts.clear();
        trace_jit_cache.clear();
        trace_jit_unsupported_pcs.clear();
        native_x64_code_cache.clear();
        trace_jit_observed_generation = imem.generation();
        if (had_entries) {
            ++trace_jit_stats.invalidations;
            ++native_x64_jit_stats.invalidations;
        }
    }

    void setExecutionBackend(VMExecutionBackend backend) {
        execution_backend = backend;
        if (backend == VMExecutionBackend::Interpreter) {
            invalidateBlockCache();
            invalidateTraceJit();
        }
    }

    [[nodiscard]] bool configureArchitecture(
        IsaEncodingVersion version,
        std::uint64_t required) {
        if (version != IsaEncodingVersion::V2) return false;
        const std::uint64_t base =
            featureBit(architecture::v2::FEATURE_BASE_V2);
        if ((required & base) == 0 ||
            (required & ~supported_features) != 0) {
            return false;
        }
        required_features = required;
        invalidateBlockCache();
        invalidateTraceJit();
        return true;
    }

    void enterWaiting() {
        status = VMStatus::WAITING;
    }

    void resumeFromEvent() {
        if (status == VMStatus::WAITING) {
            status = VMStatus::RUNNING;
        }

    }
    [[nodiscard]] bool traceJitEnabled() const {
        return decodedTraceExecutorEnabled();
    }

    [[nodiscard]] bool decodedTraceExecutorEnabled() const {
        return execution_backend ==
               VMExecutionBackend::DecodedTraceExecutor;
    }

    [[nodiscard]] bool nativeX64JitEnabled() const {
        return execution_backend == VMExecutionBackend::NativeX64Jit;
    }

    void setTraceJitHotThreshold(int threshold) {
        trace_jit_hot_threshold = std::max(1, threshold);
    }

    void setDecodedTraceHotThreshold(int threshold) {
        setTraceJitHotThreshold(threshold);
    }

    void setBlockCacheEnabled(bool enabled) {
        block_cache_enabled = enabled;
        if (!enabled) invalidateBlockCache();
    }

    void resetBlockCacheStats() {
        block_cache_stats.reset();
        decode_instructions_count = 0;
    }

    void resetTraceJitStats() {
        trace_jit_stats.reset();
        hot_pc_counts.clear();
    }

    void resetDecodedTraceStats() {
        resetTraceJitStats();
    }

    [[nodiscard]] const VMDecodedTraceStats& decodedTraceStats() const {
        return trace_jit_stats;
    }

    [[nodiscard]] double averageBlockCacheLength() const {
        return block_cache_stats.averageBlockLength();
    }

    bool growDataMemoryPreservingStack(int min_size) {
        const int old_size = dmem.size();
        if (min_size <= old_size) return true;

        const long long sp_long = ops::toLong(regfile.readSP());
        if (!dmem.growTo(min_size)) return false;

        if (sp_long >= 0 && sp_long < old_size) {
            const int old_sp = static_cast<int>(sp_long);
            const int delta = min_size - old_size;
            for (int addr = old_size - 1; addr >= old_sp; --addr) {
                auto [value, fault] = dmem.load(addr);
                if (fault != MemFaultCode::OK ||
                    dmem.store(addr + delta, value) != MemFaultCode::OK ||
                    dmem.store(addr, TernaryValue::zero()) != MemFaultCode::OK) {
                    return false;
                }
            }
            regfile.write(R26_SP, ops::fromLong(old_sp + delta));
        }
        return true;
    }

    void resetControlState() {
        privilege = PrivilegeMode::Kernel;
        previous_privilege = PrivilegeMode::Kernel;
        interrupt_enable = false;
        previous_interrupt_enable = false;
        trap_routing_enabled = false;
        epc = 0;
        cause = 0;
        tvec = 0;
        scratch = 0;
        cycle_count = 0;
        branch_instructions_count = 0;
        decode_instructions_count = 0;
        invalidateBlockCache();
        block_cache_stats.reset();
        invalidateTraceJit();
        trace_jit_stats.reset();
        native_x64_jit_stats.reset();
        timer_reload = 0;
        timer_counter = 0;
        timer_enable = false;
        timer_pending = false;
        user_imem_base = 0;
        user_imem_limit = imem.size();
        user_dmem_base = 0;
        user_dmem_limit = dmem.size();
        syscall_id = 0;
        console_char_mode = false;
        mmu_enable = false;
        mouse_x = 0;
        mouse_y = 0;
        asid = 0;
        invalidateAllTlbs(true);
        tlb_stats.reset();
        tlb_replacement_clock = 0;
        mouse_btn = 0;
        gpu_x1 = 0;
        gpu_y1 = 0;
        gpu_x2 = 0;
        gpu_y2 = 0;
        gpu_color = 0;
        gpu_page = 0;
        gpu_mode = 0;
        sprite_x = 0;
        sprite_y = 0;
        sprite_attr = 0;
        block_index = 0;
        block_addr = 0;
        block_status = 0;
        power_control = 0;
        user_imem_ptbr = 0;
        user_imem_pages = 0;
        user_dmem_ptbr = 0;
        user_dmem_pages = 0;
        page_fault_addr = 0;
        page_fault_access = OS_PAGE_ACCESS_LOAD;
        clearAtomicReservation();
    }

    [[nodiscard]] static int privilegeToInt(PrivilegeMode mode) {
        return static_cast<int>(static_cast<int8_t>(mode));
    }

    [[nodiscard]] static PrivilegeMode intToPrivilege(int value) {
        if (value < 0) return PrivilegeMode::Kernel;
        if (value > 0) return PrivilegeMode::User;
        return PrivilegeMode::Supervisor;
    }

    [[nodiscard]] static int8_t tritAt(long long value, int pos) {
        for (int i = 0; i < pos; ++i) {
            long long r = (value + 1) % 3;
            if (r < 0) r += 3;
            long long trit = r - 1;
            value = (value - trit) / 3;
        }
        long long r = (value + 1) % 3;
        if (r < 0) r += 3;
        return static_cast<int8_t>(r - 1);
    }

    [[nodiscard]] long long packStatus() const {
        return privilegeToInt(privilege) +
               3LL * (interrupt_enable ? T_POS : T_ZER) +
               9LL * privilegeToInt(previous_privilege) +
               27LL * (previous_interrupt_enable ? T_POS : T_ZER);
    }

    void unpackStatus(long long value) {
        privilege = intToPrivilege(tritAt(value, 0));
        interrupt_enable = tritAt(value, 1) == T_POS;
        previous_privilege = intToPrivilege(tritAt(value, 2));
        previous_interrupt_enable = tritAt(value, 3) == T_POS;
    }

    [[nodiscard]] bool readCSR(int id, TernaryValue& out) const {
        if (!isValidCSR(id)) return false;
        long long value = 0;
        switch (id) {
            case CSR_EPC: value = epc; break;
            case CSR_CAUSE: value = cause; break;
            case CSR_STATUS: value = packStatus(); break;
            case CSR_TVEC: value = tvec; break;
            case CSR_SCRATCH: value = scratch; break;
            case CSR_CYCLE: value = cycle_count; break;
            case CSR_TIMER_RELOAD: value = timer_reload; break;
            case CSR_TIMER_COUNTER: value = timer_counter; break;
            case CSR_TIMER_ENABLE: value = timer_enable ? 1 : 0; break;
            case CSR_TIMER_PENDING: value = timer_pending ? 1 : 0; break;
            case CSR_USER_IMEM_BASE: value = user_imem_base; break;
            case CSR_USER_IMEM_LIMIT: value = user_imem_limit; break;
            case CSR_USER_DMEM_BASE: value = user_dmem_base; break;
            case CSR_USER_DMEM_LIMIT: value = user_dmem_limit; break;
            case CSR_SYSCALL_ID: value = syscall_id; break;
            case CSR_MMU_ENABLE: value = mmu_enable ? 1 : 0; break;
            case CSR_USER_IMEM_PTBR: value = user_imem_ptbr; break;
            case CSR_USER_IMEM_PAGES: value = user_imem_pages; break;
            case CSR_USER_DMEM_PTBR: value = user_dmem_ptbr; break;
            case CSR_USER_DMEM_PAGES: value = user_dmem_pages; break;
            case CSR_PAGE_FAULT_ADDR: value = page_fault_addr; break;
            case CSR_PAGE_FAULT_ACCESS: value = page_fault_access; break;
            case CSR_CONSOLE_OUT: value = 0; break;
            case CSR_CONSOLE_CTRL: value = static_cast<long long>(syscall_buffer.size()); break;
            case CSR_CONSOLE_IN: value = peekConsoleInput(); break;
            case CSR_CONSOLE_IN_CTRL: value = consoleInputAvailable(); break;
            case CSR_MOUSE_X: value = mouse_x; break;
            case CSR_MOUSE_Y: value = mouse_y; break;
            case CSR_MOUSE_BTN: value = mouse_btn; break;
            case CSR_GPU_X1: value = gpu_x1; break;
            case CSR_GPU_Y1: value = gpu_y1; break;
            case CSR_GPU_X2: value = gpu_x2; break;
            case CSR_GPU_Y2: value = gpu_y2; break;
            case CSR_GPU_COLOR: value = gpu_color; break;
            case CSR_GPU_CMD: value = 0; break;
            case CSR_GPU_PAGE: value = gpu_page; break;
            case CSR_GPU_DRAW_BASE: value = (gpu_page == 0) ? 55000 : 50000; break;
            case CSR_GPU_MODE: value = gpu_mode; break;
            case CSR_SPRITE_X: value = sprite_x; break;
            case CSR_SPRITE_Y: value = sprite_y; break;
            case CSR_SPRITE_ATTR: value = sprite_attr; break;
            case CSR_BLOCK_INDEX: value = block_index; break;
            case CSR_BLOCK_ADDR: value = block_addr; break;
            case CSR_BLOCK_CMD: value = 0; break;
            case CSR_BLOCK_STATUS: value = block_status; break;
            case CSR_BLOCK_COUNT: value = static_cast<long long>(block_device.blockCount()); break;
            case CSR_BLOCK_WORDS: value = STORAGE_BLOCK_WORDS; break;
            case CSR_POWER_CONTROL: value = power_control; break;
            case CSR_ISA_VERSION:
                value = architecture::v2::ISA_VERSION;
                break;
            case CSR_ISA_FEATURES: value = featureWordNumeric(supported_features); break;
            case CSR_MMU_BASE_PAGE_WORDS: value = architecture::v2::BASE_PAGE_WORDS; break;
            case CSR_MMU_SUPERPAGE_WORDS: value = architecture::v2::SUPERPAGE_WORDS; break;
            case CSR_ASID: value = asid; break;
            default: return false;
        }
        out = ops::fromLong(value);
        return true;
    }

    void executeBlockCommand(long long cmd) {
        if (cmd < 0) {
            block_status = 0;
            return;
        }
        if (cmd == 4) {
            block_status = block_device.flushBackingFile() ? 1 : -1;
            if (block_status == 1) {
                block_dirty.assign(block_dirty.size(), false);
            }
            return;
        }
        const int index = static_cast<int>(block_index);
        const int addr = static_cast<int>(block_addr);
        if (index < 0 || index >= block_device.blockCount() || addr < 0) {
            block_status = -1;
            return;
        }
        std::vector<long long> block;
        if (cmd == 1) {
            if (addr + STORAGE_BLOCK_WORDS > dmem.size()) {
                block_status = -1;
                return;
            }
            if (!block_device.readBlock(index, block)) {
                block_status = -1;
                return;
            }
            for (int i = 0; i < STORAGE_BLOCK_WORDS; ++i) {
                if (dmem.store(addr + i, ops::fromLong(block[static_cast<std::size_t>(i)])) !=
                    MemFaultCode::OK) {
                    block_status = -1;
                    return;
                }
            }
            block_status = 1;
        } else if (cmd == 2) {
            if (addr + STORAGE_BLOCK_WORDS > dmem.size()) {
                block_status = -1;
                return;
            }
            for (int i = 0; i < STORAGE_BLOCK_WORDS; ++i) {
                auto [value, fault] = dmem.load(addr + i);
                if (fault != MemFaultCode::OK) {
                    block_status = -1;
                    return;
                }
                block.push_back(ops::toLong(value));
            }
            if (!block_device.writeBlock(index, block)) {
                block_status = -1;
                return;
            }
            block_dirty.set(index, true);
            block_status = 1;
        } else if (cmd == 3) {
            if (addr + STORAGE_BLOCK_WORDS > imem.size()) {
                block_status = -1;
                return;
            }
            if (!block_device.readBlock(index, block)) {
                block_status = -1;
                return;
            }
            for (int i = 0; i < STORAGE_BLOCK_WORDS; ++i) {
                TritWord27 word{};
                word.bits = static_cast<uint64_t>(block[static_cast<std::size_t>(i)]);
                if (imem.write(addr + i, word) != MemFaultCode::OK) {
                    block_status = -1;
                    return;
                }
            }
            block_status = 1;
        } else {
            block_status = -1;
        }
    }

    void executeGpuCommand(long long cmd) {
        int base_addr = (gpu_page == 0) ? 55000 : 50000;
        int limit_addr = base_addr + 4800; // 80 * 60 = 4800

        if (cmd == 1) { // Clear screen
            for (int i = base_addr; i < limit_addr; ++i) {
                if (i >= 0 && i < dmem.size()) {
                    (void)dmem.store(i, ops::fromLong(gpu_color));
                }
            }
        } else if (cmd == 2) { // Rect fill
            int x_min = static_cast<int>(std::min(gpu_x1, gpu_x2));
            int x_max = static_cast<int>(std::max(gpu_x1, gpu_x2));
            int y_min = static_cast<int>(std::min(gpu_y1, gpu_y2));
            int y_max = static_cast<int>(std::max(gpu_y1, gpu_y2));
            for (int y = y_min; y <= y_max; ++y) {
                for (int x = x_min; x <= x_max; ++x) {
                    if (x >= 0 && x < 80 && y >= 0 && y < 60) {
                        int addr = base_addr + y * 80 + x;
                        if (addr >= 0 && addr < dmem.size()) {
                            (void)dmem.store(addr, ops::fromLong(gpu_color));
                        }
                    }
                }
            }
        } else if (cmd == 3) { // Bresenham Line
            int x0 = static_cast<int>(gpu_x1), y0 = static_cast<int>(gpu_y1);
            int x1 = static_cast<int>(gpu_x2), y1 = static_cast<int>(gpu_y2);
            int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            int err = dx + dy, e2;
            while (true) {
                if (x0 >= 0 && x0 < 80 && y0 >= 0 && y0 < 60) {
                        int addr = base_addr + y0 * 80 + x0;
                        if (addr >= 0 && addr < dmem.size()) {
                            (void)dmem.store(addr, ops::fromLong(gpu_color));
                        }
                }
                if (x0 == x1 && y0 == y1) break;
                e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        } else if (cmd == 4) { // Rect border outline
            int x_min = static_cast<int>(std::min(gpu_x1, gpu_x2));
            int x_max = static_cast<int>(std::max(gpu_x1, gpu_x2));
            int y_min = static_cast<int>(std::min(gpu_y1, gpu_y2));
            int y_max = static_cast<int>(std::max(gpu_y1, gpu_y2));
            for (int x = x_min; x <= x_max; ++x) {
                if (x >= 0 && x < 80) {
                    if (y_min >= 0 && y_min < 60) {
                        int addr = base_addr + y_min * 80 + x;
                        if (addr >= 0 && addr < dmem.size()) (void)dmem.store(addr, ops::fromLong(gpu_color));
                    }
                    if (y_max >= 0 && y_max < 60) {
                        int addr = base_addr + y_max * 80 + x;
                        if (addr >= 0 && addr < dmem.size()) (void)dmem.store(addr, ops::fromLong(gpu_color));
                    }
                }
            }
            for (int y = y_min; y <= y_max; ++y) {
                if (y >= 0 && y < 60) {
                    if (x_min >= 0 && x_min < 80) {
                        int addr = base_addr + y * 80 + x_min;
                        if (addr >= 0 && addr < dmem.size()) (void)dmem.store(addr, ops::fromLong(gpu_color));
                    }
                    if (x_max >= 0 && x_max < 80) {
                        int addr = base_addr + y * 80 + x_max;
                        if (addr >= 0 && addr < dmem.size()) (void)dmem.store(addr, ops::fromLong(gpu_color));
                    }
                }
            }
        }
    }

    [[nodiscard]] bool writeCSR(int id, TernaryValue in) {
        if (!isValidCSR(id) || !isNumericMode(in.mode) || in.isInvalid()) return false;
        long long value = ops::toLong(in);
        switch (id) {
            case CSR_EPC:
                epc = static_cast<int>(value);
                return true;
            case CSR_CAUSE:
                cause = static_cast<int>(value);
                return true;
            case CSR_STATUS:
                unpackStatus(value);
                return true;
            case CSR_TVEC:
                if (value < 0 || value >= imem.size()) return false;
                tvec = static_cast<int>(value);
                trap_routing_enabled = true;
                return true;
            case CSR_SCRATCH:
                scratch = static_cast<int>(value);
                return true;
            case CSR_CYCLE:
                return false;
            case CSR_TIMER_RELOAD:
                if (value < 0) return false;
                timer_reload = value;
                return true;
            case CSR_TIMER_COUNTER:
                if (value < 0) return false;
                timer_counter = value;
                return true;
            case CSR_TIMER_ENABLE:
                timer_enable = value != 0;
                if (timer_enable && timer_counter == 0 && timer_reload > 0) {
                    timer_counter = timer_reload;
                }
                return true;
            case CSR_TIMER_PENDING:
                timer_pending = value != 0;
                return true;
            case CSR_USER_IMEM_BASE:
                user_imem_base = static_cast<int>(value);
                ++executable_mapping_generation;
                return true;
            case CSR_USER_IMEM_LIMIT:
                user_imem_limit = static_cast<int>(value);
                ++executable_mapping_generation;
                return true;
            case CSR_USER_DMEM_BASE:
                user_dmem_base = static_cast<int>(value);
                return true;
            case CSR_USER_DMEM_LIMIT:
                user_dmem_limit = static_cast<int>(value);
                return true;
            case CSR_SYSCALL_ID:
                syscall_id = static_cast<int>(value);
                return true;
            case CSR_MMU_ENABLE:
                mmu_enable = value != 0;
                ++mmu_generation;
                return true;
            case CSR_USER_IMEM_PTBR:
                if (value < 0) return false;
                user_imem_ptbr = static_cast<int>(value);
                ++executable_mapping_generation;
                ++mmu_generation;
                return true;
            case CSR_USER_IMEM_PAGES:
                if (value < 0) return false;
                user_imem_pages = static_cast<int>(value);
                ++executable_mapping_generation;
                ++mmu_generation;
                return true;
            case CSR_USER_DMEM_PTBR:
                if (value < 0) return false;
                user_dmem_ptbr = static_cast<int>(value);
                ++mmu_generation;
                setCurrentAsid(static_cast<int>(value % 19683));
                return true;
            case CSR_USER_DMEM_PAGES:
                if (value < 0) return false;
                user_dmem_pages = static_cast<int>(value);
                ++mmu_generation;
                return true;
            case CSR_PAGE_FAULT_ADDR:
                page_fault_addr = static_cast<int>(value);
                return true;
            case CSR_PAGE_FAULT_ACCESS:
                page_fault_access = static_cast<int>(value);
                return true;
            case CSR_CONSOLE_OUT:
                if (console_char_mode) {
                    syscall_buffer += static_cast<char>(value);
                } else {
                    syscall_buffer += std::to_string(value);
                }
                return true;
            case CSR_CONSOLE_CTRL:
                if (value == 2) {
                    console_char_mode = true;
                } else if (value == 3) {
                    console_char_mode = false;
                } else if (value < 0) {
                    syscall_buffer.clear();
                } else if (value > 0) {
                    syscall_buffer += "\n";
                }
                return true;
            case CSR_CONSOLE_IN:
                return false;
            case CSR_CONSOLE_IN_CTRL:
                if (value < 0) {
                    clearConsoleInput();
                } else if (value > 0) {
                    consumeConsoleInput();
                }
                return true;
            case CSR_MOUSE_X:
                mouse_x = value;
                return true;
            case CSR_MOUSE_Y:
                mouse_y = value;
                return true;
            case CSR_MOUSE_BTN:
                mouse_btn = value;
                return true;
            case CSR_GPU_X1:
                gpu_x1 = value;
                return true;
            case CSR_GPU_Y1:
                gpu_y1 = value;
                return true;
            case CSR_GPU_X2:
                gpu_x2 = value;
                return true;
            case CSR_GPU_Y2:
                gpu_y2 = value;
                return true;
            case CSR_GPU_COLOR:
                gpu_color = value;
                return true;
            case CSR_GPU_CMD:
                executeGpuCommand(value);
                return true;
            case CSR_GPU_PAGE:
                gpu_page = value;
                return true;
            case CSR_GPU_DRAW_BASE:
                return false; // read-only
            case CSR_GPU_MODE:
                gpu_mode = value;
                return true;
            case CSR_SPRITE_X:
                sprite_x = value;
                return true;
            case CSR_SPRITE_Y:
                sprite_y = value;
                return true;
            case CSR_SPRITE_ATTR:
                sprite_attr = value;
                return true;
            case CSR_BLOCK_INDEX:
                block_index = value;
                return true;
            case CSR_BLOCK_ADDR:
                block_addr = value;
                return true;
            case CSR_BLOCK_CMD:
                executeBlockCommand(value);
                return true;
            case CSR_BLOCK_STATUS:
                block_status = value;
                return true;
            case CSR_BLOCK_COUNT:
            case CSR_BLOCK_WORDS:
                return false;
            case CSR_POWER_CONTROL:
                if (value < 0 || value > 1) return false;
                power_control = value;
                return true;
            default:
            case CSR_ISA_VERSION:
            case CSR_ISA_FEATURES:
            case CSR_MMU_BASE_PAGE_WORDS:
            case CSR_MMU_SUPERPAGE_WORDS:
            case CSR_ASID:
                return false;
                return false;
        }
    }

    [[nodiscard]] bool inUserRange(int addr, int base, int limit, int capacity) const {
        return addr >= 0 && addr < capacity && addr >= base && addr < limit;
    }

    [[nodiscard]] bool canFetch(int addr) const {
        if (!imem.inRange(addr)) return false;
        if (privilege == PrivilegeMode::Kernel) return true;
        return inUserRange(addr, user_imem_base, user_imem_limit, imem.size());
    }

    [[nodiscard]] bool canLoad(int addr) const {
        if (!dmem.inRange(addr)) return false;
        if (privilege == PrivilegeMode::Kernel) return true;
        return inUserRange(addr, user_dmem_base, user_dmem_limit, dmem.size());
    }

    [[nodiscard]] bool canStore(int addr) const {
        return canLoad(addr);
    }

    template <std::size_t EntryCount>
    static void invalidateTlbArray(
        std::array<TlbEntry, EntryCount>& entries,
        bool include_global) {
        for (auto& entry : entries) {
            if (include_global || !entry.global) entry.valid = false;
        }
    }

    void invalidateAllTlbs(bool include_global = false) {
        invalidateTlbArray(instruction_tlb, include_global);
        invalidateTlbArray(data_tlb, include_global);
        invalidateTlbArray(unified_l2_tlb, include_global);
        ++mmu_generation;
    }

    void setCurrentAsid(int next_asid) {
        if (next_asid < 0 || next_asid >= 19683)
            throw std::out_of_range("ASID exceeds nine-trit range");
        // Switching ASIDs retains unrelated entries. Reuse is paired with an
        // explicit TLBINV by the process/ASID allocator.
        asid = next_asid;
    }

    void invalidateTlb(int address, int target_asid, int scope) {
        auto invalidate = [&](auto& entries) {
            for (auto& entry : entries) {
                if (!entry.valid) continue;
                const bool address_match =
                    address < 0 ||
                    address / entry.page_words == entry.vpn;
                const bool asid_match =
                    entry.global || target_asid < 0 ||
                    entry.asid == target_asid;
                bool remove = false;
                switch (scope) {
                    case 0: remove = address_match && asid_match; break;
                    case 1: remove = asid_match && !entry.global; break;
                    case 2: remove = address_match; break;
                    case 3: remove = true; break;
                    default: break;
                }
                if (remove) entry.valid = false;
            }
        };
        invalidate(instruction_tlb);
        invalidate(data_tlb);
        invalidate(unified_l2_tlb);
        ++tlb_stats.shootdowns;
        ++mmu_generation;
    }

    template <std::size_t EntryCount>
    TlbEntry* findTlbEntry(
        std::array<TlbEntry, EntryCount>& entries,
        int sets,
        int ways,
        int virtual_addr,
        int page_words,
        int translation_root,
        bool instruction_space) {
        const int vpn = virtual_addr / page_words;
        const int set = vpn % sets;
        for (int way = 0; way < ways; ++way) {
            TlbEntry& entry =
                entries[static_cast<std::size_t>(set * ways + way)];
            if (entry.valid && entry.vpn == vpn &&
                entry.page_words == page_words &&
                entry.instruction_space == instruction_space &&
                (entry.global ||
                 (entry.asid == asid &&
                  entry.translation_root == translation_root))) {
                entry.replacement_stamp = ++tlb_replacement_clock;
                return &entry;
            }
        }
        return nullptr;
    }

    template <std::size_t EntryCount>
    void insertTlbEntry(
        std::array<TlbEntry, EntryCount>& entries,
        int sets,
        int ways,
        TlbEntry entry) {
        const int set = entry.vpn % sets;
        TlbEntry* victim = nullptr;
        for (int way = 0; way < ways; ++way) {
            TlbEntry& candidate =
                entries[static_cast<std::size_t>(set * ways + way)];
            if (!candidate.valid) {
                victim = &candidate;
                break;
            }
            if (victim == nullptr ||
                candidate.replacement_stamp < victim->replacement_stamp) {
                victim = &candidate;
            }
        }
        if (victim == nullptr) return;
        if (victim->valid) ++tlb_stats.evictions;
        entry.replacement_stamp = ++tlb_replacement_clock;
        *victim = entry;
    }

    void setPageFault(int virtual_addr, int access) {
        page_fault_addr = virtual_addr;
        page_fault_access = access;
    }

    [[nodiscard]] bool translateUserPageAddress(
        int virtual_addr,
        int ptbr,
        int page_count,
        bool need_read,
        bool need_write,
        bool need_execute,
        int page_fault_cause,
        int protection_cause,
        int access,
        int target_capacity,
        int& physical_addr,
        int& routed_cause) {

        if (virtual_addr < 0) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        const int base_page_words = MMU_PAGE_WORDS;
        const int vpn = virtual_addr / base_page_words;
        if (vpn < 0 || vpn >= page_count) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        auto permissionFault = [&](const TlbEntry& entry) {
            return !entry.user ||
                   (need_read && !entry.read) ||
                   (need_write && !entry.write) ||
                   (need_execute && !entry.execute);
        };
        auto& l1 = need_execute ? instruction_tlb : data_tlb;
        TlbEntry* cached = nullptr;
        cached = findTlbEntry(
            l1, 9, 3, virtual_addr, MMU_SUPERPAGE_WORDS,
            ptbr, need_execute);
        if (cached == nullptr) {
            cached = findTlbEntry(
                l1, 9, 3, virtual_addr, base_page_words,
                ptbr, need_execute);
        }
        if (cached != nullptr && need_write && !cached->dirty) {
            cached->valid = false;
            cached = nullptr;
        }
        if (cached != nullptr) {
            if (need_execute) ++tlb_stats.instruction_l1_hits;
            else ++tlb_stats.data_l1_hits;
            if (cached->page_words == MMU_SUPERPAGE_WORDS)
                ++tlb_stats.superpage_hits;
            if (permissionFault(*cached)) {
                setPageFault(virtual_addr, access);
                routed_cause = protection_cause;
                return false;
            }
            const long long phys =
                static_cast<long long>(cached->ppn) * base_page_words +
                virtual_addr % cached->page_words;
            if (phys < 0 || phys >= target_capacity) {
                setPageFault(virtual_addr, access);
                routed_cause = page_fault_cause;
                return false;
            }
            physical_addr = static_cast<int>(phys);
            return true;
        }

        cached = findTlbEntry(
            unified_l2_tlb, 27, 9, virtual_addr,
            MMU_SUPERPAGE_WORDS, ptbr, need_execute);
        if (cached == nullptr) {
            cached = findTlbEntry(
                unified_l2_tlb, 27, 9, virtual_addr,
                base_page_words, ptbr, need_execute);
        }
        if (cached != nullptr && need_write && !cached->dirty) {
            cached->valid = false;
            cached = nullptr;
        }
        if (cached != nullptr) {
            ++tlb_stats.l2_hits;
            if (cached->page_words == MMU_SUPERPAGE_WORDS)
                ++tlb_stats.superpage_hits;
            insertTlbEntry(l1, 9, 3, *cached);
            if (permissionFault(*cached)) {
                setPageFault(virtual_addr, access);
                routed_cause = protection_cause;
                return false;
            }
            const long long phys =
                static_cast<long long>(cached->ppn) * base_page_words +
                virtual_addr % cached->page_words;
            if (phys < 0 || phys >= target_capacity) {
                setPageFault(virtual_addr, access);
                routed_cause = page_fault_cause;
                return false;
            }
            physical_addr = static_cast<int>(phys);
            return true;
        }

        ++tlb_stats.misses;
        ++tlb_stats.walks;
        int pte_vpn = vpn;
        int pte_addr = ptbr + pte_vpn;
        PageTableEntry pte;
        TernaryValue pte_value;
        bool decoded = false;
        const int superpage_base_vpn = (vpn / 27) * 27;
        const int superpage_pte_addr = ptbr + superpage_base_vpn;
        if (dmem.inRange(superpage_pte_addr)) {
            auto [candidate, fault] = dmem.load(superpage_pte_addr);
            if (fault == MemFaultCode::OK &&
                decodePageTableEntryV2(candidate, pte) &&
                pte.present && pte.superpage) {
                pte_vpn = superpage_base_vpn;
                pte_addr = superpage_pte_addr;
                pte_value = candidate;
                decoded = true;
            }
        }
        if (!decoded) {
            pte_addr = ptbr + vpn;
            if (!dmem.inRange(pte_addr)) {
                setPageFault(virtual_addr, access);
                routed_cause = page_fault_cause;
                return false;
            }
            auto [candidate, fault] = dmem.load(pte_addr);
            if (fault != MemFaultCode::OK ||
                !decodePageTableEntryV2(candidate, pte) ||
                !pte.present) {
                setPageFault(virtual_addr, access);
                routed_cause = page_fault_cause;
                return false;
            }
            pte_value = candidate;
        }
        if (!pte.user ||
            (need_read && !pte.read) ||
            (need_write && !pte.write) ||
            (need_execute && !pte.execute)) {
            setPageFault(virtual_addr, access);
            routed_cause = protection_cause;
            return false;
        }
        if (!pte.accessed || (need_write && !pte.dirty)) {
            pte.accessed = true;
            if (need_write) pte.dirty = true;
            const TernaryValue updated = encodePageTableEntry(pte);
            if (updated.isInvalid() ||
                dmem.store(pte_addr, updated) != MemFaultCode::OK) {
                setPageFault(virtual_addr, access);
                routed_cause = page_fault_cause;
                return false;
            }
            pte_value = updated;
        }
        (void)pte_value;
        TlbEntry entry;
        entry.valid = true;
        entry.global = pte.global;
        entry.asid = asid;
        entry.translation_root = ptbr;
        entry.instruction_space = need_execute;
        entry.page_words =
            pte.superpage ? MMU_SUPERPAGE_WORDS : base_page_words;
        entry.vpn = virtual_addr / entry.page_words;
        entry.ppn = pte.ppn;
        entry.user = pte.user;
        entry.read = pte.read;
        entry.write = pte.write;
        entry.execute = pte.execute;
        entry.accessed = pte.accessed;
        entry.dirty = pte.dirty;
        insertTlbEntry(unified_l2_tlb, 27, 9, entry);
        insertTlbEntry(l1, 9, 3, entry);
        const long long phys =
            static_cast<long long>(pte.ppn) * base_page_words +
            virtual_addr % entry.page_words;
        if (phys < 0 || phys >= target_capacity) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        physical_addr = static_cast<int>(phys);
        return true;
    }

    [[nodiscard]] bool translateFetchAddress(int virtual_pc, int& physical_pc, int& routed_cause) {
        if (privilege == PrivilegeMode::Kernel) {
            if (!imem.inRange(virtual_pc)) {
                setPageFault(virtual_pc, OS_PAGE_ACCESS_FETCH);
                routed_cause = OS_CAUSE_FETCH_FAULT;
                return false;
            }
            physical_pc = virtual_pc;
            return true;
        }
        if (!mmu_enable) {
            if (!canFetch(virtual_pc)) {
                setPageFault(virtual_pc, OS_PAGE_ACCESS_FETCH);
                routed_cause = OS_CAUSE_FETCH_FAULT;
                return false;
            }
            physical_pc = virtual_pc;
            return true;
        }
        return translateUserPageAddress(
            virtual_pc,
            user_imem_ptbr,
            user_imem_pages,
            false,
            false,
            true,
            OS_CAUSE_FETCH_PAGE_FAULT,
            OS_CAUSE_PROTECTION_FAULT,
            OS_PAGE_ACCESS_FETCH,
            imem.size(),
            physical_pc,
            routed_cause);
    }

    [[nodiscard]] bool translateLoadAddress(int virtual_addr, int& physical_addr, int& routed_cause) {
        if (privilege == PrivilegeMode::Kernel) {
            if (!dmem.inRange(virtual_addr)) {
                setPageFault(virtual_addr, OS_PAGE_ACCESS_LOAD);
                routed_cause = OS_CAUSE_LOAD_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        if (!mmu_enable) {
            if (!canLoad(virtual_addr)) {
                setPageFault(virtual_addr, OS_PAGE_ACCESS_LOAD);
                routed_cause = OS_CAUSE_LOAD_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        return translateUserPageAddress(
            virtual_addr,
            user_dmem_ptbr,
            user_dmem_pages,
            true,
            false,
            false,
            OS_CAUSE_LOAD_PAGE_FAULT,
            OS_CAUSE_PROTECTION_FAULT,
            OS_PAGE_ACCESS_LOAD,
            dmem.size(),
            physical_addr,
            routed_cause);
    }

    [[nodiscard]] bool translateStoreAddress(int virtual_addr, int& physical_addr, int& routed_cause) {
        if (privilege == PrivilegeMode::Kernel) {
            if (!dmem.inRange(virtual_addr)) {
                setPageFault(virtual_addr, OS_PAGE_ACCESS_STORE);
                routed_cause = OS_CAUSE_STORE_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        if (!mmu_enable) {
            if (!canStore(virtual_addr)) {
                setPageFault(virtual_addr, OS_PAGE_ACCESS_STORE);
                routed_cause = OS_CAUSE_STORE_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        return translateUserPageAddress(
            virtual_addr,
            user_dmem_ptbr,
            user_dmem_pages,
            false,
            true,
            false,
            OS_CAUSE_STORE_PAGE_FAULT,
            OS_CAUSE_PROTECTION_FAULT,
            OS_PAGE_ACCESS_STORE,
            dmem.size(),
            physical_addr,
            routed_cause);
    }

    void clearAtomicReservation() {
        atomic_reservation_valid = false;
        atomic_reservation_addr = -1;
    }

    void setAtomicReservation(int physical_addr) {
        atomic_reservation_valid = true;
        atomic_reservation_addr = physical_addr;
    }

    void noteStoreForReservation(int physical_addr) {
        if (atomic_reservation_valid && atomic_reservation_addr == physical_addr) {
            clearAtomicReservation();
        }
    }

    [[nodiscard]] bool validateControlTarget(int target) {
        if (target < 0) return false;
        if (privilege == PrivilegeMode::Kernel) return imem.inRange(target);
        if (mmu_enable) return true;
        return canFetch(target);
    }

    [[nodiscard]] static int osCauseForTrap(TrapCode code) {
        switch (code) {
            case TrapCode::TRAP_DIV_ZERO: return OS_CAUSE_DIV_ZERO;
            case TrapCode::TRAP_MEM_FAULT: return OS_CAUSE_LOAD_FAULT;
            case TrapCode::TRAP_ILLEGAL_OP: return OS_CAUSE_ILLEGAL_INSTRUCTION;
        }
        return OS_CAUSE_ILLEGAL_INSTRUCTION;
    }

    void trapWithCause(TrapCode legacy_code, int routed_cause, int epc_value) {
        clearAtomicReservation();
        trap_reg = encodeTrap(legacy_code);
        if (!trap_routing_enabled) {
            status = VMStatus::TRAPPED;
            return;
        }
        epc = epc_value;
        cause = routed_cause;
        previous_privilege = privilege;
        previous_interrupt_enable = interrupt_enable;
        privilege = PrivilegeMode::Kernel;
        interrupt_enable = false;
        status = VMStatus::RUNNING;
        pc = tvec;
    }

    void recordCycle(bool allow_timer_interrupt = true) {
        ++cycle_count;
        if (timer_enable) {
            if (timer_counter > 0) --timer_counter;
            if (timer_counter == 0) {
                timer_pending = true;
                if (timer_reload > 0) {
                    timer_counter = timer_reload;
                } else {
                    timer_enable = false;
                }
            }
        }
        if (allow_timer_interrupt && timer_pending && trap_routing_enabled && interrupt_enable) {
            timer_pending = false;
            trapWithCause(TrapCode::TRAP_ILLEGAL_OP, OS_CAUSE_TIMER_IRQ, pc);
        }
    }

    void completeInstruction(int next_pc) {
        pc = next_pc;
        recordCycle(true);
    }

    void completeTerminalInstruction() {
        recordCycle(false);
    }

    [[nodiscard]] bool returnFromTrap() {
        if (privilege != PrivilegeMode::Kernel) return false;
        privilege = previous_privilege;
        interrupt_enable = previous_interrupt_enable;
        pc = epc;
        return true;
    }

    // -------------------------------------------------------------------------
    // Trap — called by the dispatcher on any fault.
    // Sets status to TRAPPED, writes the trap code to r27, and records
    // the PC at the faulting instruction for post-mortem inspection.
    // -------------------------------------------------------------------------
    void trap(TrapCode code) {
        trapWithCause(code, osCauseForTrap(code), pc);
        // Legacy mode leaves pc at the faulting instruction. Routed mode stores
        // that PC in EPC and jumps to TVEC.
    }

    // -------------------------------------------------------------------------
    // Halt — called by the dispatcher on HALT opcode.
    // -------------------------------------------------------------------------
    void halt() {
        status = VMStatus::HALTED;
        // pc is NOT advanced: it points at the HALT instruction.
    }

    // -------------------------------------------------------------------------
    // State predicates
    // -------------------------------------------------------------------------
    [[nodiscard]] bool isRunning()  const { return status == VMStatus::RUNNING; }
    [[nodiscard]] bool isHalted()   const { return status == VMStatus::HALTED;  }
    [[nodiscard]] bool isTrapped()  const { return status == VMStatus::TRAPPED; }

    [[nodiscard]] bool isWaiting()  const { return status == VMStatus::WAITING; }
    // -------------------------------------------------------------------------
    // Debug dump — human-readable machine state summary.
    // -------------------------------------------------------------------------

    [[nodiscard]] std::string dump() const {
        std::ostringstream oss;
        oss << "=== VMState ==========================\n";
        oss << "  PC:     " << pc << "\n";
        oss << "  Status: " << vmStatusToString(status) << "\n";
        if (isTrapped()) {
            TrapCode tc = decodeTrap(trap_reg);
            std::string tc_str;
            switch (tc) {
                case TrapCode::TRAP_DIV_ZERO:   tc_str = "TRAP_DIV_ZERO";   break;
                case TrapCode::TRAP_MEM_FAULT:  tc_str = "TRAP_MEM_FAULT";  break;
                case TrapCode::TRAP_ILLEGAL_OP: tc_str = "TRAP_ILLEGAL_OP"; break;
            }
            oss << "  Trap:   " << tc_str << "\n";
        }
        oss << "  Registers (non-zero):\n";
        for (int i = 1; i < REG_COUNT; ++i) {
            TernaryValue v = regfile.read(static_cast<uint8_t>(i));
            if (!v.isZero()) {
                oss << "    r" << i << " = [non-zero]\n";
            }
        }
        oss << "======================================\n";
        return oss.str();
    }
};

// A deterministic, in-memory VM checkpoint.  The checkpoint owns a complete
// architectural state copy (registers, memories, MMU/TLB state, queues, and
// the sparse block device) so restoring it does not depend on the live VM's
// mutable caches or backing-file cursor.  Decoded/native code caches are
// intentionally discarded: they are derived state and may contain pointers
// into the pre-checkpoint machine.
struct VMCheckpoint {
    static constexpr const char* kSchema = "trit.vm_checkpoint.v1";

    std::uint64_t cycle = 0;
    int pc = 0;
    VMState state;

    VMCheckpoint() = default;

    explicit VMCheckpoint(const VMState& source)
        : cycle(static_cast<std::uint64_t>(std::max<long long>(
              0, source.cycle_count))),
          pc(source.pc),
          state(source) {
        state.invalidateBlockCache();
        state.invalidateTraceJit();
    }
};

[[nodiscard]] inline VMCheckpoint captureCheckpoint(const VMState& vm) {
    return VMCheckpoint(vm);
}

inline bool restoreCheckpoint(VMState& vm, const VMCheckpoint& checkpoint) {
    vm = checkpoint.state;
    // Assignment copies the architectural state, but all decoded/native
    // artifacts must be rebuilt against this VM instance before execution.
    vm.invalidateBlockCache();
    vm.invalidateTraceJit();
    return vm.pc == checkpoint.pc &&
           static_cast<std::uint64_t>(std::max<long long>(
               0, vm.cycle_count)) == checkpoint.cycle;
}

// =============================================================================
// SECTION 10 — Program Loading Helpers
// =============================================================================
// Convenience wrappers for building test programs and loading them into a
// VMState. The assembler (Phase 4) will supersede these for complex programs,
// but they're sufficient for Phase 3 dispatcher testing.

// Append one instruction word to a program vector.
inline void emit(std::vector<TritWord27>& prog, TritWord27 w) {
    prog.push_back(w);
}

// Emit shortcuts for all formats:
inline void emitR(std::vector<TritWord27>& prog, Opcode op,
                  uint8_t rd, uint8_t rs1, uint8_t rs2, uint8_t func = FUNC_DEFAULT) {
    // FUNC_DEFAULT maps to native arithmetic.
    prog.push_back(InstructionWord::encodeR(op, rd, rs1, rs2, func));
}

inline void emitI(std::vector<TritWord27>& prog, Opcode op,
                  uint8_t rd, uint8_t rs1, int imm) {
    prog.push_back(InstructionWord::encodeI(op, rd, rs1, imm));
}

inline void emitB(std::vector<TritWord27>& prog, Opcode op,
                  uint8_t rs, int offset) {
    prog.push_back(InstructionWord::encodeB(op, rs, offset));
}

// Load a program into a VMState and perform a cold reset.
// Returns false if the program exceeds IMEM capacity.
inline bool loadAndReset(VMState& vm, const std::vector<TritWord27>& program) {
    vm.coldReset();
    return vm.imem.loadProgram(program, 0);
}

// =============================================================================
// SECTION 11 — Phase 2 Verification
// =============================================================================
// Confirms that register file, memory, and state machinery work correctly
// before the Phase 3 dispatcher is added. Call in a unit test.

inline bool verifyVMState() {
    bool ok = true;

    // --- Register file: r0 hardwired zero ---
    {
        TernaryRegisterFile rf;
        rf.write(R0_ZERO, ops::fromLong(999));       // write to r0
        ok &= rf.read(R0_ZERO).isZero();             // must still read zero
        rf.write(3, ops::fromLong(42));
        ok &= (ops::toLong(rf.read(3)) == 42LL);     // r3 stores correctly
        rf.reset();
        ok &= rf.read(3).isZero();                   // reset clears r3
    }

    // --- Data memory: load/store round-trip ---
    {
        TernaryMemory mem(64);
        TernaryValue val = ops::fromLong(12345);
        ok &= (mem.store(0,  val) == MemFaultCode::OK);
        ok &= (mem.store(63, val) == MemFaultCode::OK);
        ok &= (mem.store(64, val) == MemFaultCode::OUT_OF_RANGE);
        ok &= (mem.store(-1, val) == MemFaultCode::OUT_OF_RANGE);
        auto [loaded, fc] = mem.load(0);
        ok &= (fc == MemFaultCode::OK);
        ok &= (ops::toLong(loaded) == 12345LL);
        auto [bad, fc2] = mem.load(64);
        ok &= (fc2 == MemFaultCode::OUT_OF_RANGE);
        ok &= bad.isZero();
    }

    // --- Instruction memory: write / fetch round-trip ---
    {
        TernaryInstructionMemory imem(8);
        TritWord27 add_instr = InstructionWord::encodeR(Opcode::ADD, R3, R1, R2);
        ok &= (imem.write(0, add_instr) == MemFaultCode::OK);
        ok &= (imem.write(8, add_instr) == MemFaultCode::OUT_OF_RANGE);
        auto [fetched, fc] = imem.fetch(0);
        ok &= (fc == MemFaultCode::OK);
        ok &= (fetched == add_instr);
        auto [bad_fetch, fc2] = imem.fetch(8);
        ok &= (fc2 == MemFaultCode::OUT_OF_RANGE);
    }

    // --- loadProgram ---
    {
        TernaryInstructionMemory imem(4);
        std::vector<TritWord27> prog = {
            InstructionWord::encodeI(Opcode::MOV,  R1, R0_ZERO, 7),
            InstructionWord::encodeI(Opcode::MOV,  R2, R0_ZERO, 3),
            InstructionWord::encodeR(Opcode::ADD,  R3, R1, R2),
            InstructionWord::encodeB(Opcode::HALT, R0_ZERO, 0),
        };
        ok &= imem.loadProgram(prog, 0);
        auto [w0, _0] = imem.fetch(0); auto iw0 = InstructionWord::decode(w0);
        auto [w2, _2] = imem.fetch(2); auto iw2 = InstructionWord::decode(w2);
        ok &= (iw0.opcode == Opcode::MOV  && iw0.imm == 7);
        ok &= (iw2.opcode == Opcode::ADD  && iw2.rd  == R3);
        // Program that's too big must be rejected cleanly:
        ok &= !imem.loadProgram(prog, 2);  // 4 words at offset 2 = exceeds capacity 4
    }

    // --- Trap encoding round-trip ---
    {
        TernaryValue none = encodeNoTrap();
        ok &= !trapValid(none);
        ok &= (readStoredTrit(none, 0) == FAULT_VALID_NONE);
        TernaryValue t = encodeTrap(TrapCode::TRAP_DIV_ZERO);
        ok &= trapValid(t);
        ok &= (readStoredTrit(t, 0) == FAULT_VALID_SET);
        ok &= (readStoredTrit(t, 1) == T_NEG);
        ok &= (decodeTrap(t) == TrapCode::TRAP_DIV_ZERO);
        t = encodeTrap(TrapCode::TRAP_ILLEGAL_OP);
        ok &= trapValid(t);
        ok &= (readStoredTrit(t, 1) == T_POS);
        ok &= (decodeTrap(t) == TrapCode::TRAP_ILLEGAL_OP);
    }

    // --- makeTritResult / readTrit0 ---
    {
        ok &= (readTrit0(makeTritResult( 1)) ==  1);
        ok &= (readTrit0(makeTritResult( 0)) ==  0);
        ok &= (readTrit0(makeTritResult(-1)) == -1);
    }

    // --- VMState cold reset: SP initialized to top of dmem ---
    {
        VMState vm(16, 64);      // 16 IMEM words, 64 DMEM words
        vm.coldReset();
        ok &= vm.isRunning();
        ok &= (vm.pc == 0);
        ok &= vm.trap_reg.isZero();
        ok &= !trapValid(vm.trap_reg);
        // SP should be 63 (top of 64-word DMEM)
        long long sp = ops::toLong(vm.regfile.readSP());
        ok &= (sp == 63LL);
        // r0 still zero after reset
        ok &= vm.regfile.read(R0_ZERO).isZero();
    }

    // --- VMState trap/halt state transitions ---
    {
        VMState vm;
        ok &= vm.isRunning();
        vm.trap(TrapCode::TRAP_DIV_ZERO);
        ok &= vm.isTrapped();
        ok &= !vm.isRunning();
        ok &= trapValid(vm.trap_reg);
        ok &= (decodeTrap(vm.trap_reg) == TrapCode::TRAP_DIV_ZERO);
    }
    {
        VMState vm;
        vm.halt();
        ok &= vm.isHalted();
        ok &= !vm.isRunning();
    }

    // --- Program load + reset helper ---
    {
        VMState vm(8, 32);
        std::vector<TritWord27> prog = {
            InstructionWord::encodeB(Opcode::HALT, R0_ZERO, 0),
        };
        ok &= loadAndReset(vm, prog);
        ok &= vm.isRunning();
        ok &= (vm.pc == 0);
    }

    return ok;
}

} // namespace vm
} // namespace sandbox

#endif // TERNARY_VM_STATE_H
