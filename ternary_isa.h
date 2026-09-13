// =============================================================================
// ternary_isa.h  —  Ternary Instruction Set Architecture
// =============================================================================
//
// THIS FILE INTEGRATES THE ISA CONTRACT.
//
// ARCHITECTURE_MANIFEST.json is authoritative for the public ISA-v2 wire map.
// Generated architecture headers and the versioned codec carry that contract
// into C++. Do not change field offsets, opcode assignments, or register
// conventions without updating the manifest, generator, and all consumers:
//
//   Stage 1  C++ Virtual Machine     ternary_vm.h / ternary_vm.cpp
//   Stage 2  CUDA kernels            ternary_gpu.cu
//   Stage 3  FPGA SystemVerilog      ternary_decode.sv
//   Stage 4  ASIC                    (same decoder, different backend)
//
// Dependencies: ternary_math.h (for LongTriple used in the data path).
// This header itself is self-contained; it does not include ternary_math.h.
//
// =============================================================================
// ARCHITECTURAL DECISIONS (Frozen — see rationale below each)
// =============================================================================
//
// DECISION 1 — INSTRUCTION CONTAINER
//   Instructions are stored in TritWord27, a dedicated 27-trit container backed
//   by uint64_t using a 2-bits-per-trit encoding. This is NOT the same encoding
//   as LongTriple's positional balanced-ternary sum.
//
//   Rationale: Instruction decoding needs simple shift-and-mask field extraction,
//   not arithmetic properties. The 2-bit encoding maps directly to SystemVerilog
//   wire slices with no division or modulo. LongTriple's sentinel values
//   (OVERFLOW_DATA, UNDERFLOW_DATA) must never alias a valid instruction word.
//   Keeping the namespaces separate eliminates that risk permanently.
//
//   Encoding table:
//     stored bits 0b00  →  trit value  -1  (T_NEG)
//     stored bits 0b01  →  trit value   0  (T_ZER)
//     stored bits 0b10  →  trit value  +1  (T_POS)
//     stored bits 0b11  →  INVALID (trap on decode)
//
//   Trit i occupies bits [2i+1 : 2i] of the uint64_t backing word.
//   Bits [63:54] are always zero (27 trits × 2 bits = 54 bits used).
//
// DECISION 2 — IMMEDIATE SIGN CONVENTION
//   All immediates (I-type imm16, B-type offset19) are balanced ternary and
//   therefore naturally signed. There is no unsigned immediate format. The range
//   of a k-trit balanced ternary immediate is:
//
//     min = -( 3^k - 1 ) / 2
//     max = +( 3^k - 1 ) / 2
//
//   For imm16:   ±21,523,360      (covers the default 1,000,000-word DMEM)
//   For offset19: ±581,130,733    (sufficient for any realistic program image)
//
//   All immediate decode calls go through decodeSigned(), which is the single
//   canonical implementation of this contract. Do not inline the decode logic.
//
// DECISION 3 — EXCEPTION / TRAP MODEL
//   A dedicated trap register r27 lives outside the 27-register general file.
//   It is written by the VM on any fault and is read-only from the ISA.
//   Without configured trap routing, the VM halts after writing r27. With
//   routing, synchronous faults remain visible until ERET; syscalls and timer
//   interrupts use CSR_CAUSE and do not masquerade as legacy r27 faults.
//
//   r27 is encoded as a two-field ternary fault record:
//     trit[0] = fault_valid. 0 means no fault, +1 means fault_class is valid.
//     trit[1] = fault_class:
//       T_NEG (-1)  TRAP_DIV_ZERO      divide by zero
//       T_ZER ( 0)  TRAP_MEM_FAULT     out-of-range memory access
//       T_POS (+1)  TRAP_ILLEGAL_OP    unknown opcode or 0b11 trit in word
//
//   On clean execution r27 holds TRAP_NONE (fault_valid = 0,
//   all other trits zero — distinct from all fault codes).
//   Routed ERET restores that no-fault state. A fault inside an active handler
//   is terminal and preserves the original EPC/privilege save frame.
//
// =============================================================================

#pragma once
#ifndef TERNARY_ISA_H
#define TERNARY_ISA_H

#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <array>
#include <string>
#include <stdexcept>
#include "generated/architecture_contract.h"

// The v3 executable/function profile keeps the v2 instruction encoding.  These
// constants describe the opt-in vector/context envelope.  VCTX operations use
// a private escape selector in the v2 wire space and are deliberately exposed
// through the owned semantic codec below; the generated v2 dispatcher must
// still opt in before executing them.
namespace sandbox::architecture::v3 {
inline constexpr int ISA_VERSION = architecture::v2::ISA_VERSION;
inline constexpr int EXECUTABLE_VERSION = 3;
inline constexpr int FUNCTION_ABI_VERSION = 3;
inline constexpr int VECTOR_ABI_VERSION = 1;
inline constexpr int SYSCALL_ABI_VERSION = architecture::v2::SYSCALL_ABI_VERSION;
inline constexpr int VECTOR_REGISTER_COUNT = 8;
inline constexpr int VECTOR_LANE_COUNT = 27;
inline constexpr int VECTOR_LANE_WORD_TRITS = architecture::v2::SCALAR_WORD_TRITS;
inline constexpr int VECTOR_CONTEXT_WORDS = 279;
inline constexpr int VECTOR_SPILL_WORDS = VECTOR_LANE_COUNT;

// Feature trits 0..8 are the v2 feature set.  The v3 envelope requires an
// explicit profile bit plus independent geometry/context/spill declarations.
inline constexpr int FEATURE_EXECUTABLE_ABI_V3 = 9;
inline constexpr int FEATURE_VECTOR_ABI_V3 = FEATURE_EXECUTABLE_ABI_V3;
inline constexpr int FEATURE_VECTOR_GEOMETRY = 10;
inline constexpr int FEATURE_VECTOR_CONTEXT = 11;
inline constexpr int FEATURE_VECTOR_SPILL = 12;
inline constexpr int FEATURE_V3_LAST = FEATURE_VECTOR_SPILL;

inline constexpr std::uint64_t featureBit(int trit) {
    return std::uint64_t{1} << static_cast<unsigned>(trit);
}

inline constexpr std::uint64_t SUPPORTED_FEATURES =
    (std::uint64_t{1} << static_cast<unsigned>(FEATURE_V3_LAST + 1)) - 1;
inline constexpr std::uint64_t REQUIRED_FEATURES =
    featureBit(architecture::v2::FEATURE_BASE_V2) |
    featureBit(architecture::v2::FEATURE_VECTOR) |
    featureBit(FEATURE_VECTOR_CONTEXT);
} // namespace sandbox::architecture::v3

namespace sandbox {
namespace isa {

// =============================================================================
// SECTION 1 — TritWord27: The Instruction Container
// =============================================================================

// Symbolic trit values used throughout this header.
static constexpr int8_t T_NEG = -1;
static constexpr int8_t T_ZER =  0;
static constexpr int8_t T_POS = +1;

// Number of trits in one instruction word.
static constexpr int ISA_WORD_TRITS = 27;

// Number of general-purpose registers (r0 through r26).
static constexpr int REG_COUNT = 27;

// Offset applied when storing a register index [0..26] into a 3-trit
// balanced ternary field [-13..+13]:  stored_value = register_index - REG_FIELD_OFFSET.
static constexpr int REG_FIELD_OFFSET = 13;

// Index of the dedicated trap register (outside the general file).
static constexpr int REG_TRAP = 27;

struct TritWord27 {
    uint64_t bits = 0;  // 2 bits per trit; bits[63:54] always 0.

    // -------------------------------------------------------------------------
    // getTrit / setTrit — primary field accessors.
    // Position 0 is the least significant trit (LST).
    // Position 26 is the most significant trit (MST) — the format discriminant.
    // -------------------------------------------------------------------------
    [[nodiscard]] int8_t getTrit(int pos) const {
        assert(pos >= 0 && pos < ISA_WORD_TRITS);
        uint64_t raw = (bits >> (2 * pos)) & 0x3ULL;
        // 0b00 → -1,  0b01 → 0,  0b10 → +1,  0b11 → invalid (caller handles)
        if (raw == 0b11) return T_NEG;  // treated as fault upstream
        return static_cast<int8_t>(raw) - 1;
    }

    // Returns true if the slot at pos contains the invalid 0b11 pattern.
    [[nodiscard]] bool isMalformed(int pos) const {
        assert(pos >= 0 && pos < ISA_WORD_TRITS);
        return ((bits >> (2 * pos)) & 0x3ULL) == 0b11;
    }

    void setTrit(int pos, int8_t val) {
        assert(pos >= 0 && pos < ISA_WORD_TRITS);
        assert(val >= -1 && val <= 1);
        uint64_t encoded = static_cast<uint64_t>(val + 1);  // -1→0, 0→1, +1→2
        uint64_t mask    = 0x3ULL << (2 * pos);
        bits = (bits & ~mask) | (encoded << (2 * pos));
    }

    // -------------------------------------------------------------------------
    // pack / unpack — bulk conversions for assembly and disassembly.
    // -------------------------------------------------------------------------
    static TritWord27 pack(const std::array<int8_t, ISA_WORD_TRITS>& trits) {
        TritWord27 w;
        for (int i = 0; i < ISA_WORD_TRITS; ++i) w.setTrit(i, trits[i]);
        return w;
    }

    [[nodiscard]] std::array<int8_t, ISA_WORD_TRITS> unpack() const {
        std::array<int8_t, ISA_WORD_TRITS> out{};
        for (int i = 0; i < ISA_WORD_TRITS; ++i) out[i] = getTrit(i);
        return out;
    }

    // Convenience: read a multi-trit field as a signed integer.
    // lsb = index of least-significant trit of the field, width = number of trits.
    [[nodiscard]] int getField(int lsb, int width) const {
        assert(lsb >= 0 && lsb + width <= ISA_WORD_TRITS);
        int val = 0, p3 = 1;
        for (int i = 0; i < width; ++i) {
            val += getTrit(lsb + i) * p3;
            p3 *= 3;
        }
        return val;
    }

