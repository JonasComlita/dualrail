// =============================================================================
// ternary_vm.h  —  Ternary VM Dispatcher (Fetch-Decode-Execute Loop)
// =============================================================================
//
// Phase 3 of the Ternary VM build plan. Implements the execution engine that
// operates on VMState (Phase 2) using the ISA definition (Phase 1).
//
// Dependencies:
//   ternary_vm_state.h  (which pulls in ternary_math.h and ternary_isa.h)
//
// Public API:
//   step(VMState&)              — Execute one instruction. Returns VMStatus.
//   step(VMState&, VMHooks)     — Execute one instruction with observer hooks.
//   run(VMState&, max_steps)    — Execute until HALT/TRAP or step limit.
//   RunResult                   — Structured result from run().
//
// Execution Model:
//   Each call to step() performs exactly one fetch-decode-execute-writeback
//   cycle. The PC is advanced AFTER successful execution, BEFORE the next
//   fetch. On HALT or TRAP the PC is NOT advanced — it continues to point
//   at the offending or terminal instruction for post-mortem inspection.
//
// Opcode Implementation Notes:
//   ADD, SUB, NEG, ABS       — width-selected native ternary arithmetic
//   MUL                      — ops::multiply (native trit multiply)
//   DIV                      — ops::divide with pre-check for zero → TRAP
//   SQRT                     — width-selected native ternary sqrt
//   TCMP                     — sign(Rs1 − Rs2) via native ternary comparison;
//                              result is a tagged T1 value
//   TMIN / TMAX              — compare natively, return the input value
//   TINV                     — alias for NEG (trit flip is its own inverse)
//   LOAD / STORE             — word-addressed DMEM access; fault → TRAP
//   JMP                      — unconditional PC-relative branch
//   BRN                      — conditional: branch if Rs.trit[0] == T_NEG
//   CALL                     — save PC+1 to r25 (LR), then JMP
//   RET                      — PC ← toLong(r25)
//   MOV / MOVH               — load immediate into Rd
//   COPY                     — Rd ← Rs1
//
// =============================================================================

#pragma once
#ifndef TERNARY_VM_H
#define TERNARY_VM_H

#include "ternary_vm_state.h"
#include "ternary_simd.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <sstream>
#include <tuple>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
#elif defined(__x86_64__) || defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sandbox {
namespace vm {

// =============================================================================
// SECTION 1 — LongTriple Arithmetic Helpers
// =============================================================================
// These bridge the gap between what ternary_math.h provides and what the
// dispatcher needs. They live here to keep ternary_vm_state.h data-only.

namespace exec {

[[nodiscard]] inline bool modeFromFunc(uint8_t func, TernaryMode& out) {
    switch (func) {
        case FUNC_T1:  out = TernaryMode::T1;  return true;
        case FUNC_T5:  out = TernaryMode::T5;  return true;
        case FUNC_T10: out = TernaryMode::T10; return true;
        case FUNC_T20: out = TernaryMode::T20; return true;
        case FUNC_T40: out = TernaryMode::T40; return true;
        case FUNC_T50: out = TernaryMode::T50; return true;
        case FUNC_L1:  out = TernaryMode::L1;  return true;
        case FUNC_L5:  out = TernaryMode::L5;  return true;
        case FUNC_L10: out = TernaryMode::L10; return true;
        case FUNC_L20: out = TernaryMode::L20; return true;
        case FUNC_L40: out = TernaryMode::L40; return true;
        case FUNC_L50: out = TernaryMode::L50; return true;
        default: return false;
    }
}

[[nodiscard]] inline bool numericModeFromFunc(uint8_t func, TernaryMode& out) {
    return isNumericWidthFunc(func) && modeFromFunc(func, out);
}

[[nodiscard]] inline bool laneModeFromFunc(uint8_t func, TernaryMode& out) {
    return isLaneWidthFunc(func) && modeFromFunc(func, out);
}

enum class NativeOp : uint8_t {
    Add, Sub, Mul, Div, Neg, Abs, Sqrt
};

template<typename F>
[[nodiscard]] inline TernaryValue unaryByMode(TernaryValue value, TernaryMode mode, F&& f) {
    if (!isNumericMode(mode) || !isNumericMode(value.mode)) return TernaryValue::invalid(mode);
    TernaryValue a = convertValue(value, mode);
    if (a.isInvalid()) return a;
    switch (mode) {
        case TernaryMode::T1:  return TernaryValue::fromT1(f(a.asT1()));
        case TernaryMode::T5:  return TernaryValue::fromT5(f(a.asT5()));
        case TernaryMode::T10: return TernaryValue::fromT10(f(a.asT10()));
        case TernaryMode::T20: return TernaryValue::fromT20(f(a.asT20()));
        case TernaryMode::T40: return TernaryValue::fromTriple(f(a.asTriple()));
        case TernaryMode::T50: return TernaryValue::fromLongTriple(f(a.asLongTripleRaw()));
        default: break;
    }
    return TernaryValue{mode, UInt128::max()};
}

[[nodiscard]] inline TernaryValue negateValue(TernaryValue value, TernaryMode mode) {
    return unaryByMode(value, mode, [](auto x) { return native_ops::negate(x); });
}

[[nodiscard]] inline TernaryValue absValue(TernaryValue value, TernaryMode mode) {
    return unaryByMode(value, mode, [](auto x) { return native_ops::abs(x); });
}

[[nodiscard]] inline TernaryValue sqrtValue(TernaryValue value, TernaryMode mode) {
    return unaryByMode(value, mode, [](auto x) { return native_ops::sqrt(x); });
}

template<typename F>
[[nodiscard]] inline TernaryValue binaryByMode(
    TernaryValue lhs, TernaryValue rhs, TernaryMode mode, F&& f) {
    if (!isNumericMode(mode) || !isNumericMode(lhs.mode) || !isNumericMode(rhs.mode)) {
        return TernaryValue::invalid(mode);
    }
    TernaryValue a = convertValue(lhs, mode);
    TernaryValue b = convertValue(rhs, mode);
    if (a.isInvalid()) return a;
    if (b.isInvalid()) return b;
    switch (mode) {
        case TernaryMode::T1:  return TernaryValue::fromT1(f(a.asT1(), b.asT1()));
        case TernaryMode::T5:  return TernaryValue::fromT5(f(a.asT5(), b.asT5()));
        case TernaryMode::T10: return TernaryValue::fromT10(f(a.asT10(), b.asT10()));
        case TernaryMode::T20: return TernaryValue::fromT20(f(a.asT20(), b.asT20()));
        case TernaryMode::T40: return TernaryValue::fromTriple(f(a.asTriple(), b.asTriple()));
        case TernaryMode::T50: return TernaryValue::fromLongTriple(f(a.asLongTripleRaw(), b.asLongTripleRaw()));
        default: break;
    }
    return TernaryValue{mode, UInt128::max()};
}

[[nodiscard]] inline TernaryValue addValue(TernaryValue a, TernaryValue b, TernaryMode mode) {
    return binaryByMode(a, b, mode, [](auto x, auto y) { return native_ops::add(x, y); });
}

[[nodiscard]] inline TernaryValue subtractValue(TernaryValue a, TernaryValue b, TernaryMode mode) {
    return binaryByMode(a, b, mode, [](auto x, auto y) { return native_ops::subtract(x, y); });
}

[[nodiscard]] inline TernaryValue multiplyValue(TernaryValue a, TernaryValue b, TernaryMode mode) {
    return binaryByMode(a, b, mode, [](auto x, auto y) { return native_ops::multiply(x, y); });
}

[[nodiscard]] inline TernaryValue divideValue(TernaryValue a, TernaryValue b, TernaryMode mode) {
    return binaryByMode(a, b, mode, [](auto x, auto y) { return native_ops::divide(x, y); });
}

[[nodiscard]] inline int8_t signValue(TernaryValue value, TernaryMode mode) {
    if (!isNumericMode(mode) || !isNumericMode(value.mode)) return 0;
    TernaryValue a = convertValue(value, mode);
    if (a.isInvalid()) return 0;
    switch (mode) {
        case TernaryMode::T1:  return native_ops::sign(a.asT1());
        case TernaryMode::T5:  return native_ops::sign(a.asT5());
        case TernaryMode::T10: return native_ops::sign(a.asT10());
        case TernaryMode::T20: return native_ops::sign(a.asT20());
        case TernaryMode::T40: return native_ops::sign(a.asTriple());
        case TernaryMode::T50: return native_ops::sign(a.asLongTripleRaw());
        default: break;
    }
    return 0;
}

[[nodiscard]] inline int8_t compareValue(TernaryValue lhs, TernaryValue rhs, TernaryMode mode) {
    if (!isNumericMode(mode) || !isNumericMode(lhs.mode) || !isNumericMode(rhs.mode)) return 0;
    TernaryValue a = convertValue(lhs, mode);
    TernaryValue b = convertValue(rhs, mode);
    if (a.isInvalid() || b.isInvalid()) return 0;
    switch (mode) {
        case TernaryMode::T1:  return native_ops::compare(a.asT1(), b.asT1());
        case TernaryMode::T5:  return native_ops::compare(a.asT5(), b.asT5());
        case TernaryMode::T10: return native_ops::compare(a.asT10(), b.asT10());
        case TernaryMode::T20: return native_ops::compare(a.asT20(), b.asT20());
        case TernaryMode::T40: return native_ops::compare(a.asTriple(), b.asTriple());
        case TernaryMode::T50: return native_ops::compare(a.asLongTripleRaw(), b.asLongTripleRaw());
        default: break;
    }
    return 0;
}

enum class LaneOp : uint8_t {
    Add, Sub, Neg, And, Or
};

[[nodiscard]] inline bool laneOperandOk(TernaryValue value, TernaryMode mode) {
    return value.mode == mode && !value.isInvalid();
}

[[nodiscard]] inline TernaryValue laneUnaryValue(TernaryValue value, TernaryMode mode, LaneOp op) {
    if (!isLaneMode(mode) || !laneOperandOk(value, mode)) return TernaryValue::invalid(mode);
    switch (mode) {
        case TernaryMode::L1:
            return TernaryValue::fromL1(op == LaneOp::Neg ? tritwiseNeg(value.asL1()) : TritLane1::invalid());
        case TernaryMode::L5:
            return TernaryValue::fromL5(op == LaneOp::Neg ? tritwiseNeg(value.asL5()) : TritLane5::invalid());
        case TernaryMode::L10:
            return TernaryValue::fromL10(op == LaneOp::Neg ? tritwiseNeg(value.asL10()) : TritLane10::invalid());
        case TernaryMode::L20:
            return TernaryValue::fromL20(op == LaneOp::Neg ? tritwiseNeg(value.asL20()) : TritLane20::invalid());
        case TernaryMode::L40:
            return TernaryValue::fromL40(op == LaneOp::Neg ? tritwiseNeg(value.asL40()) : TritLane40::invalid());
        case TernaryMode::L50:
            return TernaryValue::fromL50(op == LaneOp::Neg ? tritwiseNeg(value.asL50()) : TritLane50::invalid());
        default:
            return TernaryValue::invalid(mode);
    }
}

[[nodiscard]] inline TernaryValue laneBinaryValue(
    TernaryValue lhs, TernaryValue rhs, TernaryMode mode, LaneOp op) {

    if (!isLaneMode(mode) || !laneOperandOk(lhs, mode) || !laneOperandOk(rhs, mode)) {
        return TernaryValue::invalid(mode);
    }

    switch (mode) {
        case TernaryMode::L1:
            if (op == LaneOp::Add) return TernaryValue::fromL1(tritwiseAddCarryless(lhs.asL1(), rhs.asL1()));
            if (op == LaneOp::Sub) return TernaryValue::fromL1(tritwiseSubtractCarryless(lhs.asL1(), rhs.asL1()));
            if (op == LaneOp::And) return TernaryValue::fromL1(tritwiseAnd(lhs.asL1(), rhs.asL1()));
            if (op == LaneOp::Or)  return TernaryValue::fromL1(tritwiseOr(lhs.asL1(), rhs.asL1()));
            break;
        case TernaryMode::L5:
            if (op == LaneOp::Add) return TernaryValue::fromL5(tritwiseAddCarryless(lhs.asL5(), rhs.asL5()));
            if (op == LaneOp::Sub) return TernaryValue::fromL5(tritwiseSubtractCarryless(lhs.asL5(), rhs.asL5()));
            if (op == LaneOp::And) return TernaryValue::fromL5(tritwiseAnd(lhs.asL5(), rhs.asL5()));
            if (op == LaneOp::Or)  return TernaryValue::fromL5(tritwiseOr(lhs.asL5(), rhs.asL5()));
            break;
        case TernaryMode::L10:
            if (op == LaneOp::Add) return TernaryValue::fromL10(tritwiseAddCarryless(lhs.asL10(), rhs.asL10()));
            if (op == LaneOp::Sub) return TernaryValue::fromL10(tritwiseSubtractCarryless(lhs.asL10(), rhs.asL10()));
            if (op == LaneOp::And) return TernaryValue::fromL10(tritwiseAnd(lhs.asL10(), rhs.asL10()));
            if (op == LaneOp::Or)  return TernaryValue::fromL10(tritwiseOr(lhs.asL10(), rhs.asL10()));
            break;
        case TernaryMode::L20:
            if (op == LaneOp::Add) return TernaryValue::fromL20(tritwiseAddCarryless(lhs.asL20(), rhs.asL20()));
            if (op == LaneOp::Sub) return TernaryValue::fromL20(tritwiseSubtractCarryless(lhs.asL20(), rhs.asL20()));
            if (op == LaneOp::And) return TernaryValue::fromL20(tritwiseAnd(lhs.asL20(), rhs.asL20()));
            if (op == LaneOp::Or)  return TernaryValue::fromL20(tritwiseOr(lhs.asL20(), rhs.asL20()));
            break;
        case TernaryMode::L40:
            if (op == LaneOp::Add) return TernaryValue::fromL40(tritwiseAddCarryless(lhs.asL40(), rhs.asL40()));
            if (op == LaneOp::Sub) return TernaryValue::fromL40(tritwiseSubtractCarryless(lhs.asL40(), rhs.asL40()));
            if (op == LaneOp::And) return TernaryValue::fromL40(tritwiseAnd(lhs.asL40(), rhs.asL40()));
            if (op == LaneOp::Or)  return TernaryValue::fromL40(tritwiseOr(lhs.asL40(), rhs.asL40()));
            break;
        case TernaryMode::L50:
            if (op == LaneOp::Add) return TernaryValue::fromL50(tritwiseAddCarryless(lhs.asL50(), rhs.asL50()));
            if (op == LaneOp::Sub) return TernaryValue::fromL50(tritwiseSubtractCarryless(lhs.asL50(), rhs.asL50()));
            if (op == LaneOp::And) return TernaryValue::fromL50(tritwiseAnd(lhs.asL50(), rhs.asL50()));
            if (op == LaneOp::Or)  return TernaryValue::fromL50(tritwiseOr(lhs.asL50(), rhs.asL50()));
            break;
        default:
            break;
    }
    return TernaryValue::invalid(mode);
}

// Sign of a LongTriple: returns T_NEG, T_ZER, or T_POS.
[[nodiscard]] inline int8_t sign(LongTriple t) {
    return native_ops::sign(t);
}

// Compare two LongTriple values. Returns T_NEG / T_ZER / T_POS.
// Implements TCMP: sign(a - b).
[[nodiscard]] inline int8_t compare(LongTriple a, LongTriple b) {
    return native_ops::compare(a, b);
}

// Absolute value of a LongTriple.
[[nodiscard]] inline LongTriple abs_val(LongTriple t) {
    if (sign(t) == T_NEG) return sandbox::ops::negate(t);
    return t;
}

// Subtract: a - b through the native LongTriple arithmetic layer.
[[nodiscard]] inline LongTriple subtract(LongTriple a, LongTriple b) {
    return sandbox::ops::subtract(a, b);
}

// TMIN / TMAX: compare and return the winning input value.
[[nodiscard]] inline LongTriple tmin(LongTriple a, LongTriple b) {
    return (compare(a, b) == T_NEG) ? a : b;
}
[[nodiscard]] inline LongTriple tmax(LongTriple a, LongTriple b) {
    return (compare(a, b) == T_POS) ? a : b;
}

// Reconstruct the integer PC value from a LongTriple (used by RET).
[[nodiscard]] inline int pcFromLongTriple(LongTriple t) {
    long long v = sandbox::vm::ops::toLong(t);
    if (v < 0 || v > 0x7FFFFFFF) return -1;
    return static_cast<int>(v);
}

[[nodiscard]] inline int pcFromValue(TernaryValue t) {
    long long v = sandbox::vm::ops::toLong(t);
    if (v < 0 || v > 0x7FFFFFFF) return -1;
    return static_cast<int>(v);
}

[[nodiscard]] inline bool validVectorReg(uint8_t reg) {
    return reg < VECTOR_REGISTER_COUNT;
}

inline void prepareVectorOp(VMState& vm) {
    if (static_cast<int>(vm.vector_faults.fault_valid.size()) != vm.vector_length) {
        vm.vector_faults.reset(vm.vector_length);
    }
    for (auto& reg : vm.vregfile.reg) {
        if (static_cast<int>(reg.lane.size()) != vm.vector_length) {
            reg.reset(vm.vector_length);
        }
    }
    vm.vector_faults.clear();
}

[[nodiscard]] inline bool convertVectorNumericLane(
    TernaryValue source,
    TernaryMode mode,
    TernaryValue& out) {

    if (!isNumericMode(mode) || !isNumericMode(source.mode) || source.isInvalid()) return false;
    out = convertValue(source, mode);
    return out.mode == mode && !out.isInvalid();
}

[[nodiscard]] inline bool exactVectorNumericLane(TernaryValue source, TernaryMode mode) {
    return source.mode == mode && !source.isInvalid();
}

[[nodiscard]] inline TernaryValue makePredicateLane(int8_t cmp) {
    TritLane1 lane;
    lane.setTrit(0, cmp);
    return TernaryValue::fromL1(lane);
}

inline void writeVectorFaultZero(VMState& vm, uint8_t vreg, int lane, TrapCode code, TernaryMode mode) {
    vm.vector_faults.setLane(lane, code);
    vm.vregfile.reg[vreg].write(lane, TernaryValue::zero(mode));
}

template<typename Lane, typename TWidth>
[[nodiscard]] inline bool tryBatchVectorBinary(
    VMState& vm, uint8_t vd, uint8_t va, uint8_t vb, TernaryMode mode, Opcode opcode) {

    const int count = vm.vector_length;
    if (count > 512) return false;
    
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> la[512];
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> lb[512];
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> lout[512];

    for (int lane = 0; lane < count; ++lane) {
        TernaryValue a, b;
        if (!convertVectorNumericLane(vm.vregfile.reg[va].read(lane), mode, a) ||
            !convertVectorNumericLane(vm.vregfile.reg[vb].read(lane), mode, b)) {
            return false;
        }
        if constexpr (Lane::trits == 1) {
            la[lane] = sandbox::toLane(a.asT1());
            lb[lane] = sandbox::toLane(b.asT1());
        } else if constexpr (Lane::trits == 5) {
            la[lane] = sandbox::toLane(a.asT5());
            lb[lane] = sandbox::toLane(b.asT5());
        }
    }

    if (opcode == Opcode::VADD) {
        sandbox::simd::batchTritwiseAdd(la, lb, lout, count);
    } else {
        sandbox::simd::batchTritwiseSub(la, lb, lout, count);
    }

    for (int lane = 0; lane < count; ++lane) {
        if (!lout[lane].isValid()) {
            return false;
        }
        TWidth res = sandbox::fromLane(lout[lane]);
        if constexpr (Lane::trits == 1) {
            vm.vregfile.reg[vd].write(lane, TernaryValue::fromT1(res));
        } else if constexpr (Lane::trits == 5) {
            vm.vregfile.reg[vd].write(lane, TernaryValue::fromT5(res));
        }
    }
    return true;
}

template<typename Lane, typename TWidth>
[[nodiscard]] inline bool tryBatchVectorNeg(
    VMState& vm, uint8_t vd, uint8_t vs, TernaryMode mode) {

    const int count = vm.vector_length;
    if (count > 512) return false;
    
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> lin[512];
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> lout[512];

    for (int lane = 0; lane < count; ++lane) {
        TernaryValue src;
        if (!convertVectorNumericLane(vm.vregfile.reg[vs].read(lane), mode, src)) {
            return false;
        }
        if constexpr (Lane::trits == 1) {
            lin[lane] = sandbox::toLane(src.asT1());
        } else if constexpr (Lane::trits == 5) {
            lin[lane] = sandbox::toLane(src.asT5());
        }
    }

    sandbox::simd::batchTritwiseNeg(lin, lout, count);

    for (int lane = 0; lane < count; ++lane) {
        if (!lout[lane].isValid()) {
            return false;
        }
        TWidth res = sandbox::fromLane(lout[lane]);
        if constexpr (Lane::trits == 1) {
            vm.vregfile.reg[vd].write(lane, TernaryValue::fromT1(res));
        } else if constexpr (Lane::trits == 5) {
            vm.vregfile.reg[vd].write(lane, TernaryValue::fromT5(res));
        }
    }
    return true;
}

template<typename Lane>
[[nodiscard]] inline bool tryBatchVectorCompare(
    VMState& vm, uint8_t vd, uint8_t va, uint8_t vb, TernaryMode mode) {

    const int count = vm.vector_length;
    if (count > 512) return false;
    
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> la[512];
    alignas(32) TritLane<Lane::trits, typename Lane::storage_type> lb[512];
    alignas(32) TritLane1 lout[512];

    for (int lane = 0; lane < count; ++lane) {
        TernaryValue a, b;
        if (!convertVectorNumericLane(vm.vregfile.reg[va].read(lane), mode, a) ||
            !convertVectorNumericLane(vm.vregfile.reg[vb].read(lane), mode, b)) {
            return false;
        }
        if constexpr (Lane::trits == 1) {
            la[lane] = sandbox::toLane(a.asT1());
            lb[lane] = sandbox::toLane(b.asT1());
        } else if constexpr (Lane::trits == 5) {
            la[lane] = sandbox::toLane(a.asT5());
            lb[lane] = sandbox::toLane(b.asT5());
        }
    }

    sandbox::simd::batchTritwiseCompare(la, lb, lout, count);

    for (int lane = 0; lane < count; ++lane) {
        if (!lout[lane].isValid()) {
            return false;
        }
        vm.vregfile.reg[vd].write(lane, TernaryValue::fromL1(lout[lane]));
    }
    return true;
}

inline void writeVectorBinaryNumeric(
    VMState& vm,
    uint8_t vd,
    uint8_t va,
    uint8_t vb,
    TernaryMode mode,
    Opcode opcode) {

    if (opcode == Opcode::VADD || opcode == Opcode::VSUB) {
        bool ok = false;
        switch (mode) {
            case TernaryMode::T1:  ok = tryBatchVectorBinary<TritLane1, T1>(vm, vd, va, vb, mode, opcode); break;
            case TernaryMode::T5:  ok = tryBatchVectorBinary<TritLane5, T5>(vm, vd, va, vb, mode, opcode); break;
            default: break;
        }
        if (ok) return;
    }

    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue a;
        TernaryValue b;
        if (!convertVectorNumericLane(vm.vregfile.reg[va].read(lane), mode, a) ||
            !convertVectorNumericLane(vm.vregfile.reg[vb].read(lane), mode, b)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }

        if (opcode == Opcode::VDIV && b.isZero()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_DIV_ZERO, mode);
            continue;
        }

        TernaryValue result = TernaryValue::zero(mode);
        if (opcode == Opcode::VADD) result = addValue(a, b, mode);
        else if (opcode == Opcode::VSUB) result = subtractValue(a, b, mode);
        else if (opcode == Opcode::VMUL) result = multiplyValue(a, b, mode);
        else if (opcode == Opcode::VDIV) result = divideValue(a, b, mode);

        if (result.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
        } else {
            vm.vregfile.reg[vd].write(lane, result);
        }
    }
}

inline void writeVectorNeg(
    VMState& vm,
    uint8_t vd,
    uint8_t vs,
    TernaryMode mode) {

    bool ok = false;
    switch (mode) {
        case TernaryMode::T1:  ok = tryBatchVectorNeg<TritLane1, T1>(vm, vd, vs, mode); break;
        case TernaryMode::T5:  ok = tryBatchVectorNeg<TritLane5, T5>(vm, vd, vs, mode); break;
        default: break;
    }
    if (ok) return;

    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue src;
        if (!convertVectorNumericLane(vm.vregfile.reg[vs].read(lane), mode, src)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }

        TernaryValue result = negateValue(src, mode);
        if (result.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
        } else {
            vm.vregfile.reg[vd].write(lane, result);
        }
    }
}

inline void writeVectorCompare(
    VMState& vm,
    uint8_t vd,
    uint8_t va,
    uint8_t vb,
    TernaryMode mode) {

    bool ok = false;
    switch (mode) {
        case TernaryMode::T1:  ok = tryBatchVectorCompare<TritLane1>(vm, vd, va, vb, mode); break;
        case TernaryMode::T5:  ok = tryBatchVectorCompare<TritLane5>(vm, vd, va, vb, mode); break;
        default: break;
    }
    if (ok) return;

    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue a;
        TernaryValue b;
        if (!convertVectorNumericLane(vm.vregfile.reg[va].read(lane), mode, a) ||
            !convertVectorNumericLane(vm.vregfile.reg[vb].read(lane), mode, b)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, TernaryMode::L1);
            continue;
        }
        vm.vregfile.reg[vd].write(lane, makePredicateLane(compareValue(a, b, mode)));
    }
}

inline void writeVectorSelect(
    VMState& vm,
    uint8_t vd,
    uint8_t vcond,
    uint8_t vneg,
    uint8_t vzero,
    uint8_t vpos,
    TernaryMode mode) {

    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue cond = vm.vregfile.reg[vcond].read(lane);
        TernaryValue neg = vm.vregfile.reg[vneg].read(lane);
        TernaryValue zero = vm.vregfile.reg[vzero].read(lane);
        TernaryValue pos = vm.vregfile.reg[vpos].read(lane);
        if (cond.mode != TernaryMode::L1 || cond.isInvalid() ||
            !exactVectorNumericLane(neg, mode) ||
            !exactVectorNumericLane(zero, mode) ||
            !exactVectorNumericLane(pos, mode)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }

        const int8_t trit = cond.asL1().tritAt(0);
        vm.vregfile.reg[vd].write(lane, trit < 0 ? neg : (trit > 0 ? pos : zero));
    }
}

