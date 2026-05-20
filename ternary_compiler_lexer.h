// ternary_compiler_lexer.h - Lexer for the ternary compiler language
#pragma once
#ifndef TERNARY_COMPILER_LEXER_H
#define TERNARY_COMPILER_LEXER_H

#include "ternary_compiler_types.h"

namespace sandbox {
namespace compiler {

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
    LessEqual,
    GreaterEqual,
    EqualEqual,
    BangEqual,
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
                case '<':
                    if (peek() == '=') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::LessEqual, "<=", span));
                    } else {
                        out.push_back(tok(TokenKind::Less, "<", span));
                    }
                    break;
                case '>':
                    if (peek() == '=') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::GreaterEqual, ">=", span));
                    } else {
                        out.push_back(tok(TokenKind::Greater, ">", span));
                    }
                    break;
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
                    } else if (peek() == '=') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::EqualEqual, "==", span));
                    } else {
                        out.push_back(tok(TokenKind::Equal, "=", span));
                    }
                    break;
                case '!':
                    if (peek() == '=') {
                        advance();
                        span.length = 2;
                        out.push_back(tok(TokenKind::BangEqual, "!=", span));
                    } else {
                        diagnostics.push_back({DiagnosticSeverity::Error,
                            "unexpected character '!'", span});
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

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_LEXER_H
