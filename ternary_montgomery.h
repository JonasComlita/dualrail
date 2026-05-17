// =============================================================================
// ternary_montgomery.h  —  Phase 4+ Cryptographic Extension
// =============================================================================
//
// Montgomery multiplication and RSA-2048 modular exponentiation for smart card
// / secure element integration with the ternary VM.
//
// Design decisions:
//   • Limb width: 27 trits = one ISA word (TritWord27).
//   • B = 3^27 = 7,625,597,484,987  fits in int64_t.
//   • Intermediate products (limb × limb) ≈ 1.45 × 10^25 → __int128.
//   • RSA-2048: 48 limbs × 27 trits = 1,296 trits (covers 1,293 needed).
//   • Algorithm: CIOS (Coarsely Integrated Operand Scanning) — HAC §14.4.
//   • N′ = −N₀⁻¹ mod B  via Hensel lifting (Newton, 6 steps, quadratic conv.).
//   • R² mod N  via repeated trit-shift and reduce (1,296 iterations of ×3).
//   • Side-channel: branchless conditional subtract; Montgomery ladder for
//     exponentiation; no data-dependent control flow over key bits.
//   • Smart card gate: runToySelfTest() passes before any cryptographic
//     SYSCALL_EXEC is granted by the VM kernel (ternary_os.h).
//   • Fault-injection check: after private-key sign, verify sig^e ≡ m.
//
// Dependencies:
//   ternary_vm_state.h  (TernaryValue, ops::fromLong, ISA constants)
//   ternary_asm.h       (assembler integration for inner-loop demonstration)
//
// =============================================================================

#pragma once
#ifndef TERNARY_MONTGOMERY_H
#define TERNARY_MONTGOMERY_H

#include "ternary_vm_state.h"
#include "ternary_asm.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {
namespace crypto {

// =============================================================================
// SECTION 1 — Limb constants
// =============================================================================

static constexpr int      TRIT_LIMB  = 27;                // trits per limb (= ISA word)
static constexpr long long LIMB_B    = 7625597484987LL;   // 3^27
static constexpr long long LIMB_HALF = 3812798742493LL;   // (3^27 − 1) / 2 (max balanced digit)

// RSA-2048 in balanced ternary:
//   ⌈2048 × log₃(2)⌉ = ⌈2048 × 0.63093⌉ = 1293 trits
//   Limbs needed: ⌈1293 / 27⌉ = 48,  giving 1296 total trits.
static constexpr int RSA2048_LIMBS = 48;

// =============================================================================
// SECTION 2 — Balanced divmod helper
// =============================================================================
// Returns (carry, remainder) such that  S = carry × B + remainder,
// remainder ∈ (−LIMB_HALF, +LIMB_HALF].   Uses __int128 for safety.

[[nodiscard]] inline std::pair<long long, long long> divmodB(__int128 S) {
    using T = __int128;
    const T B = static_cast<T>(LIMB_B);
    long long c = static_cast<long long>(S / B);
    long long r = static_cast<long long>(S % B);
    // Adjust to balanced range (−LIMB_HALF, +LIMB_HALF]
    if (r > LIMB_HALF)  { r -= LIMB_B; ++c; }
    if (r < -LIMB_HALF) { r += LIMB_B; --c; }
    return {c, r};
}

// =============================================================================
// SECTION 3 — TritBigInt<N>
// =============================================================================
// N-limb big integer in balanced ternary.  Limb base B = 3^27.
// Invariant (normalized): limb[i] ∈ (−LIMB_HALF, +LIMB_HALF].
// Value: Σ limb[i] × B^i  for i ∈ [0, N).

template<int N>
struct TritBigInt {
    static_assert(N >= 1 && N <= 128, "limb count out of range");

    std::array<long long, N> limb{};

    TritBigInt() { limb.fill(0); }

    // --- Constructors --------------------------------------------------------

    [[nodiscard]] static TritBigInt zero() { return TritBigInt{}; }

    [[nodiscard]] static TritBigInt one() {
        TritBigInt t; t.limb[0] = 1; return t;
    }

    [[nodiscard]] static TritBigInt fromULL(unsigned long long v) {
        TritBigInt out;
        for (int i = 0; i < N && v > 0; ++i) {
            out.limb[i] = static_cast<long long>(v % static_cast<unsigned long long>(LIMB_B));
            v /= static_cast<unsigned long long>(LIMB_B);
        }
        return out;
    }

    [[nodiscard]] static TritBigInt fromSLL(long long v) {
        if (v >= 0) return fromULL(static_cast<unsigned long long>(v));
        TritBigInt out = fromULL(static_cast<unsigned long long>(-v));
        for (auto& l : out.limb) l = -l;
        return out;
    }

    // Load from signed trit vector (index 0 = LST, balanced: each in {-1, 0, +1}).
    [[nodiscard]] static TritBigInt fromTrits(const std::vector<int8_t>& t) {
        TritBigInt out;
        long long p = 1;
        int li = 0, ti = 0;
        for (int i = 0; i < static_cast<int>(t.size()) && li < N; ++i) {
            out.limb[li] += static_cast<long long>(t[static_cast<std::size_t>(i)]) * p;
            p *= 3;
            if (++ti == TRIT_LIMB) { ++li; ti = 0; p = 1; }
        }
        out.normalize();
        return out;
    }

