#pragma once
#include "ternary_uint128.h"
#include <string>

#ifndef _MSC_VER
using int128_t = __int128;
using uint128_t = unsigned __int128;
#else

struct int128_t;

struct uint128_t {
    sandbox::UInt128 val;

    uint128_t() = default;
    uint128_t(sandbox::UInt128 v) : val(v) {}
    uint128_t(uint64_t v) : val(v) {}
    uint128_t(int v) : val(static_cast<uint64_t>(v)) {}
    uint128_t(unsigned int v) : val(static_cast<uint64_t>(v)) {}
    uint128_t(long long v) : val(static_cast<uint64_t>(v)) {}
    
    explicit uint128_t(int128_t x);

    // Friend binary arithmetic operators
    friend uint128_t operator+(uint128_t a, uint128_t b) { return a.val + b.val; }
    friend uint128_t operator-(uint128_t a, uint128_t b) { return a.val - b.val; }
    friend uint128_t operator*(uint128_t a, uint128_t b) { return a.val * b.val; }
    friend uint128_t operator/(uint128_t a, uint128_t b) { return a.val / b.val; }
    friend uint128_t operator%(uint128_t a, uint128_t b) { return a.val % b.val; }

    friend uint128_t operator*(uint128_t a, uint32_t b) { return a.val * b; }
    friend uint128_t operator/(uint128_t a, uint32_t b) { return a.val / b; }
    friend uint128_t operator%(uint128_t a, uint32_t b) { return a.val % b; }

    friend uint128_t operator<<(uint128_t a, unsigned s) { return a.val << s; }
    friend uint128_t operator>>(uint128_t a, unsigned s) { return a.val >> s; }

    uint128_t& operator+=(uint128_t b) { val += b.val; return *this; }
    uint128_t& operator-=(uint128_t b) { val -= b.val; return *this; }
    uint128_t& operator*=(uint128_t b) { val = val * b.val; return *this; }
    uint128_t& operator/=(uint128_t b) { val = val / b.val; return *this; }
    uint128_t& operator%=(uint128_t b) { val = val % b.val; return *this; }
    uint128_t& operator<<=(unsigned s) { val <<= s; return *this; }
    uint128_t& operator>>=(unsigned s) { val >>= s; return *this; }

    uint128_t& operator*=(uint32_t b) { val = val * b; return *this; }

    // Friend comparison operators
    friend bool operator==(uint128_t a, uint128_t b) { return a.val == b.val; }
    friend bool operator!=(uint128_t a, uint128_t b) { return a.val != b.val; }
    friend bool operator<(uint128_t a, uint128_t b) { return a.val < b.val; }
    friend bool operator>(uint128_t a, uint128_t b) { return a.val > b.val; }
    friend bool operator<=(uint128_t a, uint128_t b) { return a.val <= b.val; }
    friend bool operator>=(uint128_t a, uint128_t b) { return a.val >= b.val; }

    // Implicit conversions to sandbox type
    operator sandbox::UInt128() const { return val; }

    // Explicit conversions to basic types
    explicit operator bool() const { return !val.isZero(); }
    explicit operator int8_t() const { return static_cast<int8_t>(val.lo); }
    explicit operator uint8_t() const { return static_cast<uint8_t>(val.lo); }
    explicit operator int16_t() const { return static_cast<int16_t>(val.lo); }
    explicit operator uint16_t() const { return static_cast<uint16_t>(val.lo); }
    explicit operator int32_t() const { return static_cast<int32_t>(val.lo); }
    explicit operator uint32_t() const { return static_cast<uint32_t>(val.lo); }
    explicit operator int64_t() const { return static_cast<int64_t>(val.lo); }
    explicit operator uint64_t() const { return val.lo; }

    explicit operator long double() const {
        return static_cast<long double>(val.lo) +
               static_cast<long double>(val.hi) * 18446744073709551616.0L;
    }
    explicit operator double() const {
        return static_cast<double>(static_cast<long double>(*this));
    }
};

struct int128_t {
    sandbox::Int128 val;

