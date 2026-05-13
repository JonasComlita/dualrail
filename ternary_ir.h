// =============================================================================
// ternary_ir.h - minimal typed IR lowering to existing ternary assembly
// =============================================================================
//
// Phase 5B compiler layer. This intentionally emits assembly text first and
// delegates binary instruction encoding to ternary_asm.h.

#pragma once
#ifndef TERNARY_IR_H
#define TERNARY_IR_H

#include "ternary_asm.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {
namespace ir {

enum class Type : uint8_t {
    T1,
    T5,
    T10,
    T20,
    T40,
    T50,
    L1,
    L5,
    L10,
    L20,
    L40,
    L50,
};

struct Value {
    Type type = Type::T40;
    int reg = -1;
    bool vector = false;

    [[nodiscard]] bool valid() const { return reg >= 0; }
};

struct LowerResult {
    bool success = false;
    std::string assembly;
    std::vector<std::string> diagnostics;
    vm::assembler::AssemblyResult assembled;
};

inline bool isNumeric(Type type) {
    switch (type) {
        case Type::T1:
        case Type::T5:
        case Type::T10:
        case Type::T20:
        case Type::T40:
        case Type::T50:
            return true;
        default:
            return false;
    }
}

inline bool isLane(Type type) {
    return !isNumeric(type);
}

inline const char* suffix(Type type) {
    switch (type) {
        case Type::T1: return "t1";
        case Type::T5: return "t5";
        case Type::T10: return "t10";
        case Type::T20: return "t20";
        case Type::T40: return "t40";
        case Type::T50: return "t50";
        case Type::L1: return "l1";
        case Type::L5: return "l5";
        case Type::L10: return "l10";
        case Type::L20: return "l20";
        case Type::L40: return "l40";
        case Type::L50: return "l50";
    }
    return "t40";
}

inline std::string regName(Value value) {
    return (value.vector ? "v" : "r") + std::to_string(value.reg);
}

class Program {
public:
    Program() {
        for (int r = 1; r <= 24; ++r) scalarFree_.push_back(r);
        for (int v = 0; v <= 7; ++v) vectorFree_.push_back(v);
    }

    [[nodiscard]] const std::vector<std::string>& diagnostics() const {
        return diagnostics_;
    }

    [[nodiscard]] std::string assembly() const {
        std::ostringstream out;
        for (const std::string& line : lines_) out << line << "\n";
        return out.str();
    }

    [[nodiscard]] LowerResult lower() const {
        LowerResult result;
        result.assembly = assembly();
        result.diagnostics = diagnostics_;
        if (!result.diagnostics.empty()) return result;

        result.assembled = vm::assembler::assemble(result.assembly);
        result.success = result.assembled.success;
        if (!result.assembled.success) {
            for (const auto& error : result.assembled.errors) {
                result.diagnostics.push_back(error.format());
            }
        }
        return result;
    }

    [[nodiscard]] Value zero(Type type = Type::T40) const {
        return Value{type, 0, false};
    }

    [[nodiscard]] Value param(Type type) {
        return allocScalar(type, "scalar parameter");
    }

    [[nodiscard]] Value vparam(Type type) {
        return allocVector(type, "vector parameter");
    }

    [[nodiscard]] Value constant(Type type, long long imm) {
        Value out = allocScalar(type, "constant");
        if (!out.valid()) return out;
        emit(std::string("mov.") + suffix(type) + " " + regName(out) + ", " + std::to_string(imm));
        return out;
    }

    [[nodiscard]] Value copy(Value src) {
        if (!checkScalar(src, "copy source")) return invalid(src.type);
        Value out = allocScalar(src.type, "copy destination");
        if (!out.valid()) return out;
        emit("copy " + regName(out) + ", " + regName(src));
        return out;
    }

    [[nodiscard]] Value load(Type type, Value base, int imm = 0) {
        if (!checkScalar(base, "load base") || !isNumeric(base.type)) return invalid(type);
        Value out = allocScalar(type, "load destination");
        if (!out.valid()) return out;
        emit("load " + regName(out) + ", " + regName(base) + ", " + std::to_string(imm));
        return out;
    }

