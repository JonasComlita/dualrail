// =============================================================================
// ternary_transformer_runtime.h - Phase 5D VM tensor/runtime routines
// =============================================================================
//
// Host-side runtime layer over VMState and generated VM programs. This keeps
// tensors in DMEM, keeps lane/numeric crossings explicit, and adds no new
// transformer-specific opcodes.

#pragma once
#ifndef TERNARY_TRANSFORMER_RUNTIME_H
#define TERNARY_TRANSFORMER_RUNTIME_H

#include "ternary_vm.h"
#include "ternary_ir.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sandbox {
namespace transformer_runtime {

struct TensorView {
    int base = 0;
    int rows = 0;
    int cols = 0;
    TernaryMode mode = TernaryMode::T40;
};

struct RuntimeStats {
    uint64_t scalarOps = 0;
    uint64_t dmemLoads = 0;
    uint64_t dmemStores = 0;
    uint64_t generatedKernels = 0;
    uint64_t generatedAssemblyWords = 0;
    uint64_t vmSteps = 0;
    uint64_t runtimeUs = 0;
};

struct GeneratedKernelResult {
    bool ok = false;
    std::string name;
    std::string assembly;
    std::vector<std::string> diagnostics;
    int assemblyWords = 0;
    int vmSteps = 0;
    long long runtimeUs = 0;
    std::string status;
};

inline bool validTensor(const vm::VMState& state, TensorView view) {
    if (view.base < 0 || view.rows < 0 || view.cols < 0) return false;
    const long long cells = static_cast<long long>(view.rows) * static_cast<long long>(view.cols);
    return static_cast<long long>(view.base) + cells <= state.dmem.size();
}

inline int offsetOf(TensorView view, int row, int col) {
    return view.base + row * view.cols + col;
}

inline vm::TernaryValue asT40(vm::TernaryValue value) {
    return vm::convertValue(value, TernaryMode::T40);
}

inline vm::TernaryValue intValue(long long value, TernaryMode mode = TernaryMode::T40) {
    return vm::ops::fromLong(value, mode);
}

inline vm::TernaryValue ratioValue(long long numerator, long long denominator) {
    if (denominator == 0) return vm::TernaryValue::invalid(TernaryMode::T40);
    return vm::exec::divideValue(intValue(numerator), intValue(denominator), TernaryMode::T40);
}

inline vm::TernaryValue addT40(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::addValue(asT40(a), asT40(b), TernaryMode::T40);
}

inline vm::TernaryValue subT40(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::subtractValue(asT40(a), asT40(b), TernaryMode::T40);
}

inline vm::TernaryValue mulT40(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::multiplyValue(asT40(a), asT40(b), TernaryMode::T40);
}

inline vm::TernaryValue divT40(vm::TernaryValue a, vm::TernaryValue b, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    if (asT40(b).isZero()) return vm::TernaryValue::invalid(TernaryMode::T40);
    return vm::exec::divideValue(asT40(a), asT40(b), TernaryMode::T40);
}

inline vm::TernaryValue negT40(vm::TernaryValue value, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::negateValue(asT40(value), TernaryMode::T40);
}

inline vm::TernaryValue sqrtT40(vm::TernaryValue value, RuntimeStats* stats = nullptr) {
    if (stats) ++stats->scalarOps;
    return vm::exec::sqrtValue(asT40(value), TernaryMode::T40);
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

inline sandbox::ir::Type irTypeForMode(TernaryMode mode) {
    switch (mode) {
        case TernaryMode::T1: return sandbox::ir::Type::T1;
        case TernaryMode::T5: return sandbox::ir::Type::T5;
        case TernaryMode::T10: return sandbox::ir::Type::T10;
        case TernaryMode::T20: return sandbox::ir::Type::T20;
        case TernaryMode::T40: return sandbox::ir::Type::T40;
        case TernaryMode::T50: return sandbox::ir::Type::T50;
        case TernaryMode::L1: return sandbox::ir::Type::L1;
        case TernaryMode::L5: return sandbox::ir::Type::L5;
        case TernaryMode::L10: return sandbox::ir::Type::L10;
        case TernaryMode::L20: return sandbox::ir::Type::L20;
        case TernaryMode::L40: return sandbox::ir::Type::L40;
        case TernaryMode::L50: return sandbox::ir::Type::L50;
    }
    return sandbox::ir::Type::T40;
}

inline void addKernelStats(
    RuntimeStats* stats,
    const GeneratedKernelResult& result,
    uint64_t loads,
    uint64_t stores) {

    if (!stats) return;
    ++stats->generatedKernels;
    stats->generatedAssemblyWords += static_cast<uint64_t>(result.assemblyWords);
    stats->vmSteps += static_cast<uint64_t>(result.vmSteps);
    stats->runtimeUs += static_cast<uint64_t>(result.runtimeUs);
    stats->dmemLoads += loads;
    stats->dmemStores += stores;
}

template<typename Setup>
inline GeneratedKernelResult executeGeneratedKernel(
    vm::VMState& state,
    const std::string& name,
    const sandbox::ir::Program& program,
    int maxSteps,
    Setup setup) {

    GeneratedKernelResult result;
    result.name = name;
    const auto lowered = program.lower();
    result.assembly = lowered.assembly;
    result.diagnostics = lowered.diagnostics;
    if (!lowered.success) {
        result.status = "lower/assemble failed";
        return result;
    }

    result.assemblyWords = static_cast<int>(lowered.assembled.program.size());
    state.reset();
    state.imem.reset();
    if (!state.imem.loadProgram(lowered.assembled.program, 0)) {
        result.status = "program load failed";
        return result;
    }

    setup(state);
    const auto start = std::chrono::high_resolution_clock::now();
    const vm::RunResult run = vm::run(state, maxSteps);
    const auto end = std::chrono::high_resolution_clock::now();
    result.vmSteps = run.steps;
    result.runtimeUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    result.status = run.description;
    result.ok = run.halted();
    return result;
}

inline GeneratedKernelResult executeGeneratedKernel(
    vm::VMState& state,
    const std::string& name,
    const sandbox::ir::Program& program,
    int maxSteps = 100000) {

    return executeGeneratedKernel(state, name, program, maxSteps, [](vm::VMState&) {});
}

inline sandbox::ir::Value buildExpPositiveValue(
    sandbox::ir::Program& program,
    sandbox::ir::Value x) {

    using namespace sandbox::ir;
    Value sum = program.constant(Type::T40, 1);
    Value term = program.constant(Type::T40, 1);
    for (int k = 1; k <= 22; ++k) {
        Value numerator = program.mul(term, x);
        Value denom = program.constant(Type::T40, k);
        Value nextTerm = program.div(numerator, denom);
        Value nextSum = program.add(sum, nextTerm);
        program.release(numerator);
        program.release(denom);
        program.release(term);
        program.release(sum);
        term = nextTerm;
        sum = nextSum;
    }
    program.release(term);
    return sum;
}

inline sandbox::ir::Program buildExpScalarProgram(int inputAddr, int outputAddr) {
    using namespace sandbox::ir;
    Program program;
    Value inBase = program.constant(Type::T40, inputAddr);
    Value outBase = program.constant(Type::T40, outputAddr);
    Value x = program.load(Type::T40, inBase, 0);
    Value zero = program.constant(Type::T40, 0);
    Value cond = program.cmp(x, zero);
    program.brn(cond, "negative_input");

    Value positive = buildExpPositiveValue(program, x);
    program.store(positive, outBase, 0);
    program.release(positive);
    program.jmp("done");

    program.label("negative_input");
    Value mag = program.neg(x);
    Value positiveMag = buildExpPositiveValue(program, mag);
    Value one = program.constant(Type::T40, 1);
    Value reciprocal = program.div(one, positiveMag);
    program.store(reciprocal, outBase, 0);
    program.release(mag);
    program.release(positiveMag);
    program.release(one);
    program.release(reciprocal);

    program.label("done");
    program.halt();
    return program;
}

inline GeneratedKernelResult expScalarGenerated(
    vm::VMState& state,
    int inputAddr,
    int outputAddr,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "exp.T40 scalar",
        buildExpScalarProgram(inputAddr, outputAddr),
        4096);
    addKernelStats(stats, result, 1, 1);
    return result;
}

inline sandbox::ir::Program buildTanhFromExpProgram(int expAddr, int outputAddr) {
    using namespace sandbox::ir;
    Program program;
    Value expBase = program.constant(Type::T40, expAddr);
    Value outBase = program.constant(Type::T40, outputAddr);
    Value e = program.load(Type::T40, expBase, 0);
    Value oneA = program.constant(Type::T40, 1);
    Value numerator = program.sub(e, oneA);
    Value oneB = program.constant(Type::T40, 1);
    Value denominator = program.add(e, oneB);
    Value result = program.div(numerator, denominator);
    program.store(result, outBase, 0);
    program.halt();
    return program;
}

inline sandbox::ir::Program buildTanhPrepProgram(int inputAddr, int scratchAddr) {
    using namespace sandbox::ir;
    Program program;
    Value inBase = program.constant(Type::T40, inputAddr);
    Value scratchBase = program.constant(Type::T40, scratchAddr);
    Value x = program.load(Type::T40, inBase, 0);
    Value two = program.constant(Type::T40, 2);
    Value twoX = program.mul(two, x);
    program.store(twoX, scratchBase, 0);
    program.halt();
    return program;
}

inline GeneratedKernelResult tanhScalarGenerated(
    vm::VMState& state,
    int inputAddr,
    int outputAddr,
    int scratchAddr,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult prep = executeGeneratedKernel(
        state,
        "tanh.T40 prep",
        buildTanhPrepProgram(inputAddr, scratchAddr),
        128);
    addKernelStats(stats, prep, 1, 1);
    if (!prep.ok) return prep;

    GeneratedKernelResult exp = expScalarGenerated(state, scratchAddr, scratchAddr + 1, stats);
    if (!exp.ok) return exp;

    GeneratedKernelResult finish = executeGeneratedKernel(
        state,
        "tanh.T40 finish",
        buildTanhFromExpProgram(scratchAddr + 1, outputAddr),
        128);
    addKernelStats(stats, finish, 1, 1);
    return finish;
}

inline sandbox::ir::Program buildGeluPrepProgram(int inputAddr, int scratchAddr) {
    using namespace sandbox::ir;
    Program program;
    Value inBase = program.constant(Type::T40, inputAddr);
    Value scratchBase = program.constant(Type::T40, scratchAddr);
    Value x = program.load(Type::T40, inBase, 0);
    Value x2 = program.mul(x, x);
    Value x3 = program.mul(x2, x);
    Value c1n = program.constant(Type::T40, 44715);
    Value c1d = program.constant(Type::T40, 1000000);
    Value c1 = program.div(c1n, c1d);
    Value cubic = program.mul(c1, x3);
    Value inner = program.add(x, cubic);
    Value c2n = program.constant(Type::T40, 797885);
    Value c2d = program.constant(Type::T40, 1000000);
    Value c2 = program.div(c2n, c2d);
    Value shaped = program.mul(c2, inner);
    program.store(shaped, scratchBase, 0);
    program.halt();
    return program;
}

inline sandbox::ir::Program buildGeluFinishProgram(int inputAddr, int tanhAddr, int outputAddr) {
    using namespace sandbox::ir;
    Program program;
    Value inBase = program.constant(Type::T40, inputAddr);
    Value tanhBase = program.constant(Type::T40, tanhAddr);
    Value outBase = program.constant(Type::T40, outputAddr);
    Value x = program.load(Type::T40, inBase, 0);
    Value gateRaw = program.load(Type::T40, tanhBase, 0);
    Value one = program.constant(Type::T40, 1);
    Value gate = program.add(one, gateRaw);
    Value halfN = program.constant(Type::T40, 1);
    Value halfD = program.constant(Type::T40, 2);
    Value half = program.div(halfN, halfD);
    Value halfX = program.mul(half, x);
    Value result = program.mul(halfX, gate);
    program.store(result, outBase, 0);
    program.halt();
    return program;
}

inline GeneratedKernelResult geluScalarGenerated(
    vm::VMState& state,
    int inputAddr,
    int outputAddr,
    int scratchAddr,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult prep = executeGeneratedKernel(
        state,
        "gelu.T40 prep",
        buildGeluPrepProgram(inputAddr, scratchAddr),
        256);
    addKernelStats(stats, prep, 1, 1);
    if (!prep.ok) return prep;

    GeneratedKernelResult tanh = tanhScalarGenerated(state, scratchAddr, scratchAddr + 1, scratchAddr + 2, stats);
    if (!tanh.ok) return tanh;

    GeneratedKernelResult finish = executeGeneratedKernel(
        state,
        "gelu.T40 finish",
        buildGeluFinishProgram(inputAddr, scratchAddr + 1, outputAddr),
        256);
    addKernelStats(stats, finish, 2, 1);
    return finish;
}

inline vm::TernaryValue expT40(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue x = asT40(input);
    if (x.isInvalid()) return vm::TernaryValue::invalid(TernaryMode::T40);
    if (x.isZero()) return intValue(1);

    if (vm::exec::signValue(x, TernaryMode::T40) < 0) {
        vm::TernaryValue positive = expT40(negT40(x, stats), stats);
        return divT40(intValue(1), positive, stats);
    }

    const vm::TernaryValue three = intValue(3);
    int reductions = 0;
    while (vm::exec::compareValue(x, three, TernaryMode::T40) > 0 && reductions < 16) {
        x = divT40(x, three, stats);
        ++reductions;
    }

    vm::TernaryValue sum = intValue(1);
    vm::TernaryValue term = intValue(1);
    for (int k = 1; k <= 22; ++k) {
        term = divT40(mulT40(term, x, stats), intValue(k), stats);
        sum = addT40(sum, term, stats);
    }
    for (int i = 0; i < reductions; ++i) {
        sum = mulT40(mulT40(sum, sum, stats), sum, stats);
    }
    return sum;
}

inline vm::TernaryValue tanhT40(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue twoX = mulT40(intValue(2), input, stats);
    vm::TernaryValue e = expT40(twoX, stats);
    return divT40(subT40(e, intValue(1), stats), addT40(e, intValue(1), stats), stats);
}

inline vm::TernaryValue geluT40(vm::TernaryValue input, RuntimeStats* stats = nullptr) {
    vm::TernaryValue x = asT40(input);
    vm::TernaryValue x2 = mulT40(x, x, stats);
    vm::TernaryValue x3 = mulT40(x2, x, stats);
    vm::TernaryValue inner = addT40(x, mulT40(ratioValue(44715, 1000000), x3, stats), stats);
    vm::TernaryValue shaped = mulT40(ratioValue(797885, 1000000), inner, stats);
    vm::TernaryValue gate = addT40(intValue(1), tanhT40(shaped, stats), stats);
    return mulT40(mulT40(ratioValue(1, 2), x, stats), gate, stats);
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
    maxValue = asT40(maxValue);
    for (int col = 1; col < logits.cols; ++col) {
        vm::TernaryValue value;
        if (!loadElement(state, logits, row, col, value, stats)) return false;
        value = asT40(value);
        if (vm::exec::compareValue(value, maxValue, TernaryMode::T40) > 0) maxValue = value;
    }

    std::vector<vm::TernaryValue> exps(static_cast<std::size_t>(logits.cols));
    vm::TernaryValue sum = intValue(0);
    for (int col = 0; col < logits.cols; ++col) {
        vm::TernaryValue value;
        if (!loadElement(state, logits, row, col, value, stats)) return false;
        exps[static_cast<std::size_t>(col)] = expT40(subT40(value, maxValue, stats), stats);
        sum = addT40(sum, exps[static_cast<std::size_t>(col)], stats);
    }
    if (sum.isInvalid() || sum.isZero()) return false;

    for (int col = 0; col < logits.cols; ++col) {
        vm::TernaryValue probability = divT40(exps[static_cast<std::size_t>(col)], sum, stats);
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
                sum = addT40(sum, mulT40(av, bv, stats), stats);
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
                state.accumulator = addT40(state.accumulator, mulT40(av, bv, stats), stats);
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
    const vm::TernaryValue invCols = divT40(intValue(1), intValue(input.cols), stats);
    const vm::TernaryValue eps = ratioValue(1, 1000);

    for (int row = 0; row < input.rows; ++row) {
        vm::TernaryValue mean = intValue(0);
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            mean = addT40(mean, value, stats);
        }
        mean = mulT40(mean, invCols, stats);

        vm::TernaryValue variance = intValue(0);
        std::vector<vm::TernaryValue> centered(static_cast<std::size_t>(input.cols));
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            centered[static_cast<std::size_t>(col)] = subT40(value, mean, stats);
            variance = addT40(
                variance,
                mulT40(centered[static_cast<std::size_t>(col)], centered[static_cast<std::size_t>(col)], stats),
                stats);
        }
        variance = mulT40(variance, invCols, stats);
        const vm::TernaryValue denom = sqrtT40(addT40(variance, eps, stats), stats);
        if (denom.isInvalid() || denom.isZero()) return false;

        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue scale;
            vm::TernaryValue bias;
            if (!loadElement(state, gamma, 0, col, scale, stats)) return false;
            if (!loadElement(state, beta, 0, col, bias, stats)) return false;
            vm::TernaryValue normalized = divT40(centered[static_cast<std::size_t>(col)], denom, stats);
            vm::TernaryValue shifted = addT40(mulT40(normalized, scale, stats), bias, stats);
            if (!storeElement(state, out, row, col, shifted, stats)) return false;
        }
    }
    return true;
}

