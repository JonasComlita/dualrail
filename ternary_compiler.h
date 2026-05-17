// =============================================================================
// ternary_compiler.h - Phase 7 structural compiler layer
// =============================================================================
//
// This layer sits beside the legacy Program builder in ternary_ir.h. It provides
// a small ternary-native frontend, structural SSA-facing data types, conservative
// type inference, runtime syscall lowering, and static executable linking through
// the existing assembler/VM contract.

#pragma once
#ifndef TERNARY_COMPILER_H
#define TERNARY_COMPILER_H

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
    int target_abi_version = vm::EXEC_ABI_VERSION_V1;
    int syscall_abi_version = vm::EXEC_SYSCALL_ABI_VERSION_V1;
    OptimizationLevel optimization = OptimizationLevel::Basic;
    DiagnosticsMode diagnostics_mode = DiagnosticsMode::Human;
    bool emit_debug_metadata = false;
    bool emit_profiling_metadata = true;
    bool standalone_halt_on_exit = true;
    bool enable_mem2reg = true;
    bool enable_cse = true;
    bool enable_spilling = true;
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

    [[nodiscard]] bool isNumericScalar() const {
        return kind == TypeKind::Numeric;
    }
    [[nodiscard]] bool isVoid() const {
        return kind == TypeKind::Void;
    }
    [[nodiscard]] std::string str() const {
        switch (kind) {
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

[[nodiscard]] inline int numericRank(ir::Type type) {
    switch (type) {
        case ir::Type::T1: return 0;
        case ir::Type::T5: return 1;
        case ir::Type::T10: return 2;
        case ir::Type::T20: return 3;
        case ir::Type::T40: return 4;
        case ir::Type::T50: return 5;
        default: return -1;
    }
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
    if (from.kind != TypeKind::Numeric || to.kind != TypeKind::Numeric) return sameType(from, to);
    const int a = numericRank(from.scalar);
    const int b = numericRank(to.scalar);
    return a >= 0 && b >= 0 && a <= b;
}

[[nodiscard]] inline TypeRef commonNumericType(const TypeRef& a, const TypeRef& b) {
    if (a.kind == TypeKind::Unknown && b.kind == TypeKind::Numeric) return b;
    if (b.kind == TypeKind::Unknown && a.kind == TypeKind::Numeric) return a;
    if (a.kind == TypeKind::Numeric && b.kind == TypeKind::Numeric) {
        return TypeRef::numeric(widthAtRank(std::max(numericRank(a.scalar), numericRank(b.scalar))));
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

    [[nodiscard]] TypeRef apply(const TypeRef& type) const {
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
    if (left.kind == TypeKind::Numeric && right.kind == TypeKind::Numeric) {
        subst.bindings[-1] = commonNumericType(left, right);
        return true;
    }
    if (left.kind == right.kind && left.kind == TypeKind::Pointer &&
        left.element && right.element &&
        left.region == right.region && left.state == right.state) {
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

// =============================================================================
// Structural IR
// =============================================================================

using ValueId = int;

enum class Effect : uint8_t {
    Pure,
    ReadMem,
    WriteMem,
    Syscall,
    CSR,
    Atomic,
    Control,
};

enum class InstrOpcode : uint8_t {
    Alloca,
    Const,
    Copy,
    Add,
    Sub,
    Mul,
    Div,
    Cvt,
    Cmp,
    Phi,
    FieldAddr,
    IndexAddr,
    AddrOf,
    Deref,
    Load,
    Store,
    Syscall,
    Fence,
    Tldr,
    Tstr,
    Csrr,
    Csrw,
    Csrrw,
    Call,
    CallR,
    Ret,
    Swap,
    Nop,
};

struct Instr {
    ValueId def = -1;
    InstrOpcode opcode = InstrOpcode::Nop;
    TypeRef type = TypeRef::unknown();
    std::vector<ValueId> args;
    long long imm = 0;
    int aux = 0;
    std::string symbol;
    Effect effect = Effect::Pure;
    SourceSpan span;
};

enum class TerminatorKind : uint8_t {
    None,
    Return,
    Jump,
    Branch3,
    Halt,
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    ValueId condition = -1;
    std::string target_neg;
    std::string target_zero;
    std::string target_pos;
    std::string target;
};

struct BasicBlock {
    std::string name;
    std::vector<Instr> instructions;
    Terminator terminator;
};

struct Function {
    std::string name;
    std::vector<std::pair<std::string, TypeRef>> params;
    TypeRef return_type = TypeRef::voidType();
    std::vector<BasicBlock> blocks;
    bool exported = true;
    bool unsafe_allowed = false;
};

struct Module {
    std::string name;
    std::vector<Function> functions;
    std::vector<Diagnostic> diagnostics;
    std::map<std::string, std::string> metadata;
};

struct OptimizerStats {
    int mem2reg_promotions = 0;
    int constant_folds = 0;
    int copy_props = 0;
    int strength_reductions = 0;
    int cse_hits = 0;
    int dead_instrs = 0;
    int branch_simplifications = 0;
    int swaps = 0;
};

struct AllocationResult {
    bool success = false;
    std::map<ValueId, int> scalar_registers;
    std::map<ValueId, int> vector_registers;
    std::map<ValueId, int> spill_slots;
    int spills = 0;
    std::set<int> callee_saved_used;
    std::set<int> caller_saved_live_across_calls;
    int coalesced_moves = 0;
    int interference_edges = 0;
    std::vector<Diagnostic> diagnostics;
};

// =============================================================================
// Object/link interfaces
// =============================================================================

struct ModuleAst;

struct ObjectModule {
    std::string name;
    std::string assembly;
    Module ssa;
    std::map<std::string, int> symbols;
    std::map<std::string, std::string> metadata;
};

struct CompileResult {
    bool success = false;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<ModuleAst> typed_ast;
    Module ssa_module;
    Module optimized_module;
    AllocationResult allocation;
    LayoutTable layout_table;
    OptimizerStats optimizer_stats;
    ObjectModule object;
    std::string assembly;
};

struct LinkOptions {
    int stack_hint_words = 24;
    int flags = 0;
    int syscall_abi_version = vm::EXEC_SYSCALL_ABI_VERSION_V1;
    bool standalone_halt_on_exit = true;
};

struct LinkResult {
    bool success = false;
    std::string assembly;
    std::map<std::string, int> symbol_map;
    vm::ExecutableImageHeader executable_header;
    vm::assembler::AssemblyResult assembled;
    std::vector<Diagnostic> diagnostics;
    int instruction_count = 0;
    int text_words = 0;
    int data_words = 0;
};

namespace runtime {
static constexpr int sys_write_int = 1;
static constexpr int sys_newline = 2;
static constexpr int sys_clear = 3;
static constexpr int sys_yield = 4;
static constexpr int sys_sleep_until_tick = 5;
static constexpr int sys_exit = 6;
static constexpr int sys_getpid = 7;
static constexpr int sys_uptime = 8;
static constexpr int sys_read_console_word = 9;
static constexpr int sys_spawn_static = 10;
static constexpr int sys_waitpid = 11;
} // namespace runtime

// =============================================================================
// Lexer
// =============================================================================

enum class TokenKind : uint8_t {
    End,
    Identifier,
    Number,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    Arrow,
    FatArrow,
    Plus,
    Minus,
    Star,
    Slash,
    Amp,
    Equal,
    Less,
    Greater,
    Dot,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    long long number = 0;
    SourceSpan span;
};

class Lexer {
public:
    Lexer(std::string file, std::string source)
        : file_(std::move(file)), source_(std::move(source)) {}

    [[nodiscard]] std::vector<Token> lex(std::vector<Diagnostic>& diagnostics) {
        std::vector<Token> out;
        while (!atEnd()) {
            char c = peek();
            if (std::isspace(static_cast<unsigned char>(c))) {
                consumeWhitespace();
                continue;
            }
            if (c == '/' && peekNext() == '/') {
                consumeLineComment();
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                out.push_back(identifier());
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                out.push_back(number());
                continue;
            }
            SourceSpan span = currentSpan();
            advance();
            switch (c) {
                case '(': out.push_back(tok(TokenKind::LParen, "(", span)); break;
                case ')': out.push_back(tok(TokenKind::RParen, ")", span)); break;
                case '{': out.push_back(tok(TokenKind::LBrace, "{", span)); break;
                case '}': out.push_back(tok(TokenKind::RBrace, "}", span)); break;
                case '[': out.push_back(tok(TokenKind::LBracket, "[", span)); break;
                case ']': out.push_back(tok(TokenKind::RBracket, "]", span)); break;
                case ',': out.push_back(tok(TokenKind::Comma, ",", span)); break;
                case ';': out.push_back(tok(TokenKind::Semicolon, ";", span)); break;
                case ':': out.push_back(tok(TokenKind::Colon, ":", span)); break;
                case '+': out.push_back(tok(TokenKind::Plus, "+", span)); break;
                case '*': out.push_back(tok(TokenKind::Star, "*", span)); break;
                case '/': out.push_back(tok(TokenKind::Slash, "/", span)); break;
                case '&': out.push_back(tok(TokenKind::Amp, "&", span)); break;
                case '<': out.push_back(tok(TokenKind::Less, "<", span)); break;
                case '>': out.push_back(tok(TokenKind::Greater, ">", span)); break;
                case '.': out.push_back(tok(TokenKind::Dot, ".", span)); break;
                case '-':
                    if (peek() == '>') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::Arrow, "->", span));
                    } else {
                        out.push_back(tok(TokenKind::Minus, "-", span));
                    }
                    break;
                case '=':
                    if (peek() == '>') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::FatArrow, "=>", span));
                    } else {
                        out.push_back(tok(TokenKind::Equal, "=", span));
                    }
                    break;
                default:
                    diagnostics.push_back({DiagnosticSeverity::Error,
                        std::string("unexpected character '") + c + "'", span});
                    break;
            }
        }
        out.push_back(tok(TokenKind::End, "", currentSpan()));
        return out;
    }

private:
    std::string file_;
    std::string source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    [[nodiscard]] bool atEnd() const { return pos_ >= source_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : source_[pos_]; }
    [[nodiscard]] char peekNext() const {
        return pos_ + 1 >= source_.size() ? '\0' : source_[pos_ + 1];
    }
    [[nodiscard]] SourceSpan currentSpan() const {
        return SourceSpan{file_, line_, column_, 1};
    }
    char advance() {
        char c = source_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }
    void consumeWhitespace() {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) advance();
    }
    void consumeLineComment() {
        while (!atEnd() && peek() != '\n') advance();
    }
    [[nodiscard]] static Token tok(TokenKind kind, std::string text, SourceSpan span) {
        Token token;
        token.kind = kind;
        token.text = std::move(text);
        token.span = std::move(span);
        return token;
    }
    [[nodiscard]] Token identifier() {
        SourceSpan span = currentSpan();
        std::string text;
        while (!atEnd() &&
               (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
            text.push_back(advance());
        }
        span.length = static_cast<int>(text.size());
        return tok(TokenKind::Identifier, text, span);
    }
    [[nodiscard]] Token number() {
        SourceSpan span = currentSpan();
        std::string text;
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            text.push_back(advance());
        }
        span.length = static_cast<int>(text.size());
        Token token = tok(TokenKind::Number, text, span);
        token.number = std::stoll(text);
        return token;
    }
};

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

