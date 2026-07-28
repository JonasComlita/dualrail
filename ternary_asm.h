// =============================================================================
// ternary_asm.h  —  Ternary Assembler (Two-Pass, Label-Resolving)
// =============================================================================
//
// Phase 4 of the Ternary VM build plan. Converts human-readable assembly
// source text into a std::vector<TritWord27> program image suitable for
// loading directly into TernaryInstructionMemory via loadAndReset().
//
// This is the Rosetta Stone's output layer: the program images produced here
// are the exact bit patterns that will be fed into the SystemVerilog testbench
// for Stage 3 (FPGA) validation.
//
// Dependencies:
//   ternary_vm_state.h  (which pulls in ternary_isa.h and ternary_math.h)
//
// =============================================================================
// ASSEMBLY SYNTAX
// =============================================================================
//
// GENERAL RULES:
//   - One instruction per line. Blank lines and comment-only lines are ignored.
//   - Comments begin with ; and extend to end of line.
//   - Tokens are case-insensitive (ADD, add, Add all work).
//   - Commas between operands are optional but recommended.
//   - Labels are identifiers followed by ':' on their own line or before an
//     instruction on the same line.
//   - Label names: [A-Za-z_][A-Za-z0-9_]*  (no leading digits)
//   - .org N advances the current text or data address and pads with zero/NOP.
//   - .pte ppn,user,read,write,execute[,present] emits one raw T40 PTE word.
//   - .execheader entry,text_pages,data_pages,stack_words,syscall_abi,flags
//     emits a fixed Phase 4 executable header and records metadata.
//
// REGISTER NAMES:
//   r0 .. r26     General-purpose registers (r0 hardwired zero)
//   zero          Alias for r0
//   lr            Alias for r25 (link register)
//   sp            Alias for r26 (stack pointer)
//
// INSTRUCTION FORMATS:
//
//   R-TYPE (register-register):
//     ADD   rd, rs1, rs2      rd = rs1 + rs2
//     SUB   rd, rs1, rs2      rd = rs1 - rs2
//     MUL   rd, rs1, rs2      rd = rs1 * rs2
//     DIV   rd, rs1, rs2      rd = rs1 / rs2  (traps if rs2 == 0)
//     TCMP  rd, rs1, rs2      rd = sign(rs1 - rs2) ∈ {-1, 0, +1}
//     TMIN  rd, rs1, rs2      rd = min(rs1, rs2)
//     TMAX  rd, rs1, rs2      rd = max(rs1, rs2)
//
//   R-TYPE (single source — rs2 omitted, defaults to r0):
//     SQRT  rd, rs1            rd = sqrt(rs1)
//     NEG   rd, rs1            rd = -rs1
//     ABS   rd, rs1            rd = |rs1|
//     TINV  rd, rs1            rd = -rs1  (trit flip, alias for NEG)
//     COPY  rd, rs1            rd = rs1
//
//   I-TYPE (immediate):
//     MOV   rd, imm            rd = imm  (rs1 = r0 implicit)
//     MOV   rd, rs1, imm       rd = imm  (rs1 ignored; included for uniformity)
//     MOVH  rd, imm            rd = imm << 16 trits
//     LOAD  rd, rs1, imm       rd = mem[rs1 + imm]
//     LOAD  rd, rs1            rd = mem[rs1 + 0]
//     STORE src, base, imm     mem[base + imm] = src
//     STORE src, base          mem[base + 0] = src
//
//   B-TYPE (control flow):
//     JMP   label              PC = PC + offset(label)  unconditional
//     JMP   imm                PC = PC + imm
//     BRN   rs, label          if rs.trit[0] == -1: PC = PC + offset(label)
//     BRN   rs, imm            if rs.trit[0] == -1: PC = PC + imm
//     CALL  label              lr = PC+1; PC = PC + offset(label)
//     CALL  imm                lr = PC+1; PC = PC + imm
//     RET                      PC = lr
//
//   SYSTEM:
//     NOP                      no operation
//     HALT                     stop execution
//
// LABEL REFERENCES IN BRANCHES:
//   Branch targets are always PC-relative offsets. When you write:
//     BRN r5, loop_top
//   The assembler computes: offset = address(loop_top) - address(BRN_instruction)
//   This matches the VM's BRN semantics: PC_new = PC_current + offset.
//
// EXAMPLE PROGRAM (sum 1..N):
//   ; Sum of integers from 1 to N, result in r1
//   ; Input: r2 = N
//       MOV   r1, 0          ; accumulator = 0
//       MOV   r3, 1          ; step = 1
//       MOV   r4, 0          ; zero for comparison
//   loop:
//       ADD   r1, r1, r2     ; acc += counter
//       SUB   r2, r2, r3     ; counter -= 1
//       TCMP  r5, r2, r4     ; r5 = sign(counter)
//       TINV  r5, r5         ; flip: positive → negative
//       BRN   r5, loop       ; branch back while counter was > 0
//       HALT
//
// =============================================================================

#pragma once
#ifndef TERNARY_ASM_H
#define TERNARY_ASM_H

#include "ternary_vm_state.h"
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <optional>
#include <iomanip>

namespace sandbox {
namespace vm {
namespace assembler {

using namespace isa;

#include "architecture_v2_assembler_types.h"

enum class AssemblySection {
    Text,
    Data
};

// =============================================================================
// SECTION 1 — AssemblyResult
// =============================================================================

struct AssemblyError {
    int         line;     // 1-based source line number (0 = no specific line)
    std::string message;

    [[nodiscard]] std::string format() const {
        if (line > 0)
            return "Line " + std::to_string(line) + ": " + message;
        return message;
    }
};

struct AssemblyResult {
    bool                          success = false;
    std::vector<TritWord27>       program;
    std::vector<TernaryValue>     data;
    std::vector<AssemblyError>    errors;
    std::map<std::string, int>    labels;       // text label name -> IMEM word address
    std::map<std::string, int>    data_labels;  // data label name -> DMEM word address
    std::map<std::string, ExecutableImageHeader> executable_headers;
    std::map<std::string, ExecutableImageHeaderV2> executable_headers_v2;
    IsaEncodingVersion            isa_version = IsaEncodingVersion::V1;
    std::uint64_t                 required_features = 0;