    void store(Value src, Value base, int imm = 0) {
        if (!checkScalar(src, "store source") || !checkScalar(base, "store base")) return;
        if (!isNumeric(base.type)) {
            diag("store base must be numeric");
            return;
        }
        emit("store " + regName(src) + ", " + regName(base) + ", " + std::to_string(imm));
    }

    [[nodiscard]] Value add(Value a, Value b) { return numericBinary("add", a, b, a.type); }
    [[nodiscard]] Value sub(Value a, Value b) { return numericBinary("sub", a, b, a.type); }
    [[nodiscard]] Value mul(Value a, Value b) { return numericBinary("mul", a, b, a.type); }
    [[nodiscard]] Value div(Value a, Value b) { return numericBinary("div", a, b, a.type); }
    [[nodiscard]] Value min(Value a, Value b) { return numericBinary("tmin", a, b, a.type); }
    [[nodiscard]] Value max(Value a, Value b) { return numericBinary("tmax", a, b, a.type); }

    [[nodiscard]] Value cmp(Value a, Value b) {
        return numericBinary("tcmp", a, b, Type::T1);
    }

    [[nodiscard]] Value sqrt(Value src) { return numericUnary("sqrt", src, src.type); }
    [[nodiscard]] Value neg(Value src) { return numericUnary("neg", src, src.type); }
    [[nodiscard]] Value abs(Value src) { return numericUnary("abs", src, src.type); }

    [[nodiscard]] Value inv(Value src) {
        if (isNumeric(src.type)) return numericUnary("tinv", src, src.type);
        return laneUnary("tlneg", src);
    }

    [[nodiscard]] Value tladd(Value a, Value b) { return laneBinary("tladd", a, b); }
    [[nodiscard]] Value tlsub(Value a, Value b) { return laneBinary("tlsub", a, b); }
    [[nodiscard]] Value tland(Value a, Value b) { return laneBinary("tland", a, b); }
    [[nodiscard]] Value tlor(Value a, Value b) { return laneBinary("tlor", a, b); }

    [[nodiscard]] Value cvt(Value src, Type target) {
        if (!checkScalar(src, "cvt source")) return invalid(target);
        if (src.type == target) return src;
        Value out = allocScalar(target, "cvt destination");
        if (!out.valid()) return out;
        emit(std::string("cvt.") + suffix(src.type) + "." + suffix(target) + " " +
             regName(out) + ", " + regName(src));
        return out;
    }

    [[nodiscard]] Value tsel(Value cond, Value negArm, Value zeroArm, Value posArm) {
        if (!checkScalar(cond, "tsel condition") ||
            !checkScalar(negArm, "tsel negative arm") ||
            !checkScalar(zeroArm, "tsel zero arm") ||
            !checkScalar(posArm, "tsel positive arm")) {
            return invalid(negArm.type);
        }
        if (cond.type != Type::T1 ||
            negArm.type != zeroArm.type ||
            negArm.type != posArm.type) {
            diag("tsel requires T1 condition and matching arm types");
            return invalid(negArm.type);
        }
        Value out = allocScalar(negArm.type, "tsel destination");
        if (!out.valid()) return out;
        emit("tsel " + regName(out) + ", " + regName(cond) + ", " +
             regName(negArm) + ", " + regName(zeroArm) + ", " + regName(posArm));
        return out;
    }

    void swap(Value a, Value b) {
        if (!checkScalar(a, "swap first") || !checkScalar(b, "swap second")) return;
        emit("swap " + regName(a) + ", " + regName(b));
    }

    void label(const std::string& name) {
        lines_.push_back(name + ":");
    }

    void brn(Value cond, const std::string& target) { branch("brn", cond, target); }
    void brz(Value cond, const std::string& target) { branch("brz", cond, target); }
    void brp(Value cond, const std::string& target) { branch("brp", cond, target); }

    void jmp(const std::string& target) {
        emit("jmp " + target);
    }

    [[nodiscard]] Value vlen() {
        Value out = allocScalar(Type::T40, "vlen destination");
        if (!out.valid()) return out;
        emit("vlen " + regName(out));
        return out;
    }

