// ternary_compiler_ast.h - AST nodes, layout building, constant evaluation, and folding
#pragma once
#ifndef TERNARY_COMPILER_AST_H
#define TERNARY_COMPILER_AST_H

#include "ternary_compiler_ir.h"

namespace sandbox {
namespace compiler {

// =============================================================================
// AST
// =============================================================================

struct Expr;
struct Stmt;
using ExprPtr = std::shared_ptr<Expr>;

enum class ExprKind : uint8_t {
    Number,
    Name,
    Binary,
    Unary,
    Call,
    Field,
    Index,
    StructLiteral,
    ArrayLiteral,
};

struct Expr {
    ExprKind kind = ExprKind::Number;
    SourceSpan span;
    std::string text;
    long long number = 0;
    ExprPtr left;
    ExprPtr right;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> fields;
};

[[nodiscard]] inline bool extractInteger(const ExprPtr& expr, int& out) {
    if (!expr) return false;
    if (expr->kind == ExprKind::Number) {
        out = static_cast<int>(expr->number);
        return true;
    }
    if (expr->kind == ExprKind::Unary && expr->text == "-" &&
        expr->left && expr->left->kind == ExprKind::Number) {
        out = -static_cast<int>(expr->left->number);
        return true;
    }
    if (expr->kind == ExprKind::Name) {
        if (expr->text == "RELAXED") {
            out = -1;
            return true;
        }
        if (expr->text == "ACQ_REL") {
            out = 0;
            return true;
        }
        if (expr->text == "SEQ_CST") {
            out = 1;
            return true;
        }
    }
    return false;
}

enum class StmtKind : uint8_t {
    Let,
    Assign,
    Return,
    Expr,
    If,
    WhilePos,
    MatchSign,
    UnsafeBlock,
    TupleSwap,
    Const,
};

struct MatchArm {
    std::string name;
    std::string binding;
    std::vector<Stmt> body;
    SourceSpan span;
};

struct Stmt {
    StmtKind kind = StmtKind::Expr;
    SourceSpan span;
    std::string name;
    std::string second_name;
    TypeRef annotation = TypeRef::unknown();
    bool mutable_binding = false;
    ExprPtr expr;
    ExprPtr rhs;
    ExprPtr target;
    std::vector<Stmt> body;
    std::vector<Stmt> else_body;
    std::vector<MatchArm> arms;
};

struct StructDecl {
    std::string name;
    std::vector<std::pair<std::string, TypeRef>> fields;
    SourceSpan span;
};

struct ImportDecl {
    std::string name;
    SourceSpan span;
};

struct ConstDecl {
    std::string name;
    TypeRef type;
    ExprPtr expr;
    SourceSpan span;
};

struct FunctionAst {
    std::string name;
    std::vector<std::pair<std::string, TypeRef>> params;
    TypeRef return_type = TypeRef::voidType();
    std::vector<Stmt> body;
    SourceSpan span;
    std::vector<std::string> width_params;
};

struct ModuleAst {
    std::string name;
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<FunctionAst> functions;
    std::vector<ConstDecl> consts;
};

[[nodiscard]] inline int typeSizeWords(const TypeRef& type, const LayoutTable& layouts) {
    switch (type.kind) {
        case TypeKind::Numeric:
        case TypeKind::Lane:
            return type.scalar == ir::Type::T50 ? 2 : 1;
        case TypeKind::Struct: {
            auto it = layouts.find(type.name);
            return it == layouts.end() ? 1 : std::max(1, it->second.size_words);
        }
        case TypeKind::Array:
            return std::max(0, type.array_len) *
                   (type.element ? typeSizeWords(*type.element, layouts) : 1);
        case TypeKind::Vector:
            // A vector value is a full fixed-VLEN payload whenever it crosses
            // memory, a spill slot, or the outgoing stack.  Its register ABI
            // remains one architectural vector register, but its stack
            // representation is exactly 27 words.
            return architecture::v3::VECTOR_LANE_COUNT;
        default:
            return 1;
    }
}

[[nodiscard]] inline const FieldLayout* findFieldLayout(
    const TypeRef& type,
    const LayoutTable& layouts,
    const std::string& field) {

    if (type.kind != TypeKind::Struct) return nullptr;
    auto it = layouts.find(type.name);
    if (it == layouts.end()) return nullptr;
    for (const auto& candidate : it->second.fields) {
        if (candidate.name == field) return &candidate;
    }
    return nullptr;
}

[[nodiscard]] inline LayoutTable buildLayoutTable(
    const ModuleAst& ast,
    std::vector<Diagnostic>& diagnostics) {

    LayoutTable layouts;
    for (const auto& decl : ast.structs) layouts[decl.name] = LayoutInfo{};

    bool changed = true;
    for (int pass = 0; pass < 8 && changed; ++pass) {
        changed = false;
        for (const auto& decl : ast.structs) {
            LayoutInfo info;
            int offset = 0;
            for (const auto& field : decl.fields) {
                info.fields.push_back(FieldLayout{field.first, offset, field.second});
                offset += std::max(1, typeSizeWords(field.second, layouts));
            }
            info.size_words = std::max(1, offset);
            auto& slot = layouts[decl.name];
            if (slot.size_words != info.size_words || slot.fields.size() != info.fields.size()) {
                changed = true;
            }
            slot = std::move(info);
        }
    }

    for (const auto& decl : ast.structs) {
        std::set<std::string> fields;
        for (const auto& field : decl.fields) {
            if (!fields.insert(field.first).second) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "duplicate field '" + field.first + "' in struct '" + decl.name + "'",
                    decl.span});
            }
            if (field.second.kind == TypeKind::Struct && !layouts.count(field.second.name)) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "unknown field type '" + field.second.name + "'", decl.span});
            }
        }
    }

    return layouts;
}