inline bool rmsNormRows(
    vm::VMState& state,
    TensorView input,
    TensorView gamma,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    if (input.rows != out.rows || input.cols != out.cols) return false;
    if (gamma.rows != 1 || gamma.cols != input.cols) return false;
    const vm::TernaryValue invCols = divT40(intValue(1), intValue(input.cols), stats);
    const vm::TernaryValue eps = ratioValue(1, 1000);

    for (int row = 0; row < input.rows; ++row) {
        vm::TernaryValue meanSquare = intValue(0);
        std::vector<vm::TernaryValue> values(static_cast<std::size_t>(input.cols));
        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue value;
            if (!loadElement(state, input, row, col, value, stats)) return false;
            values[static_cast<std::size_t>(col)] = value;
            meanSquare = addT40(meanSquare, mulT40(value, value, stats), stats);
        }
        meanSquare = mulT40(meanSquare, invCols, stats);
        const vm::TernaryValue denom = sqrtT40(addT40(meanSquare, eps, stats), stats);
        if (denom.isInvalid() || denom.isZero()) return false;

        for (int col = 0; col < input.cols; ++col) {
            vm::TernaryValue scale;
            if (!loadElement(state, gamma, 0, col, scale, stats)) return false;
            vm::TernaryValue normalized = divT40(values[static_cast<std::size_t>(col)], denom, stats);
            vm::TernaryValue shifted = mulT40(normalized, scale, stats);
            if (!storeElement(state, out, row, col, shifted, stats)) return false;
        }
    }
    return true;
}

