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
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <sstream>

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
    return TernaryValue::fromLongTriple(sum);
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
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue idx = indices[static_cast<std::size_t>(lane)];
        if (!isNumericMode(idx.mode) || idx.isInvalid()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }
        const long long addrLong = base + ops::toLong(idx);
        int physical_addr = static_cast<int>(addrLong);
        int cause = OS_CAUSE_LOAD_FAULT;
        if (vm.privilege != PrivilegeMode::Kernel &&
            !vm.translateLoadAddress(static_cast<int>(addrLong), physical_addr, cause)) {
            vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
            return;
        }
        if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_MEM_FAULT, mode);
            continue;
        }
        auto [loaded, fc] = vm.dmem.load(physical_addr);
        if (fc != MemFaultCode::OK) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_MEM_FAULT, mode);
            continue;
        }
        TernaryValue converted;
        if (!convertVectorNumericLane(loaded, mode, converted)) {
            writeVectorFaultZero(vm, vd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
            continue;
        }
        vm.vregfile.reg[vd].write(lane, converted);
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
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        TernaryValue idx = indices[static_cast<std::size_t>(lane)];
        if (!isNumericMode(idx.mode) || idx.isInvalid()) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_ILLEGAL_OP);
            continue;
        }
        const long long addrLong = base + ops::toLong(idx);
        int physical_addr = static_cast<int>(addrLong);
        int cause = OS_CAUSE_STORE_FAULT;
        if (vm.privilege != PrivilegeMode::Kernel &&
            !vm.translateStoreAddress(static_cast<int>(addrLong), physical_addr, cause)) {
            vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
            return;
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
        if (vm.dmem.store(physical_addr, converted) != MemFaultCode::OK) {
            vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
        } else {
            vm.noteStoreForReservation(physical_addr);
        }
    }
}

} // namespace exec

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
inline VMStatus step(VMState& vm) {
    if (!vm.isRunning()) return vm.status;

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

    // -----------------------------------------------------------------
    // DECODE
    // -----------------------------------------------------------------
    InstructionWord iw = InstructionWord::decode(raw);
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
            vm.regfile.write(iw.rd, vm.regfile.read(iw.rs1));
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
            if (!writeChecked(iw.rd, exec::addValue(vm.regfile.read(iw.rs1),
                                                    vm.regfile.read(iw.rs2), mode))) return vm.status;
            break;
        }

        case Opcode::SUB: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::subtractValue(vm.regfile.read(iw.rs1),
                                                         vm.regfile.read(iw.rs2), mode))) return vm.status;
            break;
        }

        case Opcode::MUL: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::multiplyValue(vm.regfile.read(iw.rs1),
                                                         vm.regfile.read(iw.rs2), mode))) return vm.status;
            break;
        }

        case Opcode::DIV: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue b = convertValue(vm.regfile.read(iw.rs2), mode);
            if (b.isZero()) {
                vm.trapWithCause(TrapCode::TRAP_DIV_ZERO, OS_CAUSE_DIV_ZERO, vm.pc);
                return vm.status;
            }
            if (!writeChecked(iw.rd, exec::divideValue(vm.regfile.read(iw.rs1), b, mode))) return vm.status;
            break;
        }

        case Opcode::SQRT: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue t = convertValue(vm.regfile.read(iw.rs1), mode);
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
            if (!writeChecked(iw.rd, exec::negateValue(vm.regfile.read(iw.rs1), mode))) return vm.status;
            break;
        }

        case Opcode::ABS: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::absValue(vm.regfile.read(iw.rs1), mode))) return vm.status;
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
            int8_t cmp = exec::compareValue(vm.regfile.read(iw.rs1),
                                            vm.regfile.read(iw.rs2), mode);
            vm.regfile.write(iw.rd, makeTritResult(cmp));
            break;
        }

        case Opcode::TMIN: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue b = convertValue(vm.regfile.read(iw.rs2), mode);
            if (!writeChecked(iw.rd, exec::compareValue(a, b, mode) == T_POS ? b : a)) return vm.status;
            break;
        }

        case Opcode::TMAX: {
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            TernaryValue a = convertValue(vm.regfile.read(iw.rs1), mode);
            TernaryValue b = convertValue(vm.regfile.read(iw.rs2), mode);
            if (!writeChecked(iw.rd, exec::compareValue(a, b, mode) == T_NEG ? b : a)) return vm.status;
            break;
        }

        case Opcode::TINV: {
            // Alias for NEG: trit-flip all mantissa trits. TINV(TINV(x)) == x.
            auto [ok, mode] = decodeWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::negateValue(vm.regfile.read(iw.rs1), mode))) return vm.status;
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
                    vm.regfile.read(iw.rs1), vm.regfile.read(iw.rs2), mode, op))) return vm.status;
            break;
        }

        case Opcode::TLNEG: {
            auto [ok, mode] = decodeLaneWidth(); if (!ok) return vm.status;
            if (!writeChecked(iw.rd, exec::laneUnaryValue(
                    vm.regfile.read(iw.rs1), mode, exec::LaneOp::Neg))) return vm.status;
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
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                const long long addrLong = base + iw.imm + lane;
                int physical_addr = static_cast<int>(addrLong);
                int cause = OS_CAUSE_LOAD_FAULT;
                if (vm.privilege != PrivilegeMode::Kernel &&
                    !vm.translateLoadAddress(static_cast<int>(addrLong), physical_addr, cause)) {
                    vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                     cause,
                                     vm.pc);
                    return vm.status;
                }
                if (physical_addr < 0 || physical_addr >= vm.dmem.size()) {
                    exec::writeVectorFaultZero(vm, iw.rd, lane, TrapCode::TRAP_MEM_FAULT, mode);
                    continue;
                }
                auto [loaded, fc] = vm.dmem.load(physical_addr);
                if (fc != MemFaultCode::OK) {
                    exec::writeVectorFaultZero(vm, iw.rd, lane, TrapCode::TRAP_MEM_FAULT, mode);
                    continue;
                }
                TernaryValue converted;
                if (!exec::convertVectorNumericLane(loaded, mode, converted)) {
                    exec::writeVectorFaultZero(vm, iw.rd, lane, TrapCode::TRAP_ILLEGAL_OP, mode);
                    continue;
                }
                vm.vregfile.reg[iw.rd].write(lane, converted);
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
            for (int lane = 0; lane < vm.vector_length; ++lane) {
                const long long addrLong = base + iw.imm + lane;
                int physical_addr = static_cast<int>(addrLong);
                int cause = OS_CAUSE_STORE_FAULT;
                if (vm.privilege != PrivilegeMode::Kernel &&
                    !vm.translateStoreAddress(static_cast<int>(addrLong), physical_addr, cause)) {
                    vm.trapWithCause(TrapCode::TRAP_MEM_FAULT,
                                     cause,
                                     vm.pc);
                    return vm.status;
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
                MemFaultCode fc = vm.dmem.store(physical_addr, converted);
                if (fc != MemFaultCode::OK) {
                    vm.vector_faults.setLane(lane, TrapCode::TRAP_MEM_FAULT);
                } else {
                    vm.noteStoreForReservation(physical_addr);
                }
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
            if (!isNumericMode(vm.regfile.read(iw.rs1).mode)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            long long base   = ops::toLong(vm.regfile.read(iw.rs1));
            int       addr   = static_cast<int>(base + iw.imm);
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
            if (!isNumericMode(vm.regfile.read(iw.rs1).mode)) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            long long base = ops::toLong(vm.regfile.read(iw.rs1));
            int       addr = static_cast<int>(base + iw.imm);
            int physical_addr = addr;
            int cause = OS_CAUSE_STORE_FAULT;
            if (!vm.translateStoreAddress(addr, physical_addr, cause)) {
                vm.trapWithCause(TrapCode::TRAP_MEM_FAULT, cause, vm.pc);
                return vm.status;
            }
            MemFaultCode fc = vm.dmem.store(physical_addr, vm.regfile.read(iw.rs_store));
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
            if (!isNumericMode(addrValue.mode) || addrValue.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            const int addr = static_cast<int>(ops::toLong(addrValue));
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
            if (!isNumericMode(addrValue.mode) || addrValue.isInvalid()) {
                vm.trap(TrapCode::TRAP_ILLEGAL_OP);
                return vm.status;
            }
            const int addr = static_cast<int>(ops::toLong(addrValue));
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
            break;
        }

        case Opcode::BRZ: {
            vm.branch_instructions_count++;
            int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            if (trit0 == T_ZER) {
                pc_next = vm.pc + iw.offset;
            }
            break;
        }

        case Opcode::BRP: {
            vm.branch_instructions_count++;
            int8_t trit0 = readTrit0(vm.regfile.read(iw.rs_branch));
            if (trit0 == T_POS) {
                pc_next = vm.pc + iw.offset;
            }
            break;
        }

        case Opcode::CALL: {
            // B-type: r25 (LR) ← PC + 1; PC ← PC + offset19
            vm.regfile.writeLR(sandbox::vm::ops::fromLong(vm.pc + 1));
            pc_next = vm.pc + iw.offset;
            break;
        }

        case Opcode::RET: {
            // R-type (no operands): PC ← r25 (LR)
            int ret_addr = exec::pcFromValue(vm.regfile.readLR());
            if (!vm.validateControlTarget(ret_addr)) {
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            pc_next = ret_addr;
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
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            vm.regfile.writeLR(sandbox::vm::ops::fromLong(vm.pc + 1));
            pc_next = dest;
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
                vm.trap(TrapCode::TRAP_MEM_FAULT);
                return vm.status;
            }
            pc_next = dest;
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

    Hook onStep;
    Hook onTrap;
    Hook onHalt;
};

// Execute one instruction and notify hooks after the architectural state for
// that instruction has been committed. Terminal hooks fire after onStep.
inline VMStatus step(VMState& vm, const VMHooks& hooks) {
    if (!vm.isRunning()) return vm.status;

    const int pc_before = vm.pc;
    const VMStatus before = vm.status;
    const VMStatus after = step(vm);

    if (hooks.onStep) hooks.onStep(vm, pc_before);
    if (before == VMStatus::RUNNING && after == VMStatus::TRAPPED && hooks.onTrap) {
        hooks.onTrap(vm, pc_before);
    }
    if (before == VMStatus::RUNNING && after == VMStatus::HALTED && hooks.onHalt) {
        hooks.onHalt(vm, pc_before);
    }

    return after;
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
    while (vm.isRunning()) {
        if (max_steps >= 0 && steps >= max_steps) break;
        if (hooks) step(vm, *hooks);
        else step(vm);
        ++steps;
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