    void setField(int lsb, int width, int val) {
        for (int i = 0; i < width; ++i) {
            int rem = val % 3;
            // Balanced remainder: pull into {-1, 0, +1}
            if (rem >  1) rem -= 3;
            if (rem < -1) rem += 3;
            setTrit(lsb + i, static_cast<int8_t>(rem));
            val = (val - rem) / 3;
        }
    }

    bool operator==(const TritWord27& o) const { return bits == o.bits; }
    bool operator!=(const TritWord27& o) const { return bits != o.bits; }
};

// Compile-time check: 27 trits × 2 bits/trit = 54 bits < 64.
static_assert(ISA_WORD_TRITS * 2 <= 64,
    "TritWord27 trit count exceeds uint64_t capacity.");

// =============================================================================
// SECTION 2 — Immediate Decode (Decision 2: single canonical implementation)
// =============================================================================

// Decode a k-trit balanced ternary field from a TritWord27 as a signed integer.
// This is the ONLY place the immediate sign convention is implemented.
// All LOAD/STORE offsets, MOV immediates, and branch offsets call this function.
[[nodiscard]] inline int decodeSigned(const TritWord27& w, int lsb, int width) {
    return w.getField(lsb, width);  // balanced ternary is inherently signed
}

// Encode a signed integer into a k-trit balanced ternary field.
// Throws if the value is out of range for the given width.
inline void encodeSigned(TritWord27& w, int lsb, int width, int val) {
    // Range: -(3^width - 1)/2  to  +(3^width - 1)/2
    int p3w = 1;
    for (int i = 0; i < width; ++i) p3w *= 3;
    int limit = (p3w - 1) / 2;
    if (val < -limit || val > limit) {
        throw std::out_of_range("encodeSigned: value exceeds field range");
    }
    w.setField(lsb, width, val);
}

// =============================================================================
// SECTION 3 — Instruction Word Layout (Field Offsets)
// =============================================================================
// All field positions are given as (lsb, width) pairs into the TritWord27.
// Trit index 0 = LST (least significant), index 26 = MST (format discriminant).
//
// ┌───────────────────────────────────────────────────────────────────────┐
// │                    R-TYPE  (fmt trit[26] = +1)                        │
// ├─────┬────────┬─────┬──────┬──────┬──────┬──────────────────────────┤
// │ fmt │ opcode │ Rd  │ Rs1  │ Rs2  │ func │        pad (reserved)      │
// │  1  │   4    │  3  │  3   │  3   │  3   │           10               │
// │[26] │[25:22] │[21:19]│[18:16]│[15:13]│[12:10]│         [9:0]          │
// └─────┴────────┴─────┴──────┴──────┴──────┴──────────────────────────┘
//
// ┌───────────────────────────────────────────────────────────────────────┐
// │                    I-TYPE  (fmt trit[26] = 0)                         │
// ├─────┬────────┬─────┬──────┬────────────────────────────────────────┤
// │ fmt │ opcode │ Rd  │ Rs1  │              imm16 (signed)              │
// │  1  │   4    │  3  │  3   │                  16                      │
// │[26] │[25:22] │[21:19]│[18:16]│               [15:0]                  │
// └─────┴────────┴─────┴──────┴────────────────────────────────────────┘
//
// ┌───────────────────────────────────────────────────────────────────────┐
// │                    B-TYPE  (fmt trit[26] = -1)                        │
// ├─────┬────────┬───────┬──────────────────────────────────────────────┤
// │ fmt │ opcode │  Rs   │              offset19 (signed)                 │
// │  1  │   4    │   3   │                    19                          │
// │[26] │[25:22] │[21:19]│                  [18:0]                        │
// └─────┴────────┴───────┴──────────────────────────────────────────────┘
//
// IMMEDIATE RANGES:
//   imm16:    ±21,523,360   — covers the default 1,000,000-word DMEM span
//   offset19: ±581,130,733  — covers any realistic program image

// Format discriminant
static constexpr int FIELD_FMT_LSB   = 26;  static constexpr int FIELD_FMT_W   = 1;

// Opcode field (shared by all formats)
static constexpr int FIELD_OP_LSB    = 22;  static constexpr int FIELD_OP_W    = 4;

// R-type fields
static constexpr int FIELD_RD_LSB    = 19;  static constexpr int FIELD_RD_W    = 3;
static constexpr int FIELD_RS1_LSB   = 16;  static constexpr int FIELD_RS1_W   = 3;
static constexpr int FIELD_RS2_LSB   = 13;  static constexpr int FIELD_RS2_W   = 3;
static constexpr int FIELD_FUNC_LSB  = 10;  static constexpr int FIELD_FUNC_W  = 3;
static constexpr int FIELD_PAD_LSB   =  0;  static constexpr int FIELD_PAD_W   = 10;

// Extended R4 layout for TWCMP and TCLAMP:
// [fmt:1 | opcode:4 | rd:3 | rs1:3 | rs2:3 | rs3:3 | func:3 | reserved:4]
static constexpr int FIELD_R4_RD_LSB   = 19;  static constexpr int FIELD_R4_RD_W   = 3;
static constexpr int FIELD_R4_RS1_LSB  = 16;  static constexpr int FIELD_R4_RS1_W  = 3;
static constexpr int FIELD_R4_RS2_LSB  = 13;  static constexpr int FIELD_R4_RS2_W  = 3;
static constexpr int FIELD_R4_RS3_LSB  = 10;  static constexpr int FIELD_R4_RS3_W  = 3;
static constexpr int FIELD_R4_FUNC_LSB =  7;  static constexpr int FIELD_R4_FUNC_W = 3;

// Extended R5 layout for TSEL:
// [fmt:1 | opcode:4 | rd:3 | rCond:3 | rNeg:3 | rZero:3 | rPos:3 | reserved:7]
static constexpr int FIELD_R5_RD_LSB    = 19;  static constexpr int FIELD_R5_RD_W    = 3;
static constexpr int FIELD_R5_COND_LSB  = 16;  static constexpr int FIELD_R5_COND_W  = 3;
static constexpr int FIELD_R5_NEG_LSB   = 13;  static constexpr int FIELD_R5_NEG_W   = 3;
static constexpr int FIELD_R5_ZERO_LSB  = 10;  static constexpr int FIELD_R5_ZERO_W  = 3;
static constexpr int FIELD_R5_POS_LSB   =  7;  static constexpr int FIELD_R5_POS_W   = 3;
static constexpr int FIELD_R5_RSVD_LSB  =  0;  static constexpr int FIELD_R5_RSVD_W  = 7;
// VSEL reuses R5 register fields and stores its width suffix in reserved trits.
static constexpr int FIELD_R5_FUNC_LSB  =  0;  static constexpr int FIELD_R5_FUNC_W  = 3;

// I-type fields (Rd and Rs1 share positions with R-type)
static constexpr int FIELD_IMM16_LSB =  0;  static constexpr int FIELD_IMM16_W = 16;

// Private semantic staging overlay for VLOAD/VSTORE. Public ISA-v2 words use
// [fmt:1 | EXT:4 | vRd/vSrc:3 | rBase:3 | func:3 | selector:4 | imm9:9].
static constexpr int FIELD_VMEM_FUNC_LSB = 13; static constexpr int FIELD_VMEM_FUNC_W = 3;
static constexpr int FIELD_VMEM_IMM_LSB  =  0; static constexpr int FIELD_VMEM_IMM_W  = 13;

// B-type fields (Rs shares position with Rd in R/I-type)
static constexpr int FIELD_BRS_LSB   = 19;  static constexpr int FIELD_BRS_W   = 3;
static constexpr int FIELD_OFF19_LSB =  0;  static constexpr int FIELD_OFF19_W = 19;

// =============================================================================
// SECTION 4 — Instruction Formats
// =============================================================================

enum class InstructionFormat : int8_t {
    B_TYPE = T_NEG,  // -1  Control flow: branch, jump, call, ret
    I_TYPE = T_ZER,  //  0  Immediate: load constant, load/store with offset
    R_TYPE = T_POS,  // +1  Register-register: arithmetic, compare, logic
    INVALID = 2      // Decoded from 0b11 bit pattern — triggers TRAP_ILLEGAL_OP
};

// =============================================================================
// SECTION 5 — Opcode Definitions
// =============================================================================
// Wire opcodes occupy a 4-trit field (positions [25:22]) read as an unsigned
// base-3 integer (0–80). The enum below names semantic operations; its numeric
// values are not necessarily public wire opcodes. VersionedInstructionCodec
// applies the authoritative ISA-v2 direct/extension map.
//
// TCMP CONTRACT (critical for ternary branching):
//   TCMP Rd, Rs1, Rs2 computes sign(Rs1 - Rs2) and writes the result to Rd.
//   The result is always one of { T_NEG, T_ZER, T_POS } stored in trit[0] of Rd
//   (all other trits of Rd are set to zero after TCMP).
//   This single result is the input to BRN — there is no FLAGS register.
//   BRN Rs, offset branches to PC + offset when Rs.trit[0] == T_NEG.
//   A three-way branch is:
//       TCMP  r3, r1, r2            ; r3 = sign(r1 - r2)
//       BRN   r3, negative_label    ; taken if r3 == -1
//       TINV  r4, r3                ; r4 = -r3  (zero stays zero)
//       BRN   r4, positive_label    ; taken if original result was +1
//       ; fall through for r3 == 0

enum class Opcode : uint8_t {
    // --- System ---
    NOP   =  0,   // No operation. PC advances.
    HALT  =  1,   // Stop execution. VM enters HALTED state.

    // --- Data Movement ---
    MOV   =  2,   // I-type: Rd ← imm16              (load small constant)
    MOVH  =  3,   // I-type: Rd ← imm16 << 16        (load upper half)
    COPY  =  4,   // R-type: Rd ← Rs1                (register copy)

