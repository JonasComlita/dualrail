// ternary_compiler_types.h - Type model, diagnostics, options, and unification

#pragma once
#ifndef TERNARY_COMPILER_TYPES_H
#define TERNARY_COMPILER_TYPES_H

#include "ternary_ir.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sandbox {
namespace compiler {

// =============================================================================
// Public diagnostics and options
// =============================================================================

struct SourceSpan {
    std::string file;
    int line = 1;
    int column = 1;
    int length = 1;
};

enum class DiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error,
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string message;
    SourceSpan span;

    [[nodiscard]] std::string format() const {
        std::ostringstream out;
        out << span.file << ":" << span.line << ":" << span.column << ": ";
        switch (severity) {
            case DiagnosticSeverity::Info: out << "info"; break;
            case DiagnosticSeverity::Warning: out << "warning"; break;
            case DiagnosticSeverity::Error: out << "error"; break;
        }
        out << ": " << message;
        return out.str();
    }
};

enum class OptimizationLevel : uint8_t {
    None,
    Basic,
    Aggressive,
};

enum class DiagnosticsMode : uint8_t {
    Human,
    Machine,
};

struct CompilerOptions {
    int target_abi_version = architecture::v2::FUNCTION_ABI_VERSION;
    int syscall_abi_version = architecture::v2::SYSCALL_ABI_VERSION;
    OptimizationLevel optimization = OptimizationLevel::Basic;
    DiagnosticsMode diagnostics_mode = DiagnosticsMode::Human;
    bool emit_debug_metadata = false;
    bool emit_profiling_metadata = true;
    bool standalone_halt_on_exit = true;
    bool enable_mem2reg = true;
    bool enable_cse = true;
    bool enable_spilling = true;
    // Keep legacy AST emission available for the transition build, but allow
    // CI and release tooling to require optimized SSA as the sole target
    // source while unsupported constructs are being eliminated.
    bool allow_ast_replay = true;
    bool debug_bounds_checks = false;
    bool dump_pass_pipeline = false;
};

// =============================================================================
// Type model
// =============================================================================

enum class TypeKind : uint8_t {
    Unknown,
    Void,
    Numeric,
    Lane,
    Vector,
    Pointer,
    Shared,
    Struct,
    Array,
    Function,
    TypeVar,
    Trit,
    Owned,
    Borrow,
    BorrowMut,
};

enum class PointerRegion : uint8_t {
    Stack,
    Static,
    User,
    Kernel,
};

enum class PointerState : uint8_t {
    Valid,
    Unknown,
    Null,
};

enum class MemoryOrder : int8_t {
    Relaxed = isa::ATOMIC_ORDER_RELAXED,
    AcquireRelease = isa::ATOMIC_ORDER_ACQ_REL,
    Sequential = isa::ATOMIC_ORDER_SEQ_CST,
};

using TypeVarId = int;

struct TypeRef {
    TypeKind kind = TypeKind::Unknown;
    ir::Type scalar = ir::Type::T40;
    std::shared_ptr<TypeRef> element;
    PointerRegion region = PointerRegion::Stack;
    PointerState state = PointerState::Valid;
    MemoryOrder order = MemoryOrder::AcquireRelease;
    std::string name;
    int array_len = 0;
    std::vector<TypeRef> params;
    std::shared_ptr<TypeRef> result;
    TypeVarId type_var = 0;
    std::string width_var;
    std::string owner_var;