inline sandbox::ir::Value loadElementIrT40(
    sandbox::ir::Program& program,
    TensorView view,
    int row,
    int col) {

    using namespace sandbox::ir;
    Value base = program.constant(Type::T40, view.base);
    Value loaded = program.load(irTypeForMode(view.mode), base, offsetOf(view, row, col) - view.base);
    program.release(base);
    if (view.mode == TernaryMode::T40) return loaded;
    Value widened = program.cvt(loaded, Type::T40);
    program.release(loaded);
    return widened;
}

inline void storeElementIr(
    sandbox::ir::Program& program,
    TensorView view,
    int row,
    int col,
    sandbox::ir::Value value) {

    using namespace sandbox::ir;
    Value base = program.constant(Type::T40, view.base);
    Value stored = value;
    if (view.mode != TernaryMode::T40) stored = program.cvt(value, irTypeForMode(view.mode));
    program.store(stored, base, offsetOf(view, row, col) - view.base);
    if (stored.reg != value.reg || stored.vector != value.vector) program.release(stored);
    program.release(base);
}

inline sandbox::ir::Program buildSoftmaxRowsProgram(TensorView logits, TensorView out) {
    using namespace sandbox::ir;
    Program program;
    for (int row = 0; row < logits.rows; ++row) {
        std::vector<Value> values;
        values.reserve(static_cast<std::size_t>(logits.cols));
        for (int col = 0; col < logits.cols; ++col) {
            values.push_back(loadElementIrT40(program, logits, row, col));
        }

        Value maxValue = values[0];
        bool maxIsOwned = false;
        for (int col = 1; col < logits.cols; ++col) {
            Value next = program.max(maxValue, values[static_cast<std::size_t>(col)]);
            if (maxIsOwned) program.release(maxValue);
            maxValue = next;
            maxIsOwned = true;
        }

        Value sum = program.constant(Type::T40, 0);
        for (int col = 0; col < logits.cols; ++col) {
            Value diff = program.sub(values[static_cast<std::size_t>(col)], maxValue);
            Value magnitude = program.neg(diff);
            Value expMagnitude = buildExpPositiveValue(program, magnitude);
            Value one = program.constant(Type::T40, 1);
            Value probNumerator = program.div(one, expMagnitude);
            storeElementIr(program, out, row, col, probNumerator);
            Value nextSum = program.add(sum, probNumerator);
            program.release(diff);
            program.release(magnitude);
            program.release(expMagnitude);
            program.release(one);
            program.release(probNumerator);
            program.release(sum);
            program.release(values[static_cast<std::size_t>(col)]);
            sum = nextSum;
        }
        if (maxIsOwned) program.release(maxValue);

        for (int col = 0; col < out.cols; ++col) {
            Value expValue = loadElementIrT40(program, out, row, col);
            Value probability = program.div(expValue, sum);
            storeElementIr(program, out, row, col, probability);
            program.release(expValue);
            program.release(probability);
        }
        program.release(sum);
    }
    program.halt();
    return program;
}

