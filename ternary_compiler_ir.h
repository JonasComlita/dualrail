// ternary_compiler_ir.h - Structural SSA IR, object interfaces, and runtime constants

#pragma once
#ifndef TERNARY_COMPILER_IR_H
#define TERNARY_COMPILER_IR_H

#include "ternary_compiler_types.h"

namespace sandbox {
namespace compiler {

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
    int ir_value_ceiling = 1;
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
    std::vector<std::string> function_order;
    std::map<std::string, std::string> function_sections;
    std::map<std::string, std::set<std::string>> function_refs;
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
    bool dead_strip_functions = false;
    std::vector<std::string> dead_strip_roots = {"main"};
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
static constexpr int sys_exit = 44;
static constexpr int sys_getpid = 7;
static constexpr int sys_uptime = 8;
static constexpr int sys_read_console_word = 9;
static constexpr int sys_spawn_static = 10;
static constexpr int sys_waitpid = 43;
static constexpr int sys_open = 12;
static constexpr int sys_close = 13;
static constexpr int sys_read = 14;
static constexpr int sys_write = 15;
static constexpr int sys_stat = 16;
static constexpr int sys_readdir = 17;
static constexpr int sys_brk = 18;
static constexpr int sys_sbrk = 19;
static constexpr int sys_fork = 20;
static constexpr int sys_exec = 21;
static constexpr int sys_write_char = 22;
static constexpr int sys_ipc_send = 23;
static constexpr int sys_ipc_recv = 24;
static constexpr int sys_fb_init = 25;
static constexpr int sys_fb_flip = 26;
static constexpr int sys_window_create = 27;
static constexpr int sys_window_get_buffer = 28;
static constexpr int sys_window_present = 29;
static constexpr int sys_window_move = 30;
static constexpr int sys_window_set_z = 31;
static constexpr int sys_window_destroy = 32;
static constexpr int sys_window_read_event = 33;
static constexpr int sys_window_resize = 34;
static constexpr int sys_window_request_close = 35;
static constexpr int sys_socket = 36;
static constexpr int sys_bind = 37;
static constexpr int sys_connect = 38;
static constexpr int sys_send = 39;
static constexpr int sys_recv = 40;
static constexpr int sys_mkdir = 41;
static constexpr int sys_unlink = 42;
static constexpr int sys_sleep = 45;
static constexpr int sys_ps = 46;
} // namespace runtime

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_IR_H