enum class StmtKind : uint8_t {
    Let,
    Assign,
    Return,
    Expr,
    WhilePos,
    MatchSign,
    UnsafeBlock,
    TupleSwap,
};

struct MatchArm {
    std::string name;
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

struct FunctionAst {
    std::string name;
    std::vector<std::pair<std::string, TypeRef>> params;
    TypeRef return_type = TypeRef::voidType();
    std::vector<Stmt> body;
    SourceSpan span;
};

struct ModuleAst {
    std::string name;
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<FunctionAst> functions;
};

[[nodiscard]] inline int typeSizeWords(const TypeRef& type, const LayoutTable& layouts) {
    switch (type.kind) {
        case TypeKind::Struct: {
            auto it = layouts.find(type.name);
            return it == layouts.end() ? 1 : std::max(1, it->second.size_words);
        }
        case TypeKind::Array:
            return std::max(0, type.array_len) *
                   (type.element ? typeSizeWords(*type.element, layouts) : 1);
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

struct InferResult {
    TypeRef type = TypeRef::unknown();
    Effect effect = Effect::Pure;
    bool addressable = false;
    bool value_restricted = false;
};

class TypeInferencer {
public:
    TypeInferencer(const ModuleAst& ast, const LayoutTable& layouts)
        : ast_(ast), layouts_(layouts) {
        for (const auto& fn : ast_.functions) {
            std::vector<TypeRef> params;
            for (const auto& param : fn.params) params.push_back(param.second);
            env_.values[fn.name] = Scheme{{}, functionType(params, fn.return_type), true};
        }
    }

    [[nodiscard]] std::vector<Diagnostic> inferModule() {
        for (const auto& fn : ast_.functions) inferFunction(fn);
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

    [[nodiscard]] TypeRef fresh() { return TypeRef::typeVar(next_type_var_++); }

    void inferFunction(const FunctionAst& fn) {
        TypeEnv saved = env_;
        current_return_ = fn.return_type;
        for (const auto& param : fn.params) {
            env_.values[param.first] = Scheme{{}, param.second, false};
        }
        inferBlock(fn.body);
        env_ = std::move(saved);
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
                    if (!canWiden(localType, stmt.annotation)) {
                        diagnostics_.push_back({DiagnosticSeverity::Error,
                            "implicit narrowing is not allowed from " + localType.str() +
                            " to " + stmt.annotation.str(), stmt.span});
                    }
                    unify(localType, stmt.annotation, subst_, diagnostics_, stmt.span,
                          "local annotation mismatch");
                    localType = stmt.annotation;
                }
                const bool restricted = stmt.mutable_binding || value.value_restricted ||
                                        !isPureEffect(value.effect);
                env_.values[stmt.name] = generalize(env_, subst_.apply(localType), restricted);
                return InferResult{localType, value.effect, false, restricted};
            }
            case StmtKind::Assign: {
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
            case StmtKind::WhilePos: {
                InferResult cond = inferExpr(stmt.expr);
                unify(cond.type, TypeRef::numeric(ir::Type::T40), subst_, diagnostics_,
                      stmt.span, "while condition must be numeric");
                InferResult body = inferBlock(stmt.body);
                return InferResult{TypeRef::voidType(), combineEffects(cond.effect, body.effect),
                                   false, true};
            }
            case StmtKind::MatchSign: {
                InferResult matched = inferExpr(stmt.expr);
                Effect effect = matched.effect;
                for (const auto& arm : stmt.arms) {
                    effect = combineEffects(effect, inferBlock(arm.body).effect);
                }
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
        }
        return {};
    }

    InferResult inferExpr(const ExprPtr& expr) {
        if (!expr) return {};
        switch (expr->kind) {
            case ExprKind::Number:
                return InferResult{TypeRef::numeric(ir::Type::T40), Effect::Pure};
            case ExprKind::Name: {
                auto it = env_.values.find(expr->text);
                if (it == env_.values.end()) {
                    diagnostics_.push_back({DiagnosticSeverity::Error,
                        "unknown name '" + expr->text + "'", expr->span});
                    return InferResult{fresh(), Effect::Pure};
                }
                return InferResult{instantiate(it->second, next_type_var_), Effect::Pure,
                                   true, false};
            }
            case ExprKind::Unary:
                return inferUnary(*expr);
            case ExprKind::Binary:
                return inferBinary(*expr);
            case ExprKind::Call:
                return inferCall(*expr);
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
            return inner;
        }
        if (expr.text == "*") {
            TypeRef elem = fresh();
            TypeRef ptr = TypeRef::pointer(elem);
            unify(inner.type, ptr, subst_, diagnostics_, expr.span,
                  "dereference requires a pointer");
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
        TypeRef common = commonNumericType(subst_.apply(lhs.type), subst_.apply(rhs.type));
        unify(lhs.type, common, subst_, diagnostics_, expr.span, "left operand type mismatch");
        unify(rhs.type, common, subst_, diagnostics_, expr.span, "right operand type mismatch");
        return InferResult{common, combineEffects(lhs.effect, rhs.effect), false,
                           lhs.value_restricted || rhs.value_restricted};
    }

    InferResult inferCall(const Expr& expr) {
        if (isRuntimeName(expr.text)) {
            Effect effect = Effect::Syscall;
            for (const auto& arg : expr.args) effect = combineEffects(effect, inferExpr(arg).effect);
            return InferResult{TypeRef::numeric(ir::Type::T40), effect, false, true};
        }
        if (isUnsafeIntrinsicName(expr.text)) {
            if (!unsafe_allowed_) {
                diagnostics_.push_back({DiagnosticSeverity::Error,
                    "raw CSR/atomic intrinsics require an unsafe block", expr.span});
            }
            return InferResult{TypeRef::numeric(ir::Type::T40),
                               expr.text == "csr_read" || expr.text == "csr_write"
                                   ? Effect::CSR : Effect::Atomic,
                               false, true};
        }
        auto it = env_.values.find(expr.text);
        if (it == env_.values.end()) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "unknown function '" + expr.text + "'", expr.span});
            return InferResult{fresh(), Effect::Control, false, true};
        }
        TypeRef callee = instantiate(it->second, next_type_var_);
        if (callee.kind != TypeKind::Function || !callee.result) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "called value is not a function", expr.span});
            return InferResult{fresh(), Effect::Control, false, true};
        }
        if (callee.params.size() != expr.args.size()) {
            diagnostics_.push_back({DiagnosticSeverity::Error,
                "function '" + expr.text + "' expects " +
                std::to_string(callee.params.size()) + " argument(s)", expr.span});
        }
        Effect effect = Effect::Control;
        for (std::size_t i = 0; i < expr.args.size() && i < callee.params.size(); ++i) {
            InferResult arg = inferExpr(expr.args[i]);
            effect = combineEffects(effect, arg.effect);
            unify(arg.type, callee.params[i], subst_, diagnostics_, expr.args[i]->span,
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
               name == "sys_clear" || name == "sys_yield" ||
               name == "sys_sleep_until_tick" || name == "sys_exit" ||
               name == "sys_getpid" || name == "sys_uptime" ||
               name == "sys_read_console_word" || name == "sys_spawn_static" ||
               name == "sys_waitpid";
    }

    [[nodiscard]] static bool isUnsafeIntrinsicName(const std::string& name) {
        return name == "csr_read" || name == "csr_write" ||
               name == "tldr" || name == "tstr" || name == "fence";
    }
};