inline GeneratedKernelResult softmaxRowsGenerated(
    vm::VMState& state,
    TensorView logits,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult failed;
    failed.name = "softmax rows";
    if (logits.rows != out.rows || logits.cols != out.cols ||
        !validTensor(state, logits) || !validTensor(state, out) ||
        !vm::isNumericMode(logits.mode) || !vm::isNumericMode(out.mode)) {
        failed.status = "invalid tensor shape or mode";
        return failed;
    }

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "softmax rows",
        buildSoftmaxRowsProgram(logits, out),
        200000);
    const uint64_t cols = static_cast<uint64_t>(logits.cols);
    const uint64_t rows = static_cast<uint64_t>(logits.rows);
    addKernelStats(stats, result, rows * (cols * 2 + cols), rows * (cols * 2));
    return result;
}

inline sandbox::ir::Program buildMatmulProgram(
    TensorView a,
    TensorView b,
    TensorView out,
    bool useAccumulator) {

    using namespace sandbox::ir;
    Program program;
    for (int row = 0; row < out.rows; ++row) {
        for (int col = 0; col < out.cols; ++col) {
            if (useAccumulator) {
                program.aclr(Type::T40);
            }
            Value sum = program.constant(Type::T40, 0);
            for (int k = 0; k < a.cols; ++k) {
                Value av = loadElementIrT40(program, a, row, k);
                Value bv = loadElementIrT40(program, b, k, col);
                Value product = program.mul(av, bv);
                if (useAccumulator) {
                    program.aadd(product);
                    program.release(sum);
                    sum = program.astore(Type::T40);
                } else {
                    Value next = program.add(sum, product);
                    program.release(sum);
                    sum = next;
                }
                program.release(av);
                program.release(bv);
                program.release(product);
            }
            storeElementIr(program, out, row, col, sum);
            program.release(sum);
        }
    }
    program.halt();
    return program;
}

