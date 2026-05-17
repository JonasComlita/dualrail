#pragma once
#include "ternary_asm.h"
#include "ternary_device_allocators.h"
#include "ternary_gpu_kernels.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

extern int g_failures;

inline void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

inline std::string readTextFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) return {};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        out += line;
        out += '\n';
    }
    return out;
}

template<typename T>
inline void expectLong(T value, long long want, const std::string& label) {
    const long long got = sandbox::native_ops::toLongLong(value);
    expect(got == want, label + " got " + std::to_string(got) +
                      " want " + std::to_string(want));
}

inline long double toLongDouble(sandbox::LongTriple value) {
    if (value.isZero()) return 0.0L;
    auto [m, e] = sandbox::long_ops::decode(value);
    return m * std::pow(3.0L, static_cast<long double>(e));
}

template<typename T>
inline long double toLongDoubleValue(T value) {
    return toLongDouble(sandbox::native_ops::toLongTriple(value));
}

inline void expectNear(long double got, long double want, long double tol,
                 const std::string& label) {
    const long double diff = std::fabsl(got - want);
    const long double scale = std::max(1.0L, std::fabsl(want));
    expect(diff <= tol * scale,
           label + " got " + std::to_string(static_cast<double>(got)) +
           " want " + std::to_string(static_cast<double>(want)));
}

inline long long vectorLong(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return sandbox::vm::ops::toLong(vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane));
}

inline sandbox::TernaryMode vectorMode(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane).mode;
}

inline long long loadPhysLong(sandbox::vm::VMState& vm, int addr) {
    auto [value, fault] = vm.dmem.load(addr);
    if (fault != sandbox::vm::MemFaultCode::OK) return 0;
    return sandbox::vm::ops::toLong(value);
}

inline int8_t vectorPredicateTrit(const sandbox::vm::VMState& vm, int vreg, int lane) {
    return vm.vregfile.reg[static_cast<std::size_t>(vreg)].read(lane).asL1().tritAt(0);
}

#if defined(__SIZEOF_INT128__)
using Native128 = unsigned __int128;

inline std::string native128ToString(Native128 value) {
    if (value == 0) return "0";
    std::string out;
    while (value != 0) {
        const unsigned digit = static_cast<unsigned>(value % 10);
        out.insert(out.begin(), static_cast<char>('0' + digit));
        value /= 10;
    }
    return out;
}

inline void expectUInt128(sandbox::UInt128 got, Native128 want, const std::string& label) {
    expect(got.toNative() == want,
           label + " got " + got.toString() +
           " want " + native128ToString(want));
}
#endif
