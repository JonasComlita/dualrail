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
    std::vector<AssemblyError>    errors;
    std::map<std::string, int>    labels;   // label name → word address

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
    uint8_t func = FUNC_T50;
    bool has_source_width = false;
    uint8_t source_func = 0;
    bool suffix_valid = true;
};

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
    out.has_width = true;
    const size_t dot2 = lowered.find('.', dot + 1);
    if (dot2 == std::string::npos) {
        const std::string suffix = lowered.substr(dot + 1);
        out.suffix_valid = parseWidthSuffix(suffix, out.func);
    } else {
        const std::string suffix1 = lowered.substr(dot + 1, dot2 - dot - 1);
        const std::string suffix2 = lowered.substr(dot2 + 1);
        out.suffix_valid = out.base == "cvt" &&
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
           base == "cvt" || base == "mov" ||
           base == "tladd" || base == "tlsub" || base == "tlneg" ||
           base == "tland" || base == "tlor" ||
           base == "vadd" || base == "vsub" || base == "vneg" ||
           base == "vmul" || base == "vdiv" || base == "vcmp" ||
           base == "vsel" || base == "vbcast" ||
           base == "vload" || base == "vstore";
}

[[nodiscard]] inline int instructionWordCount(const std::string& mnemonic) {
    const auto parts = splitMnemonic(mnemonic);
    return (parts.base == "mov" && parts.has_width &&
            parts.suffix_valid && !parts.has_source_width) ? 2 : 1;
}

// Build the opcode lookup table.
[[nodiscard]] inline std::map<std::string, OpcodeInfo> buildOpcodeTable() {
    using F = InstructionFormat;
    std::map<std::string, OpcodeInfo> t;

    // System
    t["nop"]  = {Opcode::NOP,  F::I_TYPE, 0, false};
    t["halt"] = {Opcode::HALT, F::B_TYPE, 0, false};

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
    t["ret"]  = {Opcode::RET,  F::R_TYPE, 0, false};

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

    return t;
}

static const std::map<std::string, OpcodeInfo> OPCODE_TABLE = buildOpcodeTable();

// =============================================================================
// SECTION 6 — Two-Pass Assembler
// =============================================================================

// Internal representation of one parsed source line.
struct SourceLine {
    int                      line_num;    // 1-based
    std::string              label;       // empty if no label on this line
    std::string              mnemonic;    // empty for label-only lines
    std::vector<std::string> operands;    // raw operand tokens
    int                      address;     // word address assigned in pass 1 (-1 = no instruction)
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
    int word_addr = 0;

    while (std::getline(stream, raw)) {
        ++line_num;
        std::string s = trim(stripComment(raw));
        if (s.empty()) continue;

        SourceLine sl;
        sl.line_num = line_num;
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

        sl.address = word_addr;
        word_addr += instructionWordCount(sl.mnemonic);
        lines.push_back(sl);
    }

    return lines;
}

// Pass 1: build the label → address map from parsed source lines.
[[nodiscard]] inline std::map<std::string, int> buildLabelMap(
        const std::vector<SourceLine>& lines,
        std::vector<AssemblyError>& errors) {

    std::map<std::string, int> labels;

    // Find the address of each label. A label defined on its own line
    // points to the next instruction's address. A label on an instruction
    // line points to that instruction's address.
    std::string pending_label;
    int pending_line = 0;

    for (auto& sl : lines) {
        if (!sl.label.empty()) {
            if (labels.count(sl.label)) {
                errors.push_back({sl.line_num,
                    "Duplicate label '" + sl.label + "'"});
            } else if (!pending_label.empty() && pending_label == sl.label) {
                // Second definition of the same label before any instruction.
                errors.push_back({sl.line_num,
                    "Duplicate label '" + sl.label + "'"});
            } else if (sl.address >= 0) {
                // Label is on the same line as an instruction.
                labels[sl.label] = sl.address;
            } else {
                // Label-only line: remember it; next instruction gets the address.
                pending_label = sl.label;
                pending_line  = sl.line_num;
            }
        }

        if (!pending_label.empty() && sl.address >= 0) {
            if (labels.count(pending_label)) {
                errors.push_back({pending_line,
                    "Duplicate label '" + pending_label + "'"});
            } else {
                labels[pending_label] = sl.address;
            }
            pending_label.clear();
        }
    }

    if (!pending_label.empty()) {
        errors.push_back({pending_line,
            "Label '" + pending_label + "' defined after last instruction"});
    }

    return labels;
}