[[nodiscard]] inline bool accumulatorSource(
    VMState& vm,
    uint8_t scalarReg,
    TernaryMode sourceMode,
    TernaryValue& outNative) {

    if (!isNumericMode(sourceMode)) return false;
    TernaryValue source = vm.regfile.read(scalarReg);
    if (!isNumericMode(source.mode) || source.isInvalid()) return false;
    TernaryValue typed = convertValue(source, sourceMode);
    if (typed.isInvalid()) return false;
    outNative = convertValue(typed, TernaryMode::T40);
    return outNative.mode == TernaryMode::T40 && !outNative.isInvalid();
}

[[nodiscard]] inline bool t1Product(TernaryValue lhs, TernaryValue rhs, int8_t& product) {
    if (lhs.mode != TernaryMode::L1 || rhs.mode != TernaryMode::L1 ||
        lhs.isInvalid() || rhs.isInvalid()) {
        product = 0;
        return false;
    }

    const int8_t a = lhs.asL1().tritAt(0);
    const int8_t b = rhs.asL1().tritAt(0);
    if (a == 0 || b == 0) {
        product = 0;
    } else {
        product = (a == b) ? T_POS : T_NEG;
    }
    return true;
}

[[nodiscard]] inline TernaryValue vectorDotT1(
    VMState& vm,
    uint8_t va,
    uint8_t vb) {

    LongTriple sum = native_ops::fromInt(0);
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        int8_t product = 0;
        if (!t1Product(vm.vregfile.reg[va].read(lane),
                       vm.vregfile.reg[vb].read(lane),
                       product)) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        if (product != 0) {
            sum = native_ops::add(sum, native_ops::fromInt(product));
        }
    }
    // VDOT.T1 is an architectural T40 scalar result. Returning a tagged T50
    // here would claim a register pair and let an unrelated write to rd+1
    // corrupt the dot result.
    return TernaryValue::fromTriple(native_ops::toT40(sum));
}

inline void writeVectorActivateT1(VMState& vm, uint8_t vd, uint8_t vs) {
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue source = vm.vregfile.reg[vs].read(lane);
        if (!isNumericMode(source.mode) || source.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, TernaryMode::L1);
            continue;
        }
        vm.vregfile.reg[vd].write(lane, makePredicateLane(signValue(source, source.mode)));
    }
}

inline void writeVectorConvert(
    VMState& vm,
    uint8_t vd,
    uint8_t vs,
    TernaryMode sourceMode,
    TernaryMode targetMode) {

    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue source;
        if (!convertVectorNumericLane(vm.vregfile.reg[vs].read(lane), sourceMode, source)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, targetMode);
            continue;
        }
        TernaryValue converted = convertValue(source, targetMode);
        if (converted.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, targetMode);
        } else {
            vm.vregfile.reg[vd].write(lane, converted);
        }
    }
}

inline void writeVectorPermute(
    VMState& vm,
    uint8_t vd,
    uint8_t vs,
    uint8_t vindex,
    TernaryMode mode) {

    const std::vector<TernaryValue> source = vm.vregfile.reg[vs].lane;
    const std::vector<TernaryValue> indices = vm.vregfile.reg[vindex].lane;
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue idx = indices[static_cast<std::size_t>(lane)];
        if (!isNumericMode(idx.mode) || idx.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }
        const long long index = ops::toLong(idx);
        if (index < 0 || index >= vm.vector_length) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }
        TernaryValue selected;
        if (!convertVectorNumericLane(source[static_cast<std::size_t>(index)], mode, selected)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }
        vm.vregfile.reg[vd].write(lane, selected);
    }
}

inline void writeVectorGather(
    VMState& vm,
    uint8_t vd,
    uint8_t baseReg,
    uint8_t vindex,
    TernaryMode mode) {

    TernaryValue baseValue = vm.regfile.read(baseReg);
    if (!isNumericMode(baseValue.mode) || baseValue.isInvalid()) {
        vm.trap(TrapCode::TRAP_ILLEGAL_OP);
        return;
    }
    const long long base = ops::toLong(baseValue);
    const std::vector<TernaryValue> indices = vm.vregfile.reg[vindex].lane;
    std::vector<TernaryValue> loaded_lanes(
        static_cast<std::size_t>(vm.vector_length),
        TernaryValue::zero(mode));
    int first_routed_cause = OS_CAUSE_LOAD_FAULT;
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue idx = indices[static_cast<std::size_t>(lane)];
        if (!isNumericMode(idx.mode) || idx.isInvalid()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        const long long addrLong = base + ops::toLong(idx);
        if (addrLong < std::numeric_limits<int>::min() ||
            addrLong > std::numeric_limits<int>::max()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        int physical_addr = static_cast<int>(addrLong);
        int cause = OS_CAUSE_LOAD_FAULT;
        if (vm.privilege != PrivilegeMode::Kernel &&
            !vm.translateLoadAddress(static_cast<int>(addrLong), physical_addr, cause)) {
            if (!vm.vector_faults.any()) first_routed_cause = cause;
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        auto [loaded, fc] = vm.dmem.load(physical_addr);
        if (fc != MemFaultCode::OK) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        TernaryValue converted;
        if (!convertVectorNumericLane(loaded, mode, converted)) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        loaded_lanes[static_cast<std::size_t>(lane)] = converted;
    }
    if (vm.vector_faults.any()) {
        const int first = vm.vector_faults.first_failing_lane;
        const TrapCode code =
            vm.vector_faults.fault_class[static_cast<std::size_t>(first)];
        vm.trapWithCause(
            code,
            code == TrapCode::TRAP_MEM_FAULT
                ? first_routed_cause
                : OS_CAUSE_ILLEGAL_INSTRUCTION,
            vm.pc);
        return;
    }
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[vd].write(
            lane, loaded_lanes[static_cast<std::size_t>(lane)]);
    }
}

inline void writeVectorScatter(
    VMState& vm,
    uint8_t vs,
    uint8_t baseReg,
    uint8_t vindex,
    TernaryMode mode) {

    TernaryValue baseValue = vm.regfile.read(baseReg);
    if (!isNumericMode(baseValue.mode) || baseValue.isInvalid()) {
        vm.trap(TrapCode::TRAP_ILLEGAL_OP);
        return;
    }
    const long long base = ops::toLong(baseValue);
    const std::vector<TernaryValue> source = vm.vregfile.reg[vs].lane;
    const std::vector<TernaryValue> indices = vm.vregfile.reg[vindex].lane;
    std::vector<int> physical_addresses(
        static_cast<std::size_t>(vm.vector_length), -1);
    std::vector<TernaryValue> stored_lanes(
        static_cast<std::size_t>(vm.vector_length),
        TernaryValue::zero(mode));
    int first_routed_cause = OS_CAUSE_STORE_FAULT;
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue idx = indices[static_cast<std::size_t>(lane)];
        if (!isNumericMode(idx.mode) || idx.isInvalid()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        const long long addrLong = base + ops::toLong(idx);
        if (addrLong < std::numeric_limits<int>::min() ||
            addrLong > std::numeric_limits<int>::max()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        int physical_addr = static_cast<int>(addrLong);
        int cause = OS_CAUSE_STORE_FAULT;
        if (vm.privilege != PrivilegeMode::Kernel &&
            !vm.translateStoreAddress(static_cast<int>(addrLong), physical_addr, cause)) {
            if (!vm.vector_faults.any()) first_routed_cause = cause;
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            continue;
        }
        TernaryValue converted;
        if (!convertVectorNumericLane(source[static_cast<std::size_t>(lane)], mode, converted)) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        physical_addresses[static_cast<std::size_t>(lane)] = physical_addr;
        stored_lanes[static_cast<std::size_t>(lane)] = converted;
    }
    if (vm.vector_faults.any()) {
        const int first = vm.vector_faults.first_failing_lane;
        const TrapCode code =
            vm.vector_faults.fault_class[static_cast<std::size_t>(first)];
        vm.trapWithCause(
            code,
            code == TrapCode::TRAP_MEM_FAULT
                ? first_routed_cause
                : OS_CAUSE_ILLEGAL_INSTRUCTION,
            vm.pc);
        return;
    }
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        const int physical_addr =
            physical_addresses[static_cast<std::size_t>(lane)];
        if (vm.dmem.store(
                physical_addr,
                stored_lanes[static_cast<std::size_t>(lane)]) !=
            MemFaultCode::OK) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
            vm.trapWithCause(
                TrapCode::TRAP_MEM_FAULT,
                OS_CAUSE_STORE_FAULT,
                vm.pc);
            return;
        }
        vm.noteStoreForReservation(physical_addr);
    }
}

} // namespace exec

// Scalar memory addresses are architectural integers, even though their
// source register is a tagged ternary value. Keep this check shared by the
// portable and decoded paths so an invalid T40 payload cannot silently turn
// into address zero, and a saturated value cannot wrap when the immediate is
// added before the VM's int-addressed memories are consulted.
enum class ScalarMemoryAddressStatus : uint8_t {
    Valid,
    InvalidOperand,
    OutOfRange,
};

[[nodiscard]] inline ScalarMemoryAddressStatus checkedScalarMemoryAddress(
    const TernaryValue& base_value,
    int immediate,
    int& out_address) {
    if (!isNumericMode(base_value.mode) || base_value.isInvalid()) {
        return ScalarMemoryAddressStatus::InvalidOperand;
    }

    const long long base = ops::toLong(base_value);
    const long long offset = static_cast<long long>(immediate);
    if ((offset > 0 && base > std::numeric_limits<long long>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<long long>::min() - offset)) {
        return ScalarMemoryAddressStatus::OutOfRange;
    }
    const long long address = base + offset;
    if (address < std::numeric_limits<int>::min() ||
        address > std::numeric_limits<int>::max()) {
        return ScalarMemoryAddressStatus::OutOfRange;
    }
    out_address = static_cast<int>(address);
    return ScalarMemoryAddressStatus::Valid;
}

// =============================================================================
// SECTION 2 — Single-Step Executor
// =============================================================================

// Execute exactly one instruction in the given VMState.
// Returns the VMStatus after execution:
//   RUNNING  — instruction completed normally; PC has been advanced.
//   HALTED   — HALT instruction encountered; PC unchanged.
//   TRAPPED  — fault occurred; PC unchanged; trap_reg written.
//
// Calling step() on a HALTED or TRAPPED VMState is a no-op (returns status).
struct VMExecutionRecord {
    bool attempted = false;
    bool has_instruction = false;
    bool malformed = false;
    int pc = -1;
    int physical_pc = -1;
    int final_pc = -1;
    std::uint64_t raw_bits = 0;
    InstructionWord instruction;
    VMStatus before_status = VMStatus::RUNNING;
    VMStatus after_status = VMStatus::RUNNING;
    PrivilegeMode before_privilege = PrivilegeMode::Kernel;
    PrivilegeMode after_privilege = PrivilegeMode::Kernel;
    int process_id = -1;
    bool branch_observed = false;
    bool branch_conditional = false;
    bool branch_taken = false;
    int branch_target = -1;
    bool trap_observed = false;
    TrapCode trap_code = TrapCode::TRAP_ILLEGAL_OP;
    int trap_cause = 0;
    int syscall_id = -1;
    long long syscall_arg0 = 0;
    long long syscall_arg1 = 0;
    long long syscall_arg2 = 0;
    long long syscall_arg3 = 0;
    long long syscall_result0 = 0;
    long long syscall_result1 = 0;
    long long syscall_result2 = 0;
    std::uint64_t cycle_before = 0;
    std::uint64_t cycle_after = 0;
};

[[nodiscard]] inline int currentProcessForProfile(const VMState& vm) {
    if (vm.coreCount() <= 0) return -1;
    const int core = std::max(0, std::min(vm.active_core, vm.coreCount() - 1));
    return vm.coreState(core).current_process;
}