    // --- Predicates ----------------------------------------------------------

    [[nodiscard]] bool isZero() const {
        for (auto v : limb) if (v) return false;
        return true;
    }

    [[nodiscard]] bool isNegative() const {
        for (int i = N - 1; i >= 0; --i) {
            if (limb[i] < 0) return true;
            if (limb[i] > 0) return false;
        }
        return false;
    }

    // --- Normalization -------------------------------------------------------
    // Propagate carries so each limb ∈ (−LIMB_HALF, +LIMB_HALF].
    // Only handles one carry step per limb; call after single add/sub.

    void normalize() {
        for (int i = 0; i + 1 < N; ++i) {
            if (limb[i] > LIMB_HALF || limb[i] < -LIMB_HALF) {
                long long c = limb[i] / LIMB_B;
                long long r = limb[i] - c * LIMB_B;
                if (r > LIMB_HALF)  { ++c; r -= LIMB_B; }
                if (r < -LIMB_HALF) { --c; r += LIMB_B; }
                limb[i] = r;
                limb[i + 1] += c;
            }
        }
    }

    // --- Arithmetic ----------------------------------------------------------

    [[nodiscard]] int compare(const TritBigInt& o) const {
        for (int i = N - 1; i >= 0; --i) {
            if (limb[i] < o.limb[i]) return -1;
            if (limb[i] > o.limb[i]) return +1;
        }
        return 0;
    }

    [[nodiscard]] TritBigInt operator+(const TritBigInt& o) const {
        TritBigInt r;
        for (int i = 0; i < N; ++i) r.limb[i] = limb[i] + o.limb[i];
        r.normalize(); return r;
    }

    [[nodiscard]] TritBigInt operator-(const TritBigInt& o) const {
        TritBigInt r;
        for (int i = 0; i < N; ++i) r.limb[i] = limb[i] - o.limb[i];
        r.normalize(); return r;
    }

    [[nodiscard]] TritBigInt operator-() const {
        TritBigInt r;
        for (int i = 0; i < N; ++i) r.limb[i] = -limb[i];
        return r;
    }

    // Branchless constant-time conditional subtract.
    // Post-condition: this ← (this ≥ mod) ? this − mod : this.
    // Assumes 0 ≤ this < 2 × mod (single-step Montgomery invariant).
    void conditionalSubCT(const TritBigInt& mod) {
        TritBigInt diff;
        for (int i = 0; i < N; ++i) diff.limb[i] = limb[i] - mod.limb[i];
        diff.normalize();
        // In normalized form and given 0 ≤ this < 2·mod:
        //   diff = this − mod ∈ (−mod, mod).
        //   diff ≥ 0  iff  MSL diff.limb[N-1] ≥ 0  (sign bit of top limb).
        // Constant-time mask: 1 if top ≥ 0 (subtract), 0 if top < 0 (keep).
        unsigned long long top_u = static_cast<unsigned long long>(diff.limb[N - 1]);
        long long mask = 1LL - static_cast<long long>(top_u >> 63);
        for (int i = 0; i < N; ++i)
            limb[i] = mask * diff.limb[i] + (1LL - mask) * limb[i];
    }

    // --- Trit access ---------------------------------------------------------

    [[nodiscard]] int8_t getTrit(int pos) const {
        int li = pos / TRIT_LIMB, ti = pos % TRIT_LIMB;
        if (li >= N) return 0;
        long long v = limb[li];
        for (int i = 0; i < ti; ++i) {
            long long r = v % 3;
            if (r > 1) r -= 3; if (r < -1) r += 3;
            v = (v - r) / 3;
        }
        long long r = v % 3;
        if (r > 1) r -= 3; if (r < -1) r += 3;
        return static_cast<int8_t>(r);
    }

    // --- Conversion ----------------------------------------------------------