    [[nodiscard]] static TypeRef unknown() { return TypeRef{}; }
    [[nodiscard]] static TypeRef voidType() {
        TypeRef out;
        out.kind = TypeKind::Void;
        return out;
    }
    [[nodiscard]] static TypeRef numeric(ir::Type type) {
        TypeRef out;
        out.kind = TypeKind::Numeric;
        out.scalar = type;
        return out;
    }
    [[nodiscard]] static TypeRef lane(ir::Type type) {
        TypeRef out;
        out.kind = TypeKind::Lane;
        out.scalar = type;
        return out;
    }
    [[nodiscard]] static TypeRef vector(TypeRef elem) {
        TypeRef out;
        out.kind = TypeKind::Vector;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        return out;
    }
    [[nodiscard]] static TypeRef pointer(
        TypeRef elem,
        PointerRegion region = PointerRegion::Stack,
        PointerState state = PointerState::Valid) {
        TypeRef out;
        out.kind = TypeKind::Pointer;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        out.region = region;
        out.state = state;
        return out;
    }
    [[nodiscard]] static TypeRef shared(TypeRef elem, MemoryOrder order) {
        TypeRef out;
        out.kind = TypeKind::Shared;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        out.order = order;
        return out;
    }
    [[nodiscard]] static TypeRef typeVar(TypeVarId id) {
        TypeRef out;
        out.kind = TypeKind::TypeVar;
        out.type_var = id;
        return out;
    }
    [[nodiscard]] static TypeRef trit() {
        TypeRef out;
        out.kind = TypeKind::Trit;
        out.scalar = ir::Type::T1;
        return out;
    }
    [[nodiscard]] static TypeRef owned(TypeRef elem) {
        TypeRef out;
        out.kind = TypeKind::Owned;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        return out;
    }
    [[nodiscard]] static TypeRef borrow(TypeRef elem) {
        TypeRef out;
        out.kind = TypeKind::Borrow;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        return out;
    }
    [[nodiscard]] static TypeRef borrowMut(TypeRef elem) {
        TypeRef out;
        out.kind = TypeKind::BorrowMut;
        out.element = std::make_shared<TypeRef>(std::move(elem));
        return out;
    }

    [[nodiscard]] bool isNumericScalar() const {
        return kind == TypeKind::Numeric;
    }
    [[nodiscard]] bool isVoid() const {
        return kind == TypeKind::Void;
    }
    [[nodiscard]] std::string str() const {
        if (!width_var.empty()) {
            return "T<" + width_var + ">";
        }
        switch (kind) {
            case TypeKind::Trit: return "trit";
            case TypeKind::Unknown: return "unknown";
            case TypeKind::Void: return "void";
            case TypeKind::Numeric: return ir::suffix(scalar);
            case TypeKind::Lane: return ir::suffix(scalar);
            case TypeKind::Vector:
                return "vec<" + (element ? element->str() : std::string("unknown")) + ">";
            case TypeKind::Pointer: {
                std::string r = region == PointerRegion::Stack ? "stack" :
                                region == PointerRegion::Static ? "static" :
                                region == PointerRegion::User ? "user" : "kernel";
                std::string s = state == PointerState::Valid ? "valid" :
                                state == PointerState::Unknown ? "unknown" : "null";
                return "ptr<" + (element ? element->str() : std::string("unknown")) +
                       ", " + r + ", " + s + ">";
            }
            case TypeKind::Shared: {
                int order_value = static_cast<int>(order);
                return "shared<" + (element ? element->str() : std::string("unknown")) +
                       ", " + std::to_string(order_value) + ">";
            }
            case TypeKind::Struct: return name;
            case TypeKind::Array:
                return "[" + (element ? element->str() : std::string("unknown")) +
                       "; " + std::to_string(array_len) + "]";
            case TypeKind::Function: {
                std::string out = "fn(";
                for (std::size_t i = 0; i < params.size(); ++i) {
                    if (i != 0) out += ", ";
                    out += params[i].str();
                }
                out += ") -> ";
                out += result ? result->str() : std::string("unknown");
                return out;
            }
            case TypeKind::TypeVar: return "'" + std::to_string(type_var);
            case TypeKind::Owned:
                return "own<" + (element ? element->str() : std::string("unknown")) + ">";
            case TypeKind::Borrow:
                return "borrow<" + (element ? element->str() : std::string("unknown")) + ">";
            case TypeKind::BorrowMut:
                return "borrow_mut<" + (element ? element->str() : std::string("unknown")) + ">";
        }
        return "unknown";
    }
};

[[nodiscard]] inline bool sameType(const TypeRef& a, const TypeRef& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == TypeKind::TypeVar) return a.type_var == b.type_var;
    if (a.scalar != b.scalar) return false;
    if (a.region != b.region || a.state != b.state || a.order != b.order) return false;
    if (a.name != b.name || a.array_len != b.array_len) return false;
    if (a.width_var != b.width_var || a.owner_var != b.owner_var) return false;
    if (static_cast<bool>(a.element) != static_cast<bool>(b.element)) return false;
    if (a.element && !sameType(*a.element, *b.element)) return false;
    if (a.params.size() != b.params.size()) return false;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        if (!sameType(a.params[i], b.params[i])) return false;
    }
    if (static_cast<bool>(a.result) != static_cast<bool>(b.result)) return false;
    if (a.result && !sameType(*a.result, *b.result)) return false;
    return true;
}