inline VMStatus step(VMState& vm, VMExecutionRecord* record = nullptr) {
    if (record) *record = VMExecutionRecord{};
    if (!vm.isRunning()) return vm.status;

    if (record) {
        record->attempted = true;
        record->pc = vm.pc;
        record->final_pc = vm.pc;
        record->before_status = vm.status;
        record->before_privilege = vm.privilege;
        record->after_status = vm.status;
        record->after_privilege = vm.privilege;
        record->process_id = currentProcessForProfile(vm);
        record->cycle_before = vm.cycle_count;
    }

    struct RecordFinalizer {
        VMState& vm;
        VMExecutionRecord* record;

        ~RecordFinalizer() {
            if (!record || !record->attempted) return;
            record->after_status = vm.status;
            record->after_privilege = vm.privilege;
            record->final_pc = vm.pc;
            record->cycle_after = vm.cycle_count;
            if (record->has_instruction &&
                record->instruction.opcode == Opcode::SYSCALL) {
                record->syscall_result0 = ops::toLong(vm.regfile.read(13));
                record->syscall_result1 = ops::toLong(vm.regfile.read(14));
                record->syscall_result2 = ops::toLong(vm.regfile.read(15));
            }
            if (record->has_instruction &&
                record->instruction.opcode == Opcode::SYSCALL &&
                record->syscall_id < 0) {
                record->syscall_id = record->instruction.imm;
            }
            if (vm.isTrapped()) {
                record->trap_observed = true;
                record->trap_code = decodeTrap(vm.trap_reg);
            } else if (record->has_instruction &&
                       record->instruction.opcode == Opcode::SYSCALL &&
                       record->before_privilege == PrivilegeMode::User &&
                       vm.privilege == PrivilegeMode::Kernel &&
                       vm.epc == record->pc) {
                record->trap_observed = true;
                record->trap_code = TrapCode::TRAP_ILLEGAL_OP;
                record->trap_cause = OS_CAUSE_SYSCALL;
            } else if (record->before_privilege != PrivilegeMode::Kernel &&
                       vm.privilege == PrivilegeMode::Kernel &&
                       vm.epc == record->pc) {
                record->trap_observed = true;
                record->trap_code = decodeTrap(vm.trap_reg);
                record->trap_cause = vm.cause;
            }
            if (record->trap_observed && record->trap_cause == 0) {
                record->trap_cause =
                    vm.cause != 0 ? vm.cause : VMState::osCauseForTrap(record->trap_code);
            }
        }
    } record_finalizer{vm, record};

    // -----------------------------------------------------------------
    // FETCH
    // -----------------------------------------------------------------
    int physical_pc = vm.pc;
    int routed_cause = OS_CAUSE_FETCH_FAULT;
    if (!vm.translateFetchAddress(vm.pc, physical_pc, routed_cause)) {
        vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, routed_cause, vm.pc);
        return vm.status;
    }
    auto [raw, fetch_fc] = vm.imem.fetch(physical_pc);
    if (fetch_fc != MemFaultCode::OK) {
        vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_FETCH_FAULT, vm.pc);
        return vm.status;
    }
    if (record) {
        record->physical_pc = physical_pc;
        record->raw_bits = raw.bits;
    }

    // -----------------------------------------------------------------
    // DECODE
    // -----------------------------------------------------------------
    ++vm.decode_instructions_count;
    InstructionWord iw =
        VersionedInstructionCodec::decode(
            raw, IsaEncodingVersion::V2);
    if (record) {
        record->has_instruction = true;
        record->malformed = iw.malformed;
        record->instruction = iw;
        if (iw.opcode == Opcode::SYSCALL) {
            record->syscall_id = iw.imm;
            record->syscall_arg0 = ops::toLong(vm.regfile.read(13));
            record->syscall_arg1 = ops::toLong(vm.regfile.read(14));
            record->syscall_arg2 = ops::toLong(vm.regfile.read(15));
            record->syscall_arg3 = ops::toLong(vm.regfile.read(16));
        }
    }
    if (iw.malformed || iw.opcode == Opcode::RESERVED) {
        vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP, OS_CAUSE_ILLEGAL_INSTRUCTION, vm.pc);
        return vm.status;
    }

    // -----------------------------------------------------------------
    // EXECUTE + WRITEBACK
    // After this block: either vm.status has changed (HALT/TRAP) or
    // the instruction completed normally and pc_next holds the new PC.
    // -----------------------------------------------------------------
    int pc_next = vm.pc + 1;  // default: advance by one word

    auto noteBranch = [&](int target, bool taken, bool conditional) {
        if (!record) return;
        record->branch_observed = true;
        record->branch_target = target;
        record->branch_taken = taken;
        record->branch_conditional = conditional;
    };

    auto decodeWidth = [&]() -> std::pair<bool, TernaryMode> {
        TernaryMode mode = TernaryMode::T40;
        if (!exec::numericModeFromFunc(iw.func, mode)) {
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return {false, mode};
        }
        return {true, mode};
    };

    auto decodeLaneWidth = [&]() -> std::pair<bool, TernaryMode> {
        TernaryMode mode = TernaryMode::L50;
        if (!exec::laneModeFromFunc(iw.func, mode)) {
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return {false, mode};
        }
        return {true, mode};
    };

    auto decodeAtomicOrder = [&]() -> std::pair<bool, int> {
        if (!isAtomicOrderFunc(iw.func)) {
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return {false, ATOMIC_ORDER_ACQ_REL};
        }
        return {true, atomicOrderFromFunc(iw.func)};
    };

    auto writeChecked = [&](uint8_t rd, TernaryValue value) -> bool {
        if (value.isInvalid()) {
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return false;
        }
        vm.regfile.write(rd, value);
        return true;
    };

    switch (iw.opcode) {

        // ----- System -----------------------------------------------

        case Opcode::NOP:
            // Nothing to do.
            break;

        case Opcode::HALT:
            vm.completeTerminalInstruction();
            vm.halt();
            return vm.status;

        case Opcode::WAIT: {
            const std::uint64_t wait_feature =
                featureBit(architecture::v2::FEATURE_WAIT);
            if ((vm.supported_features & wait_feature) == 0) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            vm.completeInstruction(pc_next);
            if (!vm.timer_pending) vm.enterWaiting();
            return vm.status;
        }

        case Opcode::TLBINV: {
            const std::uint64_t mmu_feature =
                featureBit(architecture::v2::FEATURE_MMU);
            if ((vm.supported_features & mmu_feature) == 0 ||
                vm.privilege != PrivilegeMode::Kernel) {
                vm.trapWithCause(
                    TrapCode::TRAP_ILLEGAL_OP,
                    vm.privilege == PrivilegeMode::Kernel
                        ? OS_CAUSE_ILLEGAL_INSTRUCTION
                        : OS_CAUSE_PROTECTION_FAULT,
                    vm.pc);
                return vm.status;
            }
            const long long address =
                ops::toLong(vm.regfile.read(iw.rd));
            const long long target_asid =
                ops::toLong(vm.regfile.read(iw.rs1));
            const long long scope =
                ops::toLong(vm.regfile.read(iw.rs2));
            if (address < -1 || target_asid < -1 ||
                target_asid >= 19683 || scope < 0 || scope > 3) {
                vm.trapWithCause(
                    TrapCode::TRAP_ILLEGAL_OP,
                    OS_CAUSE_ILLEGAL_INSTRUCTION,
                    vm.pc);
                return vm.status;
            }
            vm.invalidateTlb(
                static_cast<int>(address),
                static_cast<int>(target_asid),
                static_cast<int>(scope));
            break;
        }

        case Opcode::CSRR: {
            TernaryValue value;
            if (!vm.readCSR(iw.imm, value)) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            vm.regfile.write(iw.rd, value);
            break;
        }

        case Opcode::CSRW: {
            if (vm.privilege != PrivilegeMode::Kernel && iw.imm < 22) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_PROTECTION_FAULT,
                                 vm.pc);
                return vm.status;
            }
            if (!vm.writeCSR(iw.imm, vm.regfile.read(iw.rd))) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            break;
        }

        case Opcode::CSRRW: {
            if (vm.privilege != PrivilegeMode::Kernel && iw.rs2 < 22) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_PROTECTION_FAULT,
                                 vm.pc);
                return vm.status;
            }
            TernaryValue old_value;
            if (!vm.readCSR(iw.rs2, old_value)) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            const TernaryValue source_value = vm.regfile.read(iw.rs1);
            if (!vm.writeCSR(iw.rs2, source_value)) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            vm.regfile.write(iw.rd, old_value);
            break;
        }

        case Opcode::ERET: {
            if (vm.privilege != PrivilegeMode::Kernel) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_PROTECTION_FAULT,
                                 vm.pc);
                return vm.status;
            }
            if (!vm.returnFromTrap()) {
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP,
                                 OS_CAUSE_ILLEGAL_INSTRUCTION,
                                 vm.pc);
                return vm.status;
            }
            noteBranch(vm.epc, true, false);
            vm.recordCycle(true);
            return vm.status;
        }

        // ----- Data Movement ----------------------------------------

        case Opcode::MOV: {
            // I-type: Rd ← sign-extended imm16 as a LongTriple integer.
            vm.regfile.write(iw.rd, sandbox::vm::ops::fromLong(iw.imm));
            break;
        }

        case Opcode::MOVH: {
            // I-type: Rd ← (current Rd lower 16 trits) | (imm << 16 trits)
            // Loads the upper half of a 32-trit constant. Use MOV then MOVH
            // to construct large immediates: MOV loads low 16, MOVH loads high 16.
            //
            // Algorithm: unpack rd → 50 trits, decompose imm into balanced
            // ternary and write into trit positions [16..31], zero [32..49],
            // then repack.
            TernaryValue current = vm.regfile.read(iw.rd);
            LongTriple currentLT = current.toLongTriple();
            auto trits = currentLT.unpack();

            // Decompose imm into balanced ternary trits for positions [16..31].
            int immVal = iw.imm;
            for (int i = 16; i < 32; ++i) {
                int r = (immVal + 1) % 3;
                if (r < 0) r += 3;
                int8_t trit = static_cast<int8_t>(r - 1);
                trits[i] = trit;
                immVal = (immVal - trit) / 3;
            }
            // Zero out trits [32..49] to avoid stale data from prior rd value.
            for (int i = 32; i < 50; ++i) trits[i] = 0;

            vm.regfile.write(iw.rd, TernaryValue::fromLongTriple(LongTriple::pack(trits)));
            break;
        }

        case Opcode::COPY: {
            // R-type: Rd ← Rs1
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            vm.regfile.write(iw.rd, vm.regfile.readView(iw.rs1, mode));
            break;
        }

        case Opcode::SWAP: {
            TernaryValue a = vm.regfile.read(iw.rd);
            TernaryValue b = vm.regfile.read(iw.rs1);
            vm.regfile.write(iw.rd, b);
            vm.regfile.write(iw.rs1, a);
            break;
        }

        // ----- Arithmetic -------------------------------------------

        case Opcode::ADD: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::addValue(vm.regfile.readView(iw.rs1, mode),
                                                    vm.regfile.readView(iw.rs2, mode), mode))) return vm.status;
            break;
        }

        case Opcode::SUB: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::subtractValue(vm.regfile.readView(iw.rs1, mode),
                                                         vm.regfile.readView(iw.rs2, mode), mode))) return vm.status;
            break;
        }

        case Opcode::MUL: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::multiplyValue(vm.regfile.readView(iw.rs1, mode),
                                                         vm.regfile.readView(iw.rs2, mode), mode))) return vm.status;
            break;
        }

        case Opcode::DIV: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue b = vm.regfile.readView(iw.rs2, mode);
            if (b.isZero()) {
                vm.trapWithCause(TrapCode::TRAP_DIV_ZERO, OS_CAUSE_DIV_ZERO, vm.pc);
                return vm.status;
            }
            if (!writeChecked(iw.rd, exec::divideValue(vm.regfile.readView(iw.rs1, mode), b, mode))) return vm.status;
            break;
        }

        case Opcode::SQRT: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue t = vm.regfile.readView(iw.rs1, mode);
            if (exec::signValue(t, mode) == T_NEG) {
                // sqrt of negative: trap as illegal operation.
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (!writeChecked(iw.rd, exec::sqrtValue(t, mode))) return vm.status;
            break;
        }

        case Opcode::NEG: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::negateValue(vm.regfile.readView(iw.rs1, mode), mode))) return vm.status;
            break;
        }

        case Opcode::ABS: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::absValue(vm.regfile.readView(iw.rs1, mode), mode))) return vm.status;
            break;
        }

        // ----- Compare & Ternary Logic ------------------------------

        case Opcode::TCMP: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            // Rd ← sign(Rs1 - Rs2) stored as single-trit LongTriple in trit[0].
            // Contract (from ternary_isa.h Section 5):
            //   result == T_NEG (-1):  Rs1 < Rs2
            //   result == T_ZER ( 0):  Rs1 == Rs2
            //   result == T_POS (+1):  Rs1 > Rs2
            int8_t cmp = exec::compareValue(vm.regfile.readView(iw.rs1, mode),
                                            vm.regfile.readView(iw.rs2, mode), mode);
            vm.regfile.write(iw.rd, makeTritResult(cmp));
            break;
        }

        case Opcode::TMIN: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = vm.regfile.readView(iw.rs1, mode);
            TernaryValue b = vm.regfile.readView(iw.rs2, mode);
            if (!writeChecked(iw.rd, exec::compareValue(a, b, mode) == T_POS ? b : a)) return vm.status;
            break;
        }

        case Opcode::TMAX: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = vm.regfile.readView(iw.rs1, mode);
            TernaryValue b = vm.regfile.readView(iw.rs2, mode);
            if (!writeChecked(iw.rd, exec::compareValue(a, b, mode) == T_NEG ? b : a)) return vm.status;
            break;
        }

        case Opcode::TINV: {
            // Alias for NEG: trit-flip all mantissa trits. TINV(TINV(x)) == x.
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::negateValue(vm.regfile.readView(iw.rs1, mode), mode))) return vm.status;
            break;
        }

        case Opcode::TLADD:
        case Opcode::TLSUB:
        case Opcode::TLAND:
        case Opcode::TLOR: {
            auto [ok, mode] = decodeLaneWidth(); if (!ok) return vm.status;
            exec::LaneOp op = exec::LaneOp::Add;
            if (iw.opcode == Opcode::TLSUB) op = exec::LaneOp::Sub;
            else if (iw.opcode == Opcode::TLAND) op = exec::LaneOp::And;
            else if (iw.opcode == Opcode::TLOR) op = exec::LaneOp::Or;
            if (!writeChecked(iw.rd, exec::laneBinaryValue(
                    vm.regfile.readView(iw.rs1, mode),
                    vm.regfile.readView(iw.rs2, mode), mode, op))) return vm.status;
            break;
        }

        case Opcode::TLNEG: {
            auto [ok, mode] = decodeLaneWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::laneUnaryValue(
                    vm.regfile.readView(iw.rs1, mode),
                    mode, exec::LaneOp::Neg))) return vm.status;
            break;
        }

        case Opcode::TSEL: {
            const int8_t cond = readTrit0(vm.regfile.read(iw.rcond));
            const uint8_t src = cond < 0 ? iw.rneg : (cond > 0 ? iw.rpos : iw.rzero);
            vm.regfile.write(iw.rd, vm.regfile.read(src));
            break;
        }

        case Opcode::CVT: {
            TernaryMode mode = TernaryMode::T40;
            if (!exec::modeFromFunc(iw.func, mode)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue source = vm.regfile.read(iw.rs1);
            if (isWidthFunc(iw.rs2)) {
                TernaryMode sourceMode = TernaryMode::T40;
                if (!exec::modeFromFunc(iw.rs2, sourceMode)) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
                if ((isNumericMode(sourceMode) && !isNumericMode(source.mode)) ||
                    (isLaneMode(sourceMode) && !isLaneMode(source.mode))) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
                source = convertValue(source, sourceMode);
                if (source.isInvalid()) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
            } else if (iw.rs2 != R0_ZERO || isLaneMode(mode)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (!writeChecked(iw.rd, convertValue(source, mode))) return vm.status;
            break;
        }

        // ----- Vector Reference Operations --------------------------

        case Opcode::VADD:
        case Opcode::VSUB:
        case Opcode::VMUL:
        case Opcode::VDIV: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rs1) ||
                !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorBinaryNumeric(vm, iw.rd, iw.rs1, iw.rs2, mode, iw.opcode);
            break;
        }

        case Opcode::VNEG: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) || !exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorNeg(vm, iw.rd, iw.rs1, mode);
            break;
        }

        case Opcode::VCMP: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rs1) ||
                !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorCompare(vm, iw.rd, iw.rs1, iw.rs2, mode);
            break;
        }

        case Opcode::VSEL: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rcond) ||
                !exec::validVectorReg(iw.rneg) ||
                !exec::validVectorReg(iw.rzero) ||
                !exec::validVectorReg(iw.rpos)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorSelect(vm, iw.rd, iw.rcond, iw.rneg, iw.rzero, iw.rpos, mode);
            break;
        }

        case Opcode::VBCAST: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue source;
            if (!exec::convertVectorNumericLane(vm.regfile.read(iw.rs1), mode, source)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                vm.vregfile.reg[iw.rd].write(lane, source);
            }
            break;
        }

        case Opcode::VLOAD: {
            TernaryMode mode = TernaryMode::T40;
            if (!exec::numericModeFromFunc(iw.func, mode) ||
                !exec::validVectorReg(iw.rd)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue baseReg = vm.regfile.read(iw.rs1);
            if (!isNumericMode(baseReg.mode) || baseReg.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            const long long base = ops::toLong(baseReg);
            exec::prepareVectorOp(vm);
            std::vector<TernaryValue> loaded_lanes(
                static_cast<std::size_t>(vm.vector_length),
                TernaryValue::zero(mode));
            int first_routed_cause = OS_CAUSE_LOAD_FAULT;
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                const long long addrLong = base + iw.imm + lane;
                if (addrLong < std::numeric_limits<int>::min() ||
                    addrLong > std::numeric_limits<int>::max()) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                int physical_addr = static_cast<int>(addrLong);
                int cause = OS_CAUSE_LOAD_FAULT;
                if (vm.privilege != PrivilegeMode::Kernel &&
                    !vm.translateLoadAddress(static_cast<int>(addrLong), physical_addr, cause)) {
                    if (!vm.vector_faults.any())
                        first_routed_cause = cause;
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                auto [loaded, fc] = vm.dmem.load(physical_addr);
                if (fc != MemFaultCode::OK) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                TernaryValue converted;
                if (!exec::convertVectorNumericLane(loaded, mode, converted)) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_ILLEGAL_OP);
                    continue;
                }
                loaded_lanes[static_cast<std::size_t>(lane)] = converted;
            }
            if (vm.vector_faults.any()) {
                const int first = vm.vector_faults.first_failing_lane;
                const TrapCode code = vm.vector_faults.fault_class[
                    static_cast<std::size_t>(first)];
                vm.trapWithCause(
                    code,
                    code == TrapCode::TRAP_MEM_FAULT
                        ? first_routed_cause
                        : OS_CAUSE_ILLEGAL_INSTRUCTION,
                    vm.pc);
                return vm.status;
            }
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                vm.vregfile.reg[iw.rd].write(
                    lane,
                    loaded_lanes[static_cast<std::size_t>(lane)]);
            }
            break;
        }

        case Opcode::VSTORE: {
            TernaryMode mode = TernaryMode::T40;
            if (!exec::numericModeFromFunc(iw.func, mode) ||
                !exec::validVectorReg(iw.rd)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue baseReg = vm.regfile.read(iw.rs1);
            if (!isNumericMode(baseReg.mode) || baseReg.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            const long long base = ops::toLong(baseReg);
            exec::prepareVectorOp(vm);
            std::vector<int> physical_addresses(
                static_cast<std::size_t>(vm.vector_length), -1);
            std::vector<TernaryValue> stored_lanes(
                static_cast<std::size_t>(vm.vector_length),
                TernaryValue::zero(mode));
            int first_routed_cause = OS_CAUSE_STORE_FAULT;
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                const long long addrLong = base + iw.imm + lane;
                if (addrLong < std::numeric_limits<int>::min() ||
                    addrLong > std::numeric_limits<int>::max()) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                int physical_addr = static_cast<int>(addrLong);
                int cause = OS_CAUSE_STORE_FAULT;
                if (vm.privilege != PrivilegeMode::Kernel &&
                    !vm.translateStoreAddress(static_cast<int>(addrLong), physical_addr, cause)) {
                    if (!vm.vector_faults.any())
                        first_routed_cause = cause;
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
                    vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
                    continue;
                }
                TernaryValue converted;
                if (!exec::convertVectorNumericLane(vm.vregfile.reg[iw.rd].read(lane), mode, converted)) {
                    vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
                    continue;
                }
                physical_addresses[static_cast<std::size_t>(lane)] =
                    physical_addr;
                stored_lanes[static_cast<std::size_t>(lane)] = converted;
            }
            if (vm.vector_faults.any()) {
                const int first = vm.vector_faults.first_failing_lane;
                const TrapCode code = vm.vector_faults.fault_class[
                    static_cast<std::size_t>(first)];
                vm.trapWithCause(
                    code,
                    code == TrapCode::TRAP_MEM_FAULT
                        ? first_routed_cause
                        : OS_CAUSE_ILLEGAL_INSTRUCTION,
                    vm.pc);
                return vm.status;
            }
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                const int physical_addr =
                    physical_addresses[static_cast<std::size_t>(lane)];
                if (vm.dmem.store(
                        physical_addr,
                        stored_lanes[static_cast<std::size_t>(lane)]) !=
                    MemFaultCode::OK) {
                    vm.vector_faults.setLane(
                        lane, TrapCode::TRAP_MEM_FAULT);
                    vm.trapWithCause(
                        TrapCode::TRAP_MEM_FAULT,
                        OS_CAUSE_STORE_FAULT,
                        vm.pc);
                    return vm.status;
                }
                vm.noteStoreForReservation(physical_addr);
            }
            break;
        }

        case Opcode::ACLR: {
            if (!isNumericWidthFunc(iw.func)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            vm.accumulator = TernaryValue::zero(TernaryMode::T40);
            break;
        }

        case Opcode::ALOAD:
        case Opcode::AADD:
        case Opcode::ASUB:
        case Opcode::AMUL: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue sourceNative;
            if (!exec::accumulatorSource(vm, iw.rs1, mode, sourceNative)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }

            if (iw.opcode == Opcode::ALOAD) {
                vm.accumulator = sourceNative;
            } else if (iw.opcode == Opcode::AADD) {
                vm.accumulator = exec::addValue(vm.accumulator, sourceNative, TernaryMode::T40);
            } else if (iw.opcode == Opcode::ASUB) {
                vm.accumulator = exec::subtractValue(vm.accumulator, sourceNative, TernaryMode::T40);
            } else {
                vm.accumulator = exec::multiplyValue(vm.accumulator, sourceNative, TernaryMode::T40);
            }

            if (vm.accumulator.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            break;
        }

        case Opcode::ASTORE: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, convertValue(vm.accumulator, mode))) return vm.status;
            break;
        }

        case Opcode::VDOT:
        case Opcode::VMAC: {
            if (iw.func != FUNC_T1 ||
                !exec::validVectorReg(iw.rs1) ||
                !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            TernaryValue dot = exec::vectorDotT1(vm, iw.rs1, iw.rs2);
            if (dot.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (iw.opcode == Opcode::VDOT) {
                if (!writeChecked(iw.rd, dot)) return vm.status;
            } else {
                vm.accumulator = exec::addValue(vm.accumulator, dot, TernaryMode::T40);
                if (vm.accumulator.isInvalid()) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
            }
            break;
        }

        case Opcode::VACT: {
            if (iw.func != FUNC_T1 ||
                !exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorActivateT1(vm, iw.rd, iw.rs1);
            break;
        }

        case Opcode::VPACK:
        case Opcode::VUNPACK: {
            TernaryMode sourceMode = TernaryMode::T40;
            TernaryMode targetMode = TernaryMode::T40;
            if (!exec::numericModeFromFunc(iw.rs2, sourceMode) ||
                !exec::numericModeFromFunc(iw.func, targetMode) ||
                !exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorConvert(vm, iw.rd, iw.rs1, sourceMode, targetMode);
            break;
        }

        case Opcode::VPERMUTE: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rs1) ||
                !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorPermute(vm, iw.rd, iw.rs1, iw.rs2, mode);
            break;
        }

        case Opcode::VBLEND: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) ||
                !exec::validVectorReg(iw.rcond) ||
                !exec::validVectorReg(iw.rneg) ||
                !exec::validVectorReg(iw.rzero) ||
                !exec::validVectorReg(iw.rpos)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorSelect(vm, iw.rd, iw.rcond, iw.rneg, iw.rzero, iw.rpos, mode);
            break;
        }

        case Opcode::VSWAP: {
            if (!exec::validVectorReg(iw.rd) || !exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            std::swap(vm.vregfile.reg[iw.rd], vm.vregfile.reg[iw.rs1]);
            break;
        }

        case Opcode::VGATHER: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) || !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorGather(vm, iw.rd, iw.rs1, iw.rs2, mode);
            if (vm.isTrapped()) return vm.status;
            break;
        }

        case Opcode::VSCATTER: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rd) || !exec::validVectorReg(iw.rs2)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            exec::writeVectorScatter(vm, iw.rd, iw.rs1, iw.rs2, mode);
            if (vm.isTrapped()) return vm.status;
            break;
        }

        // ----- Memory -----------------------------------------------

        case Opcode::LOAD: {
            // I-type: Rd ← dmem[Rs1 + imm16]
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(vm.regfile.read(iw.rs1), iw.imm, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_LOAD_FAULT, vm.pc);
                return vm.status;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_LOAD_FAULT;
            if (!vm.translateLoadAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return vm.status;
            }
            auto [val, fc]   = vm.dmem.load(physical_addr);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_LOAD_FAULT, vm.pc);
                return vm.status;
            }
            vm.regfile.write(iw.rd, val);
            break;
        }

        case Opcode::STORE: {
            // I-type: dmem[Rs1 + imm16] ← rs_store
            // rs_store is the SOURCE register (the value to store).
            // Rs1 is the BASE ADDRESS register.
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(vm.regfile.read(iw.rs1), iw.imm, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_STORE_FAULT, vm.pc);
                return vm.status;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_STORE_FAULT;
            if (!vm.translateStoreAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return vm.status;
            }
            const TernaryValue store_value =
                vm.regfile.readPhysical(iw.rs_store);
            MemFaultCode fc = vm.dmem.store(physical_addr, store_value);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_STORE_FAULT, vm.pc);
                return vm.status;
            }
            vm.noteStoreForReservation(physical_addr);
            break;
        }

        case Opcode::TLDR: {
            auto [okOrder, order] = decodeAtomicOrder();
            (void)order;
            if (!okOrder) return vm.status;
            TernaryValue addrValue = vm.regfile.read(iw.rs1);
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(addrValue, 0, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_LOAD_FAULT, vm.pc);
                return vm.status;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_LOAD_FAULT;
            if (!vm.translateLoadAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return vm.status;
            }
            auto [val, fc] = vm.dmem.load(physical_addr);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_LOAD_FAULT, vm.pc);
                return vm.status;
            }
            vm.regfile.write(iw.rd, val);
            vm.setAtomicReservation(physical_addr);
            break;
        }

        case Opcode::TSTR: {
            auto [okOrder, order] = decodeAtomicOrder();
            (void)order;
            if (!okOrder) return vm.status;
            TernaryValue addrValue = vm.regfile.read(iw.rs1);
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(addrValue, 0, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_STORE_FAULT, vm.pc);
                return vm.status;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_STORE_FAULT;
            if (!vm.translateStoreAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return vm.status;
            }

            const bool reservation_matches =
                vm.atomic_reservation_valid &&
                vm.atomic_reservation_addr == physical_addr;
            vm.clearAtomicReservation();
            if (!reservation_matches) {
                vm.regfile.write(iw.rd, makeTritResult(T_NEG));
                break;
            }

            auto [current, fc] = vm.dmem.load(physical_addr);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_LOAD_FAULT, vm.pc);
                return vm.status;
            }
            if (current != vm.regfile.read(iw.rs3)) {
                vm.regfile.write(iw.rd, makeTritResult(T_ZER));
                break;
            }
            fc = vm.dmem.store(physical_addr, vm.regfile.read(iw.rs2));
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_STORE_FAULT, vm.pc);
                return vm.status;
            }
            vm.regfile.write(iw.rd, makeTritResult(T_POS));
            break;
        }

        // ----- Control Flow -----------------------------------------

        case Opcode::JMP: {
            // B-type: PC ← PC + offset19 (unconditional)
            pc_next = vm.pc + iw.offset;
            noteBranch(pc_next, true, false);
            break;
        }

        case Opcode::BRN: {
            vm.branch_instructions_count++;
            // B-type: if Rs.trit[0] == T_NEG → PC ← PC + offset19
            // Otherwise fall through to PC + 1.
            // This is the primary ternary comparison branch.
            // Use after TCMP: BRN r3, label executes when r3 == -1 (Rs1 < Rs2).
            int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            if (trit0 == T_NEG) {
                pc_next = vm.pc + iw.offset;
            }
            noteBranch(vm.pc + iw.offset, trit0 == T_NEG, true);
            break;
        }

        case Opcode::BRZ: {
            vm.branch_instructions_count++;
            int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            if (trit0 == T_ZER) {
                pc_next = vm.pc + iw.offset;
            }
            noteBranch(vm.pc + iw.offset, trit0 == T_ZER, true);
            break;
        }

        case Opcode::BRP: {
            vm.branch_instructions_count++;
            int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            if (trit0 == T_POS) {
                pc_next = vm.pc + iw.offset;
            }
            noteBranch(vm.pc + iw.offset, trit0 == T_POS, true);
            break;
        }

        case Opcode::CALL: {
            // B-type: r25 (LR) ← PC + 1; PC ← PC + offset19
            vm.regfile.writeLR(sandbox::vm::ops::fromLong(vm.pc + 1));
            pc_next = vm.pc + iw.offset;
            noteBranch(pc_next, true, false);
            break;
        }

        case Opcode::RET: {
            // R-type (no operands): PC ← r25 (LR)
            int ret_addr = exec::pcFromValue(vm.regfile.readLR());
            if (!vm.validateControlTarget(ret_addr)) {
                noteBranch(ret_addr, false, false);
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            pc_next = ret_addr;
            noteBranch(pc_next, true, false);
            break;
        }
        // ----- Phase 2 Extensions -----------------------------------

        case Opcode::TWCMP: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue s1 = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue s2 = convertValue(vm.regfile.read(iw.rs2), mode);
            TernaryValue s3 = convertValue(vm.regfile.read(iw.rs3), mode);
            if (!isNumericMode(s1.mode) || !isNumericMode(s2.mode) || !isNumericMode(s3.mode) ||
                s1.isInvalid() || s2.isInvalid() || s3.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (exec::compareValue(s2, s3, mode) == T_POS) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            int8_t res = 0;
            if (exec::compareValue(s1, s2, mode) == T_NEG) {
                res = T_NEG;
            } else if (exec::compareValue(s1, s3, mode) == T_POS) {
                res = T_POS;
            } else {
                res = T_ZER;
            }
            vm.regfile.write(iw.rd, makeTritResult(res));
            break;
        }

        case Opcode::TCLAMP: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue s1 = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue s2 = convertValue(vm.regfile.read(iw.rs2), mode);
            TernaryValue s3 = convertValue(vm.regfile.read(iw.rs3), mode);
            if (!isNumericMode(s1.mode) || !isNumericMode(s2.mode) || !isNumericMode(s3.mode) ||
                s1.isInvalid() || s2.isInvalid() || s3.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (exec::compareValue(s2, s3, mode) == T_POS) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            // min(s1, s3)
            TernaryValue min_val = (exec::compareValue(s1, s3, mode) == T_POS) ? s3 : s1;
            // max(s2, min_val)
            TernaryValue clamped = (exec::compareValue(s2, min_val, mode) == T_POS) ? s2 : min_val;
            writeChecked(iw.rd, clamped);
            break;
        }

        case Opcode::CALLR: {
            TernaryValue target = vm.regfile.read(iw.rs1);
            if (!isNumericMode(target.mode) || target.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            int dest = exec::pcFromValue(target);
            if (!vm.validateControlTarget(dest)) {
                noteBranch(dest, false, false);
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            vm.regfile.writeLR(sandbox::vm::ops::fromLong(vm.pc + 1));
            pc_next = dest;
            noteBranch(pc_next, true, false);
            break;
        }

        case Opcode::JMPR: {
            TernaryValue target = vm.regfile.read(iw.rs1);
            if (!isNumericMode(target.mode) || target.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            int dest = exec::pcFromValue(target);
            if (!vm.validateControlTarget(dest)) {
                noteBranch(dest, false, false);
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            pc_next = dest;
            noteBranch(pc_next, true, false);
            break;
        }

        case Opcode::TMOD: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue b = convertValue(vm.regfile.read(iw.rs2), mode);
            if (a.isInvalid() || b.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (b.isZero()) {
                vm.trapWithCause(TrapCode::TRAP_DIV_ZERO, OS_CAUSE_DIV_ZERO, vm.pc);
                return vm.status;
            }
            TernaryValue quot = exec::divideValue(a, b, mode);
            if (quot.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue truncQuot = convertValue(sandbox::vm::ops::fromLong(sandbox::vm::ops::toLong(quot)), mode);
            if (truncQuot.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue prod = exec::multiplyValue(truncQuot, b, mode);
            TernaryValue rem  = exec::subtractValue(a, prod, mode);
            if (!writeChecked(iw.rd, rem)) return vm.status;
            break;
        }

        case Opcode::TLSHIFT:
        case Opcode::TRSHIFT: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue b = vm.regfile.read(iw.rs2);
            if (a.isInvalid() || !isNumericMode(b.mode) || b.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            long long k = ops::toLong(b);
            bool is_left = (iw.opcode == Opcode::TLSHIFT);
            if (k < 0) {
                is_left = !is_left;
                k = -k;
            }
            if (k > 60) k = 60;
            TernaryValue three = convertValue(sandbox::vm::ops::fromLong(3), mode);
            TernaryValue res = a;
            for (long long i = 0; i < k; ++i) {
                if (is_left) {
                    res = exec::multiplyValue(res, three, mode);
                } else {
                    res = exec::divideValue(res, three, mode);
                }
            }
            if (!writeChecked(iw.rd, res)) return vm.status;
            break;
        }

        case Opcode::TMAC: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue src1Native;
            TernaryValue src2Native;
            if (!exec::accumulatorSource(vm, iw.rs1, mode, src1Native) ||
                !exec::accumulatorSource(vm, iw.rs2, mode, src2Native)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            TernaryValue prod = exec::multiplyValue(src1Native, src2Native, TernaryMode::T40);
            vm.accumulator = exec::addValue(vm.accumulator, prod, TernaryMode::T40);
            if (vm.accumulator.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            break;
        }

        case Opcode::TCOUNT: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue val = convertValue(vm.regfile.read(iw.rs1), mode);
            if (val.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (val.isZero()) {
                if (!writeChecked(iw.rd, sandbox::vm::ops::fromLong(0))) return vm.status;
                break;
            }
            int num_trits = 0;
            switch (mode) {
                case TernaryMode::T1:  num_trits = 1;  break;
                case TernaryMode::T5:  num_trits = 5;  break;
                case TernaryMode::T10: num_trits = 10; break;
                case TernaryMode::T20: num_trits = 20; break;
                case TernaryMode::T40: num_trits = 40; break;
                case TernaryMode::T50: num_trits = 50; break;
                default: break;
            }
            long long count = 0;
            for (int i = 0; i < num_trits; ++i) {
                if (readStoredTrit(val, i) != 0) ++count;
            }
            if (!writeChecked(iw.rd, sandbox::vm::ops::fromLong(count))) return vm.status;
            break;
        }

        case Opcode::TSCAN: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue val = convertValue(vm.regfile.read(iw.rs1), mode);
            if (val.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            if (val.isZero()) {
                if (!writeChecked(iw.rd, makeTritResult(T_NEG))) return vm.status;
                break;
            }
            int num_trits = 0;
            switch (mode) {
                case TernaryMode::T1:  num_trits = 1;  break;
                case TernaryMode::T5:  num_trits = 5;  break;
                case TernaryMode::T10: num_trits = 10; break;
                case TernaryMode::T20: num_trits = 20; break;
                case TernaryMode::T40: num_trits = 40; break;
                case TernaryMode::T50: num_trits = 50; break;
                default: break;
            }
            long long index = -1;
            for (int i = 0; i < num_trits; ++i) {
                if (readStoredTrit(val, i) != 0) {
                    index = i;
                    break;
                }
            }
            TernaryValue scanResult = index < 0
                ? makeTritResult(T_NEG)
                : sandbox::vm::ops::fromLong(index);
            if (!writeChecked(iw.rd, scanResult)) return vm.status;
            break;
        }

        case Opcode::SYSCALL: {
            if (vm.trap_routing_enabled && vm.privilege == PrivilegeMode::User) {
                vm.syscall_id = iw.imm;
                vm.trapWithCause(TrapCode::TRAP_ILLEGAL_OP, OS_CAUSE_SYSCALL, vm.pc);
                return vm.status;
            }
            if (iw.imm == 1) {
                long long val = sandbox::vm::ops::toLong(vm.regfile.read(1));
                vm.syscall_buffer += std::to_string(val);
            } else if (iw.imm == 2) {
                vm.syscall_buffer += "\n";
            } else if (iw.imm == 3) {
                vm.syscall_buffer.clear();
            } else if (iw.imm == 19) {
                // Standalone sys_sbrk helper: return the start of the fresh
                // region, then advance this VM's private heap break.
                long long delta = sandbox::vm::ops::toLong(vm.regfile.read(13));
                long long old_break = vm.standalone_heap_break;
                long long new_break = old_break + delta;
                if (new_break < 0 || new_break > static_cast<long long>(std::numeric_limits<int>::max())) {
                    vm.trap(TrapCode::TRAP_MEM_FAULT);
                    return vm.status;
                }
                if (new_break > vm.dmem.size()) {
                    const long long growth_candidate = std::max<long long>(1, vm.dmem.size()) * 2;
                    const int grown_size = static_cast<int>(std::max(new_break, growth_candidate));
                    if (!vm.growDataMemoryPreservingStack(grown_size)) {
                        vm.trap(TrapCode::TRAP_MEM_FAULT);
                        return vm.status;
                    }
                }
                vm.standalone_heap_break = new_break;
                vm.regfile.write(13, sandbox::vm::ops::fromLong(old_break));
            } else if (iw.imm == 22) {
                // sys_write_char: interpret r1 as an ASCII character code
                long long charVal = sandbox::vm::ops::toLong(vm.regfile.read(1));
                vm.syscall_buffer += static_cast<char>(charVal);
            } else {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            break;
        }

        case Opcode::FENCE: {
            auto [okOrder, order] = decodeAtomicOrder();
            (void)order;
            if (!okOrder) return vm.status;
            // Single-core VM is sequentially consistent; FENCE is an architectural marker.
            break;
        }

        case Opcode::VSUM: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            TernaryValue sum = TernaryValue::zero(mode);
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                TernaryValue val;
                if (!exec::convertVectorNumericLane(vm.vregfile.reg[iw.rs1].read(lane), mode, val)) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
                sum = exec::addValue(sum, val, mode);
                if (sum.isInvalid()) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
            }
            if (!writeChecked(iw.rd, sum)) return vm.status;
            break;
        }

        case Opcode::VHMIN:
        case Opcode::VHMAX: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!exec::validVectorReg(iw.rs1)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            exec::prepareVectorOp(vm);
            if (vm.vector_length <= 0) {
                if (!writeChecked(iw.rd, TernaryValue::zero(mode))) return vm.status;
                break;
            }
            TernaryValue best;
            if (!exec::convertVectorNumericLane(vm.vregfile.reg[iw.rs1].read(0), mode, best)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            bool is_min = (iw.opcode == Opcode::VHMIN);
            for (int lane = 1; lane < vm.vector_length; ++lane) {
                TernaryValue val;
                if (!exec::convertVectorNumericLane(vm.vregfile.reg[iw.rs1].read(lane), mode, val)) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return vm.status;
                }
                int8_t cmp = exec::compareValue(best, val, mode);
                if (is_min) {
                    if (cmp == T_POS) best = val;
                } else {
                    if (cmp == T_NEG) best = val;
                }
            }
            if (!writeChecked(iw.rd, best)) return vm.status;
            break;
        }

        case Opcode::VLEN: {
            vm.regfile.write(iw.rd, ops::fromLong(vm.vector_length));
            break;
        }

        default:
            // Should never reach here — decode() maps unknown opcodes to
            // RESERVED, which is caught above. Belt-and-suspenders trap.
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return vm.status;
    }

    // -----------------------------------------------------------------
    // ADVANCE PC (only if still running — HALT/TRAP return early above)
    // -----------------------------------------------------------------
    vm.completeInstruction(pc_next);
    return vm.status;
}

// =============================================================================
// SECTION 3 — Run Loop
// =============================================================================

struct VMHooks {
    using Hook = std::function<void(const VMState&, int pc_before)>;
    using InstructionHook = std::function<void(const VMState&, const VMExecutionRecord&)>;

    InstructionHook onInstruction;
    Hook onStep;
    Hook onTrap;
    Hook onHalt;
};

struct BranchProfileKey {
    int pc = -1;
    int opcode = -1;
    int target = -1;

    [[nodiscard]] bool operator<(const BranchProfileKey& other) const {
        return std::tie(pc, opcode, target) <
               std::tie(other.pc, other.opcode, other.target);
    }
};

struct BranchProfileCounts {
    std::uint64_t taken = 0;
    std::uint64_t fallthrough = 0;

    [[nodiscard]] std::uint64_t total() const { return taken + fallthrough; }
};

struct TrapProfileKey {
    int syscall_id = -1;
    int trap_cause = 0;
    int process_id = -1;

    [[nodiscard]] bool operator<(const TrapProfileKey& other) const {
        return std::tie(syscall_id, trap_cause, process_id) <
               std::tie(other.syscall_id, other.trap_cause, other.process_id);
    }
};

class VMExecutionProfile {
public:
    static constexpr int kNoProcessOverride = std::numeric_limits<int>::min();

    std::uint64_t total_instructions = 0;
    std::map<int, std::uint64_t> opcode_counts;
    std::map<int, std::uint64_t> pc_counts;
    std::map<int, int> pc_opcodes;
    std::map<BranchProfileKey, BranchProfileCounts> branches;
    std::map<TrapProfileKey, std::uint64_t> syscalls;
    std::map<TrapProfileKey, std::uint64_t> traps;

    void record(const VMExecutionRecord& execution, int process_id_override = kNoProcessOverride) {
        if (!execution.attempted) return;
        const int process_id =
            process_id_override == kNoProcessOverride ? execution.process_id : process_id_override;
        ++total_instructions;
        ++pc_counts[execution.pc];
        if (execution.has_instruction) {
            const int opcode = opcodeId(execution.instruction.opcode);
            ++opcode_counts[opcode];
            pc_opcodes.emplace(execution.pc, opcode);
            if (execution.branch_observed) {
                BranchProfileCounts& counts =
                    branches[BranchProfileKey{execution.pc, opcode, execution.branch_target}];
                if (!execution.branch_conditional || execution.branch_taken) ++counts.taken;
                else ++counts.fallthrough;
            }
            if (execution.instruction.opcode == Opcode::SYSCALL) {
                const int cause = execution.trap_observed ? execution.trap_cause : 0;
                ++syscalls[TrapProfileKey{execution.syscall_id, cause, process_id}];
            }
        }
        if (execution.trap_observed) {
            ++traps[TrapProfileKey{execution.syscall_id, execution.trap_cause, process_id}];
        }
    }

    [[nodiscard]] std::uint64_t opcodeCount(Opcode opcode) const {
        auto it = opcode_counts.find(opcodeId(opcode));
        return it == opcode_counts.end() ? 0 : it->second;
    }

    [[nodiscard]] std::uint64_t pcCount(int pc) const {
        auto it = pc_counts.find(pc);
        return it == pc_counts.end() ? 0 : it->second;
    }

    [[nodiscard]] BranchProfileCounts branchCounts(int pc, Opcode opcode, int target) const {
        auto it = branches.find(BranchProfileKey{pc, opcodeId(opcode), target});
        return it == branches.end() ? BranchProfileCounts{} : it->second;
    }

    [[nodiscard]] std::uint64_t syscallCount(int syscall_id, int trap_cause, int process_id) const {
        auto it = syscalls.find(TrapProfileKey{syscall_id, trap_cause, process_id});
        return it == syscalls.end() ? 0 : it->second;
    }

    [[nodiscard]] std::uint64_t trapCount(int syscall_id, int trap_cause, int process_id) const {
        auto it = traps.find(TrapProfileKey{syscall_id, trap_cause, process_id});
        return it == traps.end() ? 0 : it->second;
    }

    [[nodiscard]] std::string toJson(int top_limit = 20) const {
        std::ostringstream out;
        writeJson(out, top_limit);
        return out.str();
    }

    [[nodiscard]] std::string toText(int top_limit = 20) const {
        std::ostringstream out;
        writeText(out, top_limit);
        return out.str();
    }

    void writeJson(std::ostream& out, int top_limit = 20) const {
        out << "{\n";
        out << "  \"format_version\": 1,\n";
        out << "  \"total_instructions\": " << total_instructions << ",\n";
        out << "  \"opcodes\": [\n";
        const auto opcode_rows = topOpcodeRows(static_cast<int>(opcode_counts.size()));
        for (std::size_t i = 0; i < opcode_rows.size(); ++i) {
            const auto& row = opcode_rows[i];
            out << "    {\"opcode\": " << row.opcode
                << ", \"name\": " << jsonString(opcodeName(row.opcode))
                << ", \"count\": " << row.count << "}";
            if (i + 1 < opcode_rows.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";
        out << "  \"top_pcs\": [\n";
        const auto pc_rows = topPcRows(top_limit);
        for (std::size_t i = 0; i < pc_rows.size(); ++i) {
            const auto& row = pc_rows[i];
            out << "    {\"pc\": " << row.pc
                << ", \"opcode\": " << row.opcode
                << ", \"name\": " << jsonString(opcodeName(row.opcode))
                << ", \"count\": " << row.count << "}";
            if (i + 1 < pc_rows.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";
        out << "  \"top_branches\": [\n";
        const auto branch_rows = topBranchRows(top_limit);
        for (std::size_t i = 0; i < branch_rows.size(); ++i) {
            const auto& row = branch_rows[i];
            out << "    {\"pc\": " << row.key.pc
                << ", \"opcode\": " << row.key.opcode
                << ", \"name\": " << jsonString(opcodeName(row.key.opcode))
                << ", \"target\": " << row.key.target
                << ", \"taken\": " << row.counts.taken
                << ", \"fallthrough\": " << row.counts.fallthrough
                << ", \"total\": " << row.counts.total() << "}";
            if (i + 1 < branch_rows.size()) out << ",";
            out << "\n";
        }
        out << "  ],\n";
        out << "  \"top_syscalls\": [\n";
        const auto syscall_rows = topTrapRows(syscalls, top_limit);
        writeTrapRowsJson(out, syscall_rows);
        out << "  ],\n";
        out << "  \"top_traps\": [\n";
        const auto trap_rows = topTrapRows(traps, top_limit);
        writeTrapRowsJson(out, trap_rows);
        out << "  ]\n";
        out << "}\n";
    }

    void writeText(std::ostream& out, int top_limit = 20) const {
        out << "Trit VM execution profile\n";
        out << "total_instructions: " << total_instructions << "\n";
        out << "\nhot opcodes:\n";
        for (const auto& row : topOpcodeRows(top_limit)) {
            out << "  " << std::setw(10) << row.count << " "
                << std::setw(3) << row.opcode << " " << opcodeName(row.opcode) << "\n";
        }
        out << "\nhot PCs:\n";
        for (const auto& row : topPcRows(top_limit)) {
            out << "  " << std::setw(10) << row.count << " pc="
                << row.pc << " " << opcodeName(row.opcode) << "\n";
        }
        out << "\nhot branches:\n";
        for (const auto& row : topBranchRows(top_limit)) {
            out << "  " << std::setw(10) << row.counts.total()
                << " pc=" << row.key.pc
                << " op=" << opcodeName(row.key.opcode)
                << " target=" << row.key.target
                << " taken=" << row.counts.taken
                << " fallthrough=" << row.counts.fallthrough << "\n";
        }
        out << "\nhot syscalls:\n";
        for (const auto& row : topTrapRows(syscalls, top_limit)) {
            out << "  " << std::setw(10) << row.count
                << " syscall=" << row.key.syscall_id
                << " cause=" << row.key.trap_cause
                << " pid=" << row.key.process_id << "\n";
        }
        out << "\nhot traps:\n";
        for (const auto& row : topTrapRows(traps, top_limit)) {
            out << "  " << std::setw(10) << row.count
                << " syscall=" << row.key.syscall_id
                << " cause=" << row.key.trap_cause
                << " pid=" << row.key.process_id << "\n";
        }
    }

private:
    struct CountRow {
        int key = -1;
        std::uint64_t count = 0;
    };

    struct OpcodeRow {
        int opcode = -1;
        std::uint64_t count = 0;
    };

    struct PcRow {
        int pc = -1;
        int opcode = -1;
        std::uint64_t count = 0;
    };

    struct BranchRow {
        BranchProfileKey key;
        BranchProfileCounts counts;
    };

    struct TrapRow {
        TrapProfileKey key;
        std::uint64_t count = 0;
    };

    [[nodiscard]] static int opcodeId(Opcode opcode) {
        return static_cast<int>(static_cast<std::uint8_t>(opcode));
    }

    [[nodiscard]] static std::string opcodeName(int opcode) {
        if (opcode < 0 || opcode > 80) return "UNKNOWN";
        return opcodeToString(static_cast<Opcode>(opcode));
    }

    [[nodiscard]] static std::string jsonString(const std::string& value) {
        std::ostringstream out;
        out << '"';
        for (unsigned char ch : value) {
            switch (ch) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (ch < 0x20) {
                        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(ch) << std::dec << std::setfill(' ');
                    } else {
                        out << static_cast<char>(ch);
                    }
                    break;
            }
        }
        out << '"';
        return out.str();
    }

    [[nodiscard]] static bool countDescThenKey(const CountRow& lhs, const CountRow& rhs) {
        if (lhs.count != rhs.count) return lhs.count > rhs.count;
        return lhs.key < rhs.key;
    }

    [[nodiscard]] std::vector<OpcodeRow> topOpcodeRows(int limit) const {
        std::vector<CountRow> rows;
        for (const auto& [opcode, count] : opcode_counts) rows.push_back(CountRow{opcode, count});
        std::sort(rows.begin(), rows.end(), countDescThenKey);
        if (limit >= 0 && static_cast<int>(rows.size()) > limit) {
            rows.resize(static_cast<std::size_t>(limit));
        }
        std::vector<OpcodeRow> out;
        out.reserve(rows.size());
        for (const CountRow& row : rows) out.push_back(OpcodeRow{row.key, row.count});
        return out;
    }

    [[nodiscard]] std::vector<PcRow> topPcRows(int limit) const {
        std::vector<CountRow> rows;
        for (const auto& [pc, count] : pc_counts) rows.push_back(CountRow{pc, count});
        std::sort(rows.begin(), rows.end(), countDescThenKey);
        if (limit >= 0 && static_cast<int>(rows.size()) > limit) {
            rows.resize(static_cast<std::size_t>(limit));
        }
        std::vector<PcRow> out;
        out.reserve(rows.size());
        for (const CountRow& row : rows) {
            auto opcode = pc_opcodes.find(row.key);
            out.push_back(PcRow{row.key, opcode == pc_opcodes.end() ? -1 : opcode->second, row.count});
        }
        return out;
    }

    [[nodiscard]] std::vector<BranchRow> topBranchRows(int limit) const {
        std::vector<BranchRow> rows;
        for (const auto& [key, counts] : branches) rows.push_back(BranchRow{key, counts});
        std::sort(rows.begin(), rows.end(), [](const BranchRow& lhs, const BranchRow& rhs) {
            if (lhs.counts.total() != rhs.counts.total()) return lhs.counts.total() > rhs.counts.total();
            return lhs.key < rhs.key;
        });
        if (limit >= 0 && static_cast<int>(rows.size()) > limit) {
            rows.resize(static_cast<std::size_t>(limit));
        }
        return rows;
    }

    [[nodiscard]] static std::vector<TrapRow> topTrapRows(
        const std::map<TrapProfileKey, std::uint64_t>& source,
        int limit) {
        std::vector<TrapRow> rows;
        for (const auto& [key, count] : source) rows.push_back(TrapRow{key, count});
        std::sort(rows.begin(), rows.end(), [](const TrapRow& lhs, const TrapRow& rhs) {
            if (lhs.count != rhs.count) return lhs.count > rhs.count;
            return lhs.key < rhs.key;
        });
        if (limit >= 0 && static_cast<int>(rows.size()) > limit) {
            rows.resize(static_cast<std::size_t>(limit));
        }
        return rows;
    }

    static void writeTrapRowsJson(std::ostream& out, const std::vector<TrapRow>& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            out << "    {\"syscall_id\": " << row.key.syscall_id
                << ", \"trap_cause\": " << row.key.trap_cause
                << ", \"process_id\": " << row.key.process_id
                << ", \"count\": " << row.count << "}";
            if (i + 1 < rows.size()) out << ",";
            out << "\n";
        }
    }
};

struct PerformanceCounters {
    long long steps = 0;
    long long syscalls = 0;
    long long branches = 0;
    long long traps = 0;
    long long halts = 0;
};

[[nodiscard]] inline VMHooks makeProfilerHooks(PerformanceCounters& counters) {
    VMHooks hooks;
    hooks.onInstruction = [&counters](const VMState&, const VMExecutionRecord& execution) {
        ++counters.steps;
        if (execution.has_instruction && execution.instruction.opcode == Opcode::SYSCALL) {
            ++counters.syscalls;
        }
        if (execution.branch_observed) {
            ++counters.branches;
        }
        if (execution.trap_observed) {
            ++counters.traps;
        }
        if (execution.after_status == VMStatus::HALTED) {
            ++counters.halts;
        }
    };
    return hooks;
}

[[nodiscard]] inline VMHooks makeProfilerHooks(
    VMExecutionProfile& profile,
    std::function<int(const VMState&)> process_id_provider = {}) {
    VMHooks hooks;
    hooks.onInstruction =
        [&profile, process_id_provider](const VMState& vm, const VMExecutionRecord& execution) {
            const int process_id =
                process_id_provider ? process_id_provider(vm) : execution.process_id;
            profile.record(execution, process_id);
        };
    return hooks;
}

// Execute one instruction and notify hooks after the architectural state for
// that instruction has been committed. Terminal hooks fire after onStep.
inline VMStatus step(VMState& vm, const VMHooks& hooks) {
    if (!vm.isRunning()) return vm.status;

    const int pc_before = vm.pc;
    const VMStatus before = vm.status;
    VMExecutionRecord execution;
    const VMStatus after = step(vm, &execution);

    if (hooks.onInstruction) hooks.onInstruction(vm, execution);
    if (hooks.onStep) hooks.onStep(vm, pc_before);
    if (before == VMStatus::RUNNING && after == VMStatus::TRAPPED && hooks.onTrap) {
        hooks.onTrap(vm, pc_before);
    }
    if (before == VMStatus::RUNNING && after == VMStatus::HALTED && hooks.onHalt) {
        hooks.onHalt(vm, pc_before);
    }

    return after;
}

static constexpr int VM_BLOCK_CACHE_MAX_LENGTH = 64;

inline void syncBlockCacheGeneration(VMState& vm) {
    const std::uint64_t generation = vm.imem.generation();
    if (vm.block_cache_observed_generation == generation) return;
    const bool had_entries =
        !vm.decoded_instruction_cache.empty() || !vm.basic_block_cache.empty();
    vm.decoded_instruction_cache.clear();
    vm.basic_block_cache.clear();
    vm.block_cache_observed_generation = generation;
    if (had_entries) ++vm.block_cache_stats.invalidations;
}

[[nodiscard]] inline bool blockCacheFastPathAvailable(const VMState& vm) {
    return vm.block_cache_enabled &&
           vm.isRunning() &&
           vm.privilege == PrivilegeMode::Kernel;
}

[[nodiscard]] inline bool isCachedBlockBoundary(const InstructionWord& iw) {
    if (iw.malformed || iw.opcode == Opcode::RESERVED) return true;
    switch (iw.opcode) {
        case Opcode::HALT:
        case Opcode::JMP:
        case Opcode::BRN:
        case Opcode::BRZ:
        case Opcode::WAIT:
        case Opcode::TLBINV:
        case Opcode::BRP:
        case Opcode::CALL:
        case Opcode::RET:
        case Opcode::CALLR:
        case Opcode::JMPR:
        case Opcode::SYSCALL:
        case Opcode::CSRR:
        case Opcode::CSRW:
        case Opcode::CSRRW:
        case Opcode::ERET:
        case Opcode::FENCE:
        case Opcode::TLDR:
        case Opcode::TSTR:
            return true;
        default:
            return false;
    }
}

inline void classifyCachedInstruction(VMDecodedInstruction& decoded) {
    InstructionWord& iw = decoded.word;
    decoded.block_boundary = isCachedBlockBoundary(iw);
    if (decoded.block_boundary) return;

    switch (iw.opcode) {
        case Opcode::NOP:
            decoded.op = VMDecodedOp::Nop;
            decoded.supported = true;
            return;
        case Opcode::MOV:
            decoded.op = VMDecodedOp::Mov;
            decoded.supported = true;
            return;
        case Opcode::MOVH:
            decoded.op = VMDecodedOp::MovH;
            decoded.supported = true;
            return;
        case Opcode::COPY:
            if (!exec::numericModeFromFunc(iw.func, decoded.mode)) return;
            decoded.op = VMDecodedOp::Copy;
            decoded.supported = true;
            return;
        case Opcode::SWAP:
            decoded.op = VMDecodedOp::Swap;
            decoded.supported = true;
            return;
        case Opcode::ADD:
        case Opcode::SUB:
        case Opcode::MUL:
        case Opcode::DIV:
        case Opcode::SQRT:
        case Opcode::NEG:
        case Opcode::ABS:
        case Opcode::TCMP:
        case Opcode::TMIN:
        case Opcode::TMAX:
        case Opcode::TINV:
            if (!exec::numericModeFromFunc(iw.func, decoded.mode)) return;
            decoded.supported = true;
            switch (iw.opcode) {
                case Opcode::ADD: decoded.op = VMDecodedOp::Add; return;
                case Opcode::SUB: decoded.op = VMDecodedOp::Sub; return;
                case Opcode::MUL: decoded.op = VMDecodedOp::Mul; return;
                case Opcode::DIV: decoded.op = VMDecodedOp::Div; return;
                case Opcode::SQRT: decoded.op = VMDecodedOp::Sqrt; return;
                case Opcode::NEG: decoded.op = VMDecodedOp::Neg; return;
                case Opcode::ABS: decoded.op = VMDecodedOp::Abs; return;
                case Opcode::TCMP: decoded.op = VMDecodedOp::TCmp; return;
                case Opcode::TMIN: decoded.op = VMDecodedOp::TMin; return;
                case Opcode::TMAX: decoded.op = VMDecodedOp::TMax; return;
                case Opcode::TINV: decoded.op = VMDecodedOp::TInv; return;
                default: return;
            }
        case Opcode::TLADD:
        case Opcode::TLSUB:
        case Opcode::TLAND:
        case Opcode::TLOR:
        case Opcode::TLNEG:
            if (!exec::laneModeFromFunc(iw.func, decoded.mode)) return;
            decoded.supported = true;
            switch (iw.opcode) {
                case Opcode::TLADD: decoded.op = VMDecodedOp::TLAdd; return;
                case Opcode::TLSUB: decoded.op = VMDecodedOp::TLSub; return;
                case Opcode::TLAND: decoded.op = VMDecodedOp::TLAnd; return;
                case Opcode::TLOR: decoded.op = VMDecodedOp::TLOr; return;
                case Opcode::TLNEG: decoded.op = VMDecodedOp::TLNeg; return;
                default: return;
            }
        case Opcode::TSEL:
            decoded.op = VMDecodedOp::TSel;
            decoded.supported = true;
            return;
        case Opcode::CVT:
            if (!exec::modeFromFunc(iw.func, decoded.mode)) return;
            decoded.op = VMDecodedOp::Cvt;
            decoded.supported = true;
            return;
        case Opcode::LOAD:
            decoded.op = VMDecodedOp::Load;
            decoded.supported = true;
            return;
        case Opcode::STORE:
            decoded.op = VMDecodedOp::Store;
            decoded.supported = true;
            return;
        default:
            return;
    }
}

[[nodiscard]] inline bool decodeInstructionForCache(
    VMState& vm, int pc, VMDecodedInstruction& out) {

    const std::uint64_t generation = vm.imem.generation();
    auto cached = vm.decoded_instruction_cache.find(pc);
    if (cached != vm.decoded_instruction_cache.end() &&
        cached->second.imem_generation == generation) {
        out = cached->second.decoded;
        return true;
    }

    int physical_pc = pc;
    int routed_cause = OS_CAUSE_FETCH_FAULT;
    if (!vm.translateFetchAddress(pc, physical_pc, routed_cause)) {
        return false;
    }

    auto [raw, fetch_fc] = vm.imem.fetch(physical_pc);
    if (fetch_fc != MemFaultCode::OK) {
        return false;
    }

    ++vm.decode_instructions_count;
    ++vm.block_cache_stats.decoded_instructions;

    VMDecodedInstruction decoded;
    decoded.pc = pc;
    decoded.raw = raw;
    decoded.word =
        VersionedInstructionCodec::decode(
            raw, IsaEncodingVersion::V2);
    classifyCachedInstruction(decoded);

    vm.decoded_instruction_cache[pc] = VMDecodedCacheEntry{generation, decoded};
    out = decoded;
    return true;
}

[[nodiscard]] inline VMBasicBlock* findOrBuildCachedBlock(VMState& vm, int start_pc) {
    syncBlockCacheGeneration(vm);
    const std::uint64_t generation = vm.imem.generation();

    auto cached = vm.basic_block_cache.find(start_pc);
    if (cached != vm.basic_block_cache.end() &&
        cached->second.imem_generation == generation) {
        ++vm.block_cache_stats.hits;
        return &cached->second;
    }

    ++vm.block_cache_stats.misses;
    VMBasicBlock block;
    block.start_pc = start_pc;
    block.imem_generation = generation;

    int pc = start_pc;
    for (int i = 0; i < VM_BLOCK_CACHE_MAX_LENGTH; ++i, ++pc) {
        VMDecodedInstruction decoded;
        if (!decodeInstructionForCache(vm, pc, decoded)) break;
        if (decoded.block_boundary || !decoded.supported) break;
        block.instructions.push_back(decoded);
    }

    if (!block.instructions.empty()) {
        ++vm.block_cache_stats.blocks_built;
        vm.block_cache_stats.total_block_length +=
            static_cast<long long>(block.instructions.size());
    }

    auto inserted = vm.basic_block_cache.emplace(start_pc, std::move(block));
    return &inserted.first->second;
}

[[nodiscard]] inline bool cachedWriteChecked(
    VMState& vm, uint8_t rd, TernaryValue value) {

    if (value.isInvalid()) {
        vm.trap(TrapCode::TRAP_ILLEGAL_OP);
        return false;
    }
    vm.regfile.write(rd, value);
    return true;
}

inline void executeCachedInstruction(VMState& vm, const VMDecodedInstruction& decoded) {
    const InstructionWord& iw = decoded.word;
    const int pc_next = decoded.pc + 1;
    vm.pc = decoded.pc;

    switch (decoded.op) {
        case VMDecodedOp::Nop:
            break;

        case VMDecodedOp::Mov:
            vm.regfile.write(iw.rd, ops::fromLong(iw.imm));
            break;

        case VMDecodedOp::MovH: {
            TernaryValue current = vm.regfile.read(iw.rd);
            LongTriple currentLT = current.toLongTriple();
            auto trits = currentLT.unpack();
            int immVal = iw.imm;
            for (int i = 16; i < 32; ++i) {
                int r = (immVal + 1) % 3;
                if (r < 0) r += 3;
                int8_t trit = static_cast<int8_t>(r - 1);
                trits[i] = trit;
                immVal = (immVal - trit) / 3;
            }
            for (int i = 32; i < 50; ++i) trits[i] = 0;
            vm.regfile.write(iw.rd, TernaryValue::fromLongTriple(LongTriple::pack(trits)));
            break;
        }

        case VMDecodedOp::Copy:
            vm.regfile.write(iw.rd, vm.regfile.readView(iw.rs1, decoded.mode));
            break;

        case VMDecodedOp::Swap: {
            TernaryValue a = vm.regfile.read(iw.rd);
            TernaryValue b = vm.regfile.read(iw.rs1);
            vm.regfile.write(iw.rd, b);
            vm.regfile.write(iw.rs1, a);
            break;
        }

        case VMDecodedOp::Add:
            if (!cachedWriteChecked(vm, iw.rd, exec::addValue(
                    vm.regfile.readView(iw.rs1, decoded.mode),
                    vm.regfile.readView(iw.rs2, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::Sub:
            if (!cachedWriteChecked(vm, iw.rd, exec::subtractValue(
                    vm.regfile.readView(iw.rs1, decoded.mode),
                    vm.regfile.readView(iw.rs2, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::Mul:
            if (!cachedWriteChecked(vm, iw.rd, exec::multiplyValue(
                    vm.regfile.readView(iw.rs1, decoded.mode),
                    vm.regfile.readView(iw.rs2, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::Div: {
            TernaryValue b = vm.regfile.readView(iw.rs2, decoded.mode);
            if (b.isZero()) {
                vm.trapWithCause(TrapCode::TRAP_DIV_ZERO, OS_CAUSE_DIV_ZERO, vm.pc);
                return;
            }
            if (!cachedWriteChecked(vm, iw.rd, exec::divideValue(
                    vm.regfile.readView(iw.rs1, decoded.mode), b, decoded.mode))) return;
            break;
        }

        case VMDecodedOp::Sqrt: {
            TernaryValue t = vm.regfile.readView(iw.rs1, decoded.mode);
            if (exec::signValue(t, decoded.mode) == T_NEG) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return;
            }
            if (!cachedWriteChecked(vm, iw.rd, exec::sqrtValue(t, decoded.mode))) return;
            break;
        }

        case VMDecodedOp::Neg:
            if (!cachedWriteChecked(vm, iw.rd, exec::negateValue(
                    vm.regfile.readView(iw.rs1, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::Abs:
            if (!cachedWriteChecked(vm, iw.rd, exec::absValue(
                    vm.regfile.readView(iw.rs1, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::TCmp: {
            int8_t cmp = exec::compareValue(
                vm.regfile.readView(iw.rs1, decoded.mode),
                vm.regfile.readView(iw.rs2, decoded.mode), decoded.mode);
            vm.regfile.write(iw.rd, makeTritResult(cmp));
            break;
        }

        case VMDecodedOp::TMin: {
            TernaryValue a = vm.regfile.readView(iw.rs1, decoded.mode);
            TernaryValue b = vm.regfile.readView(iw.rs2, decoded.mode);
            if (!cachedWriteChecked(
                    vm, iw.rd, exec::compareValue(a, b, decoded.mode) == T_POS ? b : a)) return;
            break;
        }

        case VMDecodedOp::TMax: {
            TernaryValue a = vm.regfile.readView(iw.rs1, decoded.mode);
            TernaryValue b = vm.regfile.readView(iw.rs2, decoded.mode);
            if (!cachedWriteChecked(
                    vm, iw.rd, exec::compareValue(a, b, decoded.mode) == T_NEG ? b : a)) return;
            break;
        }

        case VMDecodedOp::TInv:
            if (!cachedWriteChecked(vm, iw.rd, exec::negateValue(
                    vm.regfile.readView(iw.rs1, decoded.mode), decoded.mode))) return;
            break;

        case VMDecodedOp::TLAdd:
        case VMDecodedOp::TLSub:
        case VMDecodedOp::TLAnd:
        case VMDecodedOp::TLOr: {
            exec::LaneOp op = exec::LaneOp::Add;
            if (decoded.op == VMDecodedOp::TLSub) op = exec::LaneOp::Sub;
            else if (decoded.op == VMDecodedOp::TLAnd) op = exec::LaneOp::And;
            else if (decoded.op == VMDecodedOp::TLOr) op = exec::LaneOp::Or;
            if (!cachedWriteChecked(vm, iw.rd, exec::laneBinaryValue(
                    vm.regfile.readView(iw.rs1, decoded.mode),
                    vm.regfile.readView(iw.rs2, decoded.mode),
                    decoded.mode, op))) return;
            break;
        }

        case VMDecodedOp::TLNeg:
            if (!cachedWriteChecked(vm, iw.rd, exec::laneUnaryValue(
                    vm.regfile.readView(iw.rs1, decoded.mode),
                    decoded.mode, exec::LaneOp::Neg))) return;
            break;

        case VMDecodedOp::TSel: {
            const int8_t cond = readTrit0(vm.regfile.read(iw.rcond));
            const uint8_t src = cond < 0 ? iw.rneg : (cond > 0 ? iw.rpos : iw.rzero);
            vm.regfile.write(iw.rd, vm.regfile.read(src));
            break;
        }

        case VMDecodedOp::Cvt: {
            TernaryValue source = vm.regfile.read(iw.rs1);
            if (isWidthFunc(iw.rs2)) {
                TernaryMode sourceMode = TernaryMode::T40;
                if (!exec::modeFromFunc(iw.rs2, sourceMode)) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return;
                }
                if ((isNumericMode(sourceMode) && !isNumericMode(source.mode)) ||
                    (isLaneMode(sourceMode) && !isLaneMode(source.mode))) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return;
                }
                source = convertValue(source, sourceMode);
                if (source.isInvalid()) {
                    vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                    return;
                }
            } else if (iw.rs2 != R0_ZERO || isLaneMode(decoded.mode)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return;
            }
            if (!cachedWriteChecked(vm, iw.rd, convertValue(source, decoded.mode))) return;
            break;
        }

        case VMDecodedOp::Load: {
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(vm.regfile.read(iw.rs1), iw.imm, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_LOAD_FAULT, vm.pc);
                return;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_LOAD_FAULT;
            if (!vm.translateLoadAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return;
            }
            auto [val, fc] = vm.dmem.load(physical_addr);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_LOAD_FAULT, vm.pc);
                return;
            }
            vm.regfile.write(iw.rd, val);
            break;
        }

        case VMDecodedOp::Store: {
            int addr = 0;
            const ScalarMemoryAddressStatus address_status =
                checkedScalarMemoryAddress(vm.regfile.read(iw.rs1), iw.imm, addr);
            if (address_status == ScalarMemoryAddressStatus::InvalidOperand) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return;
            }
            if (address_status == ScalarMemoryAddressStatus::OutOfRange) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                 OS_CAUSE_STORE_FAULT, vm.pc);
                return;
            }
            int physical_addr = addr;
            int cause = OS_CAUSE_STORE_FAULT;
            if (!vm.translateStoreAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return;
            }
            const TernaryValue store_value =
                vm.regfile.readPhysical(iw.rs_store);
            MemFaultCode fc = vm.dmem.store(physical_addr, store_value);
            if (fc != MemFaultCode::OK) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, OS_CAUSE_STORE_FAULT, vm.pc);
                return;
            }
            vm.noteStoreForReservation(physical_addr);
            break;
        }

        case VMDecodedOp::Unsupported:
        default:
            vm.trap(TrapCode::TRAP_ILLEGAL_OP);
            return;
    }

    if (vm.isRunning()) {
        vm.completeInstruction(pc_next);
    }
}

inline int executeCachedBlock(VMState& vm, int max_instructions) {
    if (!blockCacheFastPathAvailable(vm) || max_instructions == 0) return 0;

    VMBasicBlock* block = findOrBuildCachedBlock(vm, vm.pc);
    if (block == nullptr || block->instructions.empty()) return 0;

    int executed = 0;
    const std::uint64_t generation = block->imem_generation;
    for (const VMDecodedInstruction& decoded : block->instructions) {
        if (max_instructions >= 0 && executed >= max_instructions) break;
        if (!vm.isRunning()) break;
        if (vm.imem.generation() != generation) {
            syncBlockCacheGeneration(vm);
            break;
        }
        if (vm.pc != decoded.pc) break;

        const int expected_next = decoded.pc + 1;
        executeCachedInstruction(vm, decoded);
        ++executed;
        ++vm.block_cache_stats.instructions_executed;

        if (!vm.isRunning() || vm.power_control != 0) break;
        if (vm.pc != expected_next) break;
    }

    return executed;
}

static constexpr int VM_TRACE_JIT_MAX_LENGTH = 64;

[[nodiscard]] inline VMDecodedTraceCacheKey decodedTraceCacheKey(
    const VMState& vm,
    int pc) {
    return {
        vm.required_features,
        vm.asid,
        pc,
        static_cast<int>(vm.privilege),
        vm.mmu_enable,
        vm.user_imem_base,
        vm.user_imem_limit,
        vm.user_dmem_base,
        vm.user_dmem_limit,
        vm.user_imem_ptbr,
        vm.user_imem_pages,
        vm.user_dmem_ptbr,
        vm.user_dmem_pages,
        vm.imem.generation(),
        vm.executable_mapping_generation,
        vm.mmu_generation,
    };
}

inline void syncTraceJitGeneration(VMState& vm) {
    const std::uint64_t generation = vm.imem.generation();
    if (vm.trace_jit_observed_generation == generation) return;
    const bool had_trace_entries =
        !vm.hot_pc_counts.empty() || !vm.trace_jit_cache.empty() ||
        !vm.trace_jit_unsupported_pcs.empty();
    const bool had_native_entries = !vm.native_x64_code_cache.empty();
    vm.hot_pc_counts.clear();
    vm.trace_jit_cache.clear();
    vm.trace_jit_unsupported_pcs.clear();
    vm.native_x64_code_cache.clear();
    vm.trace_jit_observed_generation = generation;
    if (had_trace_entries) ++vm.trace_jit_stats.invalidations;
    if (had_native_entries) ++vm.native_x64_jit_stats.invalidations;
}

[[nodiscard]] inline bool traceJitFastPathAvailable(const VMState& vm) {
    return (vm.decodedTraceExecutorEnabled() ||
            vm.nativeX64JitEnabled()) &&
           vm.isRunning();
}

inline bool classifyTraceJitInstruction(VMTraceJitInstruction& emitted) {
    InstructionWord& iw = emitted.word;
    if (iw.malformed || iw.opcode == Opcode::RESERVED) return false;

    switch (iw.opcode) {
        case Opcode::NOP:
            emitted.op = VMTraceJitOp::Nop;
            return true;
        case Opcode::MOV:
            emitted.op = VMTraceJitOp::Mov;
            return true;
        case Opcode::MOVH:
            emitted.op = VMTraceJitOp::MovH;
            return true;
        case Opcode::COPY:
            if (!exec::numericModeFromFunc(iw.func, emitted.mode)) return false;
            emitted.op = VMTraceJitOp::Copy;
            return true;
        case Opcode::TCMP:
            if (!exec::numericModeFromFunc(iw.func, emitted.mode)) return false;
            emitted.op = VMTraceJitOp::TCmp;
            return true;
        case Opcode::ADD:
        case Opcode::SUB:
        case Opcode::MUL:
        case Opcode::NEG:
        case Opcode::ABS:
            if (!exec::numericModeFromFunc(iw.func, emitted.mode)) return false;
            switch (iw.opcode) {
                case Opcode::ADD: emitted.op = VMTraceJitOp::Add; return true;
                case Opcode::SUB: emitted.op = VMTraceJitOp::Sub; return true;
                case Opcode::MUL: emitted.op = VMTraceJitOp::Mul; return true;
                case Opcode::NEG: emitted.op = VMTraceJitOp::Neg; return true;
                case Opcode::ABS: emitted.op = VMTraceJitOp::Abs; return true;
                default: return false;
            }
        case Opcode::LOAD:
            emitted.op = VMTraceJitOp::Load;
            return true;
        case Opcode::STORE:
            emitted.op = VMTraceJitOp::Store;
            return true;
        case Opcode::JMP:
            emitted.op = VMTraceJitOp::Jmp;
            emitted.branch_target = emitted.pc + iw.offset;
            emitted.ends_trace = true;
            return true;
        case Opcode::BRN:
            emitted.op = VMTraceJitOp::Brn;
            emitted.branch_target = emitted.pc + iw.offset;
            emitted.ends_trace = true;
            return true;
        case Opcode::BRZ:
            emitted.op = VMTraceJitOp::Brz;
            emitted.branch_target = emitted.pc + iw.offset;
            emitted.ends_trace = true;
            return true;
        case Opcode::BRP:
            emitted.op = VMTraceJitOp::Brp;
            emitted.branch_target = emitted.pc + iw.offset;
            emitted.ends_trace = true;
            return true;
        case Opcode::CALL:
            emitted.op = VMTraceJitOp::Call;
            emitted.branch_target = emitted.pc + iw.offset;
            emitted.ends_trace = true;
            return true;
        case Opcode::RET:
            emitted.op = VMTraceJitOp::Ret;
            emitted.ends_trace = true;
            return true;
        case Opcode::CALLR:
            emitted.op = VMTraceJitOp::CallR;
            emitted.ends_trace = true;
            return true;
        case Opcode::JMPR:
            emitted.op = VMTraceJitOp::Jmpr;
            emitted.ends_trace = true;
            return true;
        default:
            return false;
    }
}

inline void annotateDecodedMicroOp(VMMicroOp& op) {
    op.memory_effect = VMMicroMemoryEffect::None;
    op.guards = VM_MICRO_GUARD_TRACE_GENERATION;
    op.side_exits.clear();
    op.trap_point = true;
    op.instruction_accounting = 1;
    switch (op.op) {
        case VMMicroOpcode::Copy:
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            break;
        case VMMicroOpcode::TCmp:
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC |
                         VM_MICRO_GUARD_RS2_NUMERIC;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            break;
        case VMMicroOpcode::Add:
        case VMMicroOpcode::Sub:
        case VMMicroOpcode::Mul:
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC |
                         VM_MICRO_GUARD_RS2_NUMERIC |
                         VM_MICRO_GUARD_RESULT_VALID;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            op.side_exits.push_back(VMMicroSideExit::InvalidResult);
            break;
        case VMMicroOpcode::Neg:
        case VMMicroOpcode::Abs:
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC |
                         VM_MICRO_GUARD_RESULT_VALID;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            op.side_exits.push_back(VMMicroSideExit::InvalidResult);
            break;
        case VMMicroOpcode::Load:
            op.memory_effect = VMMicroMemoryEffect::Read;
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC |
                         VM_MICRO_GUARD_ADDRESS_TRANSLATION |
                         VM_MICRO_GUARD_MEMORY_ACCESS;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            op.side_exits.push_back(VMMicroSideExit::AddressTranslation);
            op.side_exits.push_back(VMMicroSideExit::MemoryFault);
            break;
        case VMMicroOpcode::Store:
            op.memory_effect = VMMicroMemoryEffect::Write;
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC |
                         VM_MICRO_GUARD_ADDRESS_TRANSLATION |
                         VM_MICRO_GUARD_MEMORY_ACCESS;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            op.side_exits.push_back(VMMicroSideExit::AddressTranslation);
            op.side_exits.push_back(VMMicroSideExit::MemoryFault);
            break;
        case VMMicroOpcode::Brn:
        case VMMicroOpcode::Brz:
        case VMMicroOpcode::Brp:
            op.guards |= VM_MICRO_GUARD_RS1_NUMERIC;
            op.side_exits.push_back(VMMicroSideExit::InvalidOperand);
            op.side_exits.push_back(VMMicroSideExit::BranchLeavesTrace);
            break;
        case VMMicroOpcode::Jmp:
            op.side_exits.push_back(VMMicroSideExit::BranchLeavesTrace);
            break;
        case VMMicroOpcode::Call:
        case VMMicroOpcode::Ret:
        case VMMicroOpcode::CallR:
        case VMMicroOpcode::Jmpr:
            op.side_exits.push_back(VMMicroSideExit::BranchLeavesTrace);
            break;
        case VMMicroOpcode::Unsupported:
            op.side_exits.push_back(VMMicroSideExit::Unsupported);
            break;
        case VMMicroOpcode::Nop:
        case VMMicroOpcode::Mov:
        case VMMicroOpcode::MovH:
            break;
    }
    op.side_exits.push_back(VMMicroSideExit::GenerationMismatch);
}

[[nodiscard]] inline bool emitTraceJitInstruction(
    VMState& vm, int pc, VMTraceJitInstruction& out) {

    int physical_pc = pc;
    int routed_cause = OS_CAUSE_FETCH_FAULT;
    if (!vm.translateFetchAddress(pc, physical_pc, routed_cause)) return false;

    auto [raw, fetch_fc] = vm.imem.fetch(physical_pc);
    if (fetch_fc != MemFaultCode::OK) return false;

    VMTraceJitInstruction emitted;
    emitted.pc = pc;
    emitted.raw = raw;
    emitted.word =
        VersionedInstructionCodec::decode(
            raw, IsaEncodingVersion::V2);
    if (!classifyTraceJitInstruction(emitted)) return false;
    annotateDecodedMicroOp(emitted);

    out = emitted;
    return true;
}

[[nodiscard]] inline VMTraceJitTrace* buildTraceJitTrace(VMState& vm, int start_pc) {
    ++vm.trace_jit_stats.compilation_attempts;

    VMTraceJitTrace trace;
    trace.start_pc = start_pc;
    trace.imem_generation = vm.imem.generation();
    trace.cache_key = decodedTraceCacheKey(vm, start_pc);

    std::unordered_map<int, int> pc_to_index;
    int pc = start_pc;
    for (int i = 0; i < VM_TRACE_JIT_MAX_LENGTH; ++i) {
        if (pc_to_index.find(pc) != pc_to_index.end()) break;

        VMTraceJitInstruction emitted;
        if (!emitTraceJitInstruction(vm, pc, emitted)) break;

        const int index = static_cast<int>(trace.instructions.size());
        pc_to_index[pc] = index;
        trace.instructions.push_back(emitted);

        if (emitted.ends_trace) {
            auto target = pc_to_index.find(emitted.branch_target);
            if (target != pc_to_index.end()) {
                trace.instructions.back().branch_target_index = target->second;
            }
            break;
        }

        pc = pc + 1;
    }

    if (trace.instructions.empty()) {
        vm.trace_jit_unsupported_pcs.insert(trace.cache_key);
        ++vm.trace_jit_stats.unsupported_fallbacks;
        return nullptr;
    }

    ++vm.trace_jit_stats.traces_built;
    const VMDecodedTraceCacheKey key = trace.cache_key;
    auto inserted = vm.trace_jit_cache.emplace(key, std::move(trace));
    return &inserted.first->second;
}

[[nodiscard]] inline bool traceJitWriteChecked(
    VMState& vm,
    const VMTraceJitInstruction& emitted,
    uint8_t rd,
    TernaryValue value) {

    if (value.isInvalid()) {
        vm.pc = emitted.pc;
        ++vm.trace_jit_stats.interpreter_bailouts;
        return false;
    }
    vm.regfile.write(rd, value);
    return true;
}

[[nodiscard]] inline bool traceJitVirtualAddress(
    VMState& vm,
    const VMTraceJitInstruction& emitted,
    int& out_addr) {

    const InstructionWord& iw = emitted.word;
    const TernaryValue base_value = vm.regfile.read(iw.rs1);
    if (!isNumericMode(base_value.mode) || base_value.isInvalid()) {
        vm.pc = emitted.pc;
        ++vm.trace_jit_stats.interpreter_bailouts;
        return false;
    }

    const long long addr_long = ops::toLong(base_value) + iw.imm;
    if (addr_long < std::numeric_limits<int>::min() ||
        addr_long > std::numeric_limits<int>::max()) {
        vm.pc = emitted.pc;
        ++vm.trace_jit_stats.interpreter_bailouts;
        return false;
    }

    out_addr = static_cast<int>(addr_long);
    return true;
}

[[nodiscard]] inline bool executeTraceJitInstruction(
    VMState& vm,
    const VMTraceJitTrace& trace,
    const VMTraceJitInstruction& emitted,
    int current_index,
    int& next_index,
    bool& exit_trace) {

    const InstructionWord& iw = emitted.word;
    int pc_next = emitted.pc + 1;
    next_index = current_index + 1;
    exit_trace = false;
    vm.pc = emitted.pc;

    switch (emitted.op) {
        case VMTraceJitOp::Nop:
            break;

        case VMTraceJitOp::Mov:
            vm.regfile.write(iw.rd, ops::fromLong(iw.imm));
            break;

        case VMTraceJitOp::MovH: {
            TernaryValue current = vm.regfile.read(iw.rd);
            LongTriple currentLT = current.toLongTriple();
            auto trits = currentLT.unpack();
            int immVal = iw.imm;
            for (int i = 16; i < 32; ++i) {
                int r = (immVal + 1) % 3;
                if (r < 0) r += 3;
                int8_t trit = static_cast<int8_t>(r - 1);
                trits[i] = trit;
                immVal = (immVal - trit) / 3;
            }
            for (int i = 32; i < 50; ++i) trits[i] = 0;
            vm.regfile.write(iw.rd, TernaryValue::fromLongTriple(LongTriple::pack(trits)));
            break;
        }

        case VMTraceJitOp::Copy:
            vm.regfile.write(
                iw.rd, vm.regfile.readView(iw.rs1, emitted.mode));
            break;

        case VMTraceJitOp::TCmp: {
            const int8_t cmp = exec::compareValue(
                vm.regfile.readView(iw.rs1, emitted.mode),
                vm.regfile.readView(iw.rs2, emitted.mode),
                emitted.mode);
            if (!traceJitWriteChecked(
                    vm, emitted, iw.rd, makeTritResult(cmp))) return false;
            break;
        }

        case VMTraceJitOp::Add:
            if (!traceJitWriteChecked(vm, emitted, iw.rd, exec::addValue(
                    vm.regfile.readView(iw.rs1, emitted.mode),
                    vm.regfile.readView(iw.rs2, emitted.mode), emitted.mode))) return false;
            break;

        case VMTraceJitOp::Sub:
            if (!traceJitWriteChecked(vm, emitted, iw.rd, exec::subtractValue(
                    vm.regfile.readView(iw.rs1, emitted.mode),
                    vm.regfile.readView(iw.rs2, emitted.mode), emitted.mode))) return false;
            break;

        case VMTraceJitOp::Mul:
            if (!traceJitWriteChecked(vm, emitted, iw.rd, exec::multiplyValue(
                    vm.regfile.readView(iw.rs1, emitted.mode),
                    vm.regfile.readView(iw.rs2, emitted.mode), emitted.mode))) return false;
            break;

        case VMTraceJitOp::Neg:
            if (!traceJitWriteChecked(vm, emitted, iw.rd, exec::negateValue(
                    vm.regfile.readView(iw.rs1, emitted.mode), emitted.mode))) return false;
            break;

        case VMTraceJitOp::Abs:
            if (!traceJitWriteChecked(vm, emitted, iw.rd, exec::absValue(
                    vm.regfile.readView(iw.rs1, emitted.mode), emitted.mode))) return false;
            break;

        case VMTraceJitOp::Load: {
            int virtual_addr = 0;
            if (!traceJitVirtualAddress(vm, emitted, virtual_addr))
                return false;
            int physical_addr = 0;
            int routed_cause = OS_CAUSE_LOAD_FAULT;
            if (!vm.translateLoadAddress(
                    virtual_addr, physical_addr, routed_cause)) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            auto [value, fault] = vm.dmem.load(physical_addr);
            if (fault != MemFaultCode::OK) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            vm.regfile.write(iw.rd, value);
            break;
        }

        case VMTraceJitOp::Store: {
            int virtual_addr = 0;
            if (!traceJitVirtualAddress(vm, emitted, virtual_addr))
                return false;
            int physical_addr = 0;
            int routed_cause = OS_CAUSE_STORE_FAULT;
            if (!vm.translateStoreAddress(
                    virtual_addr, physical_addr, routed_cause)) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            const TernaryValue store_value =
                vm.regfile.readPhysical(iw.rs_store);
            if (vm.dmem.store(physical_addr, store_value) != MemFaultCode::OK) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            vm.noteStoreForReservation(physical_addr);
            break;
        }

        case VMTraceJitOp::Jmp:
            pc_next = emitted.branch_target;
            next_index = emitted.branch_target_index;
            exit_trace = next_index < 0;
            break;
        case VMTraceJitOp::Call:
            vm.regfile.writeLR(ops::fromLong(emitted.pc + 1));
            pc_next = emitted.branch_target;
            exit_trace = true;
            break;

        case VMTraceJitOp::Ret: {
            const int ret_addr = exec::pcFromValue(vm.regfile.readLR());
            if (!vm.validateControlTarget(ret_addr)) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            pc_next = ret_addr;
            exit_trace = true;
            break;
        }

        case VMTraceJitOp::CallR:
        case VMTraceJitOp::Jmpr: {
            const int dest = exec::pcFromValue(vm.regfile.read(iw.rs1));
            if (!vm.validateControlTarget(dest)) {
                vm.pc = emitted.pc;
                ++vm.trace_jit_stats.interpreter_bailouts;
                return false;
            }
            if (emitted.op == VMTraceJitOp::CallR)
                vm.regfile.writeLR(ops::fromLong(emitted.pc + 1));
            pc_next = dest;
            exit_trace = true;
            break;
        }


        case VMTraceJitOp::Brn:
        case VMTraceJitOp::Brz:
        case VMTraceJitOp::Brp: {
            ++vm.branch_instructions_count;
            const int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            const bool taken =
                (emitted.op == VMTraceJitOp::Brn && trit0 == T_NEG) ||
                (emitted.op == VMTraceJitOp::Brz && trit0 == T_ZER) ||
                (emitted.op == VMTraceJitOp::Brp && trit0 == T_POS);
            if (taken) {
                pc_next = emitted.branch_target;
                next_index = emitted.branch_target_index;
                exit_trace = next_index < 0;
            } else {
                pc_next = emitted.pc + 1;
                next_index = current_index + 1;
                exit_trace =
                    next_index >= static_cast<int>(trace.instructions.size()) ||
                    trace.instructions[static_cast<std::size_t>(next_index)].pc != pc_next;
            }
            break;
        }

        case VMTraceJitOp::Unsupported:
        default:
            vm.pc = emitted.pc;
            ++vm.trace_jit_stats.interpreter_bailouts;
            return false;
    }

    vm.completeInstruction(pc_next);
    return true;
}

inline int executeTraceJitTrace(
    VMState& vm,
    const VMTraceJitTrace& trace,
    int max_instructions) {

    if (!traceJitFastPathAvailable(vm) || max_instructions == 0 ||
        trace.instructions.empty() || vm.pc != trace.start_pc ||
        trace.cache_key != decodedTraceCacheKey(vm, trace.start_pc)) {
        return 0;
    }

    int executed = 0;
    int index = 0;
    ++vm.trace_jit_stats.traces_executed;

    while (vm.isRunning() && vm.power_control == 0 &&
           (max_instructions < 0 || executed < max_instructions) &&
           index >= 0 &&
           index < static_cast<int>(trace.instructions.size())) {

        if (!traceJitFastPathAvailable(vm) ||
            trace.cache_key !=
                decodedTraceCacheKey(vm, trace.start_pc)) {
            syncTraceJitGeneration(vm);
            break;
        }

        const VMTraceJitInstruction& emitted =
            trace.instructions[static_cast<std::size_t>(index)];
        if (vm.pc != emitted.pc) break;

        int next_index = index + 1;
        bool exit_trace = false;
        if (!executeTraceJitInstruction(
                vm, trace, emitted, index, next_index, exit_trace)) {
            break;
        }

        ++executed;
        ++vm.trace_jit_stats.instructions_executed;

        if (!vm.isRunning() || vm.power_control != 0 || exit_trace) break;
        if (next_index < 0 ||
            next_index >= static_cast<int>(trace.instructions.size())) {
            break;
        }
        index = next_index;
    }

    return executed;
}

inline int executeTraceJit(VMState& vm, int max_instructions) {
    if (!traceJitFastPathAvailable(vm) || max_instructions == 0) return 0;

    syncTraceJitGeneration(vm);
    ++vm.trace_jit_stats.hot_pc_samples;
    const int start_pc = vm.pc;
    const VMDecodedTraceCacheKey key = decodedTraceCacheKey(vm, start_pc);

    auto cached = vm.trace_jit_cache.find(key);
    if (cached != vm.trace_jit_cache.end() &&
        cached->second.cache_key == key) {
        return executeTraceJitTrace(vm, cached->second, max_instructions);
    }

    if (vm.trace_jit_unsupported_pcs.find(key) !=
        vm.trace_jit_unsupported_pcs.end()) {
        return 0;
    }

    const long long samples = ++vm.hot_pc_counts[key];
    if (samples < vm.trace_jit_hot_threshold) return 0;

    VMTraceJitTrace* trace = buildTraceJitTrace(vm, start_pc);
    if (trace == nullptr) return 0;
    return executeTraceJitTrace(vm, *trace, max_instructions);
}

struct VMNativeRunContext {
    // Native code only receives pointers to the state it commits.  This keeps
    // generated offsets tied to this standard-layout view instead of taking
    // layout-dependent offsets into VMState.
    struct StateAccess {
        int* pc = nullptr;
        long long* cycle_count = nullptr;
        long long* branch_instructions_count = nullptr;
        TernaryValue* registers = nullptr;
        // Dense DMEM fast-path state.  `dmem_words == nullptr` denotes sparse
        // backing, which must stay on the portable/helper path.
        TernaryValue* dmem_words = nullptr;
        long long dmem_capacity = 0;
        std::uint64_t* dmem_write_generation = nullptr;
        std::uint64_t* dmem_page_generations = nullptr;
        long long dmem_page_count = 0;
        long long privilege = 0;
        long long mmu_enable = 0;
        long long user_dmem_base = 0;
        long long user_dmem_limit = 0;
        long long atomic_reservation_valid = 0;
    } state;
    VMState* vm = nullptr;
    int budget = 0;
    int executed = 0;
    // Counts only instructions whose architectural commit was emitted into
    // the native block.  Helper-backed instructions still contribute to
    // `executed`, but are deliberately excluded from this measurement.
    int direct_executed = 0;
    int next_pc = 0;
    int exit_reason = 0;
};

[[nodiscard]] inline VMNativeRunContext::StateAccess
nativeX64StateAccess(VMState& vm) {
    const auto dmem = vm.dmem.nativeDenseStoreAccess();
    return VMNativeRunContext::StateAccess{
        &vm.pc,
        &vm.cycle_count,
        &vm.branch_instructions_count,
        vm.regfile.reg.data(),
        dmem.words,
        static_cast<long long>(dmem.capacity),
        dmem.write_generation,
        dmem.page_generations,
        static_cast<long long>(dmem.page_count),
        static_cast<long long>(static_cast<int8_t>(vm.privilege)),
        vm.mmu_enable ? 1LL : 0LL,
        static_cast<long long>(vm.user_dmem_base),
        static_cast<long long>(vm.user_dmem_limit),
        vm.atomic_reservation_valid ? 1LL : 0LL,
    };
}

enum class VMNativeX64ExitReason : int {
    None = 0,
    Budget,
    GuardFailure,
    Unsupported,
    BranchExit,
    VmStopped,
};

struct VMNativeX64Instruction {
    int pc = 0;
    int next_pc = 0;
    int branch_target = -1;
    int branch_target_index = -1;
    VMMicroOpcode op = VMMicroOpcode::Unsupported;
    TernaryMode mode = TernaryMode::T40;
    InstructionWord word{};
    TernaryValue immediate = TernaryValue::zero(TernaryMode::T40);
    TernaryValue* destination = nullptr;
    const TernaryValue* source = nullptr;
    const TernaryValue* source2 = nullptr;
    const TernaryValue* address_source = nullptr;
    const TernaryMode* address_mode = nullptr;
    const TernaryValue* store_source = nullptr;
    int expected_privilege = 0;
    const TernaryMode* destination_previous_mode = nullptr;
    TernaryMode* destination_mode = nullptr;
    bool ends_trace = false;
};

inline int nativeX64SideExit(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction,
    VMNativeX64ExitReason reason) {
    if (context == nullptr) return 0;
    context->exit_reason = static_cast<int>(reason);
    if (context->vm != nullptr && instruction != nullptr)
        context->vm->pc = instruction->pc;
    return 0;
}

[[nodiscard]] inline bool nativeX64CanStartInstruction(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction) {
    if (context == nullptr || instruction == nullptr || context->vm == nullptr)
        return false;
    VMState& vm = *context->vm;
    if (!vm.isRunning() || vm.power_control != 0 ||
        context->executed >= context->budget) {
        context->exit_reason = static_cast<int>(
            context->executed >= context->budget
                ? VMNativeX64ExitReason::Budget
                : VMNativeX64ExitReason::VmStopped);
        return false;
    }
    vm.pc = instruction->pc;
    return true;
}

inline int nativeX64CommitInstruction(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction) {
    if (!nativeX64CanStartInstruction(context, instruction)) return 0;
    VMState& vm = *context->vm;
    vm.completeInstruction(instruction->next_pc);
    ++context->executed;
    context->next_pc = vm.pc;
    if (instruction->ends_trace) {
        context->exit_reason = static_cast<int>(
            VMNativeX64ExitReason::BranchExit);
        return 0;
    }
    if (!vm.isRunning() || vm.power_control != 0 ||
        vm.pc != instruction->next_pc) {
        context->exit_reason = static_cast<int>(
            VMNativeX64ExitReason::VmStopped);
        return 0;
    }
    return 1;
}

inline int nativeX64DirectArithmetic(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction) {
    if (!nativeX64CanStartInstruction(context, instruction)) return 0;
    if (instruction->mode != TernaryMode::T40) {
        return nativeX64SideExit(
            context, instruction, VMNativeX64ExitReason::Unsupported);
    }
    VMState& vm = *context->vm;
    const TernaryValue lhs =
        vm.regfile.readView(instruction->word.rs1, instruction->mode);
    const TernaryValue rhs =
        vm.regfile.readView(instruction->word.rs2, instruction->mode);
    if (lhs.mode != TernaryMode::T40 || rhs.mode != TernaryMode::T40 ||
        lhs.isInvalid() || rhs.isInvalid()) {
        return nativeX64SideExit(
            context, instruction, VMNativeX64ExitReason::GuardFailure);
    }

    TernaryValue result = TernaryValue::invalid(TernaryMode::T40);
    switch (instruction->op) {
        case VMMicroOpcode::TCmp:
            result = makeTritResult(native_ops::compare(
                lhs.asTriple(), rhs.asTriple()));
            break;
        case VMMicroOpcode::Add:
            result = TernaryValue::fromTriple(native_ops::add(
                lhs.asTriple(), rhs.asTriple()));
            break;
        case VMMicroOpcode::Sub:
            result = TernaryValue::fromTriple(native_ops::subtract(
                lhs.asTriple(), rhs.asTriple()));
            break;
        case VMMicroOpcode::Mul:
            result = TernaryValue::fromTriple(native_ops::multiply(
                lhs.asTriple(), rhs.asTriple()));
            break;
        case VMMicroOpcode::Neg:
            result = TernaryValue::fromTriple(native_ops::negate(
                lhs.asTriple()));
            break;
        case VMMicroOpcode::Abs:
            result = TernaryValue::fromTriple(native_ops::abs(
                lhs.asTriple()));
            break;
        default:
            return nativeX64SideExit(
                context, instruction, VMNativeX64ExitReason::Unsupported);
    }
    if (result.isInvalid()) {
        return nativeX64SideExit(
            context, instruction, VMNativeX64ExitReason::GuardFailure);
    }
    vm.regfile.write(instruction->word.rd, result);
    return nativeX64CommitInstruction(context, instruction);
}

inline int nativeX64DirectMemory(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction) {
    // Portable/helper remainder for MMU, sparse, tagged, fractional, or
    // otherwise ineligible memory.  Keep the shared checked-address helper
    // authoritative so a native guard exit observes identical operand and
    // overflow classification before translation/trap routing.
    if (!nativeX64CanStartInstruction(context, instruction)) return 0;
    VMState& vm = *context->vm;
    int address = 0;
    const ScalarMemoryAddressStatus address_status =
        checkedScalarMemoryAddress(
            vm.regfile.read(instruction->word.rs1),
            instruction->word.imm,
            address);
    if (address_status != ScalarMemoryAddressStatus::Valid) {
        return nativeX64SideExit(
            context, instruction, VMNativeX64ExitReason::GuardFailure);
    }

    int physical_address = 0;
    int routed_cause = instruction->op == VMMicroOpcode::Load
        ? OS_CAUSE_LOAD_FAULT
        : OS_CAUSE_STORE_FAULT;
    const bool translated = instruction->op == VMMicroOpcode::Load
        ? vm.translateLoadAddress(
              address, physical_address, routed_cause)
        : vm.translateStoreAddress(
              address, physical_address, routed_cause);
    if (!translated) {
        return nativeX64SideExit(
            context, instruction, VMNativeX64ExitReason::GuardFailure);
    }

    if (instruction->op == VMMicroOpcode::Load) {
        auto [value, fault] = vm.dmem.load(physical_address);
        if (fault != MemFaultCode::OK || value.mode != TernaryMode::T40) {
            return nativeX64SideExit(
                context, instruction, VMNativeX64ExitReason::GuardFailure);
        }
        vm.regfile.write(instruction->word.rd, value);
    } else {
        const TernaryValue value =
            vm.regfile.readPhysical(instruction->word.rs_store);
        if (value.mode != TernaryMode::T40 ||
            vm.dmem.store(physical_address, value) != MemFaultCode::OK) {
            return nativeX64SideExit(
                context, instruction, VMNativeX64ExitReason::GuardFailure);
        }
        vm.noteStoreForReservation(physical_address);
    }
    return nativeX64CommitInstruction(context, instruction);
}

inline int nativeX64DirectControl(
    VMNativeRunContext* context,
    const VMNativeX64Instruction* instruction) {
    if (!nativeX64CanStartInstruction(context, instruction)) return 0;
    VMState& vm = *context->vm;
    int next_pc = instruction->next_pc;
    switch (instruction->op) {
        case VMMicroOpcode::Jmp:
            next_pc = instruction->branch_target;
            break;
        case VMMicroOpcode::Brn:
        case VMMicroOpcode::Brz:
        case VMMicroOpcode::Brp: {
            ++vm.branch_instructions_count;
            const int8_t trit0 = readTrit0(
                vm.regfile.read(instruction->word.rs_branch));
            const bool taken =
                (instruction->op == VMMicroOpcode::Brn && trit0 == T_NEG) ||
                (instruction->op == VMMicroOpcode::Brz && trit0 == T_ZER) ||
                (instruction->op == VMMicroOpcode::Brp && trit0 == T_POS);
            if (taken) next_pc = instruction->branch_target;
            break;
        }
        case VMMicroOpcode::Call:
            vm.regfile.writeLR(ops::fromLong(instruction->pc + 1));
            next_pc = instruction->branch_target;
            break;
        case VMMicroOpcode::Ret: {
            const int target = exec::pcFromValue(vm.regfile.readLR());
            if (!vm.validateControlTarget(target)) {
                return nativeX64SideExit(
                    context, instruction, VMNativeX64ExitReason::GuardFailure);
            }
            next_pc = target;
            break;
        }
        case VMMicroOpcode::CallR:
        case VMMicroOpcode::Jmpr: {
            const int target = exec::pcFromValue(
                vm.regfile.read(instruction->word.rs1));
            if (!vm.validateControlTarget(target)) {
                return nativeX64SideExit(
                    context, instruction, VMNativeX64ExitReason::GuardFailure);
            }
            if (instruction->op == VMMicroOpcode::CallR)
                vm.regfile.writeLR(ops::fromLong(instruction->pc + 1));
            next_pc = target;
            break;
        }
        default:
            return nativeX64SideExit(
                context, instruction, VMNativeX64ExitReason::Unsupported);
    }
    vm.completeInstruction(next_pc);
    ++context->executed;
    context->next_pc = vm.pc;
    context->exit_reason = static_cast<int>(
        VMNativeX64ExitReason::BranchExit);
    return 0;
}

[[nodiscard]] inline bool nativeX64TraceDirectlyEligible(
    const VMState& vm,
    const VMTraceJitTrace& trace) {
    if (trace.instructions.empty()) return false;
    // The inline commit sequence below updates PC, cycle_count, and the
    // native instruction budget without calling VMState::completeInstruction.
    // Timer delivery and routed traps must therefore remain on the precise
    // portable path, where recordCycle() can observe them between every
    // instruction.
    if (vm.timer_enable || vm.trap_routing_enabled) return false;
    // Register storage is always canonical physical T40.  Numeric view tags
    // (T1/T5/T10/T20) therefore do not change the value seen by a T40
    // lowering, and a previous direct TCMP may legitimately leave a T1 view
    // tag behind.  Lane views are different: branch predicates inspect the
    // lane's trit zero rather than the numeric sign, so keep those traces on
    // the portable path.
    for (const TernaryMode mode : vm.regfile.view_mode) {
        if (isLaneMode(mode)) return false;
    }
    for (const VMMicroOp& micro_op : trace.instructions) {
        switch (micro_op.op) {
            case VMMicroOpcode::Nop:
            case VMMicroOpcode::Mov:
            case VMMicroOpcode::Load:
            case VMMicroOpcode::Store:
            case VMMicroOpcode::Jmp:
            case VMMicroOpcode::Brn:
            case VMMicroOpcode::Brz:
            case VMMicroOpcode::Brp:
            case VMMicroOpcode::Call:
            case VMMicroOpcode::Ret:
            case VMMicroOpcode::CallR:
            case VMMicroOpcode::Jmpr:
                break;
            case VMMicroOpcode::Copy:
            case VMMicroOpcode::TCmp:
            case VMMicroOpcode::Add:
            case VMMicroOpcode::Sub:
            case VMMicroOpcode::Mul:
            case VMMicroOpcode::Neg:
            case VMMicroOpcode::Abs:
                if (micro_op.mode != TernaryMode::T40) return false;
                break;
            case VMMicroOpcode::MovH:
            case VMMicroOpcode::Unsupported:
                return false;
        }
    }
    return true;
}

struct VMNativeX64CodeBlock {
    using EntryPoint = int (*)(VMNativeRunContext*);

    void* allocation = nullptr;
    std::size_t allocation_size = 0;
    std::size_t code_size = 0;
    EntryPoint entry = nullptr;
    std::vector<VMNativeX64Instruction> lowered;
    std::size_t direct_instruction_count = 0;
    std::size_t helper_instruction_count = 0;
    bool writable = false;
    bool executable = false;

    VMNativeX64CodeBlock() = default;
    VMNativeX64CodeBlock(const VMNativeX64CodeBlock&) = delete;
    VMNativeX64CodeBlock& operator=(const VMNativeX64CodeBlock&) = delete;

    ~VMNativeX64CodeBlock() {
        if (!allocation) return;
#if defined(_WIN32)
        VirtualFree(allocation, 0, MEM_RELEASE);
#elif defined(__x86_64__) || defined(__aarch64__)
        munmap(allocation, allocation_size);
#endif
    }

    [[nodiscard]] bool isWriteXorExecute() const {
        return allocation != nullptr && executable && !writable;
    }
};

// Keep the static lowering inventory honest.  The scalar T40 arithmetic
// subset, guarded dense identity-memory LOAD/STORE subset, and in-trace
// control flow are emitted as host instructions; remaining arithmetic and
// MMU/sparse/tagged memory cases stay helper-backed so their guards and side
// exits retain the portable architectural semantics.
[[nodiscard]] inline bool nativeX64InstructionIsDirect(
    const VMNativeX64Instruction& instruction,
    std::size_t block_length) {
    switch (instruction.op) {
        case VMMicroOpcode::Nop:
        case VMMicroOpcode::Mov:
        case VMMicroOpcode::Copy:
            return true;
        case VMMicroOpcode::TCmp:
            return instruction.mode == TernaryMode::T40 &&
                   instruction.source != nullptr &&
                   instruction.source2 != nullptr;
        case VMMicroOpcode::Jmp:
        case VMMicroOpcode::Brn:
        case VMMicroOpcode::Brz:
        case VMMicroOpcode::Brp:
            return instruction.branch_target_index >= 0 &&
                   instruction.branch_target_index < block_length &&
                   instruction.word.rs_branch < REG_COUNT;
        case VMMicroOpcode::Add:
        case VMMicroOpcode::Sub:
            return instruction.mode == TernaryMode::T40 &&
                   instruction.source != nullptr &&
                   instruction.source2 != nullptr;
        case VMMicroOpcode::Mul:
            return instruction.mode == TernaryMode::T40 &&
                   instruction.source != nullptr &&
                   instruction.source2 != nullptr;
        case VMMicroOpcode::Neg:
        case VMMicroOpcode::Abs:
            return instruction.mode == TernaryMode::T40 &&
                   instruction.source != nullptr;
        case VMMicroOpcode::Call:
        case VMMicroOpcode::Ret:
        case VMMicroOpcode::CallR:
        case VMMicroOpcode::Jmpr:
        case VMMicroOpcode::MovH:
        case VMMicroOpcode::Unsupported:
            return false;
        case VMMicroOpcode::Load:
        case VMMicroOpcode::Store:
            // Memory lowerings contain an explicit identity/dense/T40 guard
            // and side-exit to the portable path for MMU, sparse, tagged,
            // fractional, permission, or faulting cases.
            return instruction.address_source != nullptr &&
                   instruction.address_mode != nullptr &&
                   (instruction.op != VMMicroOpcode::Store ||
                    instruction.store_source != nullptr);
    }
    return false;
}

[[nodiscard]] inline bool nativeX64HostAvailable() {
#if defined(_M_X64) || defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

#if defined(_M_X64) || defined(__x86_64__)
struct VMNativeX64Emitter {
    std::vector<std::uint8_t> code;
    std::vector<std::size_t> exit_jumps;

    void byte(std::uint8_t value) { code.push_back(value); }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void movImm64(std::uint8_t reg, std::uint64_t value) {
        byte(static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 1 : 0)));
        byte(static_cast<std::uint8_t>(0xB8 + (reg & 7)));
        u64(value);
    }
    void movRegReg(std::uint8_t dst, std::uint8_t src) {
        byte(static_cast<std::uint8_t>(0x48 |
            (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)));
        byte(0x89);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((src & 7) << 3) | (dst & 7)));
    }
    void addRegReg(std::uint8_t dst, std::uint8_t src) {
        byte(static_cast<std::uint8_t>(0x48 |
            (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)));
        byte(0x01);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((src & 7) << 3) | (dst & 7)));
    }
    void subRegReg(std::uint8_t dst, std::uint8_t src) {
        byte(static_cast<std::uint8_t>(0x48 |
            (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)));
        byte(0x29);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((src & 7) << 3) | (dst & 7)));
    }
    void imulRegReg(std::uint8_t dst, std::uint8_t src) {
        byte(static_cast<std::uint8_t>(0x48 |
            (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0)));
        byte(0x0F); byte(0xAF);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((dst & 7) << 3) | (src & 7)));
    }
    void negReg(std::uint8_t reg) {
        byte(static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 1 : 0)));
        byte(0xF7);
        byte(static_cast<std::uint8_t>(0xD8 | (reg & 7)));
    }
    void testRegReg(std::uint8_t lhs, std::uint8_t rhs) {
        byte(static_cast<std::uint8_t>(0x48 |
            (rhs >= 8 ? 4 : 0) | (lhs >= 8 ? 1 : 0)));
        byte(0x85);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((rhs & 7) << 3) | (lhs & 7)));
    }
    void movRegMemDisp(
        std::uint8_t dst, std::uint8_t base, std::uint32_t displacement) {
        byte(static_cast<std::uint8_t>(0x48 |
            (dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0)));
        byte(0x8B);
        const bool needs_sib = (base & 7) == 4;
        byte(static_cast<std::uint8_t>(0x80 |
            ((dst & 7) << 3) | (needs_sib ? 4 : (base & 7))));
        if (needs_sib) byte(0x24);
        u32(displacement);
    }
    void movzxRegMemByte(
        std::uint8_t dst, std::uint8_t base, std::uint32_t displacement) {
        // MOVZX r64, byte ptr [base + disp32].  A 32-bit destination write
        // zero-extends, so the explicit REX.W bit is intentionally omitted.
        byte(static_cast<std::uint8_t>(0x40 |
            (dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0)));
        byte(0x0F); byte(0xB6);
        const bool needs_sib = (base & 7) == 4;
        byte(static_cast<std::uint8_t>(0x80 |
            ((dst & 7) << 3) | (needs_sib ? 4 : (base & 7))));
        if (needs_sib) byte(0x24);
        u32(displacement);
    }
    void movMemDispReg(
        std::uint8_t base, std::uint32_t displacement, std::uint8_t src) {
        byte(static_cast<std::uint8_t>(0x48 |
            (src >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0)));
        byte(0x89);
        const bool needs_sib = (base & 7) == 4;
        byte(static_cast<std::uint8_t>(0x80 |
            ((src & 7) << 3) | (needs_sib ? 4 : (base & 7))));
        if (needs_sib) byte(0x24);
        u32(displacement);
    }
    void cmpRegReg(std::uint8_t lhs, std::uint8_t rhs) {
        byte(static_cast<std::uint8_t>(0x48 |
            (rhs >= 8 ? 4 : 0) | (lhs >= 8 ? 1 : 0)));
        byte(0x39);
        byte(static_cast<std::uint8_t>(0xC0 |
            ((rhs & 7) << 3) | (lhs & 7)));
    }
    void divReg(std::uint8_t divisor) {
        // Unsigned RDX:RAX / divisor; the quotient remains in RAX and the
        // remainder in RDX.  T40 mantissa extraction uses this for modulo
        // 3^33 because a binary mask is not valid for positional ternary.
        byte(static_cast<std::uint8_t>(0x48 | (divisor >= 8 ? 1 : 0)));
        byte(0xF7);
        byte(static_cast<std::uint8_t>(0xF0 | (divisor & 7)));
    }
    void idivReg(std::uint8_t divisor) {
        byte(static_cast<std::uint8_t>(0x48 | (divisor >= 8 ? 1 : 0)));
        byte(0xF7);
        byte(static_cast<std::uint8_t>(0xF8 | (divisor & 7)));
    }
    void mulReg(std::uint8_t multiplier) {
        byte(static_cast<std::uint8_t>(0x48 | (multiplier >= 8 ? 1 : 0)));
        byte(0xF7);
        byte(static_cast<std::uint8_t>(0xE0 | (multiplier & 7)));
    }
    void cqo() {
        byte(0x48); byte(0x99);
    }
    void addRegImm8(std::uint8_t reg, std::int8_t value) {
        byte(static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 1 : 0)));
        byte(0x83);
        byte(static_cast<std::uint8_t>(0xC0 | (reg & 7)));
        byte(static_cast<std::uint8_t>(value));
    }
    void subRegImm8(std::uint8_t reg, std::int8_t value) {
        byte(static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 1 : 0)));
        byte(0x83);
        byte(static_cast<std::uint8_t>(0xE8 | (reg & 7)));
        byte(static_cast<std::uint8_t>(value));
    }
    void clearRdx() {
        byte(0x48); byte(0x31); byte(0xD2);
    }
    std::size_t jccRel32(std::uint8_t condition) {
        byte(0x0F);
        byte(condition);
        const std::size_t displacement = code.size();
        u32(0);
        return displacement;
    }
    std::size_t jmpRel32() {
        byte(0xE9);
        const std::size_t displacement = code.size();
        u32(0);
        return displacement;
    }
    void addMemDispImm8(
        std::uint8_t base, std::uint32_t displacement, std::uint8_t value) {
        byte(static_cast<std::uint8_t>(0x48 | (base >= 8 ? 1 : 0)));
        byte(0x83);
        const bool needs_sib = (base & 7) == 4;
        byte(static_cast<std::uint8_t>(0x80 |
            (needs_sib ? 4 : (base & 7))));
        if (needs_sib) byte(0x24);
        u32(displacement);
        byte(value);
    }
    void commitSimple(int next_pc) {
        // r12 holds VMNativeRunContext*.  Use r11 for VMState* and r10 for
        // the next PC; neither value survives into the C++ side because this
        // path does not make a helper call.  Keep a separate direct counter
        // so helper-backed arithmetic/memory does not look like native code
        // in the JIT measurements.
        movRegMemDisp(
            11, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, pc)));
        movImm64(10, static_cast<std::uint64_t>(
            static_cast<std::int64_t>(next_pc)));
        movMemDispReg(
            11, 0,
            10);
        movRegMemDisp(
            11, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, cycle_count)));
        addMemDispImm8(
            11, 0, 1);
        addMemDispImm8(
            12,
            static_cast<std::uint32_t>(offsetof(VMNativeRunContext, executed)),
            1);
        addMemDispImm8(
            12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, direct_executed)),
            1);
    }
    void movByteMemImm(
        std::uint8_t base, std::uint32_t displacement, std::uint8_t value) {
        if (base >= 8) byte(0x41);
        byte(0xC6);
        const bool needs_sib = (base & 7) == 4;
        byte(static_cast<std::uint8_t>(0x80 |
            (needs_sib ? 4 : (base & 7))));
        if (needs_sib) byte(0x24);
        u32(displacement);
        byte(value);
    }
    void budgetGuard() {
        movRegMemDisp(
            0, 12,
            static_cast<std::uint32_t>(offsetof(VMNativeRunContext, budget)));
        byte(0x41); byte(0x3B); byte(0x84); byte(0x24);
        u32(static_cast<std::uint32_t>(
            offsetof(VMNativeRunContext, executed)));
        byte(0x0F); byte(0x8E);
        const std::size_t displacement = code.size();
        u32(0);
        exit_jumps.push_back(displacement);
    }
    void callHelper(
        const void* helper,
        const VMNativeX64Instruction* instruction) {
#if defined(_WIN32)
        movRegReg(1, 12);
        movImm64(2, reinterpret_cast<std::uintptr_t>(instruction));
#else
        movRegReg(7, 12);
        movImm64(6, reinterpret_cast<std::uintptr_t>(instruction));
#endif
        movImm64(0, reinterpret_cast<std::uintptr_t>(helper));
        byte(0xFF); byte(0xD0);
        byte(0x85); byte(0xC0);
        byte(0x0F); byte(0x84);
        const std::size_t displacement = code.size();
        u32(0);
        exit_jumps.push_back(displacement);
    }
    void patchExits(std::size_t target) {
        for (const std::size_t displacement : exit_jumps) {
            const std::int64_t relative =
                static_cast<std::int64_t>(target) -
                static_cast<std::int64_t>(displacement + 4);
            const std::uint32_t encoded =
                static_cast<std::uint32_t>(relative);
            for (int shift = 0; shift < 32; shift += 8)
                code[displacement + static_cast<std::size_t>(shift / 8)] =
                    static_cast<std::uint8_t>(encoded >> shift);
        }
    }
    void patchRelative(std::size_t displacement, std::size_t target) {
        const std::int64_t relative =
            static_cast<std::int64_t>(target) -
            static_cast<std::int64_t>(displacement + 4);
        const std::uint32_t encoded = static_cast<std::uint32_t>(relative);
        for (int shift = 0; shift < 32; shift += 8)
            code[displacement + static_cast<std::size_t>(shift / 8)] =
                static_cast<std::uint8_t>(encoded >> shift);
    }
    void emitGuardFailure(int pc) {
        // r12 holds VMNativeRunContext*.  Preserve precise state without
        // entering a helper: the portable dispatcher resumes at this PC.
        movRegMemDisp(
            11, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, pc)));
        movImm64(10, static_cast<std::uint64_t>(static_cast<std::int64_t>(pc)));
        movMemDispReg(
            11, 0,
            10);
        movImm64(10, static_cast<std::uint64_t>(
            static_cast<std::int64_t>(VMNativeX64ExitReason::GuardFailure)));
        movMemDispReg(
            12,
            static_cast<std::uint32_t>(offsetof(VMNativeRunContext, exit_reason)),
            10);
    }
    void emitGuardT40Mode(
        const TernaryMode* mode,
        std::vector<std::size_t>& guard_jumps) {
        if (mode == nullptr) {
            guard_jumps.push_back(jmpRel32());
            return;
        }
        movImm64(10, reinterpret_cast<std::uintptr_t>(mode));
        movzxRegMemByte(0, 10, 0);
        movImm64(10, static_cast<std::uint64_t>(TernaryMode::T40));
        cmpRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x85)); // jne
    }
    void emitGuardDestinationPair(
        TernaryMode* destination_mode,
        const TernaryMode* previous_mode,
        std::vector<std::size_t>& guard_jumps) {
        auto guardWide = [&](const TernaryMode* mode) {
            if (mode == nullptr) return;
            movImm64(10, reinterpret_cast<std::uintptr_t>(mode));
            movzxRegMemByte(0, 10, 0);
            movImm64(10, static_cast<std::uint64_t>(TernaryMode::T50));
            cmpRegReg(0, 10);
            guard_jumps.push_back(jccRel32(0x84)); // je
            movImm64(10, static_cast<std::uint64_t>(TernaryMode::L50));
            cmpRegReg(0, 10);
            guard_jumps.push_back(jccRel32(0x84)); // je
        };
        // `RegFile::write(T40)` clears a wide pair before committing.  Keep
        // that mutation on the portable path whenever either half is live;
        // ordinary single-width tags can be overwritten in place.
        guardWide(destination_mode);
        guardWide(previous_mode);
    }
    void emitLoadRawT40(
        const TernaryValue* source,
        std::vector<std::size_t>& guard_jumps) {
        if (source == nullptr) {
            guard_jumps.push_back(jmpRel32());
            return;
        }
        const std::uint64_t kPow3T40 =
            native_ops::detail::pow3(40).toUint64();
        movImm64(10, reinterpret_cast<std::uintptr_t>(source));
        // Register storage is normally canonical T40, but callers can
        // construct a VMState with a tagged/non-canonical physical payload.
        // The portable STORE preserves that payload, so side-exit before
        // rewriting its mode to T40 in the inline lowering.
        movzxRegMemByte(
            8, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, mode)));
        // R9 retains the page index for the generation update below.
        movImm64(0, static_cast<std::uint64_t>(TernaryMode::T40));
        cmpRegReg(8, 0);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        movRegMemDisp(
            0, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, lo)));
        movRegMemDisp(
            2, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, hi)));
        testRegReg(2, 2);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        movImm64(10, kPow3T40);
        cmpRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x83)); // jae
    }
    void emitLoadRawT40Unary(
        const TernaryValue* source,
        bool absolute,
        std::vector<std::size_t>& guard_jumps) {
        if (source == nullptr) {
            guard_jumps.push_back(jmpRel32());
            return;
        }

        const std::uint64_t kPow3Mantissa =
            native_ops::detail::pow3(33).toUint64();
        const std::uint64_t kPow3T40 =
            native_ops::detail::pow3(40).toUint64();
        const std::uint64_t kMantissaMidpoint =
            (kPow3Mantissa - 1) / 2;

        // T40's lower 33 positional trits are the signed mantissa and the
        // upper seven trits are the exponent.  Negating a valid value flips
        // each balanced mantissa trit, which is the positional complement
        // `3^33 - 1 - mantissa`; the exponent is intentionally untouched.
        // This operates on the raw canonical word, so fractional values are
        // exact instead of being rounded through a host integer conversion.
        movImm64(10, reinterpret_cast<std::uintptr_t>(source));
        movzxRegMemByte(
            11, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, mode)));
        movImm64(9, static_cast<std::uint64_t>(TernaryMode::T40));
        cmpRegReg(11, 9);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        movRegMemDisp(
            0, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, lo)));
        movRegMemDisp(
            11, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, hi)));
        testRegReg(11, 11);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        movImm64(10, kPow3T40);
        cmpRegReg(0, 10);
        // This rejects invalid payloads and the overflow/underflow sentinels
        // before they can be mistaken for positional T40 data.
        guard_jumps.push_back(jccRel32(0x83)); // jae

        // Keep the original raw word for ABS's non-negative path.
        movRegReg(8, 0);
        testRegReg(0, 0);
        const std::size_t zero_jump = jccRel32(0x84); // je

        movImm64(10, kPow3Mantissa);
        clearRdx();
        divReg(10); // RAX=exponent code, RDX=raw mantissa

        auto emitNegatedParts = [&]() {
            movImm64(10, kPow3Mantissa - 1);
            subRegReg(10, 2);
            movRegReg(9, 0); // preserve exponent code
            movImm64(11, kPow3Mantissa);
            imulRegReg(9, 11);
            addRegReg(9, 10);
            movRegReg(0, 9);
        };

        if (absolute) {
            movImm64(10, kMantissaMidpoint);
            cmpRegReg(2, 10);
            const std::size_t nonnegative_jump = jccRel32(0x83); // jae
            emitNegatedParts();
            const std::size_t negative_done_jump = jmpRel32();
            const std::size_t nonnegative_label = code.size();
            movRegReg(0, 8);
            const std::size_t nonnegative_done = code.size();
            patchRelative(nonnegative_jump, nonnegative_label);
            patchRelative(negative_done_jump, nonnegative_done);
        } else {
            emitNegatedParts();
        }

        const std::size_t skip_zero_jump = jmpRel32();
        const std::size_t zero_label = code.size();
        movImm64(0, 0);
        const std::size_t end = code.size();
        patchRelative(zero_jump, zero_label);
        patchRelative(skip_zero_jump, end);
    }
    void emitNativeMemoryStateGuards(
        const VMNativeX64Instruction& instruction,
        bool store,
        std::vector<std::size_t>& guard_jumps) {
        // The inline lowering is intentionally identity-only.  MMU walks,
        // TLB permission checks, page-fault metadata, and sparse/device
        // backing all remain owned by the portable helper/interpreter.
        movRegMemDisp(
            0, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, mmu_enable)));
        testRegReg(0, 0);
        guard_jumps.push_back(jccRel32(0x85)); // jne

        movRegMemDisp(
            0, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, privilege)));
        movImm64(
            10,
            static_cast<std::uint64_t>(
                static_cast<std::int64_t>(instruction.expected_privilege)));
        cmpRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x85)); // jne

        movRegMemDisp(
            11, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, dmem_words)));
        testRegReg(11, 11);
        guard_jumps.push_back(jccRel32(0x84)); // je

        if (store) {
            movRegMemDisp(
                0, 12,
                static_cast<std::uint32_t>(
                    offsetof(VMNativeRunContext, state) +
                    offsetof(VMNativeRunContext::StateAccess,
                             dmem_write_generation)));
            testRegReg(0, 0);
            guard_jumps.push_back(jccRel32(0x84)); // je
            movRegMemDisp(
                0, 12,
                static_cast<std::uint32_t>(
                    offsetof(VMNativeRunContext, state) +
                    offsetof(VMNativeRunContext::StateAccess,
                             dmem_page_generations)));
            testRegReg(0, 0);
            guard_jumps.push_back(jccRel32(0x84)); // je
            // A live reservation may need to be cleared only when its
            // address matches the store.  Side-exit for all such stores so
            // the portable path preserves both matching and non-matching
            // reservation semantics without mutating before the exit.
            movRegMemDisp(
                0, 12,
                static_cast<std::uint32_t>(
                    offsetof(VMNativeRunContext, state) +
                    offsetof(VMNativeRunContext::StateAccess,
                             atomic_reservation_valid)));
            testRegReg(0, 0);
            guard_jumps.push_back(jccRel32(0x85)); // jne
        }
    }
    void emitNativeMemoryAddress(
        const VMNativeX64Instruction& instruction,
        std::vector<std::size_t>& guard_jumps) {
        emitGuardT40Mode(instruction.address_mode, guard_jumps);
        emitLoadT40Integer(instruction.address_source, guard_jumps);
        movImm64(
            10,
            static_cast<std::uint64_t>(
                static_cast<std::int64_t>(instruction.word.imm)));
        addRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x80)); // jo

        // checkedScalarMemoryAddress rejects every negative/out-of-int
        // result before translation.  The dense capacity check below also
        // rejects values above INT_MAX, but keep the signed lower bound
        // explicit so the side-exit contract remains auditable.
        testRegReg(0, 0);
        guard_jumps.push_back(jccRel32(0x8C)); // jl
        movRegMemDisp(
            10, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess, dmem_capacity)));
        cmpRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x83)); // jae

        if (instruction.expected_privilege !=
            static_cast<int>(static_cast<int8_t>(PrivilegeMode::Kernel))) {
            movRegMemDisp(
                10, 12,
                static_cast<std::uint32_t>(
                    offsetof(VMNativeRunContext, state) +
                    offsetof(VMNativeRunContext::StateAccess,
                             user_dmem_base)));
            cmpRegReg(0, 10);
            guard_jumps.push_back(jccRel32(0x8C)); // jl
            movRegMemDisp(
                10, 12,
                static_cast<std::uint32_t>(
                    offsetof(VMNativeRunContext, state) +
                    offsetof(VMNativeRunContext::StateAccess,
                             user_dmem_limit)));
            cmpRegReg(0, 10);
            // Match the portable signed `address >= user_dmem_limit` check;
            // a malformed negative limit must not pass via an unsigned JAE.
            guard_jumps.push_back(jccRel32(0x8D)); // jge
        }
    }
    void emitNativeDenseWordPointer() {
        // rax holds the checked physical address.  Convert the word index to
        // a byte offset and add it to the dense DMEM base in r11.
        movImm64(10, static_cast<std::uint64_t>(sizeof(TernaryValue)));
        imulRegReg(0, 10);
        addRegReg(11, 0);
    }
    void emitNativeStoreGeneration() {
        // Mirror TernaryMemory::noteStore()/nextGeneration() exactly:
        // UINT64_MAX wraps to 1, then the returned generation is ++ => 2;
        // otherwise the counter increments once and the page receives the
        // same value.  All pointers and page bounds were guarded before the
        // raw word write, so this bookkeeping cannot side-exit.
        movRegMemDisp(
            10, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess,
                         dmem_write_generation)));
        movRegMemDisp(0, 10, 0);
        movImm64(11, std::numeric_limits<std::uint64_t>::max());
        cmpRegReg(0, 11);
        const std::size_t not_max = jccRel32(0x85); // jne
        movImm64(0, 1);
        patchRelative(not_max, code.size());
        addRegImm8(0, 1);
        movMemDispReg(10, 0, 0);

        movRegMemDisp(
            11, 12,
            static_cast<std::uint32_t>(
                offsetof(VMNativeRunContext, state) +
                offsetof(VMNativeRunContext::StateAccess,
                         dmem_page_generations)));
        movImm64(10, sizeof(std::uint64_t));
        imulRegReg(9, 10);
        addRegReg(11, 9);
        movMemDispReg(11, 0, 0);
    }
    void emitLoadT40Integer(
        const TernaryValue* source,
        std::vector<std::size_t>& guard_jumps) {
        if (source == nullptr) {
            guard_jumps.push_back(jmpRel32());
            return;
        }

        const std::uint64_t kPow3Mantissa =
            native_ops::detail::pow3(33).toUint64();
        const std::uint64_t kPow3T40 =
            native_ops::detail::pow3(40).toUint64();
        const std::uint64_t kMantissaMidpoint =
            (kPow3Mantissa - 1) / 2;
        const std::uint64_t kExponentMidpoint = 1093;

        movImm64(10, reinterpret_cast<std::uintptr_t>(source));
        // `emitLoadT40Integer` decodes the canonical physical T40 payload;
        // a hostile tagged register must take the portable path instead of
        // reinterpreting its narrower bits as a T40 address/operand.  Keep
        // R8 untouched because callers use it to retain the first operand.
        movzxRegMemByte(
            11, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, mode)));
        movImm64(9, static_cast<std::uint64_t>(TernaryMode::T40));
        cmpRegReg(11, 9);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        movRegMemDisp(
            0, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, lo)));
        movRegMemDisp(
            11, 10, static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, hi)));
        testRegReg(11, 11);
        guard_jumps.push_back(jccRel32(0x85)); // jne
        testRegReg(0, 0);
        const std::size_t zero_jump = jccRel32(0x84); // je

        movImm64(10, kPow3T40);
        cmpRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x83)); // jae

        movImm64(10, kPow3Mantissa);
        clearRdx();
        divReg(10);
        movRegReg(9, 0); // raw exponent code
        movRegReg(0, 2); // raw mantissa
        movImm64(10, kMantissaMidpoint);
        subRegReg(0, 10); // signed mantissa in rax
        movImm64(10, kExponentMidpoint);
        subRegReg(9, 10); // balanced exponent in r9

        // Decode the represented scalar relative to radix 3^32.  Values
        // below radix 32 require exact signed division; values above it are
        // scaled with checked signed multiplication.  This accepts the
        // normalized encodings produced by floatFromInt instead of assuming
        // that every integer retains exponent 32.
        movImm64(10, 32);
        cmpRegReg(9, 10);
        const std::size_t less_than_radix_jump = jccRel32(0x8C); // jl
        const std::size_t equal_radix_jump = jccRel32(0x84); // je

        // The radix-32 exponent is already represented by the mantissa
        // scale.  Only the excess exponent needs multiplication.
        subRegReg(9, 10);
        movImm64(10, 3);
        const std::size_t multiply_loop = code.size();
        testRegReg(9, 9);
        const std::size_t multiply_done_jump = jccRel32(0x84); // je
        imulRegReg(0, 10);
        guard_jumps.push_back(jccRel32(0x80)); // jo
        addRegImm8(9, -1);
        const std::size_t multiply_back = jmpRel32();
        patchRelative(multiply_back, multiply_loop);
        const std::size_t skip_division_jump = jmpRel32();

        const std::size_t less_than_radix_label = code.size();
        movImm64(10, 32);
        subRegReg(10, 9); // number of exact divisions
        const std::size_t divide_loop = code.size();
        testRegReg(10, 10);
        const std::size_t divide_done_jump = jccRel32(0x84); // je
        movImm64(11, 3);
        cqo();
        idivReg(11);
        testRegReg(2, 2); // a fractional remainder is not an integer
        guard_jumps.push_back(jccRel32(0x85)); // jne
        subRegImm8(10, 1);
        const std::size_t divide_back = jmpRel32();
        patchRelative(divide_back, divide_loop);

        const std::size_t scale_done_label = code.size();
        patchRelative(less_than_radix_jump, less_than_radix_label);
        patchRelative(equal_radix_jump, scale_done_label);
        patchRelative(multiply_done_jump, scale_done_label);
        patchRelative(skip_division_jump, scale_done_label);
        patchRelative(divide_done_jump, scale_done_label);

        const std::size_t skip_zero = jmpRel32();
        const std::size_t zero_label = code.size();
        movImm64(0, 0);
        const std::size_t end = code.size();
        patchRelative(zero_jump, zero_label);
        patchRelative(skip_zero, end);
    }
    void emitEncodeT40Integer(std::vector<std::size_t>& guard_jumps) {
        const std::uint64_t kPow3Mantissa =
            native_ops::detail::pow3(33).toUint64();
        const std::uint64_t kMantissaMax =
            (kPow3Mantissa - 1) / 2;
        const std::uint64_t kPow3MantissaMin =
            (native_ops::detail::pow3(32).toUint64() - 1) / 2;
        const std::uint64_t kExponentMidpoint = 1093;

        testRegReg(0, 0);
        const std::size_t zero_jump = jccRel32(0x84); // je

        // Keep the sign in r9 and normalize the positive magnitude in rax.
        testRegReg(0, 0);
        const std::size_t nonnegative_jump = jccRel32(0x8D); // jge
        negReg(0);
        guard_jumps.push_back(jccRel32(0x80)); // jo
        movImm64(9, 1); // negative
        const std::size_t sign_ready_jump = jmpRel32();
        const std::size_t nonnegative_label = code.size();
        movImm64(9, 0); // non-negative
        const std::size_t sign_ready_label = code.size();
        patchRelative(nonnegative_jump, nonnegative_label);
        patchRelative(sign_ready_jump, sign_ready_label);

        movImm64(10, 32);
        const std::size_t reduce_loop = code.size();
        movImm64(11, kMantissaMax);
        cmpRegReg(0, 11);
        const std::size_t reduce_done_jump = jccRel32(0x86); // jbe
        movImm64(11, 3);
        clearRdx();
        divReg(11);
        movImm64(11, 2);
        cmpRegReg(2, 11);
        const std::size_t no_round_jump = jccRel32(0x85); // jne
        addRegImm8(0, 1);
        const std::size_t rounded_label = code.size();
        patchRelative(no_round_jump, rounded_label);
        addRegImm8(10, 1);
        const std::size_t reduce_back = jmpRel32();
        patchRelative(reduce_back, reduce_loop);

        const std::size_t reduce_done_label = code.size();
        patchRelative(reduce_done_jump, reduce_done_label);
        const std::size_t expand_loop = code.size();
        movImm64(11, kPow3MantissaMin);
        cmpRegReg(0, 11);
        const std::size_t expand_done_jump = jccRel32(0x83); // jae
        movImm64(11, 3);
        imulRegReg(0, 11);
        guard_jumps.push_back(jccRel32(0x80)); // jo
        subRegImm8(10, 1);
        const std::size_t expand_back = jmpRel32();
        patchRelative(expand_back, expand_loop);

        const std::size_t normalize_done_label = code.size();
        patchRelative(expand_done_jump, normalize_done_label);
        movImm64(11, static_cast<std::uint64_t>(1093));
        cmpRegReg(10, 11);
        guard_jumps.push_back(jccRel32(0x8F)); // jg
        movImm64(11, static_cast<std::uint64_t>(-
            static_cast<std::int64_t>(1093)));
        cmpRegReg(10, 11);
        guard_jumps.push_back(jccRel32(0x8C)); // jl

        // Convert the balanced mantissa back to its positional payload and
        // append the biased exponent payload.  The bounded exponent check
        // proves the unsigned product fits below 3^40.
        movImm64(11, kMantissaMax);
        testRegReg(9, 9);
        const std::size_t nonnegative_mantissa_jump = jccRel32(0x84); // je
        subRegReg(11, 0); // midpoint - magnitude
        const std::size_t mantissa_ready_jump = jmpRel32();
        const std::size_t nonnegative_mantissa_label = code.size();
        addRegReg(11, 0); // midpoint + magnitude
        const std::size_t mantissa_ready_label = code.size();
        patchRelative(nonnegative_mantissa_jump, nonnegative_mantissa_label);
        patchRelative(mantissa_ready_jump, mantissa_ready_label);

        movRegReg(9, 11); // raw mantissa payload
        movImm64(11, kExponentMidpoint);
        addRegReg(10, 11);
        movRegReg(0, 10);
        movImm64(8, native_ops::detail::pow3(33).toUint64());
        mulReg(8);
        addRegReg(0, 9);
        const std::size_t result_jump = jmpRel32();

        const std::size_t zero_label = code.size();
        movImm64(0, 0);
        const std::size_t result_label = code.size();
        patchRelative(zero_jump, zero_label);
        patchRelative(result_jump, result_label);
    }
    void emitStoreT40Result(
        TernaryValue* destination,
        TernaryMode* destination_mode) {
        if (destination == nullptr) return;
        movImm64(10, reinterpret_cast<std::uintptr_t>(destination));
        movMemDispReg(
            10,
            static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, lo)),
            0);
        clearRdx();
        movMemDispReg(
            10,
            static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, hi)),
            2);
        movByteMemImm(
            10,
            static_cast<std::uint32_t>(offsetof(TernaryValue, mode)),
            static_cast<std::uint8_t>(TernaryMode::T40));
        if (destination_mode != nullptr) {
            movImm64(10, reinterpret_cast<std::uintptr_t>(destination_mode));
            movByteMemImm(
                10, 0, static_cast<std::uint8_t>(TernaryMode::T40));
        }
    }
    void emitStoreT1Result(
        TernaryValue* destination,
        TernaryMode* destination_mode) {
        if (destination == nullptr) return;
        movImm64(10, reinterpret_cast<std::uintptr_t>(destination));
        movMemDispReg(
            10,
            static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, lo)),
            0);
        clearRdx();
        movMemDispReg(
            10,
            static_cast<std::uint32_t>(
                offsetof(TernaryValue, bits) + offsetof(UInt128, hi)),
            2);
        movByteMemImm(
            10,
            static_cast<std::uint32_t>(offsetof(TernaryValue, mode)),
            static_cast<std::uint8_t>(TernaryMode::T40));
        if (destination_mode != nullptr) {
            movImm64(10, reinterpret_cast<std::uintptr_t>(destination_mode));
            movByteMemImm(
                10, 0, static_cast<std::uint8_t>(TernaryMode::T1));
        }
    }
};
#endif

