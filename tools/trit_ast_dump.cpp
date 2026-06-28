#include "ternary_compiler_ast.h"
#include "ternary_compiler_lexer.h"
#include "ternary_compiler_parser.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using sandbox::compiler::ConstDecl;
using sandbox::compiler::Diagnostic;
using sandbox::compiler::DiagnosticSeverity;
using sandbox::compiler::ExprKind;
using sandbox::compiler::ExprPtr;
using sandbox::compiler::FunctionAst;
using sandbox::compiler::ImportDecl;
using sandbox::compiler::Lexer;
using sandbox::compiler::MatchArm;
using sandbox::compiler::ModuleAst;
using sandbox::compiler::Parser;
using sandbox::compiler::SourceSpan;
using sandbox::compiler::Stmt;
using sandbox::compiler::StmtKind;
using sandbox::compiler::StructDecl;

struct CallRef {
    std::string name;
    SourceSpan span;
};

struct LocalConstRef {
    std::string name;
    std::string type;
    SourceSpan span;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in.good()) return {};
    std::string out;
    in.seekg(0, std::ios::end);
    out.resize(static_cast<std::size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u";
                    const char* digits = "0123456789abcdef";
                    out << "00"
                        << digits[(static_cast<unsigned char>(ch) >> 4) & 0x0f]
                        << digits[static_cast<unsigned char>(ch) & 0x0f];
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

void emitString(std::ostream& out, const std::string& value) {
    out << '"' << jsonEscape(value) << '"';
}

void emitSpan(std::ostream& out, const SourceSpan& span) {
    out << "{\"file\":";
    emitString(out, span.file);
    out << ",\"line\":" << span.line
        << ",\"column\":" << span.column
        << ",\"length\":" << span.length
        << "}";
}

const char* severityName(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Info: return "info";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Error: return "error";
    }
    return "error";
}

void collectCallsFromExpr(const ExprPtr& expr, std::vector<CallRef>& calls) {
    if (!expr) return;
    if (expr->kind == ExprKind::Call && !expr->text.empty()) {
        calls.push_back({expr->text, expr->span});
    }
    collectCallsFromExpr(expr->left, calls);
    collectCallsFromExpr(expr->right, calls);
    for (const auto& arg : expr->args) collectCallsFromExpr(arg, calls);
    for (const auto& field : expr->fields) collectCallsFromExpr(field.second, calls);
}

void collectCallsFromBlock(const std::vector<Stmt>& body, std::vector<CallRef>& calls);

void collectCallsFromStmt(const Stmt& stmt, std::vector<CallRef>& calls) {
    collectCallsFromExpr(stmt.expr, calls);
    collectCallsFromExpr(stmt.rhs, calls);
    collectCallsFromExpr(stmt.target, calls);
    collectCallsFromBlock(stmt.body, calls);
    collectCallsFromBlock(stmt.else_body, calls);
    for (const MatchArm& arm : stmt.arms) {
        collectCallsFromBlock(arm.body, calls);
    }
}

void collectCallsFromBlock(const std::vector<Stmt>& body, std::vector<CallRef>& calls) {
    for (const Stmt& stmt : body) collectCallsFromStmt(stmt, calls);
}

void collectLocalConstsFromBlock(const std::vector<Stmt>& body, std::vector<LocalConstRef>& out);

void collectLocalConstsFromStmt(const Stmt& stmt, std::vector<LocalConstRef>& out) {
    if (stmt.kind == StmtKind::Const) {
        out.push_back({stmt.name, stmt.annotation.str(), stmt.span});
    }
    collectLocalConstsFromBlock(stmt.body, out);
    collectLocalConstsFromBlock(stmt.else_body, out);
    for (const MatchArm& arm : stmt.arms) {
        collectLocalConstsFromBlock(arm.body, out);
    }
}

void collectLocalConstsFromBlock(const std::vector<Stmt>& body, std::vector<LocalConstRef>& out) {
    for (const Stmt& stmt : body) collectLocalConstsFromStmt(stmt, out);
}

void emitDiagnostics(std::ostream& out, const std::vector<Diagnostic>& diagnostics) {
    out << '[';
    for (std::size_t i = 0; i < diagnostics.size(); ++i) {
        if (i != 0) out << ',';
        out << "{\"severity\":";
        emitString(out, severityName(diagnostics[i].severity));
        out << ",\"message\":";
        emitString(out, diagnostics[i].message);
        out << ",\"span\":";
        emitSpan(out, diagnostics[i].span);
        out << '}';
    }
    out << ']';
}

void emitImports(std::ostream& out, const std::vector<ImportDecl>& imports) {
    out << '[';
    for (std::size_t i = 0; i < imports.size(); ++i) {
        if (i != 0) out << ',';
        out << "{\"name\":";
        emitString(out, imports[i].name);
        out << ",\"span\":";
        emitSpan(out, imports[i].span);
        out << '}';
    }
    out << ']';
}