[[nodiscard]] inline bool isNumericLike(const TypeRef& type) {
    return type.kind == TypeKind::Numeric || type.kind == TypeKind::Trit;
}

[[nodiscard]] inline bool isOwnershipWrapper(const TypeRef& type) {
    return type.kind == TypeKind::Owned ||
           type.kind == TypeKind::Borrow ||
           type.kind == TypeKind::BorrowMut;
}

[[nodiscard]] inline const TypeRef* pointerTypeView(const TypeRef& type) {
    if (type.kind == TypeKind::Pointer) return &type;
    if (isOwnershipWrapper(type) && type.element &&
        type.element->kind == TypeKind::Pointer) {
        return type.element.get();
    }
    return nullptr;
}

[[nodiscard]] inline bool pointerStatesCompatible(PointerState from, PointerState to) {
    return from == to || to == PointerState::Unknown;
}

[[nodiscard]] inline bool pointerViewsCompatible(const TypeRef& from, const TypeRef& to) {
    const TypeRef* fromPtr = pointerTypeView(from);
    const TypeRef* toPtr = pointerTypeView(to);
    if (!fromPtr || !toPtr || !fromPtr->element || !toPtr->element) return false;
    return fromPtr->region == toPtr->region &&
           pointerStatesCompatible(fromPtr->state, toPtr->state) &&
           sameType(*fromPtr->element, *toPtr->element);
}

