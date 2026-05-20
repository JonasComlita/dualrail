// =============================================================================
// ternary_compiler_parser.h - Recursive descent parser for TCL
// =============================================================================
//
// Separated from ternary_compiler.h for modularity.

#pragma once
#ifndef TERNARY_COMPILER_PARSER_H
#define TERNARY_COMPILER_PARSER_H

#include "ternary_compiler_ast.h"
#include "ternary_compiler_lexer.h"

namespace sandbox {
namespace compiler {

// =============================================================================
// Parser
// =============================================================================

class Parser {
public:
    Parser(std::string module_name, std::vector<Token> tokens)
        : module_name_(std::move(module_name)), tokens_(std::move(tokens)) {
        populateBuiltins(file_scope_consts_);
    }

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
            } else if (matchKeyword("const")) {
                module.consts.push_back(parseConstDecl());
            } else {
                error(peek(), "expected import, struct, fn, or const declaration");
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
    ConstantTable file_scope_consts_;
    std::vector<ConstantTable> const_scopes_;
    std::set<std::string> active_width_params_;

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
        active_width_params_.clear();
        if (match(TokenKind::Less)) {
            do {
                Token param = consumeIdentifier("expected width parameter name");
                fn.width_params.push_back(param.text);
                active_width_params_.insert(param.text);
                consume(TokenKind::Colon, "expected ':' after width parameter name");
                Token constraint = consumeIdentifier("expected width parameter constraint (e.g. TritWidth)");
                if (constraint.text != "TritWidth") {
                    error(constraint, "expected TritWidth constraint");
                }
            } while (match(TokenKind::Comma));
            consume(TokenKind::Greater, "expected '>' after width parameters");
        }
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
        active_width_params_.clear();
        return fn;
    }

    [[nodiscard]] ConstDecl parseConstDecl() {
        SourceSpan span = previous().span;
        Token name = consumeIdentifier("expected constant name");
        consume(TokenKind::Colon, "expected ':' after constant name");
        TypeRef type = parseType();
        consume(TokenKind::Equal, "expected '=' after constant type");
        ExprPtr expr = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after constant declaration");
        
        checkAndRegisterConst(name.text, type, expr, file_scope_consts_, ConstantTable{}, *diagnostics_, span);
        
        return ConstDecl{name.text, type, expr, span};
    }

    [[nodiscard]] Stmt parseConstStmt() {
        SourceSpan span = previous().span;
        Token name = consumeIdentifier("expected constant name");
        consume(TokenKind::Colon, "expected ':' after constant name");
        TypeRef type = parseType();
        consume(TokenKind::Equal, "expected '=' after constant type");
        ExprPtr expr = parseExpr();
        consume(TokenKind::Semicolon, "expected ';' after constant declaration");
        
        if (!const_scopes_.empty()) {
            checkAndRegisterConst(name.text, type, expr, const_scopes_.back(), file_scope_consts_, *diagnostics_, span);
        } else {
            ConstantTable dummy;
            checkAndRegisterConst(name.text, type, expr, dummy, file_scope_consts_, *diagnostics_, span);
        }
        
        Stmt stmt;
        stmt.kind = StmtKind::Const;
        stmt.name = name.text;
        stmt.annotation = type;
        stmt.expr = expr;
        stmt.span = span;
        return stmt;
    }

    [[nodiscard]] std::vector<Stmt> parseBlock() {
        consume(TokenKind::LBrace, "expected '{'");
        const_scopes_.push_back(ConstantTable{});
        std::vector<Stmt> body;
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            body.push_back(parseStmt());
        }
        const_scopes_.pop_back();
        consume(TokenKind::RBrace, "expected '}'");
        return body;
    }

    [[nodiscard]] Stmt parseStmt() {
        std::size_t start_pos = current_;
        Stmt s = parseStmtImpl();
        if (current_ == start_pos && !isAtEnd()) {
            advance();
        }
        return s;
    }

    [[nodiscard]] Stmt parseStmtImpl() {
        if (matchKeyword("let")) return parseLet(false);
        if (matchKeyword("var")) return parseLet(true);
        if (matchKeyword("const")) return parseConstStmt();
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
            error(spanToken(span), "v1 tuple assignment only supports direct variable swap (e.g. '(a, b) = (b, a);')");
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
        stmt.expr = parseExpr();
        stmt.body = parseBlock();
        return stmt;
    }

    [[nodiscard]] Stmt parseMatch() {
        Stmt stmt;
        stmt.kind = StmtKind::MatchSign;
        stmt.span = previous().span;
        stmt.expr = parseExpr();
        consume(TokenKind::LBrace, "expected '{' after match expression");
        
        std::set<std::string> seenArms;
        while (!check(TokenKind::RBrace) && !isAtEnd()) {
            MatchArm arm;
            Token armName = consumeIdentifier("expected match arm name");
            arm.name = armName.text;
            arm.span = armName.span;
            
            if (seenArms.count(arm.name)) {
                error(armName, "duplicate match arm: '" + arm.name + "'");
            }
            seenArms.insert(arm.name);
            
            if (arm.name == "valid") {
                if (match(TokenKind::LParen)) {
                    arm.binding = consumeIdentifier("expected binding identifier").text;
                    consume(TokenKind::RParen, "expected ')'");
                }
            } else if (arm.name == "_") {
                if (check(TokenKind::LParen)) {
                    error(peek(), "wildcard '_' arm cannot have a binding");
                }
            } else if (arm.name != "neg" && arm.name != "zero" && arm.name != "pos" &&
                       arm.name != "null" && arm.name != "unknown") {
                error(armName, "invalid match arm name");
            }
            
            consume(TokenKind::FatArrow, "expected '=>'");
            arm.body = parseBlock();
            stmt.arms.push_back(arm);
        }
        consume(TokenKind::RBrace, "expected '}'");
        return stmt;
    }