    // --- Arithmetic (R-type, operate on LongTriple registers) ---
    ADD   =  5,   // R-type: Rd ← Rs1 + Rs2
    SUB   =  6,   // R-type: Rd ← Rs1 - Rs2
    MUL   =  7,   // R-type: Rd ← Rs1 × Rs2
    DIV   =  8,   // R-type: Rd ← Rs1 ÷ Rs2   (DIV by zero → TRAP_DIV_ZERO)
    SQRT  =  9,   // R-type: Rd ← √Rs1
    NEG   = 10,   // R-type: Rd ← -Rs1         (trit flip, exact)
    ABS   = 11,   // R-type: Rd ← |Rs1|

    // --- Compare & Ternary Logic ---
    TCMP  = 12,   // R-type: Rd ← sign(Rs1 - Rs2) ∈ {-1, 0, +1}  (see contract above)
    TMIN  = 13,   // R-type: Rd ← min(Rs1, Rs2)   (trit-wise)
    TMAX  = 14,   // R-type: Rd ← max(Rs1, Rs2)   (trit-wise)
    TINV  = 15,   // R-type: Rd ← -Rs1             (alias for NEG in logic context)

    // --- Memory ---
    LOAD  = 16,   // I-type: Rd ← mem[Rs1 + imm16]  (word-addressed LongTriple)
    STORE = 17,   // I-type: mem[Rs1 + imm16] ← Rd  (Rd acts as source; Rd field = source reg)

    // --- Control Flow ---
    JMP   = 18,   // B-type: PC ← PC + offset19      (unconditional relative jump)
    BRN   = 19,   // B-type: if Rs.trit[0] == T_NEG: PC ← PC + offset19
    CALL  = 20,   // B-type: r25 ← PC+1; PC ← PC + offset19
    RET   = 21,   // R-type (no operands): PC ← r25
    CVT   = 22,   // R-type: Rd = convert Rs1 to width selected by func
    TSEL  = 23,   // R5-type: Rd = rNeg/rZero/rPos selected by T1 condition
    BRZ   = 24,   // B-type: if Rs.trit[0] == T_ZER: PC changes by offset19
    BRP   = 25,   // B-type: if Rs.trit[0] == T_POS: PC changes by offset19
    SWAP  = 26,   // R-type: swap Rd and Rs1

    // --- Scalar Lane Logic ---
    TLADD = 27,   // R-type: carryless per-trit lane add
    TLSUB = 28,   // R-type: carryless per-trit lane subtract
    TLNEG = 29,   // R-type: per-trit lane negation
    TLAND = 30,   // R-type: per-trit lattice min
    TLOR  = 31,   // R-type: per-trit lattice max

    // --- Vector Numeric Foundation ---
    VADD   = 32,
    VSUB   = 33,
    VNEG   = 34,
    VMUL   = 35,
    VDIV   = 36,
    VCMP   = 37,
    VSEL   = 38,
    VLOAD  = 39,
    VSTORE = 40,
    VBCAST = 41,
    VLEN   = 42,

    // --- Accumulator and T1 AI Operations ---
    ACLR   = 43,
    ALOAD  = 44,
    AADD   = 45,
    ASUB   = 46,
    AMUL   = 47,
    ASTORE = 48,
    VDOT   = 49,
    VMAC   = 50,
    VACT   = 51,

    // --- Vector Plumbing and Indexed Memory ---
    VPACK    = 52,
    VUNPACK  = 53,
    VPERMUTE = 54,
    VBLEND   = 55,
    VSWAP    = 56,
    VGATHER  = 57,
    VSCATTER = 58,

    // --- Phase 2 ISA Extension ---
    TWCMP    = 59,
    CALLR    = 60,
    JMPR     = 61,
    TMOD     = 62,
    TLSHIFT  = 63,
    TRSHIFT  = 64,
    TMAC     = 65,
    TCOUNT   = 66,
    TSCAN    = 67,
    TCLAMP   = 68,
    SYSCALL  = 69,
    FENCE    = 70,
    VSUM     = 71,
    VHMIN    = 72,
    VHMAX    = 73,

    // --- Phase 3 OS Substrate ---
    CSRR     = 74,
    CSRW     = 75,
    ERET     = 76,
    CSRRW    = 77,
    TLDR     = 78,
    TSTR     = 79,


    // v2 semantic operations; these values are not direct wire opcodes.
    WAIT     = 80,
    TLBINV   = 81,

    // v3 privileged vector-context operations. These are semantic opcodes;
    // their escape selectors are outside the v2 generated selector table and
    // are handled by encodeVectorContext/decodeVectorContext below.
    VCTXSTORE = 82,
    VCTXLOAD  = 83,