[[nodiscard]] inline std::shared_ptr<VMNativeX64CodeBlock>
compileNativeX64Trace(VMState& vm, const VMTraceJitTrace& trace) {
#if !defined(_M_X64) && !defined(__x86_64__)
    (void)vm;
    (void)trace;
    return {};
#else
    if (!nativeX64TraceDirectlyEligible(vm, trace)) return {};
    auto block = std::make_shared<VMNativeX64CodeBlock>();
    block->lowered.reserve(trace.instructions.size());
    for (const VMMicroOp& micro_op : trace.instructions) {
        VMNativeX64Instruction instruction;
        instruction.pc = micro_op.pc;
        instruction.next_pc = micro_op.pc + 1;
        instruction.branch_target = micro_op.branch_target;
        instruction.branch_target_index = micro_op.branch_target_index;
        instruction.op = micro_op.op;
        instruction.mode = micro_op.mode;
        instruction.word = micro_op.word;
        instruction.ends_trace = micro_op.ends_trace;
        instruction.expected_privilege =
            static_cast<int>(static_cast<int8_t>(vm.privilege));
        if (instruction.op == VMMicroOpcode::Mov) {
            instruction.immediate = ops::fromLong(instruction.word.imm);
        } else if (instruction.op == VMMicroOpcode::Copy ||
                   instruction.op == VMMicroOpcode::TCmp) {
            instruction.immediate = TernaryValue::zero(TernaryMode::T40);
        }
        if (instruction.word.rd != R0_ZERO &&
            instruction.word.rd < REG_COUNT) {
            instruction.destination =
                &vm.regfile.reg[instruction.word.rd];
            instruction.destination_mode =
                &vm.regfile.view_mode[instruction.word.rd];
            if (instruction.word.rd > 0) {
                instruction.destination_previous_mode =
                    &vm.regfile.view_mode[instruction.word.rd - 1];
            }
        }
        if (instruction.op == VMMicroOpcode::Copy &&
            instruction.word.rs1 != R0_ZERO &&
            instruction.word.rs1 < REG_COUNT) {
            instruction.source =
                &vm.regfile.reg[instruction.word.rs1];
        }
        if ((instruction.op == VMMicroOpcode::Add ||
             instruction.op == VMMicroOpcode::Sub ||
             instruction.op == VMMicroOpcode::Mul ||
             instruction.op == VMMicroOpcode::Neg ||
             instruction.op == VMMicroOpcode::Abs ||
             instruction.op == VMMicroOpcode::TCmp) &&
            instruction.word.rs1 < REG_COUNT) {
            instruction.source = &vm.regfile.reg[instruction.word.rs1];
        }
        if ((instruction.op == VMMicroOpcode::Add ||
             instruction.op == VMMicroOpcode::Sub ||
             instruction.op == VMMicroOpcode::Mul ||
             instruction.op == VMMicroOpcode::TCmp) &&
            instruction.word.rs2 < REG_COUNT) {
            instruction.source2 = &vm.regfile.reg[instruction.word.rs2];
        }
        if ((instruction.op == VMMicroOpcode::Load ||
             instruction.op == VMMicroOpcode::Store) &&
            instruction.word.rs1 < REG_COUNT) {
            instruction.address_source =
                &vm.regfile.reg[instruction.word.rs1];
            instruction.address_mode =
                &vm.regfile.view_mode[instruction.word.rs1];
        }
        if (instruction.op == VMMicroOpcode::Store &&
            instruction.word.rs_store < REG_COUNT) {
            instruction.store_source =
                &vm.regfile.reg[instruction.word.rs_store];
        }
        block->lowered.push_back(instruction);
    }

    VMNativeX64Emitter emitter;
    emitter.code.reserve(128 + block->lowered.size() * 64);
    std::vector<std::size_t> instruction_offsets;
    instruction_offsets.reserve(block->lowered.size());
    struct PendingBranch {
        std::size_t target_index = 0;
        std::size_t taken_jump_displacement = 0;
    };
    std::vector<PendingBranch> pending_branches;
    emitter.byte(0x41); emitter.byte(0x54);
#if defined(_WIN32)
    emitter.movRegReg(12, 1);
    emitter.byte(0x48); emitter.byte(0x83); emitter.byte(0xEC); emitter.byte(0x20);
#else
    emitter.movRegReg(12, 7);
#endif

    for (const VMNativeX64Instruction& instruction : block->lowered) {
        if (nativeX64InstructionIsDirect(
                instruction, block->lowered.size())) {
            ++block->direct_instruction_count;
        } else {
            ++block->helper_instruction_count;
        }
        instruction_offsets.push_back(emitter.code.size());
        switch (instruction.op) {
            case VMMicroOpcode::Nop:
            case VMMicroOpcode::Mov:
            case VMMicroOpcode::Copy:
                emitter.budgetGuard();
                if (instruction.op == VMMicroOpcode::Mov &&
                    instruction.destination != nullptr) {
                    emitter.movImm64(10, reinterpret_cast<std::uintptr_t>(
                        instruction.destination));
                    emitter.movImm64(0, instruction.immediate.bits.lo);
                    emitter.movMemDispReg(
                        10, static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, lo)), 0);
                    emitter.movImm64(0, instruction.immediate.bits.hi);
                    emitter.movMemDispReg(
                        10, static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, hi)), 0);
                    emitter.movByteMemImm(
                        10, static_cast<std::uint32_t>(offsetof(TernaryValue, mode)),
                        static_cast<std::uint8_t>(TernaryMode::T40));
                    emitter.movImm64(10, reinterpret_cast<std::uintptr_t>(
                        instruction.destination_mode));
                    emitter.movByteMemImm(
                        10, 0, static_cast<std::uint8_t>(TernaryMode::T40));
                } else if (instruction.op == VMMicroOpcode::Copy &&
                           instruction.destination != nullptr) {
                    emitter.movImm64(11, reinterpret_cast<std::uintptr_t>(
                        instruction.destination));
                    if (instruction.source != nullptr) {
                        emitter.movImm64(10, reinterpret_cast<std::uintptr_t>(
                            instruction.source));
                        emitter.movRegMemDisp(
                            0, 10, static_cast<std::uint32_t>(
                                offsetof(TernaryValue, bits) +
                                offsetof(UInt128, lo)));
                        emitter.movRegMemDisp(
                            2, 10, static_cast<std::uint32_t>(
                                offsetof(TernaryValue, bits) +
                                offsetof(UInt128, hi)));
                    } else {
                        emitter.movImm64(0, instruction.immediate.bits.lo);
                        emitter.movImm64(2, instruction.immediate.bits.hi);
                    }
                    emitter.movMemDispReg(
                        11, static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, lo)), 0);
                    emitter.movMemDispReg(
                        11, static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, hi)), 2);
                    emitter.movByteMemImm(
                        11, static_cast<std::uint32_t>(offsetof(TernaryValue, mode)),
                        static_cast<std::uint8_t>(TernaryMode::T40));
                    emitter.movImm64(10, reinterpret_cast<std::uintptr_t>(
                        instruction.destination_mode));
                    emitter.movByteMemImm(
                        10, 0, static_cast<std::uint8_t>(TernaryMode::T40));
                }
                emitter.commitSimple(instruction.next_pc);
                break;
            case VMMicroOpcode::TCmp: {
                if (!nativeX64InstructionIsDirect(
                        instruction, block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectArithmetic),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                std::vector<std::size_t> guard_jumps;
                emitter.emitGuardDestinationPair(
                    instruction.destination_mode,
                    instruction.destination_previous_mode,
                    guard_jumps);
                emitter.emitLoadT40Integer(
                    instruction.source, guard_jumps);
                emitter.movRegReg(8, 0);
                emitter.emitLoadT40Integer(
                    instruction.source2, guard_jumps);
                emitter.cmpRegReg(8, 0);
                const std::size_t negative_jump = emitter.jccRel32(0x8C);
                const std::size_t positive_jump = emitter.jccRel32(0x8F);
                emitter.movImm64(0, 0);
                const std::size_t zero_result_jump = emitter.jmpRel32();
                const std::size_t negative_label = emitter.code.size();
                emitter.movImm64(0, static_cast<std::uint64_t>(
                    native_ops::fromIntT40(-1).data));
                const std::size_t negative_result_jump = emitter.jmpRel32();
                const std::size_t positive_label = emitter.code.size();
                emitter.movImm64(0, static_cast<std::uint64_t>(
                    native_ops::fromIntT40(1).data));
                const std::size_t result_label = emitter.code.size();
                emitter.emitStoreT1Result(
                    instruction.destination, instruction.destination_mode);
                emitter.commitSimple(instruction.next_pc);
                const std::size_t skip_guard = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_exit = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_exit);
                emitter.patchRelative(negative_jump, negative_label);
                emitter.patchRelative(positive_jump, positive_label);
                emitter.patchRelative(zero_result_jump, result_label);
                emitter.patchRelative(negative_result_jump, result_label);
                emitter.patchRelative(skip_guard, emitter.code.size());
                for (const std::size_t jump : guard_jumps)
                    emitter.patchRelative(jump, guard_offset);
                break;
            }
            case VMMicroOpcode::Add:
            case VMMicroOpcode::Sub: {
                if (!nativeX64InstructionIsDirect(
                        instruction, block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectArithmetic),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                std::vector<std::size_t> guard_jumps;
                emitter.emitGuardDestinationPair(
                    instruction.destination_mode,
                    instruction.destination_previous_mode,
                    guard_jumps);
                emitter.emitLoadT40Integer(
                    instruction.source, guard_jumps);
                emitter.movRegReg(8, 0);
                emitter.emitLoadT40Integer(
                    instruction.source2, guard_jumps);
                if (instruction.op == VMMicroOpcode::Add) {
                    emitter.addRegReg(0, 8);
                } else {
                    emitter.subRegReg(8, 0);
                    emitter.movRegReg(0, 8);
                }
                guard_jumps.push_back(emitter.jccRel32(0x80)); // jo
                emitter.emitEncodeT40Integer(guard_jumps);
                emitter.emitStoreT40Result(
                    instruction.destination, instruction.destination_mode);
                emitter.commitSimple(instruction.next_pc);
                const std::size_t skip_guard = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_exit = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_exit);
                emitter.patchRelative(skip_guard, emitter.code.size());
                for (const std::size_t jump : guard_jumps)
                    emitter.patchRelative(jump, guard_offset);
                break;
            }
            case VMMicroOpcode::Mul: {
                if (!nativeX64InstructionIsDirect(
                        instruction, block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectArithmetic),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                std::vector<std::size_t> guard_jumps;
                emitter.emitGuardDestinationPair(
                    instruction.destination_mode,
                    instruction.destination_previous_mode,
                    guard_jumps);
                emitter.emitLoadT40Integer(
                    instruction.source, guard_jumps);
                emitter.movRegReg(8, 0);
                emitter.emitLoadT40Integer(
                    instruction.source2, guard_jumps);
                // The exact integral subset is represented in signed int64.
                // IMUL's overflow flag is the guard that sends products which
                // exceed that host range to the portable T40 implementation.
                emitter.imulRegReg(8, 0);
                guard_jumps.push_back(emitter.jccRel32(0x80)); // jo
                emitter.movRegReg(0, 8);
                emitter.emitEncodeT40Integer(guard_jumps);
                emitter.emitStoreT40Result(
                    instruction.destination, instruction.destination_mode);
                emitter.commitSimple(instruction.next_pc);
                const std::size_t skip_guard = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_exit = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_exit);
                emitter.patchRelative(skip_guard, emitter.code.size());
                for (const std::size_t jump : guard_jumps)
                    emitter.patchRelative(jump, guard_offset);
                break;
            }
            case VMMicroOpcode::Neg:
            case VMMicroOpcode::Abs: {
                if (!nativeX64InstructionIsDirect(
                        instruction, block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectArithmetic),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                std::vector<std::size_t> guard_jumps;
                emitter.emitGuardDestinationPair(
                    instruction.destination_mode,
                    instruction.destination_previous_mode,
                    guard_jumps);
                emitter.emitLoadRawT40Unary(
                    instruction.source,
                    instruction.op == VMMicroOpcode::Abs,
                    guard_jumps);
                emitter.emitStoreT40Result(
                    instruction.destination, instruction.destination_mode);
                emitter.commitSimple(instruction.next_pc);
                const std::size_t skip_guard = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_exit = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_exit);
                emitter.patchRelative(skip_guard, emitter.code.size());
                for (const std::size_t jump : guard_jumps)
                    emitter.patchRelative(jump, guard_offset);
                break;
            }
            case VMMicroOpcode::Load:
            case VMMicroOpcode::Store: {
                if (!nativeX64InstructionIsDirect(
                        instruction, block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectMemory),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                std::vector<std::size_t> guard_jumps;
                const bool store = instruction.op == VMMicroOpcode::Store;
                emitter.emitNativeMemoryStateGuards(
                    instruction, store, guard_jumps);
                if (!store) {
                    emitter.emitGuardDestinationPair(
                        instruction.destination_mode,
                        instruction.destination_previous_mode,
                        guard_jumps);
                }
                emitter.emitNativeMemoryAddress(instruction, guard_jumps);

                if (store) {
                    // Preserve the page index for the dense generation update
                    // while using rax for the byte offset below.
                    emitter.movRegReg(8, 0);
                    emitter.movRegReg(9, 0);
                    emitter.movImm64(10, SPARSE_VM_PAGE_WORDS);
                    emitter.clearRdx();
                    emitter.divReg(10);
                    emitter.movRegReg(9, 0);
                    emitter.movRegReg(0, 8);
                    emitter.movRegMemDisp(
                        10, 12,
                        static_cast<std::uint32_t>(
                            offsetof(VMNativeRunContext, state) +
                            offsetof(VMNativeRunContext::StateAccess,
                                     dmem_page_count)));
                    emitter.cmpRegReg(9, 10);
                    guard_jumps.push_back(emitter.jccRel32(0x83)); // jae
                }

                emitter.movRegMemDisp(
                    11, 12,
                    static_cast<std::uint32_t>(
                        offsetof(VMNativeRunContext, state) +
                        offsetof(VMNativeRunContext::StateAccess,
                                 dmem_words)));
                emitter.emitNativeDenseWordPointer();

                if (store) {
                    // Load the canonical physical T40 source only after all
                    // address/state guards have passed; an invalid source
                    // side-exits before the destination word is touched.
                    emitter.emitLoadRawT40(
                        instruction.store_source, guard_jumps);
                    emitter.movMemDispReg(
                        11,
                        static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, lo)),
                        0);
                    emitter.movMemDispReg(
                        11,
                        static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, hi)),
                        2);
                    emitter.movByteMemImm(
                        11,
                        static_cast<std::uint32_t>(
                            offsetof(TernaryValue, mode)),
                        static_cast<std::uint8_t>(TernaryMode::T40));
                    emitter.emitNativeStoreGeneration();
                } else {
                    const std::uint64_t kPow3T40 =
                        native_ops::detail::pow3(40).toUint64();
                    emitter.movzxRegMemByte(
                        10, 11,
                        static_cast<std::uint32_t>(offsetof(
                            TernaryValue, mode)));
                    emitter.movImm64(
                        0, static_cast<std::uint64_t>(TernaryMode::T40));
                    emitter.cmpRegReg(10, 0);
                    guard_jumps.push_back(emitter.jccRel32(0x85)); // jne
                    emitter.movRegMemDisp(
                        0, 11,
                        static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, lo)));
                    emitter.movRegMemDisp(
                        2, 11,
                        static_cast<std::uint32_t>(
                            offsetof(TernaryValue, bits) +
                            offsetof(UInt128, hi)));
                    emitter.testRegReg(2, 2);
                    guard_jumps.push_back(emitter.jccRel32(0x85)); // jne
                    emitter.movImm64(10, kPow3T40);
                    emitter.cmpRegReg(0, 10);
                    guard_jumps.push_back(emitter.jccRel32(0x83)); // jae
                    emitter.emitStoreT40Result(
                        instruction.destination,
                        instruction.destination_mode);
                }

                emitter.commitSimple(instruction.next_pc);
                const std::size_t skip_guard = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_exit = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_exit);
                emitter.patchRelative(skip_guard, emitter.code.size());
                for (const std::size_t jump : guard_jumps)
                    emitter.patchRelative(jump, guard_offset);
                break;
            }
            case VMMicroOpcode::Jmp:
                if (instruction.branch_target_index >= 0 &&
                    instruction.branch_target_index <
                        static_cast<int>(block->lowered.size())) {
                    emitter.budgetGuard();
                    // The unconditional branch has no condition path; its
                    // commit is emitted after the target labels are known.
                    emitter.commitSimple(instruction.branch_target);
                    const std::size_t jump = emitter.jmpRel32();
                    pending_branches.push_back(PendingBranch{
                        static_cast<std::size_t>(instruction.branch_target_index),
                        jump});
                    break;
                }
                emitter.callHelper(
                    reinterpret_cast<const void*>(&nativeX64DirectControl),
                    &instruction);
                break;
            case VMMicroOpcode::Brn:
            case VMMicroOpcode::Brz:
            case VMMicroOpcode::Brp: {
                if (instruction.branch_target_index < 0 ||
                    instruction.branch_target_index >=
                        static_cast<int>(block->lowered.size())) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectControl),
                        &instruction);
                    break;
                }
                emitter.budgetGuard();
                if (instruction.word.rs_branch >= REG_COUNT) {
                    emitter.callHelper(
                        reinterpret_cast<const void*>(&nativeX64DirectControl),
                        &instruction);
                    break;
                }
                // readTrit0(TernaryValue) is the numeric sign for T40.  The
                // zero encoding is a special raw zero, while every non-zero
                // valid T40 value has a midpoint-relative mantissa payload.
                const std::uint64_t mantissa =
                    native_ops::detail::pow3(33).toUint64();
                const std::uint64_t midpoint = (mantissa - 1) / 2;
                // Read the branch source through the standard-layout state
                // view.  The generated displacement is a register-file
                // element offset, never an offset into VMState itself.
                emitter.movRegMemDisp(
                    10, 12,
                    static_cast<std::uint32_t>(
                        offsetof(VMNativeRunContext, state) +
                        offsetof(VMNativeRunContext::StateAccess, registers)));
                emitter.movRegMemDisp(
                    0, 10, static_cast<std::uint32_t>(
                        static_cast<std::size_t>(instruction.word.rs_branch) *
                            sizeof(TernaryValue) +
                        offsetof(TernaryValue, bits) +
                        offsetof(UInt128, lo)));
                emitter.testRegReg(0, 0);
                const std::size_t zero_jump = emitter.jccRel32(0x84); // je
                emitter.movImm64(10, native_ops::detail::pow3(40).toUint64());
                emitter.cmpRegReg(0, 10);
                const std::size_t invalid_jump = emitter.jccRel32(0x83); // jae
                emitter.movImm64(10, mantissa);
                emitter.clearRdx();
                emitter.divReg(10);
                emitter.movImm64(10, midpoint);
                emitter.cmpRegReg(2, 10);
                const std::uint8_t condition =
                    instruction.op == VMMicroOpcode::Brn ? 0x8C :
                    instruction.op == VMMicroOpcode::Brz ? 0x84 : 0x8F;
                const std::size_t condition_jump =
                    emitter.jccRel32(condition);
                const std::size_t nonzero_fallthrough_jump =
                    emitter.jmpRel32();
                const std::size_t zero_label = emitter.code.size();
                const std::size_t zero_branch_jump = emitter.jmpRel32();
                const std::size_t fallthrough_offset = emitter.code.size();
                emitter.movRegMemDisp(
                    11, 12,
                    static_cast<std::uint32_t>(
                        offsetof(VMNativeRunContext, state) +
                        offsetof(VMNativeRunContext::StateAccess,
                                 branch_instructions_count)));
                emitter.addMemDispImm8(
                    11, 0, 1);
                emitter.commitSimple(instruction.next_pc);
                const std::size_t fallthrough_jump = emitter.jmpRel32();
                // A trace ends at every conditional branch.  The not-taken
                // path therefore returns to the portable dispatcher (which
                // resumes at the committed fall-through PC); it must not
                // fall through into the guard bytes or the next lowering.
                emitter.exit_jumps.push_back(fallthrough_jump);
                const std::size_t taken_offset = emitter.code.size();
                emitter.movRegMemDisp(
                    11, 12,
                    static_cast<std::uint32_t>(
                        offsetof(VMNativeRunContext, state) +
                        offsetof(VMNativeRunContext::StateAccess,
                                 branch_instructions_count)));
                emitter.addMemDispImm8(
                    11, 0, 1);
                emitter.commitSimple(instruction.branch_target);
                const std::size_t taken_jump = emitter.jmpRel32();
                const std::size_t guard_offset = emitter.code.size();
                emitter.emitGuardFailure(instruction.pc);
                const std::size_t guard_jump = emitter.jmpRel32();
                emitter.exit_jumps.push_back(guard_jump);
                if (instruction.branch_target_index >= 0 &&
                    instruction.branch_target_index <
                        static_cast<int>(block->lowered.size())) {
                    pending_branches.push_back(PendingBranch{
                        static_cast<std::size_t>(
                            instruction.branch_target_index),
                        taken_jump});
                } else {
                    // No in-trace target exists.  The taken path has already
                    // committed the architectural target and must return via
                    // the same epilogue as the fall-through path.
                    emitter.exit_jumps.push_back(taken_jump);
                }
                emitter.patchRelative(invalid_jump, guard_offset);
                emitter.patchRelative(condition_jump, taken_offset);
                emitter.patchRelative(nonzero_fallthrough_jump,
                                      fallthrough_offset);
                emitter.patchRelative(zero_jump, zero_label);
                if (instruction.op == VMMicroOpcode::Brz) {
                    emitter.patchRelative(zero_branch_jump, taken_offset);
                } else {
                    emitter.patchRelative(zero_branch_jump, fallthrough_offset);
                }
                break;
            }
            case VMMicroOpcode::Call:
            case VMMicroOpcode::Ret:
            case VMMicroOpcode::CallR:
            case VMMicroOpcode::Jmpr:
                emitter.callHelper(
                    reinterpret_cast<const void*>(&nativeX64DirectControl),
                    &instruction);
                break;
            case VMMicroOpcode::MovH:
            case VMMicroOpcode::Unsupported:
                return {};
        }
    }

    const std::size_t epilogue = emitter.code.size();
    emitter.patchExits(epilogue);
    for (const PendingBranch& branch : pending_branches) {
        if (branch.target_index < instruction_offsets.size()) {
            emitter.patchRelative(
                branch.taken_jump_displacement,
                instruction_offsets[branch.target_index]);
        }
    }
    emitter.movRegMemDisp(
        0, 12, static_cast<std::uint32_t>(
            offsetof(VMNativeRunContext, executed)));
