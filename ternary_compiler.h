// =============================================================================
// ternary_compiler.h - Phase 7 structural compiler layer
// =============================================================================
//
// This layer provides
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
#include "ternary_compiler_cfg.h"
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
        result.object.ssa = result.ssa_module;
    }
    return result;
}

[[nodiscard]] inline LinkResult linkModules(
    const std::vector<ObjectModule>& modules,
    const LinkOptions& options = LinkOptions{}) {

    LinkResult result;
    std::set<std::string> symbols;
    const int architectural_stack_words =
        ((std::max(
              architecture::v2::STACK_ALIGNMENT_WORDS,
              options.stack_hint_words) +
          architecture::v2::STACK_ALIGNMENT_WORDS - 1) /
         architecture::v2::STACK_ALIGNMENT_WORDS) *
        architecture::v2::STACK_ALIGNMENT_WORDS;
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
    std::uint64_t required_features = options.required_features;
    for (const auto& module : modules) {
        std::vector<vm::assembler::AssemblyError> parse_errors;
        const auto source_lines =
            vm::assembler::parseSources(module.assembly, parse_errors);
        for (const auto& source_line : source_lines) {
            const auto parts =
                vm::assembler::splitMnemonic(source_line.mnemonic);
            const auto opcode =
                vm::assembler::OPCODE_TABLE.find(parts.base);
            if (opcode == vm::assembler::OPCODE_TABLE.end()) continue;
            isa::InstructionWord semantic;
            semantic.opcode = opcode->second.opcode;
            semantic.func = parts.func;
            required_features |= isa::requiredV2Features(semantic);
        }
    }

    auto buildAssembly = [&](int text_measure, int data_words) {
        std::ostringstream asmOut;
        asmOut << ".isa 2\n";
        const auto requireFeature = [&](int trit, const char* name) {
            if ((required_features & isa::featureBit(trit)) != 0 &&
                trit != architecture::v2::FEATURE_BASE_V2) {
                asmOut << ".require " << name << "\n";
            }
        };
        requireFeature(architecture::v2::FEATURE_SCALAR_ADVANCED, "scalar_advanced");
        requireFeature(architecture::v2::FEATURE_LANE, "lane");
        requireFeature(architecture::v2::FEATURE_VECTOR, "vector");
        requireFeature(architecture::v2::FEATURE_ACCUMULATOR_AI, "accumulator_ai");
        requireFeature(architecture::v2::FEATURE_ATOMICS, "atomics");
        requireFeature(architecture::v2::FEATURE_MMU, "mmu");
        requireFeature(architecture::v2::FEATURE_WAIT, "wait");
        requireFeature(architecture::v2::FEATURE_WIDE_T50, "wide_t50");
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
            for (const auto& module : modules) asmOut << module.assembly;
        }
        asmOut << ".data\n";
        asmOut << "phase7_exec: .execheader2 0, " << text_measure << ", "
               << data_words << ", " << architectural_stack_words << ", "
               << isa::featureWordNumeric(required_features) << ", "
               << options.syscall_abi_version << ", " << options.flags << "\n";
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

    const vm::assembler::AssemblyOptions assembly_options{
        isa::IsaEncodingVersion::V2, true};
    result.assembly = buildAssembly(1, 0);
    result.assembled =
        vm::assembler::assemble(result.assembly, assembly_options);
    if (result.assembled.success) {
        int text_measure = 1;
        int data_words = 0;
        text_measure = static_cast<int>(result.assembled.program.size());
        data_words = std::max(
            0, static_cast<int>(result.assembled.data.size()) -
                   vm::EXEC_V2_HEADER_WORDS);
        if (text_measure != 1 || data_words != 0) {
            result.assembly = buildAssembly(text_measure, data_words);
            result.assembled =
                vm::assembler::assemble(result.assembly, assembly_options);
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
    if (result.assembled.executable_headers_v2.count("phase7_exec")) {
        result.executable_header_v2 =
            result.assembled.executable_headers_v2.at("phase7_exec");
    }
    for (const auto& module : modules) {
        for (const auto& symbol : module.symbols) result.symbol_map[symbol.first] = symbol.second;
    }
    return result;
}

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_H