    int128_t() = default;
    int128_t(sandbox::Int128 v) : val(v) {}
    int128_t(long long v) : val(sandbox::Int128::fromLongLong(v)) {}
    int128_t(int v) : val(sandbox::Int128::fromLongLong(v)) {}
    int128_t(unsigned int v) : val(sandbox::Int128::fromLongLong(static_cast<long long>(v))) {}
    int128_t(uint64_t v) : val(sandbox::Int128::fromMagnitude(1, sandbox::UInt128{v})) {}

    explicit int128_t(uint128_t x) : val(sandbox::Int128::fromMagnitude(1, x.val)) {}

    // Friend unary operators
    friend int128_t operator-(int128_t a) { return -a.val; }

    // Friend binary arithmetic operators
    friend int128_t operator+(int128_t a, int128_t b) { return a.val + b.val; }
    friend int128_t operator-(int128_t a, int128_t b) { return a.val - b.val; }
    friend int128_t operator*(int128_t a, int128_t b) { return a.val * b.val; }
    friend int128_t operator/(int128_t a, int128_t b) { return a.val / b.val; }

    // Modulo operator: a % b = a - (a / b) * b
    friend int128_t operator%(int128_t a, int128_t b) {
        int128_t q = a / b;
        return a - q * b;
    }
    friend int128_t operator%(int128_t a, int b) {
        return a % int128_t(b);
    }

    int128_t& operator+=(int128_t b) { val = val + b.val; return *this; }
    int128_t& operator-=(int128_t b) { val = val - b.val; return *this; }
    int128_t& operator*=(int128_t b) { val = val * b.val; return *this; }
    int128_t& operator/=(int128_t b) { val = val / b.val; return *this; }

    int128_t& operator++() {
        val = val + sandbox::Int128::fromLongLong(1);
        return *this;
    }
    int128_t operator++(int) {
        int128_t temp = *this;
        ++(*this);
        return temp;
    }
    int128_t& operator--() {
        val = val - sandbox::Int128::fromLongLong(1);
        return *this;
    }
    int128_t operator--(int) {
        int128_t temp = *this;
        --(*this);
        return temp;
    }

    // Friend comparison operators
    friend bool operator==(int128_t a, int128_t b) { return a.val == b.val; }
    friend bool operator!=(int128_t a, int128_t b) { return a.val != b.val; }
    friend bool operator<(int128_t a, int128_t b) { return a.val < b.val; }
    friend bool operator>(int128_t a, int128_t b) { return a.val > b.val; }
    friend bool operator<=(int128_t a, int128_t b) { return a.val <= b.val; }
    friend bool operator>=(int128_t a, int128_t b) { return a.val >= b.val; }

    // Implicit conversions to sandbox type
    operator sandbox::Int128() const { return val; }

    // Explicit conversions to basic types
    explicit operator bool() const { return !val.isZero(); }
    explicit operator int8_t() const { return static_cast<int8_t>(toLongLongSaturated(val)); }
    explicit operator uint8_t() const { return static_cast<uint8_t>(toLongLongSaturated(val)); }
    explicit operator int16_t() const { return static_cast<int16_t>(toLongLongSaturated(val)); }
    explicit operator uint16_t() const { return static_cast<uint16_t>(toLongLongSaturated(val)); }
    explicit operator int32_t() const { return static_cast<int32_t>(toLongLongSaturated(val)); }
    explicit operator uint32_t() const { return static_cast<uint32_t>(toLongLongSaturated(val)); }
    explicit operator int64_t() const { return static_cast<int64_t>(toLongLongSaturated(val)); }
    explicit operator uint64_t() const { return static_cast<uint64_t>(toLongLongSaturated(val)); }

    explicit operator long double() const {
        long double res = static_cast<long double>(val.magnitude.lo) +
                          static_cast<long double>(val.magnitude.hi) * 18446744073709551616.0L;
        return val.sign < 0 ? -res : res;
    }
    explicit operator double() const {
        return static_cast<double>(static_cast<long double>(*this));
    }
};

inline uint128_t::uint128_t(int128_t x) : val(x.val.magnitude) {}

#endif