inline GeneratedKernelResult matmulScalarGenerated(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult failed;
    failed.name = "matmul scalar";
    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols ||
        !validTensor(state, a) || !validTensor(state, b) || !validTensor(state, out) ||
        !vm::isNumericMode(a.mode) || !vm::isNumericMode(b.mode) || !vm::isNumericMode(out.mode)) {
        failed.status = "invalid tensor shape or mode";
        return failed;
    }

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "matmul scalar",
        buildMatmulProgram(a, b, out, false),
        200000);
    const uint64_t cells = static_cast<uint64_t>(out.rows) * static_cast<uint64_t>(out.cols);
    addKernelStats(stats, result, cells * static_cast<uint64_t>(a.cols) * 2, cells);
    return result;
}

inline GeneratedKernelResult matmulAccumulatorGenerated(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult failed;
    failed.name = "matmul accumulator";
    if (a.cols != b.rows || out.rows != a.rows || out.cols != b.cols ||
        !validTensor(state, a) || !validTensor(state, b) || !validTensor(state, out) ||
        !vm::isNumericMode(a.mode) || !vm::isNumericMode(b.mode) || !vm::isNumericMode(out.mode)) {
        failed.status = "invalid tensor shape or mode";
        return failed;
    }

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "matmul accumulator",
        buildMatmulProgram(a, b, out, true),
        200000);
    const uint64_t cells = static_cast<uint64_t>(out.rows) * static_cast<uint64_t>(out.cols);
    addKernelStats(stats, result, cells * static_cast<uint64_t>(a.cols) * 2, cells);
    return result;
}