#if defined(_WIN32)
    emitter.byte(0x48); emitter.byte(0x83); emitter.byte(0xC4); emitter.byte(0x20);
#endif
    emitter.byte(0x41); emitter.byte(0x5C);
    emitter.byte(0xC3);

#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::size_t page_size = static_cast<std::size_t>(info.dwPageSize);
#else
    const long page_size_long = sysconf(_SC_PAGESIZE);
    const std::size_t page_size = page_size_long > 0
        ? static_cast<std::size_t>(page_size_long)
        : std::size_t{4096};
#endif
    block->allocation_size =
        ((emitter.code.size() + page_size - 1) / page_size) * page_size;
#if defined(_WIN32)
    block->allocation = VirtualAlloc(
        nullptr, block->allocation_size,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    block->allocation = mmap(
        nullptr, block->allocation_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block->allocation == MAP_FAILED) block->allocation = nullptr;
#endif
    if (!block->allocation) return {};
    block->writable = true;
    std::memcpy(block->allocation, emitter.code.data(), emitter.code.size());
    block->code_size = emitter.code.size();
#if defined(_WIN32)
    DWORD previous = 0;
    if (!VirtualProtect(
            block->allocation, block->allocation_size,
            PAGE_EXECUTE_READ, &previous)) {
        return {};
    }
    FlushInstructionCache(
        GetCurrentProcess(), block->allocation, block->code_size);
#else
    if (mprotect(
            block->allocation, block->allocation_size,
            PROT_READ | PROT_EXEC) != 0) {
        return {};
    }
    __builtin___clear_cache(
        static_cast<char*>(block->allocation),
        static_cast<char*>(block->allocation) + block->code_size);
#endif
    block->writable = false;
    block->executable = true;
    block->entry = reinterpret_cast<VMNativeX64CodeBlock::EntryPoint>(
        block->allocation);
    return block;
#endif
}

