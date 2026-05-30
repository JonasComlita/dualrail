// =============================================================================
// ternary_compiler.h - Phase 7 structural compiler layer
// =============================================================================
//
// This layer sits beside the legacy Program builder in ternary_ir.h. It provides
// a small ternary-native frontend, structural SSA-facing data types, conservative
// type inference, runtime syscall lowering, and static executable linking through
// the existing assembler/VM contract.
//
// This is the umbrella header. Sub-headers provide the individual components:
//   ternary_compiler_types.h   - Type model, diagnostics, options, unification
//   ternary_compiler_ir.h      - Structural SSA IR, object/link interfaces
//   ternary_compiler_lexer.h   - Lexer (TokenKind, Token, Lexer)
//   ternary_compiler_ast.h     - AST nodes, constant evaluation, folding
//   ternary_compiler_parser.h  - Recursive descent parser
//   ternary_compiler_codegen.h - Type inference, codegen, verifier, optimizer

#pragma once
#ifndef TERNARY_COMPILER_H
#define TERNARY_COMPILER_H

#include "ternary_compiler_types.h"
#include "ternary_compiler_ir.h"
#include "ternary_compiler_lexer.h"
#include "ternary_compiler_ast.h"
#include "ternary_compiler_parser.h"
#include "ternary_compiler_codegen.h"

#include <algorithm>

namespace sandbox {
namespace compiler {

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
    foldModuleConstants(ast, diagnostics);
    bool parser_has_errors = false;
    for (const auto& d : diagnostics) {
        if (d.severity == DiagnosticSeverity::Error) {
            parser_has_errors = true;
            break;
        }
    }
    if (parser_has_errors) {
        CompileResult result;
        result.success = false;
        result.diagnostics = std::move(diagnostics);
        return result;
    }
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
    auto dataPages = [&]() {
        return std::max(1, (options.stack_hint_words + vm::MMU_PAGE_WORDS - 1) / vm::MMU_PAGE_WORDS);
    };
    auto selectedFunctionOrder = [&]() {
        std::vector<std::pair<const ObjectModule*, std::string>> order;
        if (!options.dead_strip_functions) {
            for (const auto& module : modules) {
                for (const auto& fn : module.function_order) {
                    order.push_back({&module, fn});
                }
            }
            return order;
        }

        std::map<std::string, std::set<std::string>> refs;
        std::set<std::string> defined;
        for (const auto& module : modules) {
            for (const auto& fn : module.function_order) {
                defined.insert(fn);
                auto it = module.function_refs.find(fn);
                if (it != module.function_refs.end()) {
                    refs[fn].insert(it->second.begin(), it->second.end());
                }
            }
        }

        std::set<std::string> reachable;
        std::vector<std::string> stack = options.dead_strip_roots;
        if (stack.empty()) stack.push_back("main");
        while (!stack.empty()) {
            std::string fn = stack.back();
            stack.pop_back();
            if (reachable.count(fn)) continue;
            reachable.insert(fn);
            auto ref_it = refs.find(fn);
            if (ref_it == refs.end()) continue;
            for (const auto& ref : ref_it->second) {
                if (defined.count(ref) && !reachable.count(ref)) {
                    stack.push_back(ref);
                }
            }
        }

        for (const auto& module : modules) {
            for (const auto& fn : module.function_order) {
                if (reachable.count(fn)) {
                    order.push_back({&module, fn});
                }
            }
        }
        return order;
    };

    auto functionOrder = selectedFunctionOrder();
    auto buildAssembly = [&](int text_pages) {
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
        if (options.dead_strip_functions) {
            for (const auto& entry : functionOrder) {
                auto section_it = entry.first->function_sections.find(entry.second);
                if (section_it != entry.first->function_sections.end()) {
                    asmOut << section_it->second;
                }
            }
            for (const auto& module : modules) {
                if (module.function_order.empty() && !module.assembly.empty()) {
                    asmOut << module.assembly;
                }
            }
        } else {
            for (const auto& module : modules) {
                asmOut << module.assembly;
            }
        }
        asmOut << ".data\n";
        asmOut << "phase7_exec: .execheader 0, " << text_pages << ", "
               << dataPages() << ", "
               << options.stack_hint_words << ", "
               << options.syscall_abi_version << ", "
               << options.flags << "\n";
        return asmOut.str();
    };
    for (const auto& module : modules) {
        for (const auto& symbol : module.symbols) {
            if (symbols.count(symbol.first)) {
                result.diagnostics.push_back({DiagnosticSeverity::Error,
                    "duplicate symbol '" + symbol.first + "'", SourceSpan{module.name, 1, 1, 1}});
            }
            symbols.insert(symbol.first);
        }
    }

    result.assembly = buildAssembly(1);
    result.assembled = vm::assembler::assemble(result.assembly);
    if (result.assembled.success) {
        const int computed_text_pages =
            std::max(1, (static_cast<int>(result.assembled.program.size()) +
                         vm::MMU_PAGE_WORDS - 1) / vm::MMU_PAGE_WORDS);
        if (computed_text_pages != 1) {
            result.assembly = buildAssembly(computed_text_pages);
            result.assembled = vm::assembler::assemble(result.assembly);
        }
    }
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