inline sandbox::ir::Program buildT1DotStoreProgram(TensorView out, int row, int col, bool useVmac) {
    using namespace sandbox::ir;
    Program program;
    Value base = program.constant(Type::T40, out.base);
    Value lhs = program.vparam(Type::L1);
    Value rhs = program.vparam(Type::L1);
    Value dot;
    if (useVmac) {
        program.aclr(Type::T40);
        program.vmacT1(lhs, rhs);
        dot = program.astore(Type::T40);
    } else {
        dot = program.vdotT1(lhs, rhs);
    }
    program.store(dot, base, offsetOf(out, row, col) - out.base);
    program.halt();
    return program;
}

inline GeneratedKernelResult matmulT1DotGenerated(
    vm::VMState& state,
    TensorView a,
    TensorView b,
    TensorView out,
    RuntimeStats* stats = nullptr,
    bool useVmac = false) {

    GeneratedKernelResult last;
    last.name = useVmac ? "matmul T1 vmac" : "matmul T1 vdot";
    if (a.mode != TernaryMode::L1 || b.mode != TernaryMode::L1 ||
        a.cols != b.rows || out.rows != a.rows || out.cols != b.cols ||
        !validTensor(state, a) || !validTensor(state, b) || !validTensor(state, out)) {
        last.status = "invalid tensor shape or mode";
        return last;
    }

    for (int row = 0; row < out.rows; ++row) {
        for (int col = 0; col < out.cols; ++col) {
            sandbox::ir::Program program = buildT1DotStoreProgram(out, row, col, useVmac);
            const auto lowered = program.lower();
            if (!lowered.success) {
                last.assembly = lowered.assembly;
                last.diagnostics = lowered.diagnostics;
                last.status = "lower/assemble failed";
                return last;
            }
            const int lhsReg = 0;
            const int rhsReg = 1;
            last = executeGeneratedKernel(
                state,
                useVmac ? "matmul T1 vmac" : "matmul T1 vdot",
                program,
                256,
                [&](vm::VMState& vmState) {
                    vmState.vector_length = a.cols;
                    vmState.vregfile.reset(vmState.vector_length);
                    vmState.vector_faults.reset(vmState.vector_length);
                    for (int k = 0; k < a.cols; ++k) {
                        vm::TernaryValue av;
                        vm::TernaryValue bv;
                        (void)loadElement(vmState, a, row, k, av, nullptr);
                        (void)loadElement(vmState, b, k, col, bv, nullptr);
                        vmState.vregfile.reg[static_cast<std::size_t>(lhsReg)].write(k, av);
                        vmState.vregfile.reg[static_cast<std::size_t>(rhsReg)].write(k, bv);
                    }
                });
            addKernelStats(stats, last, static_cast<uint64_t>(a.cols) * 2, 1);
            if (!last.ok) return last;
        }
    }
    return last;
}

