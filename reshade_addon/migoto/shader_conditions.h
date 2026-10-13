#pragma once
#include "CommandList.h"
#include "port_keygate.h"
#include <unordered_set>
#include <vector>

namespace MigotoPort {

// Use the engine's parsed operands instead of maintaining another INI lexer.
inline bool ExpressionReadsShader(const CommandListEvaluatable* expression)
{
    std::vector<const CommandListEvaluatable*> pending{ expression };
    while (!pending.empty()) {
        const auto* node = pending.back();
        pending.pop_back();
        if (const auto* operand = dynamic_cast<const CommandListOperand*>(node)) {
            if (operand->type == ParamOverrideType::SHADER) return true;
        } else if (const auto* op = dynamic_cast<const CommandListOperator*>(node)) {
            if (op->lhs) pending.push_back(op->lhs.get());
            if (op->rhs) pending.push_back(op->rhs.get());
        }
    }
    return false;
}

inline bool ListsReadShaderConditions(const CommandList* pre, const CommandList* post)
{
    std::vector<const CommandList*> pending{ pre, post };
    std::unordered_set<const CommandList*> visited;
    while (!pending.empty()) {
        const auto* list = pending.back();
        pending.pop_back();
        if (!list || !visited.insert(list).second) continue;
        for (const auto& wrapped : list->commands) {
            const auto* command = UnwrapObserved(wrapped.get());
            if (const auto* condition = dynamic_cast<const IfCommand*>(command)) {
                if (ExpressionReadsShader(condition->expression.evaluatable.get())) return true;
                pending.push_back(condition->true_commands_pre.get());
                pending.push_back(condition->true_commands_post.get());
                pending.push_back(condition->false_commands_pre.get());
                pending.push_back(condition->false_commands_post.get());
            } else if (const auto* run = dynamic_cast<const RunExplicitCommandList*>(command)) {
                if (run->command_list_section) {
                    pending.push_back(&run->command_list_section->command_list);
                    pending.push_back(&run->command_list_section->post_command_list);
                }
            } else if (const auto* link = dynamic_cast<const RunLinkedCommandList*>(command)) {
                pending.push_back(link->link);
            }
        }
    }
    return false;
}

} // namespace MigotoPort
