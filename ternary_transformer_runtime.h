// =============================================================================
// ternary_transformer_runtime.h - Phase 5B VM tensor/runtime routines
// =============================================================================
//
// Host-side runtime layer over VMState and the existing ISA semantics. This
// keeps tensors in DMEM, keeps lane/numeric crossings explicit, and adds no new
// transformer-specific opcodes.

#pragma once
#ifndef TERNARY_TRANSFORMER_RUNTIME_H
#define TERNARY_TRANSFORMER_RUNTIME_H

#include "ternary_vm.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sandbox {
namespace transformer_runtime {

struct TensorView {
    int base = 0;
    int rows = 0;
    int cols = 0;
    TernaryMode mode = TernaryMode::T50;
};

struct RuntimeStats {
    uint64_t scalarOps = 0;
    uint64_t dmemLoads = 0;
    uint64_t dmemStores = 0;
};

inline bool validTensor(const vm::VMState& state, TensorView view) {
    if (view.base < 0 || view.rows < 0 || view.cols < 0) return false;
    const long long cells = static_cast<long long>(view.rows) * static_cast<long long>(view.cols);
    return static_cast<long long>(view.base) + cells <= state.dmem.size();
}

inline int offsetOf(TensorView view, int row, int col) {
    return view.base + row * view.cols + col;
}

inline vm::TernaryValue asT50(vm::TernaryValue value) {
    return vm::convertValue(value, TernaryMode::T50);
}

inline vm::TernaryValue intValue(long long value, TernaryMode mode = TernaryMode::T50) {
    return vm::ops::fromLong(value, mode);
}

inline vm::TernaryValue ratioValue(long long numerator, long long denominator) {
    if (denominator == 0) return vm::TernaryValue::invalid(TernaryMode::T50);
    return vm::exec::divideValue(intValue(numerator), intValue(denominator), TernaryMode::T50);
}

inline vm::TernaryValue addT50(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::addValue(asT50(a), asT50(b), TernaryMode::T50);
}

inline vm::TernaryValue subT50(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::subtractValue(asT50(a), asT50(b), TernaryMode::T50);
}

inline vm::TernaryValue mulT50(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::multiplyValue(asT50(a), asT50(b), TernaryMode::T50);
}

inline vm::TernaryValue divT50(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    if (asT50(b).isZero()) return vm::TernaryValue::invalid(TernaryMode::T50);
    return vm::exec::divideValue(asT50(a), asT50(b), TernaryMode::T50);
}

inline vm::TernaryValue negT50(vm::TernaryValue value, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::negateValue(asT50(value), TernaryMode::T50);
}

inline vm::TernaryValue sqrtT50(vm::TernaryValue value, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::sqrtValue(asT50(value), TernaryMode::T50);
}

inline bool loadElement(
    const vm::VMState& state,
    TensorView view,
    int row,
    int col,
    vm::TernaryValue& out,
    RuntimeStats* stats = nullptr) {

    if (!validTensor(state, view) || row < 0 || row >= view.rows || col < 0 || col >= view.cols) return false;
    auto [value, fault] = state.dmem.load(offsetOf(view, row, col));
    if (stats) ++stats->dmemLoads;
    if (fault != vm::MemFaultCode::OK) return false;
    out = vm::convertValue(value, view.mode);
    return out.mode == view.mode && !out.isInvalid();
}

inline bool storeElement(
    vm::VMState& state,
    TensorView view,
    int row,
    int col,
    vm::TernaryValue value,
    RuntimeStats* stats = nullptr) {

    if (!validTensor(state, view) || row < 0 || row >= view.rows || col < 0 || col >= view.cols) return false;
    vm::TernaryValue converted = vm::convertValue(value, view.mode);
    if (converted.mode != view.mode || converted.isInvalid()) return false;
    if (stats) ++stats->dmemStores;
    return state.dmem.store(offsetOf(view, row, col), converted) == vm::MemFaultCode::OK;
}

inline vm::TernaryValue expT50(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue x = asT50(input);
    if (x.isInvalid()) return vm::TernaryValue::invalid(TernaryMode::T50);
    if (x.isZero()) return intValue(1);

    if (vm::exec::signValue(x, TernaryMode::T50) < 0) {
        vm::TernaryValue positive = expT50(negT50(x, stats), stats);
        return divT50(intValue(1), positive, stats);
    }

    const vm::TernaryValue three = intValue(3);
    int reductions = 0;
    while (vm::exec::compareValue(x, three, TernaryMode::T50) > 0 && reductions < 16) {
        x = divT50(x, three, stats);
        ++reductions;
    }

    vm::TernaryValue sum = intValue(1);
    vm::TernaryValue term = intValue(1);
    for (int k = 1; k <= 22; ++k) {
        term = divT50(mulT50(term, x, stats), intValue(k), stats);
        sum = addT50(sum, term, stats);
    }
    for (int i = 0; i < reductions; ++i) {
        sum = mulT50(mulT50(sum, sum, stats), sum, stats);
    }
    return sum;
}

inline vm::TernaryValue tanhT50(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue twoX = mulT50(intValue(2), input, stats);
    vm::TernaryValue e = expT50(twoX, stats);
    return divT50(subT50(e, intValue(1), stats), addT50(e, intValue(1), stats), stats);
}

inline vm::TernaryValue geluT50(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue x = asT50(input);
    vm::TernaryValue x2 = mulT50(x, x, stats);
    vm::TernaryValue x3 = mulT50(x2, x, stats);
    vm::TernaryValue inner = addT50(x, mulT50(ratioValue(44715, 1000000), x3, stats), stats);
    vm::TernaryValue shaped = mulT50(ratioValue(797885, 1000000), inner, stats);
    vm::TernaryValue gate = addT50(intValue(1), tanhT50(shaped, stats), stats);
    return mulT50(mulT50(ratioValue(1, 2), x, stats), gate, stats);
}

inline bool softmaxRow(
    vm::VMState& state,
    TensorView logits,
    int row,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (logits.cols != out.cols || logits.rows != out.rows || row < 0 || row >= logits.rows) return false;

    vm::TernaryValue maxValue;
    if (!loadElement(state, logits, row, 0, maxValue, stats)) return false;
    maxValue = asT50(maxValue);
    for (int col = 1; col < logits.cols; ++col) {
        vm::TernaryValue value;
        if (!loadElement(state, logits, row, col, value, stats)) return false;
        value = asT50(value);
        if (vm::exec::compareValue(value, maxValue, TernaryMode::T50) > 0) maxValue = value;
    }

    std::vector<vm::TernaryValue> exps(static_cast<std::size_t>(logits.cols));
    vm::TernaryValue sum = intValue(0);
    for (int col = 0; col < logits.cols; ++col) {
        vm::TernaryValue value;
        if (!loadElement(state, logits, row, col, value, stats)) return false;
        exps[static_cast<std::size_t>(col)] = expT50(subT50(value, maxValue, stats), stats);
        sum = addT50(sum, exps[static_cast<std::size_t>(col)], stats);
    }
    if (sum.isInvalid() || sum.isZero()) return false;

    for (int col = 0; col < logits.cols; ++col) {
        vm::TernaryValue probability = divT50(exps[static_cast<std::size_t>(col)], sum, stats);
        if (!storeElement(state, out, row, col, probability, stats)) return false;
    }
    return true;
}

inline bool softmaxRows(
    vm::VMState& state,
    TensorView logits,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (logits.rows != out.rows || logits.cols != out.cols) return false;
    for (int row = 0; row < logits.rows; ++row) {
        if (!softmaxRow(state, logits, row, out, stats)) return false;
    }
    return true;
}

inline bool matmulScalar(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols) return false;
    for (int row = 0; row < out.rows; ++row) {
        for (int col = 0; col < out.cols; ++col) {
            vm::TernaryValue sum = intValue(0);
            for (int k = 0; k < a.cols; ++k) {
                vm::TernaryValue av;
                vm::TernaryValue bv;
                if (!loadElement(state, a, row, k, av, stats)) return false;
                if (!loadElement(state, b, k, col, bv, stats)) return false;
                sum = addT50(sum, mulT50(av, bv, stats), stats);
            }
            if (!storeElement(state, out, row, col, sum, stats)) return false;
        }
    }
    return true;
}