inline sandbox::ir::Program buildLayerNormRowsProgram(
    TensorView input,
    TensorView gamma,
    TensorView beta,
    TensorView out) {

    using namespace sandbox::ir;
    Program program;
    Value invColsN = program.constant(Type::T40, 1);
    Value invColsD = program.constant(Type::T40, input.cols);
    Value invCols = program.div(invColsN, invColsD);
    Value epsN = program.constant(Type::T40, 1);
    Value epsD = program.constant(Type::T40, 1000);
    Value eps = program.div(epsN, epsD);

    for (int row = 0; row < input.rows; ++row) {
        std::vector<Value> values;
        values.reserve(static_cast<std::size_t>(input.cols));
        Value sum = program.constant(Type::T40, 0);
        for (int col = 0; col < input.cols; ++col) {
            Value value = loadElementIrT40(program, input, row, col);
            values.push_back(value);
            Value next = program.add(sum, value);
            program.release(sum);
            sum = next;
        }
        Value mean = program.mul(sum, invCols);
        program.release(sum);

        std::vector<Value> centered;
        centered.reserve(static_cast<std::size_t>(input.cols));
        Value varianceSum = program.constant(Type::T40, 0);
        for (int col = 0; col < input.cols; ++col) {
            Value delta = program.sub(values[static_cast<std::size_t>(col)], mean);
            Value square = program.mul(delta, delta);
            Value nextVariance = program.add(varianceSum, square);
            centered.push_back(delta);
            program.release(values[static_cast<std::size_t>(col)]);
            program.release(square);
            program.release(varianceSum);
            varianceSum = nextVariance;
        }
        program.release(mean);

        Value variance = program.mul(varianceSum, invCols);
        Value varianceEps = program.add(variance, eps);
        Value denom = program.sqrt(varianceEps);
        program.release(varianceSum);
        program.release(variance);
        program.release(varianceEps);

        for (int col = 0; col < input.cols; ++col) {
            Value normalized = program.div(centered[static_cast<std::size_t>(col)], denom);
            Value scale = loadElementIrT40(program, gamma, 0, col);
            Value bias = loadElementIrT40(program, beta, 0, col);
            Value scaled = program.mul(normalized, scale);
            Value shifted = program.add(scaled, bias);
            storeElementIr(program, out, row, col, shifted);
            program.release(centered[static_cast<std::size_t>(col)]);
            program.release(normalized);
            program.release(scale);
            program.release(bias);
            program.release(scaled);
            program.release(shifted);
        }
        program.release(denom);
    }

    program.release(invColsN);
    program.release(invColsD);
    program.release(invCols);
    program.release(epsN);
    program.release(epsD);
    program.release(eps);
    program.halt();
    return program;
}