inline int executeNativeX64Jit(
    VMState& vm,
    int max_instructions) {
    if (!nativeX64HostAvailable() || !vm.nativeX64JitEnabled() ||
        !vm.isRunning() || max_instructions == 0) {
        return 0;
    }
    syncTraceJitGeneration(vm);
    ++vm.trace_jit_stats.hot_pc_samples;
    const int start_pc = vm.pc;
    const VMDecodedTraceCacheKey key =
        decodedTraceCacheKey(vm, start_pc);
    auto trace = vm.trace_jit_cache.find(key);
    if (trace == vm.trace_jit_cache.end()) {
        if (vm.trace_jit_unsupported_pcs.count(key)) return 0;
        const long long samples = ++vm.hot_pc_counts[key];
        if (samples < vm.trace_jit_hot_threshold) return 0;
        if (!buildTraceJitTrace(vm, start_pc)) return 0;
        trace = vm.trace_jit_cache.find(key);
        if (trace == vm.trace_jit_cache.end()) return 0;
    }
    if (!nativeX64TraceDirectlyEligible(vm, trace->second)) {
        ++vm.native_x64_jit_stats.portable_side_exits;
        return 0;
    }

    auto code = vm.native_x64_code_cache.find(key);
    std::shared_ptr<VMNativeX64CodeBlock> block;
    if (code == vm.native_x64_code_cache.end()) {
        ++vm.native_x64_jit_stats.compilation_attempts;
        block = compileNativeX64Trace(vm, trace->second);
        if (!block || !block->isWriteXorExecute()) {
            ++vm.native_x64_jit_stats.portable_side_exits;
            return 0;
        }
        vm.native_x64_code_cache.emplace(key, block);
        ++vm.native_x64_jit_stats.blocks_built;
        ++vm.native_x64_jit_stats.wx_transitions;
    } else {
        block = std::static_pointer_cast<VMNativeX64CodeBlock>(
            code->second);
    }

    VMNativeRunContext context;
    context.vm = &vm;
    context.state = nativeX64StateAccess(vm);
    context.budget = max_instructions;
    const int executed = block->entry(&context);
    if (executed > 0) {
        ++vm.native_x64_jit_stats.blocks_executed;
        vm.native_x64_jit_stats.instructions_executed += executed;
        vm.native_x64_jit_stats.direct_instructions += context.direct_executed;
        if (context.exit_reason == static_cast<int>(
                VMNativeX64ExitReason::GuardFailure) ||
            context.exit_reason == static_cast<int>(
                VMNativeX64ExitReason::Unsupported)) {
            ++vm.native_x64_jit_stats.portable_side_exits;
        }
    } else {
        ++vm.native_x64_jit_stats.portable_side_exits;
    }
    return executed;
}
inline VMStatus stepCore(VMState& vm, int core_id) {
    if (core_id < 0 || core_id >= vm.coreCount()) return vm.status;
    if (vm.active_core >= 0 && vm.active_core < vm.coreCount() && vm.active_core != core_id) {
        vm.captureCoreState(vm.active_core);
    }
    vm.restoreCoreState(core_id);
    if (!vm.isRunning()) {
        vm.captureCoreState(core_id);
        return vm.status;
    }
    const VMStatus after = step(vm);
    vm.captureCoreState(core_id);
    return after;
}