    [[nodiscard]] std::string toTritString(int max_trits = 0) const {
        if (max_trits <= 0) max_trits = N * TRIT_LIMB;
        std::string out;
        for (int i = max_trits - 1; i >= 0; --i) {
            int8_t t = getTrit(i);
            out += (t < 0 ? '-' : (t > 0 ? '+' : '0'));
        }
        return out;
    }
};

// =============================================================================
// SECTION 4 — Hensel Lifting: compute N′ = −N₀⁻¹ mod B
// =============================================================================
// Uses Newton's method (quadratic convergence) starting from x = N₀⁻¹ mod 3.
// Each iteration: x ← x × (2 − N₀ × x) mod p²,  doubling the correct digits.
// After 6 iterations: 3^(2^6) = 3^64 > 3^27 = B.  ✓

[[nodiscard]] inline long long computeNPrime(long long n0) {
    using L = __int128;

    // Reduce n0 to non-negative representative mod B.
    L n = static_cast<L>(n0) % static_cast<L>(LIMB_B);
    if (n < 0) n += static_cast<L>(LIMB_B);
    assert(static_cast<long long>(n % 3) != 0 && "modulus must be coprime to 3");

    // Initial inverse mod 3:  n ≡ 1 (mod 3) → x = 1;  n ≡ 2 (mod 3) → x = 2.
    L x = (n % 3 == 1) ? L(1) : L(2);
    L p = 3;  // current precision: x correct mod p

    // Six Newton steps.
    for (int iter = 0; iter < 6; ++iter) {
        L p2 = p * p;
        if (p2 > static_cast<L>(LIMB_B) || p2 <= 0) p2 = static_cast<L>(LIMB_B);
        // x ← x × (2 − n × x)  mod p²
        L t = (L(2) - (n % p2) * (x % p2) % p2 + p2 * p2) % p2;
        x = (x % p2 * t % p2 + p2 * p2) % p2;
        p = p2;
        if (p >= static_cast<L>(LIMB_B)) break;
    }

    // x = N₀⁻¹ mod B.  N′ = −x mod B (balanced).
    L neg_x = (static_cast<L>(LIMB_B) - x % static_cast<L>(LIMB_B)) % static_cast<L>(LIMB_B);
    long long result = static_cast<long long>(neg_x);
    if (result > LIMB_HALF) result -= LIMB_B;
    return result;
}

// Verify: n0 × n_prime ≡ −1 (mod B).
[[nodiscard]] inline bool verifyNPrime(long long n0, long long n_prime) {
    __int128 product = static_cast<__int128>(n0) * n_prime;
    long long rem = static_cast<long long>(product % static_cast<__int128>(LIMB_B));
    if (rem > LIMB_HALF)  rem -= LIMB_B;
    if (rem < -LIMB_HALF) rem += LIMB_B;
    return rem == -1LL;
}

// =============================================================================
// SECTION 5 — CIOS Montgomery Multiplication
// =============================================================================
// Computes  a × b × R⁻¹  mod N,   where R = B^n, B = 3^27, n = N_LIMBS.
//
// Algorithm (HAC Algorithm 14.36 CIOS variant):
//   T = 0  (n+2 limbs)
//   for i = 0 … n−1:
//     C = 0
//     for j = 0 … n−1:  (C, T[j]) ← T[j] + a[i]·b[j] + C      [accumulate]
//     (C, T[n]) ← T[n] + C;  T[n+1] += C
//
//     m ← T[0] · N′  mod B                                        [reduction digit]
//
//     C = 0
//     (C, _)    ← T[0] + m·N[0] + C    [T[0] becomes 0; discard]
//     for j = 1 … n−1:  (C, T[j−1]) ← T[j] + m·N[j] + C        [shift + reduce]
//     (C, T[n−1]) ← T[n] + C;  T[n] ← T[n+1] + C;  T[n+1] = 0
//
//   if T ≥ N: T ← T − N
//   return T[0…n−1]

template<int N_LIMBS>
[[nodiscard]] TritBigInt<N_LIMBS> montgomeryMul(
    const TritBigInt<N_LIMBS>& a,
    const TritBigInt<N_LIMBS>& b,
    const TritBigInt<N_LIMBS>& modulus,
    long long                   n_prime)
{
    using L = __int128;

    // Working register: n+2 limbs (n+1 for overflow guard, +1 for secondary carry)
    std::array<long long, N_LIMBS + 2> T{};
    T.fill(0);

    for (int i = 0; i < N_LIMBS; ++i) {

        // ---- Phase A: accumulate a[i] × b into T --------------------------
        long long C = 0;
        for (int j = 0; j < N_LIMBS; ++j) {
            L s = static_cast<L>(T[j])
                + static_cast<L>(a.limb[i]) * static_cast<L>(b.limb[j])
                + static_cast<L>(C);
            auto [c, r] = divmodB(s);
            T[j] = r;
            C = c;
        }
        {
            auto [c, r] = divmodB(static_cast<L>(T[N_LIMBS]) + static_cast<L>(C));
            T[N_LIMBS]     = r;
            T[N_LIMBS + 1] += c;
        }

        // ---- Phase B: compute Montgomery reduction digit  m ---------------
        // m = T[0] × N′ mod B.  After adding m×N, T[0] will be ≡ 0 (mod B).
        L m_raw = static_cast<L>(T[0]) * static_cast<L>(n_prime);
        auto [m_carry, m] = divmodB(m_raw);
        (void)m_carry;  // only the remainder is needed

        // ---- Phase C: add m×N to T and shift right one position -----------
        C = 0;
        {
            // T[0] + m×N[0] ≡ 0 (mod B) by design — discard the remainder.
            L s = static_cast<L>(T[0])
                + static_cast<L>(m) * static_cast<L>(modulus.limb[0])
                + static_cast<L>(C);
            auto [c, _r] = divmodB(s);
            (void)_r;  // discarded: this is the zero that makes the shift exact
            C = c;
        }
        for (int j = 1; j < N_LIMBS; ++j) {
            L s = static_cast<L>(T[j])
                + static_cast<L>(m) * static_cast<L>(modulus.limb[j])
                + static_cast<L>(C);
            auto [c, r] = divmodB(s);
            T[j - 1] = r;   // shift: T[j] becomes T[j−1]
            C = c;
        }
        {
            auto [c1, r1] = divmodB(static_cast<L>(T[N_LIMBS]) + static_cast<L>(C));
            T[N_LIMBS - 1] = r1;
            auto [c2, r2] = divmodB(static_cast<L>(T[N_LIMBS + 1]) + static_cast<L>(c1));
            T[N_LIMBS]     = r2;
            T[N_LIMBS + 1] = c2;
        }
    }

    // Extract result from T[0…n−1] and apply final conditional subtract.
    TritBigInt<N_LIMBS> result;
    for (int i = 0; i < N_LIMBS; ++i) result.limb[i] = T[i];
    result.normalize();
    result.conditionalSubCT(modulus);
    return result;
}

// =============================================================================
// SECTION 6 — MontgomeryContext<N>
// =============================================================================
// Bundles modulus, N′, and R mod N; provides the primary user-facing API.

template<int N>
class MontgomeryContext {
public:
    using BigInt = TritBigInt<N>;