    [[nodiscard]] Value vbcast(Type type, Value scalar) {
        if (!checkScalar(scalar, "vbcast source")) return invalidVector(type);
        Value out = allocVector(type, "vbcast destination");
        if (!out.valid()) return out;
        emit(std::string("vbcast.") + suffix(type) + " " + regName(out) + ", " + regName(scalar));
        return out;
    }

    [[nodiscard]] Value vload(Type type, Value base, int imm = 0) {
        if (!checkScalar(base, "vload base") || !isNumeric(base.type)) return invalidVector(type);
        Value out = allocVector(type, "vload destination");
        if (!out.valid()) return out;
        emit(std::string("vload.") + suffix(type) + " " + regName(out) + ", " +
             regName(base) + ", " + std::to_string(imm));
        return out;
    }

    void vstore(Value vector, Value base, int imm = 0) {
        if (!checkVector(vector, "vstore source") || !checkScalar(base, "vstore base")) return;
        emit(std::string("vstore.") + suffix(vector.type) + " " + regName(vector) + ", " +
             regName(base) + ", " + std::to_string(imm));
    }

    [[nodiscard]] Value vadd(Value a, Value b) { return vectorBinary("vadd", a, b, a.type); }
    [[nodiscard]] Value vsub(Value a, Value b) { return vectorBinary("vsub", a, b, a.type); }
    [[nodiscard]] Value vmul(Value a, Value b) { return vectorBinary("vmul", a, b, a.type); }
    [[nodiscard]] Value vdiv(Value a, Value b) { return vectorBinary("vdiv", a, b, a.type); }
    [[nodiscard]] Value vneg(Value src) { return vectorUnary("vneg", src, src.type); }

    [[nodiscard]] Value vcmp(Value a, Value b) {
        return vectorBinary("vcmp", a, b, Type::L1);
    }

    [[nodiscard]] Value vsel(Value cond, Value negArm, Value zeroArm, Value posArm) {
        if (!checkVector(cond, "vsel condition") ||
            !checkVector(negArm, "vsel negative arm") ||
            !checkVector(zeroArm, "vsel zero arm") ||
            !checkVector(posArm, "vsel positive arm")) {
            return invalidVector(negArm.type);
        }
        if (cond.type != Type::L1 ||
            negArm.type != zeroArm.type ||
            negArm.type != posArm.type) {
            diag("vsel requires L1 condition and matching arm types");
            return invalidVector(negArm.type);
        }
        Value out = allocVector(negArm.type, "vsel destination");
        if (!out.valid()) return out;
        emit(std::string("vsel.") + suffix(negArm.type) + " " + regName(out) + ", " +
             regName(cond) + ", " + regName(negArm) + ", " +
             regName(zeroArm) + ", " + regName(posArm));
        return out;
    }

    [[nodiscard]] Value vpack(Value src, Type target) {
        return vectorConvert("vpack", src, target);
    }

    [[nodiscard]] Value vunpack(Value src, Type target) {
        return vectorConvert("vunpack", src, target);
    }

    [[nodiscard]] Value vpermute(Value src, Value index) {
        if (!checkVector(src, "vpermute source") || !checkVector(index, "vpermute index")) {
            return invalidVector(src.type);
        }
        Value out = allocVector(src.type, "vpermute destination");
        if (!out.valid()) return out;
        emit(std::string("vpermute.") + suffix(src.type) + " " + regName(out) + ", " +
             regName(src) + ", " + regName(index));
        return out;
    }

    [[nodiscard]] Value vblend(Value cond, Value falseArm, Value trueArm) {
        return vsel(cond, falseArm, falseArm, trueArm);
    }

    void vswap(Value a, Value b) {
        if (!checkVector(a, "vswap first") || !checkVector(b, "vswap second")) return;
        emit("vswap " + regName(a) + ", " + regName(b));
    }

    [[nodiscard]] Value vgather(Type type, Value base, Value index) {
        if (!checkScalar(base, "vgather base") || !checkVector(index, "vgather index")) {
            return invalidVector(type);
        }
        Value out = allocVector(type, "vgather destination");
        if (!out.valid()) return out;
        emit(std::string("vgather.") + suffix(type) + " " + regName(out) + ", " +
             regName(base) + ", " + regName(index));
        return out;
    }