[[nodiscard]] inline TypeRef functionType(std::vector<TypeRef> params, TypeRef result) {
    TypeRef out;
    out.kind = TypeKind::Function;
    out.params = std::move(params);
    out.result = std::make_shared<TypeRef>(std::move(result));
    return out;
}

[[nodiscard]] inline Effect combineEffects(Effect a, Effect b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

[[nodiscard]] inline bool isPureEffect(Effect effect) {
    return effect == Effect::Pure;
}

struct ConstantInfo {
    long long value = 0;
    TypeRef type;
};
using ConstantTable = std::map<std::string, ConstantInfo>;

inline void populateBuiltins(ConstantTable& table) {
    table["RELAXED"] = ConstantInfo{-1, TypeRef::numeric(ir::Type::T40)};
    table["ACQ_REL"] = ConstantInfo{0, TypeRef::numeric(ir::Type::T40)};
    table["SEQ_CST"] = ConstantInfo{1, TypeRef::numeric(ir::Type::T40)};
}

inline bool valueFitsInType(long long val, const TypeRef& type) {
    if (type.kind == TypeKind::Trit) {
        return val >= -1 && val <= 1;
    }
    if (type.kind == TypeKind::Numeric) {
        if (type.scalar == ir::Type::T1) return val >= -1 && val <= 1;
        if (type.scalar == ir::Type::T5) return val >= -121 && val <= 121;
        if (type.scalar == ir::Type::T10) return val >= -29524 && val <= 29524;
        if (type.scalar == ir::Type::T20) return val >= -1743392200LL && val <= 1743392200LL;
        return true;
    }
    return true;
}

inline bool evalConstantExpr(const ExprPtr& expr,
                             const ConstantTable& consts,
                             long long& outValue,
                             TypeRef& outType,
                             bool strict,
                             std::vector<Diagnostic>& diagnostics) {
    if (!expr) return false;
    switch (expr->kind) {
        case ExprKind::Number:
            outValue = expr->number;
            outType = TypeRef::numeric(ir::Type::T40);
            return true;
        case ExprKind::Name: {
            auto it = consts.find(expr->text);
            if (it != consts.end()) {
                outValue = it->second.value;
                outType = it->second.type;
                return true;
            }
            if (strict) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "expression is not compile-time constant: unknown constant '" + expr->text + "'",
                    expr->span});
            }
            return false;
        }
        case ExprKind::Unary: {
            long long val = 0;
            TypeRef t;
            if (!evalConstantExpr(expr->left, consts, val, t, strict, diagnostics)) return false;
            if (expr->text == "-") {
                outValue = -val;
                outType = t;
                return true;
            }
            if (strict) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "unary operator '" + expr->text + "' is not supported in constant expressions",
                    expr->span});
            }
            return false;
        }
        case ExprKind::Binary: {
            long long leftVal = 0, rightVal = 0;
            TypeRef leftType, rightType;
            if (!evalConstantExpr(expr->left, consts, leftVal, leftType, strict, diagnostics)) return false;
            if (!evalConstantExpr(expr->right, consts, rightVal, rightType, strict, diagnostics)) return false;
            outType = leftType;
            if (expr->text == "+") {
                outValue = leftVal + rightVal;
                return true;
            } else if (expr->text == "-") {
                outValue = leftVal - rightVal;
                return true;
            } else if (expr->text == "*") {
                outValue = leftVal * rightVal;
                return true;
            } else if (expr->text == "/") {
                if (rightVal == 0) {
                    diagnostics.push_back({DiagnosticSeverity::Error,
                        "division by zero in constant expression", expr->span});
                    return false;
                }
                outValue = leftVal / rightVal;
                return true;
            } else if (expr->text == "==") {
                outValue = (leftVal == rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            } else if (expr->text == "!=") {
                outValue = (leftVal != rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            } else if (expr->text == "<") {
                outValue = (leftVal < rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            } else if (expr->text == "<=") {
                outValue = (leftVal <= rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            } else if (expr->text == ">") {
                outValue = (leftVal > rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            } else if (expr->text == ">=") {
                outValue = (leftVal >= rightVal) ? 1 : -1;
                outType = TypeRef::trit();
                return true;
            }
            if (strict) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "binary operator '" + expr->text + "' is not supported in constant expressions",
                    expr->span});
            }
            return false;
        }
        default:
            if (strict) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "expression kind is not supported in constant expressions",
                    expr->span});
            }
            return false;
    }
}