    [[nodiscard]] ExprPtr parseExpr() { return parseComparison(); }

    [[nodiscard]] ExprPtr parseComparison() {
        ExprPtr expr = parseAddSub();
        while (match(TokenKind::Less) || match(TokenKind::LessEqual) ||
               match(TokenKind::Greater) || match(TokenKind::GreaterEqual) ||
               match(TokenKind::EqualEqual) || match(TokenKind::BangEqual)) {
            Token op = previous();
            ExprPtr rhs = parseAddSub();
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
            if (std::isupper(static_cast<unsigned char>(name.text[0])) && match(TokenKind::LBrace)) {
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
            ExprPtr lenExpr = parseExpr();
            consume(TokenKind::RBracket, "expected ']' after array type");
            ConstantTable combined = file_scope_consts_;
            for (const auto& scope : const_scopes_) {
                for (const auto& pair : scope) {
                    combined[pair.first] = pair.second;
                }
            }
            long long val = 0;
            TypeRef type;
            if (!evalConstantExpr(lenExpr, combined, val, type, /*strict=*/true, *diagnostics_)) {
                error(peek(), "array length must be a compile-time constant expression");
            }
            if (val <= 0) {
                error(peek(), "array length must be positive, found " + std::to_string(val));
            }
            TypeRef out;
            out.kind = TypeKind::Array;
            out.element = std::make_shared<TypeRef>(elem);
            out.array_len = static_cast<int>(val);
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
            if (match(TokenKind::Comma)) {
                Token qualifier = consumeIdentifier("expected pointer region or state");
                if (isPointerRegionName(qualifier.text)) {
                    region = regionFromName(qualifier);
                    if (match(TokenKind::Comma)) state = parsePointerState();
                } else if (isPointerStateName(qualifier.text)) {
                    state = stateFromName(qualifier);
                } else {
                    error(qualifier, "unknown pointer region or state");
                }
            }
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
        } else if (name.text == "own") {
            consume(TokenKind::Less, "expected '<' after own");
            base = TypeRef::owned(parseType());
            consume(TokenKind::Greater, "expected '>' after own type");
        } else if (name.text == "borrow") {
            consume(TokenKind::Less, "expected '<' after borrow");
            base = TypeRef::borrow(parseType());
            consume(TokenKind::Greater, "expected '>' after borrow type");
        } else if (name.text == "borrow_mut") {
            consume(TokenKind::Less, "expected '<' after borrow_mut");
            base = TypeRef::borrowMut(parseType());
            consume(TokenKind::Greater, "expected '>' after borrow_mut type");
        } else if (name.text == "T") {
            consume(TokenKind::Less, "expected '<' after T");
            Token width_param = consumeIdentifier("expected width parameter");
            consume(TokenKind::Greater, "expected '>' after T type");
            if (active_width_params_.find(width_param.text) == active_width_params_.end()) {
                error(width_param, "undefined width parameter '" + width_param.text + "'");
            }
            TypeRef out;
            out.kind = TypeKind::Numeric;
            out.width_var = width_param.text;
            base = out;
        }
        return base;
    }

    [[nodiscard]] TypeRef parseNamedType(const Token& name) {
        if (name.text == "t1" || name.text == "trit") return TypeRef::trit();
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
        if (isPointerRegionName(token.text)) return regionFromName(token);
        error(token, "unknown pointer region");
        return PointerRegion::Stack;
    }

    [[nodiscard]] PointerState parsePointerState() {
        Token token = consumeIdentifier("expected pointer state");
        if (isPointerStateName(token.text)) return stateFromName(token);
        error(token, "unknown pointer state");
        return PointerState::Unknown;
    }

    [[nodiscard]] static bool isPointerRegionName(const std::string& name) {
        return name == "stack" || name == "static" ||
               name == "user" || name == "kernel";
    }

    [[nodiscard]] static bool isPointerStateName(const std::string& name) {
        return name == "valid" || name == "unknown" || name == "null";
    }

    [[nodiscard]] static PointerRegion regionFromName(const Token& token) {
        if (token.text == "static") return PointerRegion::Static;
        if (token.text == "user") return PointerRegion::User;
        if (token.text == "kernel") return PointerRegion::Kernel;
        return PointerRegion::Stack;
    }

    [[nodiscard]] static PointerState stateFromName(const Token& token) {
        if (token.text == "valid") return PointerState::Valid;
        if (token.text == "null") return PointerState::Null;
        return PointerState::Unknown;
    }

    [[nodiscard]] int parseOrderLiteral() {
        if (matchKeyword("RELAXED")) return -1;
        if (matchKeyword("ACQ_REL")) return 0;
        if (matchKeyword("SEQ_CST")) return 1;

        if (check(TokenKind::Identifier)) {
            std::string t = peek().text;
            if (t == "RELAXED" || t == "ACQ_REL" || t == "SEQ_CST") {
                consumeIdentifier("expected memory order");
                return (t == "RELAXED") ? -1 : (t == "ACQ_REL") ? 0 : 1;
            }
        }

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

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_PARSER_H