    void vscatter(Value vector, Value base, Value index) {
        if (!checkVector(vector, "vscatter source") ||
            !checkScalar(base, "vscatter base") ||
            !checkVector(index, "vscatter index")) {
            return;
        }
        emit(std::string("vscatter.") + suffix(vector.type) + " " + regName(vector) + ", " +
             regName(base) + ", " + regName(index));
    }

    void aclr(Type type = Type::T40) {
        emit(std::string("aclr.") + suffix(type));
    }

    void aload(Value src) { accumulatorUnary("aload", src); }
    void aadd(Value src) { accumulatorUnary("aadd", src); }
    void asub(Value src) { accumulatorUnary("asub", src); }
    void amul(Value src) { accumulatorUnary("amul", src); }

    [[nodiscard]] Value astore(Type type = Type::T40) {
        Value out = allocScalar(type, "astore destination");
        if (!out.valid()) return out;
        emit(std::string("astore.") + suffix(type) + " " + regName(out));
        return out;
    }

    [[nodiscard]] Value vdotT1(Value a, Value b) {
        if (!checkVector(a, "vdot first") || !checkVector(b, "vdot second")) return invalid(Type::T40);
        if (a.type != Type::L1 || b.type != Type::L1) {
            diag("vdotT1 requires L1 vectors");
            return invalid(Type::T40);
        }
        Value out = allocScalar(Type::T40, "vdot destination");
        if (!out.valid()) return out;
        emit("vdot.t1 " + regName(out) + ", " + regName(a) + ", " + regName(b));
        return out;
    }

    void vmacT1(Value a, Value b) {
        if (!checkVector(a, "vmac first") || !checkVector(b, "vmac second")) return;
        if (a.type != Type::L1 || b.type != Type::L1) {
            diag("vmacT1 requires L1 vectors");
            return;
        }
        emit("vmac.t1 " + regName(a) + ", " + regName(b));
    }

    [[nodiscard]] Value vactT1(Value src) {
        if (!checkVector(src, "vact source")) return invalidVector(Type::L1);
        Value out = allocVector(Type::L1, "vact destination");
        if (!out.valid()) return out;
        emit("vact.t1 " + regName(out) + ", " + regName(src));
        return out;
    }

    void nop() { emit("nop"); }
    void halt() { emit("halt"); }