inline bool checkAndRegisterConst(const std::string& name,
                                  const TypeRef& annotation,
                                  const ExprPtr& expr,
                                  ConstantTable& table,
                                  const ConstantTable& parentTable,
                                  std::vector<Diagnostic>& diagnostics,
                                  const SourceSpan& span) {
    ConstantTable consts = parentTable;
    for (const auto& pair : table) {
        consts[pair.first] = pair.second;
    }
    
    long long val = 0;
    TypeRef exprType;
    if (!evalConstantExpr(expr, consts, val, exprType, /*strict=*/true, diagnostics)) {
        return false;
    }
    
    if (annotation.kind != TypeKind::Unknown) {
        if (!valueFitsInType(val, annotation)) {
            diagnostics.push_back({DiagnosticSeverity::Error,
                "constant value " + std::to_string(val) + " does not fit in type " + annotation.str(),
                span});
            return false;
        }
        exprType = annotation;
    }
    
    table[name] = ConstantInfo{val, exprType};
    return true;
}

inline void foldExpr(ExprPtr& expr,
                     const ConstantTable& fileConsts,
                     const std::vector<ConstantTable>& scopes,
                     std::vector<Diagnostic>& diagnostics) {
    if (!expr) return;
    
    ConstantTable consts = fileConsts;
    for (const auto& scope : scopes) {
        for (const auto& pair : scope) {
            consts[pair.first] = pair.second;
        }
    }
    
    if (expr->kind != ExprKind::Field) {
        if (expr->left) foldExpr(expr->left, fileConsts, scopes, diagnostics);
    } else {
        if (expr->left) foldExpr(expr->left, fileConsts, scopes, diagnostics);
    }
    
    if (expr->kind != ExprKind::Call) {
        if (expr->right) foldExpr(expr->right, fileConsts, scopes, diagnostics);
    } else {
        for (auto& arg : expr->args) {
            foldExpr(arg, fileConsts, scopes, diagnostics);
        }
    }
    
    if (expr->kind == ExprKind::StructLiteral) {
        for (auto& field : expr->fields) {
            foldExpr(field.second, fileConsts, scopes, diagnostics);
        }
    }
    
    if (expr->kind == ExprKind::ArrayLiteral) {
        for (auto& arg : expr->args) {
            foldExpr(arg, fileConsts, scopes, diagnostics);
        }
    }
    
    if (expr->kind == ExprKind::Number) return;
    
    long long val = 0;
    TypeRef type;
    std::vector<Diagnostic> dummyDiag;
    if (evalConstantExpr(expr, consts, val, type, /*strict=*/false, dummyDiag)) {
        expr->kind = ExprKind::Number;
        expr->number = val;
        expr->text = std::to_string(val);
        expr->left = nullptr;
        expr->right = nullptr;
        expr->args.clear();
        expr->fields.clear();
    }
}

void foldBlock(std::vector<Stmt>& body,
               const ConstantTable& fileConsts,
               std::vector<ConstantTable>& scopes,
               std::vector<Diagnostic>& diagnostics);

void foldStmt(Stmt& stmt,
              const ConstantTable& fileConsts,
              std::vector<ConstantTable>& scopes,
              std::vector<Diagnostic>& diagnostics);

