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
#include <stdexcept>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

namespace sandbox {
namespace vm {

using namespace isa;  // bring in Opcode, TrapCode, REG_COUNT, R0_ZERO, etc.

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
static constexpr int DEFAULT_DMEM_SIZE = 65536;

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
    // L-mode factories: convert lane to positional base-3 before storing.
    // The bits field ALWAYS contains positional data, regardless of mode.
    [[nodiscard]] static TernaryValue fromL1(TritLane1 v) {
        return {TernaryMode::L1, fromLane(v).data};
    }
    [[nodiscard]] static TernaryValue fromL5(TritLane5 v) {
        return {TernaryMode::L5, fromLane(v).data};
    }
    [[nodiscard]] static TernaryValue fromL10(TritLane10 v) {
        auto pos = laneToPositional<10, uint32_t>(v);
        return {TernaryMode::L10, UInt128{pos.data}};
    }
    [[nodiscard]] static TernaryValue fromL20(TritLane20 v) {
        auto pos = laneToPositional<20, uint64_t>(v);
        return {TernaryMode::L20, UInt128{pos.data}};
    }
    [[nodiscard]] static TernaryValue fromL40(TritLane40 v) {
        auto pos = laneToPositional<40, UInt128>(v);
        return {TernaryMode::L40, UInt128{pos.data}};
    }
    [[nodiscard]] static TernaryValue fromL50(TritLane50 v) {
        auto pos = laneToPositional<50, UInt128>(v);
        return {TernaryMode::L50, pos.data};
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
        return toLane(T1{static_cast<uint8_t>(bits.toUint64())});
    }
    [[nodiscard]] TritLane5 asL5() const {
        return toLane(T5{static_cast<uint8_t>(bits.toUint64())});
    }
    [[nodiscard]] TritLane10 asL10() const {
        return laneFromPositional<10, uint32_t>(T10{static_cast<uint16_t>(bits.toUint64())});
    }
    [[nodiscard]] TritLane20 asL20() const {
        return laneFromPositional<20, uint64_t>(T20{static_cast<uint32_t>(bits.toUint64())});
    }
    [[nodiscard]] TritLane40 asL40() const {
        return laneFromPositional<40, UInt128>(Triple{bits.toUint64()});
    }
    [[nodiscard]] TritLane50 asL50() const {
        return laneFromPositional<50, UInt128>(LongTriple{bits});
    }