[[nodiscard]] inline std::vector<Diagnostic> inferModuleTypes(
    const ModuleAst& ast,
    const LayoutTable& layouts) {

    TypeInferencer inferencer(ast, layouts);
    return inferencer.inferModule();
}

// =============================================================================
// Parser
// =============================================================================

class Parser {
public:
    Parser(std::string module_name, std::vector<Token> tokens)
        : module_name_(std::move(module_name)), tokens_(std::move(tokens)) {}

    [[nodiscard]] ModuleAst parse(std::vector<Diagnostic>& diagnostics) {
        diagnostics_ = &diagnostics;
        ModuleAst module;
        module.name = module_name_;
        while (!check(TokenKind::End)) {
            if (matchKeyword("import")) {
                module.imports.push_back(parseImport());
            } else if (matchKeyword("struct")) {
                module.structs.push_back(parseStruct());
            } else if (matchKeyword("fn")) {
                module.functions.push_back(parseFunction());
            } else {
                error(peek(), "expected import, struct, or fn declaration");
                advance();
            }
        }
        return module;
    }

private:
    std::string module_name_;
    std::vector<Token> tokens_;
    std::size_t current_ = 0;
    std::vector<Diagnostic>* diagnostics_ = nullptr;

    [[nodiscard]] const Token& peek() const { return tokens_[current_]; }
    [[nodiscard]] const Token& previous() const { return tokens_[current_ - 1]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }
    [[nodiscard]] bool isAtEnd() const { return check(TokenKind::End); }
    const Token& advance() {
        if (!isAtEnd()) ++current_;
        return previous();
    }
    [[nodiscard]] bool match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }
    [[nodiscard]] bool checkKeyword(const char* word) const {
        return check(TokenKind::Identifier) && peek().text == word;
    }
    [[nodiscard]] bool matchKeyword(const char* word) {
        if (!checkKeyword(word)) return false;
        advance();
        return true;
    }
    void error(const Token& token, const std::string& message) {
        diagnostics_->push_back({DiagnosticSeverity::Error, message, token.span});
    }
    Token consume(TokenKind kind, const std::string& message) {
        if (check(kind)) return advance();
        error(peek(), message);
        return peek();
    }
    Token consumeIdentifier(const std::string& message) {
        if (check(TokenKind::Identifier)) return advance();
        error(peek(), message);
        return peek();
    }

    [[nodiscard]] ImportDecl parseImport() {
        Token name = consumeIdentifier("expected import name");
        consume(TokenKind::Semicolon, "expected ';' after import");
        return ImportDecl{name.text, name.span};
    }

    [[nodiscard]] StructDecl parseStruct() {
        Token name = consumeIdentifier("expected struct name");
        StructDecl decl;
        decl.name = name.text;
        decl.span = name.span;
        consume(TokenKind::LBrace, "expected '{' after struct name");
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            Token field = consumeIdentifier("expected field name");
            consume(TokenKind::Colon, "expected ':' after field name");
            TypeRef type = parseType();
            consume(TokenKind::Semicolon, "expected ';' after field");
            decl.fields.push_back({field.text, type});
        }
        consume(TokenKind::RBrace, "expected '}' after struct");
        return decl;
    }

    [[nodiscard]] FunctionAst parseFunction() {
        Token name = consumeIdentifier("expected function name");
        FunctionAst fn;
        fn.name = name.text;
        fn.span = name.span;
        consume(TokenKind::LParen, "expected '(' after function name");
        if (!check(TokenKind::RParen)) {
            do {
                Token param = consumeIdentifier("expected parameter name");
                consume(TokenKind::Colon, "expected ':' after parameter name");
                fn.params.push_back({param.text, parseType()});
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RParen, "expected ')' after parameter list");
        consume(TokenKind::Arrow, "public function signatures require an explicit return type");
        fn.return_type = parseType();
        fn.body = parseBlock();
        return fn;
    }

    [[nodiscard]] std::vector<Stmt> parseBlock() {
        consume(TokenKind::LBrace, "expected '{'");
        std::vector<Stmt> body;
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            body.push_back(parseStmt());
        }
        consume(TokenKind::RBrace, "expected '}'");
        return body;
    }

    [[nodiscard]] Stmt parseStmt() {
        if (matchKeyword("let")) return parseLet(false);
        if (matchKeyword("var")) return parseLet(true);
        if (matchKeyword("return")) return parseReturn();
        if (matchKeyword("while")) return parseWhile();
        if (matchKeyword("match")) return parseMatch();
        if (matchKeyword("unsafe")) {
            Stmt stmt;
            stmt.kind = StmtKind::UnsafeBlock;
            stmt.span = previous().span;
            stmt.body = parseBlock();
            return stmt;
        }
        if (check(TokenKind::LParen)) return parseTupleSwap();
        (void)lookahead(0);
        ExprPtr expr = parseExpr();
        if (match(TokenKind::Equal)) {
            Stmt stmt;
            stmt.kind = StmtKind::Assign;
            stmt.target = expr;
            stmt.span = expr ? expr->span : previous().span;
            stmt.expr = parseExpr();
            consume(TokenKind::Semicolon, "expected ';' after assignment");
            return stmt;
        }
        Stmt stmt;
        stmt.kind = StmtKind::Expr;
        stmt.span = expr ? expr->span : peek().span;
        stmt.expr = expr;
        consume(TokenKind::Semicolon, "expected ';' after expression");
        return stmt;
    }

    [[nodiscard]] const Token& lookahead(std::size_t offset) const {
        const std::size_t idx = std::min(current_ + offset, tokens_.size() - 1);
        return tokens_[idx];
    }

    [[nodiscard]] Stmt parseLet(bool mut) {
        Token name = consumeIdentifier("expected local name");
        Stmt stmt;
        stmt.kind = StmtKind::Let;
        stmt.name = name.text;
        stmt.span = name.span;
        stmt.mutable_binding = mut;
        if (match(TokenKind::Colon)) stmt.annotation = parseType();
        consume(TokenKind::Equal, "expected '=' in local binding");
        stmt.expr = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after local binding");
        return stmt;
    }

    [[nodiscard]] Stmt parseAssign() {
        Token name = consumeIdentifier("expected assignment target");
        consume(TokenKind::Equal, "expected '=' in assignment");
        Stmt stmt;
        stmt.kind = StmtKind::Assign;
        stmt.name = name.text;
        stmt.span = name.span;
        stmt.expr = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after assignment");
        return stmt;
    }

    [[nodiscard]] Stmt parseTupleSwap() {
        SourceSpan span = peek().span;
        consume(TokenKind::LParen, "expected '('");
        Token a = consumeIdentifier("expected first swap target");
        consume(TokenKind::Comma, "expected ',' in tuple swap");
        Token b = consumeIdentifier("expected second swap target");
        consume(TokenKind::RParen, "expected ')' in tuple swap");
        consume(TokenKind::Equal, "expected '=' in tuple swap");
        consume(TokenKind::LParen, "expected '(' in tuple swap source");
        Token rb = consumeIdentifier("expected first swap source");
        consume(TokenKind::Comma, "expected ',' in tuple swap source");
        Token ra = consumeIdentifier("expected second swap source");
        consume(TokenKind::RParen, "expected ')' in tuple swap source");
        consume(TokenKind::Semicolon, "expected ';' after tuple swap");
        Stmt stmt;
        stmt.kind = StmtKind::TupleSwap;
        stmt.name = a.text;
        stmt.second_name = b.text;
        stmt.span = span;
        if (rb.text != b.text || ra.text != a.text) {
            error(spanToken(span), "v1 tuple assignment only supports direct variable swap");
        }
        return stmt;
    }

    [[nodiscard]] Token spanToken(const SourceSpan& span) const {
        Token token;
        token.span = span;
        return token;
    }

    [[nodiscard]] Stmt parseReturn() {
        Stmt stmt;
        stmt.kind = StmtKind::Return;
        stmt.span = previous().span;
        if (!check(TokenKind::Semicolon)) stmt.expr = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after return");
        return stmt;
    }

    [[nodiscard]] Stmt parseWhile() {
        Stmt stmt;
        stmt.kind = StmtKind::WhilePos;
        stmt.span = previous().span;
        consumeIdentifier("expected while predicate 'pos'");
        consume(TokenKind::LParen, "expected '(' after while predicate");
        stmt.expr = parseExpr();
        consume(TokenKind::RParen, "expected ')' after while condition");
        stmt.body = parseBlock();
        return stmt;
    }

    [[nodiscard]] Stmt parseMatch() {
        Stmt stmt;
        stmt.kind = StmtKind::MatchSign;
        stmt.span = previous().span;
        Token matcher = consumeIdentifier("expected match classifier");
        if (matcher.text != "sign" && matcher.text != "ptr_state") {
            error(matcher, "v1 match supports sign(...) and ptr_state(...)");
        }
        stmt.name = matcher.text;
        consume(TokenKind::LParen, "expected '(' after match classifier");
        stmt.expr = parseExpr();
        consume(TokenKind::RParen, "expected ')' after match expression");
        consume(TokenKind::LBrace, "expected '{' after match expression");
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            Token armName = consumeIdentifier("expected match arm name");
            consume(TokenKind::FatArrow, "expected '=>' after match arm");
            MatchArm arm;
            arm.name = armName.text;
            arm.span = armName.span;
            arm.body = parseBlock();
            stmt.arms.push_back(arm);
        }
        consume(TokenKind::RBrace, "expected '}' after match");
        return stmt;
    }

    [[nodiscard]] ExprPtr parseExpr() { return parseAddSub(); }

    [[nodiscard]] ExprPtr parseAddSub() {
        ExprPtr expr = parseMulDiv();
        while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
            Token op = previous();
            ExprPtr rhs = parseMulDiv();
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Binary;
            node->text = op.text;
            node->span = op.span;
            node->left = expr;
            node->right = rhs;
            expr = node;
        }
        return expr;
    }

    [[nodiscard]] ExprPtr parseMulDiv() {
        ExprPtr expr = parseUnary();
        while (match(TokenKind::Star) || match(TokenKind::Slash)) {
            Token op = previous();
            ExprPtr rhs = parseUnary();
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Binary;
            node->text = op.text;
            node->span = op.span;
            node->left = expr;
            node->right = rhs;
            expr = node;
        }
        return expr;
    }

    [[nodiscard]] ExprPtr parseUnary() {
        if (match(TokenKind::Minus) || match(TokenKind::Amp) || match(TokenKind::Star)) {
            Token op = previous();
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Unary;
            node->text = op.text;
            node->span = op.span;
            node->left = parseUnary();
            return node;
        }
        return parsePostfix();
    }

    [[nodiscard]] ExprPtr parsePostfix() {
        ExprPtr expr = parsePrimary();
        while (true) {
            if (match(TokenKind::LParen)) {
                auto node = std::make_shared<Expr>();
                node->kind = ExprKind::Call;
                node->span = expr ? expr->span : previous().span;
                if (!expr || expr->kind != ExprKind::Name) {
                    error(previous(), "v1 calls require a function name");
                } else {
                    node->text = expr->text;
                }
                if (!check(TokenKind::RParen)) {
                    do {
                        node->args.push_back(parseExpr());
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RParen, "expected ')' after call arguments");
                expr = node;
                continue;
            }
            if (match(TokenKind::Dot)) {
                Token field = consumeIdentifier("expected field name after '.'");
                auto node = std::make_shared<Expr>();
                node->kind = ExprKind::Field;
                node->span = field.span;
                node->text = field.text;
                node->left = expr;
                expr = node;
                continue;
            }
            if (match(TokenKind::LBracket)) {
                auto node = std::make_shared<Expr>();
                node->kind = ExprKind::Index;
                node->span = previous().span;
                node->left = expr;
                node->right = parseExpr();
                consume(TokenKind::RBracket, "expected ']' after index");
                expr = node;
                continue;
            }
            break;
        }
        return expr;
    }

    [[nodiscard]] ExprPtr parsePrimary() {
        if (match(TokenKind::Number)) {
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Number;
            node->span = previous().span;
            node->number = previous().number;
            node->text = previous().text;
            return node;
        }
        if (match(TokenKind::Identifier)) {
            Token name = previous();
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::Name;
            node->span = name.span;
            node->text = name.text;
            if (match(TokenKind::LBrace)) {
                node->kind = ExprKind::StructLiteral;
                if (!check(TokenKind::RBrace)) {
                    do {
                        Token field = consumeIdentifier("expected struct literal field name");
                        consume(TokenKind::Colon, "expected ':' after struct literal field");
                        node->fields.push_back({field.text, parseExpr()});
                    } while (match(TokenKind::Comma));
                }
                consume(TokenKind::RBrace, "expected '}' after struct literal");
            }
            return node;
        }
        if (match(TokenKind::LBracket)) {
            auto node = std::make_shared<Expr>();
            node->kind = ExprKind::ArrayLiteral;
            node->span = previous().span;
            if (!check(TokenKind::RBracket)) {
                do {
                    node->args.push_back(parseExpr());
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RBracket, "expected ']' after array literal");
            return node;
        }
        if (match(TokenKind::LParen)) {
            ExprPtr expr = parseExpr();
            consume(TokenKind::RParen, "expected ')' after expression");
            return expr;
        }
        error(peek(), "expected expression");
        auto node = std::make_shared<Expr>();
        node->kind = ExprKind::Number;
        node->span = peek().span;
        node->number = 0;
        return node;
    }

    [[nodiscard]] TypeRef parseType() {
        if (matchKeyword("void")) return TypeRef::voidType();
        if (match(TokenKind::LBracket)) {
            TypeRef elem = parseType();
            consume(TokenKind::Semicolon, "expected ';' in array type");
            Token len = consume(TokenKind::Number, "expected array length");
            consume(TokenKind::RBracket, "expected ']' after array type");
            TypeRef out;
            out.kind = TypeKind::Array;
            out.element = std::make_shared<TypeRef>(elem);
            out.array_len = static_cast<int>(len.number);
            return out;
        }
        Token name = consumeIdentifier("expected type name");
        TypeRef base = parseNamedType(name);
        if (name.text == "vec") {
            consume(TokenKind::Less, "expected '<' after vec");
            base = TypeRef::vector(parseType());
            consume(TokenKind::Greater, "expected '>' after vec type");
        } else if (name.text == "ptr") {
            consume(TokenKind::Less, "expected '<' after ptr");
            TypeRef elem = parseType();
            PointerRegion region = PointerRegion::Stack;
            PointerState state = PointerState::Valid;
            if (match(TokenKind::Comma)) region = parseRegion();
            if (match(TokenKind::Comma)) state = parsePointerState();
            consume(TokenKind::Greater, "expected '>' after ptr type");
            base = TypeRef::pointer(elem, region, state);
        } else if (name.text == "shared") {
            consume(TokenKind::Less, "expected '<' after shared");
            TypeRef elem = parseType();
            consume(TokenKind::Comma, "expected memory-order trit in shared type");
            int order = parseOrderLiteral();
            consume(TokenKind::Greater, "expected '>' after shared type");
            base = TypeRef::shared(elem, order == -1 ? MemoryOrder::Relaxed :
                                         order == 1 ? MemoryOrder::Sequential :
                                                      MemoryOrder::AcquireRelease);
        }
        return base;
    }

    [[nodiscard]] TypeRef parseNamedType(const Token& name) {
        if (name.text == "t1") return TypeRef::numeric(ir::Type::T1);
        if (name.text == "t5") return TypeRef::numeric(ir::Type::T5);
        if (name.text == "t10") return TypeRef::numeric(ir::Type::T10);
        if (name.text == "t20") return TypeRef::numeric(ir::Type::T20);
        if (name.text == "t40") return TypeRef::numeric(ir::Type::T40);
        if (name.text == "t50") return TypeRef::numeric(ir::Type::T50);
        if (name.text == "l1") return TypeRef::lane(ir::Type::L1);
        if (name.text == "l5") return TypeRef::lane(ir::Type::L5);
        if (name.text == "l10") return TypeRef::lane(ir::Type::L10);
        if (name.text == "l20") return TypeRef::lane(ir::Type::L20);
        if (name.text == "l40") return TypeRef::lane(ir::Type::L40);
        if (name.text == "l50") return TypeRef::lane(ir::Type::L50);
        TypeRef out;
        out.kind = TypeKind::Struct;
        out.name = name.text;
        return out;
    }

    [[nodiscard]] PointerRegion parseRegion() {
        Token token = consumeIdentifier("expected pointer region");
        if (token.text == "stack") return PointerRegion::Stack;
        if (token.text == "static") return PointerRegion::Static;
        if (token.text == "user") return PointerRegion::User;
        if (token.text == "kernel") return PointerRegion::Kernel;
        error(token, "unknown pointer region");
        return PointerRegion::Stack;
    }

    [[nodiscard]] PointerState parsePointerState() {
        Token token = consumeIdentifier("expected pointer state");
        if (token.text == "valid") return PointerState::Valid;
        if (token.text == "unknown") return PointerState::Unknown;
        if (token.text == "null") return PointerState::Null;
        error(token, "unknown pointer state");
        return PointerState::Unknown;
    }

    [[nodiscard]] int parseOrderLiteral() {
        int sign = 1;
        if (match(TokenKind::Minus)) sign = -1;
        else if (match(TokenKind::Plus)) sign = 1;
        Token token = consume(TokenKind::Number, "expected memory-order trit");
        int value = static_cast<int>(token.number) * sign;
        if (!isa::isAtomicOrder(value)) {
            error(token, "memory order must be -1, 0, or +1");
            return 0;
        }
        return value;
    }
};

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
};