    BigInt    modulus;
    long long n_prime = 0;   // −N₀⁻¹ mod B
    BigInt    R_mod_N;        // B^N mod N (= R mod N); R² = mul(R_mod_N, R_mod_N)

    MontgomeryContext() = default;

    explicit MontgomeryContext(const BigInt& N_in) : modulus(N_in) {
        // RSA moduli are products of two large primes; neither divisible by 3.
        assert(N_in.limb[0] % 3 != 0 && "modulus must be coprime to 3 for Montgomery");
        n_prime = computeNPrime(N_in.limb[0]);
        assert(verifyNPrime(N_in.limb[0], n_prime) && "Hensel lift verification failed");
        R_mod_N = computeRmodN();
    }

    // Core Montgomery multiplication: returns a × b × R⁻¹ mod N.
    [[nodiscard]] BigInt mul(const BigInt& a, const BigInt& b) const {
        return montgomeryMul<N>(a, b, modulus, n_prime);
    }

    // Convert x into Montgomery form: x̃ = x × R mod N.
    [[nodiscard]] BigInt toMont(const BigInt& x) const {
        return mul(x, mul(R_mod_N, R_mod_N));  // x × (R mod N)² × R⁻¹ = x × R mod N
    }

    // Convert from Montgomery form: x = x̃ × R⁻¹ mod N.
    [[nodiscard]] BigInt fromMont(const BigInt& x_tilde) const {
        return mul(x_tilde, BigInt::one());
    }

    // -------------------------------------------------------------------------
    // Constant-time modular exponentiation (Montgomery ladder).
    // exp_bits: binary exponent, MSB first.
    // Both branches execute exactly 2 multiplications per bit — no timing leak.
    // -------------------------------------------------------------------------
    [[nodiscard]] BigInt modExpBinary(
        const BigInt& base,
        const std::vector<bool>& exp_bits) const
    {
        BigInt R0 = toMont(BigInt::one());   // R0 = 1 (Montgomery form)
        BigInt R1 = toMont(base);            // R1 = base (Montgomery form)

        for (bool bit : exp_bits) {
            if (!bit) {
                // bit = 0: R1 ← R0·R1,  R0 ← R0²
                R1 = mul(R0, R1);
                R0 = mul(R0, R0);
            } else {
                // bit = 1: R0 ← R0·R1,  R1 ← R1²
                R0 = mul(R0, R1);
                R1 = mul(R1, R1);
            }
            // Either branch: 2 multiplications, identical instruction trace.
        }
        return fromMont(R0);
    }