struct RunResult {
    VMStatus   status;       // Final VMStatus when execution stopped
    int        steps;        // Number of instructions executed
    int        final_pc;     // PC value when execution stopped
    TrapCode   trap_code;    // Only meaningful when status == TRAPPED
    std::string description; // Human-readable reason for stopping

    [[nodiscard]] bool halted()  const { return status == VMStatus::HALTED;  }
    [[nodiscard]] bool trapped() const { return status == VMStatus::TRAPPED; }
    [[nodiscard]] bool timeout() const { return status == VMStatus::RUNNING; }
};

inline RunResult runMultiCore(VMState& vm, int max_steps = 1000000) {
    int steps = 0;
    bool any_running = true;
    while (any_running) {
        any_running = false;
        for (int core = 0; core < vm.coreCount(); ++core) {
            if (vm.coreState(core).status == VMStatus::RUNNING) {
                any_running = true;
                if (max_steps >= 0 && steps >= max_steps) {
                    RunResult timeout{VMStatus::RUNNING, steps, vm.coreState(core).pc,
                                      TrapCode::TRAP_ILLEGAL_OP,
                                      "TIMEOUT after " + std::to_string(steps) + " steps"};
                    return timeout;
                }
                stepCore(vm, core);
                ++steps;
            }
        }
    }

    VMStatus final_status = VMStatus::HALTED;
    int final_pc = 0;
    TrapCode trap = TrapCode::TRAP_ILLEGAL_OP;
    for (int core = 0; core < vm.coreCount(); ++core) {
        const VMCoreState& state = vm.coreState(core);
        final_pc = state.pc;
        if (state.status == VMStatus::TRAPPED) {
            final_status = VMStatus::TRAPPED;
            trap = decodeTrap(state.trap_reg);
            break;
        }
    }
    return RunResult{final_status, steps, final_pc, trap,
                     (final_status == VMStatus::HALTED ? "HALT" : "TRAP") +
                         std::string(" after ") + std::to_string(steps) + " core steps"};
}