    // --- Reserved semantic sentinel ---
    // Public wire reservations are defined by ARCHITECTURE_MANIFEST.json and
    // enforced by VersionedInstructionCodec.
    RESERVED = 255  // Sentinel — never encoded into an instruction word.
};

enum class IsaEncodingVersion : uint8_t {
    V2 = architecture::v2::ISA_VERSION,
};

static constexpr uint8_t OPCODE_MAX_ASSIGNED = 79;  // TSTR
static constexpr uint8_t OPCODE_RESERVED_START = 80;

static constexpr int VCTXSTORE_ESCAPE_SELECTOR =
    architecture::v3::VCTXSTORE_SELECTOR;
static constexpr int VCTXLOAD_ESCAPE_SELECTOR =
    architecture::v3::VCTXLOAD_SELECTOR;
static constexpr int FIELD_VCTX_REG_LSB =
    architecture::v3::VCTX_REGISTER_LSB;
static constexpr int FIELD_VCTX_REG_W =
    architecture::v3::VCTX_REGISTER_WIDTH;
static constexpr int FIELD_VCTX_SELECTOR_LSB =
    architecture::v3::VCTX_SELECTOR_LSB;
static constexpr int FIELD_VCTX_SELECTOR_W =
    architecture::v3::VCTX_SELECTOR_WIDTH;
static constexpr int FIELD_VCTX_RSVD_LSB =
    architecture::v3::VCTX_RESERVED_LSB;
static constexpr int FIELD_VCTX_RSVD_W =
    architecture::v3::VCTX_RESERVED_WIDTH;
static constexpr int FIELD_VCTX_OFFSET_LSB =
    architecture::v3::VCTX_OFFSET_LSB;
static constexpr int FIELD_VCTX_OFFSET_W =
    architecture::v3::VCTX_OFFSET_WIDTH;

static constexpr uint8_t FUNC_T1  =  8;
static constexpr uint8_t FUNC_T5  =  9;
static constexpr uint8_t FUNC_T10 = 10;
static constexpr uint8_t FUNC_T20 = 11;
static constexpr uint8_t FUNC_T40 = 12;
static constexpr uint8_t FUNC_T50 = 13;
static constexpr uint8_t FUNC_L1  = 14;
static constexpr uint8_t FUNC_L5  = 15;
static constexpr uint8_t FUNC_L10 = 16;
static constexpr uint8_t FUNC_L20 = 17;
static constexpr uint8_t FUNC_L40 = 18;
static constexpr uint8_t FUNC_L50 = 19;
static constexpr uint8_t FUNC_DEFAULT = FUNC_T40;

static constexpr int ATOMIC_ORDER_RELAXED = T_NEG;
static constexpr int ATOMIC_ORDER_ACQ_REL = T_ZER;
static constexpr int ATOMIC_ORDER_SEQ_CST = T_POS;
static constexpr uint8_t FUNC_ORDER_RELAXED = static_cast<uint8_t>(FUNC_DEFAULT + ATOMIC_ORDER_RELAXED);
static constexpr uint8_t FUNC_ORDER_ACQ_REL = static_cast<uint8_t>(FUNC_DEFAULT + ATOMIC_ORDER_ACQ_REL);
static constexpr uint8_t FUNC_ORDER_SEQ_CST = static_cast<uint8_t>(FUNC_DEFAULT + ATOMIC_ORDER_SEQ_CST);

[[nodiscard]] inline bool isAtomicOrder(int order) {
    return order >= ATOMIC_ORDER_RELAXED && order <= ATOMIC_ORDER_SEQ_CST;
}

[[nodiscard]] inline uint8_t atomicOrderFunc(int order) {
    return isAtomicOrder(order)
        ? static_cast<uint8_t>(FUNC_DEFAULT + order)
        : FUNC_ORDER_ACQ_REL;
}

[[nodiscard]] inline bool isAtomicOrderFunc(uint8_t func) {
    return isAtomicOrder(static_cast<int>(func) - static_cast<int>(FUNC_DEFAULT));
}

[[nodiscard]] inline int atomicOrderFromFunc(uint8_t func) {
    return static_cast<int>(func) - static_cast<int>(FUNC_DEFAULT);
}

[[nodiscard]] inline std::string atomicOrderSuffix(uint8_t func) {
    switch (atomicOrderFromFunc(func)) {
        case ATOMIC_ORDER_RELAXED: return ".-1";
        case ATOMIC_ORDER_ACQ_REL: return ".0";
        case ATOMIC_ORDER_SEQ_CST: return ".+1";
        default: return ".?";
    }
}

[[nodiscard]] inline bool isNumericWidthFunc(uint8_t func) {
    return func == FUNC_T1  || func == FUNC_T5  || func == FUNC_T10 ||
           func == FUNC_T20 || func == FUNC_T40 || func == FUNC_T50;
}

[[nodiscard]] inline bool isLaneWidthFunc(uint8_t func) {
    return func == FUNC_L1  || func == FUNC_L5  || func == FUNC_L10 ||
           func == FUNC_L20 || func == FUNC_L40 || func == FUNC_L50;
}

[[nodiscard]] inline bool isWidthFunc(uint8_t func) {
    return isNumericWidthFunc(func) || isLaneWidthFunc(func);
}

[[nodiscard]] inline int widthFuncTrits(uint8_t func) {
    switch (func) {
        case FUNC_T1:  case FUNC_L1:  return 1;
        case FUNC_T5:  case FUNC_L5:  return 5;
        case FUNC_T10: case FUNC_L10: return 10;
        case FUNC_T20: case FUNC_L20: return 20;
        case FUNC_T40: case FUNC_L40: return 40;
        case FUNC_T50: case FUNC_L50: return 50;
        default: return 0;
    }
}

[[nodiscard]] inline uint8_t matchingNumericFunc(uint8_t func) {
    switch (func) {
        case FUNC_T1: case FUNC_L1: return FUNC_T1;
        case FUNC_T5: case FUNC_L5: return FUNC_T5;
        case FUNC_T10: case FUNC_L10: return FUNC_T10;
        case FUNC_T20: case FUNC_L20: return FUNC_T20;
        case FUNC_T40: case FUNC_L40: return FUNC_T40;
        case FUNC_T50: case FUNC_L50: return FUNC_T50;
        default: return 0;
    }
}

[[nodiscard]] inline uint8_t matchingLaneFunc(uint8_t func) {
    switch (func) {
        case FUNC_T1: case FUNC_L1: return FUNC_L1;
        case FUNC_T5: case FUNC_L5: return FUNC_L5;
        case FUNC_T10: case FUNC_L10: return FUNC_L10;
        case FUNC_T20: case FUNC_L20: return FUNC_L20;
        case FUNC_T40: case FUNC_L40: return FUNC_L40;
        case FUNC_T50: case FUNC_L50: return FUNC_L50;
        default: return 0;
    }
}

[[nodiscard]] inline std::string widthFuncSuffix(uint8_t func) {
    switch (func) {
        case FUNC_T1:  return ".t1";
        case FUNC_T5:  return ".t5";
        case FUNC_T10: return ".t10";
        case FUNC_T20: return ".t20";
        case FUNC_T40: return ".t40";
        case FUNC_T50: return ".t50";
        case FUNC_L1:  return ".l1";
        case FUNC_L5:  return ".l5";
        case FUNC_L10: return ".l10";
        case FUNC_L20: return ".l20";
        case FUNC_L40: return ".l40";
        case FUNC_L50: return ".l50";
        default:       return ".?";
    }
}

static constexpr int VECTOR_REGISTER_COUNT = 8;
static constexpr int VECTOR_LANE_COUNT = architecture::v3::VECTOR_LANE_COUNT;

// =============================================================================
// SECTION 6 — Register Conventions
// =============================================================================
// Registers r0..r26 form the general-purpose file.
// r27 is the trap register (outside the general file, read-only from ISA).

static constexpr uint8_t R0_ZERO =  0;  // Hardwired zero. Writes are discarded.
static constexpr uint8_t R25_LR  = 25;  // Link register. Written by CALL.
static constexpr uint8_t R26_SP  = 26;  // Stack pointer. Convention: grows negative.
static constexpr uint8_t R27_TRAP = 27; // Trap register. Written by VM on fault.

// Recommended general-purpose aliases (callee-saved: r1–r12, caller-saved: r13–r24)
static constexpr uint8_t R1  =  1;
static constexpr uint8_t R2  =  2;
static constexpr uint8_t R3  =  3;
static constexpr uint8_t R4  =  4;
static constexpr uint8_t R5  =  5;
static constexpr uint8_t R6  =  6;
static constexpr uint8_t R7  =  7;
static constexpr uint8_t R8  =  8;
static constexpr uint8_t R9  =  9;
static constexpr uint8_t R10 = 10;
static constexpr uint8_t R11 = 11;
static constexpr uint8_t R12 = 12;

// =============================================================================
// SECTION 7 — Trap / Fault Codes (Decision 3)
// =============================================================================
// Written to r27 on any fault. VM halts immediately after writing.
// r27 is initialized to TRAP_NONE on VM reset.

enum class TrapCode : int8_t {
    TRAP_DIV_ZERO    = T_NEG,  // -1: division by zero detected in DIV
    TRAP_MEM_FAULT   = T_ZER,  //  0: address out of range in LOAD or STORE
    TRAP_ILLEGAL_OP  = T_POS,  // +1: unknown opcode or malformed instruction word
};

static constexpr int8_t FAULT_VALID_NONE = T_ZER;
static constexpr int8_t FAULT_VALID_SET  = T_POS;

// TRAP_NONE is stored as fault_valid = 0. fault_class is meaningful only when
// fault_valid = +1, which keeps no-fault distinct from TRAP_MEM_FAULT.

// Phase 3 routed trap causes. Positive values are synchronous exceptions;
// negative values are interrupts.
static constexpr int OS_CAUSE_ILLEGAL_INSTRUCTION = 1;
static constexpr int OS_CAUSE_FETCH_FAULT         = 2;
static constexpr int OS_CAUSE_LOAD_FAULT          = 3;
static constexpr int OS_CAUSE_STORE_FAULT         = 4;
static constexpr int OS_CAUSE_PROTECTION_FAULT    = 5;
static constexpr int OS_CAUSE_DIV_ZERO            = 6;
static constexpr int OS_CAUSE_SYSCALL             = 7;
static constexpr int OS_CAUSE_FETCH_PAGE_FAULT    = 8;
static constexpr int OS_CAUSE_LOAD_PAGE_FAULT     = 9;
static constexpr int OS_CAUSE_STORE_PAGE_FAULT    = 10;
static constexpr int OS_CAUSE_TIMER_IRQ           = -1;

static constexpr int OS_PAGE_ACCESS_FETCH = T_NEG;
static constexpr int OS_PAGE_ACCESS_LOAD  = T_ZER;
static constexpr int OS_PAGE_ACCESS_STORE = T_POS;

enum class PrivilegeMode : int8_t {
    Kernel     = T_NEG,
    Supervisor = T_ZER,
    User       = T_POS,
};

#define TRIT_CSR_ALIAS(name) \
    inline constexpr int CSR_##name = architecture::v2::CSR_##name
TRIT_CSR_ALIAS(EPC);
TRIT_CSR_ALIAS(CAUSE);
TRIT_CSR_ALIAS(STATUS);
TRIT_CSR_ALIAS(TVEC);
TRIT_CSR_ALIAS(SCRATCH);
TRIT_CSR_ALIAS(CYCLE);
TRIT_CSR_ALIAS(TIMER_RELOAD);
TRIT_CSR_ALIAS(TIMER_COUNTER);
TRIT_CSR_ALIAS(TIMER_ENABLE);
TRIT_CSR_ALIAS(TIMER_PENDING);
TRIT_CSR_ALIAS(USER_IMEM_BASE);
TRIT_CSR_ALIAS(USER_IMEM_LIMIT);
TRIT_CSR_ALIAS(USER_DMEM_BASE);
TRIT_CSR_ALIAS(USER_DMEM_LIMIT);
TRIT_CSR_ALIAS(SYSCALL_ID);
TRIT_CSR_ALIAS(MMU_ENABLE);
TRIT_CSR_ALIAS(USER_IMEM_PTBR);
TRIT_CSR_ALIAS(USER_IMEM_PAGES);
TRIT_CSR_ALIAS(USER_DMEM_PTBR);
TRIT_CSR_ALIAS(USER_DMEM_PAGES);
TRIT_CSR_ALIAS(PAGE_FAULT_ADDR);
TRIT_CSR_ALIAS(PAGE_FAULT_ACCESS);
TRIT_CSR_ALIAS(CONSOLE_OUT);
TRIT_CSR_ALIAS(CONSOLE_CTRL);
TRIT_CSR_ALIAS(CONSOLE_IN);
TRIT_CSR_ALIAS(CONSOLE_IN_CTRL);
TRIT_CSR_ALIAS(MOUSE_X);
TRIT_CSR_ALIAS(MOUSE_Y);
TRIT_CSR_ALIAS(MOUSE_BTN);
TRIT_CSR_ALIAS(GPU_X1);
TRIT_CSR_ALIAS(GPU_Y1);
TRIT_CSR_ALIAS(GPU_X2);
TRIT_CSR_ALIAS(GPU_Y2);
TRIT_CSR_ALIAS(GPU_COLOR);
TRIT_CSR_ALIAS(GPU_CMD);
TRIT_CSR_ALIAS(GPU_PAGE);
TRIT_CSR_ALIAS(GPU_DRAW_BASE);
TRIT_CSR_ALIAS(GPU_MODE);
TRIT_CSR_ALIAS(SPRITE_X);
TRIT_CSR_ALIAS(SPRITE_Y);
TRIT_CSR_ALIAS(SPRITE_ATTR);
TRIT_CSR_ALIAS(BLOCK_INDEX);
TRIT_CSR_ALIAS(BLOCK_ADDR);
TRIT_CSR_ALIAS(BLOCK_CMD);
TRIT_CSR_ALIAS(BLOCK_STATUS);
TRIT_CSR_ALIAS(BLOCK_COUNT);
TRIT_CSR_ALIAS(BLOCK_WORDS);
TRIT_CSR_ALIAS(POWER_CONTROL);
TRIT_CSR_ALIAS(ISA_VERSION);
TRIT_CSR_ALIAS(ISA_FEATURES);
TRIT_CSR_ALIAS(MMU_BASE_PAGE_WORDS);
TRIT_CSR_ALIAS(MMU_SUPERPAGE_WORDS);
TRIT_CSR_ALIAS(ASID);
#undef TRIT_CSR_ALIAS
inline constexpr int CSR_MAX_ID = architecture::v2::CSR_MAX_ID;

[[nodiscard]] inline bool isValidCSR(int id) {
    return architecture::v2::csrDescriptor(id) != nullptr;
}

[[nodiscard]] inline const char* csrToString(int id) {
    const auto* descriptor = architecture::v2::csrDescriptor(id);
    return descriptor != nullptr ? descriptor->name.data() : "unknown";
}

[[nodiscard]] inline bool csrAccessAllows(
    architecture::v2::CsrAccessLevel access,
    PrivilegeMode privilege) {
    using architecture::v2::CsrAccessLevel;
    if (access == CsrAccessLevel::NONE) return false;
    if (privilege == PrivilegeMode::Kernel) return true;
    if (privilege == PrivilegeMode::Supervisor) {
        return access == CsrAccessLevel::SUPERVISOR ||
               access == CsrAccessLevel::USER;
    }
    return access == CsrAccessLevel::USER;
}

[[nodiscard]] inline bool canReadCSR(int id, PrivilegeMode privilege) {
    const auto* descriptor = architecture::v2::csrDescriptor(id);
    return descriptor != nullptr &&
           csrAccessAllows(descriptor->read_access, privilege);
}

[[nodiscard]] inline bool canWriteCSR(int id, PrivilegeMode privilege) {
    const auto* descriptor = architecture::v2::csrDescriptor(id);
    return descriptor != nullptr &&
           csrAccessAllows(descriptor->write_access, privilege);
}

#include "generated/architecture_isa_v2.h"

// =============================================================================
// SECTION 8 — Decoded Instruction Word
// =============================================================================
// InstructionWord is the C++ representation of a decoded semantic operation.
// It is transient and is never stored in memory or a register. Its static
// encodeSemantic*/decodeSemantic helpers implement the assembler's private
// staging representation, not the public ISA wire contract. Public ISA v2
// words must pass through VersionedInstructionCodec.

struct InstructionWord {
    InstructionFormat fmt    = InstructionFormat::INVALID;
    Opcode            opcode = Opcode::RESERVED;