// Pass 2: encode each source line into a TritWord27.
// Resolves label references to PC-relative offsets.
[[nodiscard]] inline std::vector<TritWord27> encode(
        const std::vector<SourceLine>& lines,
        const std::map<std::string, int>& labels,
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

    // Helper: resolve an immediate or label to an integer.
    // For labels in branch instructions, computes PC-relative offset.
    auto resolveImm = [&](const std::string& tok, int pc,
                           int line_num) -> std::optional<int> {
        auto parsed = parseImmOrLabel(tok);
        if (parsed.isLabel) {
            auto it = labels.find(parsed.labelName);
            if (it == labels.end()) {
                errors.push_back({line_num,
                    "Undefined label '" + parsed.labelName + "'"});
                return std::nullopt;
            }
            return it->second - pc;   // PC-relative offset
        }
        return parsed.imm;
    };

    for (auto& sl : lines) {
        if (sl.address < 0) continue;  // label-only line, no instruction

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
        const bool laneMnemonic = parts.base == "tladd" || parts.base == "tlsub" ||
                                  parts.base == "tlneg" || parts.base == "tland" ||
                                  parts.base == "tlor";
        const bool numericMnemonic =
            parts.base == "add" || parts.base == "sub" || parts.base == "mul" ||
            parts.base == "div" || parts.base == "sqrt" || parts.base == "neg" ||
            parts.base == "abs" || parts.base == "tinv" || parts.base == "tcmp" ||
            parts.base == "tmin" || parts.base == "tmax";
        const bool vectorNumericMnemonic =
            parts.base == "vadd" || parts.base == "vsub" || parts.base == "vneg" ||
            parts.base == "vmul" || parts.base == "vdiv" || parts.base == "vcmp" ||
            parts.base == "vsel" || parts.base == "vbcast" ||
            parts.base == "vload" || parts.base == "vstore";
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
        if (vectorNumericMnemonic && (!parts.has_width || !isNumericWidthFunc(parts.func))) {
            errors.push_back({sl.line_num,
                "Vector numeric mnemonic '" + parts.base + "' requires a .tN suffix"});
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

        } else if (mnemonic == "ret") {
            word = InstructionWord::encodeR(Opcode::RET, 0, 0, 0);

        } else if (mnemonic == "jmp" || mnemonic == "call") {
            // B-type: target only (rs = r0)
            if (ops.empty()) {
                errors.push_back({line, mnemonic + " requires a target"});
                ok = false;
            } else {
                auto offset = resolveImm(ops[0], pc, line);
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
                auto offset = resolveImm(ops[1], pc, line);
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
                    auto imm = resolveImm(ops[imm_idx], pc, line);
                    if (!imm) { ok = false; }
                    else {
                        try {
                            word = InstructionWord::encodeI(info.opcode,
                                static_cast<uint8_t>(rd), 0, imm.value());
                            if (parts.has_width) {
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
                    auto v = resolveImm(ops[2], pc, line);
                    if (!v) { ok = false; }
                    else imm = v.value();
                }
                if (ok) {
                    try {
                        word = InstructionWord::encodeI(Opcode::LOAD,
                            static_cast<uint8_t>(rd),
                            static_cast<uint8_t>(rs1), imm);
                    } catch (std::out_of_range& e) {
                        errors.push_back({line, std::string(e.what())});
                        ok = false;
                    }
                }
            }

        } else if (mnemonic == "store") {
            // I-type: STORE src, base [, imm]
            // Rd field = src (value to write), Rs1 field = base address register.
            if (ops.size() < 2) {
                errors.push_back({line, "STORE requires src and base"});
                ok = false;
            } else {
                int src  = getReg(ops[0], line);
                int base = getReg(ops[1], line);
                int imm  = 0;
                if (ops.size() >= 3) {
                    auto v = resolveImm(ops[2], pc, line);
                    if (!v) { ok = false; }
                    else imm = v.value();
                }
                if (ok) {
                    try {
                        word = InstructionWord::encodeI(Opcode::STORE,
                            static_cast<uint8_t>(src),
                            static_cast<uint8_t>(base), imm);
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
                        FUNC_T50);
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
                    auto v = resolveImm(ops[2], pc, line);
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
                        FUNC_T50);
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

// =============================================================================
// SECTION 7 — Public API: assemble()
// =============================================================================

// Assemble source text into a TritWord27 program image.
// Returns an AssemblyResult with success flag, program, errors, and label map.
[[nodiscard]] inline AssemblyResult assemble(const std::string& source) {
    AssemblyResult result;

    // Parse source lines.
    auto lines = parseSources(source, result.errors);
    if (!result.errors.empty()) return result;

    // Pass 1: collect labels.
    result.labels = buildLabelMap(lines, result.errors);
    if (!result.errors.empty()) return result;

    // Pass 2: encode.
    result.program = encode(lines, result.labels, result.errors);
    result.success = result.errors.empty();
    return result;
}

// Convenience: assemble and throw on any error.
[[nodiscard]] inline std::vector<TritWord27> assembleOrThrow(
        const std::string& source) {
    return assemble(source).require();
}

// =============================================================================
// SECTION 8 — Disassembler: program image → annotated source listing
// =============================================================================
// Useful for debugging: takes an assembled program and produces the
// mnemonic listing with addresses, matching the format the SystemVerilog
// testbench will consume for annotation.

[[nodiscard]] inline std::string listing(
        const std::vector<TritWord27>& program,
        const std::map<std::string, int>& labels = {}) {

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
            << disassemble(program[i]) << "\n";
    }
    return oss.str();
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
        ok &= (iw.opcode == Opcode::STORE && iw.rd == 1 &&
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