void emitStructs(std::ostream& out, const std::vector<StructDecl>& structs) {
    out << '[';
    for (std::size_t i = 0; i < structs.size(); ++i) {
        if (i != 0) out << ',';
        out << "{\"name\":";
        emitString(out, structs[i].name);
        out << ",\"span\":";
        emitSpan(out, structs[i].span);
        out << ",\"fields\":[";
        for (std::size_t j = 0; j < structs[i].fields.size(); ++j) {
            if (j != 0) out << ',';
            out << "{\"name\":";
            emitString(out, structs[i].fields[j].first);
            out << ",\"type\":";
            emitString(out, structs[i].fields[j].second.str());
            out << '}';
        }
        out << "]}";
    }
    out << ']';
}

void emitTopLevelConsts(std::ostream& out, const std::vector<ConstDecl>& consts) {
    out << '[';
    for (std::size_t i = 0; i < consts.size(); ++i) {
        if (i != 0) out << ',';
        std::vector<CallRef> calls;
        collectCallsFromExpr(consts[i].expr, calls);
        out << "{\"name\":";
        emitString(out, consts[i].name);
        out << ",\"type\":";
        emitString(out, consts[i].type.str());
        out << ",\"span\":";
        emitSpan(out, consts[i].span);
        out << ",\"calls\":[";
        for (std::size_t j = 0; j < calls.size(); ++j) {
            if (j != 0) out << ',';
            out << "{\"name\":";
            emitString(out, calls[j].name);
            out << ",\"span\":";
            emitSpan(out, calls[j].span);
            out << '}';
        }
        out << "]}";
    }
    out << ']';
}

void emitFunctions(std::ostream& out, const std::vector<FunctionAst>& functions) {
    out << '[';
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (i != 0) out << ',';
        const FunctionAst& fn = functions[i];
        std::vector<CallRef> calls;
        std::vector<LocalConstRef> local_consts;
        collectCallsFromBlock(fn.body, calls);
        collectLocalConstsFromBlock(fn.body, local_consts);

        out << "{\"name\":";
        emitString(out, fn.name);
        out << ",\"span\":";
        emitSpan(out, fn.span);
        out << ",\"return_type\":";
        emitString(out, fn.return_type.str());
        out << ",\"width_params\":[";
        for (std::size_t j = 0; j < fn.width_params.size(); ++j) {
            if (j != 0) out << ',';
            emitString(out, fn.width_params[j]);
        }
        out << "],\"params\":[";
        for (std::size_t j = 0; j < fn.params.size(); ++j) {
            if (j != 0) out << ',';
            out << "{\"name\":";
            emitString(out, fn.params[j].first);
            out << ",\"type\":";
            emitString(out, fn.params[j].second.str());
            out << '}';
        }
        out << "],\"local_consts\":[";
        for (std::size_t j = 0; j < local_consts.size(); ++j) {
            if (j != 0) out << ',';
            out << "{\"name\":";
            emitString(out, local_consts[j].name);
            out << ",\"type\":";
            emitString(out, local_consts[j].type);
            out << ",\"span\":";
            emitSpan(out, local_consts[j].span);
            out << '}';
        }
        out << "],\"calls\":[";
        for (std::size_t j = 0; j < calls.size(); ++j) {
            if (j != 0) out << ',';
            out << "{\"name\":";
            emitString(out, calls[j].name);
            out << ",\"span\":";
            emitSpan(out, calls[j].span);
            out << '}';
        }
        out << "]}";
    }
    out << ']';
}

void emitModule(std::ostream& out, const std::string& path) {
    std::string source = readFile(path);
    std::vector<Diagnostic> diagnostics;
    if (source.empty()) {
        diagnostics.push_back({DiagnosticSeverity::Error, "could not read source file", SourceSpan{path, 1, 1, 1}});
    }

    ModuleAst module;
    module.name = path;
    if (!source.empty()) {
        Lexer lexer(path, source);
        std::vector<sandbox::compiler::Token> tokens = lexer.lex(diagnostics);
        Parser parser(path, std::move(tokens));
        module = parser.parse(diagnostics);
    }

    bool has_error = false;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            has_error = true;
            break;
        }
    }

    out << "{\"source_file\":";
    emitString(out, path);
    out << ",\"ok\":" << (has_error ? "false" : "true");
    out << ",\"diagnostics\":";
    emitDiagnostics(out, diagnostics);
    out << ",\"imports\":";
    emitImports(out, module.imports);
    out << ",\"structs\":";
    emitStructs(out, module.structs);
    out << ",\"consts\":";
    emitTopLevelConsts(out, module.consts);
    out << ",\"functions\":";
    emitFunctions(out, module.functions);
    out << '}';
}

void showUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " <source.trit> [more.trit ...]\n";
    std::cerr << "Emits compiler Parser/ModuleAst JSON for knowledge graph tooling.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        showUsage(argv[0]);
        return 1;
    }

    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            showUsage(argv[0]);
            return 0;
        }
        paths.push_back(std::move(arg));
    }

    std::cout << "{\"schema\":\"trit-ast-v1\",\"modules\":[";
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) std::cout << ',';
        emitModule(std::cout, paths[i]);
    }
    std::cout << "]}\n";
    return 0;
}