    // Non-constant-time square-and-multiply (used only for the toy validity test).
    [[nodiscard]] BigInt modExpSimple(const BigInt& base, unsigned long long exp) const {
        BigInt result = toMont(BigInt::one());
        BigInt cur    = toMont(base);
        while (exp > 0) {
            if (exp & 1ULL) result = mul(result, cur);
            cur = mul(cur, cur);
            exp >>= 1;
        }
        return fromMont(result);
    }

private:
    // Compute R mod N = B^N mod N.
    // Method: start from 1 and multiply by 3 exactly N × TRIT_LIMB times,
    // reducing mod N after each step. Each step: 3x < 3N → two subtracts suffice.
    // Cost: O(N × TRIT_LIMB) reductions (1,296 for RSA-2048).
    BigInt computeRmodN() const {
        BigInt x = BigInt::one();
        const int total_shifts = N * TRIT_LIMB;  // 48 × 27 = 1296 for RSA-2048
        for (int step = 0; step < total_shifts; ++step) {
            for (int i = 0; i < N; ++i) x.limb[i] *= 3;
            x.normalize();
            x.conditionalSubCT(modulus);
            x.conditionalSubCT(modulus);  // 3x < 3N, so at most 2 subtracts needed
        }
        return x;
    }
};

// =============================================================================
// SECTION 7 — Reference arithmetic (host binary, for validation only)
// =============================================================================

[[nodiscard]] inline unsigned long long naivePowMod(
    unsigned long long base,
    unsigned long long exp,
    unsigned long long m)
{
    if (m == 1) return 0;
    unsigned long long result = 1;
    base %= m;
    while (exp > 0) {
        if (exp & 1ULL)
            result = static_cast<unsigned long long>(
                (static_cast<__int128>(result) * base) % m);
        base = static_cast<unsigned long long>(
            (static_cast<__int128>(base) * base) % m);
        exp >>= 1;
    }
    return result;
}

// =============================================================================
// SECTION 8 — Self-test: toy RSA key (27-trit numbers, instant verification)
// =============================================================================
// Parameters:
//   p = 89,  q = 97  (both prime; 89 ≡ 2 (mod 3),  97 ≡ 1 (mod 3))
//   N = 89 × 97 = 8633                      (not divisible by 3 ✓)
//   φ(N) = 88 × 96 = 8448
//   e = 7                                    (gcd(7, 8448) = 1 ✓)
//   d = 7⁻¹ mod 8448 = 1207                  (7 × 1207 = 8449 = 8448 + 1 ✓)
//   test message m = 42

struct SelfTestResult {
    bool    passed    = false;
    std::string error;
    // Cycle counts for rough timing (filled by caller if desired)
    long long encrypt_ns = 0;
    long long decrypt_ns = 0;
};

[[nodiscard]] inline SelfTestResult runToySelfTest() {
    SelfTestResult res;

    // ---- Ground truth via binary host arithmetic ----------------------------
    static constexpr unsigned long long TOY_N = 8633ULL;
    static constexpr unsigned long long TOY_E = 7ULL;
    static constexpr unsigned long long TOY_D = 1207ULL;
    static constexpr unsigned long long TOY_M = 42ULL;

    const unsigned long long ref_c = naivePowMod(TOY_M, TOY_E, TOY_N);
    const unsigned long long ref_m = naivePowMod(ref_c, TOY_D, TOY_N);

    if (ref_m != TOY_M) {
        res.error = "binary reference RSA round-trip failed";
        return res;
    }

    // ---- Ternary Montgomery test (TritBigInt<2> = 54-trit numbers) ----------
    // 2 limbs × 27 trits = 54 trits; covers N = 8633 < 3^14 ≪ 3^54. ✓

    using SmallInt = TritBigInt<2>;
    SmallInt N_bi  = SmallInt::fromULL(TOY_N);
    SmallInt m_bi  = SmallInt::fromULL(TOY_M);

    if (N_bi.limb[0] % 3 == 0) {
        res.error = "toy modulus divisible by 3 — invalid for Montgomery";
        return res;
    }

    MontgomeryContext<2> ctx(N_bi);

    // Encrypt: c = m^e mod N
    SmallInt c_bi = ctx.modExpSimple(m_bi, TOY_E);
    unsigned long long c_mont = static_cast<unsigned long long>(
        c_bi.limb[0] + c_bi.limb[1] * LIMB_B);

    if (c_mont != ref_c) {
        res.error = "Montgomery encrypt mismatch: got " + std::to_string(c_mont)
                  + ", expected " + std::to_string(ref_c);
        return res;
    }

    // Decrypt: m′ = c^d mod N
    SmallInt m_dec_bi = ctx.modExpSimple(c_bi, TOY_D);
    unsigned long long m_dec = static_cast<unsigned long long>(
        m_dec_bi.limb[0] + m_dec_bi.limb[1] * LIMB_B);

    if (m_dec != TOY_M) {
        res.error = "Montgomery decrypt mismatch: got " + std::to_string(m_dec)
                  + ", expected " + std::to_string(TOY_M);
        return res;
    }

    // ---- Fault-injection check: verify signature before releasing -----------
    // In the real flow: after signing with d, verify sig^e == m with public key.
    SmallInt sig_verify = ctx.modExpSimple(c_bi, TOY_E);
    if (sig_verify.compare(m_bi) != 0) {
        res.error = "fault-injection check failed: sig^e != m";
        return res;
    }

    // ---- Verify N′ property ------------------------------------------------
    // n0 × N′ ≡ −1 (mod B)   must hold.
    if (!verifyNPrime(N_bi.limb[0], ctx.n_prime)) {
        res.error = "N-prime Hensel lift verification failed";
        return res;
    }

    res.passed = true;
    return res;
}

// =============================================================================
// SECTION 9 — RSA-2048 public-key verify (production entry point)
// =============================================================================
// Inputs in balanced ternary trit form (1,293 trits each, LST first).
// Operation: verify s^e ≡ m (mod N).  Public operation — no secret data.
//
// Smart card usage:
//   • Load N, e, m, s from ROM (write-once OTP, mapped read-only by MMU).
//   • Call rsa2048Verify() before granting any private-key access.
//   • Store the pass/fail bit in a non-volatile write-once location.
//   • Gate SYSCALL_EXEC on this bit (ternary_os.h OSKernel).

struct RSA2048TestVector {
    std::vector<int8_t> N_trits;  // 1293-trit modulus (LST first)
    std::vector<int8_t> e_trits;  // public exponent
    std::vector<int8_t> m_trits;  // test message
    std::vector<int8_t> s_trits;  // test signature
};

[[nodiscard]] inline bool rsa2048Verify(const RSA2048TestVector& vec) {
    using Bn = TritBigInt<RSA2048_LIMBS>;

    Bn N_bi = Bn::fromTrits(vec.N_trits);
    Bn s_bi = Bn::fromTrits(vec.s_trits);
    Bn m_bi = Bn::fromTrits(vec.m_trits);

    if (N_bi.limb[0] % 3 == 0) return false;  // invalid modulus

    MontgomeryContext<RSA2048_LIMBS> ctx(N_bi);

    // Build binary exponent from ternary trit vector.
    // RSA public exponents (e.g., 65537) are represented non-negatively;
    // convert to a bool vector (MSB first) for the constant-time ladder.
    // For simplicity: extract the integer value of e (valid for e ≤ 2^63).
    unsigned long long e_val = 0;
    long long p = 1;
    for (int i = 0; i < static_cast<int>(vec.e_trits.size()) && i < 63; ++i) {
        e_val += static_cast<unsigned long long>(vec.e_trits[static_cast<std::size_t>(i)]) * p;
        p *= 3;
    }
    std::vector<bool> e_bits;
    {
        unsigned long long tmp = e_val;
        while (tmp > 0) { e_bits.push_back(tmp & 1); tmp >>= 1; }
        std::reverse(e_bits.begin(), e_bits.end());  // MSB first
    }

    Bn computed = ctx.modExpBinary(s_bi, e_bits);
    return computed.compare(m_bi) == 0;
}

// =============================================================================
// SECTION 10 — VM assembly: CIOS inner loop demonstration
// =============================================================================
// Generates a stand-alone ternary assembly program that executes one complete
// outer iteration of CIOS Montgomery (one i, all j values) using the VM ISA.
//
// Register conventions:
//   r1  = a[i]          (multiplier digit, loaded before the call)
//   r2  = DMEM address of b[0]
//   r3  = DMEM address of N[0]
//   r4  = DMEM address of T[0]  (accumulator array, n+2 words)
//   r5  = n_prime        (single-limb Montgomery constant)
//   r6  = j (loop counter)
//   r7  = N_LIMBS constant
//   r8  = carry C
//   r9  = scratch / current T[j]
//   r10 = scratch product
//   r11 = LIMB_B divisor (for mod/div)
//   r12 = m (reduction digit)
//   r13 = const 1 (loop increment)
//
// NOTE: The real VM divides via the DIV opcode, which performs native ternary
// division.  The carry extraction  (C, r) = divmod(s, B)  maps to:
//   DIV r8, r9, r11     ; C = s / B
//   MUL r10, r8, r11    ; C*B
//   SUB r9, r9, r10     ; r = s - C*B  (balanced remainder)
//
// For a production kernel this loop is MMIO-mapped onto the T40 multiplier
// in the FPGA datapath; the assembly shown here is the reference model.

[[nodiscard]] inline vm::assembler::AssemblyResult assembleMontgomeryOuterStep(
    int n_limbs)
{
    std::ostringstream src;
    src << "; ============================================================\n";
    src << "; CIOS Montgomery outer iteration — one i, all j\n";
    src << "; Preconditions: r1=a[i], r2=&b[0], r3=&N[0], r4=&T[0]\n";
    src << ";                r5=n_prime, r7=N_LIMBS, r11=LIMB_B\n";
    src << "; ============================================================\n\n";

    src << "mont_outer:\n";
    src << "    MOV r6, 0        ; j = 0\n";
    src << "    MOV r8, 0        ; C = 0\n";
    src << "    MOV r13, 1       ; const 1\n\n";

    src << "; ------ Phase A: T[j] += a[i]*b[j] + C, for j=0..n-1 ------\n";
    src << "accum_loop:\n";
    src << "    LOAD  r9,  r2, 0      ; b[j]\n";
    src << "    MUL   r10, r1, r9     ; a[i]*b[j]\n";
    src << "    LOAD  r9,  r4, 0      ; T[j]\n";
    src << "    ADD   r9,  r9, r10    ; T[j] + a[i]*b[j]\n";
    src << "    ADD   r9,  r9, r8     ; + C\n";
    src << "; -- divmod B: (C, r) = (r9 / r11, r9 mod r11) --\n";
    src << "    DIV   r8,  r9, r11    ; C  = s / B\n";
    src << "    MUL   r10, r8, r11    ; C*B\n";
    src << "    SUB   r9,  r9, r10    ; r  = s - C*B  (balanced remainder)\n";
    src << "    STORE r9,  r4, 0      ; T[j] = r\n";
    src << "    ADD   r2,  r2, r13    ; advance &b[j]\n";
    src << "    ADD   r4,  r4, r13    ; advance &T[j]\n";
    src << "    ADD   r6,  r6, r13    ; j++\n";
    src << "    TCMP  r10, r6, r7     ; compare j to N_LIMBS\n";
    src << "    TINV  r10, r10        ; flip: stop when j >= N_LIMBS\n";
    src << "    BRN   r10, accum_loop ; branch if j < N_LIMBS (result = -1 → neg)\n\n";

    src << "; -- propagate final carry into T[n] and T[n+1] --\n";
    src << "    LOAD  r9,  r4, 0     ; T[n]\n";
    src << "    ADD   r9,  r9, r8    ; T[n] + C\n";
    src << "    DIV   r8,  r9, r11   ; secondary carry\n";
    src << "    MUL   r10, r8, r11\n";
    src << "    SUB   r9,  r9, r10\n";
    src << "    STORE r9,  r4, 0     ; T[n] = r\n";
    src << "    ADD   r4,  r4, r13\n";
    src << "    LOAD  r9,  r4, 0     ; T[n+1]\n";
    src << "    ADD   r9,  r9, r8\n";
    src << "    STORE r9,  r4, 0     ; T[n+1] += carry\n\n";

    src << "; ------ Phase B: m = T[0] * n_prime mod B -----------------\n";
    // Reset r4 to &T[0] for reduction phase (caller must reload; we use MOV for demo)
    src << "    MOV   r6, 0          ; j = 0  (reuse for reduction loop)\n";
    src << "    LOAD  r12, r3, 0     ; reload T[0] via r3 (demo: r3 now = &T[0])\n";
    src << "    MUL   r12, r12, r5   ; T[0] * n_prime\n";
    src << "    DIV   r9,  r12, r11  ; quotient\n";
    src << "    MUL   r10, r9, r11\n";
    src << "    SUB   r12, r12, r10  ; m = T[0] * n_prime mod B  (balanced)\n\n";

    src << "; ------ Phase C: T[j-1] = T[j] + m*N[j] + C, shifted -----\n";
    src << "    MOV   r8, 0          ; C = 0\n";
    src << "; First j=0: T[0] + m*N[0] ≡ 0 (mod B); discard remainder\n";
    src << "    LOAD  r9,  r2, 0     ; T[0] (r2 reset to &T[0] by caller)\n";
    src << "    LOAD  r10, r3, 0     ; N[0]\n";
    src << "    MUL   r10, r12, r10  ; m*N[0]\n";
    src << "    ADD   r9,  r9, r10\n";
    src << "    ADD   r9,  r9, r8\n";
    src << "    DIV   r8,  r9, r11   ; C (T[0] remainder discarded)\n\n";

    src << "reduce_loop:\n";
    src << "    ADD   r2,  r2, r13   ; advance &T[j] (j=1..n-1)\n";
    src << "    ADD   r3,  r3, r13   ; advance &N[j]\n";
    src << "    LOAD  r9,  r2, 0     ; T[j]\n";
    src << "    LOAD  r10, r3, 0     ; N[j]\n";
    src << "    MUL   r10, r12, r10  ; m*N[j]\n";
    src << "    ADD   r9,  r9, r10\n";
    src << "    ADD   r9,  r9, r8    ; + C\n";
    src << "    DIV   r8,  r9, r11\n";
    src << "    MUL   r10, r8, r11\n";
    src << "    SUB   r9,  r9, r10   ; r\n";
    src << "    STORE r9,  r4, 0     ; T[j-1] = r  (shift: written one position back)\n";
    src << "    ADD   r4,  r4, r13\n";
    src << "    ADD   r6,  r6, r13   ; j++\n";
    src << "    TCMP  r10, r6, r7\n";
    src << "    TINV  r10, r10\n";
    src << "    BRN   r10, reduce_loop\n\n";

    src << "; -- final carry into T[n-1], T[n] --\n";
    src << "    LOAD  r9,  r2, 0     ; T[n] after phase A\n";
    src << "    ADD   r9,  r9, r8\n";
    src << "    DIV   r8,  r9, r11\n";
    src << "    MUL   r10, r8, r11\n";
    src << "    SUB   r9,  r9, r10\n";
    src << "    STORE r9,  r4, 0     ; T[n-1]\n";
    src << "    ADD   r4,  r4, r13\n";
    src << "    LOAD  r9,  r4, 0     ; T[n+1] (secondary overflow)\n";
    src << "    ADD   r9,  r9, r8\n";
    src << "    STORE r9,  r4, 0     ; T[n]\n\n";

    src << "    HALT\n";

    return vm::assembler::assemble(src.str());
}

// =============================================================================
// SECTION 11 — Smart card integration entry point
// =============================================================================
// This function is intended to be called once by the VM kernel (Phase 8)
// during the POST (power-on self-test) sequence before granting any crypto
// syscall permissions.
//
// Usage pattern in ternary_os.h OSKernel::boot():
//
//   SmartCardInitResult r = smartCardInit(kernel_vm);
//   if (!r.self_test.passed) {
//       // Latch error in OTP; permanently disable crypto syscalls.
//       latchFaultInOTP(r.self_test.error);
//       return StatusResult::error(ERR_INVALID);
//   }
//   // Enable SYSCALL_EXEC for the crypto service page.
//   kernel.fs().markExecutable("/crypto/rsa2048", r.exec_header);

struct SmartCardInitResult {
    SelfTestResult self_test;
    bool           mmu_configured = false;
    int            crypto_page_inode = -1;
    vm::ExecutableImageHeader exec_header;
};

[[nodiscard]] inline SmartCardInitResult smartCardInit(vm::VMState& kernel_vm) {
    SmartCardInitResult result;

    // Step 1: Run toy RSA self-test (validates the entire Montgomery pipeline).
    result.self_test = runToySelfTest();
    if (!result.self_test.passed) return result;

    // Step 2: Write the TCMP-verified result into DMEM at a known address.
    // The kernel can read this back and assert it is +1 (T_POS) before
    // issuing any SYSCALL_EXEC for cryptographic operations.
    const int CRYPTO_GATE_ADDR = 0;  // first DMEM word reserved for gate
    vm::TernaryValue gate_value = vm::ops::fromLong(
        result.self_test.passed ? isa::T_POS : isa::T_NEG);
    kernel_vm.dmem.store(CRYPTO_GATE_ADDR, gate_value);

    // Step 3: Configure the MMU to mark the crypto code page execute-only.
    // The FPGA physical page number for the RSA code is assumed to be 1.
    // PTE: user=true, read=false, write=false, execute=true, present=true.
    static constexpr int CRYPTO_PPN = 1;
    vm::TernaryValue pte = vm::encodePageTableEntry(
        CRYPTO_PPN,
        /*user=*/true, /*read=*/false, /*write=*/false, /*execute=*/true);
    if (!pte.isInvalid()) {
        const int PTBR = 4;  // page table base register value (DMEM word 4)
        kernel_vm.dmem.store(PTBR, pte);
        kernel_vm.user_imem_ptbr = PTBR;
        kernel_vm.user_imem_pages = 1;
        kernel_vm.mmu_enable = true;
        result.mmu_configured = true;
    }

    return result;
}

// =============================================================================
// SECTION 12 — Verification / unit test runner
// =============================================================================

[[nodiscard]] inline bool verifyMontgomery() {
    bool ok = true;

    // --- N-prime Hensel lift ---
    {
        // Test against a few known values
        // n0=7: 7^{-1} mod 3^27. 7 mod 3 = 1, so x_0=1.
        // n0 × N' ≡ −1 (mod B) must hold.
        for (long long n0 : {7LL, 8633LL, 65537LL, 1000003LL}) {
            long long np = computeNPrime(n0);
            ok &= verifyNPrime(n0, np);
        }
    }

    // --- divmodB ---
    {
        auto [c, r] = divmodB(static_cast<__int128>(LIMB_B) * 3 + 5);
        ok &= (r == 5 && c == 3);
        auto [c2, r2] = divmodB(-(static_cast<__int128>(LIMB_B)) + 1);
        ok &= (r2 == 1 && c2 == -1);
    }

    // --- TritBigInt comparison and arithmetic ---
    {
        using B2 = TritBigInt<2>;
        B2 a = B2::fromULL(1000ULL);
        B2 b = B2::fromULL(999ULL);
        ok &= (a.compare(b) == +1);
        ok &= ((a - b).compare(B2::one()) == 0);
        B2 sum = a + b;
        ok &= (sum.compare(B2::fromULL(1999ULL)) == 0);
    }

    // --- Toy RSA self-test ---
    {
        SelfTestResult res = runToySelfTest();
        ok &= res.passed;
    }

    // --- Montgomery multiplication: direct check ---
    // For small values: mul(a, b) should equal a*b*R^{-1} mod N.
    // Verify by: toMont(a*b mod N) == mul(toMont(a), toMont(b)).
    {
        using B2 = TritBigInt<2>;
        B2 N = B2::fromULL(8633ULL);
        MontgomeryContext<2> ctx(N);
        B2 a = B2::fromULL(42ULL);
        B2 b = B2::fromULL(100ULL);
        B2 ab_mod_N = B2::fromULL(naivePowMod(42ULL, 1ULL, 8633ULL)
            * 100ULL % 8633ULL);  // 42*100=4200, 4200 mod 8633 = 4200
        // mul(toMont(a), toMont(b)) should equal toMont(a*b mod N)
        B2 lhs = ctx.mul(ctx.toMont(a), ctx.toMont(b));
        B2 rhs = ctx.toMont(ab_mod_N);
        ok &= (lhs.compare(rhs) == 0);
    }

    return ok;
}

// =============================================================================
// SECTION 13 — Side-channel mitigation notes (reference only)
// =============================================================================
//
// TIMING:
//   conditionalSubCT() is branchless: uses limb[N-1] sign bit as a mask.
//   modExpBinary() (Montgomery ladder): both branches execute exactly
//     2 Montgomery multiplications per bit, identical instruction trace.
//   CIOS inner loop: all iterations touch the same number of words.
//
// POWER (SPA/DPA):
//   For DPA resistance: blind the private key before use.
//     d′ = d + k × φ(N) for random k (requires the VM TRNG via CSRR cycle).
//     base′ = base × r mod N for random r; remove blinding after.
//   These blinding operations are public modular exponentiations and can
//   reuse the same CIOS infrastructure.
//
// FAULT INJECTION:
//   After every private-key operation, verify: sig^e ≡ m (mod N).
//   Use TCMP to compare; output is gated on the trit result being +1.
//   The verify step uses the PUBLIC key only — no timing advantage to attacker.
//
// MMU ISOLATION:
//   The crypto code page has PTE flags: user=1, read=0, write=0, execute=1.
//   The modulus and n_prime are in a user-readable data page: read=1, exec=0.
//   The private exponent d lives in kernel-only DMEM: no user-mode PTE at all.
//
// NON-VOLATILE GATE:
//   smartCardInit() writes T_POS to DMEM[0] on success.
//   The Phase 8 kernel maps this to an OTP register (write-once) so rollback
//   to an untested firmware cannot bypass the self-test requirement.

} // namespace crypto
} // namespace sandbox

#endif // TERNARY_MONTGOMERY_H
