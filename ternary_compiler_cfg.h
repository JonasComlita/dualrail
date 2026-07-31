#pragma once
#ifndef TERNARY_COMPILER_CFG_H
#define TERNARY_COMPILER_CFG_H

#include "ternary_compiler_ir.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sandbox {
namespace compiler {

struct ControlFlowGraph {
    std::vector<std::string> order;
    std::map<std::string, std::size_t> index;
    std::map<std::string, std::set<std::string>> predecessors;
    std::map<std::string, std::set<std::string>> successors;
    std::vector<std::string> invalid_targets;
};

[[nodiscard]] inline ControlFlowGraph buildControlFlowGraph(
    const Function& function) {
    ControlFlowGraph cfg;
    for (std::size_t i = 0; i < function.blocks.size(); ++i) {
        cfg.order.push_back(function.blocks[i].name);
        cfg.index[function.blocks[i].name] = i;
        cfg.predecessors[function.blocks[i].name];
        cfg.successors[function.blocks[i].name];
    }
    auto addEdge = [&](const std::string& from, const std::string& to) {
        if (to.empty()) return;
        if (!cfg.index.count(to)) {
            cfg.invalid_targets.push_back(to);
            return;
        }
        cfg.successors[from].insert(to);
        cfg.predecessors[to].insert(from);
    };
    for (const BasicBlock& block : function.blocks) {
        const Terminator& term = block.terminator;
        if (term.kind == TerminatorKind::Jump) {
            addEdge(block.name, term.target);
        } else if (term.kind == TerminatorKind::Branch3) {
            addEdge(block.name, term.target_neg);
            addEdge(block.name, term.target_zero);
            addEdge(block.name, term.target_pos);
        }
    }
    std::sort(cfg.invalid_targets.begin(), cfg.invalid_targets.end());
    cfg.invalid_targets.erase(
        std::unique(cfg.invalid_targets.begin(), cfg.invalid_targets.end()),
        cfg.invalid_targets.end());
    return cfg;
}

// Phi copies conceptually execute on incoming edges. Split any edge whose
// predecessor has multiple successors and whose phi-bearing successor has
// multiple predecessors so target lowering always has a concrete block in
// which to materialize those copies.
[[nodiscard]] inline int splitCriticalPhiEdges(Function& function) {
    const ControlFlowGraph cfg = buildControlFlowGraph(function);
    if (!cfg.invalid_targets.empty()) return 0;

    struct Split {
        std::string predecessor;
        std::string successor;
        std::string block;
    };
    std::set<std::string> names;
    for (const BasicBlock& block : function.blocks)
        names.insert(block.name);
    std::vector<Split> splits;
    for (const BasicBlock& successor : function.blocks) {
        const bool has_phi = std::any_of(
            successor.instructions.begin(),
            successor.instructions.end(),
            [](const Instr& instr) {
                return instr.opcode == InstrOpcode::Phi;
            });
        if (!has_phi ||
            cfg.predecessors.at(successor.name).size() < 2) {
            continue;
        }
        for (const std::string& predecessor :
             cfg.predecessors.at(successor.name)) {
            if (cfg.successors.at(predecessor).size() < 2)
                continue;
            std::string name =
                function.name + "_critical_phi_edge_" +
                std::to_string(splits.size());
            int suffix = 0;
            while (names.count(name)) {
                name = function.name +
                       "_critical_phi_edge_" +
                       std::to_string(splits.size()) +
                       "_" + std::to_string(++suffix);
            }
            names.insert(name);
            splits.push_back(
                Split{predecessor, successor.name, name});
        }
    }

    auto redirect = [](
        Terminator& terminator,
        const std::string& from,
        const std::string& to) {
        if (terminator.kind == TerminatorKind::Jump) {
            if (terminator.target == from)
                terminator.target = to;
            return;
        }
        if (terminator.kind != TerminatorKind::Branch3)
            return;
        if (terminator.target_neg == from)
            terminator.target_neg = to;
        if (terminator.target_zero == from)
            terminator.target_zero = to;
        if (terminator.target_pos == from)
            terminator.target_pos = to;
    };
    for (const Split& split : splits) {
        BasicBlock& predecessor =
            function.blocks[
                cfg.index.at(split.predecessor)];
        BasicBlock& successor =
            function.blocks[
                cfg.index.at(split.successor)];
        redirect(
            predecessor.terminator,
            split.successor,
            split.block);
        for (Instr& instr : successor.instructions) {
            if (instr.opcode != InstrOpcode::Phi)
                continue;
            for (auto& incoming : instr.phi_incoming) {
                if (incoming.first == split.predecessor)
                    incoming.first = split.block;
            }
        }
        BasicBlock edge;
        edge.name = split.block;
        edge.terminator.kind = TerminatorKind::Jump;
        edge.terminator.target = split.successor;
        function.blocks.push_back(std::move(edge));
    }
    return static_cast<int>(splits.size());
}

struct DominanceInfo {
    std::map<std::string, std::set<std::string>> dominators;
    std::map<std::string, std::string> immediate_dominator;
    std::map<std::string, std::set<std::string>> frontier;