    // R-type fields
    uint8_t rd   = 0;
    uint8_t rs1  = 0;
    uint8_t rs2  = 0;
    uint8_t func = 0;

    // Extended R4 fields for window ops.
    bool    r4_layout = false;
    uint8_t rs3       = 0;

    // Extended R5 fields, currently used by TSEL.
    bool    r5_layout = false;
    uint8_t rcond = 0;
    uint8_t rneg  = 0;
    uint8_t rzero = 0;
    uint8_t rpos  = 0;

    // I-type and B-type immediates (signed, balanced ternary decoded)
    int imm    = 0;  // I-type: 16-trit signed immediate
    int offset = 0;  // B-type: 19-trit signed offset

    // B-type condition register (reuses rd field position)
    uint8_t rs_branch = 0;

    // STORE source register — aliases the rd field position but semantically
    // represents the data source, not a destination.
    uint8_t rs_store = 0;

    // True if the raw TritWord27 contained any 0b11 trit pattern.
    bool malformed = false;

    // -------------------------------------------------------------------------
    // decodeSemantic — decode the private semantic staging representation.
    // Production fetch/decode and external tools must instead call
    // VersionedInstructionCodec::decode() for the selected ISA version.
    // -------------------------------------------------------------------------
    [[nodiscard]] static InstructionWord decodeSemantic(const TritWord27& w) {
        InstructionWord iw;

        // Check for any malformed trit (0b11 pattern) in the entire word.
        for (int i = 0; i < ISA_WORD_TRITS; ++i) {
            if (w.isMalformed(i)) { iw.malformed = true; return iw; }
        }

        // --- Format discriminant (trit[26]) ---
        int8_t fmtTrit = w.getTrit(FIELD_FMT_LSB);
        switch (fmtTrit) {
            case T_POS: iw.fmt = InstructionFormat::R_TYPE; break;
            case T_ZER: iw.fmt = InstructionFormat::I_TYPE; break;
            case T_NEG: iw.fmt = InstructionFormat::B_TYPE; break;
            default:    iw.malformed = true; return iw;
        }

        // --- Opcode (trits[25:22], read as unsigned base-3 integer) ---
        int rawOp = 0, p3 = 1;
        for (int i = 0; i < FIELD_OP_W; ++i) {
            int8_t t = w.getTrit(FIELD_OP_LSB + i);
            // Convert balanced trit (-1, 0, +1) to unsigned base-3 digit (0, 1, 2).
            rawOp += (t + 1) * p3;
            p3 *= 3;
        }
        if (rawOp >= OPCODE_RESERVED_START) {
            // Reserved opcode — still decode fields, let VM raise TRAP_ILLEGAL_OP.
            iw.opcode = Opcode::RESERVED;
        } else {
            iw.opcode = static_cast<Opcode>(rawOp);
        }

        // --- Format-specific field decode ---
        switch (iw.fmt) {
            case InstructionFormat::R_TYPE:
                if (iw.opcode == Opcode::TSEL || iw.opcode == Opcode::VSEL ||
                    iw.opcode == Opcode::VBLEND) {
                    iw.r5_layout = true;
                    iw.rd    = static_cast<uint8_t>(w.getField(FIELD_R5_RD_LSB,   FIELD_R5_RD_W)   + REG_FIELD_OFFSET);
                    iw.rcond = static_cast<uint8_t>(w.getField(FIELD_R5_COND_LSB, FIELD_R5_COND_W) + REG_FIELD_OFFSET);
                    iw.rneg  = static_cast<uint8_t>(w.getField(FIELD_R5_NEG_LSB,  FIELD_R5_NEG_W)  + REG_FIELD_OFFSET);
                    iw.rzero = static_cast<uint8_t>(w.getField(FIELD_R5_ZERO_LSB, FIELD_R5_ZERO_W) + REG_FIELD_OFFSET);
                    iw.rpos  = static_cast<uint8_t>(w.getField(FIELD_R5_POS_LSB,  FIELD_R5_POS_W)  + REG_FIELD_OFFSET);
                    iw.rs1   = iw.rcond;
                    iw.rs2   = iw.rneg;
                    iw.func  = (iw.opcode == Opcode::VSEL || iw.opcode == Opcode::VBLEND)
                        ? static_cast<uint8_t>(w.getField(FIELD_R5_FUNC_LSB, FIELD_R5_FUNC_W) + REG_FIELD_OFFSET)
                        : FUNC_DEFAULT;
                } else if (iw.opcode == Opcode::TWCMP || iw.opcode == Opcode::TCLAMP ||
                           iw.opcode == Opcode::TSTR) {
                    iw.r4_layout = true;
                    iw.rd   = static_cast<uint8_t>(w.getField(FIELD_R4_RD_LSB,   FIELD_R4_RD_W)   + REG_FIELD_OFFSET);
                    iw.rs1  = static_cast<uint8_t>(w.getField(FIELD_R4_RS1_LSB,  FIELD_R4_RS1_W)  + REG_FIELD_OFFSET);
                    iw.rs2  = static_cast<uint8_t>(w.getField(FIELD_R4_RS2_LSB,  FIELD_R4_RS2_W)  + REG_FIELD_OFFSET);
                    iw.rs3  = static_cast<uint8_t>(w.getField(FIELD_R4_RS3_LSB,  FIELD_R4_RS3_W)  + REG_FIELD_OFFSET);
                    iw.func = static_cast<uint8_t>(w.getField(FIELD_R4_FUNC_LSB, FIELD_R4_FUNC_W) + REG_FIELD_OFFSET);
                } else {
                    iw.rd   = static_cast<uint8_t>(w.getField(FIELD_RD_LSB,   FIELD_RD_W)   + REG_FIELD_OFFSET);
                    iw.rs1  = static_cast<uint8_t>(w.getField(FIELD_RS1_LSB,  FIELD_RS1_W)  + REG_FIELD_OFFSET);
                    iw.rs2  = static_cast<uint8_t>(w.getField(FIELD_RS2_LSB,  FIELD_RS2_W)  + REG_FIELD_OFFSET);
                    iw.func = static_cast<uint8_t>(w.getField(FIELD_FUNC_LSB, FIELD_FUNC_W) + REG_FIELD_OFFSET);
                }
                break;

            case InstructionFormat::I_TYPE:
                iw.rd       = static_cast<uint8_t>(w.getField(FIELD_RD_LSB,  FIELD_RD_W)  + REG_FIELD_OFFSET);
                iw.rs1      = static_cast<uint8_t>(w.getField(FIELD_RS1_LSB, FIELD_RS1_W) + REG_FIELD_OFFSET);
                iw.rs_store = iw.rd;  // STORE uses the rd-position field as source
                if (iw.opcode == Opcode::VLOAD || iw.opcode == Opcode::VSTORE) {
                    iw.func = static_cast<uint8_t>(w.getField(FIELD_VMEM_FUNC_LSB, FIELD_VMEM_FUNC_W) + REG_FIELD_OFFSET);
                    iw.imm  = decodeSigned(w, FIELD_VMEM_IMM_LSB, FIELD_VMEM_IMM_W);
                } else {
                    iw.imm  = decodeSigned(w, FIELD_IMM16_LSB, FIELD_IMM16_W);
                }
                break;

            case InstructionFormat::B_TYPE:
                iw.rs_branch = static_cast<uint8_t>(w.getField(FIELD_BRS_LSB, FIELD_BRS_W) + REG_FIELD_OFFSET);
                iw.offset    = decodeSigned(w, FIELD_OFF19_LSB, FIELD_OFF19_W);
                break;

            default:
                iw.malformed = true;
        }

        // Validate register indices are in range [0, REG_COUNT-1].
        // Note: register fields are stored as balanced ternary (-13..+13 for 3 trits)
        // and shifted by +1 above to produce [0..26]. Clamp check:
        auto checkReg = [&](uint8_t r) {
            if (r >= REG_COUNT) iw.malformed = true;
        };
        if (iw.fmt == InstructionFormat::R_TYPE) {
            if (iw.r5_layout) {
                checkReg(iw.rd); checkReg(iw.rcond); checkReg(iw.rneg);
                checkReg(iw.rzero); checkReg(iw.rpos);
            } else if (iw.r4_layout) {
                checkReg(iw.rd); checkReg(iw.rs1); checkReg(iw.rs2); checkReg(iw.rs3);
            } else {
                checkReg(iw.rd); checkReg(iw.rs1); checkReg(iw.rs2);
            }
        } else if (iw.fmt == InstructionFormat::I_TYPE) {
            checkReg(iw.rd); checkReg(iw.rs1);
        } else if (iw.fmt == InstructionFormat::B_TYPE) {
            checkReg(iw.rs_branch);
        }

        return iw;
    }

