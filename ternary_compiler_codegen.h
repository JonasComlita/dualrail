// ternary_compiler_codegen.h - Type inference, code generation, verification, and optimization

#pragma once
#ifndef TERNARY_COMPILER_CODEGEN_H
#define TERNARY_COMPILER_CODEGEN_H

#include "ternary_compiler_ast.h"
#include "ternary_compiler_cfg.h"
#include "ternary_compiler_parser.h"
#include <array>
#include <deque>
#include <iterator>

namespace sandbox {
namespace compiler {

constexpr int kRegisterArgCount = 6;
constexpr int kCallArgScratchWords = 16;
constexpr int kCallArgScratchAreas = 8;

class TypeInferencer {
public:
    TypeInferencer(const ModuleAst& ast, const LayoutTable& layouts, std::map<std::string, FunctionAst>* instantiated_functions_out = nullptr)
        : ast_(ast), layouts_(layouts), instantiated_functions_out_(instantiated_functions_out) {
        // Register module-level consts in the environment
        for (const auto& const_decl : ast_.consts) {
            // For now, just register consts with their annotated types
            // The actual evaluation happens during code generation
            env_.values[const_decl.name] = Scheme{{}, const_decl.type, true};
        }
        
        for (const auto& fn : ast_.functions) {
            if (fn.width_params.empty()) {
                std::vector<TypeRef> params;
                for (const auto& param : fn.params) params.push_back(param.second);
                env_.values[fn.name] = Scheme{{}, functionType(params, fn.return_type), true};
            }
        }
    }

    [[nodiscard]] std::vector<Diagnostic> inferModule() {
        for (const auto& fn : ast_.functions) {
            if (fn.width_params.empty()) {
                inferFunction(fn);
            }
        }
        return diagnostics_;
    }

private:
    const ModuleAst& ast_;
    const LayoutTable& layouts_;
    TypeEnv env_;
    Substitution subst_;
    std::vector<Diagnostic> diagnostics_;
    TypeVarId next_type_var_ = 1;
    TypeRef current_return_ = TypeRef::voidType();
    bool unsafe_allowed_ = false;
    std::map<std::string, FunctionAst>* instantiated_functions_out_ = nullptr;
    std::set<std::string> moved_in_current_scope_;

    [[nodiscard]] TypeRef fresh() { return TypeRef::typeVar(next_type_var_++); }

    void inferFunction(const FunctionAst& fn) {
        TypeEnv saved = env_;
        std::set<std::string> saved_moves = moved_in_current_scope_;
        TypeRef saved_return = current_return_;
        moved_in_current_scope_.clear();
        current_return_ = fn.return_type;
        for (const auto& param : fn.params) {
            env_.values[param.first] = Scheme{{}, param.second, false};
        }
        inferBlock(fn.body);
        env_ = std::move(saved);
        moved_in_current_scope_ = std::move(saved_moves);
        current_return_ = saved_return;
    }

    InferResult inferBlock(const std::vector<Stmt>& body) {
        InferResult out;
        for (const auto& stmt : body) {
            InferResult stmtResult = inferStmt(stmt);
            out.effect = combineEffects(out.effect, stmtResult.effect);
            out.value_restricted = out.value_restricted || stmtResult.value_restricted;
            out.type = stmtResult.type;
        }
        return out;
    }

    InferResult inferStmt(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Let: {
                InferResult value = inferExpr(stmt.expr);
                TypeRef localType = subst_.apply(value.type);
                if (stmt.annotation.kind != TypeKind::Unknown) {
                    const bool fittingLiteral =
                        stmt.expr && stmt.expr->kind == ExprKind::Number &&
                        valueFitsInType(stmt.expr->number, stmt.annotation);
                    if (fittingLiteral) {
                        localType = stmt.annotation;
                    }
                    if (!fittingLiteral && !canWiden(localType, stmt.annotation)) {
                        diagnostics_.push_back({DiagnosticSeverity::Error,
                            "implicit narrowing is not allowed from " + localType.str() +
                            " to " + stmt.annotation.str(), stmt.span});
                    }
                    if (!fittingLiteral) {
                        unify(localType, stmt.annotation, subst_, diagnostics_, stmt.span,
                              "local annotation mismatch");
                    }
                    localType = stmt.annotation;
                }
                const bool restricted = stmt.mutable_binding || value.value_restricted ||
                                        !isPureEffect(value.effect);
                env_.values[stmt.name] = generalize(env_, subst_.apply(localType), restricted);
                return InferResult{localType, value.effect, false, restricted};
            }
            case StmtKind::Assign: {
                if (stmt.target && stmt.target->kind == ExprKind::Name) {
                    moved_in_current_scope_.erase(stmt.target->text);
                }
                InferResult lhs = inferExpr(stmt.target);
                InferResult rhs = inferExpr(stmt.expr);
                TypeRef left = subst_.apply(lhs.type);
                TypeRef right = subst_.apply(rhs.type);
                if (!canWiden(right, left)) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "implicit narrowing is not allowed from " + right.str() +
                        " to " + left.str(), stmt.span});
                }
                unify(right, left, subst_, diagnostics_, stmt.span, "assignment type mismatch");
                return InferResult{left, combineEffects(lhs.effect, rhs.effect), false, true};
            }
            case StmtKind::Return: {
                InferResult value = stmt.expr ? inferExpr(stmt.expr)
                                              : InferResult{TypeRef::voidType(), Effect::Control};
                TypeRef actual = subst_.apply(value.type);
                if (!canWiden(actual, current_return_)) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "implicit narrowing is not allowed from " + actual.str() +
                        " to " + current_return_.str(), stmt.span});
                }
                unify(actual, current_return_, subst_, diagnostics_, stmt.span,
                      "return type mismatch");
                value.effect = combineEffects(value.effect, Effect::Control);
                value.value_restricted = true;
                return value;
            }
            case StmtKind::Expr:
                return inferExpr(stmt.expr);
            case StmtKind::If: {
                InferResult cond = inferExpr(stmt.expr);
                unify(cond.type, TypeRef::trit(), subst_, diagnostics_,
                      stmt.span, "if condition must evaluate to T1");
                InferResult then_body = inferBlock(stmt.body);
                InferResult else_body = inferBlock(stmt.else_body);
                Effect effect = combineEffects(cond.effect,
                                               combineEffects(then_body.effect, else_body.effect));
                return InferResult{TypeRef::voidType(), effect, false, true};
            }
            case StmtKind::WhilePos: {
                InferResult cond = inferExpr(stmt.expr);
                unify(cond.type, TypeRef::trit(), subst_, diagnostics_,
                      stmt.span, "while condition must evaluate to T1");
                InferResult body = inferBlock(stmt.body);
                return InferResult{TypeRef::voidType(), combineEffects(cond.effect, body.effect),
                                   false, true};
            }
            case StmtKind::MatchSign: {
                InferResult matched = inferExpr(stmt.expr);
                Effect effect = matched.effect;
                TypeRef subjectType = subst_.apply(matched.type);
                if (stmt.expr && stmt.expr->kind == ExprKind::Name &&
                    pointerTypeView(subjectType)) {
                    moved_in_current_scope_.erase(stmt.expr->text);
                }

                const TypeRef* subjectPointer = pointerTypeView(subjectType);
                bool isPointer = subjectPointer != nullptr;
                bool isNumericOrTrit = isNumericLike(subjectType);

                std::set<std::string> seenArms;
                for (const auto& arm : stmt.arms) {
                    seenArms.insert(arm.name);
                }

                if (isPointer) {
                    bool exhaustive = seenArms.count("_") ||
                                      (seenArms.count("null") && seenArms.count("unknown") && seenArms.count("valid"));
                    if (!exhaustive) {
                        diagnostics_.push_back({DiagnosticSeverity::Error,
                            "match on pointer must cover null, unknown, and valid arms", stmt.span});
                    }
                } else if (isNumericOrTrit) {
                    bool exhaustive = seenArms.count("_") ||
                                      (seenArms.count("neg") && seenArms.count("zero") && seenArms.count("pos"));
                    if (!exhaustive) {
                        diagnostics_.push_back({DiagnosticSeverity::Error,
                            "match on numeric/trit must cover neg, zero, and pos arms", stmt.span});
                    }
                } else {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "match subject must be pointer, numeric, or trit, but found " + subjectType.str(), stmt.span});
                }

                auto base_moves = moved_in_current_scope_;
                std::set<std::string> union_moves = base_moves;

                for (const auto& arm : stmt.arms) {
                    auto savedEnv = env_;
                    moved_in_current_scope_ = base_moves;
                    if (isPointer && arm.name == "valid") {
                        if (arm.binding.empty()) {
                            diagnostics_.push_back({DiagnosticSeverity::Error,
                                "expected binding identifier in valid arm", arm.span});
                        } else {
                            TypeRef innerType = subjectPointer && subjectPointer->element
                                ? *subjectPointer->element
                                : TypeRef::numeric(ir::Type::T40);
                            PointerRegion region = subjectPointer ? subjectPointer->region : PointerRegion::Stack;
                            env_.values[arm.binding] = Scheme{ {}, TypeRef::pointer(innerType, region, PointerState::Valid), false };
                        }
                    }
                    effect = combineEffects(effect, inferBlock(arm.body).effect);
                    env_ = std::move(savedEnv);
                    union_moves.insert(moved_in_current_scope_.begin(), moved_in_current_scope_.end());
                }
                moved_in_current_scope_ = union_moves;
                return InferResult{TypeRef::voidType(), effect, false, true};
            }
            case StmtKind::UnsafeBlock: {
                const bool old = unsafe_allowed_;
                unsafe_allowed_ = true;
                InferResult out = inferBlock(stmt.body);
                unsafe_allowed_ = old;
                out.value_restricted = true;
                return out;
            }
            case StmtKind::TupleSwap:
                return InferResult{TypeRef::voidType(), Effect::WriteMem, false, true};
            case StmtKind::Const: {
                // Infer the type of the const expression and register it in the environment
                InferResult expr_result = inferExpr(stmt.expr);
                TypeRef const_type = stmt.annotation.kind != TypeKind::Unknown 
                    ? stmt.annotation 
                    : subst_.apply(expr_result.type);
                // Register the const in the environment with its inferred type
                env_.values[stmt.name] = Scheme{{}, const_type, true};
                return InferResult{TypeRef::voidType(), Effect::Pure, false, false};
            }
        }
        return {};
    }

    InferResult inferExpr(const ExprPtr& expr) {
        if (!expr) return {};
        switch (expr->kind) {
            case ExprKind::Number:
                return InferResult{TypeRef::numeric(ir::Type::T40), Effect::Pure};
            case ExprKind::Name: {
                if (moved_in_current_scope_.count(expr->text)) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "use of moved value '" + expr->text + "'", expr->span});
                }
                auto it = env_.values.find(expr->text);
                if (it == env_.values.end()) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "unknown name '" + expr->text + "'", expr->span});
                    return InferResult{fresh(), Effect::Pure};
                }
                TypeRef t = instantiate(it->second, next_type_var_);
                if (t.kind == TypeKind::Owned) {
                    moved_in_current_scope_.insert(expr->text);
                }
                return InferResult{t, Effect::Pure, true, false};
            }
            case ExprKind::Unary:
                return inferUnary(*expr);
            case ExprKind::Binary:
                return inferBinary(*expr);
            case ExprKind::Call:
                return inferCall(expr);
            case ExprKind::Field:
                return inferField(*expr);
            case ExprKind::Index:
                return inferIndex(*expr);
            case ExprKind::StructLiteral:
                return inferStructLiteral(*expr);
            case ExprKind::ArrayLiteral:
                return inferArrayLiteral(*expr);
        }
        return {};
    }

    InferResult inferUnary(const Expr& expr) {
        InferResult inner = inferExpr(expr.left);
        if (expr.text == "-") {
            unify(inner.type, TypeRef::numeric(ir::Type::T40), subst_, diagnostics_,
                  expr.span, "unary minus requires a numeric operand");
            inner.type = commonNumericType(subst_.apply(inner.type), TypeRef::numeric(ir::Type::T40));
            return inner;
        }
        if (expr.text == "&") {
            inner.addressable = false;
            inner.type = TypeRef::pointer(subst_.apply(inner.type));
            if (expr.left && expr.left->kind == ExprKind::Name) {
                moved_in_current_scope_.erase(expr.left->text);
            }
            return inner;
        }
        if (expr.text == "*") {
            if (expr.left && expr.left->kind == ExprKind::Name) {
                moved_in_current_scope_.erase(expr.left->text);
            }
            TypeRef elem = fresh();
            TypeRef ptr = TypeRef::pointer(elem);
            unify(inner.type, ptr, subst_, diagnostics_, expr.span,
                  "dereference requires a pointer");
            TypeRef concrete_ptr = subst_.apply(inner.type);
            const TypeRef* pointer = pointerTypeView(concrete_ptr);
            if (pointer && pointer->state != PointerState::Valid) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "cannot dereference pointer before proving it is valid", expr.span});
            }
            inner.type = subst_.apply(elem);
            inner.effect = combineEffects(inner.effect, Effect::ReadMem);
            inner.addressable = true;
            inner.value_restricted = true;
            return inner;
        }
        return inner;
    }

    InferResult inferBinary(const Expr& expr) {
        InferResult lhs = inferExpr(expr.left);
        InferResult rhs = inferExpr(expr.right);
        TypeRef leftType = subst_.apply(lhs.type);
        TypeRef rightType = subst_.apply(rhs.type);
        TypeRef common = commonNumericType(leftType, rightType);
        if (expr.right && expr.right->kind == ExprKind::Number &&
            isNumericLike(leftType) &&
            valueFitsInType(expr.right->number, leftType)) {
            common = leftType;
        } else if (expr.left && expr.left->kind == ExprKind::Number &&
                   isNumericLike(rightType) &&
                   valueFitsInType(expr.left->number, rightType)) {
            common = rightType;
        }
        unify(lhs.type, common, subst_, diagnostics_, expr.span, "left operand type mismatch");
        unify(rhs.type, common, subst_, diagnostics_, expr.span, "right operand type mismatch");
        TypeRef resultType = isComparison(expr.text) ? TypeRef::trit() : common;
        return InferResult{resultType, combineEffects(lhs.effect, rhs.effect), false,
                           lhs.value_restricted || rhs.value_restricted};
    }

    InferResult inferCall(const ExprPtr& expr) {
        if (expr->text == "shared_alloc") {
            if (expr->args.size() != 1) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "shared_alloc requires exactly one initial value argument", expr->span});
                return InferResult{TypeRef::shared(fresh(), MemoryOrder::Relaxed), Effect::WriteMem, false, true};
            }
            InferResult arg_res = inferExpr(expr->args[0]);
            return InferResult{TypeRef::shared(arg_res.type, MemoryOrder::Relaxed), combineEffects(arg_res.effect, Effect::WriteMem), false, true};
        }
        if (expr->text == "atomic_load") {
            if (expr->args.size() != 2) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_load requires shared pointer and memory order arguments", expr->span});
                return InferResult{fresh(), Effect::Atomic, false, true};
            }
            InferResult shared_res = inferExpr(expr->args[0]);
            TypeRef shared_type = subst_.apply(shared_res.type);
            if (shared_type.kind != TypeKind::Shared) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_load first argument must be shared<T, ORDER>, but found " + shared_type.str(), expr->span});
                return InferResult{fresh(), Effect::Atomic, false, true};
            }
            int order_val = 0;
            if (!extractInteger(expr->args[1], order_val) || !isa::isAtomicOrder(order_val)) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_load memory order must be a compile-time constant (-1, 0, or 1)", expr->span});
                return InferResult{shared_type.element ? *shared_type.element : TypeRef::numeric(ir::Type::T40), Effect::Atomic, false, true};
            }
            int declared_order = static_cast<int>(shared_type.order);
            if (order_val < declared_order) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_load memory order (" + std::to_string(order_val) + ") is weaker than declared order (" + std::to_string(declared_order) + ")", expr->span});
            }
            TypeRef element = shared_type.element ? *shared_type.element : TypeRef::numeric(ir::Type::T40);
            return InferResult{element, combineEffects(shared_res.effect, Effect::Atomic), false, true};
        }
        if (expr->text == "atomic_store") {
            if (expr->args.size() != 3) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_store requires shared pointer, value, and memory order arguments", expr->span});
                return InferResult{TypeRef::voidType(), Effect::Atomic, false, true};
            }
            InferResult shared_res = inferExpr(expr->args[0]);
            TypeRef shared_type = subst_.apply(shared_res.type);
            if (shared_type.kind != TypeKind::Shared) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_store first argument must be shared<T, ORDER>, but found " + shared_type.str(), expr->span});
                return InferResult{TypeRef::voidType(), Effect::Atomic, false, true};
            }
            InferResult val_res = inferExpr(expr->args[1]);
            TypeRef element = shared_type.element ? *shared_type.element : TypeRef::numeric(ir::Type::T40);
            unify(val_res.type, element, subst_, diagnostics_, expr->span, "atomic_store value type mismatch");
            int order_val = 0;
            if (!extractInteger(expr->args[2], order_val) || !isa::isAtomicOrder(order_val)) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_store memory order must be a compile-time constant (-1, 0, or 1)", expr->span});
                return InferResult{TypeRef::voidType(), Effect::Atomic, false, true};
            }
            int declared_order = static_cast<int>(shared_type.order);
            if (order_val < declared_order) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "atomic_store memory order (" + std::to_string(order_val) + ") is weaker than declared order (" + std::to_string(declared_order) + ")", expr->span});
            }
            Effect combined = combineEffects(shared_res.effect, val_res.effect);
            return InferResult{TypeRef::voidType(), combineEffects(combined, Effect::Atomic), false, true};
        }
        if (isRuntimeName(expr->text)) {
            Effect effect = Effect::Syscall;
            for (const auto& arg : expr->args) effect = combineEffects(effect, inferExpr(arg).effect);
            return InferResult{TypeRef::numeric(ir::Type::T40), effect, false, true};
        }
        if (isUnsafeIntrinsicName(expr->text)) {
            if (!unsafe_allowed_) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "raw CSR/atomic intrinsics require an unsafe block", expr->span});
            }
            return InferResult{TypeRef::numeric(ir::Type::T40),
                               expr->text == "csr_read" || expr->text == "csr_write"
                                   ? Effect::CSR : Effect::Atomic,
                               false, true};
        }

        const FunctionAst* found_generic = nullptr;
        for (const auto& fn : ast_.functions) {
            if (fn.name == expr->text) {
                found_generic = &fn;
                break;
            }
        }

        if (found_generic && !found_generic->width_params.empty()) {
            std::map<std::string, TypeRef> width_bindings;
            for (const auto& wp : found_generic->width_params) {
                width_bindings[wp] = fresh();
            }

            std::vector<TypeRef> generic_params;
            for (const auto& param : found_generic->params) {
                generic_params.push_back(substituteWidth(param.second, width_bindings));
            }

            if (generic_params.size() != expr->args.size()) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "function '" + expr->text + "' expects " +
                    std::to_string(generic_params.size()) + " argument(s)", expr->span});
            }
            Effect effect = Effect::Control;
            for (std::size_t i = 0; i < expr->args.size() && i < generic_params.size(); ++i) {
                InferResult arg = inferExpr(expr->args[i]);
                effect = combineEffects(effect, arg.effect);
                unify(arg.type, generic_params[i], subst_, diagnostics_, expr->args[i]->span,
                      "function argument mismatch");
            }

            std::string mangled_name = found_generic->name;
            std::map<std::string, TypeRef> concrete_bindings;
            for (const auto& wp : found_generic->width_params) {
                TypeRef concrete_wp = subst_.apply(width_bindings[wp]);
                if (!isNumericLike(concrete_wp)) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "width parameter '" + wp + "' could not be resolved to a concrete numeric type",
                        expr->span});
                    concrete_wp = TypeRef::numeric(ir::Type::T40);
                }
                if (concrete_wp.kind == TypeKind::Trit) {
                    concrete_wp = TypeRef::numeric(ir::Type::T1);
                }
                concrete_bindings[wp] = concrete_wp;
                mangled_name += "__W" + std::string(ir::suffix(concrete_wp.scalar));
            }

            expr->text = mangled_name;

            if (instantiated_functions_out_ && instantiated_functions_out_->find(mangled_name) == instantiated_functions_out_->end()) {
                FunctionAst monomorphized = *found_generic;
                monomorphized.name = mangled_name;
                monomorphized.width_params.clear();
                for (auto& param : monomorphized.params) {
                    param.second = substituteWidth(param.second, concrete_bindings);
                }
                monomorphized.return_type = substituteWidth(monomorphized.return_type, concrete_bindings);
                for (auto& stmt : monomorphized.body) {
                    specializeStmt(stmt, concrete_bindings);
                }

                std::vector<TypeRef> concrete_params;
                for (const auto& param : monomorphized.params) {
                    concrete_params.push_back(param.second);
                }
                TypeRef concrete_fn_type = functionType(concrete_params, monomorphized.return_type);
                env_.values[mangled_name] = Scheme{{}, concrete_fn_type, true};

                (*instantiated_functions_out_)[mangled_name] = monomorphized;

                inferFunction(monomorphized);
            }

            TypeRef return_type = substituteWidth(found_generic->return_type, concrete_bindings);
            return InferResult{subst_.apply(return_type), effect, false, true};
        }

        auto it = env_.values.find(expr->text);
        if (it == env_.values.end()) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "unknown function '" + expr->text + "'", expr->span});
            return InferResult{fresh(), Effect::Control, false, true};
        }
        TypeRef callee = instantiate(it->second, next_type_var_);
        if (callee.kind != TypeKind::Function || !callee.result) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "called value is not a function", expr->span});
            return InferResult{fresh(), Effect::Control, false, true};
        }
        if (callee.params.size() != expr->args.size()) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "function '" + expr->text + "' expects " +
                std::to_string(callee.params.size()) + " argument(s)", expr->span});
        }
        Effect effect = Effect::Control;
        for (std::size_t i = 0; i < expr->args.size() && i < callee.params.size(); ++i) {
            InferResult arg = inferExpr(expr->args[i]);
            effect = combineEffects(effect, arg.effect);
            unify(arg.type, callee.params[i], subst_, diagnostics_, expr->args[i]->span,
                  "function argument mismatch");
        }
        return InferResult{subst_.apply(*callee.result), effect, false, true};
    }

    InferResult inferField(const Expr& expr) {
        InferResult base = inferExpr(expr.left);
        TypeRef baseType = subst_.apply(base.type);
        const FieldLayout* field = findFieldLayout(baseType, layouts_, expr.text);
        if (!field) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "unknown field '" + expr.text + "' on " + baseType.str(), expr.span});
            return InferResult{fresh(), base.effect, true, true};
        }
        return InferResult{field->type, base.effect, true, base.value_restricted};
    }

    InferResult inferIndex(const Expr& expr) {
        InferResult base = inferExpr(expr.left);
        InferResult index = inferExpr(expr.right);
        unify(index.type, TypeRef::numeric(ir::Type::T40), subst_, diagnostics_,
              expr.span, "array index must be numeric");
        TypeRef baseType = subst_.apply(base.type);
        if (baseType.kind != TypeKind::Array || !baseType.element) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "indexing requires an array value", expr.span});
            return InferResult{fresh(), combineEffects(base.effect, index.effect), true, true};
        }
        int constantIndex = 0;
        if (literalInteger(expr.right, constantIndex) &&
            (constantIndex < 0 || constantIndex >= baseType.array_len)) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "constant array index " + std::to_string(constantIndex) +
                " is out of bounds for length " + std::to_string(baseType.array_len),
                expr.span});
        }
        return InferResult{*baseType.element, combineEffects(base.effect, index.effect),
                           true, true};
    }

    InferResult inferStructLiteral(const Expr& expr) {
        TypeRef type;
        type.kind = TypeKind::Struct;
        type.name = expr.text;
        auto layoutIt = layouts_.find(expr.text);
        if (layoutIt == layouts_.end()) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "unknown struct type '" + expr.text + "'", expr.span});
            return InferResult{type, Effect::Pure};
        }
        Effect effect = Effect::Pure;
        for (const auto& field : layoutIt->second.fields) {
            const ExprPtr* fieldExpr = nullptr;
            for (const auto& candidate : expr.fields) {
                if (candidate.first == field.name) fieldExpr = &candidate.second;
            }
            if (!fieldExpr) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "missing field '" + field.name + "' in struct literal", expr.span});
                continue;
            }
            InferResult value = inferExpr(*fieldExpr);
            effect = combineEffects(effect, value.effect);
            unify(value.type, field.type, subst_, diagnostics_, (*fieldExpr)->span,
                  "struct field type mismatch");
        }
        return InferResult{type, effect, false, false};
    }

    InferResult inferArrayLiteral(const Expr& expr) {
        TypeRef elem = expr.args.empty() ? fresh() : inferExpr(expr.args[0]).type;
        Effect effect = Effect::Pure;
        if (!expr.args.empty()) {
            for (const auto& arg : expr.args) {
                InferResult value = inferExpr(arg);
                effect = combineEffects(effect, value.effect);
                unify(value.type, elem, subst_, diagnostics_, arg->span,
                      "array element type mismatch");
                elem = subst_.apply(elem);
            }
        }
        TypeRef array;
        array.kind = TypeKind::Array;
        array.element = std::make_shared<TypeRef>(subst_.apply(elem));
        array.array_len = static_cast<int>(expr.args.size());
        return InferResult{array, effect, false, false};
    }

    [[nodiscard]] static bool literalInteger(const ExprPtr& expr, int& out) {
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
        return false;
    }

    [[nodiscard]] static bool isRuntimeName(const std::string& name) {
        return name == "sys_write_int" || name == "sys_newline" ||
               name == "sys_clear" || name == "sys_exit" ||
               name == "sys_getpid" || name == "sys_uptime" ||
               name == "sys_read_console_word" || name == "sys_spawn_static" ||
               name == "sys_waitpid" || name == "sys_open" ||
               name == "sys_close" || name == "sys_read" ||
               name == "sys_write" || name == "sys_stat" ||
               name == "sys_readdir" || name == "sys_brk" ||
               name == "sys_sbrk" || name == "sys_fork" ||
               name == "sys_exec" || name == "sys_write_char" ||
               name == "sys_ipc_send" || name == "sys_ipc_recv" ||
               name == "sys_fb_init" || name == "sys_fb_flip" ||
               name == "sys_window_create" || name == "sys_window_get_buffer" ||
               name == "sys_window_present" || name == "sys_window_move" ||
               name == "sys_window_set_z" || name == "sys_window_destroy" ||
               name == "sys_window_read_event" || name == "sys_window_resize" ||
               name == "sys_window_request_close" || name == "sys_socket" ||
               name == "sys_bind" || name == "sys_connect" ||
               name == "sys_send" || name == "sys_recv" ||
               name == "sys_mkdir" || name == "sys_unlink" ||
               name == "sys_sleep" || name == "sys_yield" ||
               name == "sys_sleep_until_tick" || name == "sys_ps" ||
               name == "sys_fsync" || name == "sys_kill" ||
               name == "sys_suspend" || name == "sys_resume" ||
               name == "sys_getproc" || name == "sys_futex_wait" ||
               name == "sys_futex_wake" || name == "sys_ipc_recv_blocking" ||
               name == "sys_wait_event" || name == "sys_sleep_ms" ||
               name == "sys_app_spawn" ||
               name == "sys_reboot";
    }

    [[nodiscard]] static bool isUnsafeIntrinsicName(const std::string& name) {
        return name == "csr_read" || name == "csr_write" ||
               name == "arch_wait" ||
               name == "tlbinv" || name == "tmod" ||
               name == "tldr" || name == "tstr" || name == "fence" ||
               name == "load" || name == "store";
    }
};

[[nodiscard]] inline std::vector<Diagnostic> inferModuleTypes(
    const ModuleAst& ast,
    const LayoutTable& layouts,
    std::map<std::string, FunctionAst>& instantiated_functions_out) {

    TypeInferencer inferencer(ast, layouts, &instantiated_functions_out);
    return inferencer.inferModule();
}

// =============================================================================
// Analysis and code generation
// =============================================================================

struct LocalInfo {
    TypeRef type = TypeRef::unknown();
    int offset = 0;
    int size_words = 1;
    bool mutable_binding = false;
    bool address_taken = false;
    bool by_pointer = false;
    ValueId ir_address = -1;
};

[[nodiscard]] inline bool usesWideT50Pair(const TypeRef& type) {
    return (type.kind == TypeKind::Numeric || type.kind == TypeKind::Lane) &&
           type.scalar == ir::Type::T50;
}

[[nodiscard]] inline std::string scalarMemoryMnemonic(
        const char* operation,
        const TypeRef& type) {
    return std::string(operation) + (usesWideT50Pair(type) ? ".t50" : "");
}

struct FunctionContext {
    const FunctionAst* ast = nullptr;
    Function ir;
    BasicBlock* block = nullptr;
    std::map<std::string, LocalInfo> locals;
    ConstantTable constants;  // module and local const values
    std::map<std::string, TypeRef> function_returns;
    std::map<std::string, std::vector<TypeRef>> function_params;
    const LayoutTable* layouts = nullptr;
    const CompilerOptions* options = nullptr;
    std::vector<Diagnostic>* diagnostics = nullptr;
    std::ostringstream asm_out;
    int next_value = 1;
    int next_local_offset = 1;
    int frame_words = 9;
    int label_counter = 0;
    bool unsafe_allowed = false;
    std::vector<int> free_regs = {19, 20, 21, 22, 23};

    bool dry_run = false;
    const AllocationResult* allocation = nullptr;
    std::map<int, ValueId> reg_to_value;
    std::vector<std::vector<std::string>> scope_vars;
    std::set<std::string> moved_vars;
    std::vector<int> callee_saved_regs;
    int return_slot_offset = -1;
    int call_arg_slot_base = -1;
    int call_arg_depth = 0;
    int max_call_arg_depth = 0;
    int next_virtual_reg = 100;

    [[nodiscard]] std::string label(const std::string& stem) {
        return ast->name + "_" + stem + "_" + std::to_string(label_counter++);
    }
    [[nodiscard]] static bool isColorableTemporary(int reg) {
        return (reg >= 19 && reg <= 23) || reg >= 100;
    }