inline bool matmulAccumulator(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols) return false;
    for (int row = 0; row < out.rows; ++row) {
        for (int col = 0; col < out.cols; ++col) {
            state.accumulator = intValue(0);
            for (int k = 0; k < a.cols; ++k) {
                vm::TernaryValue av;
                vm::TernaryValue bv;
                if (!loadElement(state, a, row, k, av, stats)) return false;
                if (!loadElement(state, b, k, col, bv, stats)) return false;
                state.accumulator = addT50(state.accumulator, mulT50(av, bv, stats), stats);
            }
            if (!storeElement(state, out, row, col, state.accumulator, stats)) return false;
        }
    }
    return true;
}

inline bool matmulT1Dot(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (a.mode != TernaryMode::L1 || b.mode != TernaryMode::L1) return false;
    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols) return false;

    const int oldLength = state.vector_length;
    state.vector_length = a.cols;
    state.vregfile.reset(state.vector_length);
    state.vector_faults.reset(state.vector_length);

    for (int row = 0; row < out.rows; ++row) {
        for (int col = 0; col < out.cols; ++col) {
            state.vector_faults.clear();
            for (int k = 0; k < a.cols; ++k) {
                vm::TernaryValue av;
                vm::TernaryValue bv;
                if (!loadElement(state, a, row, k, av, stats)) return false;
                if (!loadElement(state, b, k, col, bv, stats)) return false;
                state.vregfile.reg[0].write(k, av);
                state.vregfile.reg[1].write(k, bv);
            }
            vm::TernaryValue dot = vm::exec::vectorDotT1(state, 0, 1);
            if (stats) ++stats->scalarOps;
            if (state.vector_faults.any()) return false;
            if (!storeElement(state, out, row, col, dot, stats)) return false;
        }
    }

    state.vector_length = oldLength;
    state.vregfile.reset(state.vector_length);
    state.vector_faults.reset(state.vector_length);
    return true;
}