    // -------------------------------------------------------------------------
    // Semantic encode helpers build the assembler's private staging word.
    // They are not public ISA encoders; VersionedInstructionCodec owns the
    // canonical wire representation.
    // -------------------------------------------------------------------------

    [[nodiscard]] static TritWord27 encodeSemanticR(Opcode op,
                                            uint8_t rd, uint8_t rs1, uint8_t rs2,
                                            uint8_t func = FUNC_DEFAULT) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_POS);
        setOpcode(w, op);
        // Shift register index [0..26] back to balanced trit field [-13..+13].
        w.setField(FIELD_RD_LSB,   FIELD_RD_W,   static_cast<int>(rd)   - REG_FIELD_OFFSET);
        w.setField(FIELD_RS1_LSB,  FIELD_RS1_W,  static_cast<int>(rs1)  - REG_FIELD_OFFSET);
        w.setField(FIELD_RS2_LSB,  FIELD_RS2_W,  static_cast<int>(rs2)  - REG_FIELD_OFFSET);
        w.setField(FIELD_FUNC_LSB, FIELD_FUNC_W, static_cast<int>(func) - REG_FIELD_OFFSET);
        return w;
    }

    [[nodiscard]] static TritWord27 encodeSemanticR4(Opcode op,
                                             uint8_t rd,
                                             uint8_t rs1,
                                             uint8_t rs2,
                                             uint8_t rs3,
                                             uint8_t func = FUNC_DEFAULT) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_POS);
        setOpcode(w, op);
        w.setField(FIELD_R4_RD_LSB,   FIELD_R4_RD_W,   static_cast<int>(rd)   - REG_FIELD_OFFSET);
        w.setField(FIELD_R4_RS1_LSB,  FIELD_R4_RS1_W,  static_cast<int>(rs1)  - REG_FIELD_OFFSET);
        w.setField(FIELD_R4_RS2_LSB,  FIELD_R4_RS2_W,  static_cast<int>(rs2)  - REG_FIELD_OFFSET);
        w.setField(FIELD_R4_RS3_LSB,  FIELD_R4_RS3_W,  static_cast<int>(rs3)  - REG_FIELD_OFFSET);
        w.setField(FIELD_R4_FUNC_LSB, FIELD_R4_FUNC_W, static_cast<int>(func) - REG_FIELD_OFFSET);
        return w;
    }

    [[nodiscard]] static TritWord27 encodeSemanticR5(Opcode op,
                                             uint8_t rd,
                                             uint8_t rcond,
                                             uint8_t rneg,
                                             uint8_t rzero,
                                             uint8_t rpos,
                                             uint8_t func = FUNC_DEFAULT) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_POS);
        setOpcode(w, op);
        w.setField(FIELD_R5_RD_LSB,   FIELD_R5_RD_W,   static_cast<int>(rd)    - REG_FIELD_OFFSET);
        w.setField(FIELD_R5_COND_LSB, FIELD_R5_COND_W, static_cast<int>(rcond) - REG_FIELD_OFFSET);
        w.setField(FIELD_R5_NEG_LSB,  FIELD_R5_NEG_W,  static_cast<int>(rneg)  - REG_FIELD_OFFSET);
        w.setField(FIELD_R5_ZERO_LSB, FIELD_R5_ZERO_W, static_cast<int>(rzero) - REG_FIELD_OFFSET);
        w.setField(FIELD_R5_POS_LSB,  FIELD_R5_POS_W,  static_cast<int>(rpos)  - REG_FIELD_OFFSET);
        if (op == Opcode::VSEL || op == Opcode::VBLEND) {
            w.setField(FIELD_R5_FUNC_LSB, FIELD_R5_FUNC_W, static_cast<int>(func) - REG_FIELD_OFFSET);
        }
        return w;
    }

    [[nodiscard]] static TritWord27 encodeSemanticI(Opcode op,
                                            uint8_t rd, uint8_t rs1,
                                            int imm) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_ZER);
        setOpcode(w, op);
        w.setField(FIELD_RD_LSB,  FIELD_RD_W,  static_cast<int>(rd)  - REG_FIELD_OFFSET);
        w.setField(FIELD_RS1_LSB, FIELD_RS1_W, static_cast<int>(rs1) - REG_FIELD_OFFSET);
        encodeSigned(w, FIELD_IMM16_LSB, FIELD_IMM16_W, imm);
        return w;
    }

    // STORE-type encoding: semantically identical to encodeI but names the
    // rd-position field as rs_store (source data) and rs1 as rs_base (address).
    [[nodiscard]] static TritWord27 encodeSemanticS(Opcode op,
                                            uint8_t rs_store, uint8_t rs_base,
                                            int imm) {
        return encodeSemanticI(op, rs_store, rs_base, imm);
    }

    [[nodiscard]] static TritWord27 encodeSemanticVectorMemory(Opcode op,
                                                       uint8_t vreg,
                                                       uint8_t base,
                                                       int imm,
                                                       uint8_t func) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_ZER);
        setOpcode(w, op);
        w.setField(FIELD_RD_LSB,        FIELD_RD_W,        static_cast<int>(vreg) - REG_FIELD_OFFSET);
        w.setField(FIELD_RS1_LSB,       FIELD_RS1_W,       static_cast<int>(base) - REG_FIELD_OFFSET);
        w.setField(FIELD_VMEM_FUNC_LSB, FIELD_VMEM_FUNC_W, static_cast<int>(func) - REG_FIELD_OFFSET);
        encodeSigned(w, FIELD_VMEM_IMM_LSB, FIELD_VMEM_IMM_W, imm);
        return w;
    }

    [[nodiscard]] static TritWord27 encodeSemanticB(Opcode op,
                                            uint8_t rs_branch,
                                            int offset) {
        TritWord27 w;
        w.setTrit(FIELD_FMT_LSB, T_NEG);
        setOpcode(w, op);
        w.setField(FIELD_BRS_LSB, FIELD_BRS_W, static_cast<int>(rs_branch) - REG_FIELD_OFFSET);
        encodeSigned(w, FIELD_OFF19_LSB, FIELD_OFF19_W, offset);
        return w;
    }

private:
    // Encode opcode value into the 4-trit opcode field.
    // Opcode is stored as unsigned base-3 in balanced-trit slots:
    // digit d (0,1,2) stored as trit (d-1) = (-1,0,+1).
    static void setOpcode(TritWord27& w, Opcode op) {
        uint8_t val = static_cast<uint8_t>(op);
        for (int i = 0; i < FIELD_OP_W; ++i) {
            uint8_t digit = val % 3;
            w.setTrit(FIELD_OP_LSB + i, static_cast<int8_t>(digit) - 1);
            val /= 3;
        }
    }
};

#include "generated/architecture_instruction_codec.h"

#include "architecture_v2_support.h"

// =============================================================================
// SECTION 8a — v3 privileged vector-context escape operations
// =============================================================================
//
// The generated v2 selector table is intentionally immutable for v2
// compatibility.  VCTXSTORE/VCTXLOAD therefore use an owned, unambiguous
// escape form: I-format, raw opcode ESCAPE_OPCODE, a context base register at
// [21:19], a 5-trit selector at [18:14], two neutral reserved trits at [13:12],
// and a 12-trit signed offset at [11:0]. The context address is the scalar base
// register plus the signed offset. V2 decoders continue to see the escape as
// reserved; v3-capable loaders dispatch through these helpers only after the
// VECTOR_CONTEXT feature and kernel privilege checks succeed.

struct VectorContextInstruction {
    bool valid = false;
    Opcode opcode = Opcode::RESERVED;
    uint8_t context_register = R0_ZERO;
    int offset = 0;
};

[[nodiscard]] inline bool isVectorContextOpcode(Opcode opcode) {
    return opcode == Opcode::VCTXSTORE || opcode == Opcode::VCTXLOAD;
}

[[nodiscard]] inline std::uint64_t requiredVectorContextFeatures(
    const VectorContextInstruction& instruction) {
    return instruction.valid && isVectorContextOpcode(instruction.opcode)
        ? featureBit(architecture::v3::FEATURE_VECTOR_CONTEXT)
        : 0;
}

[[nodiscard]] inline TritWord27 encodeVectorContext(
    Opcode opcode,
    uint8_t context_register,
    int offset = 0) {
    if (!isVectorContextOpcode(opcode) || context_register >= REG_COUNT) {
        throw std::invalid_argument("invalid vector-context opcode/register");
    }
    TritWord27 word;
    word.setTrit(FIELD_FMT_LSB, T_ZER);
    encodeUnsignedField(
        word, FIELD_OP_LSB, FIELD_OP_W, architecture::v3::VCTX_WIRE_OPCODE);
    word.setField(
        FIELD_VCTX_REG_LSB, FIELD_VCTX_REG_W,
        static_cast<int>(context_register) - REG_FIELD_OFFSET);
    encodeUnsignedField(
        word, FIELD_VCTX_SELECTOR_LSB, FIELD_VCTX_SELECTOR_W,
        opcode == Opcode::VCTXSTORE
            ? VCTXSTORE_ESCAPE_SELECTOR
            : VCTXLOAD_ESCAPE_SELECTOR);
    word.setField(FIELD_VCTX_RSVD_LSB, FIELD_VCTX_RSVD_W, 0);
    encodeSigned(word, FIELD_VCTX_OFFSET_LSB, FIELD_VCTX_OFFSET_W, offset);
    return word;
}