inline void foldStmt(Stmt& stmt,
                     const ConstantTable& fileConsts,
                     std::vector<ConstantTable>& scopes,
                     std::vector<Diagnostic>& diagnostics) {
    switch (stmt.kind) {
        case StmtKind::Let:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::Const: {
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            ConstantTable combined = fileConsts;
            for (const auto& scope : scopes) {
                for (const auto& pair : scope) {
                    combined[pair.first] = pair.second;
                }
            }
            long long val = 0;
            TypeRef type;
            if (evalConstantExpr(stmt.expr, combined, val, type, /*strict=*/true, diagnostics)) {
                if (!scopes.empty()) {
                    scopes.back()[stmt.name] = ConstantInfo{val, type};
                }
            }
            break;
        }
        case StmtKind::Assign:
            foldExpr(stmt.target, fileConsts, scopes, diagnostics);
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::Return:
            if (stmt.expr) foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::Expr:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::If:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            foldBlock(stmt.body, fileConsts, scopes, diagnostics);
            foldBlock(stmt.else_body, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::WhilePos:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            foldBlock(stmt.body, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::MatchSign:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            for (auto& arm : stmt.arms) {
                foldBlock(arm.body, fileConsts, scopes, diagnostics);
            }
            break;
        case StmtKind::UnsafeBlock:
            foldBlock(stmt.body, fileConsts, scopes, diagnostics);
            break;
        case StmtKind::TupleSwap:
            foldExpr(stmt.expr, fileConsts, scopes, diagnostics);
            foldExpr(stmt.rhs, fileConsts, scopes, diagnostics);
            break;
    }
}

inline void foldBlock(std::vector<Stmt>& body,
                      const ConstantTable& fileConsts,
                      std::vector<ConstantTable>& scopes,
                      std::vector<Diagnostic>& diagnostics) {
    scopes.push_back(ConstantTable{});
    for (auto& stmt : body) {
        foldStmt(stmt, fileConsts, scopes, diagnostics);
    }
    scopes.pop_back();
}

inline void foldModuleConstants(ModuleAst& ast, std::vector<Diagnostic>& diagnostics) {
    ConstantTable fileConsts;
    populateBuiltins(fileConsts);
    for (const auto& c : ast.consts) {
        long long val = 0;
        TypeRef type;
        if (evalConstantExpr(c.expr, fileConsts, val, type, /*strict=*/true, diagnostics)) {
            fileConsts[c.name] = ConstantInfo{val, type};
        }
    }
    
    for (auto& fn : ast.functions) {
        std::vector<ConstantTable> scopes;
        foldBlock(fn.body, fileConsts, scopes, diagnostics);
    }
}

struct InferResult {
    TypeRef type = TypeRef::unknown();
    Effect effect = Effect::Pure;
    bool addressable = false;
    bool value_restricted = false;
};

inline TypeRef substituteWidth(const TypeRef& type, const std::map<std::string, TypeRef>& bindings) {
    if (!type.width_var.empty()) {
        auto it = bindings.find(type.width_var);
        if (it != bindings.end()) return it->second;
    }
    TypeRef out = type;
    if (type.element) out.element = std::make_shared<TypeRef>(substituteWidth(*type.element, bindings));
    out.params.clear();
    for (const auto& param : type.params) out.params.push_back(substituteWidth(param, bindings));
    if (type.result) out.result = std::make_shared<TypeRef>(substituteWidth(*type.result, bindings));
    return out;
}

inline void specializeExpr(ExprPtr& expr, const std::map<std::string, TypeRef>& bindings) {
    if (!expr) return;
    specializeExpr(expr->left, bindings);
    specializeExpr(expr->right, bindings);
    for (auto& arg : expr->args) specializeExpr(arg, bindings);
    for (auto& f : expr->fields) specializeExpr(f.second, bindings);
}

inline void specializeStmt(Stmt& stmt, const std::map<std::string, TypeRef>& bindings);

inline void specializeStmt(Stmt& stmt, const std::map<std::string, TypeRef>& bindings) {
    stmt.annotation = substituteWidth(stmt.annotation, bindings);
    specializeExpr(stmt.expr, bindings);
    specializeExpr(stmt.rhs, bindings);
    for (auto& child : stmt.body) specializeStmt(child, bindings);
    for (auto& arm : stmt.arms) {
        for (auto& child : arm.body) specializeStmt(child, bindings);
    }
}

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_AST_H