struct FunctionContext {
    const FunctionAst* ast = nullptr;
    Function ir;
    BasicBlock* block = nullptr;
    std::map<std::string, LocalInfo> locals;
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

    [[nodiscard]] std::string label(const std::string& stem) {
        return ast->name + "_" + stem + "_" + std::to_string(label_counter++);
    }
    void line(const std::string& text) { asm_out << "    " << text << "\n"; }
    void raw(const std::string& text) { asm_out << text << "\n"; }
    [[nodiscard]] int acquire() {
        if (free_regs.empty()) {
            diagnostics->push_back({DiagnosticSeverity::Error,
                "temporary register pressure exceeded v1 expression allocator", ast->span});
            return 19;
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
    ValueId value(InstrOpcode op, TypeRef type, const SourceSpan& span) {
        ValueId id = next_value++;
        if (block) {
            Instr instr;
            instr.def = id;
            instr.opcode = op;
            instr.type = std::move(type);
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

class CompilerImpl {
public:
    CompilerImpl(ModuleAst ast, CompilerOptions options)
        : ast_(std::move(ast)), options_(options) {
        layout_table_ = buildLayoutTable(ast_, diagnostics_);
        for (const auto& fn : ast_.functions) {
            function_returns_[fn.name] = fn.return_type;
            for (const auto& param : fn.params) function_params_[fn.name].push_back(param.second);
        }
        auto inferenceDiagnostics = inferModuleTypes(ast_, layout_table_);
        diagnostics_.insert(diagnostics_.end(),
                            inferenceDiagnostics.begin(),
                            inferenceDiagnostics.end());
    }

    [[nodiscard]] CompileResult compile() {
        CompileResult result;
        result.ssa_module.name = ast_.name;
        result.typed_ast = std::make_shared<ModuleAst>(ast_);
        result.layout_table = layout_table_;
        for (const auto& fn : ast_.functions) {
            if (fn.name == "main") compileFunction(fn, result);
        }
        for (const auto& fn : ast_.functions) {
            if (fn.name != "main") compileFunction(fn, result);
        }
        result.success = diagnostics_.empty();
        result.diagnostics = diagnostics_;
        result.ssa_module.diagnostics = diagnostics_;
        result.object.name = ast_.name;
        result.object.ssa = result.ssa_module;
        result.object.assembly = result.assembly;
        result.object.metadata["phase"] = "7";
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

    void compileFunction(const FunctionAst& fn, CompileResult& result) {
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

        collectLocals(fn, ctx);
        ctx.frame_words = align9(std::max(1, ctx.next_local_offset));
        emitFunctionPrologue(fn, ctx);
        for (const auto& stmt : fn.body) emitStmt(stmt, ctx);
        emitDefaultReturn(ctx);
        result.assembly += ctx.asm_out.str();
        result.ssa_module.functions.push_back(ctx.ir);
        result.object.symbols[fn.name] = 0;
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
            collectLocalsIn(stmt.body, ctx);
            for (const auto& arm : stmt.arms) collectLocalsIn(arm.body, ctx);
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
        ctx.line("mov.t40 r24, " + std::to_string(ctx.frame_words));
        ctx.line("sub.t40 sp, sp, r24");
        ctx.line("store lr, sp, 0");
        for (std::size_t i = 0; i < fn.params.size() && i < 6; ++i) {
            const auto it = ctx.locals.find(fn.params[i].first);
            if (it != ctx.locals.end()) {
                ctx.line("store r" + std::to_string(13 + static_cast<int>(i)) +
                         ", sp, " + std::to_string(it->second.offset));
            }
        }
    }

    void emitEpilogue(FunctionContext& ctx) {
        const std::string epilogue = ctx.ast->name + "_return";
        ctx.raw(epilogue + ":");
        ctx.line("load lr, sp, 0");
        ctx.line("mov.t40 r24, " + std::to_string(ctx.frame_words));
        ctx.line("add.t40 sp, sp, r24");
        ctx.line("ret");
        ctx.block->terminator.kind = TerminatorKind::Return;
    }

    void emitDefaultReturn(FunctionContext& ctx) {
        ctx.line("mov." + std::string(ir::suffix(ctx.ast->return_type.scalar)) + " r13, 0");
        ctx.line("jmp " + ctx.ast->name + "_return");
        emitEpilogue(ctx);
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
            case StmtKind::MatchSign: emitMatch(stmt, ctx); break;
            case StmtKind::UnsafeBlock: {
                const bool old = ctx.unsafe_allowed;
                ctx.unsafe_allowed = true;
                for (const auto& child : stmt.body) emitStmt(child, ctx);
                ctx.unsafe_allowed = old;
                break;
            }
            case StmtKind::TupleSwap: emitTupleSwap(stmt, ctx); break;
        }
    }

    void emitLet(const Stmt& stmt, FunctionContext& ctx) {
        auto it = ctx.locals.find(stmt.name);
        if (it == ctx.locals.end()) return;
        if (isAggregateType(it->second.type)) {
            int base = emitLocalBase(it->second, ctx);
            emitAggregateInitToAddress(stmt.expr, it->second.type, base, ctx);
            ctx.release(base);
            ctx.value(InstrOpcode::Store, it->second.type, stmt.span);
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
        ctx.line("store r" + std::to_string(code.reg) + ", sp, " + std::to_string(it->second.offset));
        ctx.value(InstrOpcode::Store, it->second.type, stmt.span);
        ctx.release(code.reg);
    }

    void emitAssign(const Stmt& stmt, FunctionContext& ctx) {
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
        ctx.line("store r" + std::to_string(code.reg) + ", r" +
                 std::to_string(place.reg) + ", 0");
        ctx.value(InstrOpcode::Store, place.type, stmt.span);
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
        if (stmt.expr) {
            ExprCode code = emitExpr(stmt.expr, ctx.ast->return_type, ctx);
            if (!canWiden(code.type, ctx.ast->return_type)) {
                diag("implicit narrowing is not allowed from " + code.type.str() +
                     " to " + ctx.ast->return_type.str(), stmt.span);
            }
            emitCvtIfNeeded(code, ctx.ast->return_type, ctx);
            ctx.line("copy r13, r" + std::to_string(code.reg));
            ctx.release(code.reg);
        } else {
            ctx.line("mov.t40 r13, 0");
        }
        ctx.line("jmp " + ctx.ast->name + "_return");
        ctx.value(InstrOpcode::Ret, ctx.ast->return_type, stmt.span);
    }

    void emitWhile(const Stmt& stmt, FunctionContext& ctx) {
        std::string start = ctx.label("while_start");
        std::string body = ctx.label("while_body");
        std::string end = ctx.label("while_end");
        ctx.raw(start + ":");
        ExprCode cond = emitExpr(stmt.expr, TypeRef::unknown(), ctx);
        ctx.line("brp r" + std::to_string(cond.reg) + ", " + body);
        ctx.line("jmp " + end);
        ctx.release(cond.reg);
        ctx.raw(body + ":");
        for (const auto& child : stmt.body) emitStmt(child, ctx);
        ctx.line("jmp " + start);
        ctx.raw(end + ":");
    }

    void emitMatch(const Stmt& stmt, FunctionContext& ctx) {
        if (stmt.name != "sign") {
            diag("ptr_state match currently performs type refinement only and is not lowerable", stmt.span);
            return;
        }
        std::set<std::string> seen;
        for (const auto& arm : stmt.arms) seen.insert(arm.name);
        if (!seen.count("neg") || !seen.count("zero") || !seen.count("pos")) {
            diag("match sign(...) must cover neg, zero, and pos arms", stmt.span);
        }
        std::string neg = ctx.label("match_neg");
        std::string zero = ctx.label("match_zero");
        std::string pos = ctx.label("match_pos");
        std::string end = ctx.label("match_end");
        ExprCode cond = emitExpr(stmt.expr, TypeRef::unknown(), ctx);
        ctx.line("brn r" + std::to_string(cond.reg) + ", " + neg);
        ctx.line("brz r" + std::to_string(cond.reg) + ", " + zero);
        ctx.line("brp r" + std::to_string(cond.reg) + ", " + pos);
        ctx.release(cond.reg);
        emitArm("neg", neg, end, stmt, ctx);
        emitArm("zero", zero, end, stmt, ctx);
        emitArm("pos", pos, end, stmt, ctx);
        ctx.raw(end + ":");
    }

    void emitArm(
        const std::string& name,
        const std::string& label,
        const std::string& end,
        const Stmt& stmt,
        FunctionContext& ctx) {
        ctx.raw(label + ":");
        for (const auto& arm : stmt.arms) {
            if (arm.name == name) {
                for (const auto& child : arm.body) emitStmt(child, ctx);
                break;
            }
        }
        ctx.line("jmp " + end);
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
        int ra = ctx.acquire();
        int rb = ctx.acquire();
        ctx.line("load r" + std::to_string(ra) + ", sp, " + std::to_string(a->second.offset));
        ctx.line("load r" + std::to_string(rb) + ", sp, " + std::to_string(b->second.offset));
        ctx.line("swap r" + std::to_string(ra) + ", r" + std::to_string(rb));
        ctx.line("store r" + std::to_string(ra) + ", sp, " + std::to_string(a->second.offset));
        ctx.line("store r" + std::to_string(rb) + ", sp, " + std::to_string(b->second.offset));
        ctx.value(InstrOpcode::Swap, a->second.type, stmt.span);
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
        ValueId id = ctx.value(InstrOpcode::Const, type, span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().imm = value;
        }
        return ExprCode{reg, type, false, id};
    }

    [[nodiscard]] ExprCode emitName(const Expr& expr, FunctionContext& ctx) {
        auto it = ctx.locals.find(expr.text);
        if (it == ctx.locals.end()) {
            diag("unknown name '" + expr.text + "'", expr.span);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
        }
        if (isAggregateType(it->second.type)) {
            int addr = emitLocalBase(it->second, ctx);
            return ExprCode{addr, it->second.type, true};
        }
        int reg = ctx.acquire();
        ctx.line("load r" + std::to_string(reg) + ", sp, " + std::to_string(it->second.offset));
        ValueId id = ctx.value(InstrOpcode::Load, it->second.type, expr.span);
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
            ctx.value(InstrOpcode::AddrOf, TypeRef::pointer(place.type), expr.span);
            return ExprCode{place.reg, TypeRef::pointer(place.type), false};
        }
        if (expr.text == "*") {
            ExprCode ptr = emitExpr(expr.left, TypeRef::unknown(), ctx);
            if (ptr.type.kind != TypeKind::Pointer) {
                diag("dereference requires a pointer", expr.span);
            } else if (ptr.type.state != PointerState::Valid) {
                diag("cannot dereference pointer before proving it is valid", expr.span);
            }
            TypeRef elem = ptr.type.element ? *ptr.type.element : TypeRef::numeric(ir::Type::T40);
            if (isAggregateType(elem)) {
                ptr.type = elem;
                ptr.address = true;
                return ptr;
            }
            int out = ptr.reg;
            ctx.line("load r" + std::to_string(out) + ", r" + std::to_string(ptr.reg) + ", 0");
            ValueId id = ctx.value(InstrOpcode::Deref, elem, expr.span);
            return ExprCode{out, elem, false, id};
        }
        diag("unsupported unary operator '" + expr.text + "'", expr.span);
        return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
    }

    [[nodiscard]] int emitLocalBase(const LocalInfo& local, FunctionContext& ctx) {
        int reg = ctx.acquire();
        if (local.by_pointer) {
            ctx.line("load r" + std::to_string(reg) + ", sp, " + std::to_string(local.offset));
            return reg;
        }
        int off = ctx.acquire();
        ctx.line("mov.t40 r" + std::to_string(off) + ", " + std::to_string(local.offset));
        ctx.line("add.t40 r" + std::to_string(reg) + ", sp, r" + std::to_string(off));
        ctx.release(off);
        return reg;
    }

    void addImmediateToReg(int reg, int offset, FunctionContext& ctx) {
        if (offset == 0) return;
        int tmp = ctx.acquire();
        ctx.line("mov.t40 r" + std::to_string(tmp) + ", " + std::to_string(offset));
        ctx.line("add.t40 r" + std::to_string(reg) + ", r" + std::to_string(reg) +
                 ", r" + std::to_string(tmp));
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
            ctx.value(InstrOpcode::AddrOf, TypeRef::pointer(it->second.type), expr->span);
            return LValueCode{reg, it->second.type, it->second.mutable_binding, true};
        }
        if (expr->kind == ExprKind::Unary && expr->text == "*") {
            ExprCode ptr = emitExpr(expr->left, TypeRef::unknown(), ctx);
            if (ptr.type.kind != TypeKind::Pointer) {
                diag("dereference requires a pointer", expr->span);
                ctx.release(ptr.reg);
                return LValueCode{};
            }
            if (ptr.type.state != PointerState::Valid) {
                diag("cannot dereference pointer before proving it is valid", expr->span);
            }
            TypeRef elem = ptr.type.element ? *ptr.type.element : TypeRef::numeric(ir::Type::T40);
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
            ctx.value(InstrOpcode::FieldAddr, TypeRef::pointer(field->type), expr->span);
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
                    ctx.line("mul.t40 r" + std::to_string(index.reg) + ", r" +
                             std::to_string(index.reg) + ", r" + std::to_string(scale));
                    ctx.release(scale);
                }
                ctx.line("add.t40 r" + std::to_string(base.reg) + ", r" +
                         std::to_string(base.reg) + ", r" + std::to_string(index.reg));
                ctx.release(index.reg);
            }
            ctx.value(InstrOpcode::IndexAddr, TypeRef::pointer(elem), expr->span);
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
            return ExprCode{place.reg, place.type, true};
        }
        ctx.line("load r" + std::to_string(place.reg) + ", r" +
                 std::to_string(place.reg) + ", 0");
        ValueId id = ctx.value(InstrOpcode::Load, place.type, span);
        return ExprCode{place.reg, place.type, false, id};
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
        ctx.line("store r" + std::to_string(code.reg) + ", r" +
                 std::to_string(baseReg) + ", " + std::to_string(offset));
        ctx.value(InstrOpcode::Store, expected, expr ? expr->span : SourceSpan{});
        ctx.release(code.reg);
    }

    void emitAggregateCopy(int srcReg, int dstReg, const TypeRef& type, FunctionContext& ctx) {
        const int words = std::max(1, typeSizeWords(type, layout_table_));
        int tmp = ctx.acquire();
        for (int i = 0; i < words; ++i) {
            ctx.line("load r" + std::to_string(tmp) + ", r" + std::to_string(srcReg) +
                     ", " + std::to_string(i));
            ctx.line("store r" + std::to_string(tmp) + ", r" + std::to_string(dstReg) +
                     ", " + std::to_string(i));
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
        std::string mnemonic = expr.text == "+" ? "add" :
                               expr.text == "-" ? "sub" :
                               expr.text == "*" ? "mul" : "div";
        ctx.line(mnemonic + "." + std::string(ir::suffix(common.scalar)) + " r" +
                 std::to_string(lhs.reg) + ", r" + std::to_string(lhs.reg) +
                 ", r" + std::to_string(rhs.reg));
        ValueId id = ctx.value(expr.text == "+" ? InstrOpcode::Add :
                               expr.text == "-" ? InstrOpcode::Sub :
                               expr.text == "*" ? InstrOpcode::Mul : InstrOpcode::Div,
                               common, expr.span);
        if (ctx.block && !ctx.block->instructions.empty()) {
            ctx.block->instructions.back().args = {lhs.value, rhs.value};
        }
        ctx.release(rhs.reg);
        return ExprCode{lhs.reg, common, false, id};
    }

    [[nodiscard]] ExprCode emitCall(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
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
        for (std::size_t i = 0; i < expr.args.size() && i < 6; ++i) {
            TypeRef expected = TypeRef::numeric(ir::Type::T40);
            if (paramIt != ctx.function_params.end() && i < paramIt->second.size()) {
                expected = paramIt->second[i];
            }
            ExprCode arg = emitExpr(expr.args[i], expected, ctx);
            if (isAggregateType(expected) && !arg.address) {
                diag("aggregate arguments are passed by pointer in v1", expr.args[i]->span);
            }
            ctx.line("copy r" + std::to_string(13 + static_cast<int>(i)) +
                     ", r" + std::to_string(arg.reg));
            ctx.release(arg.reg);
        }
        ctx.line("call " + expr.text);
        TypeRef ret = retIt->second;
        int out = ctx.acquire();
        ctx.line("copy r" + std::to_string(out) + ", r13");
        ValueId id = ctx.value(InstrOpcode::Call, ret, expr.span);
        return ExprCode{out, ret, false, id};
    }

    [[nodiscard]] ExprCode emitRuntimeCall(const Expr& expr, TypeRef expected, FunctionContext& ctx) {
        const int service = runtimeService(expr.text);
        if (expr.args.size() > 6) diag("syscall wrapper accepts at most six arguments", expr.span);
        for (std::size_t i = 0; i < expr.args.size() && i < 6; ++i) {
            ExprCode arg = emitExpr(expr.args[i], TypeRef::numeric(ir::Type::T40), ctx);
            ctx.line("copy r" + std::to_string(13 + static_cast<int>(i)) +
                     ", r" + std::to_string(arg.reg));
            if (service == runtime::sys_write_int && i == 0) {
                ctx.line("copy r1, r" + std::to_string(arg.reg));
            }
            ctx.release(arg.reg);
        }
        ctx.line("syscall " + std::to_string(service));
        int out = ctx.acquire();
        ctx.line("copy r" + std::to_string(out) + ", r13");
        ValueId id = ctx.value(InstrOpcode::Syscall, TypeRef::numeric(ir::Type::T40), expr.span);
        (void)expected;
        return ExprCode{out, TypeRef::numeric(ir::Type::T40), false, id};
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
            instr.opcode = InstrOpcode::Csrr;
            instr.type = TypeRef::numeric(ir::Type::T40);
            instr.aux = csr;
            instr.effect = Effect::CSR;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T40)};
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
            ctx.block->instructions.push_back(instr);
            return emitImmediate(0, TypeRef::numeric(ir::Type::T40), expr.span, ctx);
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
            instr.opcode = InstrOpcode::Tldr;
            instr.type = TypeRef::numeric(ir::Type::T40);
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T40)};
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
            instr.opcode = InstrOpcode::Tstr;
            instr.type = TypeRef::numeric(ir::Type::T1);
            instr.aux = order;
            instr.effect = Effect::Atomic;
            instr.span = expr.span;
            ctx.block->instructions.push_back(instr);
            return ExprCode{out, TypeRef::numeric(ir::Type::T1)};
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
        if (name == "sys_yield") return runtime::sys_yield;
        if (name == "sys_sleep_until_tick") return runtime::sys_sleep_until_tick;
        if (name == "sys_exit") return runtime::sys_exit;
        if (name == "sys_getpid") return runtime::sys_getpid;
        if (name == "sys_uptime") return runtime::sys_uptime;
        if (name == "sys_read_console_word") return runtime::sys_read_console_word;
        if (name == "sys_spawn_static") return runtime::sys_spawn_static;
        if (name == "sys_waitpid") return runtime::sys_waitpid;
        return 0;
    }

    [[nodiscard]] static bool isUnsafeIntrinsic(const std::string& name) {
        return name == "csr_read" || name == "csr_write" ||
               name == "tldr" || name == "tstr" || name == "fence";
    }

    [[nodiscard]] static bool extractInteger(const ExprPtr& expr, int& out) {
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

    void emitCvtIfNeeded(ExprCode& code, const TypeRef& target, FunctionContext& ctx) {
        if (code.type.kind != TypeKind::Numeric || target.kind != TypeKind::Numeric) return;
        if (code.type.scalar == target.scalar) return;
        if (!canWiden(code.type, target)) return;
        ctx.line("cvt." + std::string(ir::suffix(code.type.scalar)) + "." +
                 ir::suffix(target.scalar) + " r" + std::to_string(code.reg) +
                 ", r" + std::to_string(code.reg));
        ValueId id = ctx.value(InstrOpcode::Cvt, target, SourceSpan{});
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
        std::set<ValueId> defined;
        for (const auto& block : fn.blocks) {
            if (block.terminator.kind == TerminatorKind::None) {
                diagnostics.push_back({DiagnosticSeverity::Error,
                    "basic block '" + block.name + "' has no terminator",
                    SourceSpan{module.name, 1, 1, 1}});
            }
            for (const auto& instr : block.instructions) {
                for (ValueId arg : instr.args) {
                    if (arg >= 0 && !defined.count(arg)) {
                        diagnostics.push_back({DiagnosticSeverity::Error,
                            "use-before-def in function '" + fn.name + "'", instr.span});
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
                if (instr.def >= 0) defined.insert(instr.def);
            }
        }
    }
    return diagnostics;
}

[[nodiscard]] inline bool isPureInstruction(const Instr& instr) {
    return instr.effect == Effect::Pure &&
           instr.opcode != InstrOpcode::Store &&
           instr.opcode != InstrOpcode::Syscall &&
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
    const CompilerOptions& options = CompilerOptions{}) {

    AllocationResult result;
    const std::vector<int> scalarColors = {
        19, 20, 21, 22, 23,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16, 17, 18,
        24
    };
    const std::vector<int> calleePreferred = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const std::vector<int> vectorColors = {0, 1, 2, 3, 4, 5, 6, 7};

    std::map<ValueId, TypeRef> valueTypes;
    std::map<ValueId, std::set<ValueId>> graph;
    std::set<std::pair<ValueId, ValueId>> moveEdges;
    std::set<ValueId> liveAcrossCalls;

    for (const auto& fn : module.functions) {
        for (const auto& block : fn.blocks) {
            std::set<ValueId> live;
            if (block.terminator.condition >= 0) live.insert(block.terminator.condition);
            for (auto it = block.instructions.rbegin(); it != block.instructions.rend(); ++it) {
                const Instr& instr = *it;
                if (instr.opcode == InstrOpcode::Call || instr.opcode == InstrOpcode::CallR ||
                    instr.opcode == InstrOpcode::Syscall) {
                    liveAcrossCalls.insert(live.begin(), live.end());
                }
                if (instr.def >= 0) {
                    valueTypes[instr.def] = instr.type;
                    graph[instr.def];
                    for (ValueId value : live) addInterferenceEdge(graph, instr.def, value);
                    live.erase(instr.def);
                }
                if (instr.opcode == InstrOpcode::Copy && instr.def >= 0 &&
                    instr.args.size() == 1 && instr.args[0] >= 0) {
                    ValueId a = instr.def;
                    ValueId b = instr.args[0];
                    if (a > b) std::swap(a, b);
                    moveEdges.insert({a, b});
                }
                for (ValueId arg : instr.args) {
                    if (arg >= 0) live.insert(arg);
                }
            }
        }
    }

    std::vector<ValueId> values;
    for (const auto& entry : valueTypes) values.push_back(entry.first);
    std::sort(values.begin(), values.end(),
        [&](ValueId a, ValueId b) {
            const std::size_t da = graph[a].size();
            const std::size_t db = graph[b].size();
            if (da != db) return da > db;
            return a < b;
        });

    auto colorAvailable = [&](ValueId value, int color, const std::map<ValueId, int>& colors) {
        for (ValueId neighbor : graph[value]) {
            auto it = colors.find(neighbor);
            if (it != colors.end() && it->second == color) return false;
        }
        return true;
    };

    int nextSpillSlot = 0;
    for (ValueId value : values) {
        const TypeRef type = valueTypes[value];
        const bool vector = type.kind == TypeKind::Vector;
        std::map<ValueId, int>& colors = vector ? result.vector_registers
                                                : result.scalar_registers;
        const std::vector<int>& palette = vector ? vectorColors :
            (liveAcrossCalls.count(value) ? calleePreferred : scalarColors);

        bool assigned = false;
        for (const auto& move : moveEdges) {
            ValueId peer = move.first == value ? move.second :
                           move.second == value ? move.first : -1;
            if (peer < 0 || graph[value].count(peer)) continue;
            auto it = colors.find(peer);
            if (it != colors.end() && colorAvailable(value, it->second, colors)) {
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

[[nodiscard]] inline OptimizerStats optimizeModule(
    Module& module,
    OptimizationLevel level,
    const CompilerOptions& options = CompilerOptions{}) {

    OptimizerStats stats;
    if (level == OptimizationLevel::None) return stats;

    for (auto& fn : module.functions) {
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

            std::set<ValueId> used;
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
                finalConstants.count(block.terminator.condition)) {
                const long long value = finalConstants[block.terminator.condition];
                block.terminator.kind = TerminatorKind::Jump;
                block.terminator.target = value < 0 ? block.terminator.target_neg :
                                          value == 0 ? block.terminator.target_zero :
                                                       block.terminator.target_pos;
                ++stats.branch_simplifications;
            }
        }
    }
    if (options.enable_mem2reg) stats.mem2reg_promotions = 0;
    return stats;
}

// =============================================================================
// Public compile and link entry points
// =============================================================================

[[nodiscard]] inline CompileResult compileSource(
    const std::string& name,
    const std::string& text,
    const CompilerOptions& options = CompilerOptions{}) {

    std::vector<Diagnostic> diagnostics;
    Lexer lexer(name, text);
    std::vector<Token> tokens = lexer.lex(diagnostics);
    Parser parser(name, std::move(tokens));
    ModuleAst ast = parser.parse(diagnostics);
    CompilerImpl impl(std::move(ast), options);
    CompileResult result = impl.compile();
    result.diagnostics.insert(result.diagnostics.begin(), diagnostics.begin(), diagnostics.end());
    result.success = result.diagnostics.empty();
    result.ssa_module.diagnostics = result.diagnostics;
    if (result.success) {
        result.optimizer_stats = optimizeModule(result.ssa_module, options.optimization, options);
        result.optimized_module = result.ssa_module;
        result.allocation = allocateRegisters(result.ssa_module, options);
        result.object.ssa = result.ssa_module;
        auto verifierDiagnostics = verifyModule(result.ssa_module);
        result.diagnostics.insert(result.diagnostics.end(),
                                  verifierDiagnostics.begin(),
                                  verifierDiagnostics.end());
        result.diagnostics.insert(result.diagnostics.end(),
                                  result.allocation.diagnostics.begin(),
                                  result.allocation.diagnostics.end());
        result.success = result.diagnostics.empty();
    }
    return result;
}

[[nodiscard]] inline LinkResult linkModules(
    const std::vector<ObjectModule>& modules,
    const LinkOptions& options = LinkOptions{}) {

    LinkResult result;
    std::set<std::string> symbols;
    std::ostringstream asmOut;
    asmOut << ".text\n";
    asmOut << "_start:\n";
    asmOut << "    call main\n";
    if (options.standalone_halt_on_exit) {
        asmOut << "    halt\n";
    } else {
        asmOut << "    syscall " << runtime::sys_exit << "\n";
        asmOut << "    halt\n";
    }
    for (const auto& module : modules) {
        for (const auto& symbol : module.symbols) {
            if (symbols.count(symbol.first)) {
                result.diagnostics.push_back({DiagnosticSeverity::Error,
                    "duplicate symbol '" + symbol.first + "'", SourceSpan{module.name, 1, 1, 1}});
            }
            symbols.insert(symbol.first);
        }
        asmOut << module.assembly;
    }
    asmOut << ".data\n";
    asmOut << "phase7_exec: .execheader 0, 1, 1, "
           << options.stack_hint_words << ", "
           << options.syscall_abi_version << ", "
           << options.flags << "\n";

    result.assembly = asmOut.str();
    result.assembled = vm::assembler::assemble(result.assembly);
    result.success = result.diagnostics.empty() && result.assembled.success;
    if (!result.assembled.success) {
        for (const auto& error : result.assembled.errors) {
            result.diagnostics.push_back({DiagnosticSeverity::Error, error.format(), SourceSpan{}});
        }
    }
    result.text_words = static_cast<int>(result.assembled.program.size());
    result.data_words = static_cast<int>(result.assembled.data.size());
    result.instruction_count = result.text_words;
    if (result.assembled.executable_headers.count("phase7_exec")) {
        result.executable_header = result.assembled.executable_headers.at("phase7_exec");
    }
    for (const auto& module : modules) {
        for (const auto& symbol : module.symbols) result.symbol_map[symbol.first] = symbol.second;
    }
    return result;
}

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_H