[[nodiscard]] inline VectorContextInstruction decodeVectorContext(
    const TritWord27& word) {
    VectorContextInstruction decoded;

    for (int trit = 0; trit < ISA_WORD_TRITS; ++trit) {
        if (word.isMalformed(trit)) return decoded;
    }
    if (decodeUnsignedField(word, FIELD_OP_LSB, FIELD_OP_W) !=
            architecture::v3::VCTX_WIRE_OPCODE ||
        word.getTrit(FIELD_FMT_LSB) != T_ZER) {
        return decoded;
    }
    if (word.getField(FIELD_VCTX_RSVD_LSB, FIELD_VCTX_RSVD_W) != 0) {
        return decoded;
    }
    const int selector = decodeUnsignedField(
        word, FIELD_VCTX_SELECTOR_LSB, FIELD_VCTX_SELECTOR_W);
    if (selector == VCTXSTORE_ESCAPE_SELECTOR) {
        decoded.opcode = Opcode::VCTXSTORE;
    } else if (selector == VCTXLOAD_ESCAPE_SELECTOR) {
        decoded.opcode = Opcode::VCTXLOAD;
    } else {
        return decoded;
    }
    const int context_register =
        word.getField(FIELD_VCTX_REG_LSB, FIELD_VCTX_REG_W) +
        REG_FIELD_OFFSET;
    if (context_register < 0 || context_register >= REG_COUNT) return {};
    decoded.valid = true;
    decoded.context_register = static_cast<uint8_t>(context_register);
    decoded.offset = decodeSigned(
        word, FIELD_VCTX_OFFSET_LSB, FIELD_VCTX_OFFSET_W);
    return decoded;
}

[[nodiscard]] inline bool isPrivilegedVectorContextOpcode(
    const VectorContextInstruction& instruction) {
    return instruction.valid && isVectorContextOpcode(instruction.opcode);
}

// =============================================================================
// SECTION 9 — Round-Trip Verification
// =============================================================================
// Verify that the private semantic staging representation round-trips. This is
// not a public ISA-v2 wire-contract check; those tests use
// VersionedInstructionCodec.

inline bool verifySemanticRoundTrip() {
    bool ok = true;

    // R-type: ADD r3, r1, r2
    {
        TritWord27 w = InstructionWord::encodeSemanticR(Opcode::ADD, R3, R1, R2);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt    == InstructionFormat::R_TYPE);
        ok &= (iw.opcode == Opcode::ADD);
        ok &= (iw.rd     == R3);
        ok &= (iw.rs1    == R1);
        ok &= (iw.rs2    == R2);
    }

    // I-type: MOV r5, 12157  (a plausible constant)
    {
        TritWord27 w = InstructionWord::encodeSemanticI(Opcode::MOV, R5, R0_ZERO, 12157);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt    == InstructionFormat::I_TYPE);
        ok &= (iw.opcode == Opcode::MOV);
        ok &= (iw.rd     == R5);
        ok &= (iw.imm    == 12157);
    }

    // I-type: LOAD r4, r26 + (-8)  (load from stack)
    {
        TritWord27 w = InstructionWord::encodeSemanticI(Opcode::LOAD, R4, R26_SP, -8);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt    == InstructionFormat::I_TYPE);
        ok &= (iw.opcode == Opcode::LOAD);
        ok &= (iw.rd     == R4);
        ok &= (iw.rs1    == R26_SP);
        ok &= (iw.imm    == -8);
    }

    // B-type: BRN r3, -5  (branch back 5 words if r3 is negative)
    {
        TritWord27 w = InstructionWord::encodeSemanticB(Opcode::BRN, R3, -5);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt       == InstructionFormat::B_TYPE);
        ok &= (iw.opcode    == Opcode::BRN);
        ok &= (iw.rs_branch == R3);
        ok &= (iw.offset    == -5);
    }

    // B-type: HALT  (encoded as a B-type NOP with offset 0)
    {
        TritWord27 w = InstructionWord::encodeSemanticB(Opcode::HALT, R0_ZERO, 0);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.opcode == Opcode::HALT);
    }

    // R5-type: TSEL r6, r3, r1, r2, r4
    {
        TritWord27 w = InstructionWord::encodeSemanticR5(Opcode::TSEL, R6, R3, R1, R2, R4);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt == InstructionFormat::R_TYPE);
        ok &= (iw.opcode == Opcode::TSEL);
        ok &= iw.r5_layout;
        ok &= (iw.rd == R6);
        ok &= (iw.rcond == R3);
        ok &= (iw.rneg == R1);
        ok &= (iw.rzero == R2);
        ok &= (iw.rpos == R4);
    }

    // R5-type: VSEL.t20 v6, v3, v1, v2, v4
    {
        TritWord27 w = InstructionWord::encodeSemanticR5(Opcode::VSEL, 6, 3, 1, 2, 4, FUNC_T20);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.fmt == InstructionFormat::R_TYPE);
        ok &= (iw.opcode == Opcode::VSEL);
        ok &= iw.r5_layout;
        ok &= (iw.rd == 6);
        ok &= (iw.rcond == 3);
        ok &= (iw.rneg == 1);
        ok &= (iw.rzero == 2);
        ok &= (iw.rpos == 4);
        ok &= (iw.func == FUNC_T20);
    }

    // Negative: reserved opcode must decode to Opcode::RESERVED
    {
        TritWord27 w{};
        w.setTrit(FIELD_FMT_LSB, T_POS);
        // Force opcode field to value 80 (reserved)
        uint8_t val = 80;
        for (int i = 0; i < FIELD_OP_W; ++i) {
            uint8_t d = val % 3; val /= 3;
            w.setTrit(FIELD_OP_LSB + i, static_cast<int8_t>(d) - 1);
        }
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= (iw.opcode == Opcode::RESERVED);
    }

    // Phase 4 reserved family: assigned opcodes decode and have names now,
    // even when the VM implementation intentionally traps later families.
    for (uint8_t op = 27; op <= OPCODE_MAX_ASSIGNED; ++op) {
        TritWord27 w = InstructionWord::encodeSemanticR(static_cast<Opcode>(op), R3, R1, R2);
        InstructionWord iw = InstructionWord::decodeSemantic(w);
        ok &= !iw.malformed;
        ok &= (iw.opcode == static_cast<Opcode>(op));
    }

    return ok;
}

// =============================================================================
// SECTION 10 — Human-Readable Disassembly (for debugging and test output)
// =============================================================================

[[nodiscard]] inline std::string opcodeToString(Opcode op) {
    switch (op) {
        case Opcode::NOP:   return "NOP";
        case Opcode::HALT:  return "HALT";
        case Opcode::MOV:   return "MOV";
        case Opcode::MOVH:  return "MOVH";
        case Opcode::COPY:  return "COPY";
        case Opcode::ADD:   return "ADD";
        case Opcode::SUB:   return "SUB";
        case Opcode::MUL:   return "MUL";
        case Opcode::DIV:   return "DIV";
        case Opcode::SQRT:  return "SQRT";
        case Opcode::NEG:   return "NEG";
        case Opcode::ABS:   return "ABS";
        case Opcode::TCMP:  return "TCMP";
        case Opcode::TMIN:  return "TMIN";
        case Opcode::TMAX:  return "TMAX";
        case Opcode::TINV:  return "TINV";
        case Opcode::LOAD:  return "LOAD";
        case Opcode::STORE: return "STORE";
        case Opcode::JMP:   return "JMP";
        case Opcode::BRN:   return "BRN";
        case Opcode::CALL:  return "CALL";
        case Opcode::RET:   return "RET";
        case Opcode::CVT:   return "CVT";
        case Opcode::TSEL:  return "TSEL";
        case Opcode::BRZ:   return "BRZ";
        case Opcode::BRP:   return "BRP";
        case Opcode::SWAP:  return "SWAP";
        case Opcode::TLADD: return "TLADD";
        case Opcode::TLSUB: return "TLSUB";
        case Opcode::TLNEG: return "TLNEG";
        case Opcode::TLAND: return "TLAND";
        case Opcode::TLOR:  return "TLOR";
        case Opcode::VADD:  return "VADD";
        case Opcode::VSUB:  return "VSUB";
        case Opcode::VNEG:  return "VNEG";
        case Opcode::VMUL:  return "VMUL";
        case Opcode::VDIV:  return "VDIV";
        case Opcode::VCMP:  return "VCMP";
        case Opcode::VSEL:  return "VSEL";
        case Opcode::VLOAD: return "VLOAD";
        case Opcode::VSTORE: return "VSTORE";
        case Opcode::VBCAST: return "VBCAST";
        case Opcode::VLEN:   return "VLEN";
        case Opcode::ACLR:   return "ACLR";
        case Opcode::ALOAD:  return "ALOAD";
        case Opcode::AADD:   return "AADD";
        case Opcode::ASUB:   return "ASUB";
        case Opcode::AMUL:   return "AMUL";
        case Opcode::ASTORE: return "ASTORE";
        case Opcode::VDOT:   return "VDOT";
        case Opcode::VMAC:   return "VMAC";
        case Opcode::VACT:   return "VACT";
        case Opcode::VPACK:  return "VPACK";
        case Opcode::VUNPACK: return "VUNPACK";
        case Opcode::VPERMUTE: return "VPERMUTE";
        case Opcode::VBLEND: return "VBLEND";
        case Opcode::VSWAP:  return "VSWAP";
        case Opcode::VGATHER: return "VGATHER";
        case Opcode::VSCATTER: return "VSCATTER";
        case Opcode::TWCMP:   return "TWCMP";
        case Opcode::CALLR:   return "CALLR";
        case Opcode::JMPR:    return "JMPR";
        case Opcode::TMOD:    return "TMOD";
        case Opcode::TLSHIFT: return "TLSHIFT";
        case Opcode::TRSHIFT: return "TRSHIFT";
        case Opcode::TMAC:    return "TMAC";
        case Opcode::TCOUNT:  return "TCOUNT";
        case Opcode::TSCAN:   return "TSCAN";
        case Opcode::TCLAMP:  return "TCLAMP";
        case Opcode::SYSCALL: return "SYSCALL";
        case Opcode::FENCE:   return "FENCE";
        case Opcode::VSUM:    return "VSUM";
        case Opcode::VHMIN:   return "VHMIN";
        case Opcode::VHMAX:   return "VHMAX";
        case Opcode::CSRR:    return "CSRR";
        case Opcode::CSRW:    return "CSRW";
        case Opcode::ERET:    return "ERET";
        case Opcode::CSRRW:   return "CSRRW";
        case Opcode::TLDR:    return "TLDR";
        case Opcode::TSTR:    return "TSTR";
        case Opcode::WAIT:    return "WAIT";
        case Opcode::TLBINV:  return "TLBINV";
        default:            return "???";
    }
}