    // Convenience: check and throw on error.
    [[nodiscard]] const std::vector<TritWord27>& require() const {
        if (!success) {
            std::string msg = "Assembly failed:\n";
            for (auto& e : errors) msg += "  " + e.format() + "\n";
            throw std::runtime_error(msg);
        }
        return program;
    }
};

// =============================================================================
// SECTION 2 — Tokenizer
// =============================================================================

// Strip inline comment (everything from ; onward).
[[nodiscard]] inline std::string stripComment(const std::string& line) {
    auto pos = line.find(';');
    return (pos == std::string::npos) ? line : line.substr(0, pos);
}

// Lowercase in-place.
[[nodiscard]] inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Trim leading/trailing whitespace.
[[nodiscard]] inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Split a string by delimiters (comma and/or whitespace), skipping empty tokens.
[[nodiscard]] inline std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// =============================================================================
// SECTION 3 — Register Name Parser
// =============================================================================

#include "architecture_v2_assembler_directives.h"

// Parse a register name token. Returns register index [0..26] or -1 on error.
[[nodiscard]] inline int parseRegister(const std::string& tok) {
    std::string s = toLower(tok);
    if (s == "zero" || s == "r0")  return 0;
    if (s == "lr"   || s == "r25") return 25;
    if (s == "sp"   || s == "r26") return 26;

    if (s.size() >= 2 && s[0] == 'r') {
        // Parse rN or rNN
        std::string digits = s.substr(1);
        if (digits.empty()) return -1;
        for (char c : digits) if (!std::isdigit(c)) return -1;
        int n = std::stoi(digits);
        if (n >= 0 && n < REG_COUNT) return n;
    }
    return -1;
}

// Parse a vector register token. Returns [0..7] or -1 on error.
[[nodiscard]] inline int parseVectorRegister(const std::string& tok) {
    std::string s = toLower(tok);
    if (s.size() >= 2 && s[0] == 'v') {
        std::string digits = s.substr(1);
        if (digits.empty()) return -1;
        for (char c : digits) if (!std::isdigit(c)) return -1;
        int n = std::stoi(digits);
        if (n >= 0 && n < VECTOR_REGISTER_COUNT) return n;
    }
    return -1;
}

// Parse a CSR name or numeric CSR id. Returns [0..CSR_MAX_ID] or -1 on error.
[[nodiscard]] inline int parseCSR(const std::string& tok) {
    std::string s = toLower(tok);
    if (s == "epc") return CSR_EPC;
    if (s == "cause") return CSR_CAUSE;
    if (s == "status") return CSR_STATUS;
    if (s == "tvec") return CSR_TVEC;
    if (s == "scratch") return CSR_SCRATCH;
    if (s == "cycle") return CSR_CYCLE;
    if (s == "timer_reload") return CSR_TIMER_RELOAD;
    if (s == "timer_counter") return CSR_TIMER_COUNTER;
    if (s == "timer_enable") return CSR_TIMER_ENABLE;
    if (s == "timer_pending") return CSR_TIMER_PENDING;
    if (s == "user_imem_base") return CSR_USER_IMEM_BASE;
    if (s == "user_imem_limit") return CSR_USER_IMEM_LIMIT;
    if (s == "user_dmem_base") return CSR_USER_DMEM_BASE;
    if (s == "user_dmem_limit") return CSR_USER_DMEM_LIMIT;
    if (s == "syscall_id") return CSR_SYSCALL_ID;
    if (s == "mmu_enable") return CSR_MMU_ENABLE;
    if (s == "user_imem_ptbr") return CSR_USER_IMEM_PTBR;
    if (s == "user_imem_pages") return CSR_USER_IMEM_PAGES;
    if (s == "user_dmem_ptbr") return CSR_USER_DMEM_PTBR;
    if (s == "user_dmem_pages") return CSR_USER_DMEM_PAGES;
    if (s == "page_fault_addr") return CSR_PAGE_FAULT_ADDR;
    if (s == "page_fault_access") return CSR_PAGE_FAULT_ACCESS;
    if (s == "console_out") return CSR_CONSOLE_OUT;
    if (s == "console_ctrl") return CSR_CONSOLE_CTRL;
    if (s == "console_in") return CSR_CONSOLE_IN;
    if (s == "console_in_ctrl") return CSR_CONSOLE_IN_CTRL;
    if (s == "mouse_x") return CSR_MOUSE_X;
    if (s == "mouse_y") return CSR_MOUSE_Y;
    if (s == "mouse_btn") return CSR_MOUSE_BTN;
    if (s == "gpu_x1") return CSR_GPU_X1;
    if (s == "gpu_y1") return CSR_GPU_Y1;
    if (s == "gpu_x2") return CSR_GPU_X2;
    if (s == "gpu_y2") return CSR_GPU_Y2;
    if (s == "gpu_color") return CSR_GPU_COLOR;
    if (s == "gpu_cmd") return CSR_GPU_CMD;
    if (s == "gpu_page") return CSR_GPU_PAGE;
    if (s == "gpu_draw_base") return CSR_GPU_DRAW_BASE;
    if (s == "gpu_mode") return CSR_GPU_MODE;
    if (s == "sprite_x") return CSR_SPRITE_X;
    if (s == "sprite_y") return CSR_SPRITE_Y;
    if (s == "sprite_attr") return CSR_SPRITE_ATTR;
    if (s == "block_index") return CSR_BLOCK_INDEX;
    if (s == "block_addr") return CSR_BLOCK_ADDR;
    if (s == "block_cmd") return CSR_BLOCK_CMD;
    if (s == "block_status") return CSR_BLOCK_STATUS;
    if (s == "block_count") return CSR_BLOCK_COUNT;
    if (s == "block_words") return CSR_BLOCK_WORDS;
    if (s == "power_control") return CSR_POWER_CONTROL;
    if (s == "isa_version") return CSR_ISA_VERSION;
    if (s == "isa_features") return CSR_ISA_FEATURES;
    if (s == "mmu_base_page_words") return CSR_MMU_BASE_PAGE_WORDS;
    if (s == "mmu_superpage_words") return CSR_MMU_SUPERPAGE_WORDS;
    if (s == "asid") return CSR_ASID;

    if (s.empty()) return -1;
    for (char c : s) {
        if (!std::isdigit(c)) return -1;
    }
    int id = std::stoi(s);
    return isValidCSR(id) ? id : -1;
}

// =============================================================================
// SECTION 4 — Immediate / Label Token Parser
// =============================================================================

struct ImmOrLabel {
    bool        isLabel;
    int         imm;         // valid when !isLabel
    std::string labelName;   // valid when isLabel
};

// Parse an immediate or label token.
// Immediates: decimal integers, optionally signed.
// Labels: identifiers matching [A-Za-z_][A-Za-z0-9_]*
[[nodiscard]] inline ImmOrLabel parseImmOrLabel(const std::string& tok) {
    if (tok.empty()) return {false, 0, ""};

    // Try integer parse.
    size_t start = (tok[0] == '-' || tok[0] == '+') ? 1 : 0;
    bool isNum = !tok.empty() && (start < tok.size());
    for (size_t i = start; i < tok.size(); ++i) {
        if (!std::isdigit(tok[i])) { isNum = false; break; }
    }
    if (isNum && tok.size() > start) {
        return {false, std::stoi(tok), ""};
    }

    // Otherwise treat as label name.
    // Validate: must start with letter or underscore.
    char first = tok[0];
    if (!std::isalpha(first) && first != '_') {
        return {false, 0, ""};  // invalid — caller checks
    }
    return {true, 0, toLower(tok)};
}

// =============================================================================
// SECTION 5 — Opcode Table
// =============================================================================

struct OpcodeInfo {
    Opcode          opcode;
    InstructionFormat fmt;
    int             num_explicit_operands; // how many tokens to parse after mnemonic
    bool            rs2_optional;          // true for NEG/ABS/SQRT/COPY/TINV
};

struct MnemonicParts {
    std::string base;
    bool has_width = false;
    uint8_t func = FUNC_DEFAULT;
    bool has_source_width = false;
    uint8_t source_func = 0;
    bool has_order = false;
    int order = ATOMIC_ORDER_ACQ_REL;
    bool suffix_valid = true;
};

[[nodiscard]] inline bool parseAtomicOrderSuffix(const std::string& suffix, int& order) {
    if (suffix == "-1" || suffix == "relaxed") {
        order = ATOMIC_ORDER_RELAXED;
        return true;
    }
    if (suffix == "0" || suffix == "acqrel" || suffix == "acquire-release") {
        order = ATOMIC_ORDER_ACQ_REL;
        return true;
    }
    if (suffix == "+1" || suffix == "1" || suffix == "seqcst" || suffix == "sc") {
        order = ATOMIC_ORDER_SEQ_CST;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool parseWidthSuffix(const std::string& suffix, uint8_t& func) {
    if (suffix == "t1") func = FUNC_T1;
    else if (suffix == "t5") func = FUNC_T5;
    else if (suffix == "t10") func = FUNC_T10;
    else if (suffix == "t20") func = FUNC_T20;
    else if (suffix == "t40") func = FUNC_T40;
    else if (suffix == "t50") func = FUNC_T50;
    else if (suffix == "l1") func = FUNC_L1;
    else if (suffix == "l5") func = FUNC_L5;
    else if (suffix == "l10") func = FUNC_L10;
    else if (suffix == "l20") func = FUNC_L20;
    else if (suffix == "l40") func = FUNC_L40;
    else if (suffix == "l50") func = FUNC_L50;
    else return false;
    return true;
}

[[nodiscard]] inline MnemonicParts splitMnemonic(const std::string& mnemonic) {
    MnemonicParts out;
    const std::string lowered = toLower(mnemonic);
    const size_t dot = lowered.find('.');
    if (dot == std::string::npos) {
        out.base = lowered;
        return out;
    }

    out.base = lowered.substr(0, dot);
    const size_t dot2 = lowered.find('.', dot + 1);
    if (dot2 == std::string::npos) {
        const std::string suffix = lowered.substr(dot + 1);
        int order = ATOMIC_ORDER_ACQ_REL;
        if (parseAtomicOrderSuffix(suffix, order)) {
            out.has_order = true;
            out.order = order;
            out.func = atomicOrderFunc(order);
        } else {
            out.has_width = true;
            out.suffix_valid = parseWidthSuffix(suffix, out.func);
        }
    } else {
        const std::string suffix1 = lowered.substr(dot + 1, dot2 - dot - 1);
        const std::string suffix2 = lowered.substr(dot2 + 1);
        out.has_width = true;
        out.suffix_valid = (out.base == "cvt" ||
                            out.base == "vpack" ||
                            out.base == "vunpack") &&
                           parseWidthSuffix(suffix1, out.source_func) &&
                           parseWidthSuffix(suffix2, out.func);
        out.has_source_width = out.suffix_valid;
    }
    return out;
}

[[nodiscard]] inline bool supportsWidthSuffix(const std::string& base) {
    return base == "add" || base == "sub" || base == "mul" || base == "div" ||
           base == "sqrt" || base == "neg" || base == "abs" || base == "tinv" ||
           base == "tcmp" || base == "tmin" || base == "tmax" ||
           base == "twcmp" || base == "tclamp" || base == "tmod" ||
           base == "tlshift" || base == "trshift" ||
           base == "tmac" || base == "tcount" || base == "tscan" ||
           base == "cvt" || base == "mov" || base == "copy" ||
           base == "load" || base == "store" ||
           base == "tladd" || base == "tlsub" || base == "tlneg" ||
           base == "tland" || base == "tlor" ||
           base == "vadd" || base == "vsub" || base == "vneg" ||
           base == "vmul" || base == "vdiv" || base == "vcmp" ||
           base == "vsel" || base == "vbcast" ||
           base == "vload" || base == "vstore" ||
           base == "aclr" || base == "aload" || base == "aadd" ||
           base == "asub" || base == "amul" || base == "astore" ||
           base == "vdot" || base == "vmac" || base == "vact" ||
           base == "vpack" || base == "vunpack" ||
           base == "vpermute" || base == "vblend" ||
           base == "vgather" || base == "vscatter" ||
           base == "vsum" || base == "vhmin" || base == "vhmax";
}

[[nodiscard]] inline bool supportsAtomicOrderSuffix(const std::string& base) {
    return base == "fence" || base == "tldr" || base == "tstr";
}

[[nodiscard]] inline int instructionWordCount(const std::string& mnemonic) {
    const auto parts = splitMnemonic(mnemonic);
    if (parts.has_width && parts.func == FUNC_T50) {
        if (parts.base == "load") return 3;
        if (parts.base == "store") return 2;
    }
    return ((parts.base == "mov" || parts.base == "movh") && parts.has_width && parts.func != FUNC_T40 &&
            parts.suffix_valid && !parts.has_source_width) ? 2 : 1;
}

// Build the opcode lookup table.
[[nodiscard]] inline std::map<std::string, OpcodeInfo> buildOpcodeTable() {
    using F = InstructionFormat;
    std::map<std::string, OpcodeInfo> t;

    // System
    t["nop"]  = {Opcode::NOP,  F::I_TYPE, 0, false};
    t["halt"] = {Opcode::HALT, F::B_TYPE, 0, false};
    t["wait"] = {Opcode::WAIT, F::B_TYPE, 0, false};
    t["tlbinv"] = {Opcode::TLBINV, F::R_TYPE, 3, false};

    // Data movement
    t["mov"]  = {Opcode::MOV,  F::I_TYPE, 2, false};  // rd, imm  OR  rd, rs1, imm
    t["movh"] = {Opcode::MOVH, F::I_TYPE, 2, false};
    t["copy"] = {Opcode::COPY, F::R_TYPE, 2, true};   // rd, rs1
    t["cvt"]  = {Opcode::CVT,  F::R_TYPE, 2, true};   // rd, rs1
    t["swap"] = {Opcode::SWAP, F::R_TYPE, 2, true};   // rA, rB

    // Arithmetic (2 sources)
    t["add"]  = {Opcode::ADD,  F::R_TYPE, 3, false};
    t["sub"]  = {Opcode::SUB,  F::R_TYPE, 3, false};
    t["mul"]  = {Opcode::MUL,  F::R_TYPE, 3, false};
    t["div"]  = {Opcode::DIV,  F::R_TYPE, 3, false};

    // Arithmetic (1 source, rs2 = r0)
    t["sqrt"] = {Opcode::SQRT, F::R_TYPE, 2, true};
    t["neg"]  = {Opcode::NEG,  F::R_TYPE, 2, true};
    t["abs"]  = {Opcode::ABS,  F::R_TYPE, 2, true};
    t["tinv"] = {Opcode::TINV, F::R_TYPE, 2, true};

    // Compare & logic
    t["tcmp"] = {Opcode::TCMP, F::R_TYPE, 3, false};
    t["tmin"] = {Opcode::TMIN, F::R_TYPE, 3, false};
    t["tmax"] = {Opcode::TMAX, F::R_TYPE, 3, false};
    t["tsel"] = {Opcode::TSEL, F::R_TYPE, 5, false};
    t["twcmp"] = {Opcode::TWCMP, F::R_TYPE, 4, false};
    t["tclamp"] = {Opcode::TCLAMP, F::R_TYPE, 4, false};

    // Scalar lane logic
    t["tladd"] = {Opcode::TLADD, F::R_TYPE, 3, false};
    t["tlsub"] = {Opcode::TLSUB, F::R_TYPE, 3, false};
    t["tlneg"] = {Opcode::TLNEG, F::R_TYPE, 2, true};
    t["tland"] = {Opcode::TLAND, F::R_TYPE, 3, false};
    t["tlor"]  = {Opcode::TLOR,  F::R_TYPE, 3, false};

    // Memory (LOAD: rd, rs1, imm  or  rd, rs1)
    t["load"]  = {Opcode::LOAD,  F::I_TYPE, 3, false};  // special handling below
    t["store"] = {Opcode::STORE, F::I_TYPE, 3, false};

    // Control flow
    t["jmp"]  = {Opcode::JMP,  F::B_TYPE, 1, false};   // target (rs = r0)
    t["brn"]  = {Opcode::BRN,  F::B_TYPE, 2, false};   // rs, target
    t["brz"]  = {Opcode::BRZ,  F::B_TYPE, 2, false};   // rs, target
    t["brp"]  = {Opcode::BRP,  F::B_TYPE, 2, false};   // rs, target
    t["call"] = {Opcode::CALL, F::B_TYPE, 1, false};   // target (rs = r0)
    t["callr"] = {Opcode::CALLR, F::R_TYPE, 1, true};   // absolute PC in rS
    t["jmpr"]  = {Opcode::JMPR,  F::R_TYPE, 1, true};   // absolute PC in rS
    t["ret"]  = {Opcode::RET,  F::R_TYPE, 0, false};
    t["syscall"] = {Opcode::SYSCALL, F::I_TYPE, 1, false}; // sandbox or OS service id
    t["fence"]   = {Opcode::FENCE,   F::R_TYPE, 0, true};
    t["csrr"]    = {Opcode::CSRR,    F::I_TYPE, 2, false}; // rd, csr
    t["csrw"]    = {Opcode::CSRW,    F::I_TYPE, 2, false}; // csr, rs
    t["csrrw"]   = {Opcode::CSRRW,   F::R_TYPE, 3, false}; // rd, csr, rs
    t["eret"]    = {Opcode::ERET,    F::R_TYPE, 0, false};
    t["tldr"]    = {Opcode::TLDR,    F::R_TYPE, 2, true};  // rd, address register
    t["tstr"]    = {Opcode::TSTR,    F::R_TYPE, 4, false}; // rdStatus, address, new, expected

    // Phase 2 scalar arithmetic and analysis
    t["tmod"]    = {Opcode::TMOD,    F::R_TYPE, 3, false};
    t["tlshift"] = {Opcode::TLSHIFT, F::R_TYPE, 3, false};
    t["trshift"] = {Opcode::TRSHIFT, F::R_TYPE, 3, false};
    t["tmac"]    = {Opcode::TMAC,    F::R_TYPE, 2, false};
    t["tcount"]  = {Opcode::TCOUNT,  F::R_TYPE, 2, true};
    t["tscan"]   = {Opcode::TSCAN,   F::R_TYPE, 2, true};

    // Vector foundation
    t["vadd"]   = {Opcode::VADD,   F::R_TYPE, 3, false};
    t["vsub"]   = {Opcode::VSUB,   F::R_TYPE, 3, false};
    t["vneg"]   = {Opcode::VNEG,   F::R_TYPE, 2, true};
    t["vmul"]   = {Opcode::VMUL,   F::R_TYPE, 3, false};
    t["vdiv"]   = {Opcode::VDIV,   F::R_TYPE, 3, false};
    t["vcmp"]   = {Opcode::VCMP,   F::R_TYPE, 3, false};
    t["vsel"]   = {Opcode::VSEL,   F::R_TYPE, 5, false};
    t["vload"]  = {Opcode::VLOAD,  F::I_TYPE, 3, false};
    t["vstore"] = {Opcode::VSTORE, F::I_TYPE, 3, false};
    t["vbcast"] = {Opcode::VBCAST, F::R_TYPE, 2, true};
    t["vlen"] = {Opcode::VLEN, F::R_TYPE, 1, true};

    // Accumulator and T1 AI
    t["aclr"]   = {Opcode::ACLR,   F::R_TYPE, 0, true};
    t["aload"]  = {Opcode::ALOAD,  F::R_TYPE, 1, true};
    t["aadd"]   = {Opcode::AADD,   F::R_TYPE, 1, true};
    t["asub"]   = {Opcode::ASUB,   F::R_TYPE, 1, true};
    t["amul"]   = {Opcode::AMUL,   F::R_TYPE, 1, true};
    t["astore"] = {Opcode::ASTORE, F::R_TYPE, 1, true};
    t["vdot"]   = {Opcode::VDOT,   F::R_TYPE, 3, false};
    t["vmac"]   = {Opcode::VMAC,   F::R_TYPE, 2, false};
    t["vact"]   = {Opcode::VACT,   F::R_TYPE, 2, true};

    // Vector plumbing and indexed memory
    t["vpack"]    = {Opcode::VPACK,    F::R_TYPE, 2, true};
    t["vunpack"]  = {Opcode::VUNPACK,  F::R_TYPE, 2, true};
    t["vpermute"] = {Opcode::VPERMUTE, F::R_TYPE, 3, false};
    t["vblend"]   = {Opcode::VBLEND,   F::R_TYPE, 4, false};
    t["vswap"]    = {Opcode::VSWAP,    F::R_TYPE, 2, true};
    t["vgather"]  = {Opcode::VGATHER,  F::R_TYPE, 3, false};
    t["vscatter"] = {Opcode::VSCATTER, F::R_TYPE, 3, false};
    t["vsum"]     = {Opcode::VSUM,     F::R_TYPE, 2, false};
    t["vhmin"]    = {Opcode::VHMIN,    F::R_TYPE, 2, false};
    t["vhmax"]    = {Opcode::VHMAX,    F::R_TYPE, 2, false};

    return t;
}

static const std::map<std::string, OpcodeInfo> OPCODE_TABLE = buildOpcodeTable();

// =============================================================================
// SECTION 6 — Two-Pass Assembler
// =============================================================================

// Internal representation of one parsed source line.
struct SourceLine {
    int                      line_num;    // 1-based
    AssemblySection          section = AssemblySection::Text;
    std::string              label;       // empty if no label on this line
    std::string              mnemonic;    // empty for label-only lines
    std::vector<std::string> operands;    // raw operand tokens
    int                      address;     // section-local word address (-1 = label-only line)
    int                      word_count = 0;
};

// Parse source into SourceLine list. Extracts labels, mnemonics, operands.
// Does not validate operands.
[[nodiscard]] inline std::vector<SourceLine> parseSources(
        const std::string& source,
        std::vector<AssemblyError>& errors) {

    std::vector<SourceLine> lines;
    std::istringstream stream(source);
    std::string raw;
    int line_num = 0;
    int text_addr = 0;
    int data_addr = 0;
    AssemblySection current_section = AssemblySection::Text;

    while (std::getline(stream, raw)) {
        ++line_num;
        std::string s = trim(stripComment(raw));
        if (s.empty()) continue;

        SourceLine sl;
        sl.line_num = line_num;
        sl.section = current_section;
        sl.address  = -1;

        // Extract label if present (token ending with ':').
        // Label may be followed by an instruction on the same line.
        size_t colon = s.find(':');
        if (colon != std::string::npos) {
            std::string label_part = trim(s.substr(0, colon));
            bool valid = !label_part.empty() &&
                         (std::isalpha(label_part[0]) || label_part[0] == '_');
            for (char c : label_part)
                if (!std::isalnum(c) && c != '_') { valid = false; break; }

            if (!valid) {
                errors.push_back({line_num, "Invalid label name: '" + label_part + "'"});
            } else {
                sl.label = toLower(label_part);
            }
            s = trim(s.substr(colon + 1));
            if (s.empty()) {
                lines.push_back(sl);
                continue;  // label-only line
            }
        }

        // Tokenize the instruction part.
        auto tokens = tokenize(s);
        if (tokens.empty()) {
            if (!sl.label.empty()) lines.push_back(sl);
            continue;
        }

        sl.mnemonic = toLower(tokens[0]);
        for (size_t i = 1; i < tokens.size(); ++i)
            sl.operands.push_back(tokens[i]);

        if (sl.mnemonic == ".isa" || sl.mnemonic == ".require") {
            continue;
        }

        if (sl.mnemonic == ".text" || sl.mnemonic == ".data") {
            if (!sl.label.empty()) {
                errors.push_back({line_num, "Section directive cannot carry a label"});
            }
            if (!sl.operands.empty()) {
                errors.push_back({line_num, sl.mnemonic + " takes no operands"});
            }
            current_section = (sl.mnemonic == ".text")
                ? AssemblySection::Text
                : AssemblySection::Data;
            continue;
        }

        if (sl.mnemonic == ".org") {
            if (!sl.label.empty()) {
                errors.push_back({line_num, ".org cannot carry a label"});
            }
            if (sl.operands.size() != 1) {
                errors.push_back({line_num, ".org requires one absolute address"});
                continue;
            }
            auto parsed = parseImmOrLabel(sl.operands[0]);
            if (parsed.isLabel || parsed.imm < 0) {
                errors.push_back({line_num, ".org requires a non-negative numeric address"});
                continue;
            }
            int& current_addr = current_section == AssemblySection::Text ? text_addr : data_addr;
            if (parsed.imm < current_addr) {
                errors.push_back({line_num, ".org cannot move the current address backwards"});
                continue;
            }
            current_addr = parsed.imm;
            continue;
        }

        sl.section = current_section;
        if (current_section == AssemblySection::Data) {
            if (sl.mnemonic != ".word" && sl.mnemonic != ".pte" && sl.mnemonic != ".execheader" && sl.mnemonic != ".execheader2") {
                errors.push_back({line_num, "Only .word, .pte, .execheader, and .execheader2 directives are valid in .data"});
                lines.push_back(sl);
                continue;
            }
            if (sl.mnemonic == ".word" && sl.operands.empty()) {
                errors.push_back({line_num, ".word requires at least one operand"});
                lines.push_back(sl);
                continue;
            }
            if (sl.mnemonic == ".pte" &&
                sl.operands.size() != 5 &&
                sl.operands.size() != 6) {
                errors.push_back({line_num, ".pte requires ppn, user, read, write, execute[, present]"});
                lines.push_back(sl);
                continue;
            }
            if (sl.mnemonic == ".execheader" && sl.operands.size() != 6) {
                errors.push_back({line_num, ".execheader requires entry_pc, text_pages, data_pages, stack_words, syscall_abi, flags"});
                lines.push_back(sl);
                continue;
            }
            if (sl.mnemonic == ".execheader2" && sl.operands.size() != 7) {
                errors.push_back({line_num, ".execheader2 requires entry_pc, text_words, data_words, stack_words, required_feature_word, syscall_abi, flags"});
                lines.push_back(sl);
                continue;
            }
            sl.address = data_addr;
            sl.word_count = sl.mnemonic == ".pte" ? 1 :
                            sl.mnemonic == ".execheader" ? EXEC_HEADER_WORDS :
                            sl.mnemonic == ".execheader2" ? EXEC_V2_HEADER_WORDS :
                            static_cast<int>(sl.operands.size());
            data_addr += sl.word_count;
        } else {
            if (sl.mnemonic == ".word" || sl.mnemonic == ".pte" || sl.mnemonic == ".execheader" || sl.mnemonic == ".execheader2") {
                errors.push_back({line_num, sl.mnemonic + " is only valid in .data"});
                lines.push_back(sl);
                continue;
            }
            sl.address = text_addr;
            sl.word_count = instructionWordCount(sl.mnemonic);
            text_addr += sl.word_count;
        }
        lines.push_back(sl);
    }

    return lines;
}

// Pass 1: build the label → address map from parsed source lines.
struct LabelMaps {
    std::map<std::string, int> text;
    std::map<std::string, int> data;
};

[[nodiscard]] inline bool labelAlreadySeen(
        const LabelMaps& labels,
        const std::vector<std::string>& pending_text,
        const std::vector<std::string>& pending_data,
        const std::string& label) {
    if (labels.text.count(label) || labels.data.count(label)) return true;
    for (const auto& p : pending_text) { if (p == label) return true; }
    for (const auto& p : pending_data) { if (p == label) return true; }
    return false;
}

[[nodiscard]] inline LabelMaps buildLabelMaps(
        const std::vector<SourceLine>& lines,
        std::vector<AssemblyError>& errors) {

    LabelMaps labels;

    std::vector<std::string> pending_text_labels;
    std::vector<std::string> pending_data_labels;
    int pending_text_line = 0;
    int pending_data_line = 0;

    for (auto& sl : lines) {
        std::map<std::string, int>& section_labels =
            sl.section == AssemblySection::Text ? labels.text : labels.data;
        std::vector<std::string>& pending_labels =
            sl.section == AssemblySection::Text ? pending_text_labels : pending_data_labels;
        int& pending_line =
            sl.section == AssemblySection::Text ? pending_text_line : pending_data_line;

        if (!sl.label.empty()) {
            if (labelAlreadySeen(labels, pending_text_labels, pending_data_labels, sl.label)) {
                errors.push_back({sl.line_num,
                    "Duplicate label '" + sl.label + "'"});
            } else if (sl.address >= 0) {
                section_labels[sl.label] = sl.address;
            } else {
                pending_labels.push_back(sl.label);
                pending_line  = sl.line_num;
            }
        }

        if (!pending_labels.empty() && sl.address >= 0) {
            for (const auto& pending : pending_labels) {
                if (section_labels.count(pending)) {
                    errors.push_back({pending_line,
                        "Duplicate label '" + pending + "'"});
                } else {
                    section_labels[pending] = sl.address;
                }
            }
            pending_labels.clear();
        }
    }

    if (!pending_text_labels.empty()) {
        errors.push_back({pending_text_line,
            "Label '" + pending_text_labels.front() + "' defined after last text instruction"});
    }
    if (!pending_data_labels.empty()) {
        errors.push_back({pending_data_line,
            "Label '" + pending_data_labels.front() + "' defined after last data word"});
    }

    return labels;
}

// Pass 2: encode each source line into a TritWord27.
// Resolves label references to PC-relative offsets.
[[nodiscard]] inline std::vector<TritWord27> encode(
        const std::vector<SourceLine>& lines,
        const std::map<std::string, int>& text_labels,
        const std::map<std::string, int>& data_labels,
        std::vector<AssemblyError>& errors) {

    std::vector<TritWord27> program;

    // Helper: get register index or emit error and return 0.
    auto getReg = [&](const std::string& tok, int line_num) -> int {
        int r = parseRegister(tok);
        if (r < 0) {
            errors.push_back({line_num, "Unknown register '" + tok + "'"});
            return 0;
        }
        return r;
    };

    auto getVecReg = [&](const std::string& tok, int line_num) -> int {
        int r = parseVectorRegister(tok);
        if (r < 0) {
            errors.push_back({line_num, "Unknown vector register '" + tok + "'"});
            return 0;
        }
        return r;
    };

    auto resolveAbsolute = [&](const std::string& tok,
                               int line_num) -> std::optional<int> {
        auto parsed = parseImmOrLabel(tok);
        if (parsed.isLabel) {
            auto text_it = text_labels.find(parsed.labelName);
            if (text_it != text_labels.end()) return text_it->second;
            auto data_it = data_labels.find(parsed.labelName);
            if (data_it != data_labels.end()) return data_it->second;
            errors.push_back({line_num,
                "Undefined label '" + parsed.labelName + "'"});
            return std::nullopt;
        }
        return parsed.imm;
    };

    // Helper: resolve branch/call labels to PC-relative text offsets.
    auto resolveRelativeText = [&](const std::string& tok, int pc,
                                   int line_num) -> std::optional<int> {
        auto parsed = parseImmOrLabel(tok);
        if (parsed.isLabel) {
            auto it = text_labels.find(parsed.labelName);
            if (it == text_labels.end()) {
                if (data_labels.count(parsed.labelName)) {
                    errors.push_back({line_num,
                        "Data label '" + parsed.labelName + "' cannot be a branch target"});
                    return std::nullopt;
                }
                errors.push_back({line_num,
                    "Undefined label '" + parsed.labelName + "'"});
                return std::nullopt;
            }
            return it->second - pc;   // PC-relative offset
        }
        return parsed.imm;
    };

    auto parseAtomicOrderOperand = [&](const std::string& tok,
                                       int line_num) -> std::optional<int> {
        int order = ATOMIC_ORDER_ACQ_REL;
        if (!parseAtomicOrderSuffix(toLower(tok), order)) {
            errors.push_back({line_num,
                "Atomic memory order must be -1, 0, or +1"});
            return std::nullopt;
        }
        return order;
    };

    for (auto& sl : lines) {
        if (sl.section != AssemblySection::Text) continue;
        if (sl.address < 0) continue;  // label-only line, no instruction

        while (static_cast<int>(program.size()) < sl.address) {
            program.push_back(InstructionWord::encodeI(Opcode::NOP, 0, 0, 0));
        }

        MnemonicParts parts = splitMnemonic(sl.mnemonic);
        if (!parts.suffix_valid) {
            errors.push_back({sl.line_num,
                "Unknown width suffix in mnemonic '" + sl.mnemonic + "'"});
            program.push_back(TritWord27{});
            continue;
        }
        if (parts.has_width && !supportsWidthSuffix(parts.base)) {
            errors.push_back({sl.line_num,
                "Width suffix is not valid for mnemonic '" + parts.base + "'"});
            program.push_back(TritWord27{});
            continue;
        }
        if (parts.has_order && !supportsAtomicOrderSuffix(parts.base)) {
            errors.push_back({sl.line_num,
                "Memory-order suffix is not valid for mnemonic '" + parts.base + "'"});
            program.push_back(TritWord27{});
            continue;
        }
        const bool laneMnemonic = parts.base == "tladd" || parts.base == "tlsub" ||
                                  parts.base == "tlneg" || parts.base == "tland" ||
                                  parts.base == "tlor";
        const bool numericMnemonic =
            parts.base == "add" || parts.base == "sub" || parts.base == "mul" ||
            parts.base == "div" || parts.base == "sqrt" || parts.base == "neg" ||
            parts.base == "abs" || parts.base == "tinv" || parts.base == "tcmp" ||
            parts.base == "tmin" || parts.base == "tmax";
        const bool phase2NumericMnemonic =
            parts.base == "twcmp" || parts.base == "tclamp" ||
            parts.base == "tmod" || parts.base == "tlshift" ||
            parts.base == "trshift" || parts.base == "tmac" ||
            parts.base == "tcount" || parts.base == "tscan";
        const bool vectorNumericMnemonic =
            parts.base == "vadd" || parts.base == "vsub" || parts.base == "vneg" ||
            parts.base == "vmul" || parts.base == "vdiv" || parts.base == "vcmp" ||
            parts.base == "vsel" || parts.base == "vbcast" ||
            parts.base == "vload" || parts.base == "vstore";
        const bool phase2VectorReductionMnemonic =
            parts.base == "vsum" || parts.base == "vhmin" || parts.base == "vhmax";
        const bool accumulatorMnemonic =
            parts.base == "aclr" || parts.base == "aload" || parts.base == "aadd" ||
            parts.base == "asub" || parts.base == "amul" || parts.base == "astore";
        const bool t1AiMnemonic =
            parts.base == "vdot" || parts.base == "vmac" || parts.base == "vact";
        const bool vectorConvertMnemonic =
            parts.base == "vpack" || parts.base == "vunpack";
        const bool vectorPlumbingNumericMnemonic =
            parts.base == "vpermute" || parts.base == "vblend" ||
            parts.base == "vgather" || parts.base == "vscatter";
        if (laneMnemonic && (!parts.has_width || !isLaneWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Lane mnemonic '" + parts.base + "' requires an .lN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (numericMnemonic && parts.has_width && !isNumericWidthFunc(parts.func)) {
            errors.push_back({sl.line_num,
                "Numeric mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if ((parts.base == "load" || parts.base == "store") &&
            parts.has_width && parts.func != FUNC_T50) {
            errors.push_back({sl.line_num,
                "Scalar memory width suffix is only valid for paired .t50 access"});
            for (int word_index = 0; word_index < sl.word_count; ++word_index)
                program.push_back(TritWord27{});
            continue;
        }
        if (phase2NumericMnemonic && (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Phase 2 numeric mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (vectorNumericMnemonic && (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Vector numeric mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (phase2VectorReductionMnemonic && (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Vector reduction mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (accumulatorMnemonic && (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Accumulator mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (t1AiMnemonic && (!parts.has_width || parts.func != FUNC_T1)) {
            errors.push_back({sl.line_num,
                "T1 AI mnemonic '" + parts.base + "' requires a .t1 suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (vectorConvertMnemonic &&
            (!parts.has_source_width ||
             !isNumericWidthFunc(parts.source_func) ||
             !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Vector conversion mnemonic '" + parts.base + "' requires .src.dst numeric suffixes"});
            program.push_back(TritWord27{});
            continue;
        }
        if (vectorPlumbingNumericMnemonic &&
            (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Vector mnemonic '" + parts.base + "' requires a .tN suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if (parts.base == "vswap" && parts.has_width) {
            errors.push_back({sl.line_num, "vswap does not take a width suffix"});
            program.push_back(TritWord27{});
            continue;
        }
        if ((parts.base == "callr" || parts.base == "jmpr" ||
             parts.base == "syscall" || parts.base == "fence" ||
             parts.base == "csrr" || parts.base == "csrw" ||
             parts.base == "csrrw" || parts.base == "tldr" ||
             parts.base == "tstr" ||
             parts.base == "eret") &&
            parts.has_width) {
            errors.push_back({sl.line_num,
                parts.base + " does not take a width suffix"});
            program.push_back(TritWord27{});
            continue;
        }

        auto it = OPCODE_TABLE.find(parts.base);
        if (it == OPCODE_TABLE.end()) {
            errors.push_back({sl.line_num,
                "Unknown mnemonic '" + sl.mnemonic + "'"});
            program.push_back(TritWord27{});  // placeholder to keep addresses valid
            continue;
        }

        const OpcodeInfo& info  = it->second;
        const std::string mnemonic = parts.base;
        const auto& ops         = sl.operands;
        const int   pc          = sl.address;
        int         line        = sl.line_num;
        TritWord27  word{};
        bool        ok          = true;

        // ---- Encode by opcode ----

        if (mnemonic == "nop") {
            word = InstructionWord::encodeI(Opcode::NOP, 0, 0, 0);

        } else if (mnemonic == "halt") {
            word = InstructionWord::encodeB(Opcode::HALT, 0, 0);

        } else if (mnemonic == "wait") {
            if (!ops.empty()) {
                errors.push_back({line, "wait takes no operands"});
                ok = false;
            } else {
                word = InstructionWord::encodeB(Opcode::WAIT, 0, 0);
            }


        } else if (mnemonic == "ret") {
            word = InstructionWord::encodeR(Opcode::RET, 0, 0, 0);

        } else if (mnemonic == "eret") {
            if (!ops.empty()) {
                errors.push_back({line, "eret takes no operands"});
                ok = false;
            } else {
                word = InstructionWord::encodeR(Opcode::ERET,
                    R0_ZERO,
                    R0_ZERO,
                    R0_ZERO,
                    FUNC_DEFAULT);
            }

        } else if (mnemonic == "csrr") {
            if (ops.size() != 2) {
                errors.push_back({line, "csrr requires rd and csr"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int csr = parseCSR(ops[1]);
                if (csr < 0) {
                    errors.push_back({line, "Unknown CSR '" + ops[1] + "'"});
                    ok = false;
                }
                if (ok) {
                    word = InstructionWord::encodeI(Opcode::CSRR,
                        static_cast<uint8_t>(rd),
                        R0_ZERO,
                        csr);
                }
            }

        } else if (mnemonic == "csrw") {
            if (ops.size() != 2) {
                errors.push_back({line, "csrw requires csr and rs"});
                ok = false;
            } else {
                int csr = parseCSR(ops[0]);
                int rs = getReg(ops[1], line);
                if (csr < 0) {
                    errors.push_back({line, "Unknown CSR '" + ops[0] + "'"});
                    ok = false;
                }
                if (ok) {
                    word = InstructionWord::encodeI(Opcode::CSRW,
                        static_cast<uint8_t>(rs),
                        R0_ZERO,
                        csr);
                }
            }

        } else if (mnemonic == "csrrw") {
            if (ops.size() != 3) {
                errors.push_back({line, "csrrw requires rd, csr, and rs"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int csr = parseCSR(ops[1]);
                int rs = getReg(ops[2], line);
                if (csr < 0 || csr >= REG_COUNT) {
                    errors.push_back({line, "Unknown CSR '" + ops[1] + "'"});
                    ok = false;
                }
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::CSRRW,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(rs),
                        static_cast<uint8_t>(csr),
                        FUNC_DEFAULT);
                }
            }

        } else if (mnemonic == "callr" || mnemonic == "jmpr") {
            if (ops.size() != 1) {
                errors.push_back({line, mnemonic + " requires rTarget"});
                ok = false;
            } else {
                int target = getReg(ops[0], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        R0_ZERO,
                        static_cast<uint8_t>(target),
                        R0_ZERO,
                        FUNC_DEFAULT);
                }
            }

        } else if (mnemonic == "syscall") {
            if (ops.size() != 1) {
                errors.push_back({line, "syscall requires service id"});
                ok = false;
            } else {
                auto service = resolveAbsolute(ops[0], line);
                if (!service) {
                    ok = false;
                } else {
                    try {
                        word = InstructionWord::encodeI(Opcode::SYSCALL,
                            R0_ZERO,
                            R0_ZERO,
                            service.value());
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "fence") {
            if (ops.size() > 1 || (parts.has_order && !ops.empty())) {
                errors.push_back({line, "fence takes at most one memory-order operand"});
                ok = false;
            } else {
                int order = parts.order;
                if (!parts.has_order && ops.size() == 1) {
                    auto parsed = parseAtomicOrderOperand(ops[0], line);
                    if (!parsed) {
                        ok = false;
                    } else {
                        order = parsed.value();
                    }
                }
                if (!ok) {
                    program.push_back(TritWord27{});
                    continue;
                }
                word = InstructionWord::encodeR(Opcode::FENCE,
                    R0_ZERO,
                    R0_ZERO,
                    R0_ZERO,
                    atomicOrderFunc(order));
            }

        } else if (mnemonic == "tldr") {
            if (ops.size() < 2 || ops.size() > 3 || (parts.has_order && ops.size() == 3)) {
                errors.push_back({line, "tldr requires rd, rAddress and optional memory order"});
                ok = false;
            } else {
                int order = parts.order;
                if (!parts.has_order && ops.size() == 3) {
                    auto parsed = parseAtomicOrderOperand(ops[2], line);
                    if (!parsed) {
                        ok = false;
                    } else {
                        order = parsed.value();
                    }
                }
                int rd = getReg(ops[0], line);
                int addr = getReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::TLDR,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(addr),
                        R0_ZERO,
                        atomicOrderFunc(order));
                }
            }

        } else if (mnemonic == "tstr") {
            if (ops.size() < 4 || ops.size() > 5 || (parts.has_order && ops.size() == 5)) {
                errors.push_back({line, "tstr requires rdStatus, rAddress, rNew, rExpected and optional memory order"});
                ok = false;
            } else {
                int order = parts.order;
                if (!parts.has_order && ops.size() == 5) {
                    auto parsed = parseAtomicOrderOperand(ops[4], line);
                    if (!parsed) {
                        ok = false;
                    } else {
                        order = parsed.value();
                    }
                }
                int rd = getReg(ops[0], line);
                int addr = getReg(ops[1], line);
                int desired = getReg(ops[2], line);
                int expected = getReg(ops[3], line);
                if (ok) {
                    word = InstructionWord::encodeR4(Opcode::TSTR,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(addr),
                        static_cast<uint8_t>(desired),
                        static_cast<uint8_t>(expected),
                        atomicOrderFunc(order));
                }
            }

        } else if (mnemonic == "jmp" || mnemonic == "call") {
            // B-type: target only (rs = r0)
            if (ops.empty()) {
                errors.push_back({line, mnemonic + " requires a target"});
                ok = false;
            } else {
                auto offset = resolveRelativeText(ops[0], pc, line);
                if (!offset) { ok = false; }
                else {
                    try {
                        word = InstructionWord::encodeB(info.opcode, 0,
                                                        offset.value());
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "brn" || mnemonic == "brz" || mnemonic == "brp") {
            // B-type: rs, target
            if (ops.size() < 2) {
                errors.push_back({line, mnemonic + " requires rs and target"});
                ok = false;
            } else {
                int rs = getReg(ops[0], line);
                auto offset = resolveRelativeText(ops[1], pc, line);
                if (!offset) { ok = false; }
                else {
                    try {
                        word = InstructionWord::encodeB(info.opcode,
                            static_cast<uint8_t>(rs), offset.value());
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "mov" || mnemonic == "movh") {
            // I-type: MOV rd, imm   OR   MOV rd, rs1, imm
            // rs1 is ignored in both cases (MOV always uses r0 as base).
            if (ops.empty()) {
                errors.push_back({line, mnemonic + " requires rd and imm"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int imm_idx = (ops.size() >= 3) ? 2 : 1;  // skip optional rs1
                if ((int)ops.size() <= imm_idx) {
                    errors.push_back({line, mnemonic + " requires an immediate"});
                    ok = false;
                } else {
                    auto imm = resolveAbsolute(ops[imm_idx], line);
                    if (!imm) { ok = false; }
                    else {
                        try {
                            word = InstructionWord::encodeI(info.opcode,
                                static_cast<uint8_t>(rd), 0, imm.value());
                            if (parts.has_width && parts.func != FUNC_T40) {
                                program.push_back(word);
                                const uint8_t sourceFunc = isLaneWidthFunc(parts.func)
                                    ? matchingNumericFunc(parts.func)
                                    : R0_ZERO;
                                program.push_back(InstructionWord::encodeR(
                                    Opcode::CVT,
                                    static_cast<uint8_t>(rd),
                                    static_cast<uint8_t>(rd),
                                    sourceFunc,
                                    parts.func));
                                continue;
                            }
                        } catch (std::out_of_range& e) {
                            errors.push_back({line, std::string(e.what())});
                            ok = false;
                        }
                    }
                }
            }

        } else if (mnemonic == "load") {
            // I-type: LOAD rd, rs1 [, imm]   (imm defaults to 0)
            if (ops.size() < 2) {
                errors.push_back({line, "LOAD requires rd and rs1"});
                ok = false;
            } else {
                int rd   = getReg(ops[0], line);
                int rs1  = getReg(ops[1], line);
                int imm  = 0;
                if (ops.size() >= 3) {
                    auto v = resolveAbsolute(ops[2], line);
                    if (!v) { ok = false; }
                    else imm = v.value();
                }
                if (ok) {
                    try {
                        word = InstructionWord::encodeI(Opcode::LOAD,
                            static_cast<uint8_t>(rd),
                            static_cast<uint8_t>(rs1), imm);
                        if (parts.has_width && parts.func == FUNC_T50) {
                            if (rd + 1 >= REG_COUNT) {
                                errors.push_back({line,
                                    "LOAD.t50 destination pair exceeds the register file"});
                                ok = false;
                            } else {
                                program.push_back(word);
                                program.push_back(InstructionWord::encodeI(
                                    Opcode::LOAD,
                                    static_cast<uint8_t>(rd + 1),
                                    static_cast<uint8_t>(rs1),
                                    imm + 1));
                                program.push_back(InstructionWord::encodeR(
                                    Opcode::COPY,
                                    static_cast<uint8_t>(rd),
                                    static_cast<uint8_t>(rd),
                                    R0_ZERO,
                                    FUNC_T50));
                                continue;
                            }
                        }
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "store") {
            // I-type: STORE rs_store, base [, imm]
            // rs_store = source register (value to write), Rs1 = base address register.
            if (ops.size() < 2) {
                errors.push_back({line, "STORE requires src and base"});
                ok = false;
            } else {
                int src  = getReg(ops[0], line);
                int base = getReg(ops[1], line);
                int imm  = 0;
                if (ops.size() >= 3) {
                    auto v = resolveAbsolute(ops[2], line);
                    if (!v) { ok = false; }
                    else imm = v.value();
                }
                if (ok) {
                    try {
                        word = InstructionWord::encodeS(Opcode::STORE,
                            static_cast<uint8_t>(src),
                            static_cast<uint8_t>(base), imm);
                        if (parts.has_width && parts.func == FUNC_T50) {
                            if (src + 1 >= REG_COUNT) {
                                errors.push_back({line,
                                    "STORE.t50 source pair exceeds the register file"});
                                ok = false;
                            } else {
                                program.push_back(word);
                                program.push_back(InstructionWord::encodeS(
                                    Opcode::STORE,
                                    static_cast<uint8_t>(src + 1),
                                    static_cast<uint8_t>(base),
                                    imm + 1));
                                continue;
                            }
                        }
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "tsel") {
            if (ops.size() != 5) {
                errors.push_back({line, "tsel requires rd, rCond, rNeg, rZero, rPos"});
                ok = false;
            } else {
                int rd    = getReg(ops[0], line);
                int rcond = getReg(ops[1], line);
                int rneg  = getReg(ops[2], line);
                int rzero = getReg(ops[3], line);
                int rpos  = getReg(ops[4], line);
                if (ok) {
                    word = InstructionWord::encodeR5(Opcode::TSEL,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(rcond),
                        static_cast<uint8_t>(rneg),
                        static_cast<uint8_t>(rzero),
                        static_cast<uint8_t>(rpos));
                }
            }

        } else if (mnemonic == "twcmp" || mnemonic == "tclamp") {
            if (ops.size() != 4) {
                errors.push_back({line, mnemonic + " requires rd, rValue, rLow, rHigh"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int value = getReg(ops[1], line);
                int low = getReg(ops[2], line);
                int high = getReg(ops[3], line);
                if (ok) {
                    word = InstructionWord::encodeR4(info.opcode,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(value),
                        static_cast<uint8_t>(low),
                        static_cast<uint8_t>(high),
                        parts.func);
                }
            }

        } else if (mnemonic == "cvt") {
            if (!parts.has_width) {
                errors.push_back({line, "cvt requires a destination width suffix"});
                ok = false;
            }
            if (ok && isLaneWidthFunc(parts.func) && !parts.has_source_width) {
                errors.push_back({line, "cvt to a lane mode requires source and destination suffixes"});
                ok = false;
            }
            if (ok && parts.has_source_width) {
                const int srcWidth = widthFuncTrits(parts.source_func);
                const int dstWidth = widthFuncTrits(parts.func);
                const bool numericToNumeric = isNumericWidthFunc(parts.source_func) && isNumericWidthFunc(parts.func);
                const bool matchingBoundary =
                    srcWidth == dstWidth &&
                    ((isNumericWidthFunc(parts.source_func) && isLaneWidthFunc(parts.func)) ||
                     (isLaneWidthFunc(parts.source_func) && isNumericWidthFunc(parts.func)) ||
                     (isLaneWidthFunc(parts.source_func) && isLaneWidthFunc(parts.func)));
                if (!numericToNumeric && !matchingBoundary) {
                    errors.push_back({line, "invalid cvt source/destination suffix combination"});
                    ok = false;
                }
            }
            if (ops.size() != 2) {
                errors.push_back({line, "cvt requires rd and rs1"});
                ok = false;
            } else {
                int rd  = getReg(ops[0], line);
                int rs1 = getReg(ops[1], line);
                const uint8_t sourceFunc = parts.has_source_width ? parts.source_func : R0_ZERO;
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::CVT,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(rs1),
                        sourceFunc,
                        parts.func);
                }
            }

        } else if (mnemonic == "swap") {
            if (ops.size() != 2) {
                errors.push_back({line, "swap requires rA and rB"});
                ok = false;
            } else {
                int ra = getReg(ops[0], line);
                int rb = getReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::SWAP,
                        static_cast<uint8_t>(ra),
                        static_cast<uint8_t>(rb),
                        R0_ZERO,
                        FUNC_DEFAULT);
                }
            }

        } else if (mnemonic == "tmac") {
            if (ops.size() != 2) {
                errors.push_back({line, "tmac requires rA and rB"});
                ok = false;
            } else {
                int ra = getReg(ops[0], line);
                int rb = getReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::TMAC,
                        R0_ZERO,
                        static_cast<uint8_t>(ra),
                        static_cast<uint8_t>(rb),
                        parts.func);
                }
            }

        } else if (mnemonic == "vadd" || mnemonic == "vsub" ||
                   mnemonic == "vmul" || mnemonic == "vdiv" ||
                   mnemonic == "vcmp") {
            if (ops.size() != 3) {
                errors.push_back({line, mnemonic + " requires vD, vA, vB"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int va = getVecReg(ops[1], line);
                int vb = getVecReg(ops[2], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(va),
                        static_cast<uint8_t>(vb),
                        parts.func);
                }
            }

        } else if (mnemonic == "vneg") {
            if (ops.size() != 2) {
                errors.push_back({line, "vneg requires vD and vS"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int vs = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VNEG,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vs),
                        R0_ZERO,
                        parts.func);
                }
            }

        } else if (mnemonic == "vbcast") {
            if (ops.size() != 2) {
                errors.push_back({line, "vbcast requires vD and rS"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int rs = getReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VBCAST,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(rs),
                        R0_ZERO,
                        parts.func);
                }
            }

        } else if (mnemonic == "vsel") {
            if (ops.size() != 5) {
                errors.push_back({line, "vsel requires vD, vCond, vNeg, vZero, vPos"});
                ok = false;
            } else {
                int vd    = getVecReg(ops[0], line);
                int vcond = getVecReg(ops[1], line);
                int vneg  = getVecReg(ops[2], line);
                int vzero = getVecReg(ops[3], line);
                int vpos  = getVecReg(ops[4], line);
                if (ok) {
                    word = InstructionWord::encodeR5(Opcode::VSEL,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vcond),
                        static_cast<uint8_t>(vneg),
                        static_cast<uint8_t>(vzero),
                        static_cast<uint8_t>(vpos),
                        parts.func);
                }
            }

        } else if (mnemonic == "vload" || mnemonic == "vstore") {
            if (ops.size() < 2 || ops.size() > 3) {
                errors.push_back({line, mnemonic + " requires vReg, rBase [, imm]"});
                ok = false;
            } else {
                int vreg = getVecReg(ops[0], line);
                int base = getReg(ops[1], line);
                int imm = 0;
                if (ops.size() >= 3) {
                    auto v = resolveAbsolute(ops[2], line);
                    if (!v) ok = false;
                    else imm = v.value();
                }
                if (ok) {
                    try {
                        word = InstructionWord::encodeVectorMemory(info.opcode,
                            static_cast<uint8_t>(vreg),
                            static_cast<uint8_t>(base),
                            imm,
                            parts.func);
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "vlen") {
            if (parts.has_width) {
                errors.push_back({line, "vlen does not take a width suffix"});
                ok = false;
            } else if (ops.size() != 1) {
                errors.push_back({line, "vlen requires rd"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VLEN,
                        static_cast<uint8_t>(rd),
                        R0_ZERO,
                        R0_ZERO,
                        FUNC_DEFAULT);
                }
            }

        } else if (mnemonic == "aclr") {
            if (!ops.empty()) {
                errors.push_back({line, "aclr takes no operands"});
                ok = false;
            } else {
                word = InstructionWord::encodeR(Opcode::ACLR, R0_ZERO, R0_ZERO, R0_ZERO, parts.func);
            }

        } else if (mnemonic == "aload" || mnemonic == "aadd" ||
                   mnemonic == "asub" || mnemonic == "amul") {
            if (ops.size() != 1) {
                errors.push_back({line, mnemonic + " requires rS"});
                ok = false;
            } else {
                int rs = getReg(ops[0], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        R0_ZERO,
                        static_cast<uint8_t>(rs),
                        R0_ZERO,
                        parts.func);
                }
            }

        } else if (mnemonic == "astore") {
            if (ops.size() != 1) {
                errors.push_back({line, "astore requires rD"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::ASTORE,
                        static_cast<uint8_t>(rd),
                        R0_ZERO,
                        R0_ZERO,
                        parts.func);
                }
            }

        } else if (mnemonic == "vdot") {
            if (ops.size() != 3) {
                errors.push_back({line, "vdot requires rD, vA, vB"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int va = getVecReg(ops[1], line);
                int vb = getVecReg(ops[2], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VDOT,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(va),
                        static_cast<uint8_t>(vb),
                        FUNC_T1);
                }
            }

        } else if (mnemonic == "vmac") {
            if (ops.size() != 2) {
                errors.push_back({line, "vmac requires vA, vB"});
                ok = false;
            } else {
                int va = getVecReg(ops[0], line);
                int vb = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VMAC,
                        R0_ZERO,
                        static_cast<uint8_t>(va),
                        static_cast<uint8_t>(vb),
                        FUNC_T1);
                }
            }

        } else if (mnemonic == "vact") {
            if (ops.size() != 2) {
                errors.push_back({line, "vact requires vD, vS"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int vs = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VACT,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vs),
                        R0_ZERO,
                        FUNC_T1);
                }
            }

        } else if (mnemonic == "vpack" || mnemonic == "vunpack") {
            if (ops.size() != 2) {
                errors.push_back({line, mnemonic + " requires vD, vS"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int vs = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vs),
                        parts.source_func,
                        parts.func);
                }
            }

        } else if (mnemonic == "vpermute") {
            if (ops.size() != 3) {
                errors.push_back({line, "vpermute requires vD, vS, vIndex"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int vs = getVecReg(ops[1], line);
                int vi = getVecReg(ops[2], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VPERMUTE,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vs),
                        static_cast<uint8_t>(vi),
                        parts.func);
                }
            }

        } else if (mnemonic == "vblend") {
            if (ops.size() != 4) {
                errors.push_back({line, "vblend requires vD, vCond, vFalse, vTrue"});
                ok = false;
            } else {
                int vd = getVecReg(ops[0], line);
                int vc = getVecReg(ops[1], line);
                int vf = getVecReg(ops[2], line);
                int vt = getVecReg(ops[3], line);
                if (ok) {
                    word = InstructionWord::encodeR5(Opcode::VBLEND,
                        static_cast<uint8_t>(vd),
                        static_cast<uint8_t>(vc),
                        static_cast<uint8_t>(vf),
                        static_cast<uint8_t>(vf),
                        static_cast<uint8_t>(vt),
                        parts.func);
                }
            }

        } else if (mnemonic == "vswap") {
            if (ops.size() != 2) {
                errors.push_back({line, "vswap requires vA, vB"});
                ok = false;
            } else {
                int va = getVecReg(ops[0], line);
                int vb = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(Opcode::VSWAP,
                        static_cast<uint8_t>(va),
                        static_cast<uint8_t>(vb),
                        R0_ZERO,
                        FUNC_DEFAULT);
                }
            }

        } else if (mnemonic == "vgather" || mnemonic == "vscatter") {
            if (ops.size() != 3) {
                errors.push_back({line, mnemonic + " requires vReg, rBase, vIndex"});
                ok = false;
            } else {
                int vreg = getVecReg(ops[0], line);
                int base = getReg(ops[1], line);
                int vi = getVecReg(ops[2], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        static_cast<uint8_t>(vreg),
                        static_cast<uint8_t>(base),
                        static_cast<uint8_t>(vi),
                        parts.func);
                }
            }

        } else if (mnemonic == "vsum" || mnemonic == "vhmin" || mnemonic == "vhmax") {
            if (ops.size() != 2) {
                errors.push_back({line, mnemonic + " requires rD and vS"});
                ok = false;
            } else {
                int rd = getReg(ops[0], line);
                int vs = getVecReg(ops[1], line);
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(vs),
                        R0_ZERO,
                        parts.func);
                }
            }

        } else if (info.fmt == InstructionFormat::R_TYPE) {
            if (mnemonic == "cvt" && !parts.has_width) {
                errors.push_back({line, "CVT requires a width suffix"});
                ok = false;
            }
            // R-type: rd, rs1 [, rs2]   (rs2 defaults to r0 if rs2_optional)
            if (ops.size() < 2) {
                errors.push_back({line, mnemonic + " requires at least rd and rs1"});
                ok = false;
            } else {
                int rd  = getReg(ops[0], line);
                int rs1 = getReg(ops[1], line);
                int rs2 = 0;
                if (ops.size() >= 3) {
                    rs2 = getReg(ops[2], line);
                } else if (!info.rs2_optional) {
                    errors.push_back({line, mnemonic + " requires rd, rs1, rs2"});
                    ok = false;
                }
                if (ok) {
                    word = InstructionWord::encodeR(info.opcode,
                        static_cast<uint8_t>(rd),
                        static_cast<uint8_t>(rs1),
                        static_cast<uint8_t>(rs2),
                        parts.func);
                }
            }

        } else {
            errors.push_back({line, "Unhandled mnemonic '" + sl.mnemonic + "'"});
            ok = false;
        }

        if (!ok) word = TritWord27{};  // safe placeholder (NOP)
        program.push_back(word);
    }

    return program;
}

[[nodiscard]] inline std::vector<TernaryValue> encodeData(
        const std::vector<SourceLine>& lines,
        const std::map<std::string, int>& text_labels,
        const std::map<std::string, int>& data_labels,
        std::vector<AssemblyError>& errors) {

    std::vector<TernaryValue> data;

    auto resolveAbsolute = [&](const std::string& tok,
                               int line_num) -> std::optional<int> {
        auto parsed = parseImmOrLabel(tok);
        if (parsed.isLabel) {
            auto text_it = text_labels.find(parsed.labelName);
            if (text_it != text_labels.end()) return text_it->second;
            auto data_it = data_labels.find(parsed.labelName);
            if (data_it != data_labels.end()) return data_it->second;
            errors.push_back({line_num,
                "Undefined label '" + parsed.labelName + "'"});
            return std::nullopt;
        }
        return parsed.imm;
    };

    auto requireFlag = [&](const std::string& tok,
                           int line_num,
                           const std::string& name) -> std::optional<bool> {
        auto value = resolveAbsolute(tok, line_num);
        if (!value) return std::nullopt;
        if (value.value() != 0 && value.value() != 1) {
            errors.push_back({line_num, ".pte " + name + " flag must be 0 or 1"});
            return std::nullopt;
        }
        return value.value() != 0;
    };

    for (auto& sl : lines) {
        if (sl.section != AssemblySection::Data || sl.address < 0) continue;

        while (static_cast<int>(data.size()) < sl.address) {
            data.push_back(TernaryValue::zero());
        }
        if (sl.mnemonic == ".word") {
            for (const std::string& operand : sl.operands) {
                auto value = resolveAbsolute(operand, sl.line_num);
                data.push_back(value ? ops::fromLong(value.value()) : TernaryValue::zero());
            }
        } else if (sl.mnemonic == ".execheader") {
            std::array<int, 6> fields{};
            bool ok = true;
            for (int i = 0; i < 6; ++i) {
                auto value = resolveAbsolute(sl.operands[static_cast<std::size_t>(i)], sl.line_num);
                if (!value) {
                    ok = false;
                } else {
                    fields[static_cast<std::size_t>(i)] = value.value();
                }
            }
            if (!ok) {
                for (int i = 0; i < EXEC_HEADER_WORDS; ++i) data.push_back(TernaryValue::zero());
                continue;
            }
            auto header = encodeExecutableHeader(
                fields[0],
                fields[1],
                fields[2],
                fields[3],
                fields[4],
                fields[5]);
            data.insert(data.end(), header.begin(), header.end());
        } else if (sl.mnemonic == ".execheader2") {
            std::array<int, 7> fields{};
            bool ok = true;
            for (int index = 0; index < 7; ++index) {
                auto value = resolveAbsolute(
                    sl.operands[static_cast<std::size_t>(index)],
                    sl.line_num);
                if (!value) {
                    ok = false;
                } else {
                    fields[static_cast<std::size_t>(index)] = value.value();
                }
            }
            ExecutableImageHeaderV2 header;
            if (ok && !featureMaskFromNumeric(
                          fields[4], header.required_features)) {
                errors.push_back({sl.line_num,
                    ".execheader2 feature word contains a negative trit"});
                ok = false;
            }
            if (ok) {
                header.entry_pc = fields[0];
                header.text_words = fields[1];
                header.data_words = fields[2];
                header.stack_words = fields[3];
                header.syscall_abi_version = fields[5];
                header.flags = fields[6];
                try {
                    auto encoded = encodeExecutableHeaderV2(header);
                    data.insert(data.end(), encoded.begin(), encoded.end());
                    continue;
                } catch (const std::exception& error) {
                    errors.push_back({sl.line_num, error.what()});
                }
            }
            for (int index = 0; index < EXEC_V2_HEADER_WORDS; ++index)
                data.push_back(TernaryValue::zero());
        } else if (sl.mnemonic == ".pte") {
            auto ppn = resolveAbsolute(sl.operands[0], sl.line_num);
            auto user = requireFlag(sl.operands[1], sl.line_num, "user");
            auto read = requireFlag(sl.operands[2], sl.line_num, "read");
            auto write = requireFlag(sl.operands[3], sl.line_num, "write");
            auto execute = requireFlag(sl.operands[4], sl.line_num, "execute");
            std::optional<bool> present = true;
            if (sl.operands.size() == 6) {
                present = requireFlag(sl.operands[5], sl.line_num, "present");
            }
            if (!ppn || ppn.value() < 0 || !user || !read || !write || !execute || !present) {
                if (ppn && ppn.value() < 0) {
                    errors.push_back({sl.line_num, ".pte physical page number must be non-negative"});
                }
                data.push_back(TernaryValue::zero());
                continue;
            }
            TernaryValue pte = encodePageTableEntry(
                ppn.value(),
                user.value(),
                read.value(),
                write.value(),
                execute.value(),
                present.value());
            if (pte.isInvalid()) {
                errors.push_back({sl.line_num, ".pte physical page number is out of range"});
                data.push_back(TernaryValue::zero());
            } else {
                data.push_back(pte);
            }
        }
    }

    return data;
}

[[nodiscard]] inline std::map<std::string, ExecutableImageHeader> collectExecutableHeaders(
        const std::vector<SourceLine>& lines,
        const std::vector<TernaryValue>& data,
        std::vector<AssemblyError>& errors) {

    std::map<std::string, ExecutableImageHeader> headers;
    for (const SourceLine& sl : lines) {
        if (sl.section != AssemblySection::Data ||
            sl.mnemonic != ".execheader" ||
            sl.address < 0) {
            continue;
        }
        if (sl.label.empty()) {
            errors.push_back({sl.line_num, ".execheader requires a label"});
            continue;
        }
        ExecutableImageHeader header;
        if (!decodeExecutableHeader(data, sl.address, header)) {
            errors.push_back({sl.line_num, "Invalid executable header"});
            continue;
        }
        headers[sl.label] = header;
    }
    return headers;
}

[[nodiscard]] inline std::map<std::string, ExecutableImageHeaderV2>
collectExecutableHeadersV2(
        const std::vector<SourceLine>& lines,
        const std::vector<TernaryValue>& data,
        std::vector<AssemblyError>& errors) {
    std::map<std::string, ExecutableImageHeaderV2> headers;
    for (const SourceLine& line : lines) {
        if (line.section != AssemblySection::Data ||
            line.mnemonic != ".execheader2" || line.address < 0) {
            continue;
        }
        if (line.label.empty()) {
            errors.push_back(
                {line.line_num, ".execheader2 requires a label"});
            continue;
        }
        ExecutableImageHeaderV2 header;
        if (!decodeExecutableHeaderV2(data, line.address, header)) {
            errors.push_back(
                {line.line_num, "Invalid executable header v2"});
            continue;
        }
        headers[line.label] = header;
    }
    return headers;
}

// =============================================================================
// SECTION 7 — Public API: assemble()
// =============================================================================

// Assemble source text into a TritWord27 program image.
// Returns an AssemblyResult with success flag, program, errors, and label map.
[[nodiscard]] inline AssemblyResult assemble(
        const std::string& source,
        const AssemblyOptions& options) {
    AssemblyResult result;

    const ArchitectureDirectives architecture_directives =
        parseArchitectureDirectives(source, options, result.errors);
    result.isa_version = architecture_directives.isa;
    result.required_features = architecture_directives.required_features;
    if (!result.errors.empty()) return result;

    auto lines = parseSources(source, result.errors);
    if (!result.errors.empty()) return result;

    LabelMaps labels = buildLabelMaps(lines, result.errors);
    result.labels = labels.text;
    result.data_labels = labels.data;
    if (!result.errors.empty()) return result;

    result.program = encode(lines, result.labels, result.data_labels, result.errors);
    if (result.errors.empty() &&
        result.isa_version == IsaEncodingVersion::V2) {
        for (std::size_t pc = 0; pc < result.program.size(); ++pc) {
            const SourceLine* origin = nullptr;
            for (const auto& line : lines) {
                if (line.section == AssemblySection::Text &&
                    line.address >= 0 &&
                    static_cast<int>(pc) >= line.address &&
                    static_cast<int>(pc) < line.address + line.word_count) {
                    origin = &line;
                    break;
                }
            }
            if (origin != nullptr &&
                splitMnemonic(origin->mnemonic).base == "wait") {
                const std::uint64_t wait_feature =
                    featureBit(architecture::v2::FEATURE_WAIT);
                if ((result.required_features & wait_feature) == 0) {
                    result.errors.push_back({origin->line_num,
                        "WAIT requires .require wait"});
                } else {
                    result.program[pc] = VersionedInstructionCodec::encodeB(
                        Opcode::WAIT, R0_ZERO, 0,
                        IsaEncodingVersion::V2);
                }
                continue;
            }
            if (origin != nullptr &&
                splitMnemonic(origin->mnemonic).base == "tlbinv") {
                const std::uint64_t mmu_feature =
                    featureBit(architecture::v2::FEATURE_MMU);
                if ((result.required_features & mmu_feature) == 0) {
                    result.errors.push_back({origin->line_num,
                        "TLBINV requires .require mmu"});
                } else {
                    const int address =
                        parseRegister(origin->operands[0]);
                    const int target_asid =
                        parseRegister(origin->operands[1]);
                    const int scope =
                        parseRegister(origin->operands[2]);
                    result.program[pc] = VersionedInstructionCodec::encodeR(
                        Opcode::TLBINV,
                        static_cast<uint8_t>(address),
                        static_cast<uint8_t>(target_asid),
                        static_cast<uint8_t>(scope),
                        FUNC_DEFAULT,
                        IsaEncodingVersion::V2);
                }
                continue;
            }

            const InstructionWord decoded = VersionedInstructionCodec::decode(
                result.program[pc], IsaEncodingVersion::V1);
            const std::uint64_t missing =
                requiredV2Features(decoded) & ~result.required_features;
            if (missing != 0) {
                int source_line = 0;
                for (const auto& line : lines) {
                    if (line.section == AssemblySection::Text &&
                        line.address >= 0 &&
                        static_cast<int>(pc) >= line.address &&
                        static_cast<int>(pc) < line.address + line.word_count) {
                        source_line = line.line_num;
                        break;
                    }
                }
                result.errors.push_back({source_line,
                    "Instruction requires an undeclared ISA v2 feature; "
                    "add a matching .require directive"});
                continue;
            }
            try {
                result.program[pc] =
                    transcodeV1InstructionToV2(result.program[pc]);
            } catch (const std::exception& error) {
                result.errors.push_back({0,
                    std::string("ISA v2 encoding failed: ") + error.what()});
            }
        }
    }
    result.data = encodeData(lines, result.labels, result.data_labels, result.errors);
    result.executable_headers = collectExecutableHeaders(lines, result.data, result.errors);
    result.executable_headers_v2 =
        collectExecutableHeadersV2(lines, result.data, result.errors);
    for (const auto& [label, header] : result.executable_headers_v2) {
        if (result.isa_version != IsaEncodingVersion::V2) {
            result.errors.push_back({
                header.header_addr,
                ".execheader2 '" + label +
                    "' requires an ISA v2 assembly unit"});
        } else if (header.required_features != result.required_features) {
            result.errors.push_back({
                header.header_addr,
                ".execheader2 '" + label +
                    "' feature word must exactly match the unit's .require "
                    "feature set"});
        }
    }
    result.success = result.errors.empty();
    return result;
}

// Transition-only compatibility entry point. Existing embedded v1 assembly
// remains readable for one release; new toolchain callers use strict options.
[[nodiscard]] inline AssemblyResult assemble(const std::string& source) {
    return assemble(source, AssemblyOptions{});
}

[[nodiscard]] inline AssemblyResult assembleV2(const std::string& source) {
    return assemble(source, AssemblyOptions{IsaEncodingVersion::V2, true});
}

// Convenience: assemble and throw on any error.
[[nodiscard]] inline std::vector<TritWord27> assembleOrThrow(
        const std::string& source) {
    return assemble(source).require();
}

// Load both text and data images produced by assemble(). Data labels are
// absolute DMEM offsets starting at zero.
inline bool loadAndReset(VMState& vm, const AssemblyResult& assembled) {
    if (!assembled.success) return false;
    if (static_cast<int>(assembled.program.size()) > vm.imem.size()) return false;
    if (static_cast<int>(assembled.data.size()) > vm.dmem.size()) return false;

    vm.coldReset();
    if (!vm.configureArchitecture(
            assembled.isa_version, assembled.required_features))
        return false;
    if (!vm.imem.loadProgram(assembled.program, 0)) return false;
    for (int i = 0; i < static_cast<int>(assembled.data.size()); ++i) {
        if (vm.dmem.store(i, assembled.data[static_cast<std::size_t>(i)]) != MemFaultCode::OK) {
            return false;
        }
    }
    vm.standalone_heap_break =
        std::max<long long>(vm.standalone_heap_break,
                            static_cast<long long>(assembled.data.size()) + 16);
    return true;
}

// =============================================================================
// SECTION 8 — Disassembler: program image → annotated source listing
// =============================================================================
// Useful for debugging: takes an assembled program and produces the
// mnemonic listing with addresses, matching the format the SystemVerilog
// testbench will consume for annotation.

[[nodiscard]] inline std::string listing(
        const std::vector<TritWord27>& program,
        const std::map<std::string, int>& labels,
        IsaEncodingVersion version) {

    // Build reverse label map: address → label name
    std::map<int, std::string> addr_to_label;
    for (auto& [name, addr] : labels)
        addr_to_label[addr] = name;

    std::ostringstream oss;
    for (int i = 0; i < static_cast<int>(program.size()); ++i) {
        // Print label if present
        auto lit = addr_to_label.find(i);
        if (lit != addr_to_label.end()) {
            oss << lit->second << ":\n";
        }
        // Address and disassembly
        oss << "  [" << std::setw(4) << std::setfill('0') << i << "]  "
            << disassemble(program[i], version) << "\n";
    }
    return oss.str();
}

[[nodiscard]] inline std::string listing(
        const std::vector<TritWord27>& program,
        const std::map<std::string, int>& labels = {}) {
    return listing(program, labels, IsaEncodingVersion::V1);
}

[[nodiscard]] inline std::string listing(const AssemblyResult& assembled) {
    return listing(
        assembled.program, assembled.labels, assembled.isa_version);
}

// =============================================================================
// SECTION 9 — Phase 4 Verification
// =============================================================================

inline bool verifyAssembler() {
    bool ok = true;

    // --- Round-trip: encode then decode a single instruction ---
    {
        auto prog = assembleOrThrow("ADD r3, r1, r2\nHALT");
        ok &= (prog.size() == 2);
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (!iw.malformed && iw.opcode == Opcode::ADD);
        ok &= (iw.rd == 3 && iw.rs1 == 1 && iw.rs2 == 2);
    }

    // --- Register aliases ---
    {
        auto prog = assembleOrThrow("COPY r1, zero\nHALT");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (!iw.malformed && iw.opcode == Opcode::COPY);
        ok &= (iw.rs1 == 0);
    }

    // --- MOV immediate ---
    {
        auto prog = assembleOrThrow("MOV r5, -42\nHALT");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::MOV && iw.rd == 5 && iw.imm == -42);
    }

    // --- LOAD with offset ---
    {
        auto prog = assembleOrThrow("LOAD r4, sp, -8\nHALT");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::LOAD && iw.rd == 4 &&
               iw.rs1 == 26 && iw.imm == -8);
    }

    // --- STORE with offset ---
    {
        auto prog = assembleOrThrow("STORE r1, sp, -4\nHALT");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::STORE && iw.rs_store == 1 &&
               iw.rs1 == 26 && iw.imm == -4);
    }

    // --- Label-based BRN (forward and backward) ---
    {
        // Backward branch: BRN at PC 4 should have offset = 0 - 4 = -4
        std::string src = R"(
            MOV r1, 0
            MOV r2, 5
            MOV r3, 1
            MOV r4, 0
loop:
            ADD r1, r1, r2
            SUB r2, r2, r3
            TCMP r5, r2, r4
            TINV r5, r5
            BRN r5, loop
            HALT
        )";
        auto res = assemble(src);
        ok &= res.success;
        ok &= (res.labels.count("loop") && res.labels.at("loop") == 4);
        // BRN is at PC 8, loop is at PC 4: offset = 4 - 8 = -4
        auto iw_brn = InstructionWord::decode(res.program[8]);
        ok &= (iw_brn.opcode == Opcode::BRN && iw_brn.offset == -4);
    }

    // --- Forward JMP ---
    {
        std::string src = R"(
            MOV r1, 1
            JMP skip
            MOV r1, 999
skip:
            HALT
        )";
        auto res = assemble(src);
        ok &= res.success;
        // JMP at PC 1, skip at PC 3: offset = 3 - 1 = 2
        auto iw_jmp = InstructionWord::decode(res.program[1]);
        ok &= (iw_jmp.opcode == Opcode::JMP && iw_jmp.offset == 2);
    }

    // --- CALL/RET round-trip in the assembler ---
    {
        std::string src = R"(
            MOV r1, 10
            CALL add_one
            HALT
add_one:
            MOV r2, 1
            ADD r1, r1, r2
            RET
        )";
        auto res = assemble(src);
        ok &= res.success;
        // CALL at PC 1, add_one at PC 3: offset = 3 - 1 = 2
        auto iw_call = InstructionWord::decode(res.program[1]);
        ok &= (iw_call.opcode == Opcode::CALL && iw_call.offset == 2);
        auto iw_ret = InstructionWord::decode(res.program[5]);
        ok &= (iw_ret.opcode == Opcode::RET);
    }

    // --- Error: unknown mnemonic ---
    {
        auto res = assemble("FROBBLE r1, r2\nHALT");
        ok &= !res.success;
        ok &= !res.errors.empty();
    }

    // --- Error: undefined label ---
    {
        auto res = assemble("JMP nowhere\nHALT");
        ok &= !res.success;
    }

    // --- Error: duplicate label ---
    {
        auto res = assemble("foo:\nfoo:\nHALT");
        ok &= !res.success;
    }

    // --- Comment stripping and blank line tolerance ---
    {
        std::string src = R"(
            ; This is a comment
            MOV r1, 7   ; inline comment

            ; blank lines above are fine
            HALT
        )";
        auto res = assemble(src);
        ok &= res.success;
        ok &= (res.program.size() == 2);
    }

    // --- Case insensitivity ---
    {
        auto prog = assembleOrThrow("add R3, R1, R2\nhalt");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::ADD && iw.rd == 3);
    }

    // --- Phase 4A base ISA additions ---
    {
        auto prog = assembleOrThrow("tsel r6, r3, r1, r2, r4\nhalt");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (!iw.malformed && iw.opcode == Opcode::TSEL && iw.r5_layout);
        ok &= (iw.rd == 6 && iw.rcond == 3 && iw.rneg == 1 &&
               iw.rzero == 2 && iw.rpos == 4);
    }
    {
        auto prog = assembleOrThrow("brz r1, 2\nbrp r2, -1\nhalt");
        auto brz = InstructionWord::decode(prog[0]);
        auto brp = InstructionWord::decode(prog[1]);
        ok &= (brz.opcode == Opcode::BRZ && brz.rs_branch == 1 && brz.offset == 2);
        ok &= (brp.opcode == Opcode::BRP && brp.rs_branch == 2 && brp.offset == -1);
    }
    {
        auto prog = assembleOrThrow("swap r1, r2\nhalt");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::SWAP && iw.rd == 1 && iw.rs1 == 2);
    }
    {
        auto prog = assembleOrThrow("cvt.t10.t20 r3, r2\nhalt");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::CVT && iw.rs2 == FUNC_T10 && iw.func == FUNC_T20);
    }
    {
        auto prog = assembleOrThrow("mov.l20 r1, 7\ntladd.l20 r3, r1, r1\ntlneg.l20 r4, r3\nhalt");
        ok &= (prog.size() == 5);
        auto movCvt = InstructionWord::decode(prog[1]);
        auto add = InstructionWord::decode(prog[2]);
        auto neg = InstructionWord::decode(prog[3]);
        ok &= (movCvt.opcode == Opcode::CVT && movCvt.rs2 == FUNC_T20 && movCvt.func == FUNC_L20);
        ok &= (add.opcode == Opcode::TLADD && add.func == FUNC_L20);
        ok &= (neg.opcode == Opcode::TLNEG && neg.func == FUNC_L20);
    }
    {
        ok &= !assemble("tladd.t20 r1, r2, r3\n").success;
        ok &= !assemble("tladd r1, r2, r3\n").success;
        ok &= !assemble("cvt.t20.l10 r1, r2\n").success;
        ok &= !assemble("add.l20 r1, r2, r3\n").success;
    }
    {
        auto prog = assembleOrThrow("vlen r3\nhalt");
        auto iw = InstructionWord::decode(prog[0]);
        ok &= (iw.opcode == Opcode::VLEN && iw.rd == 3);
        ok &= (parseVectorRegister("v7") == 7 && parseVectorRegister("v8") < 0);
        ok &= !assemble("vlen v0\nhalt").success;
    }

    return ok;
}

} // namespace assembler
} // namespace vm
} // namespace sandbox

#endif // TERNARY_ASM_H