    [[nodiscard]] bool dominates(const std::string& candidate,
                                 const std::string& block) const {
        const auto found = dominators.find(block);
        return found != dominators.end() && found->second.count(candidate) != 0;
    }
};

[[nodiscard]] inline DominanceInfo computeDominance(
    const Function& function,
    const ControlFlowGraph& cfg) {
    DominanceInfo info;
    if (cfg.order.empty()) return info;
    const std::string& entry = cfg.order.front();
    const std::set<std::string> all(cfg.order.begin(), cfg.order.end());
    for (const std::string& block : cfg.order) {
        info.dominators[block] = block == entry
            ? std::set<std::string>{entry}
            : all;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 1; i < cfg.order.size(); ++i) {
            const std::string& block = cfg.order[i];
            const auto& preds = cfg.predecessors.at(block);
            std::set<std::string> next;
            bool first = true;
            for (const std::string& pred : preds) {
                if (first) {
                    next = info.dominators[pred];
                    first = false;
                } else {
                    std::set<std::string> intersection;
                    std::set_intersection(
                        next.begin(), next.end(),
                        info.dominators[pred].begin(),
                        info.dominators[pred].end(),
                        std::inserter(intersection, intersection.begin()));
                    next = std::move(intersection);
                }
            }
            if (first) next.clear(); // unreachable block
            next.insert(block);
            if (next != info.dominators[block]) {
                info.dominators[block] = std::move(next);
                changed = true;
            }
        }
    }

    info.immediate_dominator[entry] = "";
    for (std::size_t i = 1; i < cfg.order.size(); ++i) {
        const std::string& block = cfg.order[i];
        std::set<std::string> strict = info.dominators[block];
        strict.erase(block);
        std::string idom;
        for (const std::string& candidate : strict) {
            bool dominated_by_other = false;
            for (const std::string& other : strict) {
                if (candidate == other) continue;
                if (info.dominates(candidate, other)) {
                    dominated_by_other = true;
                    break;
                }
            }
            if (!dominated_by_other) {
                idom = candidate;
                break;
            }
        }
        info.immediate_dominator[block] = idom;
    }

    for (const std::string& block : cfg.order) info.frontier[block];
    for (const std::string& block : cfg.order) {
        const auto& preds = cfg.predecessors.at(block);
        if (preds.size() < 2) continue;
        for (const std::string& pred : preds) {
            std::string runner = pred;
            while (!runner.empty() &&
                   runner != info.immediate_dominator[block]) {
                info.frontier[runner].insert(block);
                runner = info.immediate_dominator[runner];
            }
        }
    }
    (void)function;
    return info;
}

struct BlockLiveness {
    std::map<std::string, std::set<ValueId>> use;
    std::map<std::string, std::set<ValueId>> def;
    std::map<std::string, std::set<ValueId>> live_in;
    std::map<std::string, std::set<ValueId>> live_out;
};

[[nodiscard]] inline BlockLiveness computeBlockLiveness(
    const Function& function,
    const ControlFlowGraph& cfg) {
    BlockLiveness result;
    for (const BasicBlock& block : function.blocks) {
        std::set<ValueId>& use = result.use[block.name];
        std::set<ValueId>& def = result.def[block.name];
        for (const Instr& instr : block.instructions) {
            if (instr.opcode != InstrOpcode::Phi) {
                for (ValueId arg : instr.args) {
                    if (arg >= 0 && !def.count(arg)) use.insert(arg);
                }
            }
            if (instr.def >= 0) def.insert(instr.def);
        }
        if (block.terminator.condition >= 0 &&
            !def.count(block.terminator.condition)) {
            use.insert(block.terminator.condition);
        }
        result.live_in[block.name];
        result.live_out[block.name];
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto block_it = cfg.order.rbegin();
             block_it != cfg.order.rend(); ++block_it) {
            const std::string& block = *block_it;
            std::set<ValueId> out;
            for (const std::string& succ : cfg.successors.at(block)) {
                out.insert(result.live_in[succ].begin(),
                           result.live_in[succ].end());
                const BasicBlock& successor =
                    function.blocks[cfg.index.at(succ)];
                for (const Instr& phi : successor.instructions) {
                    if (phi.opcode != InstrOpcode::Phi) break;
                    for (const auto& incoming : phi.phi_incoming) {
                        if (incoming.first == block && incoming.second >= 0)
                            out.insert(incoming.second);
                    }
                }
            }
            std::set<ValueId> in = result.use[block];
            for (ValueId value : out) {
                if (!result.def[block].count(value)) in.insert(value);
            }
            if (out != result.live_out[block] ||
                in != result.live_in[block]) {
                result.live_out[block] = std::move(out);
                result.live_in[block] = std::move(in);
                changed = true;
            }
        }
    }
    return result;
}