[[nodiscard]] inline std::string disassemble(const InstructionWord& iw) {
    if (iw.malformed) return "<MALFORMED>";

    // System instructions that use no operands.
    if (iw.opcode == Opcode::NOP)  return "NOP";
    if (iw.opcode == Opcode::HALT) return "HALT";
    if (iw.opcode == Opcode::RET)  return "RET";
    if (iw.opcode == Opcode::ERET) return "ERET";
    if (iw.opcode == Opcode::WAIT) return "WAIT";

    std::string mnemonic = opcodeToString(iw.opcode);
    if (iw.opcode == Opcode::CVT && iw.fmt == InstructionFormat::R_TYPE) {
        if (isWidthFunc(iw.rs2)) mnemonic += widthFuncSuffix(iw.rs2);
        if (isWidthFunc(iw.func)) mnemonic += widthFuncSuffix(iw.func);
    } else if ((iw.opcode == Opcode::VSEL || iw.opcode == Opcode::VBLEND) &&
               iw.fmt == InstructionFormat::R_TYPE) {
        if (isNumericWidthFunc(iw.func)) mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::TLADD || iw.opcode == Opcode::TLSUB ||
         iw.opcode == Opcode::TLNEG || iw.opcode == Opcode::TLAND ||
         iw.opcode == Opcode::TLOR) &&
        isLaneWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::FENCE || iw.opcode == Opcode::TLDR ||
         iw.opcode == Opcode::TSTR) &&
        iw.func != FUNC_DEFAULT && isAtomicOrderFunc(iw.func)) {
        mnemonic += atomicOrderSuffix(iw.func);
    } else if (iw.r4_layout && isNumericWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::TMOD || iw.opcode == Opcode::TLSHIFT ||
         iw.opcode == Opcode::TRSHIFT || iw.opcode == Opcode::TMAC ||
         iw.opcode == Opcode::TCOUNT || iw.opcode == Opcode::TSCAN) &&
        isNumericWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::ADD || iw.opcode == Opcode::SUB ||
         iw.opcode == Opcode::MUL || iw.opcode == Opcode::DIV ||
         iw.opcode == Opcode::SQRT || iw.opcode == Opcode::NEG ||
         iw.opcode == Opcode::ABS || iw.opcode == Opcode::TCMP ||
         iw.opcode == Opcode::TMIN || iw.opcode == Opcode::TMAX ||
         iw.opcode == Opcode::TINV || iw.opcode == Opcode::CVT) &&
        iw.func != FUNC_DEFAULT) {
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::VADD || iw.opcode == Opcode::VSUB ||
         iw.opcode == Opcode::VNEG || iw.opcode == Opcode::VMUL ||
         iw.opcode == Opcode::VDIV || iw.opcode == Opcode::VCMP ||
         iw.opcode == Opcode::VBCAST || iw.opcode == Opcode::VDOT ||
         iw.opcode == Opcode::VMAC || iw.opcode == Opcode::VACT ||
         iw.opcode == Opcode::ACLR || iw.opcode == Opcode::ALOAD ||
         iw.opcode == Opcode::AADD || iw.opcode == Opcode::ASUB ||
         iw.opcode == Opcode::AMUL || iw.opcode == Opcode::ASTORE ||
         iw.opcode == Opcode::VPERMUTE || iw.opcode == Opcode::VGATHER ||
         iw.opcode == Opcode::VSCATTER || iw.opcode == Opcode::VSUM ||
         iw.opcode == Opcode::VHMIN || iw.opcode == Opcode::VHMAX) &&
        isNumericWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::R_TYPE &&
        (iw.opcode == Opcode::VPACK || iw.opcode == Opcode::VUNPACK) &&
        isNumericWidthFunc(iw.rs2) && isNumericWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.rs2);
        mnemonic += widthFuncSuffix(iw.func);
    } else if (iw.fmt == InstructionFormat::I_TYPE &&
        (iw.opcode == Opcode::VLOAD || iw.opcode == Opcode::VSTORE) &&
        isNumericWidthFunc(iw.func)) {
        mnemonic += widthFuncSuffix(iw.func);
    }

    std::string s = mnemonic + " ";
    switch (iw.fmt) {
        case InstructionFormat::R_TYPE:
            if (iw.opcode == Opcode::TSEL) {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rcond)
                   + ", r" + std::to_string(iw.rneg)
                   + ", r" + std::to_string(iw.rzero)
                   + ", r" + std::to_string(iw.rpos);
            } else if (iw.opcode == Opcode::CSRRW) {
                s += "r" + std::to_string(iw.rd)
                   + ", " + std::string(csrToString(iw.rs2))
                   + ", r" + std::to_string(iw.rs1);
            } else if (iw.r4_layout) {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1)
                   + ", r" + std::to_string(iw.rs2)
                   + ", r" + std::to_string(iw.rs3);
            } else if (iw.opcode == Opcode::VSEL) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rcond)
                   + ", v" + std::to_string(iw.rneg)
                   + ", v" + std::to_string(iw.rzero)
                   + ", v" + std::to_string(iw.rpos);
            } else if (iw.opcode == Opcode::VBLEND) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rcond)
                   + ", v" + std::to_string(iw.rneg)
                   + ", v" + std::to_string(iw.rpos);
            } else if (iw.opcode == Opcode::VLEN) {
                s += "r" + std::to_string(iw.rd);
            } else if (iw.opcode == Opcode::ACLR) {
                s.pop_back();
            } else if (iw.opcode == Opcode::ALOAD ||
                       iw.opcode == Opcode::AADD ||
                       iw.opcode == Opcode::ASUB ||
                       iw.opcode == Opcode::AMUL) {
                s += "r" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::ASTORE) {
                s += "r" + std::to_string(iw.rd);
            } else if (iw.opcode == Opcode::VBCAST) {
                s += "v" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::VACT) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::VMAC) {
                s += "v" + std::to_string(iw.rs1)
                   + ", v" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::VDOT) {
                s += "r" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1)
                   + ", v" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::VNEG) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::VPACK ||
                       iw.opcode == Opcode::VUNPACK) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::VPERMUTE) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1)
                   + ", v" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::VSWAP) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::VGATHER ||
                       iw.opcode == Opcode::VSCATTER) {
                s += "v" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1)
                   + ", v" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::VADD ||
                       iw.opcode == Opcode::VSUB ||
                       iw.opcode == Opcode::VMUL ||
                       iw.opcode == Opcode::VDIV ||
                       iw.opcode == Opcode::VCMP) {
                s += "v" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1)
                   + ", v" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::SWAP ||
                       iw.opcode == Opcode::CVT ||
                       iw.opcode == Opcode::COPY ||
                       iw.opcode == Opcode::SQRT ||
                       iw.opcode == Opcode::NEG ||
                       iw.opcode == Opcode::ABS ||
                       iw.opcode == Opcode::TINV ||
                       iw.opcode == Opcode::TLNEG ||
                       iw.opcode == Opcode::TCOUNT ||
                       iw.opcode == Opcode::TSCAN) {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::CALLR ||
                       iw.opcode == Opcode::JMPR) {
                s += "r" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::FENCE) {
                s.pop_back();
            } else if (iw.opcode == Opcode::TLDR) {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1);
            } else if (iw.opcode == Opcode::TMAC) {
                s += "r" + std::to_string(iw.rs1)
                   + ", r" + std::to_string(iw.rs2);
            } else if (iw.opcode == Opcode::VSUM ||
                       iw.opcode == Opcode::VHMIN ||
                       iw.opcode == Opcode::VHMAX) {
                s += "r" + std::to_string(iw.rd)
                   + ", v" + std::to_string(iw.rs1);
            } else {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1)
                   + ", r" + std::to_string(iw.rs2);
            }
            break;
        case InstructionFormat::I_TYPE:
            if (iw.opcode == Opcode::CSRR) {
                s += "r" + std::to_string(iw.rd)
                   + ", " + std::string(csrToString(iw.imm));
            } else if (iw.opcode == Opcode::CSRW) {
                s += std::string(csrToString(iw.imm))
                   + ", r" + std::to_string(iw.rd);
            } else if (iw.opcode == Opcode::SYSCALL) {
                s += std::to_string(iw.imm);
            } else if (iw.opcode == Opcode::VLOAD || iw.opcode == Opcode::VSTORE) {
                s += "v" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1)
                   + ", " + std::to_string(iw.imm);
            } else {
                s += "r" + std::to_string(iw.rd)
                   + ", r" + std::to_string(iw.rs1)
                   + ", " + std::to_string(iw.imm);
            }
            break;
        case InstructionFormat::B_TYPE:
            s += "r" + std::to_string(iw.rs_branch)
               + ", " + std::to_string(iw.offset);
            break;
        default:
            s += "<invalid format>";
    }
    return s;
}

[[nodiscard]] inline std::string disassemble(
        const TritWord27& word,
        IsaEncodingVersion version) {
    return disassemble(VersionedInstructionCodec::decode(word, version));
}

[[nodiscard]] inline std::string disassemble(const TritWord27& word) {
    return disassemble(word, IsaEncodingVersion::V2);
}

} // namespace isa
} // namespace sandbox

#endif // TERNARY_ISA_H