[[nodiscard]] inline bool isComparison(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

[[nodiscard]] inline int numericRank(const TypeRef& type) {
    if (type.kind == TypeKind::Trit) return 0;
    if (type.kind == TypeKind::Numeric) {
        switch (type.scalar) {
            case ir::Type::T1: return 0;
            case ir::Type::T5: return 1;
            case ir::Type::T10: return 2;
            case ir::Type::T20: return 3;
            case ir::Type::T40: return 4;
            case ir::Type::T50: return 5;
            default: return -1;
        }
    }
    return -1;
}

[[nodiscard]] inline ir::Type widthAtRank(int rank) {
    switch (rank) {
        case 0: return ir::Type::T1;
        case 1: return ir::Type::T5;
        case 2: return ir::Type::T10;
        case 3: return ir::Type::T20;
        case 4: return ir::Type::T40;
        case 5: return ir::Type::T50;
        default: return ir::Type::T40;
    }
}

[[nodiscard]] inline bool canWiden(const TypeRef& from, const TypeRef& to) {
    if (from.kind == TypeKind::Unknown || to.kind == TypeKind::Unknown) return true;
    if (from.kind == TypeKind::Shared && to.kind == TypeKind::Shared) {
        if (from.element && to.element) {
            return canWiden(*from.element, *to.element);
        }
        return false;
    }
    const TypeRef* fromPtr = pointerTypeView(from);
    const TypeRef* toPtr = pointerTypeView(to);
    bool isFromNum = isNumericLike(from);
    bool isToNum = isNumericLike(to);
    if (fromPtr || toPtr) {
        if ((isFromNum && toPtr) || (fromPtr && isToNum)) return true;
        if (fromPtr && toPtr) return pointerViewsCompatible(from, to);
        return false;
    }
    if (!isFromNum || !isToNum) return sameType(from, to);
    const int a = numericRank(from);
    const int b = numericRank(to);
    return a >= 0 && b >= 0 && a <= b;
}

[[nodiscard]] inline TypeRef commonNumericType(const TypeRef& a, const TypeRef& b) {
    if (a.kind == TypeKind::Unknown && (b.kind == TypeKind::Numeric || b.kind == TypeKind::Trit)) return b;
    if (b.kind == TypeKind::Unknown && (a.kind == TypeKind::Numeric || a.kind == TypeKind::Trit)) return a;
    bool isANum = (a.kind == TypeKind::Numeric || a.kind == TypeKind::Trit);
    bool isBNum = (b.kind == TypeKind::Numeric || b.kind == TypeKind::Trit);
    if (isANum && isBNum) {
        int rA = numericRank(a);
        int rB = numericRank(b);
        int maxR = std::max(rA, rB);
        if (maxR == 0) return TypeRef::trit();
        return TypeRef::numeric(widthAtRank(maxR));
    }
    return TypeRef::numeric(ir::Type::T40);
}

struct Scheme {
    std::vector<TypeVarId> quantified;
    TypeRef type = TypeRef::unknown();
    bool generalized = false;
};

struct Constraint {
    TypeRef left = TypeRef::unknown();
    TypeRef right = TypeRef::unknown();
    SourceSpan span;
    std::string reason;
};

struct TypeEnv {
    std::map<std::string, Scheme> values;
};

[[nodiscard]] inline std::set<TypeVarId> freeTypeVars(const TypeRef& type) {
    std::set<TypeVarId> vars;
    if (type.kind == TypeKind::TypeVar) vars.insert(type.type_var);
    if (type.element) {
        auto nested = freeTypeVars(*type.element);
        vars.insert(nested.begin(), nested.end());
    }
    for (const auto& param : type.params) {
        auto nested = freeTypeVars(param);
        vars.insert(nested.begin(), nested.end());
    }
    if (type.result) {
        auto nested = freeTypeVars(*type.result);
        vars.insert(nested.begin(), nested.end());
    }
    return vars;
}

[[nodiscard]] inline std::set<TypeVarId> freeSchemeVars(const Scheme& scheme) {
    std::set<TypeVarId> vars = freeTypeVars(scheme.type);
    for (TypeVarId id : scheme.quantified) vars.erase(id);
    return vars;
}

[[nodiscard]] inline std::set<TypeVarId> freeEnvVars(const TypeEnv& env) {
    std::set<TypeVarId> vars;
    for (const auto& entry : env.values) {
        auto nested = freeSchemeVars(entry.second);
        vars.insert(nested.begin(), nested.end());
    }
    return vars;
}

struct Substitution {
    std::map<TypeVarId, TypeRef> bindings;
    std::map<std::string, TypeRef> width_bindings;
    TypeRef last_numeric_widening = TypeRef::unknown();

    [[nodiscard]] TypeRef apply(const TypeRef& type) const {
        if (!type.width_var.empty()) {
            auto it = width_bindings.find(type.width_var);
            if (it != width_bindings.end()) {
                return apply(it->second);
            }
        }
        if (type.kind == TypeKind::TypeVar) {
            auto it = bindings.find(type.type_var);
            if (it == bindings.end()) return type;
            return apply(it->second);
        }
        TypeRef out = type;
        if (type.element) out.element = std::make_shared<TypeRef>(apply(*type.element));
        out.params.clear();
        for (const auto& param : type.params) out.params.push_back(apply(param));
        if (type.result) out.result = std::make_shared<TypeRef>(apply(*type.result));
        return out;
    }

    [[nodiscard]] Scheme apply(const Scheme& scheme) const {
        Substitution scoped = *this;
        for (TypeVarId id : scheme.quantified) scoped.bindings.erase(id);
        Scheme out = scheme;
        out.type = scoped.apply(scheme.type);
        return out;
    }
};

[[nodiscard]] inline bool occursIn(TypeVarId id, const TypeRef& type, const Substitution& subst) {
    TypeRef applied = subst.apply(type);
    if (applied.kind == TypeKind::TypeVar) return applied.type_var == id;
    if (applied.element && occursIn(id, *applied.element, subst)) return true;
    for (const auto& param : applied.params) {
        if (occursIn(id, param, subst)) return true;
    }
    return applied.result && occursIn(id, *applied.result, subst);
}

inline bool unify(TypeRef left,
                  TypeRef right,
                  Substitution& subst,
                  std::vector<Diagnostic>& diagnostics,
                  const SourceSpan& span,
                  const std::string& reason = "type mismatch") {
    left = subst.apply(left);
    right = subst.apply(right);
    if (sameType(left, right)) return true;
    if (left.kind == TypeKind::Unknown || right.kind == TypeKind::Unknown) return true;
    if (!left.width_var.empty()) {
        subst.width_bindings[left.width_var] = right;
        return true;
    }
    if (!right.width_var.empty()) {
        subst.width_bindings[right.width_var] = left;
        return true;
    }
    if (left.kind == TypeKind::TypeVar) {
        if (occursIn(left.type_var, right, subst)) {
            diagnostics.push_back({DiagnosticSeverity::Error,
                "occurs-check failed while unifying " + left.str() + " with " + right.str(),
                span});
            return false;
        }
        subst.bindings[left.type_var] = right;
        return true;
    }
    if (right.kind == TypeKind::TypeVar) {
        return unify(right, left, subst, diagnostics, span, reason);
    }
    const TypeRef* leftPtr = pointerTypeView(left);
    const TypeRef* rightPtr = pointerTypeView(right);
    const bool leftNum = isNumericLike(left);
    const bool rightNum = isNumericLike(right);
    if ((leftNum && rightPtr) || (leftPtr && rightNum)) {
        return true;
    }
    if (leftPtr && rightPtr) {
        if (leftPtr->region != rightPtr->region ||
            !pointerStatesCompatible(leftPtr->state, rightPtr->state)) {
            diagnostics.push_back({DiagnosticSeverity::Error,
                reason + ": expected " + right.str() + ", found " + left.str(), span});
            return false;
        }
        return unify(*leftPtr->element, *rightPtr->element, subst, diagnostics, span, reason);
    }
    if (left.kind == TypeKind::Numeric && right.kind == TypeKind::Numeric) {
        subst.last_numeric_widening = commonNumericType(left, right);
        return true;
    }
    if (left.kind == right.kind && left.kind == TypeKind::Pointer &&
        left.element && right.element &&
        left.region == right.region && left.state == right.state) {
        return unify(*left.element, *right.element, subst, diagnostics, span, reason);
    }
    if (((left.kind == TypeKind::Borrow && right.kind == TypeKind::Pointer) ||
         (left.kind == TypeKind::Pointer && right.kind == TypeKind::Borrow)) &&
        left.element && right.element) {
        return unify(*left.element, *right.element, subst, diagnostics, span, reason);
    }
    if (left.kind == right.kind && (left.kind == TypeKind::Owned || left.kind == TypeKind::Borrow || left.kind == TypeKind::BorrowMut) &&
        left.element && right.element) {
        return unify(*left.element, *right.element, subst, diagnostics, span, reason);
    }
    if (left.kind == right.kind && left.kind == TypeKind::Shared &&
        left.element && right.element) {
        return unify(*left.element, *right.element, subst, diagnostics, span, reason);
    }
    if (left.kind == right.kind && left.kind == TypeKind::Array &&
        left.array_len == right.array_len && left.element && right.element) {
        return unify(*left.element, *right.element, subst, diagnostics, span, reason);
    }
    if (left.kind == right.kind && left.kind == TypeKind::Function &&
        left.params.size() == right.params.size() && left.result && right.result) {
        bool ok = true;
        for (std::size_t i = 0; i < left.params.size(); ++i) {
            ok = unify(left.params[i], right.params[i], subst, diagnostics, span, reason) && ok;
        }
        return unify(*left.result, *right.result, subst, diagnostics, span, reason) && ok;
    }
    diagnostics.push_back({DiagnosticSeverity::Error,
        reason + ": expected " + right.str() + ", found " + left.str(), span});
    return false;
}

[[nodiscard]] inline Scheme generalize(const TypeEnv& env, const TypeRef& type, bool value_restricted) {
    Scheme scheme;
    scheme.type = type;
    if (value_restricted) return scheme;
    std::set<TypeVarId> vars = freeTypeVars(type);
    std::set<TypeVarId> envVars = freeEnvVars(env);
    for (TypeVarId id : envVars) vars.erase(id);
    scheme.quantified.assign(vars.begin(), vars.end());
    scheme.generalized = !scheme.quantified.empty();
    return scheme;
}

[[nodiscard]] inline TypeRef instantiate(const Scheme& scheme, TypeVarId& next_type_var) {
    std::map<TypeVarId, TypeRef> replacements;
    for (TypeVarId id : scheme.quantified) {
        replacements[id] = TypeRef::typeVar(next_type_var++);
    }
    Substitution subst;
    subst.bindings = std::move(replacements);
    return subst.apply(scheme.type);
}

struct FieldLayout {
    std::string name;
    int offset_words = 0;
    TypeRef type = TypeRef::unknown();
};

struct LayoutInfo {
    int size_words = 1;
    int align_words = 1;
    std::vector<FieldLayout> fields;
};

using LayoutTable = std::map<std::string, LayoutInfo>;

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_TYPES_H