inline GeneratedKernelResult layerNormRowsGenerated(
    vm::VMState& state,
    TensorView input,
    TensorView gamma,
    TensorView beta,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult failed;
    failed.name = "layer norm rows";
    if (input.rows != out.rows || input.cols != out.cols ||
        gamma.rows != 1 || beta.rows != 1 ||
        gamma.cols != input.cols || beta.cols != input.cols ||
        !validTensor(state, input) || !validTensor(state, gamma) ||
        !validTensor(state, beta) || !validTensor(state, out) ||
        !vm::isNumericMode(input.mode) || !vm::isNumericMode(gamma.mode) ||
        !vm::isNumericMode(beta.mode) || !vm::isNumericMode(out.mode)) {
        failed.status = "invalid tensor shape or mode";
        return failed;
    }

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "layer norm rows",
        buildLayerNormRowsProgram(input, gamma, beta, out),
        200000);
    const uint64_t rows = static_cast<uint64_t>(input.rows);
    const uint64_t cols = static_cast<uint64_t>(input.cols);
    addKernelStats(stats, result, rows * (cols * 2 + cols * 2), rows * cols);
    return result;
}

inline sandbox::ir::Program buildRmsNormRowsProgram(
    TensorView input,
    TensorView gamma,
    TensorView out) {

    using namespace sandbox::ir;
    Program program;
    Value invColsN = program.constant(Type::T40, 1);
    Value invColsD = program.constant(Type::T40, input.cols);
    Value invCols = program.div(invColsN, invColsD);
    Value epsN = program.constant(Type::T40, 1);
    Value epsD = program.constant(Type::T40, 1000);
    Value eps = program.div(epsN, epsD);

    for (int row = 0; row < input.rows; ++row) {
        std::vector<Value> values;
        values.reserve(static_cast<std::size_t>(input.cols));
        Value meanSquareSum = program.constant(Type::T40, 0);
        for (int col = 0; col < input.cols; ++col) {
            Value value = loadElementIrT40(program, input, row, col);
            values.push_back(value);
            Value square = program.mul(value, value);
            Value next = program.add(meanSquareSum, square);
            program.release(meanSquareSum);
            program.release(square);
            meanSquareSum = next;
        }
        Value meanSquare = program.mul(meanSquareSum, invCols);
        Value meanSquareEps = program.add(meanSquare, eps);
        Value denom = program.sqrt(meanSquareEps);
        program.release(meanSquareSum);
        program.release(meanSquare);
        program.release(meanSquareEps);

        for (int col = 0; col < input.cols; ++col) {
            Value normalized = program.div(values[static_cast<std::size_t>(col)], denom);
            Value scale = loadElementIrT40(program, gamma, 0, col);
            Value shifted = program.mul(normalized, scale);
            storeElementIr(program, out, row, col, shifted);
            program.release(values[static_cast<std::size_t>(col)]);
            program.release(normalized);
            program.release(scale);
            program.release(shifted);
        }
        program.release(denom);
    }

    program.release(invColsN);
    program.release(invColsD);
    program.release(invCols);
    program.release(epsN);
    program.release(epsD);
    program.release(eps);
    program.halt();
    return program;
}

inline GeneratedKernelResult rmsNormRowsGenerated(
    vm::VMState& state,
    TensorView input,
    TensorView gamma,
    TensorView out,
    RuntimeStats* stats = nullptr) {

    GeneratedKernelResult failed;
    failed.name = "rms norm rows";
    if (input.rows != out.rows || input.cols != out.cols ||
        gamma.rows != 1 || gamma.cols != input.cols ||
        !validTensor(state, input) || !validTensor(state, gamma) ||
        !validTensor(state, out) ||
        !vm::isNumericMode(input.mode) || !vm::isNumericMode(gamma.mode) ||
        !vm::isNumericMode(out.mode)) {
        failed.status = "invalid tensor shape or mode";
        return failed;
    }

    GeneratedKernelResult result = executeGeneratedKernel(
        state,
        "rms norm rows",
        buildRmsNormRowsProgram(input, gamma, out),
        200000);
    const uint64_t rows = static_cast<uint64_t>(input.rows);
    const uint64_t cols = static_cast<uint64_t>(input.cols);
    addKernelStats(stats, result, rows * (cols * 2 + cols), rows * cols);
    return result;
}


} // namespace transformer_runtime
} // namespace sandbox

#endif // TERNARY_TRANSFORMER_RUNTIME_H