    std::string colorRegister(int reg, bool is_dest) {
        if (!isColorableTemporary(reg)) {
            return "r" + std::to_string(reg);
        }
        ValueId id = -1;
        if (is_dest) {
            id = next_value;
        } else {
            auto it = reg_to_value.find(reg);
            if (it != reg_to_value.end()) {
                id = it->second;
            }
        }
        if (id >= 0 && allocation) {
            auto color_it = allocation->scalar_registers.find(id);
            if (color_it != allocation->scalar_registers.end()) {
                return "r" + std::to_string(color_it->second);
            }
        }
        if (reg >= 100 && allocation && diagnostics) {
            diagnostics->push_back({DiagnosticSeverity::Error,
                "uncolored virtual register r" + std::to_string(reg) +
                    " reached assembly emission; spill lowering is not implemented",
                ast ? ast->span : SourceSpan{}});
        }
        return "r" + std::to_string(reg);
    }
    std::string rewriteAssemblyLine(const std::string& line) {
        if (dry_run || !allocation) return line;
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.empty()) return line;
        std::string mnemonic;
        std::size_t space = trimmed.find_first_of(" \t,");
        if (space != std::string::npos) {
            mnemonic = trimmed.substr(0, space);
        } else {
            mnemonic = trimmed;
        }
        bool is_store = (mnemonic == "store" || mnemonic.rfind("store.", 0) == 0);
        bool is_branch = (mnemonic == "brn" || mnemonic == "brz" || mnemonic == "brp" || mnemonic.rfind("branch_", 0) == 0);
        bool is_syscall = (mnemonic == "syscall");
        bool is_halt = (mnemonic == "halt");
        bool is_swap = (mnemonic == "swap");
        bool is_csrw = (mnemonic == "csrw");
        bool has_dest = !is_store && !is_branch && !is_syscall && !is_halt && !is_swap && !is_csrw;
        bool first_reg = true;
        std::string result;
        std::size_t i = 0;
        int pending_dest_reg = -1;
        while (i < line.size()) {
            if (line[i] == 'r' && i + 1 < line.size() && std::isdigit(line[i + 1])) {
                std::size_t j = i + 1;
                while (j < line.size() && std::isdigit(line[j])) {
                    ++j;
                }
                int reg = std::stoi(line.substr(i + 1, j - i - 1));
                bool is_dest = false;
                if (has_dest && first_reg) {
                    is_dest = true;
                    first_reg = false;
                    pending_dest_reg = reg;
                }
                result += colorRegister(reg, is_dest);
                i = j;
            } else {
                result += line[i];
                ++i;
            }
        }
        if (pending_dest_reg != -1 && isColorableTemporary(pending_dest_reg)) {
            reg_to_value[pending_dest_reg] = next_value;
        }
        return result;
    }
    void line(const std::string& text) {
        if (dry_run) return;
        asm_out << "    " << rewriteAssemblyLine(text) << "\n";
    }
    void raw(const std::string& text) {
        if (dry_run) return;
        asm_out << rewriteAssemblyLine(text) << "\n";
    }
    [[nodiscard]] int acquire() {
        if (free_regs.empty()) {
            return next_virtual_reg++;
        }
        int reg = free_regs.back();
        free_regs.pop_back();
        return reg;
    }
    void release(int reg) {
        if (reg < 19 || reg > 23) return;
        if (std::find(free_regs.begin(), free_regs.end(), reg) == free_regs.end()) {
            free_regs.push_back(reg);
            std::sort(free_regs.begin(), free_regs.end());
        }
    }
    ValueId value(InstrOpcode op, TypeRef type, const SourceSpan& span, int dest_reg = -1) {
        ValueId id = next_value++;
        if (dest_reg != -1) {
            reg_to_value[dest_reg] = id;
        }
        if (block) {
            Instr instr;
            instr.def = id;
            instr.opcode = op;
            instr.type = std::move(type);
            switch (op) {
                case InstrOpcode::Load:
                case InstrOpcode::SpillLoad:
                case InstrOpcode::Deref:
                    instr.effect = Effect::ReadMem;
                    break;
                case InstrOpcode::Store:
                case InstrOpcode::SpillStore:
                case InstrOpcode::Swap:
                    instr.effect = Effect::WriteMem;
                    break;
                case InstrOpcode::Syscall:
                    instr.effect = Effect::Syscall;
                    break;
                case InstrOpcode::Tldr:
                case InstrOpcode::Tstr:
                case InstrOpcode::Fence:
                    instr.effect = Effect::Atomic;
                    break;
                case InstrOpcode::Csrr:
                case InstrOpcode::Csrw:
                case InstrOpcode::Csrrw:
                case InstrOpcode::TlbInv:
                    instr.effect = Effect::CSR;
                    break;
                case InstrOpcode::Wait:
                case InstrOpcode::Call:
                case InstrOpcode::CallR:
                case InstrOpcode::Ret:
                    instr.effect = Effect::Control;
                    break;
                default:
                    instr.effect = Effect::Pure;
                    break;
            }
            instr.span = span;
            block->instructions.push_back(std::move(instr));
        }
        return id;
    }
};

struct ExprCode {
    int reg = 0;
    TypeRef type = TypeRef::unknown();
    bool address = false;
    ValueId value = -1;
};

struct LValueCode {
    int reg = 0;
    TypeRef type = TypeRef::unknown();
    bool mutable_binding = true;
    bool valid = false;
};

[[nodiscard]] AllocationResult allocateRegisters(
    const Module& module,
    const CompilerOptions& options = CompilerOptions{});
[[nodiscard]] AllocationResult allocateRegistersWithSpillRewrite(
    Module& module,
    const CompilerOptions& options = CompilerOptions{},
    int max_rounds = 8);
[[nodiscard]] std::vector<Diagnostic> verifyModule(const Module& module);
[[nodiscard]] OptimizerStats optimizeModule(
    Module& module,
    OptimizationLevel level,
    const CompilerOptions& options);

class CompilerImpl {
public:
    CompilerImpl(ModuleAst ast, CompilerOptions options)
        : ast_(std::move(ast)), options_(options) {
        layout_table_ = buildLayoutTable(ast_, diagnostics_);
        
        // Build module-level constants
        for (const auto& const_decl : ast_.consts) {
            long long val = 0;
            TypeRef type;
            std::vector<Diagnostic> const_diags;
            if (evalConstantExpr(const_decl.expr, module_constants_, val, type, false, const_diags)) {
                module_constants_[const_decl.name] = ConstantInfo{val, type};
            } else {
                // If evaluation failed, report it
                diagnostics_.insert(diagnostics_.end(), const_diags.begin(), const_diags.end());
            }
        }
        
        for (const auto& fn : ast_.functions) {
            function_returns_[fn.name] = fn.return_type;
            for (const auto& param : fn.params) function_params_[fn.name].push_back(param.second);
        }
        auto inferenceDiagnostics = inferModuleTypes(ast_, layout_table_, instantiated_functions_);
        diagnostics_.insert(diagnostics_.end(),
                            inferenceDiagnostics.begin(),
                            inferenceDiagnostics.end());
    }

    [[nodiscard]] CompileResult compile() {
        CompileResult result;
        result.ssa_module.name = ast_.name;
        result.typed_ast = std::make_shared<ModuleAst>(ast_);
        result.layout_table = layout_table_;

        // Populate typed_ast with instantiated functions
        for (const auto& pair : instantiated_functions_) {
            result.typed_ast->functions.push_back(pair.second);
        }

        // Register function returns and params for instantiated functions
        for (const auto& pair : instantiated_functions_) {
            const auto& fn = pair.second;
            function_returns_[fn.name] = fn.return_type;
            function_params_[fn.name].clear();
            for (const auto& param : fn.params) {
                function_params_[fn.name].push_back(param.second);
            }
        }

        std::vector<const FunctionAst*> compile_order;

        // Compile main if it's non-generic
        for (const auto& fn : ast_.functions) {
            if (fn.width_params.empty() && fn.name == "main") {
                compile_order.push_back(&fn);
            }
        }

        // Compile other non-generic functions
        for (const auto& fn : ast_.functions) {
            if (fn.width_params.empty() && fn.name != "main") {
                compile_order.push_back(&fn);
            }
        }

        // Compile all instantiated functions
        for (const auto& pair : instantiated_functions_) {
            compile_order.push_back(&pair.second);
        }

        Module dry_module;
        dry_module.name = ast_.name;
        std::map<std::string, int> value_starts;
        int next_module_value = 1;
        for (const FunctionAst* fn : compile_order) {
            value_starts[fn->name] = next_module_value;
            Function dry = compileFunctionDry(*fn, next_module_value);
            next_module_value = dry.ir_value_ceiling;
            dry_module.functions.push_back(std::move(dry));
        }

        // Keep allocation keyed to the address IR until target emission is
        // fully IR-driven.  The optimized copy is exposed and verified now,
        // but must not silently remove values still consumed by the legacy
        // target replay.
        Module optimized_module;
        optimized_module.name = dry_module.name;
        auto accumulateOptimizerStats = [](
            OptimizerStats& destination,
            const OptimizerStats& source) {
            destination.mem2reg_promotions += source.mem2reg_promotions;
            destination.sccp_constants += source.sccp_constants;
            destination.constant_folds += source.constant_folds;
            destination.copy_props += source.copy_props;
            destination.strength_reductions += source.strength_reductions;
            destination.gvn_hits += source.gvn_hits;
            destination.cse_hits += source.cse_hits;
            destination.dead_instrs += source.dead_instrs;
            destination.branch_simplifications +=
                source.branch_simplifications;
            destination.licm_hoists += source.licm_hoists;
            destination.induction_simplifications +=
                source.induction_simplifications;
            destination.tsel_conversions += source.tsel_conversions;
            destination.swaps += source.swaps;
        };
        int ssa_admitted_functions = 0;
        int cfg_fallback_functions = 0;
        for (const Function& source_function : dry_module.functions) {
            Module candidate;
            candidate.name = dry_module.name;
            candidate.functions.push_back(source_function);
            OptimizerStats candidate_stats = optimizeModule(
                candidate, options_.optimization, options_);
            const auto candidate_diagnostics = verifyModule(candidate);
            if (candidate_diagnostics.empty()) {
                if (candidate.functions.front().cfg_complete)
                    ++ssa_admitted_functions;
                optimized_module.functions.push_back(
                    std::move(candidate.functions.front()));
                accumulateOptimizerStats(
                    result.optimizer_stats, candidate_stats);
                continue;
            }

            // A structurally incomplete frontend CFG must not enter mem2reg or
            // global SSA transforms. Keep it explicit and run only the
            // conservative block-local portfolio until its builder is fixed.
            Module fallback;
            fallback.name = dry_module.name;
            fallback.functions.push_back(source_function);
            fallback.functions.front().cfg_complete = false;
            const OptimizerStats fallback_stats = optimizeModule(
                fallback, options_.optimization, options_);
            const auto fallback_diagnostics = verifyModule(fallback);
            diagnostics_.insert(
                diagnostics_.end(),
                fallback_diagnostics.begin(),
                fallback_diagnostics.end());
            optimized_module.functions.push_back(
                std::move(fallback.functions.front()));
            accumulateOptimizerStats(
                result.optimizer_stats, fallback_stats);
            ++cfg_fallback_functions;
        }
        auto verifier_diagnostics = verifyModule(optimized_module);
        diagnostics_.insert(diagnostics_.end(),
                            verifier_diagnostics.begin(),
                            verifier_diagnostics.end());
        AllocationResult allocation = allocateRegisters(dry_module, options_);
        diagnostics_.insert(diagnostics_.end(),
                            allocation.diagnostics.begin(),
                            allocation.diagnostics.end());

        int ir_emitted_functions = 0;
        int target_replay_functions = 0;
        int target_critical_edges_split = 0;
        int target_spill_rewrite_rounds = 0;
        int target_spill_loads = 0;
        int target_spill_stores = 0;
        std::vector<std::string> ir_emitted_function_names;
        std::vector<std::string> target_replay_function_names;
        std::map<std::string, const FunctionAst*> ast_by_name;
        for (const FunctionAst* fn : compile_order) {
            ast_by_name[fn->name] = fn;
        }
        for (std::size_t function_index = 0;
             function_index < optimized_module.functions.size();
             ++function_index) {
            const Function& function = optimized_module.functions[function_index];
            Function target_function = function;
            target_critical_edges_split +=
                splitCriticalPhiEdges(target_function);
            Module allocation_unit;
            allocation_unit.name = optimized_module.name;
            allocation_unit.functions.push_back(
                target_function);
            const AllocationResult ir_allocation =
                allocateRegistersWithSpillRewrite(
                    allocation_unit, options_, 8);
            target_spill_rewrite_rounds +=
                ir_allocation.spill_rewrite_rounds;
            target_spill_loads +=
                ir_allocation.spill_loads;
            target_spill_stores +=
                ir_allocation.spill_stores;
            std::string ir_assembly;
            if (ir_allocation.success &&
                emitScalarSsaFunction(
                    allocation_unit.functions.front(),
                    ir_allocation,
                    ir_assembly)) {
                result.object.symbols[function.name] = 0;
                result.object.function_order.push_back(function.name);
                std::set<std::string> refs;
                for (const auto& block : function.blocks) {
                    for (const auto& instr : block.instructions) {
                        if (instr.opcode == InstrOpcode::Call &&
                            !instr.symbol.empty()) {
                            refs.insert(instr.symbol);
                        }
                    }
                }
                result.object.function_refs[function.name] = std::move(refs);
                result.object.function_sections[function.name] =
                    std::move(ir_assembly);
                ++ir_emitted_functions;
                ir_emitted_function_names.push_back(
                    function.name);
            } else {
                const auto ast_it = ast_by_name.find(function.name);
                if (!options_.allow_ast_replay) {
                    diagnostics_.push_back(Diagnostic{
                        DiagnosticSeverity::Error,
                        "optimized SSA target lowering failed for '" +
                            function.name +
                            "' and AST replay is disabled",
                        SourceSpan{ast_.name, 1, 1, 1}});
                } else if (ast_it != ast_by_name.end() &&
                           function_index < dry_module.functions.size()) {
                    // AST emission is now a narrowly scoped compatibility
                    // fallback for a target-lowering rejection.  Directly
                    // lowerable functions never enter this path, so their
                    // object sections are produced solely from optimized SSA.
                    compileFunctionReal(
                        *ast_it->second,
                        result,
                        allocation,
                        dry_module.functions[function_index],
                        value_starts[function.name]);
                }
                ++target_replay_functions;
                target_replay_function_names.push_back(function.name);
            }
        }
        result.assembly.clear();
        for (const std::string& function_name :
             result.object.function_order) {
            const auto section =
                result.object.function_sections.find(function_name);
            if (section != result.object.function_sections.end())
                result.assembly += section->second;
        }

        result.ssa_module = dry_module;
        result.optimized_module = optimized_module;
        result.allocation = allocation;
        result.success = diagnostics_.empty();
        result.diagnostics = diagnostics_;
        result.ssa_module.diagnostics = diagnostics_;
        result.object.name = ast_.name;
        // The object-facing SSA artifact is the module that actually feeds
        // target lowering.  Keep the pre-optimization dry module available as
        // `CompileResult.ssa_module` for diagnostics, but never publish it as
        // the code-generation source of an object.
        result.object.ssa = result.optimized_module;
        result.object.assembly = result.assembly;
        result.object.metadata["phase"] = "ir-transition-v2";
        result.object.metadata["pipeline"] =
            "typed-ast,address-cfg-ir,verify,optimize,allocate,target-ir-with-explicit-replay-fallback";
        result.object.metadata["object.ssa_is_optimized"] = "true";
        result.object.metadata["target.ast_replay_allowed"] =
            options_.allow_ast_replay ? "true" : "false";
        result.object.metadata["ssa.admitted_functions"] =
            std::to_string(ssa_admitted_functions);
        result.object.metadata["ssa.cfg_fallback_functions"] =
            std::to_string(cfg_fallback_functions);
        result.object.metadata["target.ir_emitted_functions"] =
            std::to_string(ir_emitted_functions);
        result.object.metadata["target.ast_replay_functions"] =
            std::to_string(target_replay_functions);
        result.object.metadata["target.memory_alias_model"] =
            "ordered-effects-with-call-carrier-proof";
        result.object.metadata["target.vector_lowering"] =
            "fail-closed-no-authoritative-call-abi";
        result.object.metadata["target.critical_edges_split"] =
            std::to_string(target_critical_edges_split);
        result.object.metadata[
            "target.spill_rewrite_rounds"] =
            std::to_string(target_spill_rewrite_rounds);
        result.object.metadata["target.spill_loads"] =
            std::to_string(target_spill_loads);
        result.object.metadata["target.spill_stores"] =
            std::to_string(target_spill_stores);
        std::ostringstream ir_emitted_names;
        for (std::size_t index = 0;
             index < ir_emitted_function_names.size(); ++index) {
            if (index != 0) ir_emitted_names << ",";
            ir_emitted_names
                << ir_emitted_function_names[index];
        }
        result.object.metadata[
            "target.ir_emitted_function_names"] =
            ir_emitted_names.str();
        std::ostringstream replay_names;
        for (std::size_t index = 0;
             index < target_replay_function_names.size(); ++index) {
            if (index != 0) replay_names << ",";
            replay_names << target_replay_function_names[index];
        }
        result.object.metadata[
            "target.ast_replay_function_names"] =
            replay_names.str();
        result.object.metadata["packing.9trit"] = "reserved";
        return result;
    }

private:
    ModuleAst ast_;
    CompilerOptions options_;
    LayoutTable layout_table_;
    std::vector<Diagnostic> diagnostics_;
    std::map<std::string, TypeRef> function_returns_;
    std::map<std::string, std::vector<TypeRef>> function_params_;
    std::map<std::string, FunctionAst> instantiated_functions_;
    std::set<std::string> compiled_functions_;
    ConstantTable module_constants_;  // module-level const values
    std::map<std::string, int> function_call_scratch_areas_;

    std::vector<int> getCalleeSavedUsed(const AllocationResult& allocation, const Function& fn) {
        std::set<int> used;
        for (const auto& block : fn.blocks) {
            for (const auto& instr : block.instructions) {
                if (instr.def < 0) continue;
                auto it = allocation.scalar_registers.find(instr.def);
                if (it == allocation.scalar_registers.end()) continue;
                int reg = it->second;
                if (reg >= 1 && reg <= 12) {
                    used.insert(reg);
                    if (usesWideT50Pair(instr.type) && reg + 1 <= 12)
                        used.insert(reg + 1);
                }
            }
        }
        return std::vector<int>(used.begin(), used.end());
    }