// Run the VM until HALT, TRAP, or max_steps is reached.
// One step is one architecturally executed instruction, regardless of opcode
// family or vector length. max_steps == -1 means unlimited.
// Reaching max_steps leaves VMStatus as RUNNING and reports timeout().
// Returns a RunResult describing why execution stopped.
inline RunResult run(VMState& vm, int max_steps = 1000000,
                     const VMHooks* hooks = nullptr) {
    int steps = 0;
    while (vm.isRunning() && vm.power_control == 0) {
        if (max_steps >= 0 && steps >= max_steps) break;
        if (!hooks &&
            vm.execution_backend == VMExecutionBackend::NativeX64Jit) {
            const int remaining = max_steps < 0
                ? std::numeric_limits<int>::max()
                : max_steps - steps;
            const int native_steps =
                executeNativeX64Jit(vm, remaining);
            if (native_steps > 0) {
                steps += native_steps;
                continue;
            }
        }
        if (!hooks && vm.execution_backend == VMExecutionBackend::TraceJit) {
            const int remaining = max_steps < 0
                ? std::numeric_limits<int>::max()
                : max_steps - steps;
            const int trace_steps = executeTraceJit(vm, remaining);
            if (trace_steps > 0) {
                steps += trace_steps;
                continue;
            }
        }
        if (!hooks &&
            vm.execution_backend == VMExecutionBackend::CachedBlockInterpreter &&
            blockCacheFastPathAvailable(vm)) {
            const int remaining = max_steps < 0
                ? std::numeric_limits<int>::max()
                : max_steps - steps;
            const int cached_steps = executeCachedBlock(vm, remaining);
            if (cached_steps > 0) {
                steps += cached_steps;
                continue;
            }
        }
        if (hooks) step(vm, *hooks);
        else step(vm);
        ++steps;
        ++vm.block_cache_stats.fallback_steps;
    }

    RunResult r;
    r.status   = vm.status;
    r.steps    = steps;
    r.final_pc = vm.pc;

    if (vm.isHalted()) {
        r.trap_code   = TrapCode::TRAP_ILLEGAL_OP;  // unused
        r.description = "HALT at PC=" + std::to_string(vm.pc)
                      + " after " + std::to_string(steps) + " steps";
    } else if (vm.isTrapped()) {
        r.trap_code = decodeTrap(vm.trap_reg);
        std::string tc;
        switch (r.trap_code) {
            case TrapCode::TRAP_DIV_ZERO:   tc = "TRAP_DIV_ZERO";   break;
            case TrapCode::TRAP_MEM_FAULT:  tc = "TRAP_MEM_FAULT";  break;
            case TrapCode::TRAP_ILLEGAL_OP: tc = "TRAP_ILLEGAL_OP"; break;
        }
        r.description = tc + " at PC=" + std::to_string(vm.pc)
                      + " after " + std::to_string(steps) + " steps";
    } else if (vm.power_control != 0) {
        r.trap_code = TrapCode::TRAP_ILLEGAL_OP;  // unused
        r.description = "Power control request at PC=" + std::to_string(vm.pc) +
                        " after " + std::to_string(steps) + " steps";
    } else {
        r.trap_code   = TrapCode::TRAP_ILLEGAL_OP;  // unused
        r.description = "Step limit reached after "
                      + std::to_string(steps) + " steps";
    }

    return r;
}

} // namespace vm
} // namespace sandbox

#endif // TERNARY_VM_H