    [[nodiscard]] bool isInvalid() const {
        switch (mode) {
            case TernaryMode::T1:  return native_ops::isInvalid(asT1());
            case TernaryMode::T5:  return native_ops::isInvalid(asT5());
            case TernaryMode::T10: return native_ops::isInvalid(asT10());
            case TernaryMode::T20: return native_ops::isInvalid(asT20());
            case TernaryMode::T40: return native_ops::isInvalid(asTriple());
            case TernaryMode::T50: return native_ops::isInvalid(asLongTripleRaw());
            // L-modes now store positional data, so isInvalid checks
            // the same way as numeric modes (via isSpecial/OVERFLOW_DATA).
            case TernaryMode::L1:  return native_ops::isInvalid(asT1());
            case TernaryMode::L5:  return native_ops::isInvalid(asT5());
            case TernaryMode::L10: return asT10().isSpecial();
            case TernaryMode::L20: return asT20().isSpecial();
            case TernaryMode::L40: return asTriple().isSpecial();
            case TernaryMode::L50: return asLongTripleRaw().isSpecial();
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
            // L-modes now store positional data — zero is just data == 0.
            case TernaryMode::L1:  return asT1().isZero();
            case TernaryMode::L5:  return asT5().isZero();
            case TernaryMode::L10: return asT10().isZero();
            case TernaryMode::L20: return asT20().isZero();
            case TernaryMode::L40: return asTriple().isZero();
            case TernaryMode::L50: return asLongTripleRaw().isZero();
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

[[nodiscard]] inline TernaryValue convertValue(TernaryValue value, TernaryMode target) {
    if (value.mode == target) return value;
    if (value.isInvalid()) return TernaryValue::invalid(target);

    // With unified positional storage, same-width L<->T is a tag change.
    // Cross-width still needs the full arithmetic path.
    if (isLaneMode(value.mode) || isLaneMode(target)) {
        if (modeTritWidth(value.mode) == modeTritWidth(target)) {
            // Same width: data is identical (both positional), just change the tag.
            return {target, value.bits};
        }
        // Cross-width: reinterpret as numeric, then convert through LongTriple.
        TernaryValue asNumeric = {matchingNumericMode(value.mode), value.bits};
        TernaryValue converted = convertValue(asNumeric, matchingNumericMode(target));
        if (converted.isInvalid()) return TernaryValue::invalid(target);
        return {target, converted.bits};
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
        // L-modes store positional data — delegate to numeric unpack.
        case TernaryMode::L1:
            return pos == 0 ? static_cast<int8_t>(native_ops::toLongLong(val.asT1())) : 0;
        case TernaryMode::L5: {
            if (pos >= T5::TRITS) return 0;
            uint8_t temp = val.asT5().data;
            for (int i = 0; i < pos; ++i) temp = static_cast<uint8_t>(temp / 3);
            return static_cast<int8_t>(temp % 3) - 1;
        }
        case TernaryMode::L10: {
            if (pos >= 10) return 0;
            return val.asT10().unpack()[pos];
        }
        case TernaryMode::L20: {
            if (pos >= 20) return 0;
            return val.asT20().unpack()[pos];
        }
        case TernaryMode::L40: {
            if (pos >= 40) return 0;
            return val.asTriple().unpack()[pos];
        }
        case TernaryMode::L50: {
            if (pos >= 50) return 0;
            return val.asLongTripleRaw().unpack()[pos];
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
    std::array<TernaryValue, REG_COUNT> reg;

    TernaryRegisterFile() { reset(); }

    void reset() {
        for (auto& r : reg) r = TernaryValue::zero();
    }

    // Read register [idx]. r0 always returns exact zero regardless of
    // any prior attempted write. Out-of-range index returns zero silently.
    [[nodiscard]] TernaryValue read(uint8_t idx) const {
        if (idx == R0_ZERO)    return TernaryValue::zero();
        if (idx < REG_COUNT)   return reg[idx];
        return TernaryValue::zero();  // out-of-range: silent zero
    }

    // Write register [idx]. Writes to r0 are discarded. Writes to
    // r27 or above are discarded (r27 is the separate trap register).
    void write(uint8_t idx, TernaryValue val) {
        if (idx == R0_ZERO) return;   // hardwired zero
        if (idx < REG_COUNT) reg[idx] = val;
        // idx >= REG_COUNT: silently discard
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

struct TernaryMemory {
    TernaryValue* words = nullptr;
    int capacity = 0;
    VMStateAllocator* allocator = &defaultVMStateAllocator();

    explicit TernaryMemory(int size = DEFAULT_DMEM_SIZE,
                           VMStateAllocator& alloc = defaultVMStateAllocator())
        : allocator(&alloc) {
        allocate(size);
        reset();
    }

    ~TernaryMemory() {
        release();
    }

    TernaryMemory(const TernaryMemory& other)
        : allocator(other.allocator) {
        allocate(other.capacity);
        try {
            if (capacity > 0) std::copy(other.words, other.words + capacity, words);
        } catch (...) {
            release();
            throw;
        }
    }

    TernaryMemory& operator=(const TernaryMemory& other) {
        if (this == &other) return *this;
        TernaryMemory tmp(other);  // copy-and-swap for strong exception safety
        std::swap(words, tmp.words);
        std::swap(capacity, tmp.capacity);
        std::swap(allocator, tmp.allocator);
        return *this;
    }

    TernaryMemory(TernaryMemory&& other) noexcept
        : words(other.words), capacity(other.capacity), allocator(other.allocator) {
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
    }

    TernaryMemory& operator=(TernaryMemory&& other) noexcept {
        if (this == &other) return *this;
        release();
        words = other.words;
        capacity = other.capacity;
        allocator = other.allocator;
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        return *this;
    }

    void allocate(int size) {
        if (size < 0) throw std::invalid_argument("negative TernaryMemory size");
        capacity = size;
        words = allocator->allocateDataWords(capacity);
    }

    void release() {
        if (words != nullptr) {
            allocator->deallocateDataWords(words);
            words = nullptr;
        }
        capacity = 0;
    }

    void reset() {
        if (capacity > 0) std::fill(words, words + capacity, TernaryValue::zero());
    }

    void resize(int new_size) {
        release();
        allocate(new_size);
        reset();
    }

    // Load one LongTriple word from address [addr].
    // Returns {value, MemFaultCode::OK} on success.
    // Returns {LongTriple{0}, MemFaultCode::OUT_OF_RANGE} on fault.
    [[nodiscard]] std::pair<TernaryValue, MemFaultCode> load(int addr) const {
        if (addr < 0 || addr >= capacity) {
            return {TernaryValue::zero(), MemFaultCode::OUT_OF_RANGE};
        }
        return {words[addr], MemFaultCode::OK};
    }

    // Store one LongTriple word to address [addr].
    // Returns MemFaultCode::OK on success, OUT_OF_RANGE on fault.
    MemFaultCode store(int addr, TernaryValue val) {
        if (addr < 0 || addr >= capacity) {
            return MemFaultCode::OUT_OF_RANGE;
        }
        words[addr] = val;
        return MemFaultCode::OK;
    }

    MemFaultCode store(int addr, LongTriple val) {
        return store(addr, TernaryValue::fromLongTriple(val));
    }

    [[nodiscard]] bool inRange(int addr) const {
        return addr >= 0 && addr < capacity;
    }

    [[nodiscard]] int size() const { return capacity; }
};

// =============================================================================
// SECTION 6 — Instruction Memory
// =============================================================================
// Word-addressed flat array of TritWord27 instruction words.
// Separate from data memory (Harvard architecture).
// The PC is an index into this array.

struct TernaryInstructionMemory {
    TritWord27* words = nullptr;
    int capacity = 0;
    VMStateAllocator* allocator = &defaultVMStateAllocator();

    explicit TernaryInstructionMemory(int size = DEFAULT_IMEM_SIZE,
                                      VMStateAllocator& alloc = defaultVMStateAllocator())
        : allocator(&alloc) {
        allocate(size);
        reset();
    }

    ~TernaryInstructionMemory() {
        release();
    }

    TernaryInstructionMemory(const TernaryInstructionMemory& other)
        : allocator(other.allocator) {
        allocate(other.capacity);
        try {
            if (capacity > 0) std::copy(other.words, other.words + capacity, words);
        } catch (...) {
            release();
            throw;
        }
    }

    TernaryInstructionMemory& operator=(const TernaryInstructionMemory& other) {
        if (this == &other) return *this;
        TernaryInstructionMemory tmp(other);  // copy-and-swap for strong exception safety
        std::swap(words, tmp.words);
        std::swap(capacity, tmp.capacity);
        std::swap(allocator, tmp.allocator);
        return *this;
    }

    TernaryInstructionMemory(TernaryInstructionMemory&& other) noexcept
        : words(other.words), capacity(other.capacity), allocator(other.allocator) {
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
    }

    TernaryInstructionMemory& operator=(TernaryInstructionMemory&& other) noexcept {
        if (this == &other) return *this;
        release();
        words = other.words;
        capacity = other.capacity;
        allocator = other.allocator;
        other.words = nullptr;
        other.capacity = 0;
        other.allocator = &defaultVMStateAllocator();
        return *this;
    }

    void allocate(int size) {
        if (size < 0) throw std::invalid_argument("negative TernaryInstructionMemory size");
        capacity = size;
        words = allocator->allocateInstructionWords(capacity);
    }

    void release() {
        if (words != nullptr) {
            allocator->deallocateInstructionWords(words);
            words = nullptr;
        }
        capacity = 0;
    }

    void reset() {
        if (capacity > 0) std::fill(words, words + capacity, TritWord27{});
    }

    void resize(int new_size) {
        release();
        allocate(new_size);
        reset();
    }

    // Fetch the instruction word at PC address [addr].
    // Returns {word, OK} on success, {zero_word, OUT_OF_RANGE} on fault.
    [[nodiscard]] std::pair<TritWord27, MemFaultCode> fetch(int addr) const {
        if (addr < 0 || addr >= capacity) {
            return {TritWord27{}, MemFaultCode::OUT_OF_RANGE};
        }
        return {words[addr], MemFaultCode::OK};
    }

    // Write a single instruction word (used by the assembler / test harness).
    MemFaultCode write(int addr, TritWord27 iw) {
        if (addr < 0 || addr >= capacity) {
            return MemFaultCode::OUT_OF_RANGE;
        }
        words[addr] = iw;
        return MemFaultCode::OK;
    }

    // Load a full program from a vector of TritWord27 instruction words
    // starting at [start_addr]. Returns false and does not write if the
    // program would exceed capacity.
    bool loadProgram(const std::vector<TritWord27>& program, int start_addr = 0) {
        if (start_addr < 0) return false;
        if (start_addr + static_cast<int>(program.size()) > capacity) return false;
        for (int i = 0; i < static_cast<int>(program.size()); ++i) {
            words[start_addr + i] = program[i];
        }
        return true;
    }

    [[nodiscard]] bool inRange(int addr) const {
        return addr >= 0 && addr < capacity;
    }

    [[nodiscard]] int size() const { return capacity; }
};

// =============================================================================
// SECTION 7 — VM Execution Status
// =============================================================================

enum class VMStatus : uint8_t {
    RUNNING  = 0,  // Normal execution. PC will advance on next step.
    HALTED   = 1,  // Clean HALT instruction executed. PC points to HALT word.
    TRAPPED  = 2,  // Fault occurred. trap_reg written. Execution stopped.
};

[[nodiscard]] inline std::string vmStatusToString(VMStatus s) {
    switch (s) {
        case VMStatus::RUNNING: return "RUNNING";
        case VMStatus::HALTED:  return "HALTED";
        case VMStatus::TRAPPED: return "TRAPPED";
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

static constexpr int MMU_PAGE_WORDS = 27;
static constexpr int PTE_FLAG_VALID = 0;
static constexpr int PTE_FLAG_USER = 1;
static constexpr int PTE_FLAG_READ = 2;
static constexpr int PTE_FLAG_WRITE = 3;
static constexpr int PTE_FLAG_EXECUTE = 4;
static constexpr int PTE_PPN_SHIFT = 5;
static constexpr long long PTE_PPN_SCALE = 243; // 3^5

struct PageTableEntry {
    int ppn = 0;
    bool present = false;
    bool user = false;
    bool read = false;
    bool write = false;
    bool execute = false;
};

[[nodiscard]] inline TernaryValue encodePageTableEntry(
    int ppn,
    bool user,
    bool read,
    bool write,
    bool execute,
    bool present = true) {

    long long encoded = static_cast<long long>(ppn) * PTE_PPN_SCALE;
    encoded += (present ? T_POS : T_NEG);
    encoded += 3LL * (user ? T_POS : T_NEG);
    encoded += 9LL * (read ? T_POS : T_NEG);
    encoded += 27LL * (write ? T_POS : T_NEG);
    encoded += 81LL * (execute ? T_POS : T_NEG);
    return ops::fromLong(encoded, TernaryMode::T40);
}

[[nodiscard]] inline bool decodePageTableEntry(TernaryValue value, PageTableEntry& out) {
    if (!isNumericMode(value.mode) || value.isInvalid()) return false;
    const long long raw = ops::toLong(value);
    const int8_t valid = readStoredTrit(value, PTE_FLAG_VALID);
    const int8_t user = readStoredTrit(value, PTE_FLAG_USER);
    const int8_t read = readStoredTrit(value, PTE_FLAG_READ);
    const int8_t write = readStoredTrit(value, PTE_FLAG_WRITE);
    const int8_t execute = readStoredTrit(value, PTE_FLAG_EXECUTE);
    const long long low =
        static_cast<long long>(valid) +
        3LL * static_cast<long long>(user) +
        9LL * static_cast<long long>(read) +
        27LL * static_cast<long long>(write) +
        81LL * static_cast<long long>(execute);
    const long long ppn = (raw - low) / PTE_PPN_SCALE;
    out.ppn = static_cast<int>(ppn);
    out.present = valid == T_POS;
    out.user = user == T_POS;
    out.read = read == T_POS;
    out.write = write == T_POS;
    out.execute = execute == T_POS;
    return true;
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

    void reset(int length) {
        fault_valid.assign(std::max(0, length), 0);
        fault_class.assign(std::max(0, length), TrapCode::TRAP_MEM_FAULT);
    }

    void clear() {
        std::fill(fault_valid.begin(), fault_valid.end(), 0);
        std::fill(fault_class.begin(), fault_class.end(), TrapCode::TRAP_MEM_FAULT);
    }

    void setLane(int lane, TrapCode code) {
        if (lane < 0 || lane >= static_cast<int>(fault_valid.size())) return;
        fault_valid[static_cast<std::size_t>(lane)] = 1;
        fault_class[static_cast<std::size_t>(lane)] = code;
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

struct VMState {
    TernaryRegisterFile      regfile;   // r0..r26 general-purpose registers
    TernaryInstructionMemory imem;      // Instruction memory (Harvard IMEM)
    TernaryMemory            dmem;      // Data memory (Harvard DMEM)
    int                      pc = 0;   // Program counter (word-addressed into imem)
    VMStatus                 status = VMStatus::RUNNING;
    TernaryValue             trap_reg;  // r27: written on fault, read-only from ISA
    int                      vector_length = DEFAULT_VECTOR_LENGTH;
    TernaryVectorFile        vregfile;
    VectorFaultState         vector_faults;
    TernaryValue             accumulator;
    std::string              syscall_buffer;
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
    long long                timer_reload = 0;
    long long                timer_counter = 0;
    bool                     timer_enable = false;
    bool                     timer_pending = false;
    int                      user_imem_base = 0;
    int                      user_imem_limit = 0;
    int                      user_dmem_base = 0;
    int                      user_dmem_limit = 0;
    int                      syscall_id = 0;
    bool                     mmu_enable = false;
    int                      user_imem_ptbr = 0;
    int                      user_imem_pages = 0;
    int                      user_dmem_ptbr = 0;
    int                      user_dmem_pages = 0;
    int                      page_fault_addr = 0;
    int                      page_fault_access = OS_PAGE_ACCESS_LOAD;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------
    VMState()
        : regfile(), imem(DEFAULT_IMEM_SIZE), dmem(DEFAULT_DMEM_SIZE),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        resetControlState();
    }

    explicit VMState(VMStateAllocator& allocator)
        : regfile(), imem(DEFAULT_IMEM_SIZE, allocator), dmem(DEFAULT_DMEM_SIZE, allocator),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        resetControlState();
    }

    VMState(int imem_size, int dmem_size,
            VMStateAllocator& allocator = defaultVMStateAllocator())
        : regfile(), imem(imem_size, allocator), dmem(dmem_size, allocator),
          pc(0), status(VMStatus::RUNNING), trap_reg(encodeNoTrap()),
          accumulator(TernaryValue::zero()) {
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
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
        vregfile.reset(vector_length);
        vector_faults.reset(vector_length);
        resetControlState();

        // Initialize SP to top of data memory.
        // native_ops::fromInt puts a small integer into LongTriple format.
        regfile.write(R26_SP, ops::fromLong(dmem.size() - 1));
    }

    // Clear both memories (set all words to zero / NOP).
    void clearMemory() {
        imem.reset();
        dmem.reset();
    }

    // Full cold reset: register file, PC, status, and both memories.
    void coldReset() {
        reset();
        clearMemory();
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
        timer_reload = 0;
        timer_counter = 0;
        timer_enable = false;
        timer_pending = false;
        user_imem_base = 0;
        user_imem_limit = imem.size();
        user_dmem_base = 0;
        user_dmem_limit = dmem.size();
        syscall_id = 0;
        mmu_enable = false;
        user_imem_ptbr = 0;
        user_imem_pages = 0;
        user_dmem_ptbr = 0;
        user_dmem_pages = 0;
        page_fault_addr = 0;
        page_fault_access = OS_PAGE_ACCESS_LOAD;
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
            default: return false;
        }
        out = ops::fromLong(value);
        return true;
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
                return true;
            case CSR_USER_IMEM_LIMIT:
                user_imem_limit = static_cast<int>(value);
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
                return true;
            case CSR_USER_IMEM_PTBR:
                if (value < 0) return false;
                user_imem_ptbr = static_cast<int>(value);
                return true;
            case CSR_USER_IMEM_PAGES:
                if (value < 0) return false;
                user_imem_pages = static_cast<int>(value);
                return true;
            case CSR_USER_DMEM_PTBR:
                if (value < 0) return false;
                user_dmem_ptbr = static_cast<int>(value);
                return true;
            case CSR_USER_DMEM_PAGES:
                if (value < 0) return false;
                user_dmem_pages = static_cast<int>(value);
                return true;
            case CSR_PAGE_FAULT_ADDR:
                page_fault_addr = static_cast<int>(value);
                return true;
            case CSR_PAGE_FAULT_ACCESS:
                page_fault_access = static_cast<int>(value);
                return true;
            default:
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
        const int vpn = virtual_addr / MMU_PAGE_WORDS;
        const int offset = virtual_addr % MMU_PAGE_WORDS;
        if (vpn < 0 || vpn >= page_count) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        const int pte_addr = ptbr + vpn;
        if (!dmem.inRange(pte_addr)) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        auto [pte_value, pte_fault] = dmem.load(pte_addr);
        if (pte_fault != MemFaultCode::OK) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        PageTableEntry pte;
        if (!decodePageTableEntry(pte_value, pte) || !pte.present) {
            setPageFault(virtual_addr, access);
            routed_cause = page_fault_cause;
            return false;
        }
        if (!pte.user ||
            (need_read && !pte.read) ||
            (need_write && !pte.write) ||
            (need_execute && !pte.execute)) {
            setPageFault(virtual_addr, access);
            routed_cause = protection_cause;
            return false;
        }
        const long long phys = static_cast<long long>(pte.ppn) * MMU_PAGE_WORDS + offset;
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
                routed_cause = OS_CAUSE_FETCH_FAULT;
                return false;
            }
            physical_pc = virtual_pc;
            return true;
        }
        if (!mmu_enable) {
            if (!canFetch(virtual_pc)) {
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
                routed_cause = OS_CAUSE_LOAD_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        if (!mmu_enable) {
            if (!canLoad(virtual_addr)) {
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
                routed_cause = OS_CAUSE_STORE_FAULT;
                return false;
            }
            physical_addr = virtual_addr;
            return true;
        }
        if (!mmu_enable) {
            if (!canStore(virtual_addr)) {
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