    [[nodiscard]] bool emitScalarSsaFunction(
        const Function& function,
        const AllocationResult& allocation,
        std::string& assembly) {
        if (!function.cfg_complete || function.blocks.empty()) {
            return false;
        }

        std::set<ValueId> frame_indices;
        std::set<ValueId> stack_addresses;
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Alloca &&
                    instr.def >= 0) {
                    frame_indices.insert(instr.def);
                    stack_addresses.insert(instr.def);
                }
            }
        }
        bool address_changed = true;
        while (address_changed) {
            address_changed = false;
            for (const BasicBlock& block : function.blocks) {
                for (const Instr& instr :
                     block.instructions) {
                    if (instr.def < 0 ||
                        stack_addresses.count(instr.def)) {
                        continue;
                    }
                    bool derived = false;
                    switch (instr.opcode) {
                        case InstrOpcode::AddrOf:
                        case InstrOpcode::Copy:
                        case InstrOpcode::Cvt:
                        case InstrOpcode::Add:
                        case InstrOpcode::Sub:
                        case InstrOpcode::FieldAddr:
                        case InstrOpcode::IndexAddr:
                            derived = std::any_of(
                                instr.args.begin(),
                                instr.args.end(),
                                [&](ValueId argument) {
                                    return stack_addresses.count(
                                               argument) != 0;
                                });
                            break;
                        case InstrOpcode::Phi:
                            derived =
                                !instr.phi_incoming.empty() &&
                                std::all_of(
                                    instr.phi_incoming.begin(),
                                    instr.phi_incoming.end(),
                                    [&](const auto& incoming) {
                                        return stack_addresses.count(
                                                   incoming.second) != 0;
                                    });
                            break;
                        case InstrOpcode::Tsel:
                            derived = instr.args.size() == 4 &&
                                stack_addresses.count(
                                    instr.args[1]) &&
                                stack_addresses.count(
                                    instr.args[2]) &&
                                stack_addresses.count(
                                    instr.args[3]);
                            break;
                        case InstrOpcode::Load:
                            if (instr.args.size() == 1 &&
                                stack_addresses.count(
                                    instr.args[0])) {
                                bool saw_store = false;
                                bool all_stack = true;
                                for (const BasicBlock& source_block :
                                     function.blocks) {
                                    for (const Instr& store :
                                         source_block.instructions) {
                                        if (store.opcode !=
                                                InstrOpcode::Store ||
                                            store.args.size() != 2 ||
                                            store.args[0] !=
                                                instr.args[0]) {
                                            continue;
                                        }
                                        saw_store = true;
                                        if (!stack_addresses.count(
                                                store.args[1])) {
                                            all_stack = false;
                                        }
                                    }
                                }
                                derived =
                                    saw_store && all_stack;
                            }
                            break;
                        default:
                            break;
                    }
                    if (derived) {
                        stack_addresses.insert(instr.def);
                        address_changed = true;
                    }
                }
            }
        }

        std::map<ValueId, TypeRef> value_types;
        for (const BasicBlock& block : function.blocks) {
            if (block.terminator.kind == TerminatorKind::None ||
                block.terminator.kind == TerminatorKind::Halt) {
                return false;
            }
            for (const Instr& instr : block.instructions) {
                if (instr.def >= 0)
                    value_types[instr.def] = instr.type;
                switch (instr.opcode) {
                    case InstrOpcode::Alloca:
                    case InstrOpcode::Param:
                    case InstrOpcode::Const:
                    case InstrOpcode::Copy:
                    case InstrOpcode::Add:
                    case InstrOpcode::Sub:
                    case InstrOpcode::Mul:
                    case InstrOpcode::Div:
                    case InstrOpcode::Tmod:
                    case InstrOpcode::Cvt:
                    case InstrOpcode::Cmp:
                    case InstrOpcode::Tsel:
                    case InstrOpcode::Phi:
                    case InstrOpcode::FieldAddr:
                    case InstrOpcode::IndexAddr:
                    case InstrOpcode::AddrOf:
                    case InstrOpcode::Deref:
                    case InstrOpcode::Load:
                    case InstrOpcode::Store:
                    case InstrOpcode::SpillLoad:
                    case InstrOpcode::SpillStore:
                    case InstrOpcode::Call:
                    case InstrOpcode::Syscall:
                    case InstrOpcode::Wait:
                    case InstrOpcode::TlbInv:
                    case InstrOpcode::Fence:
                    case InstrOpcode::Tldr:
                    case InstrOpcode::Tstr:
                    case InstrOpcode::Csrr:
                    case InstrOpcode::Csrw:
                    case InstrOpcode::Csrrw:
                    case InstrOpcode::Swap:
                    case InstrOpcode::Ret:
                    case InstrOpcode::Nop:
                        break;
                    default:
                        return false;
                }
                // Structs and arrays are represented in target IR by scalar
                // frame addresses and word-wise memory operations. Reject
                // only values that still require a non-scalar register class
                // or ownership lowering here.
                if (instr.type.kind == TypeKind::Vector ||
                    instr.type.kind == TypeKind::Owned ||
                    instr.type.kind == TypeKind::Shared) {
                    return false;
                }
            }
        }
        auto registerFor = [&](ValueId value,
                               int& reg) {
            if (value < 0) {
                reg = 0;
                return true;
            }
            const auto found =
                allocation.scalar_registers.find(value);
            if (found == allocation.scalar_registers.end())
                return false;
            reg = found->second;
            return true;
        };
        auto valueType = [&](ValueId value) {
            const auto found = value_types.find(value);
            return found == value_types.end()
                ? TypeRef::numeric(ir::Type::T40)
                : found->second;
        };
        auto regName = [](int reg) {
            return "r" + std::to_string(reg);
        };

        const ControlFlowGraph cfg =
            buildControlFlowGraph(function);
        if (!cfg.invalid_targets.empty()) return false;
        // Memory operations are explicit in the address-based IR. Local
        // allocas are materialized through frame_offsets below, while
        // validated raw pointers and aggregate parameters are ordinary
        // register addresses. Preserve both classes directly, including
        // loop-carried stack state, instead of falling back to AST replay.
        const bool has_external_memory = std::any_of(
            function.blocks.begin(), function.blocks.end(),
            [&](const BasicBlock& block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const Instr& instr) {
                        return (instr.opcode == InstrOpcode::Load ||
                                instr.opcode == InstrOpcode::Store ||
                                instr.opcode == InstrOpcode::Deref ||
                                instr.opcode == InstrOpcode::Swap) &&
                               !instr.args.empty() &&
                               (instr.opcode == InstrOpcode::Swap
                                    ? std::any_of(
                                          instr.args.begin(),
                                          instr.args.end(),
                                          [&](ValueId address) {
                                              return !stack_addresses.count(
                                                  address);
                                          })
                                    : !stack_addresses.count(instr.args[0]));
                    });
            });
        std::set<ValueId> external_address_values;
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Load ||
                    instr.opcode == InstrOpcode::Store ||
                    instr.opcode == InstrOpcode::Deref) {
                    if (!instr.args.empty() &&
                        !stack_addresses.count(instr.args[0])) {
                        external_address_values.insert(instr.args[0]);
                    }
                } else if (instr.opcode == InstrOpcode::Swap) {
                    for (ValueId address : instr.args) {
                        if (!stack_addresses.count(address))
                            external_address_values.insert(address);
                    }
                }
            }
        }
        // Preserve the carrier set through address arithmetic and merges. Raw
        // unsafe pointers are represented as numeric values in the frontend,
        // so a type-only pointer test would miss them.
        bool external_address_changed = true;
        while (external_address_changed) {
            external_address_changed = false;
            for (const BasicBlock& block : function.blocks) {
                for (const Instr& instr : block.instructions) {
                    if (instr.def < 0 ||
                        external_address_values.count(instr.def)) {
                        continue;
                    }
                    bool derived = false;
                    switch (instr.opcode) {
                        case InstrOpcode::AddrOf:
                        case InstrOpcode::Copy:
                        case InstrOpcode::Cvt:
                        case InstrOpcode::Add:
                        case InstrOpcode::Sub:
                        case InstrOpcode::FieldAddr:
                        case InstrOpcode::IndexAddr:
                            derived = std::any_of(
                                instr.args.begin(), instr.args.end(),
                                [&](ValueId arg) {
                                    return external_address_values.count(arg) != 0;
                                });
                            break;
                        case InstrOpcode::Phi:
                            derived = std::any_of(
                                instr.phi_incoming.begin(),
                                instr.phi_incoming.end(),
                                [&](const auto& incoming) {
                                    return external_address_values.count(
                                               incoming.second) != 0;
                                });
                            break;
                        case InstrOpcode::Tsel:
                            derived = std::any_of(
                                instr.args.begin() +
                                    std::min<std::size_t>(1, instr.args.size()),
                                instr.args.end(),
                                [&](ValueId arg) {
                                    return external_address_values.count(arg) != 0;
                                });
                            break;
                        default:
                            break;
                    }
                    if (derived) {
                        external_address_values.insert(instr.def);
                        external_address_changed = true;
                    }
                }
            }
        }
        const bool has_call_or_syscall = std::any_of(
            function.blocks.begin(), function.blocks.end(),
            [](const BasicBlock& block) {
                return std::any_of(
                    block.instructions.begin(), block.instructions.end(),
                    [](const Instr& instr) {
                        return instr.opcode == InstrOpcode::Call ||
                               instr.opcode == InstrOpcode::CallR ||
                               instr.opcode == InstrOpcode::Syscall;
                    });
            });
        // Loads and stores are effectful IR nodes and are therefore never
        // CSE'd or speculated. Unknown aliases remain ordered memory effects;
        // the only additional proof required at a call boundary is that an
        // address carrier live after the call is in a callee-saved register.
        // This replaces the old operation-count replay heuristic with an
        // explicit memory-SSA/call-clobber check.
        if (has_external_memory && has_call_or_syscall) {
            const BlockLiveness liveness = computeBlockLiveness(function, cfg);
            auto calleeSavedScalar = [&](ValueId value) {
                const auto found = allocation.scalar_registers.find(value);
                if (found == allocation.scalar_registers.end()) return false;
                return found->second >= 1 && found->second <= 12;
            };
            for (const BasicBlock& block : function.blocks) {
                std::set<ValueId> live =
                    liveness.live_out.count(block.name)
                        ? liveness.live_out.at(block.name)
                        : std::set<ValueId>{};
                if (block.terminator.condition >= 0)
                    live.insert(block.terminator.condition);
                for (auto it = block.instructions.rbegin();
                     it != block.instructions.rend(); ++it) {
                    const Instr& instr = *it;
                    if (instr.opcode == InstrOpcode::Call ||
                        instr.opcode == InstrOpcode::CallR ||
                        instr.opcode == InstrOpcode::Syscall) {
                        for (ValueId value : live) {
                            if (external_address_values.count(value) != 0 &&
                                !calleeSavedScalar(value)) {
                                // Spill rewriting should have materialized a
                                // fresh load around the call. A remaining
                                // spilled carrier has no safe target form.
                                return false;
                            }
                        }
                    }
                    if (instr.def >= 0) live.erase(instr.def);
                    for (ValueId arg : instr.args)
                        if (arg >= 0) live.insert(arg);
                }
            }
        }
        struct EdgeMove {
            int destination = -1;
            int source = -1;
            TypeRef type = TypeRef::unknown();
            bool source_is_scratch = false;
        };
        std::map<
            std::pair<std::string, std::string>,
            std::vector<EdgeMove>> edge_moves;
        for (const BasicBlock& block : function.blocks) {
            bool past_phis = false;
            for (const Instr& instr : block.instructions) {
                if (instr.opcode != InstrOpcode::Phi) {
                    past_phis = true;
                    continue;
                }
                if (past_phis || instr.def < 0) return false;
                int destination = -1;
                if (!registerFor(instr.def, destination))
                    return false;
                for (const auto& incoming :
                     instr.phi_incoming) {
                    int source = -1;
                    if (!registerFor(incoming.second, source) ||
                        !cfg.predecessors.at(block.name).count(
                            incoming.first)) {
                        return false;
                    }
                    if (destination != source) {
                        edge_moves[
                            {incoming.first, block.name}]
                            .push_back(
                                EdgeMove{
                                    destination,
                                    source,
                                    instr.type,
                                    false});
                    }
                }
            }
        }
        for (const Instr& instr :
             function.blocks.front().instructions) {
            if (instr.opcode != InstrOpcode::Param)
                continue;
            int destination = -1;
            if (!registerFor(instr.def, destination))
                return false;
            const int width =
                usesWideT50Pair(instr.type) ? 2 : 1;
            if (instr.aux < 0) {
                return false;
            }
            if (instr.aux + width > kRegisterArgCount) continue;
            const int source = 13 + instr.aux;
            if (destination != source) {
                edge_moves[
                    {"$params",
                     function.blocks.front().name}]
                    .push_back(
                        EdgeMove{
                            destination,
                            source,
                            instr.type,
                            false});
            }
        }
        auto callMoveKey = [](
            const BasicBlock& block,
            const Instr& instr) {
            return std::make_pair(
                std::string("$call"),
                block.name + ":" +
                    std::to_string(instr.def));
        };
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode != InstrOpcode::Call)
                    continue;
                if (instr.symbol.empty() || instr.def < 0)
                    return false;
                int argument_word = 0;
                for (ValueId argument : instr.args) {
                    const auto type = value_types.find(argument);
                    int source = -1;
                    if (type == value_types.end() ||
                        !registerFor(argument, source) ||
                        type->second.kind == TypeKind::Vector ||
                        type->second.kind == TypeKind::Struct ||
                        type->second.kind == TypeKind::Array ||
                        type->second.kind == TypeKind::Owned ||
                        type->second.kind == TypeKind::Shared) {
                        return false;
                    }
                    const int width =
                        usesWideT50Pair(type->second) ? 2 : 1;
                    if (argument_word + width <= kRegisterArgCount) {
                        const int destination = 13 + argument_word;
                        if (destination != source) {
                            edge_moves[callMoveKey(block, instr)]
                                .push_back(
                                    EdgeMove{
                                        destination,
                                        source,
                                        type->second,
                                        false});
                        }
                    }
                    argument_word += width;
                }
            }
        }
        auto syscallMoveKey = [](
            const BasicBlock& block,
            const Instr& instr) {
            return std::make_pair(
                std::string("$syscall"),
                block.name + ":" +
                    std::to_string(instr.def));
        };
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode != InstrOpcode::Syscall)
                    continue;
                if (instr.def < 0 || instr.args.size() > 4)
                    return false;
                for (std::size_t index = 0;
                     index < instr.args.size(); ++index) {
                    const ValueId argument =
                        instr.args[index];
                    const auto type = value_types.find(argument);
                    int source = -1;
                    if (type == value_types.end() ||
                        !registerFor(argument, source) ||
                        usesWideT50Pair(type->second) ||
                        type->second.kind == TypeKind::Vector ||
                        type->second.kind == TypeKind::Struct ||
                        type->second.kind == TypeKind::Array ||
                        type->second.kind == TypeKind::Owned ||
                        type->second.kind == TypeKind::Shared) {
                        return false;
                    }
                    const int destination =
                        13 + static_cast<int>(index);
                    if (destination != source) {
                        edge_moves[
                            syscallMoveKey(block, instr)]
                            .push_back(
                                EdgeMove{
                                    destination,
                                    source,
                                    type->second,
                                    false});
                    }
                }
            }
        }

        const std::vector<int> callee_saved =
            getCalleeSavedUsed(allocation, function);
        const int parallel_copy_scratch =
            1 + static_cast<int>(callee_saved.size());
        std::map<ValueId, int> frame_offsets;
        int frame_cursor = parallel_copy_scratch + 2;
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode != InstrOpcode::Alloca ||
                    instr.def < 0) {
                    continue;
                }
                frame_offsets[instr.def] = frame_cursor;
                frame_cursor += std::max(1, instr.aux);
            }
        }
        const int spill_base = frame_cursor;
        int spill_extent = 0;
        for (const BasicBlock& block : function.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::SpillLoad) {
                    spill_extent = std::max(
                        spill_extent,
                        instr.aux +
                            (usesWideT50Pair(instr.type)
                                 ? 2
                                 : 1));
                } else if (
                    instr.opcode ==
                        InstrOpcode::SpillStore &&
                    !instr.args.empty()) {
                    spill_extent = std::max(
                        spill_extent,
                        instr.aux +
                            (usesWideT50Pair(
                                 valueType(instr.args[0]))
                                 ? 2
                                 : 1));
                }
            }
        }
        const int frame_words = align9(
            spill_base + spill_extent);
        std::ostringstream out;
        out << function.name << ":\n";
        out << "    mov.t40 r24, " << frame_words << "\n";
        out << "    sub.t40 sp, sp, r24\n";
        out << "    store lr, sp, 0\n";
        for (std::size_t index = 0;
             index < callee_saved.size(); ++index) {
            out << "    store r" << callee_saved[index]
                << ", sp, " << (index + 1) << "\n";
        }
        auto widthOf = [](const TypeRef& type) {
            return usesWideT50Pair(type) ? 2 : 1;
        };
        // Parameters beyond r13-r18 are loaded from the caller's temporary
        // outgoing area. The caller subtracts that area immediately before
        // the call, so after this prologue subtracts its own frame the
        // incoming words are at [sp + frame_words + stack_word].
        int parameter_register_word = 0;
        int parameter_stack_word = 0;
        for (const Instr& parameter : function.blocks.front().instructions) {
            if (parameter.opcode != InstrOpcode::Param) continue;
            const int width = widthOf(parameter.type);
            if (parameter_register_word + width <= kRegisterArgCount) {
                parameter_register_word += width;
                continue;
            }
            int destination = -1;
            if (!registerFor(parameter.def, destination)) return false;
            const int scratch = width == 2 ? 23 : 24;
            out << "    " << scalarMemoryMnemonic("load", parameter.type)
                << " r" << scratch << ", sp, "
                << (frame_words + parameter_stack_word) << "\n";
            if (destination != scratch) {
                out << "    copy" << (width == 2 ? ".t50" : "")
                    << " " << regName(destination) << ", r" << scratch
                    << "\n";
            }
            parameter_stack_word += width;
        }
        auto rangesOverlap = [](
            int lhs, int lhs_width,
            int rhs, int rhs_width) {
            return lhs < rhs + rhs_width &&
                   rhs < lhs + lhs_width;
        };
        auto emitParallelMoveSet =
            [&](std::vector<EdgeMove> pending) {
            bool scratch_in_use = false;
            int scratch_users = 0;
            while (!pending.empty()) {
                pending.erase(
                    std::remove_if(
                        pending.begin(), pending.end(),
                        [&](const EdgeMove& move) {
                            return !move.source_is_scratch &&
                                   move.destination ==
                                       move.source;
                        }),
                    pending.end());
                if (pending.empty()) break;

                std::size_t selected = pending.size();
                for (std::size_t index = 0;
                     index < pending.size(); ++index) {
                    const EdgeMove& candidate =
                        pending[index];
                    bool destination_is_source = false;
                    for (std::size_t other = 0;
                         other < pending.size(); ++other) {
                        if (index == other ||
                            pending[other]
                                .source_is_scratch) {
                            continue;
                        }
                        if (rangesOverlap(
                                candidate.destination,
                                widthOf(candidate.type),
                                pending[other].source,
                                widthOf(pending[other].type))) {
                            destination_is_source = true;
                            break;
                        }
                    }
                    if (!destination_is_source) {
                        selected = index;
                        break;
                    }
                }

                if (selected == pending.size()) {
                    if (scratch_in_use) return false;
                    const EdgeMove& cycle = pending.front();
                    out << "    "
                        << scalarMemoryMnemonic(
                               "store", cycle.type)
                        << " " << regName(cycle.destination)
                        << ", sp, "
                        << parallel_copy_scratch << "\n";
                    const int saved_width =
                        widthOf(cycle.type);
                    for (EdgeMove& move : pending) {
                        if (move.source_is_scratch)
                            continue;
                        if (move.source ==
                                cycle.destination &&
                            widthOf(move.type) ==
                                saved_width) {
                            move.source_is_scratch = true;
                            ++scratch_users;
                        } else if (rangesOverlap(
                                       move.source,
                                       widthOf(move.type),
                                       cycle.destination,
                                       saved_width)) {
                            return false;
                        }
                    }
                    if (scratch_users == 0) return false;
                    scratch_in_use = true;
                    continue;
                }

                EdgeMove move = pending[selected];
                pending.erase(
                    pending.begin() +
                    static_cast<std::ptrdiff_t>(
                        selected));
                if (move.source_is_scratch) {
                    out << "    "
                        << scalarMemoryMnemonic(
                               "load", move.type)
                        << " " << regName(move.destination)
                        << ", sp, "
                        << parallel_copy_scratch << "\n";
                    --scratch_users;
                    if (scratch_users == 0)
                        scratch_in_use = false;
                } else {
                    out << "    copy"
                        << (widthOf(move.type) == 2
                                ? ".t50"
                                : "")
                        << " " << regName(move.destination)
                        << ", " << regName(move.source)
                        << "\n";
                }
            }
            return !scratch_in_use;
        };
        auto emitParallelCopies =
            [&](const std::string& predecessor,
                const std::string& successor) {
            return emitParallelMoveSet(
                edge_moves[{predecessor, successor}]);
        };
        if (!emitParallelCopies(
                "$params",
                function.blocks.front().name)) {
            return false;
        }

        bool emitted_return = false;
        std::vector<
            std::pair<
                std::pair<std::string, std::string>,
                std::string>> synthetic_edges;
        for (std::size_t block_index = 0;
             block_index < function.blocks.size(); ++block_index) {
            const BasicBlock& block =
                function.blocks[block_index];
            out << block.name << ":\n";
            for (const Instr& instr : block.instructions) {
            int destination = -1;
            if (instr.def >= 0 &&
                instr.opcode != InstrOpcode::Alloca &&
                !registerFor(instr.def, destination)) {
                return false;
            }
            auto argumentRegister = [&](std::size_t index,
                                         int& reg) {
                if (index >= instr.args.size())
                    return false;
                const ValueId value = instr.args[index];
                const auto frame =
                    frame_offsets.find(value);
                if (frame != frame_offsets.end()) {
                    out << "    mov.t40 r24, "
                        << frame->second << "\n";
                    out << "    add.t40 r24, sp, r24\n";
                    reg = 24;
                    return true;
                }
                return registerFor(value, reg);
            };
            const std::string suffix =
                instr.type.kind == TypeKind::Trit
                    ? "t1"
                    : std::string(ir::suffix(instr.type.scalar));
            switch (instr.opcode) {
                case InstrOpcode::Alloca:
                    // Virtual frame index: materialized only at a use.
                    break;
                case InstrOpcode::Param: {
                    // Parameters are assigned as one parallel ABI copy set
                    // before the entry block so overlapping r13-r18 sources
                    // cannot be clobbered by an earlier destination.
                    break;
                }
                case InstrOpcode::Const:
                    if (instr.imm < 0) {
                        out << "    mov." << suffix << " "
                            << regName(destination) << ", "
                            << -instr.imm << "\n";
                        out << "    neg." << suffix << " "
                            << regName(destination) << ", "
                            << regName(destination) << "\n";
                    } else {
                        out << "    mov." << suffix << " "
                            << regName(destination) << ", "
                            << instr.imm << "\n";
                    }
                    break;
                case InstrOpcode::Copy: {
                    int source = -1;
                    if (!argumentRegister(0, source)) return false;
                    if (destination != source) {
                        out << "    copy"
                            << (usesWideT50Pair(instr.type)
                                    ? ".t50"
                                    : "")
                            << " " << regName(destination)
                            << ", " << regName(source) << "\n";
                    }
                    break;
                }
                case InstrOpcode::AddrOf: {
                    int source = -1;
                    if (instr.args.size() != 1 ||
                        !argumentRegister(0, source)) {
                        return false;
                    }
                    if (destination != source) {
                        out << "    copy "
                            << regName(destination)
                            << ", " << regName(source)
                            << "\n";
                    }
                    break;
                }
                case InstrOpcode::FieldAddr:
                case InstrOpcode::IndexAddr: {
                    // Address arithmetic is emitted by the preceding Add
                    // (or preserved as an identity for a zero offset). The
                    // address node itself is an SSA alias so later loads and
                    // stores retain an explicit memory dependency.
                    int source = -1;
                    if (instr.args.size() != 1 ||
                        !argumentRegister(0, source)) {
                        return false;
                    }
                    if (destination != source) {
                        out << "    copy "
                            << regName(destination)
                            << ", " << regName(source)
                            << "\n";
                    }
                    break;
                }
                case InstrOpcode::Deref:
                case InstrOpcode::Load: {
                    int address = -1;
                    if (instr.args.size() != 1 ||
                        !argumentRegister(0, address)) {
                        return false;
                    }
                    out << "    "
                        << scalarMemoryMnemonic(
                               "load", instr.type)
                        << " " << regName(destination)
                        << ", " << regName(address)
                        << ", 0\n";
                    break;
                }
                case InstrOpcode::Store: {
                    int address = -1;
                    int source = -1;
                    if (instr.args.size() != 2 ||
                        !argumentRegister(0, address) ||
                        !argumentRegister(1, source)) {
                        return false;
                    }
                    out << "    "
                        << scalarMemoryMnemonic(
                               "store",
                               valueType(instr.args[1]))
                        << " " << regName(source)
                        << ", " << regName(address)
                        << ", " << instr.aux << "\n";
                    break;
                }
                case InstrOpcode::Swap: {
                    const bool scalar_type =
                        instr.type.kind == TypeKind::Numeric ||
                        instr.type.kind == TypeKind::Lane ||
                        instr.type.kind == TypeKind::Trit;
                    if (instr.args.size() != 2 || !scalar_type ||
                        usesWideT50Pair(instr.type)) {
                        return false;
                    }
                    auto emitSwapMemory = [&](const char* operation,
                                              ValueId address,
                                              int value_register) {
                        const auto frame = frame_offsets.find(address);
                        if (frame != frame_offsets.end()) {
                            out << "    "
                                << scalarMemoryMnemonic(
                                       operation, instr.type)
                                << " r" << value_register
                                << ", sp, " << frame->second
                                << "\n";
                            return true;
                        }
                        int address_register = -1;
                        if (!registerFor(address, address_register) ||
                            address_register == 13 ||
                            address_register == 14) {
                            return false;
                        }
                        out << "    "
                            << scalarMemoryMnemonic(
                                   operation, instr.type)
                            << " r" << value_register
                            << ", " << regName(address_register)
                            << ", 0\n";
                        return true;
                    };
                    if (!emitSwapMemory(
                            "load", instr.args[0], 13) ||
                        !emitSwapMemory(
                            "load", instr.args[1], 14)) {
                        return false;
                    }
                    out << "    swap r13, r14\n";
                    if (!emitSwapMemory(
                            "store", instr.args[0], 13) ||
                        !emitSwapMemory(
                            "store", instr.args[1], 14)) {
                        return false;
                    }
                    break;
                }
                case InstrOpcode::SpillLoad:
                    if (instr.aux < 0) return false;
                    out << "    "
                        << scalarMemoryMnemonic(
                               "load", instr.type)
                        << " " << regName(destination)
                        << ", sp, "
                        << (spill_base + instr.aux)
                        << "\n";
                    break;
                case InstrOpcode::SpillStore: {
                    int source = -1;
                    if (instr.aux < 0 ||
                        instr.args.size() != 1 ||
                        !argumentRegister(0, source)) {
                        return false;
                    }
                    out << "    "
                        << scalarMemoryMnemonic(
                               "store",
                               valueType(instr.args[0]))
                        << " " << regName(source)
                        << ", sp, "
                        << (spill_base + instr.aux)
                        << "\n";
                    break;
                }
                case InstrOpcode::Add:
                case InstrOpcode::Sub:
                case InstrOpcode::Mul:
                case InstrOpcode::Div:
                case InstrOpcode::Tmod: {
                    int lhs = -1;
                    int rhs = -1;
                    if (!argumentRegister(0, lhs) ||
                        !argumentRegister(1, rhs)) {
                        return false;
                    }
                    const char* mnemonic =
                        instr.opcode == InstrOpcode::Add ? "add" :
                        instr.opcode == InstrOpcode::Sub ? "sub" :
                        instr.opcode == InstrOpcode::Mul ? "mul" :
                        instr.opcode == InstrOpcode::Div ? "div" :
                                                         "tmod";
                    out << "    " << mnemonic << "." << suffix
                        << " " << regName(destination)
                        << ", " << regName(lhs)
                        << ", " << regName(rhs) << "\n";
                    break;
                }
                case InstrOpcode::Cvt: {
                    int source = -1;
                    if (!argumentRegister(0, source)) return false;
                    const TypeRef from = valueType(instr.args[0]);
                    const std::string from_suffix =
                        from.kind == TypeKind::Trit
                            ? "t1"
                            : std::string(ir::suffix(from.scalar));
                    out << "    cvt." << from_suffix << "."
                        << suffix << " " << regName(destination)
                        << ", " << regName(source) << "\n";
                    break;
                }
                case InstrOpcode::Cmp: {
                    int lhs = -1;
                    int rhs = -1;
                    if (!argumentRegister(0, lhs) ||
                        !argumentRegister(1, rhs)) {
                        return false;
                    }
                    const TypeRef operand_type =
                        valueType(instr.args[0]);
                    const std::string operand_suffix =
                        operand_type.kind == TypeKind::Trit
                            ? "t1"
                            : std::string(
                                  ir::suffix(operand_type.scalar));
                    out << "    tcmp." << operand_suffix << " "
                        << regName(destination) << ", "
                        << regName(lhs) << ", " << regName(rhs)
                        << "\n";
                    break;
                }
                case InstrOpcode::Tsel: {
                    if (instr.args.size() != 4) return false;
                    int condition = -1;
                    int negative = -1;
                    int zero = -1;
                    int positive = -1;
                    if (!argumentRegister(0, condition) ||
                        !argumentRegister(1, negative) ||
                        !argumentRegister(2, zero) ||
                        !argumentRegister(3, positive)) {
                        return false;
                    }
                    out << "    tsel " << regName(destination)
                        << ", " << regName(condition)
                        << ", " << regName(negative)
                        << ", " << regName(zero)
                        << ", " << regName(positive) << "\n";
                    break;
                }
                case InstrOpcode::Phi:
                    // Materialized on predecessor edges after allocation.
                    break;
                case InstrOpcode::Wait:
                    if (!instr.args.empty()) return false;
                    out << "    wait\n";
                    break;
                case InstrOpcode::Fence:
                    if (!instr.args.empty() ||
                        !isa::isAtomicOrder(instr.aux)) {
                        return false;
                    }
                    out << "    fence"
                        << ir::memoryOrderSuffix(instr.aux)
                        << "\n";
                    break;
                case InstrOpcode::Tldr: {
                    int address = -1;
                    if (instr.args.size() != 1 ||
                        !isa::isAtomicOrder(instr.aux) ||
                        !argumentRegister(0, address)) {
                        return false;
                    }
                    out << "    tldr"
                        << ir::memoryOrderSuffix(instr.aux)
                        << " " << regName(destination)
                        << ", " << regName(address) << "\n";
                    break;
                }
                case InstrOpcode::Tstr: {
                    int address = -1;
                    int desired = -1;
                    int expected = -1;
                    if (instr.args.size() != 3 ||
                        !isa::isAtomicOrder(instr.aux) ||
                        !argumentRegister(0, address) ||
                        !argumentRegister(1, desired) ||
                        !argumentRegister(2, expected)) {
                        return false;
                    }
                    out << "    tstr"
                        << ir::memoryOrderSuffix(instr.aux)
                        << " " << regName(destination)
                        << ", " << regName(address)
                        << ", " << regName(desired)
                        << ", " << regName(expected) << "\n";
                    break;
                }
                case InstrOpcode::Csrr:
                    if (!instr.args.empty() ||
                        !isa::isValidCSR(instr.aux)) {
                        return false;
                    }
                    out << "    csrr "
                        << regName(destination) << ", "
                        << isa::csrToString(instr.aux)
                        << "\n";
                    break;
                case InstrOpcode::Csrw: {
                    int source = -1;
                    if (instr.args.size() != 1 ||
                        !isa::isValidCSR(instr.aux) ||
                        !argumentRegister(0, source)) {
                        return false;
                    }
                    out << "    csrw "
                        << isa::csrToString(instr.aux)
                        << ", " << regName(source) << "\n";
                    break;
                }
                case InstrOpcode::Csrrw: {
                    int source = -1;
                    if (instr.args.size() != 1 ||
                        !isa::isValidCSR(instr.aux) ||
                        !argumentRegister(0, source)) {
                        return false;
                    }
                    out << "    csrrw "
                        << regName(destination) << ", "
                        << isa::csrToString(instr.aux)
                        << ", " << regName(source) << "\n";
                    break;
                }
                case InstrOpcode::TlbInv: {
                    int address = -1;
                    int asid = -1;
                    int scope = -1;
                    if (instr.args.size() != 3 ||
                        !argumentRegister(0, address) ||
                        !argumentRegister(1, asid) ||
                        !argumentRegister(2, scope)) {
                        return false;
                    }
                    out << "    tlbinv "
                        << regName(address) << ", "
                        << regName(asid) << ", "
                        << regName(scope) << "\n";
                    break;
                }
                case InstrOpcode::Call: {
                    int argument_word = 0;
                    int stack_word_count = 0;
                    for (ValueId argument : instr.args) {
                        const TypeRef type = valueType(argument);
                        const int width = widthOf(type);
                        if (argument_word + width > kRegisterArgCount)
                            stack_word_count += width;
                        argument_word += width;
                    }
                    if (stack_word_count > 0) {
                        out << "    mov.t40 r24, " << stack_word_count << "\n";
                        out << "    sub.t40 sp, sp, r24\n";
                        argument_word = 0;
                        int stack_word = 0;
                        for (ValueId argument : instr.args) {
                            const TypeRef type = valueType(argument);
                            const int width = widthOf(type);
                            int source = -1;
                            if (!registerFor(argument, source)) return false;
                            if (argument_word + width > kRegisterArgCount) {
                                out << "    " << scalarMemoryMnemonic("store", type)
                                    << " r" << source << ", sp, " << stack_word
                                    << "\n";
                                stack_word += width;
                            }
                            argument_word += width;
                        }
                    }
                    if (!emitParallelMoveSet(
                            edge_moves[
                                callMoveKey(block, instr)])) {
                        return false;
                    }
                    out << "    call " << instr.symbol << "\n";
                    if (stack_word_count > 0) {
                        out << "    mov.t40 r24, " << stack_word_count << "\n";
                        out << "    add.t40 sp, sp, r24\n";
                    }
                    if (destination != 13) {
                        out << "    copy"
                            << (usesWideT50Pair(instr.type)
                                    ? ".t50"
                                    : "")
                            << " " << regName(destination)
                            << ", r13\n";
                    }
                    break;
                }
                case InstrOpcode::Syscall: {
                    if (!emitParallelMoveSet(
                            edge_moves[
                                syscallMoveKey(
                                    block, instr)])) {
                        return false;
                    }
                    if ((instr.aux ==
                             runtime::sys_write_int ||
                         instr.aux ==
                             runtime::sys_write_char) &&
                        !instr.args.empty()) {
                        // The standalone VM console oracle still observes
                        // r1, while the architectural/kernel ABI consumes
                        // the canonical first argument in r13.
                        out << "    store r1, sp, "
                            << parallel_copy_scratch
                            << "\n";
                        out << "    copy r1, r13\n";
                    }
                    out << "    syscall " << instr.aux
                        << "\n";
                    if ((instr.aux ==
                             runtime::sys_write_int ||
                         instr.aux ==
                             runtime::sys_write_char) &&
                        !instr.args.empty()) {
                        out << "    load r1, sp, "
                            << parallel_copy_scratch
                            << "\n";
                    }
                    const int result_register =
                        runtimeReturnsPayload(instr.aux)
                            ? 14
                            : 13;
                    if (destination != result_register) {
                        out << "    copy "
                            << regName(destination)
                            << ", r" << result_register
                            << "\n";
                    }
                    break;
                }
                case InstrOpcode::Ret: {
                    if (instr.args.size() > 1) return false;
                    if (!instr.args.empty()) {
                        int source = -1;
                        if (!argumentRegister(0, source))
                            return false;
                        if (source != 13) {
                            out << "    copy"
                                << (usesWideT50Pair(instr.type)
                                        ? ".t50"
                                        : "")
                                << " r13, " << regName(source)
                                << "\n";
                        }
                    }
                    out << "    jmp " << function.name
                        << "_return\n";
                    emitted_return = true;
                    break;
                }
                case InstrOpcode::Nop:
                    break;
                default:
                    return false;
            }
            }
            switch (block.terminator.kind) {
                case TerminatorKind::Return:
                    break;
                case TerminatorKind::Jump:
                    if (!emitParallelCopies(
                            block.name,
                            block.terminator.target)) {
                        return false;
                    }
                    out << "    jmp "
                        << block.terminator.target << "\n";
                    break;
                case TerminatorKind::Branch3: {
                    int condition = -1;
                    if (!registerFor(
                            block.terminator.condition,
                            condition)) {
                        return false;
                    }
                    const auto& successors =
                        cfg.successors.at(block.name);
                    if (successors.size() == 1) {
                        const std::string& successor =
                            *successors.begin();
                        if (!emitParallelCopies(
                                block.name, successor)) {
                            return false;
                        }
                        out << "    jmp " << successor
                            << "\n";
                        break;
                    }
                    std::map<std::string, std::string>
                        branch_targets;
                    for (const std::string& successor :
                         successors) {
                        const auto edge =
                            std::make_pair(
                                block.name, successor);
                        if (edge_moves[edge].empty()) {
                            branch_targets[successor] =
                                successor;
                        } else {
                            branch_targets[successor] =
                                function.name +
                                "_phi_edge_" +
                                std::to_string(
                                    synthetic_edges.size());
                            synthetic_edges.push_back(
                                {edge,
                                 branch_targets[successor]});
                        }
                    }
                    out << "    brn " << regName(condition)
                        << ", "
                        << branch_targets[
                               block.terminator.target_neg]
                        << "\n";
                    out << "    brz " << regName(condition)
                        << ", "
                        << branch_targets[
                               block.terminator.target_zero]
                        << "\n";
                    out << "    jmp "
                        << branch_targets[
                               block.terminator.target_pos]
                        << "\n";
                    break;
                }
                case TerminatorKind::None:
                case TerminatorKind::Halt:
                    return false;
            }
        }
        if (!emitted_return) return false;

        for (const auto& synthetic : synthetic_edges) {
            out << synthetic.second << ":\n";
            if (!emitParallelCopies(
                    synthetic.first.first,
                    synthetic.first.second)) {
                return false;
            }
            out << "    jmp "
                << synthetic.first.second << "\n";
        }
        out << function.name << "_return:\n";
        for (std::size_t index = 0;
             index < callee_saved.size(); ++index) {
            out << "    load r" << callee_saved[index]
                << ", sp, " << (index + 1) << "\n";
        }
        out << "    load lr, sp, 0\n";
        out << "    mov.t40 r24, " << frame_words << "\n";
        out << "    add.t40 sp, sp, r24\n";
        out << "    ret\n";
        assembly = out.str();
        return true;
    }

    Function compileFunctionDry(const FunctionAst& fn, int start_value) {
        FunctionContext dry_ctx;
        dry_ctx.ast = &fn;
        dry_ctx.ir.name = fn.name;
        dry_ctx.ir.cfg_complete = true;
        dry_ctx.ir.params = fn.params;
        dry_ctx.ir.return_type = fn.return_type;
        dry_ctx.ir.blocks.push_back(BasicBlock{fn.name + "_entry", {}, {}});
        dry_ctx.block = &dry_ctx.ir.blocks.back();
        dry_ctx.function_returns = function_returns_;
        dry_ctx.function_params = function_params_;
        dry_ctx.layouts = &layout_table_;
        dry_ctx.options = &options_;
        dry_ctx.diagnostics = &diagnostics_;
        dry_ctx.constants = module_constants_;  // Populate with module-level constants
        dry_ctx.dry_run = true;
        dry_ctx.next_value = start_value;

        collectLocals(fn, dry_ctx);
        materializeLocalAllocas(dry_ctx);
        dry_ctx.return_slot_offset = dry_ctx.next_local_offset;
        dry_ctx.call_arg_slot_base =
            dry_ctx.return_slot_offset +
            std::max(1, typeSizeWords(fn.return_type, layout_table_));
        dry_ctx.frame_words = align9(std::max(
            1,
            dry_ctx.call_arg_slot_base + kCallArgScratchWords * kCallArgScratchAreas));
        emitFunctionPrologue(fn, dry_ctx);
        dry_ctx.scope_vars.push_back({});
        emitStatements(fn.body, dry_ctx);
        emitDefaultReturn(dry_ctx);
        dry_ctx.scope_vars.pop_back();

        function_call_scratch_areas_[fn.name] = dry_ctx.max_call_arg_depth;
        dry_ctx.ir.ir_value_ceiling = dry_ctx.next_value;
        // Admit a frontend function to SSA transforms only when the address
        // CFG already satisfies the same dominance contract required after
        // mem2reg. Complex legacy constructs remain visible as incomplete
        // instead of weakening verification or risking a miscompile.
        Module cfg_probe;
        cfg_probe.name = ast_.name;
        cfg_probe.functions.push_back(dry_ctx.ir);
        if (!verifyModule(cfg_probe).empty()) {
            dry_ctx.ir.cfg_complete = false;
        }
        return dry_ctx.ir;
    }

    void compileFunctionReal(const FunctionAst& fn,
                             CompileResult& result,
                             const AllocationResult& allocation,
                             const Function& allocated_fn,
                             int start_value) {
        FunctionContext ctx;
        ctx.ast = &fn;
        ctx.ir.name = fn.name;
        ctx.ir.params = fn.params;
        ctx.ir.return_type = fn.return_type;
        ctx.ir.blocks.push_back(BasicBlock{fn.name + "_entry", {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.function_returns = function_returns_;
        ctx.function_params = function_params_;
        ctx.layouts = &layout_table_;
        ctx.options = &options_;
        ctx.diagnostics = &diagnostics_;
        ctx.constants = module_constants_;  // Populate with module-level constants
        ctx.dry_run = false;
        ctx.allocation = &allocation;
        ctx.next_value = start_value;

        collectLocals(fn, ctx);
        materializeLocalAllocas(ctx);
        ctx.callee_saved_regs = getCalleeSavedUsed(allocation, allocated_fn);
        ctx.return_slot_offset = ctx.next_local_offset + static_cast<int>(ctx.callee_saved_regs.size());
        ctx.call_arg_slot_base =
            ctx.return_slot_offset +
            std::max(1, typeSizeWords(fn.return_type, layout_table_));
        const auto scratch_it = function_call_scratch_areas_.find(fn.name);
        const int scratch_areas = scratch_it == function_call_scratch_areas_.end()
            ? 0
            : std::min(kCallArgScratchAreas, std::max(0, scratch_it->second));
        ctx.frame_words =
            align9(std::max(1, ctx.call_arg_slot_base + kCallArgScratchWords * scratch_areas));
        emitFunctionPrologue(fn, ctx);
        ctx.scope_vars.push_back({});
        emitStatements(fn.body, ctx);
        emitDefaultReturn(ctx);
        ctx.scope_vars.pop_back();

        result.assembly += ctx.asm_out.str();
        ctx.ir.ir_value_ceiling = ctx.next_value;
        std::set<std::string> refs;
        for (const auto& block : ctx.ir.blocks) {
            for (const auto& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Call && !instr.symbol.empty()) {
                    refs.insert(instr.symbol);
                }
            }
        }
        result.ssa_module.functions.push_back(ctx.ir);
        result.object.symbols[fn.name] = 0;
        result.object.function_order.push_back(fn.name);
        result.object.function_sections[fn.name] = ctx.asm_out.str();
        result.object.function_refs[fn.name] = std::move(refs);
    }

    [[nodiscard]] static int align9(int value) {
        return ((value + 8) / 9) * 9;
    }

    [[nodiscard]] static bool isAggregateType(const TypeRef& type) {
        return type.kind == TypeKind::Struct || type.kind == TypeKind::Array;
    }

    void collectLocals(const FunctionAst& fn, FunctionContext& ctx) {
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            const bool aggregate = isAggregateType(fn.params[i].second);
            const int size = aggregate ? 1 : std::max(1, typeSizeWords(fn.params[i].second, layout_table_));
            ctx.locals[fn.params[i].first] = LocalInfo{fn.params[i].second,
                                                       ctx.next_local_offset,
                                                       size,
                                                       true,
                                                       false,
                                                       aggregate};
            ctx.next_local_offset += size;
        }
        collectLocalsIn(fn.body, ctx);
    }

    void collectLocalsIn(const std::vector<Stmt>& body, FunctionContext& ctx) {
        for (const auto& stmt : body) {
            if (stmt.kind == StmtKind::Let && ctx.locals.count(stmt.name) == 0) {
                TypeRef type = stmt.annotation;
                if (type.kind == TypeKind::Unknown && stmt.expr) {
                    type = inferLiteralAggregateType(stmt.expr, ctx);
                }
                const int size = std::max(1, typeSizeWords(type, layout_table_));
                ctx.locals[stmt.name] = LocalInfo{stmt.annotation,
                                                  ctx.next_local_offset,
                                                  size,
                                                  stmt.mutable_binding,
                                                  false,
                                                  false};
                if (ctx.locals[stmt.name].type.kind == TypeKind::Unknown) {
                    ctx.locals[stmt.name].type = type;
                }
                ctx.next_local_offset += size;
            }
            if (stmt.kind == StmtKind::MatchSign) {
                for (const auto& arm : stmt.arms) {
                    if (arm.name == "valid" && !arm.binding.empty() && ctx.locals.count(arm.binding) == 0) {
                        ctx.locals[arm.binding] = LocalInfo{TypeRef::unknown(),
                                                          ctx.next_local_offset,
                                                          1,
                                                          false,
                                                          false,
                                                          false};
                        ctx.next_local_offset += 1;
                    }
                }
            }
            collectLocalsIn(stmt.body, ctx);
            for (const auto& arm : stmt.arms) collectLocalsIn(arm.body, ctx);
        }
    }

    void materializeLocalAllocas(FunctionContext& ctx) {
        for (auto& [name, local] : ctx.locals) {
            Instr alloca;
            alloca.def = ctx.next_value++;
            alloca.opcode = InstrOpcode::Alloca;
            alloca.type = local.type;
            alloca.imm = local.offset;
            alloca.aux = local.size_words;
            alloca.symbol = name;
            alloca.effect = Effect::Pure;
            alloca.span = ctx.ast ? ctx.ast->span : SourceSpan{};
            local.ir_address = alloca.def;
            if (ctx.block) {
                ctx.block->instructions.push_back(std::move(alloca));
            }
        }
        if (!ctx.ast || !ctx.block) return;
        int argument_word = 0;
        for (const auto& parameter : ctx.ast->params) {
            const auto local = ctx.locals.find(parameter.first);
            if (local == ctx.locals.end()) continue;
            Instr param;
            param.def = ctx.next_value++;
            param.opcode = InstrOpcode::Param;
            param.type = parameter.second;
            param.aux = argument_word;
            param.symbol = parameter.first;
            param.effect = Effect::Pure;
            param.span = ctx.ast->span;
            ctx.block->instructions.push_back(param);

            Instr store;
            store.def = -1;
            store.opcode = InstrOpcode::Store;
            store.type = parameter.second;
            store.args = {local->second.ir_address, param.def};
            store.effect = Effect::WriteMem;
            store.span = ctx.ast->span;
            ctx.block->instructions.push_back(std::move(store));
            argument_word += std::max(
                1, typeSizeWords(parameter.second, layout_table_));
        }
    }

    [[nodiscard]] TypeRef inferLiteralAggregateType(const ExprPtr& expr, FunctionContext& ctx) const {
        if (!expr) return TypeRef::unknown();
        if (expr->kind == ExprKind::StructLiteral) {
            TypeRef out;
            out.kind = TypeKind::Struct;
            out.name = expr->text;
            return out;
        }
        if (expr->kind == ExprKind::ArrayLiteral) {
            TypeRef elem = TypeRef::numeric(ir::Type::T40);
            if (!expr->args.empty() && expr->args[0] &&
                expr->args[0]->kind == ExprKind::StructLiteral) {
                elem.kind = TypeKind::Struct;
                elem.name = expr->args[0]->text;
            }
            TypeRef out;
            out.kind = TypeKind::Array;
            out.element = std::make_shared<TypeRef>(elem);
            out.array_len = static_cast<int>(expr->args.size());
            return out;
        }
        (void)ctx;
        return TypeRef::unknown();
    }

    void emitFunctionPrologue(const FunctionAst& fn, FunctionContext& ctx) {
        ctx.raw(fn.name + ":");
        ctx.raw("    mov.t40 r24, " + std::to_string(ctx.frame_words));
        ctx.raw("    sub.t40 sp, sp, r24");
        ctx.raw("    store lr, sp, 0");
        for (std::size_t i = 0; i < ctx.callee_saved_regs.size(); ++i) {
            ctx.raw("    store r" + std::to_string(ctx.callee_saved_regs[i]) + ", sp, " +
                    std::to_string(ctx.next_local_offset + static_cast<int>(i)));
        }
        int argument_register_word = 0;
        int stack_argument_word = 0;
        for (const auto& parameter : fn.params) {
            const auto it = ctx.locals.find(parameter.first);
            if (it == ctx.locals.end()) continue;
            const int width = isAggregateType(parameter.second)
                ? 1
                : std::max(1, typeSizeWords(parameter.second, layout_table_));
            if (argument_register_word + width <= kRegisterArgCount) {
                const int source = 13 + argument_register_word;
                ctx.raw("    " + scalarMemoryMnemonic("store", parameter.second) +
                        " r" + std::to_string(source) + ", sp, " +
                        std::to_string(it->second.offset));
                argument_register_word += width;
            } else {
                const int scratch = width == 2 ? 23 : 24;
                const int stack_arg_offset =
                    ctx.frame_words + stack_argument_word;
                ctx.raw("    " + scalarMemoryMnemonic("load", parameter.second) +
                        " r" + std::to_string(scratch) + ", sp, " +
                        std::to_string(stack_arg_offset));
                ctx.raw("    " + scalarMemoryMnemonic("store", parameter.second) +
                        " r" + std::to_string(scratch) + ", sp, " +
                        std::to_string(it->second.offset));
                stack_argument_word += width;
            }
        }
    }

    void emitEpilogue(FunctionContext& ctx) {
        const std::string epilogue = ctx.ast->name + "_return";
        ctx.raw(epilogue + ":");
        for (std::size_t i = 0; i < ctx.callee_saved_regs.size(); ++i) {
            ctx.raw("    load r" + std::to_string(ctx.callee_saved_regs[i]) + ", sp, " +
                    std::to_string(ctx.next_local_offset + static_cast<int>(i)));
        }
        ctx.raw("    load lr, sp, 0");
        ctx.raw("    mov.t40 r24, " + std::to_string(ctx.frame_words));
        ctx.raw("    add.t40 sp, sp, r24");
        ctx.raw("    ret");
        ctx.block->terminator.kind = TerminatorKind::Return;
    }

    void emitDefaultReturn(FunctionContext& ctx) {
        const bool has_ret =
            ctx.block &&
            ctx.block->terminator.kind == TerminatorKind::Return;
        if (!has_ret) {
            emitDropsForReturn(ctx);
            ctx.raw("    mov." + std::string(ir::suffix(ctx.ast->return_type.scalar)) + " r13, 0");
            ctx.raw("    jmp " + ctx.ast->name + "_return");
            Instr zero;
            zero.def = ctx.next_value++;
            zero.opcode = InstrOpcode::Const;
            zero.type = ctx.ast->return_type;
            zero.imm = 0;
            zero.effect = Effect::Pure;
            zero.span = ctx.ast->span;
            const ValueId zero_value = zero.def;
            ctx.block->instructions.push_back(std::move(zero));

            Instr ret;
            ret.def = -1;
            ret.opcode = InstrOpcode::Ret;
            ret.type = ctx.ast->return_type;
            ret.args = {zero_value};
            ret.effect = Effect::Control;
            ret.span = ctx.ast->span;
            ctx.block->instructions.push_back(std::move(ret));
            ctx.block->terminator.kind = TerminatorKind::Return;
        }
        emitEpilogue(ctx);
    }

    void emitDrops(const std::vector<std::string>& vars, FunctionContext& ctx) {
        for (const auto& name : vars) {
            auto it = ctx.locals.find(name);
            if (it != ctx.locals.end()) {
                if (it->second.type.kind == TypeKind::Owned && ctx.moved_vars.count(name) == 0) {
                    ctx.line("load r13, sp, " + std::to_string(it->second.offset));
                    ctx.line("call free");
                    ctx.moved_vars.insert(name);
                }
            }
        }
    }

    void emitDropsForReturn(FunctionContext& ctx) {
        for (auto it = ctx.scope_vars.rbegin(); it != ctx.scope_vars.rend(); ++it) {
            emitDrops(*it, ctx);
        }
    }

    void emitStmt(const Stmt& stmt, FunctionContext& ctx) {
        switch (stmt.kind) {
            case StmtKind::Let: emitLet(stmt, ctx); break;
            case StmtKind::Assign: emitAssign(stmt, ctx); break;
            case StmtKind::Return: emitReturn(stmt, ctx); break;
            case StmtKind::Expr: {
                ExprCode code = emitExpr(stmt.expr, TypeRef::unknown(), ctx);
                ctx.release(code.reg);
                break;
            }
            case StmtKind::WhilePos: emitWhile(stmt, ctx); break;
            case StmtKind::If: emitIf(stmt, ctx); break;
            case StmtKind::MatchSign: emitMatch(stmt, ctx); break;
            case StmtKind::UnsafeBlock: {
                const bool old = ctx.unsafe_allowed;
                ctx.unsafe_allowed = true;
                ctx.scope_vars.push_back({});
                emitStatements(stmt.body, ctx);
                emitDrops(ctx.scope_vars.back(), ctx);
                ctx.scope_vars.pop_back();
                ctx.unsafe_allowed = old;
                break;
            }
            case StmtKind::TupleSwap: emitTupleSwap(stmt, ctx); break;
            case StmtKind::Const: {
                // Evaluate const expression at compile time
                long long value = 0;
                TypeRef type;
                std::vector<Diagnostic> dummy_diags;
                // Try to evaluate the constant expression
                if (evalConstantExpr(stmt.expr, ctx.constants, value, type, false, dummy_diags)) {
                    ctx.constants[stmt.name] = ConstantInfo{value, type};
                }
                break;
            }
        }
    }

    void emitStatements(
        const std::vector<Stmt>& statements,
        FunctionContext& ctx) {
        for (const Stmt& statement : statements) {
            if (ctx.block &&
                ctx.block->terminator.kind != TerminatorKind::None) {
                break;
            }
            emitStmt(statement, ctx);
        }
    }

    void emitLet(const Stmt& stmt, FunctionContext& ctx) {
        if (!ctx.scope_vars.empty()) {
            ctx.scope_vars.back().push_back(stmt.name);
        }
        auto it = ctx.locals.find(stmt.name);
        if (it == ctx.locals.end()) return;
        if (isAggregateType(it->second.type)) {
            int base = emitLocalBase(it->second, ctx);
            emitAggregateInitToAddress(stmt.expr, it->second.type, base, ctx);
            ctx.release(base);
            return;
        }
        TypeRef expected = it->second.type.kind == TypeKind::Unknown
            ? TypeRef::numeric(ir::Type::T40)
            : it->second.type;
        ExprCode code = emitExpr(stmt.expr, expected, ctx);
        if (it->second.type.kind == TypeKind::Unknown) it->second.type = code.type;
        if (!canWiden(code.type, it->second.type)) {
            diag("implicit narrowing is not allowed from " + code.type.str() +
                 " to " + it->second.type.str(), stmt.span);
        }
        emitCvtIfNeeded(code, it->second.type, ctx);
        ctx.line(scalarMemoryMnemonic("store", it->second.type) +
                 " r" + std::to_string(code.reg) + ", sp, " +
                 std::to_string(it->second.offset));
        ctx.value(InstrOpcode::Store, it->second.type, stmt.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                it->second.ir_address, code.value};
        }
        ctx.release(code.reg);
    }

    void emitAssign(const Stmt& stmt, FunctionContext& ctx) {
        if (stmt.target && stmt.target->kind == ExprKind::Name) {
            ctx.moved_vars.erase(stmt.target->text);
        } else if (!stmt.name.empty()) {
            ctx.moved_vars.erase(stmt.name);
        }
        ExprPtr target = stmt.target;
        if (!target && !stmt.name.empty()) {
            target = std::make_shared<Expr>();
            target->kind = ExprKind::Name;
            target->text = stmt.name;
            target->span = stmt.span;
        }
        TypeRef targetType = inferPlaceType(target, ctx);
        if (isAggregateType(targetType)) {
            LValueCode place = emitAddress(target, ctx);
            if (!place.valid) {
                diag("invalid assignment target", stmt.span);
                return;
            }
            if (!place.mutable_binding) {
                diag("cannot assign to immutable binding", stmt.span);
            }
            emitAggregateInitToAddress(stmt.expr, place.type, place.reg, ctx);
            ctx.release(place.reg);
            return;
        }

        ExprCode code = emitExpr(stmt.expr, targetType, ctx);
        LValueCode place = emitAddress(target, ctx);
        if (!place.valid) {
            diag("invalid assignment target", stmt.span);
            ctx.release(code.reg);
            return;
        }
        if (!place.mutable_binding) {
            diag("cannot assign to immutable binding", stmt.span);
        }
        if (!canWiden(code.type, place.type)) {
            diag("implicit narrowing is not allowed from " + code.type.str() +
                 " to " + place.type.str(), stmt.span);
        }
        emitCvtIfNeeded(code, place.type, ctx);
        ctx.line(scalarMemoryMnemonic("store", place.type) +
                 " r" + std::to_string(code.reg) + ", r" +
                 std::to_string(place.reg) + ", 0");
        ctx.value(InstrOpcode::Store, place.type, stmt.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ValueId addr_val = -1;
            if (target && target->kind == ExprKind::Name) {
                const auto local = ctx.locals.find(target->text);
                if (local != ctx.locals.end()) {
                    addr_val = local->second.ir_address;
                }
            }
            if (addr_val < 0) {
                auto it_addr = ctx.reg_to_value.find(place.reg);
                if (it_addr != ctx.reg_to_value.end()) {
                    addr_val = it_addr->second;
                }
            }
            ctx.block->instructions.back().args = {addr_val, code.value};
        }
        ctx.release(code.reg);
        ctx.release(place.reg);
    }

    [[nodiscard]] TypeRef inferPlaceType(const ExprPtr& expr, const FunctionContext& ctx) const {
        if (!expr) return TypeRef::unknown();
        if (expr->kind == ExprKind::Name) {
            auto it = ctx.locals.find(expr->text);
            return it == ctx.locals.end() ? TypeRef::unknown() : it->second.type;
        }
        if (expr->kind == ExprKind::Field) {
            TypeRef base = inferPlaceType(expr->left, ctx);
            const FieldLayout* field = findFieldLayout(base, layout_table_, expr->text);
            return field ? field->type : TypeRef::unknown();
        }
        if (expr->kind == ExprKind::Index) {
            TypeRef base = inferPlaceType(expr->left, ctx);
            if (base.kind == TypeKind::Array && base.element) return *base.element;
            return TypeRef::unknown();
        }
        if (expr->kind == ExprKind::Unary && expr->text == "*") {
            return TypeRef::unknown();
        }
        return TypeRef::unknown();
    }

    void emitReturn(const Stmt& stmt, FunctionContext& ctx) {
        ValueId return_value = -1;
        if (stmt.expr) {
            ExprCode code = emitExpr(stmt.expr, ctx.ast->return_type, ctx);
            if (!canWiden(code.type, ctx.ast->return_type)) {
                diag("implicit narrowing is not allowed from " + code.type.str() +
                 " to " + ctx.ast->return_type.str(), stmt.span);
            }
            emitCvtIfNeeded(code, ctx.ast->return_type, ctx);
            ctx.line(scalarMemoryMnemonic("store", ctx.ast->return_type) +
                     " r" + std::to_string(code.reg) + ", sp, " +
                     std::to_string(ctx.return_slot_offset));
            return_value = code.value;
            ctx.release(code.reg);
        }
        emitDropsForReturn(ctx);
        if (stmt.expr) {
            ctx.line(scalarMemoryMnemonic("load", ctx.ast->return_type) +
                     " r13, sp, " + std::to_string(ctx.return_slot_offset));
        } else {
            ctx.line("mov.t40 r13, 0");
        }
        ctx.line("jmp " + ctx.ast->name + "_return");
        Instr ret;
        ret.def = -1;
        ret.opcode = InstrOpcode::Ret;
        ret.type = ctx.ast->return_type;
        if (return_value >= 0) ret.args = {return_value};
        ret.effect = Effect::Control;
        ret.span = stmt.span;
        if (ctx.block) ctx.block->instructions.push_back(std::move(ret));
        if (ctx.block) {
            ctx.block->terminator.kind = TerminatorKind::Return;
        }
    }

    void emitIf(const Stmt& stmt, FunctionContext& ctx) {
        std::string then_label = ctx.label("if_then");
        std::string else_label = ctx.label("if_else");
        std::string end_label = ctx.label("if_end");
        ExprCode cond = emitExpr(stmt.expr, TypeRef::trit(), ctx);
        ctx.line("brn r" + std::to_string(cond.reg) + ", " + else_label);
        ctx.line("brz r" + std::to_string(cond.reg) + ", " + else_label);
        if (ctx.block) {
            ctx.block->terminator.kind = TerminatorKind::Branch3;
            ctx.block->terminator.condition = cond.value;
            ctx.block->terminator.target_neg = else_label;
            ctx.block->terminator.target_zero = else_label;
            ctx.block->terminator.target_pos = then_label;
        }
        ctx.release(cond.reg);

        ctx.ir.blocks.push_back(BasicBlock{then_label, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(then_label + ":");
        ctx.scope_vars.push_back({});
        emitStatements(stmt.body, ctx);
        emitDrops(ctx.scope_vars.back(), ctx);
        ctx.scope_vars.pop_back();
        ctx.line("jmp " + end_label);
        if (ctx.block && ctx.block->terminator.kind == TerminatorKind::None) {
            ctx.block->terminator.kind = TerminatorKind::Jump;
            ctx.block->terminator.target = end_label;
        }

        ctx.ir.blocks.push_back(BasicBlock{else_label, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(else_label + ":");
        ctx.scope_vars.push_back({});
        emitStatements(stmt.else_body, ctx);
        emitDrops(ctx.scope_vars.back(), ctx);
        ctx.scope_vars.pop_back();
        if (ctx.block && ctx.block->terminator.kind == TerminatorKind::None) {
            ctx.block->terminator.kind = TerminatorKind::Jump;
            ctx.block->terminator.target = end_label;
        }

        ctx.ir.blocks.push_back(BasicBlock{end_label, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(end_label + ":");
    }

    void emitWhile(const Stmt& stmt, FunctionContext& ctx) {
        std::string start = ctx.label("while_start");
        std::string body = ctx.label("while_body");
        std::string end = ctx.label("while_end");
        if (ctx.block && ctx.block->terminator.kind == TerminatorKind::None) {
            ctx.block->terminator.kind = TerminatorKind::Jump;
            ctx.block->terminator.target = start;
        }
        ctx.ir.blocks.push_back(BasicBlock{start, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(start + ":");
        ExprCode cond = emitExpr(stmt.expr, TypeRef::unknown(), ctx);
        ctx.line("brn r" + std::to_string(cond.reg) + ", " + end);
        ctx.line("brz r" + std::to_string(cond.reg) + ", " + end);
        if (ctx.block) {
            ctx.block->terminator.kind = TerminatorKind::Branch3;
            ctx.block->terminator.condition = cond.value;
            ctx.block->terminator.target_neg = end;
            ctx.block->terminator.target_zero = end;
            ctx.block->terminator.target_pos = body;
        }
        ctx.release(cond.reg);
        ctx.ir.blocks.push_back(BasicBlock{body, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(body + ":");
        ctx.scope_vars.push_back({});
        emitStatements(stmt.body, ctx);
        emitDrops(ctx.scope_vars.back(), ctx);
        ctx.scope_vars.pop_back();
        ctx.line("jmp " + start);
        if (ctx.block && ctx.block->terminator.kind == TerminatorKind::None) {
            ctx.block->terminator.kind = TerminatorKind::Jump;
            ctx.block->terminator.target = start;
        }
        ctx.ir.blocks.push_back(BasicBlock{end, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(end + ":");
    }

    enum class MatchTselKind : uint8_t {
        Return,
        AssignLocal,
    };

    struct MatchTselPlan {
        MatchTselKind kind = MatchTselKind::Return;
        const Stmt* neg_stmt = nullptr;
        const Stmt* zero_stmt = nullptr;
        const Stmt* pos_stmt = nullptr;
        std::string target_name;
    };

    [[nodiscard]] static bool isPureMatchExpr(const ExprPtr& expr) {
        if (!expr) return false;
        switch (expr->kind) {
            case ExprKind::Number:
            case ExprKind::Name:
                return true;
            case ExprKind::Unary:
                return expr->text == "-" && isPureMatchExpr(expr->left);
            case ExprKind::Binary:
                if (expr->text == "/") return false;
                return isPureMatchExpr(expr->left) && isPureMatchExpr(expr->right);
            case ExprKind::Call:
            case ExprKind::Field:
            case ExprKind::Index:
            case ExprKind::StructLiteral:
            case ExprKind::ArrayLiteral:
                return false;
        }
        return false;
    }

    [[nodiscard]] const MatchArm* findMatchArm(
        const Stmt& stmt,
        const std::string& name) const {

        for (const auto& arm : stmt.arms) {
            if (arm.name == name) return &arm;
        }
        for (const auto& arm : stmt.arms) {
            if (arm.name == "_") return &arm;
        }
        return nullptr;
    }

    [[nodiscard]] static bool armIsPureReturn(
        const MatchArm* arm,
        const Stmt** out_stmt) {

        if (!arm || arm->body.size() != 1) return false;
        const Stmt& stmt = arm->body.front();
        if (stmt.kind != StmtKind::Return || !isPureMatchExpr(stmt.expr)) return false;
        *out_stmt = &stmt;
        return true;
    }

    [[nodiscard]] static bool simpleAssignTargetName(
        const Stmt& stmt,
        std::string& out) {

        if (stmt.target && stmt.target->kind == ExprKind::Name) {
            out = stmt.target->text;
            return true;
        }
        if (!stmt.name.empty()) {
            out = stmt.name;
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool armIsPureLocalAssign(
        const MatchArm* arm,
        const Stmt** out_stmt,
        std::string& out_target) {

        if (!arm || arm->body.size() != 1) return false;
        const Stmt& stmt = arm->body.front();
        if (stmt.kind != StmtKind::Assign || !isPureMatchExpr(stmt.expr)) return false;
        if (!simpleAssignTargetName(stmt, out_target)) return false;
        *out_stmt = &stmt;
        return true;
    }

    [[nodiscard]] bool analyzeTselMatch(
        const Stmt& stmt,
        const std::string& negName,
        const std::string& zeroName,
        const std::string& posName,
        MatchTselPlan& plan) const {

        const MatchArm* negArm = findMatchArm(stmt, negName);
        const MatchArm* zeroArm = findMatchArm(stmt, zeroName);
        const MatchArm* posArm = findMatchArm(stmt, posName);

        const Stmt* negStmt = nullptr;
        const Stmt* zeroStmt = nullptr;
        const Stmt* posStmt = nullptr;
        if (armIsPureReturn(negArm, &negStmt) &&
            armIsPureReturn(zeroArm, &zeroStmt) &&
            armIsPureReturn(posArm, &posStmt)) {
            plan.kind = MatchTselKind::Return;
            plan.neg_stmt = negStmt;
            plan.zero_stmt = zeroStmt;
            plan.pos_stmt = posStmt;
            return true;
        }

        std::string negTarget;
        std::string zeroTarget;
        std::string posTarget;
        if (armIsPureLocalAssign(negArm, &negStmt, negTarget) &&
            armIsPureLocalAssign(zeroArm, &zeroStmt, zeroTarget) &&
            armIsPureLocalAssign(posArm, &posStmt, posTarget) &&
            negTarget == zeroTarget && negTarget == posTarget) {
            plan.kind = MatchTselKind::AssignLocal;
            plan.neg_stmt = negStmt;
            plan.zero_stmt = zeroStmt;
            plan.pos_stmt = posStmt;
            plan.target_name = negTarget;
            return true;
        }

        return false;
    }

    [[nodiscard]] ExprCode emitTselValue(
        const MatchTselPlan& plan,
        const ExprCode& cond,
        const TypeRef& targetType,
        const SourceSpan& span,
        FunctionContext& ctx) {

        ExprCode neg = emitExpr(plan.neg_stmt->expr, targetType, ctx);
        ExprCode zero = emitExpr(plan.zero_stmt->expr, targetType, ctx);
        ExprCode pos = emitExpr(plan.pos_stmt->expr, targetType, ctx);

        if (!canWiden(neg.type, targetType)) {
            diag("implicit narrowing is not allowed from " + neg.type.str() +
                 " to " + targetType.str(), plan.neg_stmt->span);
        }
        if (!canWiden(zero.type, targetType)) {
            diag("implicit narrowing is not allowed from " + zero.type.str() +
                 " to " + targetType.str(), plan.zero_stmt->span);
        }
        if (!canWiden(pos.type, targetType)) {
            diag("implicit narrowing is not allowed from " + pos.type.str() +
                 " to " + targetType.str(), plan.pos_stmt->span);
        }

        emitCvtIfNeeded(neg, targetType, ctx);
        emitCvtIfNeeded(zero, targetType, ctx);
        emitCvtIfNeeded(pos, targetType, ctx);

        int out = ctx.acquire();
        ctx.line("tsel r" + std::to_string(out) + ", r" + std::to_string(cond.reg) +
                 ", r" + std::to_string(neg.reg) + ", r" + std::to_string(zero.reg) +
                 ", r" + std::to_string(pos.reg));
        ValueId id = ctx.value(InstrOpcode::Tsel, targetType, span, out);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                cond.value,
                neg.value,
                zero.value,
                pos.value,
            };
        }

        ctx.release(neg.reg);
        ctx.release(zero.reg);
        ctx.release(pos.reg);
        return ExprCode{out, targetType, false, id};
    }

    [[nodiscard]] bool tryEmitTselMatch(
        const Stmt& stmt,
        const ExprCode& cond,
        bool isPointer,
        const std::string& negName,
        const std::string& zeroName,
        const std::string& posName,
        FunctionContext& ctx) {

        if (isPointer) return false;

        MatchTselPlan plan;
        if (!analyzeTselMatch(stmt, negName, zeroName, posName, plan)) return false;

        TypeRef targetType = TypeRef::unknown();
        if (plan.kind == MatchTselKind::Return) {
            targetType = ctx.ast->return_type;
        } else {
            auto local = ctx.locals.find(plan.target_name);
            if (local == ctx.locals.end() || isAggregateType(local->second.type)) return false;
            targetType = local->second.type;
        }

        if (!isNumericLike(targetType) || usesWideT50Pair(targetType))
            return false;

        ExprCode selected = emitTselValue(plan, cond, targetType, stmt.span, ctx);
        if (plan.kind == MatchTselKind::Return) {
            ctx.line(scalarMemoryMnemonic("store", targetType) +
                     " r" + std::to_string(selected.reg) + ", sp, " +
                     std::to_string(ctx.return_slot_offset));
            const ValueId return_value = selected.value;
            ctx.release(selected.reg);
            emitDropsForReturn(ctx);
            ctx.line(scalarMemoryMnemonic("load", targetType) +
                     " r13, sp, " + std::to_string(ctx.return_slot_offset));
            ctx.line("jmp " + ctx.ast->name + "_return");
            Instr ret;
            ret.def = -1;
            ret.opcode = InstrOpcode::Ret;
            ret.type = targetType;
            ret.args = {return_value};
            ret.effect = Effect::Control;
            ret.span = stmt.span;
            if (ctx.block)
                ctx.block->instructions.push_back(std::move(ret));
            if (ctx.block) {
                ctx.block->terminator.kind = TerminatorKind::Return;
            }
            return true;
        }

        auto local = ctx.locals.find(plan.target_name);
        if (local == ctx.locals.end()) {
            ctx.release(selected.reg);
            return false;
        }
        ctx.moved_vars.erase(plan.target_name);
        if (!local->second.mutable_binding) {
            diag("cannot assign to immutable binding", stmt.span);
        }
        ctx.line(scalarMemoryMnemonic("store", targetType) +
                 " r" + std::to_string(selected.reg) + ", sp, " +
                 std::to_string(local->second.offset));
        ctx.value(InstrOpcode::Store, targetType, stmt.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                local->second.ir_address, selected.value};
        }
        ctx.release(selected.reg);
        return true;
    }

    void emitMatch(const Stmt& stmt, FunctionContext& ctx) {
        // Evaluate the match subject expression
        ExprCode cond = emitExpr(stmt.expr, TypeRef::unknown(), ctx);
        TypeRef subjType = cond.type;
        if (stmt.expr && stmt.expr->kind == ExprKind::Name && pointerTypeView(subjType)) {
            ctx.moved_vars.erase(stmt.expr->text);
        }
        
        // Determine if this is a pointer or numeric match based on arm names
        bool isPointer = pointerTypeView(subjType) != nullptr;
        for (const auto& arm : stmt.arms) {
            if (arm.name == "valid" || arm.name == "null" || arm.name == "unknown") {
                isPointer = true;
                break;
            }
        }
        
        // TCL 1.0 Feature A1: Implicit ternary conversion
        // If subject is numeric/trit but not already ternary, implicitly compare to 0
        // This eliminates the need for match sign(expr) - now just match expr
        if (!isPointer && subjType.kind == TypeKind::Numeric && subjType.scalar != ir::Type::T1) {
            // For numeric types, compile as sign(expr) by comparing expr to 0
            int reg_cmp = ctx.acquire();
            ctx.line("mov.t40 r" + std::to_string(reg_cmp) + ", 0");
            ValueId zero_id = ctx.value(InstrOpcode::Const, TypeRef::numeric(ir::Type::T40), stmt.span, reg_cmp);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().imm = 0;
            }
            ctx.line("tcmp r" + std::to_string(cond.reg) + ", r" + std::to_string(cond.reg) + ", r" + std::to_string(reg_cmp));
            ValueId cmp_id = ctx.value(InstrOpcode::Cmp, TypeRef::numeric(ir::Type::T1), stmt.span, cond.reg);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {cond.value, zero_id};
            }
            cond.value = cmp_id;
            cond.type = TypeRef::trit();
            ctx.release(reg_cmp);
        }
        // If already Trit (T1), no conversion needed

        std::string negName = isPointer ? "null" : "neg";
        std::string zeroName = isPointer ? "unknown" : "zero";
        std::string posName = isPointer ? "valid" : "pos";

        if (tryEmitTselMatch(stmt, cond, isPointer, negName, zeroName, posName, ctx)) {
            ctx.release(cond.reg);
            return;
        }
        std::string neg = ctx.label("match_" + negName);
        std::string zero = ctx.label("match_" + zeroName);
        std::string pos = ctx.label("match_" + posName);
        std::string end = ctx.label("match_end");

        ctx.line("brn r" + std::to_string(cond.reg) + ", " + neg);
        ctx.line("brz r" + std::to_string(cond.reg) + ", " + zero);
        if (ctx.block) {
            ctx.block->terminator.kind = TerminatorKind::Branch3;
            ctx.block->terminator.condition = cond.value;
            ctx.block->terminator.target_neg = neg;
            ctx.block->terminator.target_zero = zero;
            ctx.block->terminator.target_pos = pos;
        }

        emitArm(posName, pos, end, stmt, cond, ctx);
        emitArm(negName, neg, end, stmt, cond, ctx);
        emitArm(zeroName, zero, end, stmt, cond, ctx);
        ctx.release(cond.reg);
        ctx.ir.blocks.push_back(BasicBlock{end, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(end + ":");
    }

    void emitArm(
        const std::string& name,
        const std::string& label,
        const std::string& end,
        const Stmt& stmt,
        const ExprCode& cond,
        FunctionContext& ctx) {
        ctx.ir.blocks.push_back(BasicBlock{label, {}, {}});
        ctx.block = &ctx.ir.blocks.back();
        ctx.raw(label + ":");
        const MatchArm* matchArm = nullptr;
        for (const auto& arm : stmt.arms) {
            if (arm.name == name) {
                matchArm = &arm;
                break;
            }
        }
        if (!matchArm) {
            for (const auto& arm : stmt.arms) {
                if (arm.name == "_") {
                    matchArm = &arm;
                    break;
                }
            }
        }
        if (matchArm) {
            ctx.scope_vars.push_back({});
            if (name == "valid" && !matchArm->binding.empty()) {
                const TypeRef* matchedPointer = pointerTypeView(cond.type);
                TypeRef inner = matchedPointer && matchedPointer->element
                    ? *matchedPointer->element
                    : TypeRef::numeric(ir::Type::T40);
                PointerRegion region = matchedPointer ? matchedPointer->region : PointerRegion::Stack;
                TypeRef bindingType = TypeRef::pointer(inner, region, PointerState::Valid);
                ctx.locals[matchArm->binding].type = bindingType;
                ctx.line("store r" + std::to_string(cond.reg) + ", sp, " + std::to_string(ctx.locals[matchArm->binding].offset));
                ctx.value(InstrOpcode::Store, bindingType, stmt.span);
                if (ctx.block && !ctx.block->instructions.empty()) {
                    ctx.block->instructions.back().args = {
                        ctx.locals[matchArm->binding].ir_address,
                        cond.value};
                }
                ctx.scope_vars.back().push_back(matchArm->binding);
            }
            emitStatements(matchArm->body, ctx);
            emitDrops(ctx.scope_vars.back(), ctx);
            ctx.scope_vars.pop_back();
        }
        ctx.line("jmp " + end);
        if (ctx.block &&
            ctx.block->terminator.kind == TerminatorKind::None) {
            ctx.block->terminator.kind = TerminatorKind::Jump;
            ctx.block->terminator.target = end;
        }
    }

    void emitTupleSwap(const Stmt& stmt, FunctionContext& ctx) {
        auto a = ctx.locals.find(stmt.name);
        auto b = ctx.locals.find(stmt.second_name);
        if (a == ctx.locals.end() || b == ctx.locals.end()) {
            diag("tuple swap target is not a local binding", stmt.span);
            return;
        }
        if (!sameType(a->second.type, b->second.type)) {
            diag("tuple swap requires matching local types", stmt.span);
        }
        if (ctx.dry_run) {
            // Tuple swap is a memory operation in SSA: its operands are the
            // two address values, and target lowering owns the temporary
            // registers and architectural SWAP instruction. Keeping the
            // operation as one ordered IR node avoids replaying the AST or
            // pretending that a two-cell update is a single SSA result.
            Instr swap;
            swap.opcode = InstrOpcode::Swap;
            swap.type = a->second.type;
            swap.args = {a->second.ir_address, b->second.ir_address};
            swap.effect = Effect::WriteMem;
            swap.span = stmt.span;
            if (ctx.block) ctx.block->instructions.push_back(std::move(swap));
            return;
        }
        int ra = ctx.acquire();
        int rb = ctx.acquire();
        ctx.line(scalarMemoryMnemonic("load", a->second.type) +
                 " r" + std::to_string(ra) + ", sp, " +
                 std::to_string(a->second.offset));
        ValueId id_a = ctx.value(InstrOpcode::Load, a->second.type, stmt.span, ra);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                a->second.ir_address};
        }
        
        ctx.line(scalarMemoryMnemonic("load", b->second.type) +
                 " r" + std::to_string(rb) + ", sp, " +
                 std::to_string(b->second.offset));
        ValueId id_b = ctx.value(InstrOpcode::Load, b->second.type, stmt.span, rb);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                b->second.ir_address};
        }
        
        ctx.line("swap r" + std::to_string(ra) + ", r" + std::to_string(rb));
        // The structural IR expresses the swap through the two reversed
        // stores below. The target-only SWAP instruction above belongs only
        // to legacy assembly replay and must not become a second IR effect.
        
        ctx.line(scalarMemoryMnemonic("store", a->second.type) +
                 " r" + std::to_string(ra) + ", sp, " +
                 std::to_string(a->second.offset));
        ctx.value(InstrOpcode::Store, a->second.type, stmt.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                a->second.ir_address, id_b};
        }
        
        ctx.line(scalarMemoryMnemonic("store", b->second.type) +
                 " r" + std::to_string(rb) + ", sp, " +
                 std::to_string(b->second.offset));
        ctx.value(InstrOpcode::Store, b->second.type, stmt.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                b->second.ir_address, id_a};
        }
        
        ctx.release(ra);
        ctx.release(rb);
    }

    [[nodiscard]] ExprCode emitExpr(const ExprPtr& expr, TypeRef expected, FunctionContext& ctx) {
        if (!expr) return emitImmediate(0, expected, SourceSpan{}, ctx);
        switch (expr->kind) {
            case ExprKind::Number:
                return emitImmediate(expr->number,
                                     expected.kind == TypeKind::Numeric ? expected
                                                                        : TypeRef::numeric(ir::Type::T40),
                                     expr->span, ctx);
            case ExprKind::Name:
                return emitName(*expr, ctx);
            case ExprKind::Unary:
                return emitUnary(*expr, expected, ctx);
            case ExprKind::Binary:
                return emitBinary(*expr, expected, ctx);
            case ExprKind::Call:
                return emitCall(*expr, expected, ctx);
            case ExprKind::Field:
            case ExprKind::Index:
                return emitLoadFromAddress(emitAddress(expr, ctx), expr->span, ctx);
            case ExprKind::StructLiteral:
            case ExprKind::ArrayLiteral:
                diag("aggregate literal must initialize an addressable local or assignment target",
                     expr->span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr->span, ctx);
        }
        return emitImmediate(0, expected, expr->span, ctx);
    }

    [[nodiscard]] ExprCode emitImmediate(
        long long value,
        TypeRef type,
        const SourceSpan& span,
        FunctionContext& ctx) {
        if (type.kind == TypeKind::Unknown) type = TypeRef::numeric(ir::Type::T40);
        int reg = ctx.acquire();
        if (value < 0) {
            ctx.line("mov." + std::string(ir::suffix(type.scalar)) + " r" +
                     std::to_string(reg) + ", " + std::to_string(-value));
            ctx.line("neg." + std::string(ir::suffix(type.scalar)) + " r" +
                     std::to_string(reg) + ", r" + std::to_string(reg));
        } else {
            ctx.line("mov." + std::string(ir::suffix(type.scalar)) + " r" +
                     std::to_string(reg) + ", " + std::to_string(value));
        }
        ValueId id = ctx.value(InstrOpcode::Const, type, span, reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().imm = value;
        }
        return ExprCode{reg, type, false, id};
    }

    [[nodiscard]] ExprCode emitName(const Expr& expr, FunctionContext& ctx) {
        // Check if it's a constant first
        auto const_it = ctx.constants.find(expr.text);
        if (const_it != ctx.constants.end()) {
            return emitImmediate(const_it->second.value, const_it->second.type, expr.span, ctx);
        }
        
        auto it = ctx.locals.find(expr.text);
        if (it == ctx.locals.end()) {
            diag("unknown name '" + expr.text + "'", expr.span);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }
        if (it->second.type.kind == TypeKind::Owned) {
            ctx.moved_vars.insert(expr.text);
        }
        if (isAggregateType(it->second.type)) {
            int addr = emitLocalBase(it->second, ctx);
            ValueId id = -1;
            auto it_reg = ctx.reg_to_value.find(addr);
            if (it_reg != ctx.reg_to_value.end()) id = it_reg->second;
            return ExprCode{addr, it->second.type, true, id};
        }
        int reg = ctx.acquire();
        ctx.line(scalarMemoryMnemonic("load", it->second.type) +
                 " r" + std::to_string(reg) + ", sp, " +
                 std::to_string(it->second.offset));
        ValueId id = ctx.value(InstrOpcode::Load, it->second.type, expr.span, reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                it->second.ir_address};
        }
        return ExprCode{reg, it->second.type, false, id};
    }

    [[nodiscard]] ExprCode emitUnary(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        if (expr.text == "-") {
            ExprCode inner = emitExpr(expr.left, expected, ctx);
            ctx.line("neg." + std::string(ir::suffix(inner.type.scalar)) + " r" +
                     std::to_string(inner.reg) + ", r" + std::to_string(inner.reg));
            return inner;
        }
        if (expr.text == "&") {
            LValueCode place = emitAddress(expr.left, ctx);
            if (!place.valid) {
                diag("address-of requires an addressable place", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ValueId place_value = -1;
            const auto current =
                ctx.reg_to_value.find(place.reg);
            if (current != ctx.reg_to_value.end())
                place_value = current->second;
            ValueId id = ctx.value(InstrOpcode::AddrOf, TypeRef::pointer(place.type), expr.span, place.reg);
            if (ctx.block &&
                !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    place_value};
            }
            return ExprCode{place.reg, TypeRef::pointer(place.type), false, id};
        }
        if (expr.text == "*") {
            ExprCode ptr = emitExpr(expr.left, TypeRef::unknown(), ctx);
            if (expr.left && expr.left->kind == ExprKind::Name) {
                ctx.moved_vars.erase(expr.left->text);
            }
            const TypeRef* pointer = pointerTypeView(ptr.type);
            if (!pointer) {
                diag("dereference requires a pointer", expr.span);
            } else if (pointer->state != PointerState::Valid) {
                diag("cannot dereference pointer before proving it is valid", expr.span);
            }
            TypeRef elem = pointer && pointer->element ? *pointer->element : TypeRef::numeric(ir::Type::T40);
            if (isAggregateType(elem)) {
                ptr.type = elem;
                ptr.address = true;
                return ptr;
            }
            int out = ptr.reg;
            ctx.line(scalarMemoryMnemonic("load", elem) +
                     " r" + std::to_string(out) + ", r" +
                     std::to_string(ptr.reg) + ", 0");
            ValueId id = ctx.value(InstrOpcode::Deref, elem, expr.span, out);
            if (ctx.block &&
                !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    ptr.value};
            }
            return ExprCode{out, elem, false, id};
        }
        diag("unsupported unary operator '" + expr.text + "'", expr.span);
        return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
    }

    [[nodiscard]] int emitLocalBase(const LocalInfo& local, FunctionContext& ctx) {
        int reg = ctx.acquire();
        if (local.by_pointer) {
            ctx.line("load r" + std::to_string(reg) + ", sp, " + std::to_string(local.offset));
            ctx.value(InstrOpcode::Load, TypeRef::pointer(local.type), SourceSpan{}, reg);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    local.ir_address};
            }
            return reg;
        }
        int off = ctx.acquire();
        ctx.line("mov.t40 r" + std::to_string(off) + ", " + std::to_string(local.offset));
        ValueId off_id = ctx.value(InstrOpcode::Const, TypeRef::numeric(ir::Type::T40), SourceSpan{}, off);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().imm = local.offset;
        }
        ctx.line("add.t40 r" + std::to_string(reg) + ", sp, r" + std::to_string(off));
        const ValueId address_id =
            ctx.value(
                InstrOpcode::AddrOf,
                TypeRef::pointer(local.type),
                SourceSpan{}, reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {
                isAggregateType(local.type)
                    ? local.ir_address
                    : off_id};
        }
        if (ctx.dry_run &&
            !isAggregateType(local.type)) {
            // Scalar local loads/stores use the virtual frame index directly
            // so mem2reg can reason about the allocation. The target-only
            // address calculation above remains for value-ID parity with
            // legacy replay and is dead unless unary '&' consumes it.
            ctx.reg_to_value[reg] = local.ir_address;
        } else {
            ctx.reg_to_value[reg] = address_id;
        }
        ctx.release(off);
        return reg;
    }

    void addImmediateToReg(int reg, int offset, FunctionContext& ctx) {
        if (offset == 0) return;
        int tmp = ctx.acquire();
        ctx.line("mov.t40 r" + std::to_string(tmp) + ", " + std::to_string(offset));
        ValueId tmp_id = ctx.value(InstrOpcode::Const, TypeRef::numeric(ir::Type::T40), SourceSpan{}, tmp);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().imm = offset;
        }
        ValueId reg_val = -1;
        auto it = ctx.reg_to_value.find(reg);
        if (it != ctx.reg_to_value.end()) {
            reg_val = it->second;
        }
        ctx.line("add.t40 r" + std::to_string(reg) + ", r" + std::to_string(reg) +
                 ", r" + std::to_string(tmp));
        ctx.value(InstrOpcode::Add, TypeRef::numeric(ir::Type::T40), SourceSpan{}, reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {reg_val, tmp_id};
        }
        ctx.release(tmp);
    }

    [[nodiscard]] LValueCode emitAddress(const ExprPtr& expr, FunctionContext& ctx) {
        if (!expr) return LValueCode{};
        if (expr->kind == ExprKind::Name) {
            auto it = ctx.locals.find(expr->text);
            if (it == ctx.locals.end()) {
                diag("unknown place '" + expr->text + "'", expr->span);
                return LValueCode{};
            }
            int reg = emitLocalBase(it->second, ctx);
            return LValueCode{reg, it->second.type, it->second.mutable_binding, true};
        }
        if (expr->kind == ExprKind::Unary && expr->text == "*") {
            ExprCode ptr = emitExpr(expr->left, TypeRef::unknown(), ctx);
            if (expr->left && expr->left->kind == ExprKind::Name) {
                ctx.moved_vars.erase(expr->left->text);
            }
            const TypeRef* pointer = pointerTypeView(ptr.type);
            if (!pointer) {
                diag("dereference requires a pointer", expr->span);
                ctx.release(ptr.reg);
                return LValueCode{};
            }
            if (pointer->state != PointerState::Valid) {
                diag("cannot dereference pointer before proving it is valid", expr->span);
            }
            TypeRef elem = pointer->element ? *pointer->element : TypeRef::numeric(ir::Type::T40);
            return LValueCode{ptr.reg, elem, true, true};
        }
        if (expr->kind == ExprKind::Field) {
            LValueCode base = emitAddress(expr->left, ctx);
            if (!base.valid) return base;
            const FieldLayout* field = findFieldLayout(base.type, layout_table_, expr->text);
            if (!field) {
                diag("unknown field '" + expr->text + "' on " + base.type.str(), expr->span);
                ctx.release(base.reg);
                return LValueCode{};
            }
            addImmediateToReg(base.reg, field->offset_words, ctx);
            ValueId address_value = -1;
            const auto after_offset = ctx.reg_to_value.find(base.reg);
            if (after_offset != ctx.reg_to_value.end())
                address_value = after_offset->second;
            ctx.value(InstrOpcode::FieldAddr, TypeRef::pointer(field->type), expr->span, base.reg);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    address_value};
            }
            base.type = field->type;
            return base;
        }
        if (expr->kind == ExprKind::Index) {
            LValueCode base = emitAddress(expr->left, ctx);
            if (!base.valid) return base;
            if (base.type.kind != TypeKind::Array || !base.type.element) {
                diag("indexing requires an array value", expr->span);
                ctx.release(base.reg);
                return LValueCode{};
            }
            TypeRef elem = *base.type.element;
            const int elemSize = std::max(1, typeSizeWords(elem, layout_table_));
            int constantIndex = 0;
            if (extractInteger(expr->right, constantIndex)) {
                if (constantIndex < 0 || constantIndex >= base.type.array_len) {
                    diag("constant array index " + std::to_string(constantIndex) +
                         " is out of bounds for length " + std::to_string(base.type.array_len),
                         expr->span);
                }
                addImmediateToReg(base.reg, constantIndex * elemSize, ctx);
            } else {
                ExprCode index = emitExpr(expr->right, TypeRef::numeric(ir::Type::T40), ctx);
                emitCvtIfNeeded(index, TypeRef::numeric(ir::Type::T40), ctx);
                if (elemSize != 1) {
                    int scale = ctx.acquire();
                    ctx.line("mov.t40 r" + std::to_string(scale) + ", " + std::to_string(elemSize));
                    ValueId scale_id = ctx.value(InstrOpcode::Const, TypeRef::numeric(ir::Type::T40), expr->span, scale);
                    if (ctx.block && !ctx.block->instructions.empty()) {
                        ctx.block->instructions.back().imm = elemSize;
                    }
                    ctx.line("mul.t40 r" + std::to_string(index.reg) + ", r" +
                             std::to_string(index.reg) + ", r" + std::to_string(scale));
                    ValueId old_index_val = index.value;
                    index.value = ctx.value(InstrOpcode::Mul, TypeRef::numeric(ir::Type::T40), expr->span, index.reg);
                    if (ctx.block && !ctx.block->instructions.empty()) {
                        ctx.block->instructions.back().args = {old_index_val, scale_id};
                    }
                    ctx.release(scale);
                }
                ctx.line("add.t40 r" + std::to_string(base.reg) + ", r" +
                         std::to_string(base.reg) + ", r" + std::to_string(index.reg));
                ctx.release(index.reg);
            }
            ValueId address_value = -1;
            const auto after_index = ctx.reg_to_value.find(base.reg);
            if (after_index != ctx.reg_to_value.end())
                address_value = after_index->second;
            ctx.value(InstrOpcode::IndexAddr, TypeRef::pointer(elem), expr->span, base.reg);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    address_value};
            }
            base.type = elem;
            return base;
        }
        diag("expression is not addressable", expr->span);
        return LValueCode{};
    }

    [[nodiscard]] ExprCode emitLoadFromAddress(
        LValueCode place,
        const SourceSpan& span,
        FunctionContext& ctx) {

        if (!place.valid) {
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), span, ctx);
        }
        if (isAggregateType(place.type)) {
            ValueId place_val = -1;
            auto it = ctx.reg_to_value.find(place.reg);
            if (it != ctx.reg_to_value.end()) {
                place_val = it->second;
            }
            return ExprCode{place.reg, place.type, true, place_val};
        }
        int out = ctx.acquire();
        ctx.line(scalarMemoryMnemonic("load", place.type) +
                 " r" + std::to_string(out) + ", r" +
                 std::to_string(place.reg) + ", 0");
        ValueId id = ctx.value(InstrOpcode::Load, place.type, span, out);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ValueId place_val = -1;
            auto it = ctx.reg_to_value.find(place.reg);
            if (it != ctx.reg_to_value.end()) {
                place_val = it->second;
            }
            ctx.block->instructions.back().args = {place_val};
        }
        ctx.release(place.reg);
        return ExprCode{out, place.type, false, id};
    }

    void emitScalarStoreAtAddress(
        const ExprPtr& expr,
        const TypeRef& expected,
        int baseReg,
        int offset,
        FunctionContext& ctx) {

        ExprCode code = emitExpr(expr, expected, ctx);
        if (!canWiden(code.type, expected)) {
            diag("implicit narrowing is not allowed from " + code.type.str() +
                 " to " + expected.str(), expr ? expr->span : SourceSpan{});
        }
        emitCvtIfNeeded(code, expected, ctx);
        ctx.line(scalarMemoryMnemonic("store", expected) +
                 " r" + std::to_string(code.reg) + ", r" +
                 std::to_string(baseReg) + ", " + std::to_string(offset));
        ctx.value(InstrOpcode::Store, expected, expr ? expr->span : SourceSpan{});
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().aux = offset;
            ValueId base_val = -1;
            auto it = ctx.reg_to_value.find(baseReg);
            if (it != ctx.reg_to_value.end()) base_val = it->second;
            ctx.block->instructions.back().args = {base_val, code.value};
        }
        ctx.release(code.reg);
    }

    void emitAggregateCopy(int srcReg, int dstReg, const TypeRef& type, FunctionContext& ctx) {
        const int words = std::max(1, typeSizeWords(type, layout_table_));
        int tmp = ctx.acquire();
        ValueId src_val = -1;
        {
            auto it = ctx.reg_to_value.find(srcReg);
            if (it != ctx.reg_to_value.end()) src_val = it->second;
        }
        ValueId dst_val = -1;
        {
            auto it = ctx.reg_to_value.find(dstReg);
            if (it != ctx.reg_to_value.end()) dst_val = it->second;
        }
        for (int i = 0; i < words; ++i) {
            ctx.line("load r" + std::to_string(tmp) + ", r" + std::to_string(srcReg) +
                     ", " + std::to_string(i));
            ValueId load_id = ctx.value(InstrOpcode::Load, TypeRef::numeric(ir::Type::T40), SourceSpan{}, tmp);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {src_val};
            }
            ctx.line("store r" + std::to_string(tmp) + ", r" + std::to_string(dstReg) +
                     ", " + std::to_string(i));
            ctx.value(InstrOpcode::Store, TypeRef::numeric(ir::Type::T40), SourceSpan{});
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().aux = i;
                ctx.block->instructions.back().args = {dst_val, load_id};
            }
        }
        ctx.release(tmp);
    }

    const ExprPtr* findStructLiteralField(const Expr& expr, const std::string& field) const {
        for (const auto& candidate : expr.fields) {
            if (candidate.first == field) return &candidate.second;
        }
        return nullptr;
    }

    void emitAggregateInitToAddress(
        const ExprPtr& expr,
        const TypeRef& type,
        int baseReg,
        FunctionContext& ctx) {

        if (!expr) return;
        if (type.kind == TypeKind::Struct && expr->kind == ExprKind::StructLiteral) {
            if (expr->text != type.name) {
                diag("struct literal " + expr->text + " cannot initialize " + type.str(),
                     expr->span);
            }
            auto layoutIt = layout_table_.find(type.name);
            if (layoutIt == layout_table_.end()) {
                diag("unknown struct type '" + type.name + "'", expr->span);
                return;
            }
            for (const auto& field : layoutIt->second.fields) {
                const ExprPtr* fieldExpr = findStructLiteralField(*expr, field.name);
                if (!fieldExpr) {
                    diag("missing field '" + field.name + "' in struct literal", expr->span);
                    continue;
                }
                if (isAggregateType(field.type)) {
                    int child = ctx.acquire();
                    ctx.line("copy r" + std::to_string(child) + ", r" + std::to_string(baseReg));
                    ctx.value(InstrOpcode::AddrOf, TypeRef::pointer(field.type), expr->span, child);
                    addImmediateToReg(child, field.offset_words, ctx);
                    emitAggregateInitToAddress(*fieldExpr, field.type, child, ctx);
                    ctx.release(child);
                } else {
                    emitScalarStoreAtAddress(*fieldExpr, field.type, baseReg,
                                             field.offset_words, ctx);
                }
            }
            return;
        }
        if (type.kind == TypeKind::Array && expr->kind == ExprKind::ArrayLiteral && type.element) {
            if (static_cast<int>(expr->args.size()) != type.array_len) {
                diag("array literal length " + std::to_string(expr->args.size()) +
                     " does not match " + std::to_string(type.array_len), expr->span);
            }
            const TypeRef elem = *type.element;
            const int elemSize = std::max(1, typeSizeWords(elem, layout_table_));
            const int count = std::min(static_cast<int>(expr->args.size()), type.array_len);
            for (int i = 0; i < count; ++i) {
                const int offset = i * elemSize;
                if (isAggregateType(elem)) {
                    int child = ctx.acquire();
                    ctx.line("copy r" + std::to_string(child) + ", r" + std::to_string(baseReg));
                    ctx.value(InstrOpcode::AddrOf, TypeRef::pointer(elem), expr->span, child);
                    addImmediateToReg(child, offset, ctx);
                    emitAggregateInitToAddress(expr->args[static_cast<std::size_t>(i)],
                                               elem, child, ctx);
                    ctx.release(child);
                } else {
                    emitScalarStoreAtAddress(expr->args[static_cast<std::size_t>(i)],
                                             elem, baseReg, offset, ctx);
                }
            }
            return;
        }

        ExprCode source = emitExpr(expr, type, ctx);
        if (!source.address || !isAggregateType(source.type)) {
            diag("aggregate assignment requires an aggregate value", expr->span);
            ctx.release(source.reg);
            return;
        }
        emitAggregateCopy(source.reg, baseReg, type, ctx);
        ctx.release(source.reg);
    }

    [[nodiscard]] ExprCode emitBinary(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        TypeRef target = expected.kind == TypeKind::Numeric ? expected : TypeRef::unknown();
        ExprCode lhs = emitExpr(expr.left, target, ctx);
        ExprCode rhs = emitExpr(expr.right, lhs.type, ctx);
        TypeRef common = commonNumericType(lhs.type, rhs.type);
        emitCvtIfNeeded(lhs, common, ctx);
        emitCvtIfNeeded(rhs, common, ctx);

        if (isComparison(expr.text)) {
            int rCmp = ctx.acquire();
            ctx.line("tcmp." + std::string(ir::suffix(common.scalar)) + " r" +
                     std::to_string(rCmp) + ", r" + std::to_string(lhs.reg) +
                     ", r" + std::to_string(rhs.reg));
            ValueId cmp_val = ctx.value(InstrOpcode::Cmp, TypeRef::numeric(ir::Type::T1), expr.span, rCmp);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {lhs.value, rhs.value};
            }

            ctx.release(lhs.reg);
            ctx.release(rhs.reg);

            int valNeg = -1, valZero = -1, valPos = -1;
            if (expr.text == "<") {
                valNeg = 1; valZero = -1; valPos = -1;
            } else if (expr.text == "<=") {
                valNeg = 1; valZero = 1; valPos = -1;
            } else if (expr.text == ">") {
                valNeg = -1; valZero = -1; valPos = 1;
            } else if (expr.text == ">=") {
                valNeg = -1; valZero = 1; valPos = 1;
            } else if (expr.text == "==") {
                valNeg = -1; valZero = 1; valPos = -1;
            } else if (expr.text == "!=") {
                valNeg = 1; valZero = -1; valPos = 1;
            }

            int rTrue = ctx.acquire();
            ctx.line("mov.t1 r" + std::to_string(rTrue) + ", 1");
            ValueId true_val = ctx.value(InstrOpcode::Const, TypeRef::trit(), expr.span, rTrue);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().imm = 1;
            }
            int rFalse = ctx.acquire();
            ctx.line("mov.t1 r" + std::to_string(rFalse) + ", 1");
            ctx.line("neg.t1 r" + std::to_string(rFalse) + ", r" + std::to_string(rFalse)); // -1
            ValueId false_val = ctx.value(InstrOpcode::Const, TypeRef::trit(), expr.span, rFalse);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().imm = -1;
            }

            std::string negReg = (valNeg == 1) ? "r" + std::to_string(rTrue) : (valNeg == 0) ? "r0" : "r" + std::to_string(rFalse);
            std::string zeroReg = (valZero == 1) ? "r" + std::to_string(rTrue) : (valZero == 0) ? "r0" : "r" + std::to_string(rFalse);
            std::string posReg = (valPos == 1) ? "r" + std::to_string(rTrue) : (valPos == 0) ? "r0" : "r" + std::to_string(rFalse);

            int rOut = ctx.acquire();
            ctx.line("tsel r" + std::to_string(rOut) + ", r" + std::to_string(rCmp) +
                     ", " + negReg + ", " + zeroReg + ", " + posReg);

            ctx.release(rCmp);
            ctx.release(rTrue);
            ctx.release(rFalse);

            ValueId id = ctx.value(InstrOpcode::Tsel, TypeRef::trit(), expr.span, rOut);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {cmp_val,
                    (valNeg == 1) ? true_val : (valNeg == 0) ? -1 : false_val,
                    (valZero == 1) ? true_val : (valZero == 0) ? -1 : false_val,
                    (valPos == 1) ? true_val : (valPos == 0) ? -1 : false_val};
            }
            return ExprCode{rOut, TypeRef::trit(), false, id};
        }

        std::string mnemonic = expr.text == "+" ? "add" :
                               expr.text == "-" ? "sub" :
                               expr.text == "*" ? "mul" : "div";
        ctx.line(mnemonic + "." + std::string(ir::suffix(common.scalar)) + " r" +
                 std::to_string(lhs.reg) + ", r" + std::to_string(lhs.reg) +
                 ", r" + std::to_string(rhs.reg));
        ValueId id = ctx.value(expr.text == "+" ? InstrOpcode::Add :
                               expr.text == "-" ? InstrOpcode::Sub :
                               expr.text == "*" ? InstrOpcode::Mul : InstrOpcode::Div,
                               common, expr.span, lhs.reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {lhs.value, rhs.value};
        }
        ctx.release(rhs.reg);
        return ExprCode{lhs.reg, common, false, id};
    }

    [[nodiscard]] ExprCode emitCall(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        if (expr.text == "shared_alloc" || expr.text == "atomic_load" || expr.text == "atomic_store") {
            return emitAtomicOp(expr, expected, ctx);
        }
        if (isRuntimeWrapper(expr.text)) return emitRuntimeCall(expr, expected, ctx);
        if (isUnsafeIntrinsic(expr.text)) {
            if (!ctx.unsafe_allowed) {
                diag("raw CSR/atomic intrinsics require an unsafe block", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            return emitUnsafeIntrinsic(expr, expected, ctx);
        }
        auto retIt = ctx.function_returns.find(expr.text);
        if (retIt == ctx.function_returns.end()) {
            diag("unknown function '" + expr.text + "'", expr.span);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }
        const auto paramIt = ctx.function_params.find(expr.text);
        std::vector<ValueId> arg_values;
        const std::size_t argc = expr.args.size();
        const int saved_call_arg_depth = ctx.call_arg_depth;
        if (saved_call_arg_depth >= kCallArgScratchAreas) {
            diag("nested function calls exceed bootstrap call scratch depth", expr.span);
        }
        ctx.max_call_arg_depth =
            std::max(ctx.max_call_arg_depth,
                     std::min(saved_call_arg_depth + 1, kCallArgScratchAreas));
        const int scratch_base =
            ctx.call_arg_slot_base +
            std::min(saved_call_arg_depth, kCallArgScratchAreas - 1) * kCallArgScratchWords;
        ctx.call_arg_depth =
            std::min(saved_call_arg_depth + 1, kCallArgScratchAreas);
        struct CallArgumentSlot {
            TypeRef type;
            int scratch_word = 0;
            int width = 1;
            int register_word = -1;
            int stack_word = -1;
        };
        std::vector<CallArgumentSlot> argument_slots;
        int scratch_word_count = 0;
        int register_word_count = 0;
        int stack_word_count = 0;
        for (std::size_t i = 0; i < argc; ++i) {
            TypeRef argument_type = TypeRef::numeric(ir::Type::T40);
            if (paramIt != ctx.function_params.end() && i < paramIt->second.size()) {
                argument_type = paramIt->second[i];
            }
            ExprCode arg = emitExpr(expr.args[i], argument_type, ctx);
            if (isAggregateType(argument_type) && !arg.address) {
                diag("aggregate arguments are passed by pointer in the v2 ABI", expr.args[i]->span);
            }
            const int width = isAggregateType(argument_type)
                ? 1
                : std::max(1, typeSizeWords(argument_type, layout_table_));
            CallArgumentSlot slot;
            slot.type = argument_type;
            slot.scratch_word = scratch_word_count;
            slot.width = width;
            if (register_word_count + width <= kRegisterArgCount) {
                slot.register_word = register_word_count;
                register_word_count += width;
            } else {
                slot.stack_word = stack_word_count;
                stack_word_count += width;
            }
            ctx.line(scalarMemoryMnemonic("store", argument_type) +
                     " r" + std::to_string(arg.reg) + ", sp, " +
                     std::to_string(scratch_base + scratch_word_count));
            scratch_word_count += width;
            argument_slots.push_back(slot);
            arg_values.push_back(arg.value);
            ctx.release(arg.reg);
        }
        if (scratch_word_count > kCallArgScratchWords) {
            diag("function-call arguments require " +
                     std::to_string(scratch_word_count) +
                     " scratch words, exceeding the bootstrap limit of " +
                     std::to_string(kCallArgScratchWords),
                 expr.span);
        }
        ctx.call_arg_depth = saved_call_arg_depth;
        if (stack_word_count > 0) {
            ctx.line("mov.t40 r24, " + std::to_string(stack_word_count));
            ctx.line("sub.t40 sp, sp, r24");
        }
        for (const auto& slot : argument_slots) {
            const int shifted_scratch =
                scratch_base + stack_word_count + slot.scratch_word;
            if (slot.register_word >= 0) {
                ctx.line(scalarMemoryMnemonic("load", slot.type) +
                         " r" + std::to_string(13 + slot.register_word) +
                         ", sp, " + std::to_string(shifted_scratch));
            } else {
                const int scratch = slot.width == 2 ? 23 : 24;
                ctx.line(scalarMemoryMnemonic("load", slot.type) +
                         " r" + std::to_string(scratch) + ", sp, " +
                         std::to_string(shifted_scratch));
                ctx.line(scalarMemoryMnemonic("store", slot.type) +
                         " r" + std::to_string(scratch) + ", sp, " +
                         std::to_string(slot.stack_word));
            }
        }
        ctx.line("call " + expr.text);
        if (stack_word_count > 0) {
            ctx.line("mov.t40 r24, " + std::to_string(stack_word_count));
            ctx.line("add.t40 sp, sp, r24");
        }
        TypeRef ret = retIt->second;
        int out = ctx.acquire();
        ctx.line(std::string("copy") + (usesWideT50Pair(ret) ? ".t50" : "") +
                 " r" + std::to_string(out) + ", r13");
        ValueId id = ctx.value(InstrOpcode::Call, ret, expr.span, out);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().symbol = expr.text;
            ctx.block->instructions.back().args = arg_values;
            ctx.block->instructions.back().effect = Effect::Control;
        }
        return ExprCode{out, ret, false, id};
    }

    [[nodiscard]] ExprCode emitRuntimeCall(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        const int service = runtimeService(expr.text);
        if (expr.args.size() > 4)
            diag("syscall ABI v2 accepts at most four arguments",
                 expr.span);
        std::vector<ValueId> arg_values;
        const std::size_t argc =
            std::min<std::size_t>(expr.args.size(), 4);
        const int saved_call_arg_depth = ctx.call_arg_depth;
        if (saved_call_arg_depth >= kCallArgScratchAreas) {
            diag("nested syscall wrapper calls exceed bootstrap call scratch depth", expr.span);
        }
        ctx.max_call_arg_depth =
            std::max(ctx.max_call_arg_depth,
                     std::min(saved_call_arg_depth + 1, kCallArgScratchAreas));
        const int scratch_base =
            ctx.call_arg_slot_base +
            std::min(saved_call_arg_depth, kCallArgScratchAreas - 1) * kCallArgScratchWords;
        ctx.call_arg_depth =
            std::min(saved_call_arg_depth + 1, kCallArgScratchAreas);
        for (std::size_t i = 0; i < argc; ++i) {
            ExprCode arg = emitExpr(expr.args[i], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line("store r" + std::to_string(arg.reg) + ", sp, " +
                     std::to_string(scratch_base + static_cast<int>(i)));
            arg_values.push_back(arg.value);
            ctx.release(arg.reg);
        }
        ctx.call_arg_depth = saved_call_arg_depth;
        for (std::size_t i = 0; i < argc; ++i) {
            if (i == 0) {
                ctx.line("load r24, sp, " + std::to_string(scratch_base));
                ctx.line("copy r13, r24");
            } else {
                ctx.line("load r" + std::to_string(13 + static_cast<int>(i)) +
                         ", sp, " +
                         std::to_string(scratch_base + static_cast<int>(i)));
            }
        }
        if ((service == runtime::sys_write_int || service == runtime::sys_write_char) && argc > 0) {
            ctx.line("copy r1, r13");
        }
        ctx.line("syscall " + std::to_string(service));
        int out = ctx.acquire();
        ctx.line("copy r" + std::to_string(out) + ", r" +
                 std::to_string(runtimeReturnsPayload(service) ? 14 : 13));
        ValueId id = ctx.value(InstrOpcode::Syscall, TypeRef::numeric(ir::Type::T40), expr.span, out);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().aux = service;
            ctx.block->instructions.back().args = arg_values;
            ctx.block->instructions.back().effect = Effect::Syscall;
        }
        (void)expected;
        return ExprCode{out, TypeRef::numeric(ir::Type::T40), false, id};
    }

    [[nodiscard]] ExprCode emitAtomicOp(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        if (expr.text == "shared_alloc") {
            ExprCode val = emitExpr(expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            
            ctx.line("mov.t40 r13, 1");
            ctx.line("syscall 19");
            
            int rAddr = ctx.acquire();
            ctx.line("copy r" + std::to_string(rAddr) + ", r13");
            TypeRef sharedType = TypeRef::shared(val.type, MemoryOrder::AcquireRelease);
            ValueId syscall_id = ctx.value(InstrOpcode::Syscall, sharedType, expr.span, rAddr);
            
            ValueId final_id = -1;
            auto it = ctx.reg_to_value.find(rAddr);
            if (it != ctx.reg_to_value.end()) {
                final_id = it->second;
            }

            ctx.line("store r" + std::to_string(val.reg) + ", r" + std::to_string(rAddr) + ", 0");
            ctx.value(InstrOpcode::Store, val.type, expr.span);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {final_id, val.value};
            }
            ctx.release(val.reg);
            
            return ExprCode{rAddr, sharedType, false, final_id};
        }
        if (expr.text == "atomic_load") {
            ExprCode addr = emitExpr(expr.args[0], TypeRef::unknown(), ctx);
            int order = isa::ATOMIC_ORDER_ACQ_REL;
            (void)extractInteger(expr.args[1], order);

            int out = ctx.acquire();
            ctx.line("tldr" + ir::memoryOrderSuffix(order) + " r" +
                     std::to_string(out) + ", r" + std::to_string(addr.reg));
            ctx.release(addr.reg);

            TypeRef elemType = addr.type.element ? *addr.type.element : TypeRef::numeric(ir::Type::T40);

            Instr instr;
            instr.def = ctx.next_value++;
            ctx.reg_to_value[out] = instr.def;
            instr.opcode = InstrOpcode::Tldr;
            instr.type = elemType;
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            instr.args = {addr.value};
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, elemType, false, instr.def};
        }
        if (expr.text == "atomic_store") {
            ExprCode addr = emitExpr(expr.args[0], TypeRef::unknown(), ctx);
            TypeRef elemType = addr.type.element ? *addr.type.element : TypeRef::numeric(ir::Type::T40);
            ExprCode val = emitExpr(expr.args[1], elemType, ctx);
            int order = isa::ATOMIC_ORDER_ACQ_REL;
            (void)extractInteger(expr.args[2], order);

            std::string L_loop = ctx.label("atomic_store_loop");
            
            ctx.raw(L_loop + ":");
            
            int rCurr = ctx.acquire();
            ctx.line("tldr" + ir::memoryOrderSuffix(order) + " r" +
                     std::to_string(rCurr) + ", r" + std::to_string(addr.reg));
            ValueId curr_id = ctx.value(InstrOpcode::Tldr, elemType, expr.span, rCurr);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {addr.value};
            }
            
            int rStatus = ctx.acquire();
            ctx.line("tstr" + ir::memoryOrderSuffix(order) + " r" +
                     std::to_string(rStatus) + ", r" + std::to_string(addr.reg) +
                     ", r" + std::to_string(val.reg) + ", r" + std::to_string(rCurr));
            ValueId status_id = ctx.value(InstrOpcode::Tstr, TypeRef::numeric(ir::Type::T1), expr.span, rStatus);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {addr.value, val.value, curr_id};
            }
            
            ctx.release(rCurr);
            
            int rCmp = ctx.acquire();
            ctx.line("tcmp.t1 r" + std::to_string(rCmp) + ", r" + std::to_string(rStatus) + ", r0");
            ValueId cmp_id = ctx.value(InstrOpcode::Cmp, TypeRef::numeric(ir::Type::T1), expr.span, rCmp);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {status_id};
            }
            
            ctx.release(rStatus);
            
            ctx.line("brn r" + std::to_string(rCmp) + ", " + L_loop);
            ctx.line("brz r" + std::to_string(rCmp) + ", " + L_loop);
            
            ctx.release(rCmp);

            ctx.release(addr.reg);
            ctx.release(val.reg);

            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }
        (void)expected;
        return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
    }

    [[nodiscard]] ExprCode emitUnsafeIntrinsic(
        const Expr& expr,
        TypeRef expected,
        FunctionContext& ctx) {

        if (expr.text == "csr_read") {
            if (expr.args.size() != 1 || !expr.args[0] ||
                expr.args[0]->kind != ExprKind::Name) {
                diag("csr_read requires a CSR name argument", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            const int csr = vm::assembler::parseCSR(expr.args[0]->text);
            if (!isa::isValidCSR(csr)) {
                diag("csr_read requires a valid CSR name", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            int out = ctx.acquire();
            ctx.line("csrr r" + std::to_string(out) + ", " + std::string(isa::csrToString(csr)));
            Instr instr;
            instr.def = ctx.next_value++;
            ctx.reg_to_value[out] = instr.def;
            instr.opcode = InstrOpcode::Csrr;
            instr.type = TypeRef::numeric(ir::Type::T40);
            instr.aux = csr;
            instr.effect = Effect::CSR;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T40), false, instr.def};
        }

        if (expr.text == "csr_write") {
            if (expr.args.size() != 2 || !expr.args[0] ||
                expr.args[0]->kind != ExprKind::Name) {
                diag("csr_write requires CSR name and value arguments", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            const int csr = vm::assembler::parseCSR(expr.args[0]->text);
            if (!isa::isValidCSR(csr)) {
                diag("csr_write requires a valid CSR name", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ExprCode value = emitExpr(expr.args[1], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line("csrw " + std::string(isa::csrToString(csr)) + ", r" +
                     std::to_string(value.reg));
            ctx.release(value.reg);
            Instr instr;
            instr.opcode = InstrOpcode::Csrw;
            instr.type = TypeRef::voidType();
            instr.aux = csr;
            instr.effect = Effect::CSR;
            instr.span = expr.span;
            instr.args = {value.value};
            ctx.block->instructions.push_back(instr);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }

        if (expr.text == "arch_wait") {
            if (!expr.args.empty()) {
                diag("arch_wait takes no arguments", expr.span);
            }
            ctx.line("wait");
            Instr instr;
            instr.opcode = InstrOpcode::Wait;
            instr.type = TypeRef::voidType();
            instr.effect = Effect::Control;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return emitImmediate(
                0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }

        if (expr.text == "tlbinv") {
            if (expr.args.size() != 3) {
                diag("tlbinv requires address, ASID, and scope", expr.span);
                return emitImmediate(
                    0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ExprCode address = emitExpr(
                expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode target_asid = emitExpr(
                expr.args[1], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode scope = emitExpr(
                expr.args[2], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line(
                "tlbinv r" + std::to_string(address.reg) +
                ", r" + std::to_string(target_asid.reg) +
                ", r" + std::to_string(scope.reg));
            Instr instr;
            instr.opcode = InstrOpcode::TlbInv;
            instr.type = TypeRef::voidType();
            instr.effect = Effect::Control;
            instr.span = expr.span;
            instr.args = {address.value, target_asid.value, scope.value};
            ctx.block->instructions.push_back(instr);
            ctx.release(address.reg);
            ctx.release(target_asid.reg);
            ctx.release(scope.reg);
            return emitImmediate(
                0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }

        if (expr.text == "tmod") {
            if (expr.args.size() != 2) {
                diag("tmod requires dividend and divisor", expr.span);
                return emitImmediate(
                    0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ExprCode dividend = emitExpr(
                expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode divisor = emitExpr(
                expr.args[1], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line(
                "tmod.t40 r" + std::to_string(dividend.reg) +
                ", r" + std::to_string(dividend.reg) +
                ", r" + std::to_string(divisor.reg));
            const ValueId result = ctx.value(
                InstrOpcode::Tmod,
                TypeRef::numeric(ir::Type::T40),
                expr.span,
                dividend.reg);
            if (ctx.block && !ctx.block->instructions.empty()) {
                ctx.block->instructions.back().args = {
                    dividend.value, divisor.value};
            }
            ctx.release(divisor.reg);
            return ExprCode{
                dividend.reg,
                TypeRef::numeric(ir::Type::T40),
                false,
                result};
        }

        if (expr.text == "fence") {
            int order = isa::ATOMIC_ORDER_ACQ_REL;
            if (!expr.args.empty()) {
                if (!extractInteger(expr.args[0], order) || !isa::isAtomicOrder(order)) {
                    diag("fence order must be -1, 0, or +1", expr.span);
                }
            }
            ctx.line("fence" + ir::memoryOrderSuffix(order));
            Instr instr;
            instr.opcode = InstrOpcode::Fence;
            instr.type = TypeRef::voidType();
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }

        if (expr.text == "load") {
            if (expr.args.empty()) {
                diag("load requires an address", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ExprCode addr = emitExpr(expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            int out = ctx.acquire();
            ctx.line("load r" + std::to_string(out) + ", r" + std::to_string(addr.reg) + ", 0");
            ctx.release(addr.reg);
            Instr instr;
            instr.def = ctx.next_value++;
            ctx.reg_to_value[out] = instr.def;
            instr.opcode = InstrOpcode::Load;
            instr.type = TypeRef::numeric(ir::Type::T40);
            instr.effect = Effect::ReadMem;
            instr.span = expr.span;
            instr.args = {addr.value};
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T40), false, instr.def};
        }

        if (expr.text == "store") {
            if (expr.args.size() < 2) {
                diag("store requires address and value arguments", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            ExprCode addr = emitExpr(expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode val = emitExpr(expr.args[1], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line("store r" + std::to_string(val.reg) + ", r" + std::to_string(addr.reg) + ", 0");
            ctx.release(addr.reg);
            ctx.release(val.reg);
            Instr instr;
            instr.opcode = InstrOpcode::Store;
            instr.type = TypeRef::voidType();
            instr.effect = Effect::WriteMem;
            instr.span = expr.span;
            instr.args = {addr.value, val.value};
            ctx.block->instructions.push_back(instr);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }

        if (expr.text == "tldr") {
            if (expr.args.empty()) {
                diag("tldr requires address and optional order", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
            }
            int order = isa::ATOMIC_ORDER_ACQ_REL;
            if (expr.args.size() >= 2 &&
                (!extractInteger(expr.args[1], order) || !isa::isAtomicOrder(order))) {
                diag("tldr order must be -1, 0, or +1", expr.span);
            }
            ExprCode addr = emitExpr(expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            int out = ctx.acquire();
            ctx.line("tldr" + ir::memoryOrderSuffix(order) + " r" +
                     std::to_string(out) + ", r" + std::to_string(addr.reg));
            ctx.release(addr.reg);
            Instr instr;
            instr.def = ctx.next_value++;
            ctx.reg_to_value[out] = instr.def;
            instr.opcode = InstrOpcode::Tldr;
            instr.type = TypeRef::numeric(ir::Type::T40);
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            instr.args = {addr.value};
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T40), false, instr.def};
        }

        if (expr.text == "tstr") {
            if (expr.args.size() < 3) {
                diag("tstr requires address, desired, expected, and optional order", expr.span);
                return emitImmediate(0, TypeRef::numeric(ir::Type::T1), expr.span, ctx);
            }
            int order = isa::ATOMIC_ORDER_ACQ_REL;
            if (expr.args.size() >= 4 &&
                (!extractInteger(expr.args[3], order) || !isa::isAtomicOrder(order))) {
                diag("tstr order must be -1, 0, or +1", expr.span);
            }
            ExprCode addr = emitExpr(expr.args[0], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode desired = emitExpr(expr.args[1], TypeRef::numeric(ir::Type::T40), ctx);
            ExprCode expectedValue = emitExpr(expr.args[2], TypeRef::numeric(ir::Type::T40), ctx);
            int out = ctx.acquire();
            ctx.line("tstr" + ir::memoryOrderSuffix(order) + " r" +
                     std::to_string(out) + ", r" + std::to_string(addr.reg) +
                     ", r" + std::to_string(desired.reg) +
                     ", r" + std::to_string(expectedValue.reg));
            ctx.release(addr.reg);
            ctx.release(desired.reg);
            ctx.release(expectedValue.reg);
            Instr instr;
            instr.def = ctx.next_value++;
            ctx.reg_to_value[out] = instr.def;
            instr.opcode = InstrOpcode::Tstr;
            instr.type = TypeRef::numeric(ir::Type::T1);
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            instr.args = {addr.value, desired.value, expectedValue.value};
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T1), false, instr.def};
        }

        (void)expected;
        diag("unknown unsafe intrinsic '" + expr.text + "'", expr.span);
        return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
    }

    [[nodiscard]] static bool isRuntimeWrapper(const std::string& name) {
        return runtimeService(name) != 0;
    }

    [[nodiscard]] static int runtimeService(const std::string& name) {
        if (name == "sys_write_int") return runtime::sys_write_int;
        if (name == "sys_newline") return runtime::sys_newline;
        if (name == "sys_clear") return runtime::sys_clear;
        if (name == "sys_exit") return runtime::sys_exit;
        if (name == "sys_getpid") return runtime::sys_getpid;
        if (name == "sys_uptime") return runtime::sys_uptime;
        if (name == "sys_read_console_word") return runtime::sys_read_console_word;
        if (name == "sys_spawn_static") return runtime::sys_spawn_static;
        if (name == "sys_waitpid") return runtime::sys_waitpid;
        if (name == "sys_open") return runtime::sys_open;
        if (name == "sys_close") return runtime::sys_close;
        if (name == "sys_read") return runtime::sys_read;
        if (name == "sys_write") return runtime::sys_write;
        if (name == "sys_stat") return runtime::sys_stat;
        if (name == "sys_readdir") return runtime::sys_readdir;
        if (name == "sys_brk") return runtime::sys_brk;
        if (name == "sys_sbrk") return runtime::sys_sbrk;
        if (name == "sys_fork") return runtime::sys_fork;
        if (name == "sys_exec") return runtime::sys_exec;
        if (name == "sys_write_char") return runtime::sys_write_char;
        if (name == "sys_ipc_send") return runtime::sys_ipc_send;
        if (name == "sys_ipc_recv") return runtime::sys_ipc_recv;
        if (name == "sys_fb_init") return runtime::sys_fb_init;
        if (name == "sys_fb_flip") return runtime::sys_fb_flip;
        if (name == "sys_window_create") return runtime::sys_window_create;
        if (name == "sys_window_get_buffer") return runtime::sys_window_get_buffer;
        if (name == "sys_window_present") return runtime::sys_window_present;
        if (name == "sys_window_move") return runtime::sys_window_move;
        if (name == "sys_window_set_z") return runtime::sys_window_set_z;
        if (name == "sys_window_destroy") return runtime::sys_window_destroy;
        if (name == "sys_window_read_event") return runtime::sys_window_read_event;
        if (name == "sys_window_resize") return runtime::sys_window_resize;
        if (name == "sys_window_request_close") return runtime::sys_window_request_close;
        if (name == "sys_socket") return runtime::sys_socket;
        if (name == "sys_bind") return runtime::sys_bind;
        if (name == "sys_connect") return runtime::sys_connect;
        if (name == "sys_send") return runtime::sys_send;
        if (name == "sys_recv") return runtime::sys_recv;
        if (name == "sys_mkdir") return runtime::sys_mkdir;
        if (name == "sys_unlink") return runtime::sys_unlink;
        if (name == "sys_sleep") return runtime::sys_sleep;
        if (name == "sys_yield") return runtime::sys_yield;
        if (name == "sys_sleep_until_tick")
            return runtime::sys_sleep_until_tick;
        if (name == "sys_ps") return runtime::sys_ps;
        if (name == "sys_fsync") return runtime::sys_fsync;
        if (name == "sys_kill") return runtime::sys_kill;
        if (name == "sys_suspend") return runtime::sys_suspend;
        if (name == "sys_resume") return runtime::sys_resume;
        if (name == "sys_getproc") return runtime::sys_getproc;
        if (name == "sys_futex_wait") return runtime::sys_futex_wait;
        if (name == "sys_futex_wake") return runtime::sys_futex_wake;
        if (name == "sys_ipc_recv_blocking") return runtime::sys_ipc_recv_blocking;
        if (name == "sys_wait_event") return runtime::sys_wait_event;
        if (name == "sys_sleep_ms") return runtime::sys_sleep_ms;
        if (name == "sys_app_spawn") return runtime::sys_app_spawn;
        if (name == "sys_reboot") return runtime::sys_reboot;
        return 0;
    }

    [[nodiscard]] static bool runtimeReturnsPayload(int service) {
        return service == runtime::sys_getpid ||
               service == runtime::sys_uptime ||
               service == runtime::sys_read_console_word ||
               service == runtime::sys_open ||
               service == runtime::sys_read ||
               service == runtime::sys_write ||
               service == runtime::sys_stat ||
               service == runtime::sys_readdir ||
               service == runtime::sys_fork ||
               service == runtime::sys_exec ||
               service == runtime::sys_ipc_send ||
               service == runtime::sys_ipc_recv ||
               service == runtime::sys_fb_init ||
               service == runtime::sys_fb_flip ||
               service == runtime::sys_window_create ||
               service == runtime::sys_window_get_buffer ||
               service == runtime::sys_window_present ||
               service == runtime::sys_window_read_event ||
               service == runtime::sys_window_request_close ||
               service == runtime::sys_socket ||
               service == runtime::sys_bind ||
               service == runtime::sys_connect ||
               service == runtime::sys_send ||
               service == runtime::sys_recv ||
               service == runtime::sys_mkdir ||
               service == runtime::sys_unlink ||
               service == runtime::sys_waitpid ||
               service == runtime::sys_exit ||
               service == runtime::sys_sleep ||
               service == runtime::sys_ps ||
               service == runtime::sys_fsync ||
               service == runtime::sys_kill ||
               service == runtime::sys_suspend ||
               service == runtime::sys_resume ||
               service == runtime::sys_getproc ||
               service == runtime::sys_futex_wait ||
               service == runtime::sys_futex_wake ||
               service == runtime::sys_ipc_recv_blocking ||
               service == runtime::sys_wait_event ||
               service == runtime::sys_sleep_ms ||
               service == runtime::sys_app_spawn ||
               service == runtime::sys_reboot;
    }

    [[nodiscard]] static bool isUnsafeIntrinsic(const std::string& name) {
        return name == "csr_read" || name == "csr_write" ||
               name == "arch_wait" ||
               name == "tlbinv" || name == "tmod" ||
               name == "tldr" || name == "tstr" || name == "fence" ||
               name == "load" || name == "store";
    }

    void emitCvtIfNeeded(ExprCode& code, const TypeRef& target, FunctionContext& ctx) {
        if (code.type.kind != TypeKind::Numeric || target.kind != TypeKind::Numeric) return;
        if (code.type.scalar == target.scalar) return;
        if (!canWiden(code.type, target)) return;
        ctx.line("cvt." + std::string(ir::suffix(code.type.scalar)) + "." +
                 ir::suffix(target.scalar) + " r" + std::to_string(code.reg) +
                 ", r" + std::to_string(code.reg));
        ValueId id = ctx.value(InstrOpcode::Cvt, target, SourceSpan{}, code.reg);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {code.value};
        }
        code.type = target;
        code.value = id;
    }

    void diag(const std::string& message, const SourceSpan& span) {
        diagnostics_.push_back({DiagnosticSeverity::Error, message, span});
    }
};

// =============================================================================
// Verifier and optimizer
// =============================================================================

[[nodiscard]] inline std::vector<Diagnostic> verifyModule(const Module& module) {
    std::vector<Diagnostic> diagnostics;
    for (const auto& fn : module.functions) {
        if (fn.blocks.empty()) {
            diagnostics.push_back({DiagnosticSeverity::Error,
                "function '" + fn.name + "' has no basic blocks", SourceSpan{module.name, 1, 1, 1}});
            continue;
        }
        const ControlFlowGraph cfg = buildControlFlowGraph(fn);
        const DominanceInfo dominance = computeDominance(fn, cfg);
        for (const std::string& target : cfg.invalid_targets) {
            diagnostics.push_back({DiagnosticSeverity::Error,
                "unknown CFG target '" + target + "' in function '" +
                    fn.name + "'",
                SourceSpan{module.name, 1, 1, 1}});
        }

        struct Definition {
            std::string block;
            int instruction = -1;
        };
        std::map<ValueId, Definition> definitions;
        for (const auto& block : fn.blocks) {
            for (std::size_t index = 0;
                 index < block.instructions.size(); ++index) {
                const Instr& instr = block.instructions[index];
                if (instr.def < 0) continue;
                if (definitions.count(instr.def)) {
                    diagnostics.push_back({DiagnosticSeverity::Error,
                        "duplicate SSA definition for value " +
                            std::to_string(instr.def),
                        instr.span});
                } else {
                    definitions[instr.def] = {
                        block.name, static_cast<int>(index)};
                }
            }
        }

        auto validateUse = [&](ValueId value,
                               const std::string& use_block,
                               int use_instruction,
                               const SourceSpan& span) {
            if (value < 0) return;
            const auto found = definitions.find(value);
            if (found == definitions.end()) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "use-before-def for value " + std::to_string(value) +
                    " in function '" + fn.name + "'", span});
                return;
            }
            const Definition& def = found->second;
            const bool valid =
                !fn.cfg_complete ||
                (def.block == use_block
                    ? def.instruction < use_instruction
                    : dominance.dominates(def.block, use_block));
            if (!valid) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "SSA definition does not dominate use in function '" +
                        fn.name + "'",
                    span});
            }
        };

        for (const auto& block : fn.blocks) {
            if (fn.cfg_complete &&
                block.terminator.kind == TerminatorKind::None) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "basic block '" + block.name + "' has no terminator",
                    SourceSpan{module.name, 1, 1, 1}});
            }
            for (std::size_t index = 0;
                 index < block.instructions.size(); ++index) {
                const Instr& instr = block.instructions[index];
                if (instr.opcode == InstrOpcode::Phi) {
                    std::set<std::string> incoming_predecessors;
                    for (const auto& incoming : instr.phi_incoming) {
                        incoming_predecessors.insert(incoming.first);
                        const auto found = definitions.find(incoming.second);
                        if (found == definitions.end() ||
                            (fn.cfg_complete &&
                            !dominance.dominates(
                                found->second.block, incoming.first))) {
                            diagnostics.push_back({DiagnosticSeverity::Error,
                                "phi incoming value does not dominate predecessor",
                                instr.span});
                        }
                    }
                    if (fn.cfg_complete &&
                        incoming_predecessors !=
                        cfg.predecessors.at(block.name)) {
                        diagnostics.push_back({DiagnosticSeverity::Error,
                            "phi incoming predecessor set does not match CFG",
                            instr.span});
                    }
                } else {
                    for (ValueId arg : instr.args) {
                        validateUse(arg, block.name,
                                    static_cast<int>(index), instr.span);
                    }
                }
                if (instr.opcode == InstrOpcode::Csrr ||
                    instr.opcode == InstrOpcode::Csrw ||
                    instr.opcode == InstrOpcode::Csrrw) {
                    if (!isa::isValidCSR(instr.aux)) {
                        diagnostics.push_back({DiagnosticSeverity::Error,
                            "invalid CSR id in structural IR", instr.span});
                    }
                }
                if (instr.opcode == InstrOpcode::Fence ||
                    instr.opcode == InstrOpcode::Tldr ||
                    instr.opcode == InstrOpcode::Tstr) {
                    if (!isa::isAtomicOrder(instr.aux)) {
                        diagnostics.push_back({DiagnosticSeverity::Error,
                            "invalid memory-order trit in structural IR", instr.span});
                    }
                }
            }
            validateUse(block.terminator.condition, block.name,
                        static_cast<int>(block.instructions.size()),
                        SourceSpan{module.name, 1, 1, 1});
        }
    }
    return diagnostics;
}

[[nodiscard]] inline bool isPureInstruction(const Instr& instr) {
    return instr.effect == Effect::Pure &&
           instr.opcode != InstrOpcode::Alloca &&
           instr.opcode != InstrOpcode::Store &&
           instr.opcode != InstrOpcode::Syscall &&
           instr.opcode != InstrOpcode::Wait &&
           instr.opcode != InstrOpcode::TlbInv &&
           instr.opcode != InstrOpcode::Fence &&
           instr.opcode != InstrOpcode::Tldr &&
           instr.opcode != InstrOpcode::Tstr &&
           instr.opcode != InstrOpcode::Csrr &&
           instr.opcode != InstrOpcode::Csrw &&
           instr.opcode != InstrOpcode::Csrrw &&
           instr.opcode != InstrOpcode::Call &&
           instr.opcode != InstrOpcode::CallR &&
           instr.opcode != InstrOpcode::Ret;
}

[[nodiscard]] inline bool isSpeculatableInstruction(
    const Instr& instr) {
    if (!isPureInstruction(instr)) return false;
    switch (instr.opcode) {
        case InstrOpcode::Param:
        case InstrOpcode::Const:
        case InstrOpcode::Copy:
        case InstrOpcode::Add:
        case InstrOpcode::Sub:
        case InstrOpcode::Mul:
        case InstrOpcode::Cvt:
        case InstrOpcode::Cmp:
        case InstrOpcode::Tsel:
        case InstrOpcode::FieldAddr:
        case InstrOpcode::IndexAddr:
        case InstrOpcode::AddrOf:
            return true;
        default:
            // Division, remainder, pointer dereference, and target-specific
            // operations may trap and cannot be moved to paths that did not
            // execute them in the source CFG.
            return false;
    }
}

// Costs used by transforms that trade control flow for eager computation.
// Keep this table exhaustive: a transform must have a target cost for every
// instruction it selects before it is allowed to rewrite the CFG.  The
// values are relative v2 instruction costs, not host-cycle promises.
struct TargetCostTable {
    static constexpr int unavailable = -1;
    static constexpr int branch3 = 3; // brn, brz, and the fall-through jump
    static constexpr int jump = 1;
    static constexpr int tsel = 1;

    [[nodiscard]] static constexpr int instruction(InstrOpcode opcode) {
        switch (opcode) {
            case InstrOpcode::Alloca: return 2;
            case InstrOpcode::Param: return 0;
            case InstrOpcode::Const: return 1;
            case InstrOpcode::Copy: return 1;
            case InstrOpcode::Add:
            case InstrOpcode::Sub:
            case InstrOpcode::Mul:
            case InstrOpcode::Cvt:
            case InstrOpcode::Cmp:
            case InstrOpcode::FieldAddr:
            case InstrOpcode::IndexAddr:
            case InstrOpcode::AddrOf:
                return 1;
            case InstrOpcode::Div:
            case InstrOpcode::Tmod:
                return 4;
            case InstrOpcode::Tsel:
                return tsel;
            case InstrOpcode::Phi:
                return 0;
            case InstrOpcode::Deref:
            case InstrOpcode::Load:
            case InstrOpcode::Store:
            case InstrOpcode::SpillLoad:
            case InstrOpcode::SpillStore:
                return 3;
            case InstrOpcode::Syscall:
            case InstrOpcode::Call:
            case InstrOpcode::CallR:
                return 8;
            case InstrOpcode::Wait:
            case InstrOpcode::TlbInv:
            case InstrOpcode::Fence:
            case InstrOpcode::Tldr:
            case InstrOpcode::Tstr:
            case InstrOpcode::Csrr:
            case InstrOpcode::Csrw:
            case InstrOpcode::Csrrw:
                return 5;
            case InstrOpcode::Ret:
                return 1;
            case InstrOpcode::Swap:
                return 2;
            case InstrOpcode::Nop:
                return 0;
        }
        return unavailable;
    }
};

// Constant branch folding is only valid when eliminating the other arms
// cannot erase an observable operation. A side effect may be in a successor
// several blocks away, so walk each arm's reachable region before replacing
// a Branch3 with a Jump.
[[nodiscard]] inline bool branchRegionHasObservableEffects(
    const Function& function,
    const ControlFlowGraph& cfg,
    const BasicBlock& block) {
    std::vector<std::string> work = {
        block.terminator.target_neg,
        block.terminator.target_zero,
        block.terminator.target_pos};
    std::set<std::string> visited;
    while (!work.empty()) {
        const std::string current = work.back();
        work.pop_back();
        if (current.empty() || !visited.insert(current).second) continue;
        const auto index = cfg.index.find(current);
        if (index == cfg.index.end()) continue;
        const BasicBlock& successor =
            function.blocks[static_cast<std::size_t>(index->second)];
        for (const Instr& instr : successor.instructions) {
            if (instr.effect != Effect::Pure) return true;
        }
        for (const std::string& next : cfg.successors.at(current))
            work.push_back(next);
    }
    return false;
}

inline void addInterferenceEdge(
    std::map<ValueId, std::set<ValueId>>& graph,
    ValueId a,
    ValueId b) {

    if (a < 0 || b < 0 || a == b) return;
    graph[a].insert(b);
    graph[b].insert(a);
}

[[nodiscard]] inline AllocationResult allocateRegisters(
    const Module& module,
    const CompilerOptions& options) {

    AllocationResult result;
    const std::vector<int> scalarColors = {
        19, 20, 21, 22, 23,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
    };
    const std::vector<int> calleePreferred = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<int> vectorColors = {0, 1, 2, 3, 4, 5, 6, 7};
    // r24 is reserved by the target emitter while it materializes frame
    // addresses and adjusts the stack pointer. Keep it out of every
    // allocator palette, including spill-rewrite temporaries, so frame
    // memory lowering cannot clobber a live SSA value.
    // r13/r14 are reserved by direct SSA tuple-swap lowering as its two
    // memory temporaries. They remain available for the ABI at calls and
    // syscalls, but no allocated SSA value may be live in them at a Swap.
    const std::vector<int> spillTemporaryColors =
        {15, 16, 17, 18};

    std::map<ValueId, TypeRef> valueTypes;
    std::map<ValueId, std::set<ValueId>> graph;
    std::set<std::pair<ValueId, ValueId>> moveEdges;
    std::set<ValueId> liveAcrossCalls;
    std::set<ValueId> spillTemporaries;
    std::set<ValueId> frameValues;
    for (const Function& fn : module.functions) {
        for (const BasicBlock& block : fn.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Alloca &&
                    instr.def >= 0) {
                    frameValues.insert(instr.def);
                }
            }
        }
    }

    for (const auto& fn : module.functions) {
        const ControlFlowGraph cfg = buildControlFlowGraph(fn);
        const BlockLiveness block_liveness =
            computeBlockLiveness(fn, cfg);
        for (const auto& block : fn.blocks) {
            std::set<ValueId> live =
                block_liveness.live_out.count(block.name)
                    ? block_liveness.live_out.at(block.name)
                    : std::set<ValueId>{};
            for (ValueId frame : frameValues)
                live.erase(frame);
            if (block.terminator.condition >= 0 &&
                !frameValues.count(
                    block.terminator.condition)) {
                live.insert(block.terminator.condition);
            }
            for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
                const Instr& instr = *it;
                if (instr.opcode == InstrOpcode::Call || instr.opcode == InstrOpcode::CallR ||
                    instr.opcode == InstrOpcode::Syscall) {
                    for (ValueId value : live) {
                        if (!frameValues.count(value))
                            liveAcrossCalls.insert(value);
                    }
                }
                std::vector<ValueId> uses;
                for (ValueId arg : instr.args) {
                    if (arg >= 0 && !frameValues.count(arg))
                        uses.push_back(arg);
                }
                for (std::size_t i = 0; i < uses.size(); ++i) {
                    for (std::size_t j = i + 1; j < uses.size(); ++j) {
                        addInterferenceEdge(graph, uses[i], uses[j]);
                    }
                }
                if (instr.def >= 0 &&
                    !frameValues.count(instr.def)) {
                    valueTypes[instr.def] = instr.type;
                    if (instr.spill_temporary)
                        spillTemporaries.insert(instr.def);
                    graph[instr.def];
                    for (ValueId value : live) addInterferenceEdge(graph, instr.def, value);
                    live.erase(instr.def);
                } else if (instr.def >= 0) {
                    live.erase(instr.def);
                }
                if (instr.opcode == InstrOpcode::Copy && instr.def >= 0 &&
                    !frameValues.count(instr.def) &&
                    instr.args.size() == 1 && instr.args[0] >= 0 &&
                    !frameValues.count(instr.args[0])) {
                    ValueId a = instr.def;
                    ValueId b = instr.args[0];
                    if (a > b) std::swap(a, b);
                    moveEdges.insert({a, b});
                }
                for (ValueId arg : instr.args) {
                    if (arg >= 0 && !frameValues.count(arg))
                        live.insert(arg);
                }
            }
        }
    }

    // Function parameters are materialized by one parallel ABI move set at
    // the start of the emitted function.  Their SSA lifetimes can be
    // disjoint (for example, when the frontend first stores each parameter
    // into a local), but the ABI sources are all consumed before any of those
    // moves execute.  Keep parameters of the same register class interfering
    // so the allocator cannot assign two simultaneous ABI arguments to one
    // destination register and silently overwrite an earlier argument.
    for (const auto& fn : module.functions) {
        std::vector<ValueId> parameters;
        for (const BasicBlock& block : fn.blocks) {
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Param &&
                    instr.def >= 0 && !frameValues.count(instr.def)) {
                    parameters.push_back(instr.def);
                }
            }
        }
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            for (std::size_t j = i + 1; j < parameters.size(); ++j) {
                const ValueId lhs = parameters[i];
                const ValueId rhs = parameters[j];
                const auto lhs_type = valueTypes.find(lhs);
                const auto rhs_type = valueTypes.find(rhs);
                if (lhs_type == valueTypes.end() ||
                    rhs_type == valueTypes.end()) {
                    continue;
                }
                if ((lhs_type->second.kind == TypeKind::Vector) !=
                    (rhs_type->second.kind == TypeKind::Vector)) {
                    continue;
                }
                addInterferenceEdge(graph, lhs, rhs);
            }
        }
    }

    auto registerWidth = [&](ValueId value) {
        auto type = valueTypes.find(value);
        if (type == valueTypes.end()) return 1;
        return ((type->second.kind == TypeKind::Numeric ||
                 type->second.kind == TypeKind::Lane) &&
                type->second.scalar == ir::Type::T50)
            ? 2
            : 1;
    };
    auto sameRegisterClass = [&](ValueId lhs, ValueId rhs) {
        return (valueTypes[lhs].kind == TypeKind::Vector) ==
               (valueTypes[rhs].kind == TypeKind::Vector);
    };
    auto effectiveColorCount = [&](ValueId value) {
        if (valueTypes[value].kind == TypeKind::Vector)
            return static_cast<int>(vectorColors.size());
        if (spillTemporaries.count(value))
            return static_cast<int>(spillTemporaryColors.size());
        if (liveAcrossCalls.count(value))
            return static_cast<int>(calleePreferred.size());
        if (registerWidth(value) == 2) return 8;
        return static_cast<int>(scalarColors.size());
    };

    // Loop-weighted use counts drive spill selection. A use in a natural loop
    // is deliberately more expensive than one on a cold straight-line path.
    std::map<ValueId, long long> spill_cost;
    for (const auto& fn : module.functions) {
        const ControlFlowGraph cfg = buildControlFlowGraph(fn);
        const DominanceInfo dominance = computeDominance(fn, cfg);
        std::map<std::string, int> loop_depth;
        for (const std::string& block : cfg.order) loop_depth[block] = 0;
        for (const std::string& latch : cfg.order) {
            for (const std::string& header : cfg.successors.at(latch)) {
                if (!dominance.dominates(header, latch)) continue;
                std::set<std::string> loop{header, latch};
                std::vector<std::string> work{latch};
                while (!work.empty()) {
                    const std::string current = work.back();
                    work.pop_back();
                    for (const std::string& pred :
                         cfg.predecessors.at(current)) {
                        if (loop.insert(pred).second && pred != header)
                            work.push_back(pred);
                    }
                }
                for (const std::string& member : loop)
                    ++loop_depth[member];
            }
        }
        for (const BasicBlock& block : fn.blocks) {
            long long weight = 1;
            for (int depth = 0;
                 depth < std::min(3, loop_depth[block.name]); ++depth) {
                weight *= 10;
            }
            for (const Instr& instr : block.instructions) {
                if (instr.def >= 0 &&
                    !frameValues.count(instr.def))
                    spill_cost[instr.def] += weight;
                for (ValueId arg : instr.args)
                    if (arg >= 0 &&
                        !frameValues.count(arg))
                        spill_cost[arg] += weight;
                for (const auto& incoming : instr.phi_incoming)
                    if (incoming.second >= 0 &&
                        !frameValues.count(incoming.second))
                        spill_cost[incoming.second] += weight;
            }
            if (block.terminator.condition >= 0 &&
                !frameValues.count(
                    block.terminator.condition))
                spill_cost[block.terminator.condition] += weight;
        }
    }

    std::set<ValueId> move_related;
    for (const auto& move : moveEdges) {
        move_related.insert(move.first);
        move_related.insert(move.second);
    }
    std::set<ValueId> remaining;
    for (const auto& entry : valueTypes) remaining.insert(entry.first);
    std::vector<ValueId> simplify_stack;
    simplify_stack.reserve(remaining.size());
    auto remainingDegree = [&](ValueId value) {
        int degree = 0;
        for (ValueId neighbor : graph[value]) {
            if (remaining.count(neighbor) &&
                sameRegisterClass(value, neighbor)) {
                ++degree;
            }
        }
        return degree;
    };
    while (!remaining.empty()) {
        ValueId selected = -1;
        for (ValueId value : remaining) {
            if (!move_related.count(value) &&
                remainingDegree(value) < effectiveColorCount(value)) {
                selected = value;
                break;
            }
        }
        if (selected < 0) {
            for (ValueId value : remaining) {
                if (remainingDegree(value) <
                    effectiveColorCount(value)) {
                    selected = value;
                    ++result.freeze_steps;
                    move_related.erase(value);
                    break;
                }
            }
        }
        if (selected < 0) {
            // Minimize weighted cost per current interference edge.
            long double best_score =
                std::numeric_limits<long double>::max();
            for (ValueId value : remaining) {
                const int degree = std::max(1, remainingDegree(value));
                const long double score =
                    static_cast<long double>(
                        std::max<long long>(1, spill_cost[value])) /
                    static_cast<long double>(degree);
                if (score < best_score) {
                    best_score = score;
                    selected = value;
                }
            }
            ++result.spill_candidates;
        }
        simplify_stack.push_back(selected);
        remaining.erase(selected);
        ++result.simplify_steps;
    }
    std::vector<ValueId> values(
        simplify_stack.rbegin(), simplify_stack.rend());
    auto rangesOverlap = [](int lhs, int lhs_width,
                            int rhs, int rhs_width) {
        return lhs < rhs + rhs_width && rhs < lhs + lhs_width;
    };
    auto colorAvailable = [&](ValueId value, int color,
                              const std::map<ValueId, int>& colors) {
        const int width = registerWidth(value);
        if (width == 2 && color + 1 >= isa::REG_COUNT) return false;
        for (ValueId neighbor : graph[value]) {
            auto it = colors.find(neighbor);
            if (it != colors.end() &&
                rangesOverlap(color, width, it->second,
                              registerWidth(neighbor))) {
                return false;
            }
        }
        return true;
    };

    int nextSpillSlot = 0;
    for (ValueId value : values) {
        const TypeRef type = valueTypes[value];
        const bool vector = type.kind == TypeKind::Vector;
        std::map<ValueId, int>& colors = vector ? result.vector_registers
                                                : result.scalar_registers;
        const std::vector<int>& base_palette = vector ? vectorColors :
            (spillTemporaries.count(value) ? spillTemporaryColors :
             (liveAcrossCalls.count(value) ? calleePreferred : scalarColors));
        std::vector<int> pair_palette;
        const std::vector<int>* palette_ptr = &base_palette;
        if (!vector && registerWidth(value) == 2) {
            for (int color : base_palette) {
                if (color + 1 >= isa::REG_COUNT) continue;
                if (color == 12 || color == 18 || color == 24 ||
                    color + 1 == 24 ||
                    color == isa::R25_LR || color == isa::R26_SP) {
                    continue;
                }
                pair_palette.push_back(color);
            }
            palette_ptr = &pair_palette;
        }
        const std::vector<int>& palette = *palette_ptr;

        bool assigned = false;
        for (const auto& move : moveEdges) {
            ValueId peer = move.first == value ? move.second :
                           move.second == value ? move.first : -1;
            if (peer < 0 || graph[value].count(peer)) continue;
            auto it = colors.find(peer);
            if (it != colors.end() &&
                std::find(palette.begin(), palette.end(), it->second) != palette.end() &&
                colorAvailable(value, it->second, colors)) {
                colors[value] = it->second;
                ++result.coalesced_moves;
                assigned = true;
                break;
            }
        }
        if (assigned) continue;

        for (int color : palette) {
            if (colorAvailable(value, color, colors)) {
                colors[value] = color;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            if (options.enable_spilling) {
                result.spill_slots[value] = nextSpillSlot;
                nextSpillSlot += 9;
                ++result.spills;
                continue;
            }
            result.diagnostics.push_back({DiagnosticSeverity::Error,
                "register allocation failed for value " + std::to_string(value),
                SourceSpan{module.name, 1, 1, 1}});
        }
    }

    for (const auto& entry : result.scalar_registers) {
        const int reg = entry.second;
        if (reg >= 1 && reg <= 12) result.callee_saved_used.insert(reg);
        if (registerWidth(entry.first) == 2 &&
            reg + 1 >= 1 && reg + 1 <= 12) {
            result.callee_saved_used.insert(reg + 1);
        }
        if (liveAcrossCalls.count(entry.first) &&
            ((reg >= 19 && reg <= 24) || (reg >= 13 && reg <= 18))) {
            result.caller_saved_live_across_calls.insert(reg);
        }
    }
    for (const auto& entry : graph) result.interference_edges += static_cast<int>(entry.second.size());
    result.interference_edges /= 2;
    result.success = result.diagnostics.empty();
    return result;
}

[[nodiscard]] inline AllocationResult allocateRegistersWithSpillRewrite(
    Module& module,
    const CompilerOptions& options,
    int max_rounds) {

    AllocationResult final_result;
    if (max_rounds < 1) max_rounds = 1;
    std::map<ValueId, int> all_spill_slots;
    int total_spills = 0;
    int total_loads = 0;
    int total_stores = 0;
    int next_slot = 0;
    int next_value = 1;
    for (const Function& fn : module.functions) {
        next_value = std::max(next_value, fn.ir_value_ceiling);
        for (const BasicBlock& block : fn.blocks) {
            for (const Instr& instr : block.instructions) {
                next_value = std::max(next_value, instr.def + 1);
                if ((instr.opcode == InstrOpcode::SpillLoad ||
                     instr.opcode == InstrOpcode::SpillStore) &&
                    instr.aux >= 0) {
                    next_slot = std::max(next_slot, instr.aux + 9);
                }
            }
        }
    }

    for (int round = 0; round < max_rounds; ++round) {
        AllocationResult allocation =
            allocateRegisters(module, options);
        if (!allocation.diagnostics.empty()) {
            allocation.spill_slots.insert(
                all_spill_slots.begin(), all_spill_slots.end());
            allocation.spills = total_spills + allocation.spills;
            allocation.spill_rewrite_rounds = round;
            return allocation;
        }
        if (allocation.spill_slots.empty()) {
            allocation.spill_slots = all_spill_slots;
            allocation.spills = total_spills;
            allocation.spill_rewrite_rounds = round;
            allocation.spill_loads = total_loads;
            allocation.spill_stores = total_stores;
            allocation.success = true;
            return allocation;
        }

        std::map<ValueId, int> current_slots;
        for (const auto& spill : allocation.spill_slots) {
            current_slots[spill.first] = next_slot;
            all_spill_slots[spill.first] = next_slot;
            next_slot += 9;
        }
        total_spills += static_cast<int>(current_slots.size());

        std::map<ValueId, TypeRef> value_types;
        for (const Function& fn : module.functions) {
            for (const BasicBlock& block : fn.blocks) {
                for (const Instr& instr : block.instructions) {
                    if (instr.def >= 0) value_types[instr.def] = instr.type;
                }
            }
        }
        auto spillType = [&](ValueId value) {
            const auto found = value_types.find(value);
            return found == value_types.end()
                ? TypeRef::numeric(ir::Type::T40)
                : found->second;
        };
        auto makeLoad = [&](ValueId original,
                            const SourceSpan& span) {
            Instr load;
            load.def = next_value++;
            load.opcode = InstrOpcode::SpillLoad;
            load.type = spillType(original);
            load.aux = current_slots.at(original);
            load.symbol = "$spill";
            load.effect = Effect::ReadMem;
            load.span = span;
            load.spill_temporary = true;
            ++total_loads;
            return load;
        };
        auto makeStore = [&](ValueId original,
                             ValueId source,
                             const SourceSpan& span) {
            Instr store;
            store.opcode = InstrOpcode::SpillStore;
            store.type = TypeRef::voidType();
            store.args = {source};
            store.aux = current_slots.at(original);
            store.symbol = "$spill";
            store.effect = Effect::WriteMem;
            store.span = span;
            ++total_stores;
            return store;
        };

        for (Function& fn : module.functions) {
            const ControlFlowGraph cfg = buildControlFlowGraph(fn);
            if (!cfg.invalid_targets.empty()) continue;

            // Phi operands are edge uses. Materialize each spilled incoming
            // value in its predecessor, once per (predecessor,value) pair.
            std::map<std::pair<std::string, ValueId>, ValueId> edge_loads;
            for (BasicBlock& block : fn.blocks) {
                for (Instr& phi : block.instructions) {
                    if (phi.opcode != InstrOpcode::Phi) break;
                    for (auto& incoming : phi.phi_incoming) {
                        if (!current_slots.count(incoming.second)) continue;
                        const auto key =
                            std::make_pair(incoming.first, incoming.second);
                        auto existing = edge_loads.find(key);
                        if (existing == edge_loads.end()) {
                            Instr load =
                                makeLoad(incoming.second, phi.span);
                            const ValueId replacement = load.def;
                            fn.blocks[cfg.index.at(incoming.first)]
                                .instructions.push_back(std::move(load));
                            existing = edge_loads.emplace(
                                key, replacement).first;
                        }
                        incoming.second = existing->second;
                    }
                }
            }

            for (BasicBlock& block : fn.blocks) {
                std::vector<Instr> rewritten;
                rewritten.reserve(block.instructions.size() * 2);
                std::vector<Instr> phi_stores;
                std::size_t index = 0;
                while (index < block.instructions.size() &&
                       block.instructions[index].opcode ==
                           InstrOpcode::Phi) {
                    Instr phi = std::move(block.instructions[index++]);
                    if (current_slots.count(phi.def)) {
                        const ValueId original = phi.def;
                        phi.def = next_value++;
                        phi_stores.push_back(
                            makeStore(original, phi.def, phi.span));
                    }
                    rewritten.push_back(std::move(phi));
                }
                rewritten.insert(
                    rewritten.end(),
                    std::make_move_iterator(phi_stores.begin()),
                    std::make_move_iterator(phi_stores.end()));

                for (; index < block.instructions.size(); ++index) {
                    Instr instr =
                        std::move(block.instructions[index]);
                    std::map<ValueId, ValueId> loaded_for_instruction;
                    for (ValueId& argument : instr.args) {
                        if (!current_slots.count(argument)) continue;
                        auto loaded =
                            loaded_for_instruction.find(argument);
                        if (loaded == loaded_for_instruction.end()) {
                            Instr load = makeLoad(argument, instr.span);
                            const ValueId replacement = load.def;
                            rewritten.push_back(std::move(load));
                            loaded = loaded_for_instruction.emplace(
                                argument, replacement).first;
                        }
                        argument = loaded->second;
                    }
                    if (current_slots.count(instr.def)) {
                        const ValueId original = instr.def;
                        instr.def = next_value++;
                        instr.spill_temporary = true;
                        const ValueId replacement = instr.def;
                        rewritten.push_back(std::move(instr));
                        rewritten.push_back(
                            makeStore(
                                original, replacement,
                                rewritten.back().span));
                    } else {
                        rewritten.push_back(std::move(instr));
                    }
                }
                if (current_slots.count(block.terminator.condition)) {
                    Instr load = makeLoad(
                        block.terminator.condition, SourceSpan{});
                    block.terminator.condition = load.def;
                    rewritten.push_back(std::move(load));
                }
                block.instructions = std::move(rewritten);
            }
            fn.ir_value_ceiling = next_value;
        }
    }

    final_result = allocateRegisters(module, options);
    const int unresolved_spills = final_result.spills;
    final_result.spill_slots = all_spill_slots;
    final_result.spills = total_spills + unresolved_spills;
    final_result.spill_rewrite_rounds = max_rounds;
    final_result.spill_loads = total_loads;
    final_result.spill_stores = total_stores;
    if (unresolved_spills > 0) {
        final_result.diagnostics.push_back({
            DiagnosticSeverity::Error,
            "spill rewrite did not converge after " +
                std::to_string(max_rounds) + " rounds",
            SourceSpan{module.name, 1, 1, 1}});
        final_result.success = false;
    }
    return final_result;
}

[[nodiscard]] inline std::string instrKey(const Instr& instr) {
    std::ostringstream out;
    out << static_cast<int>(instr.opcode) << "|" << instr.type.str() << "|";
    for (ValueId arg : instr.args) out << arg << ",";
    out << "|" << instr.imm << "|" << instr.aux << "|" << instr.symbol << "|"
        << static_cast<int>(instr.effect);
    return out.str();
}

inline ValueId resolveCopy(ValueId value, const std::map<ValueId, ValueId>& copies) {
    std::set<ValueId> seen;
    while (copies.count(value) && !seen.count(value)) {
        seen.insert(value);
        value = copies.at(value);
    }
    return value;
}

[[nodiscard]] inline bool evaluateSsaConstant(
    const Instr& instr,
    const std::map<ValueId, long long>& constants,
    long long& value) {
    auto argument = [&](std::size_t index, long long& out) {
        if (index >= instr.args.size()) return false;
        const auto found = constants.find(instr.args[index]);
        if (found == constants.end()) return false;
        out = found->second;
        return true;
    };
    if (instr.opcode == InstrOpcode::Const) {
        value = instr.imm;
        return true;
    }
    if (instr.opcode == InstrOpcode::Copy) {
        return argument(0, value);
    }
    if (instr.opcode == InstrOpcode::Phi) {
        bool first = true;
        long long common = 0;
        for (const auto& incoming : instr.phi_incoming) {
            const auto found = constants.find(incoming.second);
            if (found == constants.end()) return false;
            if (first) {
                common = found->second;
                first = false;
            } else if (common != found->second) {
                return false;
            }
        }
        if (first) return false;
        value = common;
        return true;
    }
    long long lhs = 0;
    long long rhs = 0;
    if (instr.opcode == InstrOpcode::Cmp &&
        argument(0, lhs) && argument(1, rhs)) {
        value = lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
        return true;
    }
    if (instr.opcode == InstrOpcode::Tsel &&
        instr.args.size() == 4 && argument(0, lhs)) {
        const std::size_t selected = lhs < 0 ? 1 : (lhs > 0 ? 3 : 2);
        if (instr.args[selected] < 0) {
            value = 0;
            return true;
        }
        const auto found = constants.find(instr.args[selected]);
        if (found == constants.end()) return false;
        value = found->second;
        return true;
    }
    if (!argument(0, lhs) || !argument(1, rhs)) return false;
    switch (instr.opcode) {
        case InstrOpcode::Add: value = lhs + rhs; return true;
        case InstrOpcode::Sub: value = lhs - rhs; return true;
        case InstrOpcode::Mul: value = lhs * rhs; return true;
        case InstrOpcode::Div:
            if (rhs == 0) return false;
            value = lhs / rhs;
            return true;
        case InstrOpcode::Tmod:
            if (rhs == 0) return false;
            value = lhs % rhs;
            return true;
        default:
            return false;
    }
}

inline void rewriteAsConstant(Instr& instr, long long value) {
    instr.opcode = InstrOpcode::Const;
    instr.args.clear();
    instr.phi_incoming.clear();
    instr.imm = value;
    instr.effect = Effect::Pure;
}

inline int runSparseConditionalConstantPropagation(Function& fn) {
    if (fn.blocks.empty()) return 0;
    const ControlFlowGraph cfg = buildControlFlowGraph(fn);
    if (!cfg.invalid_targets.empty()) return 0;

    enum class LatticeKind : uint8_t { Unknown, Constant, Overdefined };
    struct LatticeValue {
        LatticeKind kind = LatticeKind::Unknown;
        long long value = 0;
    };
    std::map<ValueId, LatticeValue> values;
    std::map<std::string, bool> executable;
    std::deque<std::string> worklist;

    auto stateOf = [&](ValueId value) {
        const auto found = values.find(value);
        return found == values.end() ? LatticeValue{} : found->second;
    };
    auto mergeValue = [&](ValueId value, LatticeValue incoming) {
        if (value < 0 || incoming.kind == LatticeKind::Unknown) return false;
        LatticeValue& current = values[value];
        if (current.kind == LatticeKind::Overdefined) return false;
        if (current.kind == LatticeKind::Unknown) {
            current = incoming;
            return true;
        }
        if (incoming.kind == LatticeKind::Overdefined ||
            current.value != incoming.value) {
            current.kind = LatticeKind::Overdefined;
            return true;
        }
        return false;
    };
    auto markExecutable = [&](const std::string& name) {
        if (name.empty() || executable[name]) return false;
        executable[name] = true;
        worklist.push_back(name);
        return true;
    };
    auto enqueueExecutable = [&]() {
        for (const BasicBlock& block : fn.blocks) {
            if (executable[block.name]) worklist.push_back(block.name);
        }
    };

    auto evaluate = [&](const Instr& instr) {
        if (instr.def < 0) return LatticeValue{};
        if (instr.opcode == InstrOpcode::Const)
            return LatticeValue{LatticeKind::Constant, instr.imm};
        auto argument = [&](std::size_t index) {
            return index < instr.args.size()
                ? stateOf(instr.args[index])
                : LatticeValue{LatticeKind::Overdefined, 0};
        };
        if (instr.opcode == InstrOpcode::Copy) {
            return argument(0);
        }
        if (instr.opcode == InstrOpcode::Phi) {
            LatticeValue result{};
            bool saw_incoming = false;
            for (const auto& incoming : instr.phi_incoming) {
                if (!executable[incoming.first]) continue;
                saw_incoming = true;
                const LatticeValue candidate = stateOf(incoming.second);
                if (candidate.kind == LatticeKind::Unknown) continue;
                if (result.kind == LatticeKind::Unknown) {
                    result = candidate;
                } else if (candidate.kind == LatticeKind::Overdefined ||
                           result.kind == LatticeKind::Overdefined ||
                           result.value != candidate.value) {
                    result.kind = LatticeKind::Overdefined;
                }
            }
            return saw_incoming ? result : LatticeValue{};
        }
        const LatticeValue lhs = argument(0);
        const LatticeValue rhs = argument(1);
        if (instr.opcode == InstrOpcode::Tsel)
            // TSEL is an intentional v2 target operation. Preserve it in
            // optimized SSA even when all four operands are currently
            // constant so branch-free match lowering remains observable and
            // target emission does not regress to a replay path.
            return LatticeValue{LatticeKind::Overdefined, 0};
        if (lhs.kind != LatticeKind::Constant ||
            rhs.kind != LatticeKind::Constant) {
            return lhs.kind == LatticeKind::Overdefined ||
                           rhs.kind == LatticeKind::Overdefined
                ? LatticeValue{LatticeKind::Overdefined, 0}
                : LatticeValue{};
        }
        long long result = 0;
        const auto addOverflow = [](long long a, long long b) {
            return (b > 0 && a > std::numeric_limits<long long>::max() - b) ||
                   (b < 0 && a < std::numeric_limits<long long>::min() - b);
        };
        const auto subOverflow = [](long long a, long long b) {
            return (b < 0 && a > std::numeric_limits<long long>::max() + b) ||
                   (b > 0 && a < std::numeric_limits<long long>::min() + b);
        };
        const auto mulOverflow = [](long long a, long long b) {
            if (a == 0 || b == 0) return false;
            if (a == -1) return b == std::numeric_limits<long long>::min();
            if (b == -1) return a == std::numeric_limits<long long>::min();
            if (a > 0) {
                if (b > 0)
                    return a > std::numeric_limits<long long>::max() / b;
                return b < std::numeric_limits<long long>::min() / a;
            }
            if (b > 0)
                return a < std::numeric_limits<long long>::min() / b;
            return a < std::numeric_limits<long long>::max() / b;
        };
        switch (instr.opcode) {
            case InstrOpcode::Add:
                if (addOverflow(lhs.value, rhs.value))
                    return LatticeValue{LatticeKind::Overdefined, 0};
                result = lhs.value + rhs.value;
                return LatticeValue{LatticeKind::Constant, result};
            case InstrOpcode::Sub:
                if (subOverflow(lhs.value, rhs.value))
                    return LatticeValue{LatticeKind::Overdefined, 0};
                result = lhs.value - rhs.value;
                return LatticeValue{LatticeKind::Constant, result};
            case InstrOpcode::Mul:
                if (mulOverflow(lhs.value, rhs.value))
                    return LatticeValue{LatticeKind::Overdefined, 0};
                result = lhs.value * rhs.value;
                return LatticeValue{LatticeKind::Constant, result};
            case InstrOpcode::Div:
            case InstrOpcode::Tmod:
                if (rhs.value == 0 ||
                    (lhs.value == std::numeric_limits<long long>::min() &&
                     rhs.value == -1))
                    return LatticeValue{LatticeKind::Overdefined, 0};
                result = instr.opcode == InstrOpcode::Div
                    ? lhs.value / rhs.value
                    : lhs.value % rhs.value;
                return LatticeValue{LatticeKind::Constant, result};
            case InstrOpcode::Cmp:
                return LatticeValue{LatticeKind::Constant,
                                    lhs.value < rhs.value ? -1 :
                                    lhs.value > rhs.value ? 1 : 0};
            default:
                return LatticeValue{LatticeKind::Overdefined, 0};
        }
    };

    markExecutable(fn.blocks.front().name);
    while (!worklist.empty()) {
        const std::string block_name = worklist.front();
        worklist.pop_front();
        const auto block_index = cfg.index.find(block_name);
        if (block_index == cfg.index.end()) continue;
        BasicBlock& block = fn.blocks[static_cast<std::size_t>(block_index->second)];
        bool state_changed = false;
        for (const Instr& instr : block.instructions) {
            state_changed = mergeValue(instr.def, evaluate(instr)) || state_changed;
        }
        const auto markAll = [&]() {
            for (const std::string& successor : cfg.successors.at(block.name))
                markExecutable(successor);
        };
        switch (block.terminator.kind) {
            case TerminatorKind::Jump:
                markExecutable(block.terminator.target);
                break;
            case TerminatorKind::Branch3: {
                const LatticeValue condition = stateOf(block.terminator.condition);
                if (condition.kind == LatticeKind::Constant &&
                    !branchRegionHasObservableEffects(
                        fn, cfg, block)) {
                    markExecutable(condition.value < 0
                        ? block.terminator.target_neg
                        : condition.value > 0
                            ? block.terminator.target_pos
                            : block.terminator.target_zero);
                } else {
                    markAll();
                }
                break;
            }
            case TerminatorKind::Return:
            case TerminatorKind::None:
            case TerminatorKind::Halt:
                break;
        }
        if (state_changed) enqueueExecutable();
    }

    int rewritten = 0;
    for (BasicBlock& block : fn.blocks) {
        if (!executable[block.name]) continue;
        for (Instr& instr : block.instructions) {
            // Keep phi nodes explicit for the later edge-copy lowering pass.
            // Even when SCCP proves a single executable incoming edge, the
            // structural SSA form remains the authoritative representation
            // until CFG simplification removes that block explicitly.
            if (instr.def < 0 || instr.opcode == InstrOpcode::Const ||
                instr.opcode == InstrOpcode::Phi ||
                !isPureInstruction(instr)) continue;
            const LatticeValue value = stateOf(instr.def);
            if (value.kind != LatticeKind::Constant) continue;
            rewriteAsConstant(instr, value.value);
            ++rewritten;
        }
        if (block.terminator.kind == TerminatorKind::Branch3) {
            const LatticeValue condition = stateOf(block.terminator.condition);
            if (condition.kind == LatticeKind::Constant &&
                !branchRegionHasObservableEffects(fn, cfg, block)) {
                block.terminator.kind = TerminatorKind::Jump;
                block.terminator.target = condition.value < 0
                    ? block.terminator.target_neg
                    : condition.value > 0
                        ? block.terminator.target_pos
                        : block.terminator.target_zero;
            }
        }
    }
    return rewritten;
}

inline int runGlobalCopyPropagation(Function& fn) {
    const ControlFlowGraph cfg = buildControlFlowGraph(fn);
    if (!cfg.invalid_targets.empty() || fn.blocks.empty()) return 0;
    const DominanceInfo dominance = computeDominance(fn, cfg);

    // A copy is visible only after its defining instruction and only along a
    // path dominated by that definition.  The previous module-wide map could
    // rewrite a use in a sibling branch with a value defined on a different
    // path, turning a valid SSA graph into a latent use-before-definition.
    std::map<ValueId, ValueId> copies;
    std::map<ValueId, std::string> defining_block;
    std::map<ValueId, int> defining_index;
    for (const BasicBlock& block : fn.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            const Instr& instr = block.instructions[index];
            if (instr.def >= 0) {
                defining_block[instr.def] = block.name;
                defining_index[instr.def] = static_cast<int>(index);
            }
        }
    }

    auto visibleAt = [&](ValueId value,
                         const std::string& use_block,
                         int use_index) {
        const auto block_it = defining_block.find(value);
        if (block_it == defining_block.end()) return false;
        if (!dominance.dominates(block_it->second, use_block)) return false;
        if (block_it->second == use_block) {
            const auto index_it = defining_index.find(value);
            if (index_it == defining_index.end() ||
                index_it->second >= use_index) {
                return false;
            }
        }
        return true;
    };
    auto resolveAt = [&](ValueId value,
                         const std::string& use_block,
                         int use_index) {
        std::set<ValueId> seen;
        ValueId current = value;
        while (copies.count(current) && !seen.count(current)) {
            if (!visibleAt(current, use_block, use_index)) break;
            seen.insert(current);
            const ValueId next = copies.at(current);
            if (!visibleAt(next, use_block, use_index)) break;
            current = next;
        }
        return current;
    };

    int rewrites = 0;
    for (BasicBlock& block : fn.blocks) {
        for (std::size_t index = 0; index < block.instructions.size(); ++index) {
            Instr& instr = block.instructions[index];
            for (ValueId& arg : instr.args) {
                const ValueId resolved = resolveAt(
                    arg, block.name, static_cast<int>(index));
                if (resolved != arg) {
                    arg = resolved;
                    ++rewrites;
                }
            }
            if (instr.opcode == InstrOpcode::Phi) {
                for (auto& incoming : instr.phi_incoming) {
                    const auto predecessor = cfg.index.find(incoming.first);
                    if (predecessor == cfg.index.end()) continue;
                    const BasicBlock& pred =
                        fn.blocks[static_cast<std::size_t>(predecessor->second)];
                    const ValueId resolved = resolveAt(
                        incoming.second,
                        incoming.first,
                        static_cast<int>(pred.instructions.size()));
                    if (resolved != incoming.second) {
                        incoming.second = resolved;
                        ++rewrites;
                    }
                }
            }
            if (instr.opcode == InstrOpcode::Copy &&
                instr.def >= 0 && instr.args.size() == 1) {
                copies[instr.def] = instr.args.front();
            }
        }
        const ValueId resolved = resolveAt(
            block.terminator.condition,
            block.name,
            static_cast<int>(block.instructions.size()));
        if (resolved != block.terminator.condition) {
            block.terminator.condition = resolved;
            ++rewrites;
        }
    }
    return rewrites;
}

inline int runGlobalValueNumbering(Function& fn) {
    const ControlFlowGraph cfg = buildControlFlowGraph(fn);
    const DominanceInfo dominance = computeDominance(fn, cfg);
    std::map<std::string, std::pair<ValueId, std::string>> available;
    int hits = 0;
    for (BasicBlock& block : fn.blocks) {
        for (Instr& instr : block.instructions) {
            if (instr.def < 0 || !isPureInstruction(instr) ||
                instr.opcode == InstrOpcode::Const ||
                instr.opcode == InstrOpcode::Copy ||
                instr.opcode == InstrOpcode::Phi ||
                instr.opcode == InstrOpcode::Nop) {
                continue;
            }
            const std::string key = instrKey(instr);
            const auto found = available.find(key);
            if (found != available.end() &&
                dominance.dominates(found->second.second, block.name)) {
                instr.opcode = InstrOpcode::Copy;
                instr.args = {found->second.first};
                instr.effect = Effect::Pure;
                ++hits;
            } else {
                available[key] = {instr.def, block.name};
            }
        }
    }
    return hits;
}

inline int runCfgWideDeadCodeElimination(Function& fn) {
    int removed = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        std::map<ValueId, int> uses;
        for (const BasicBlock& block : fn.blocks) {
            for (const Instr& instr : block.instructions) {
                for (ValueId arg : instr.args) if (arg >= 0) ++uses[arg];
                for (const auto& incoming : instr.phi_incoming)
                    if (incoming.second >= 0) ++uses[incoming.second];
            }
            if (block.terminator.condition >= 0)
                ++uses[block.terminator.condition];
        }
        for (BasicBlock& block : fn.blocks) {
            const auto old_size = block.instructions.size();
            block.instructions.erase(
                std::remove_if(
                    block.instructions.begin(),
                    block.instructions.end(),
                    [&](const Instr& instr) {
                        return instr.opcode == InstrOpcode::Nop ||
                               (instr.def >= 0 && uses[instr.def] == 0 &&
                                isPureInstruction(instr));
                    }),
                block.instructions.end());
            if (block.instructions.size() != old_size) {
                removed += static_cast<int>(
                    old_size - block.instructions.size());
                changed = true;
            }
        }
    }
    return removed;
}

inline int runLoopInvariantCodeMotion(Function& fn) {
    const ControlFlowGraph cfg = buildControlFlowGraph(fn);
    const DominanceInfo dominance = computeDominance(fn, cfg);
    std::map<ValueId, std::string> defining_block;
    for (const BasicBlock& block : fn.blocks)
        for (const Instr& instr : block.instructions)
            if (instr.def >= 0) defining_block[instr.def] = block.name;
    int hoisted = 0;
    for (const std::string& latch : cfg.order) {
        for (const std::string& header : cfg.successors.at(latch)) {
            if (!dominance.dominates(header, latch)) continue;
            std::set<std::string> loop{header, latch};
            std::vector<std::string> work{latch};
            while (!work.empty()) {
                const std::string block = work.back();
                work.pop_back();
                for (const std::string& pred : cfg.predecessors.at(block)) {
                    if (loop.insert(pred).second && pred != header)
                        work.push_back(pred);
                }
            }
            std::vector<std::string> preheaders;
            for (const std::string& pred : cfg.predecessors.at(header))
                if (!loop.count(pred)) preheaders.push_back(pred);
            if (preheaders.size() != 1) continue;
            BasicBlock& preheader =
                fn.blocks[cfg.index.at(preheaders.front())];
            bool progress = true;
            while (progress) {
                progress = false;
                for (const std::string& block_name : loop) {
                    if (block_name == header) continue;
                    BasicBlock& block =
                        fn.blocks[cfg.index.at(block_name)];
                    for (auto it = block.instructions.begin();
                         it != block.instructions.end(); ++it) {
                        if (it->def < 0 ||
                            !isSpeculatableInstruction(*it) ||
                            it->opcode == InstrOpcode::Phi ||
                            it->opcode == InstrOpcode::Const) {
                            continue;
                        }
                        bool invariant = true;
                        for (ValueId arg : it->args) {
                            const auto def = defining_block.find(arg);
                            if (def != defining_block.end() &&
                                loop.count(def->second)) {
                                invariant = false;
                                break;
                            }
                        }
                        if (!invariant) continue;
                        preheader.instructions.push_back(std::move(*it));
                        defining_block[preheader.instructions.back().def] =
                            preheader.name;
                        block.instructions.erase(it);
                        ++hoisted;
                        progress = true;
                        break;
                    }
                    if (progress) break;
                }
            }
        }
    }
    return hoisted;
}

// Simplify redundant loop induction variables without changing arithmetic
// overflow or trap behavior.  Two header phis are equivalent when they have
// the same preheader value and the same constant Add/Sub recurrence.  Uses of
// the redundant phi and its backedge chain can then use the canonical IV;
// ordinary SSA DCE removes the duplicate recurrence.  A zero-step recurrence
// is handled as the degenerate form of the same proof.
inline int runInductionSimplification(Function& fn) {
    const ControlFlowGraph cfg = buildControlFlowGraph(fn);
    if (fn.blocks.empty() || !cfg.invalid_targets.empty()) return 0;
    const DominanceInfo dominance = computeDominance(fn, cfg);

    struct StepSignature {
        InstrOpcode opcode = InstrOpcode::Nop;
        long long constant = 0;
        TypeRef type = TypeRef::unknown();
        bool valid = false;
    };
    struct Candidate {
        ValueId phi = -1;
        ValueId initial = -1;
        ValueId backedge = -1;
        ValueId step = -1;
        StepSignature signature;
        std::set<std::string> loop;
    };

    std::map<ValueId, const Instr*> definitions;
    std::map<ValueId, std::string> defining_blocks;
    std::map<ValueId, int> defining_indices;
    std::map<ValueId, TypeRef> defining_types;
    std::map<ValueId, ValueId> copies;
    std::map<ValueId, long long> constants;
    for (const BasicBlock& block : fn.blocks) {
        for (std::size_t index = 0;
             index < block.instructions.size(); ++index) {
            const Instr& instr = block.instructions[index];
            if (instr.def < 0) continue;
            definitions[instr.def] = &instr;
            defining_blocks[instr.def] = block.name;
            defining_indices[instr.def] = static_cast<int>(index);
            defining_types[instr.def] = instr.type;
            if (instr.opcode == InstrOpcode::Copy &&
                instr.args.size() == 1 && instr.args.front() >= 0) {
                copies[instr.def] = instr.args.front();
            }
            if (instr.opcode == InstrOpcode::Const) {
                constants[instr.def] = instr.imm;
            }
        }
    }
    auto resolve = [&](ValueId value) {
        std::set<ValueId> seen;
        while (value >= 0 && copies.count(value) &&
               !seen.count(value)) {
            seen.insert(value);
            value = copies.at(value);
        }
        return value;
    };
    auto sameValue = [&](ValueId lhs, ValueId rhs) {
        lhs = resolve(lhs);
        rhs = resolve(rhs);
        if (lhs == rhs) return true;
        const auto left_constant = constants.find(lhs);
        const auto right_constant = constants.find(rhs);
        return left_constant != constants.end() &&
               right_constant != constants.end() &&
               defining_types.count(lhs) && defining_types.count(rhs) &&
               sameType(defining_types.at(lhs), defining_types.at(rhs)) &&
               left_constant->second == right_constant->second;
    };
    auto copyChain = [&](ValueId value) {
        std::vector<ValueId> chain;
        std::set<ValueId> seen;
        while (value >= 0 && !seen.count(value)) {
            chain.push_back(value);
            seen.insert(value);
            const auto next = copies.find(value);
            if (next == copies.end()) break;
            value = next->second;
        }
        return chain;
    };

    // Find natural loops using the same backedge/dominance proof as LICM.
    std::vector<std::pair<std::string, std::set<std::string>>> loops;
    for (const std::string& latch : cfg.order) {
        for (const std::string& header : cfg.successors.at(latch)) {
            if (!dominance.dominates(header, latch)) continue;
            std::set<std::string> loop{header, latch};
            std::vector<std::string> work{latch};
            while (!work.empty()) {
                const std::string block = work.back();
                work.pop_back();
                for (const std::string& predecessor :
                     cfg.predecessors.at(block)) {
                    if (loop.insert(predecessor).second &&
                        predecessor != header) {
                        work.push_back(predecessor);
                    }
                }
            }
            loops.push_back({header, std::move(loop)});
        }
    }

    std::vector<Candidate> candidates;
    for (const auto& loop_entry : loops) {
        const std::string& header = loop_entry.first;
        const std::set<std::string>& loop = loop_entry.second;
        const BasicBlock& block = fn.blocks[cfg.index.at(header)];
        for (const Instr& phi : block.instructions) {
            if (phi.opcode != InstrOpcode::Phi || phi.def < 0 ||
                phi.phi_incoming.size() != 2) {
                if (phi.opcode != InstrOpcode::Phi) break;
                continue;
            }
            ValueId initial = -1;
            ValueId backedge = -1;
            int outside = 0;
            int inside = 0;
            for (const auto& incoming : phi.phi_incoming) {
                if (loop.count(incoming.first)) {
                    backedge = incoming.second;
                    ++inside;
                } else {
                    initial = incoming.second;
                    ++outside;
                }
            }
            if (outside != 1 || inside != 1 || initial < 0 ||
                backedge < 0) {
                continue;
            }
            const ValueId resolved_backedge = resolve(backedge);
            const auto step_definition = definitions.find(resolved_backedge);
            if (step_definition == definitions.end()) continue;
            const Instr& step = *step_definition->second;
            if (step.opcode != InstrOpcode::Add &&
                step.opcode != InstrOpcode::Sub) {
                continue;
            }
            if (step.args.size() != 2 || step.def < 0 ||
                !loop.count(defining_blocks[step.def])) {
                continue;
            }
            ValueId constant_value = -1;
            bool uses_phi = false;
            if (step.opcode == InstrOpcode::Sub) {
                uses_phi = sameValue(step.args[0], phi.def);
                constant_value = step.args[1];
            } else {
                const bool lhs_phi = sameValue(step.args[0], phi.def);
                const bool rhs_phi = sameValue(step.args[1], phi.def);
                if (lhs_phi == rhs_phi) continue;
                uses_phi = true;
                constant_value = lhs_phi ? step.args[1] : step.args[0];
            }
            const ValueId resolved_constant = resolve(constant_value);
            const auto constant = constants.find(resolved_constant);
            if (!uses_phi || constant == constants.end()) continue;
            candidates.push_back(Candidate{
                phi.def,
                initial,
                backedge,
                resolved_backedge,
                StepSignature{step.opcode, constant->second, phi.type, true},
                loop});
        }
    }

    std::map<ValueId, ValueId> replacements;
    int simplified = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& current = candidates[i];
        if (replacements.count(current.phi)) continue;
        for (std::size_t j = i + 1; j < candidates.size(); ++j) {
            const Candidate* canonical = &current;
            const Candidate* redundant = &candidates[j];
            const std::string& current_step_block =
                defining_blocks.at(current.step);
            const std::string& other_step_block =
                defining_blocks.at(candidates[j].step);
            if (current_step_block == other_step_block &&
                defining_indices.at(current.step) >
                    defining_indices.at(candidates[j].step)) {
                std::swap(canonical, redundant);
            }
            const std::string& canonical_step_block =
                defining_blocks.at(canonical->step);
            const std::string& redundant_step_block =
                defining_blocks.at(redundant->step);
            const bool canonical_step_dominates =
                dominance.dominates(canonical_step_block,
                                     redundant_step_block) &&
                (canonical_step_block != redundant_step_block ||
                 defining_indices.at(canonical->step) <
                     defining_indices.at(redundant->step));
            if (replacements.count(redundant->phi) ||
                canonical->loop != redundant->loop ||
                !canonical_step_dominates ||
                !sameType(canonical->signature.type,
                          redundant->signature.type) ||
                canonical->signature.opcode != redundant->signature.opcode ||
                canonical->signature.constant !=
                    redundant->signature.constant ||
                !sameValue(canonical->initial, redundant->initial)) {
                continue;
            }
            replacements[redundant->phi] = canonical->phi;
            for (ValueId value : copyChain(redundant->backedge))
                replacements[value] = canonical->step;
            ++simplified;
        }
    }

    // A zero-step recurrence has no changing induction state.  Replace both
    // the IV and its backedge chain with the preheader value, preserving the
    // exact arithmetic operation until DCE removes the now-dead chain.
    for (const Candidate& candidate : candidates) {
        if (replacements.count(candidate.phi) ||
            candidate.signature.constant != 0) {
            continue;
        }
        replacements[candidate.phi] = candidate.initial;
        for (ValueId value : copyChain(candidate.backedge))
            replacements[value] = candidate.initial;
        ++simplified;
    }
    if (replacements.empty()) return 0;

    auto resolveReplacement = [&](ValueId value) {
        std::set<ValueId> seen;
        while (value >= 0 && replacements.count(value) &&
               !seen.count(value)) {
            seen.insert(value);
            value = replacements.at(value);
        }
        return value;
    };
    for (BasicBlock& block : fn.blocks) {
        for (Instr& instr : block.instructions) {
            for (ValueId& argument : instr.args)
                argument = resolveReplacement(argument);
            for (auto& incoming : instr.phi_incoming)
                incoming.second = resolveReplacement(incoming.second);
        }
        block.terminator.condition =
            resolveReplacement(block.terminator.condition);
    }
    return simplified;
}

// Convert a small pure branch diamond to target TSELs only when the v2 cost
// table predicts no increase in dynamic work.  Arm instructions are moved
// into the branch block (and therefore execute eagerly), so only
// speculatable operations with known target costs are eligible.  The arm
// blocks are removed only when they have no other predecessors and the merge
// has no other predecessors; this keeps the resulting SSA and phi dominance
// proof closed.
inline int runCostControlledTselConversion(Function& fn) {
    int converted = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        const ControlFlowGraph cfg = buildControlFlowGraph(fn);
        if (fn.blocks.empty() || !cfg.invalid_targets.empty()) break;
        const DominanceInfo dominance = computeDominance(fn, cfg);
        std::map<ValueId, std::string> defining_block;
        std::map<ValueId, int> defining_index;
        std::map<ValueId, TypeRef> defining_type;
        for (const BasicBlock& block : fn.blocks) {
            for (std::size_t index = 0;
                 index < block.instructions.size(); ++index) {
                const Instr& instr = block.instructions[index];
                if (instr.def < 0) continue;
                defining_block[instr.def] = block.name;
                defining_index[instr.def] = static_cast<int>(index);
                defining_type[instr.def] = instr.type;
            }
        }

        for (const BasicBlock& branch_snapshot : fn.blocks) {
            if (branch_snapshot.terminator.kind !=
                    TerminatorKind::Branch3 ||
                branch_snapshot.terminator.condition < 0) {
                continue;
            }
            const std::string branch_name = branch_snapshot.name;
            const std::array<std::string, 3> outcomes = {
                branch_snapshot.terminator.target_neg,
                branch_snapshot.terminator.target_zero,
                branch_snapshot.terminator.target_pos};
            std::vector<std::string> arms;
            for (const std::string& arm : outcomes) {
                if (arm.empty() ||
                    std::find(arms.begin(), arms.end(), arm) == arms.end()) {
                    arms.push_back(arm);
                }
            }
            if (arms.size() < 2 || arms.size() > 3) continue;

            std::string merge_name;
            bool valid_arms = true;
            for (const std::string& arm_name : arms) {
                const auto arm_index = cfg.index.find(arm_name);
                if (arm_index == cfg.index.end() ||
                    arm_name == branch_name ||
                    cfg.predecessors.at(arm_name).size() != 1 ||
                    !cfg.predecessors.at(arm_name).count(branch_name)) {
                    valid_arms = false;
                    break;
                }
                const BasicBlock& arm =
                    fn.blocks[static_cast<std::size_t>(arm_index->second)];
                if (arm.terminator.kind != TerminatorKind::Jump ||
                    arm.terminator.target.empty()) {
                    valid_arms = false;
                    break;
                }
                if (merge_name.empty()) merge_name = arm.terminator.target;
                if (merge_name != arm.terminator.target) {
                    valid_arms = false;
                    break;
                }
            }
            if (!valid_arms || merge_name == branch_name) continue;
            const auto merge_index = cfg.index.find(merge_name);
            if (merge_index == cfg.index.end()) continue;
            std::set<std::string> expected_predecessors(
                arms.begin(), arms.end());
            if (cfg.predecessors.at(merge_name) != expected_predecessors)
                continue;
            const BasicBlock& merge_snapshot =
                fn.blocks[static_cast<std::size_t>(merge_index->second)];

            std::vector<const Instr*> phis;
            for (const Instr& instr : merge_snapshot.instructions) {
                if (instr.opcode != InstrOpcode::Phi) break;
                phis.push_back(&instr);
            }
            if (phis.empty()) continue;

            std::map<ValueId, std::pair<std::string, int>> arm_definition_site;
            long long sum_arm_cost = 0;
            long long max_arm_cost = 0;
            for (const std::string& arm_name : arms) {
                const BasicBlock& arm =
                    fn.blocks[cfg.index.at(arm_name)];
                long long arm_cost = 0;
                for (std::size_t index = 0;
                     index < arm.instructions.size(); ++index) {
                    const Instr& instr = arm.instructions[index];
                    const int cost =
                        TargetCostTable::instruction(instr.opcode);
                    if (instr.def < 0 || cost == TargetCostTable::unavailable ||
                        instr.opcode == InstrOpcode::Param ||
                        !isSpeculatableInstruction(instr)) {
                        valid_arms = false;
                        break;
                    }
                    arm_definition_site[instr.def] = {
                        arm_name, static_cast<int>(index)};
                    arm_cost += cost;
                }
                if (!valid_arms) break;
                sum_arm_cost += arm_cost;
                max_arm_cost = std::max(max_arm_cost, arm_cost);
            }
            if (!valid_arms) continue;

            // A source from an arm must be defined earlier in that same arm;
            // every other source must dominate the branch block itself.
            auto sourceAvailableAtBranch = [&](ValueId value) {
                if (value < 0) return true;
                const auto found = defining_block.find(value);
                if (found == defining_block.end()) return false;
                if (found->second == branch_name) {
                    return defining_index.at(value) <
                        static_cast<int>(branch_snapshot.instructions.size());
                }
                return dominance.dominates(found->second, branch_name);
            };
            for (const std::string& arm_name : arms) {
                const BasicBlock& arm =
                    fn.blocks[cfg.index.at(arm_name)];
                for (std::size_t index = 0;
                     index < arm.instructions.size() && valid_arms; ++index) {
                    for (ValueId argument : arm.instructions[index].args) {
                        if (argument < 0) continue;
                        const auto local = arm_definition_site.find(argument);
                        if (local != arm_definition_site.end()) {
                            if (local->second.first != arm_name ||
                                local->second.second >=
                                    static_cast<int>(index)) {
                                valid_arms = false;
                                break;
                            }
                        } else if (!sourceAvailableAtBranch(argument)) {
                            valid_arms = false;
                            break;
                        }
                    }
                }
            }
            if (!valid_arms) continue;

            std::vector<std::array<ValueId, 3>> phi_values;
            for (const Instr* phi : phis) {
                std::map<std::string, ValueId> incoming;
                for (const auto& edge : phi->phi_incoming)
                    incoming[edge.first] = edge.second;
                if (incoming.size() != expected_predecessors.size()) {
                    valid_arms = false;
                    break;
                }
                std::array<ValueId, 3> selected = {-1, -1, -1};
                for (std::size_t outcome = 0; outcome < outcomes.size();
                     ++outcome) {
                    const auto value = incoming.find(outcomes[outcome]);
                    if (value == incoming.end() || value->second < 0) {
                        valid_arms = false;
                        break;
                    }
                    selected[outcome] = value->second;
                    const auto source_type =
                        defining_type.find(value->second);
                    const auto arm_source =
                        arm_definition_site.find(value->second);
                    const bool source_is_valid =
                        arm_source != arm_definition_site.end()
                            ? arm_source->second.first == outcomes[outcome]
                            : sourceAvailableAtBranch(value->second);
                    if (source_type == defining_type.end() ||
                        !sameType(source_type->second, phi->type) ||
                        !source_is_valid) {
                        valid_arms = false;
                        break;
                    }
                }
                if (!valid_arms || !isNumericLike(phi->type) ||
                    usesWideT50Pair(phi->type)) {
                    valid_arms = false;
                    break;
                }
                phi_values.push_back(selected);
            }
            if (!valid_arms) continue;

            const long long branch_cost =
                TargetCostTable::branch3 + TargetCostTable::jump +
                max_arm_cost;
            const long long tsel_cost =
                TargetCostTable::jump +
                static_cast<long long>(phis.size()) *
                    TargetCostTable::tsel + sum_arm_cost;
            if (tsel_cost > branch_cost) continue;

            const auto branch_index = cfg.index.at(branch_name);
            BasicBlock& branch = fn.blocks[branch_index];
            std::vector<Instr> moved;
            for (const std::string& arm_name : arms) {
                BasicBlock& arm = fn.blocks[cfg.index.at(arm_name)];
                for (Instr& instr : arm.instructions)
                    moved.push_back(std::move(instr));
            }
            branch.instructions.insert(
                branch.instructions.end(),
                std::make_move_iterator(moved.begin()),
                std::make_move_iterator(moved.end()));
            for (std::size_t index = 0; index < phis.size(); ++index) {
                Instr tsel = *phis[index];
                tsel.opcode = InstrOpcode::Tsel;
                tsel.args = {
                    branch_snapshot.terminator.condition,
                    phi_values[index][0],
                    phi_values[index][1],
                    phi_values[index][2]};
                tsel.phi_incoming.clear();
                tsel.effect = Effect::Pure;
                branch.instructions.push_back(std::move(tsel));
            }
            branch.terminator.kind = TerminatorKind::Jump;
            branch.terminator.target = merge_name;
            branch.terminator.condition = -1;

            BasicBlock& merge = fn.blocks[cfg.index.at(merge_name)];
            const auto first_non_phi = std::find_if(
                merge.instructions.begin(), merge.instructions.end(),
                [](const Instr& instr) {
                    return instr.opcode != InstrOpcode::Phi;
                });
            merge.instructions.erase(merge.instructions.begin(),
                                     first_non_phi);
            fn.blocks.erase(
                std::remove_if(
                    fn.blocks.begin(), fn.blocks.end(),
                    [&](const BasicBlock& block) {
                        return std::find(arms.begin(), arms.end(),
                                         block.name) != arms.end();
                    }),
                fn.blocks.end());
            ++converted;
            progress = true;
            break;
        }
    }
    return converted;
}

[[nodiscard]] inline OptimizerStats optimizeModule(
    Module& module,
    OptimizationLevel level,
    const CompilerOptions& options = CompilerOptions{}) {

    OptimizerStats stats;
    if (level == OptimizationLevel::None) return stats;

    for (auto& fn : module.functions) {
        if (options.enable_mem2reg && fn.cfg_complete) {
            const Mem2RegResult promoted = promoteMemoryToSSA(fn);
            stats.mem2reg_promotions += promoted.promoted_allocas;
        }
        if (fn.cfg_complete) {
            stats.induction_simplifications +=
                runInductionSimplification(fn);
            stats.copy_props += runGlobalCopyPropagation(fn);
            if (level == OptimizationLevel::Aggressive) {
                stats.tsel_conversions +=
                    runCostControlledTselConversion(fn);
            }
        }
        if (fn.cfg_complete &&
            level == OptimizationLevel::Aggressive && options.enable_cse) {
            const int hits = runGlobalValueNumbering(fn);
            stats.gvn_hits += hits;
            stats.cse_hits += hits;
            stats.licm_hoists += runLoopInvariantCodeMotion(fn);
        }
        const ControlFlowGraph cfg = buildControlFlowGraph(fn);
        const BlockLiveness liveness = computeBlockLiveness(fn, cfg);
        for (auto& block : fn.blocks) {
            std::map<ValueId, long long> constants;
            for (auto& instr : block.instructions) {
                for (ValueId& arg : instr.args) {
                    (void)arg;
                }
                if (instr.opcode == InstrOpcode::Const && instr.def >= 0) {
                    constants[instr.def] = instr.imm;
                    continue;
                }
                if ((instr.opcode == InstrOpcode::Add || instr.opcode == InstrOpcode::Sub ||
                     instr.opcode == InstrOpcode::Mul || instr.opcode == InstrOpcode::Div) &&
                    instr.args.size() == 2 &&
                    constants.count(instr.args[0]) && constants.count(instr.args[1])) {
                    const long long a = constants[instr.args[0]];
                    const long long b = constants[instr.args[1]];
                    if (instr.opcode != InstrOpcode::Div || b != 0) {
                        long long value = 0;
                        if (instr.opcode == InstrOpcode::Add) value = a + b;
                        else if (instr.opcode == InstrOpcode::Sub) value = a - b;
                        else if (instr.opcode == InstrOpcode::Mul) value = a * b;
                        else value = a / b;
                        instr.opcode = InstrOpcode::Const;
                        instr.args.clear();
                        instr.imm = value;
                        constants[instr.def] = value;
                        ++stats.constant_folds;
                    }
                }
                if ((instr.opcode == InstrOpcode::Mul || instr.opcode == InstrOpcode::Div ||
                     instr.opcode == InstrOpcode::Add || instr.opcode == InstrOpcode::Sub) &&
                    instr.args.size() == 2 && instr.def >= 0) {
                    const bool rhsZero = constants.count(instr.args[1]) && constants[instr.args[1]] == 0;
                    const bool rhsOne = constants.count(instr.args[1]) && constants[instr.args[1]] == 1;
                    if ((instr.opcode == InstrOpcode::Add && rhsZero) ||
                        (instr.opcode == InstrOpcode::Sub && rhsZero) ||
                        (instr.opcode == InstrOpcode::Mul && rhsOne) ||
                        (instr.opcode == InstrOpcode::Div && rhsOne)) {
                        instr.opcode = InstrOpcode::Copy;
                        instr.args = {instr.args[0]};
                        ++stats.strength_reductions;
                    }
                }
            }

            std::map<ValueId, ValueId> copies;
            for (auto& instr : block.instructions) {
                for (ValueId& arg : instr.args) arg = resolveCopy(arg, copies);
                if (instr.opcode == InstrOpcode::Copy && instr.args.size() == 1) {
                    if (instr.def == instr.args[0]) {
                        instr.opcode = InstrOpcode::Nop;
                        ++stats.copy_props;
                    } else {
                        copies[instr.def] = instr.args[0];
                    }
                }
            }

            if (options.enable_cse) {
                std::map<std::string, ValueId> seen;
                for (auto& instr : block.instructions) {
                    if (instr.def < 0 || !isPureInstruction(instr) ||
                        instr.opcode == InstrOpcode::Const ||
                        instr.opcode == InstrOpcode::Copy ||
                        instr.opcode == InstrOpcode::Phi ||
                        instr.opcode == InstrOpcode::Nop) {
                        continue;
                    }
                    const std::string key = instrKey(instr);
                    auto it = seen.find(key);
                    if (it != seen.end()) {
                        instr.opcode = InstrOpcode::Copy;
                        instr.args = {it->second};
                        instr.effect = Effect::Pure;
                        ++stats.cse_hits;
                    } else {
                        seen[key] = instr.def;
                    }
                }
            }

            std::set<ValueId> used = liveness.live_out.count(block.name)
                ? liveness.live_out.at(block.name)
                : std::set<ValueId>{};
            if (block.terminator.condition >= 0) used.insert(block.terminator.condition);
            for (const auto& instr : block.instructions) {
                for (ValueId arg : instr.args) if (arg >= 0) used.insert(arg);
            }
            block.instructions.erase(
                std::remove_if(block.instructions.begin(), block.instructions.end(),
                    [&](const Instr& instr) {
                        if (instr.opcode == InstrOpcode::Nop) {
                            ++stats.dead_instrs;
                            return true;
                        }
                        if (instr.def >= 0 && !used.count(instr.def) && isPureInstruction(instr)) {
                            ++stats.dead_instrs;
                            return true;
                        }
                        return false;
                    }),
                block.instructions.end());

            std::map<ValueId, long long> finalConstants;
            for (const auto& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Const && instr.def >= 0) {
                    finalConstants[instr.def] = instr.imm;
                }
                if (instr.opcode == InstrOpcode::Swap) ++stats.swaps;
            }
            if (block.terminator.kind == TerminatorKind::Branch3 &&
                finalConstants.count(block.terminator.condition) &&
                !branchRegionHasObservableEffects(fn, cfg, block)) {
                const long long value = finalConstants[block.terminator.condition];
                block.terminator.kind = TerminatorKind::Jump;
                block.terminator.target = value < 0 ? block.terminator.target_neg :
                                          value == 0 ? block.terminator.target_zero :
                                                       block.terminator.target_pos;
                ++stats.branch_simplifications;
            }
        }  // for (auto& block : fn.blocks)
        if (fn.cfg_complete) {
            const int sccp_rewrites =
                runSparseConditionalConstantPropagation(fn);
            stats.sccp_constants += sccp_rewrites;
            stats.constant_folds += sccp_rewrites;
            stats.dead_instrs += runCfgWideDeadCodeElimination(fn);
        }
    }  // for (auto& fn : module.functions)
    return stats;
}

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_CODEGEN_H