inline bool signActivationRows(
    vm::VMState& state,
    TensorView input,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (out.rows != input.rows || out.cols != input.cols || out.mode != TernaryMode::L1) return false;
    for (int row = 0; row < input.rows; ++row) {
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            vm::TernaryValue pred = vm::exec::makePredicateLane(vm::exec::signValue(value, value.mode));
            if (!storeElement(state, out, row, col, pred, stats)) return false;
        }
    }
    return true;
}

inline bool layerNormRows(
    vm::VMState& state,
    TensorView input,
    TensorView gamma,
    TensorView beta,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (input.rows != out.rows || input.cols != out.cols) return false;
    if (gamma.rows != 1 || beta.rows != 1 || gamma.cols != input.cols || beta.cols != input.cols) return false;
    const vm::TernaryValue invCols = divT50(intValue(1), intValue(input.cols), stats);
    const vm::TernaryValue eps = ratioValue(1, 1000);

    for (int row = 0; row < input.rows; ++row) {
        vm::TernaryValue mean = intValue(0);
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            mean = addT50(mean, value, stats);
        }
        mean = mulT50(mean, invCols, stats);

        vm::TernaryValue variance = intValue(0);
        std::vector<vm::TernaryValue> centered(static_cast<std::size_t>(input.cols));
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            centered[static_cast<std::size_t>(col)] = subT50(value, mean, stats);
            variance = addT50(
                variance,
                mulT50(centered[static_cast<std::size_t>(col)], centered[static_cast<std::size_t>(col)], stats),
                stats);
        }
        variance = mulT50(variance, invCols, stats);
        const vm::TernaryValue denom = sqrtT50(addT50(variance, eps, stats), stats);
        if (denom.isInvalid() || denom.isZero()) return false;

        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue scale;
            vm::TernaryValue bias;
            if (!loadElement(state, gamma, 0, col, scale, stats)) return false;
            if (!loadElement(state, beta, 0, col, bias, stats)) return false;
            vm::TernaryValue normalized = divT50(centered[static_cast<std::size_t>(col)], denom, stats);
            vm::TernaryValue shifted = addT50(mulT50(normalized, scale, stats), bias, stats);
            if (!storeElement(state, out, row, col, shifted, stats)) return false;
        }
    }
    return true;
}

} // namespace transformer_runtime
} // namespace sandbox

#endif // TERNARY_TRANSFORMER_RUNTIME_H