    void release(Value value) {
        if (!value.valid() || value.reg == 0 || value.reg >= 25) return;
        std::vector<int>& freeList = value.vector ? vectorFree_ : scalarFree_;
        if (std::find(freeList.begin(), freeList.end(), value.reg) == freeList.end()) {
            freeList.push_back(value.reg);
            std::sort(freeList.begin(), freeList.end());
        }
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> diagnostics_;
    std::vector<int> scalarFree_;
    std::vector<int> vectorFree_;

    [[nodiscard]] Value invalid(Type type) const { return Value{type, -1, false}; }
    [[nodiscard]] Value invalidVector(Type type) const { return Value{type, -1, true}; }

    void diag(const std::string& message) {
        diagnostics_.push_back(message);
    }

    void emit(const std::string& line) {
        lines_.push_back("    " + line);
    }

    [[nodiscard]] Value allocScalar(Type type, const std::string& purpose) {
        if (scalarFree_.empty()) {
            diag("scalar register exhausted while allocating " + purpose);
            return invalid(type);
        }
        const int reg = scalarFree_.front();
        scalarFree_.erase(scalarFree_.begin());
        return Value{type, reg, false};
    }

    [[nodiscard]] Value allocVector(Type type, const std::string& purpose) {
        if (vectorFree_.empty()) {
            diag("vector register exhausted while allocating " + purpose);
            return invalidVector(type);
        }
        const int reg = vectorFree_.front();
        vectorFree_.erase(vectorFree_.begin());
        return Value{type, reg, true};
    }

    bool checkScalar(Value value, const std::string& role) {
        if (!value.valid() || value.vector) {
            diag(role + " must be a scalar value");
            return false;
        }
        return true;
    }

    bool checkVector(Value value, const std::string& role) {
        if (!value.valid() || !value.vector) {
            diag(role + " must be a vector value");
            return false;
        }
        return true;
    }

    [[nodiscard]] Value numericBinary(const std::string& mnemonic, Value a, Value b, Type resultType) {
        if (!checkScalar(a, mnemonic + " first operand") ||
            !checkScalar(b, mnemonic + " second operand")) {
            return invalid(resultType);
        }
        if (!isNumeric(a.type) || !isNumeric(b.type) || a.type != b.type) {
            diag(mnemonic + " requires matching numeric scalar operands");
            return invalid(resultType);
        }
        Value out = allocScalar(resultType, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(a.type) + " " + regName(out) + ", " +
             regName(a) + ", " + regName(b));
        return out;
    }

    [[nodiscard]] Value numericUnary(const std::string& mnemonic, Value src, Type resultType) {
        if (!checkScalar(src, mnemonic + " source")) return invalid(resultType);
        if (!isNumeric(src.type)) {
            diag(mnemonic + " requires a numeric scalar operand");
            return invalid(resultType);
        }
        Value out = allocScalar(resultType, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(src.type) + " " + regName(out) + ", " + regName(src));
        return out;
    }

    [[nodiscard]] Value laneBinary(const std::string& mnemonic, Value a, Value b) {
        if (!checkScalar(a, mnemonic + " first operand") ||
            !checkScalar(b, mnemonic + " second operand")) {
            return invalid(a.type);
        }
        if (!isLane(a.type) || a.type != b.type) {
            diag(mnemonic + " requires matching lane scalar operands");
            return invalid(a.type);
        }
        Value out = allocScalar(a.type, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(a.type) + " " + regName(out) + ", " +
             regName(a) + ", " + regName(b));
        return out;
    }

    [[nodiscard]] Value laneUnary(const std::string& mnemonic, Value src) {
        if (!checkScalar(src, mnemonic + " source")) return invalid(src.type);
        if (!isLane(src.type)) {
            diag(mnemonic + " requires a lane scalar operand");
            return invalid(src.type);
        }
        Value out = allocScalar(src.type, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(src.type) + " " + regName(out) + ", " + regName(src));
        return out;
    }

    void branch(const std::string& mnemonic, Value cond, const std::string& target) {
        if (!checkScalar(cond, mnemonic + " condition")) return;
        if (cond.type != Type::T1) {
            diag(mnemonic + " requires a T1 condition");
            return;
        }
        emit(mnemonic + " " + regName(cond) + ", " + target);
    }

    [[nodiscard]] Value vectorBinary(const std::string& mnemonic, Value a, Value b, Type resultType) {
        if (!checkVector(a, mnemonic + " first operand") ||
            !checkVector(b, mnemonic + " second operand")) {
            return invalidVector(resultType);
        }
        if (!isNumeric(a.type) || !isNumeric(b.type) || a.type != b.type) {
            diag(mnemonic + " requires matching numeric vector operands");
            return invalidVector(resultType);
        }
        Value out = allocVector(resultType, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(a.type) + " " + regName(out) + ", " +
             regName(a) + ", " + regName(b));
        return out;
    }

    [[nodiscard]] Value vectorUnary(const std::string& mnemonic, Value src, Type resultType) {
        if (!checkVector(src, mnemonic + " source")) return invalidVector(resultType);
        if (!isNumeric(src.type)) {
            diag(mnemonic + " requires a numeric vector operand");
            return invalidVector(resultType);
        }
        Value out = allocVector(resultType, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(src.type) + " " + regName(out) + ", " + regName(src));
        return out;
    }

    [[nodiscard]] Value vectorConvert(const std::string& mnemonic, Value src, Type target) {
        if (!checkVector(src, mnemonic + " source")) return invalidVector(target);
        if (src.type == target) return src;
        Value out = allocVector(target, mnemonic + " destination");
        if (!out.valid()) return out;
        emit(mnemonic + "." + suffix(src.type) + "." + suffix(target) + " " +
             regName(out) + ", " + regName(src));
        return out;
    }

    void accumulatorUnary(const std::string& mnemonic, Value src) {
        if (!checkScalar(src, mnemonic + " source")) return;
        if (!isNumeric(src.type)) {
            diag(mnemonic + " requires a numeric scalar operand");
            return;
        }
        emit(mnemonic + "." + suffix(src.type) + " " + regName(src));
    }
};

} // namespace ir
} // namespace sandbox

#endif // TERNARY_IR_H