struct Mem2RegResult {
    int promoted_allocas = 0;
    int inserted_phis = 0;
    int removed_loads = 0;
    int removed_stores = 0;
};

// Promote only scalar stack cells whose address is used exclusively by plain
// loads and stores. Definite-assignment analysis rejects cells that could be
// read before a store, avoiding an implicit "undef" value in the public IR.
[[nodiscard]] inline Mem2RegResult promoteMemoryToSSA(Function& function) {
    Mem2RegResult result;
    ControlFlowGraph cfg = buildControlFlowGraph(function);
    if (function.blocks.empty() || !cfg.invalid_targets.empty()) return result;
    DominanceInfo dominance = computeDominance(function, cfg);

    struct Candidate {
        ValueId address = -1;
        TypeRef value_type = TypeRef::unknown();
        std::set<std::string> definition_blocks;
    };
    std::map<ValueId, Candidate> candidates;
    for (const BasicBlock& block : function.blocks) {
        for (const Instr& instr : block.instructions) {
            if (instr.opcode != InstrOpcode::Alloca || instr.def < 0) continue;
            candidates[instr.def] = Candidate{instr.def, instr.type, {}};
        }
    }
    if (candidates.empty()) return result;

    std::set<ValueId> escaped;
    for (const BasicBlock& block : function.blocks) {
        if (candidates.count(block.terminator.condition))
            escaped.insert(block.terminator.condition);
        for (const Instr& instr : block.instructions) {
            for (std::size_t index = 0; index < instr.args.size(); ++index) {
                const ValueId argument = instr.args[index];
                if (!candidates.count(argument)) continue;
                const bool plain_load =
                    instr.opcode == InstrOpcode::Load && index == 0;
                const bool plain_store =
                    instr.opcode == InstrOpcode::Store && index == 0 &&
                    instr.args.size() == 2;
                if (!plain_load && !plain_store) escaped.insert(argument);
                if (plain_load || plain_store) {
                    TypeRef type = plain_load ? instr.type :
                        TypeRef::unknown();
                    if (plain_store && instr.args.size() == 2) {
                        // The stored value's type is recovered below from its
                        // definition if the alloca did not carry it directly.
                        type = candidates[argument].value_type;
                    }
                    if (type.kind != TypeKind::Unknown)
                        candidates[argument].value_type = type;
                    if (plain_store)
                        candidates[argument].definition_blocks.insert(block.name);
                }
            }
            for (const auto& incoming : instr.phi_incoming) {
                if (candidates.count(incoming.second))
                    escaped.insert(incoming.second);
            }
        }
    }
    for (ValueId address : escaped) candidates.erase(address);

    auto scalarPromotable = [](const TypeRef& type) {
        return type.kind == TypeKind::Numeric ||
               type.kind == TypeKind::Lane ||
               type.kind == TypeKind::Trit;
    };
    for (auto it = candidates.begin(); it != candidates.end();) {
        if (!scalarPromotable(it->second.value_type) ||
            it->second.definition_blocks.empty()) {
            it = candidates.erase(it);
        } else {
            ++it;
        }
    }

    // A candidate must be initialized on every path before every load.
    const std::string& entry = cfg.order.front();
    for (auto it = candidates.begin(); it != candidates.end();) {
        const ValueId address = it->first;
        std::map<std::string, bool> assigned_in;
        std::map<std::string, bool> assigned_out;
        for (const std::string& block : cfg.order) {
            assigned_in[block] = block != entry;
            assigned_out[block] = block != entry;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const std::string& block_name : cfg.order) {
                bool in = false;
                if (block_name != entry) {
                    const auto& predecessors = cfg.predecessors.at(block_name);
                    in = !predecessors.empty();
                    for (const std::string& predecessor : predecessors)
                        in = in && assigned_out[predecessor];
                }
                bool out = in;
                const BasicBlock& block =
                    function.blocks[cfg.index.at(block_name)];
                for (const Instr& instr : block.instructions) {
                    if (instr.opcode == InstrOpcode::Store &&
                        instr.args.size() == 2 &&
                        instr.args[0] == address) {
                        out = true;
                    }
                }
                if (in != assigned_in[block_name] ||
                    out != assigned_out[block_name]) {
                    assigned_in[block_name] = in;
                    assigned_out[block_name] = out;
                    changed = true;
                }
            }
        }
        bool safe = true;
        for (const BasicBlock& block : function.blocks) {
            bool assigned = assigned_in[block.name];
            for (const Instr& instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Load &&
                    !instr.args.empty() && instr.args[0] == address &&
                    !assigned) {
                    safe = false;
                    break;
                }
                if (instr.opcode == InstrOpcode::Store &&
                    instr.args.size() == 2 && instr.args[0] == address)
                    assigned = true;
            }
            if (!safe) break;
        }
        if (!safe) it = candidates.erase(it);
        else ++it;
    }
    if (candidates.empty()) return result;

    int next_value = std::max(1, function.ir_value_ceiling);
    for (const BasicBlock& block : function.blocks) {
        for (const Instr& instr : block.instructions)
            next_value = std::max(next_value, instr.def + 1);
    }

    // aux stores the originating alloca only while mem2reg is running.
    for (auto& entry_pair : candidates) {
        Candidate& candidate = entry_pair.second;
        std::set<std::string> work = candidate.definition_blocks;
        std::set<std::string> has_phi;
        while (!work.empty()) {
            const std::string block = *work.begin();
            work.erase(work.begin());
            for (const std::string& frontier : dominance.frontier[block]) {
                if (!has_phi.insert(frontier).second) continue;
                Instr phi;
                phi.def = next_value++;
                phi.opcode = InstrOpcode::Phi;
                phi.type = candidate.value_type;
                phi.aux = candidate.address;
                BasicBlock& destination =
                    function.blocks[cfg.index.at(frontier)];
                destination.instructions.insert(
                    destination.instructions.begin(), std::move(phi));
                ++result.inserted_phis;
                if (!candidate.definition_blocks.count(frontier))
                    work.insert(frontier);
            }
        }
    }

    std::map<std::string, std::vector<std::string>> dom_children;
    for (const auto& idom : dominance.immediate_dominator) {
        if (!idom.second.empty()) dom_children[idom.second].push_back(idom.first);
    }
    std::map<ValueId, std::vector<ValueId>> current;
    std::map<ValueId, ValueId> replacements;
    std::function<void(const std::string&)> rename =
        [&](const std::string& block_name) {
            BasicBlock& block = function.blocks[cfg.index.at(block_name)];
            std::map<ValueId, int> pushed;
            std::vector<Instr> rewritten;
            rewritten.reserve(block.instructions.size());
            for (Instr instr : block.instructions) {
                if (instr.opcode == InstrOpcode::Phi &&
                    candidates.count(instr.aux)) {
                    current[instr.aux].push_back(instr.def);
                    ++pushed[instr.aux];
                    rewritten.push_back(std::move(instr));
                    continue;
                }
                if (instr.opcode == InstrOpcode::Alloca &&
                    candidates.count(instr.def)) {
                    ++result.promoted_allocas;
                    continue;
                }
                if (instr.opcode == InstrOpcode::Store &&
                    instr.args.size() == 2 &&
                    candidates.count(instr.args[0])) {
                    const ValueId address = instr.args[0];
                    current[address].push_back(instr.args[1]);
                    ++pushed[address];
                    ++result.removed_stores;
                    continue;
                }
                if (instr.opcode == InstrOpcode::Load &&
                    !instr.args.empty() &&
                    candidates.count(instr.args[0])) {
                    const ValueId address = instr.args[0];
                    if (!current[address].empty())
                        replacements[instr.def] = current[address].back();
                    ++result.removed_loads;
                    continue;
                }
                rewritten.push_back(std::move(instr));
            }
            block.instructions = std::move(rewritten);

            for (const std::string& successor : cfg.successors.at(block_name)) {
                BasicBlock& target =
                    function.blocks[cfg.index.at(successor)];
                for (Instr& phi : target.instructions) {
                    if (phi.opcode != InstrOpcode::Phi) break;
                    if (candidates.count(phi.aux) &&
                        !current[phi.aux].empty()) {
                        phi.phi_incoming.push_back(
                            {block_name, current[phi.aux].back()});
                    }
                }
            }
            for (const std::string& child : dom_children[block_name])
                rename(child);
            for (const auto& count : pushed) {
                for (int i = 0; i < count.second; ++i)
                    current[count.first].pop_back();
            }
        };
    rename(entry);

    auto resolve = [&](ValueId value) {
        std::set<ValueId> seen;
        while (replacements.count(value) && !seen.count(value)) {
            seen.insert(value);
            value = replacements[value];
        }
        return value;
    };
    for (BasicBlock& block : function.blocks) {
        for (Instr& instr : block.instructions) {
            for (ValueId& argument : instr.args) argument = resolve(argument);
            for (auto& incoming : instr.phi_incoming)
                incoming.second = resolve(incoming.second);
            if (instr.opcode == InstrOpcode::Phi) instr.aux = 0;
        }
        block.terminator.condition = resolve(block.terminator.condition);
    }
    function.ir_value_ceiling = next_value;
    return result;
}

} // namespace compiler
} // namespace sandbox

#endif // TERNARY_COMPILER_CFG_H
