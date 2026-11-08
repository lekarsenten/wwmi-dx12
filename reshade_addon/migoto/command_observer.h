#pragma once
// Every parsed command of their lists runs wrapped: the add-on's observers hear of it, a trace collects the leaves
// a section ran, a selection input it writes bumps the generation, and its cost is tallied by kind.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CommandList;
class CommandListCommand;
class CommandListState;
class CustomResource;

namespace MigotoPort {

typedef void (*CommandObserverFn)(const wchar_t* ini_line, void* user);

struct ObservedCmd {
    enum class Kind { Other, RunList, ResourceCopy };
    Kind kind = Kind::Other;
    bool post = false;
    const wchar_t* run_target = nullptr; // linked list: ini_section, directly available
    const void* run_explicit = nullptr; // explicit list: ExplicitCommandListSection*, name resolved upstream
    int dst_type = 0;
    wchar_t dst_shader = 0;
    unsigned dst_slot = 0;
    const void* dst_custom = nullptr;
    int src_type = 0;
    const void* src_custom = nullptr;
};

typedef void (*TypedCommandObserverFn)(const ObservedCmd& cmd, void* user);

class CommandObserver {
public:
    struct CmdCost {
        std::atomic<uint64_t> calls{ 0 };
        std::atomic<uint64_t> incl{ 0 };
        std::atomic<uint64_t> self{ 0 };
    };

    void SetObserver(CommandObserverFn fn, void* user);
    void SetTypedObserver(TypedCommandObserverFn fn, void* user);
    void Observe(const wchar_t* ini_line);
    // The ini line of the command this thread is running, or null between commands (COPY-WHAT names its copy).
    const wchar_t* CurrentIniLine() const;
    bool Active() const;
    bool Any() const;

    std::vector<CommandListCommand*>* SetTrace(std::vector<CommandListCommand*>* trace);
    // `run = CommandList...`: the hook runs the target list itself and returns true, or returns false to leave it to theirs.
    void SetRunExplicitHook(bool (*fn)(CommandListCommand* inner, ::CommandListState* state));
    bool RunExplicit(CommandListCommand* inner, ::CommandListState* state);

    void Wrap(CommandList* list);
    void WrapAll(std::vector<CommandList*>& lists);
    CommandListCommand* Unwrap(CommandListCommand* cmd);

    // This thread's section run bound a slot to a texture still loading in the background. Cleared as a run starts.
    void ClearBoundLoading();
    bool BoundLoading() const;
    // ... and the texture its last `this =` assigned while that texture was still loading, else null.
    void ClearThisLoading();
    CustomResource* ThisLoading() const;

    CmdCost* CostKind(CommandListCommand* c);
    void CostEnd(CmdCost& c, int64_t t0, uint64_t saved);
    void CostMark();
    void CostReport(FILE* f, int frames);

private:
    bool (*run_explicit_hook_)(CommandListCommand*, ::CommandListState*) = nullptr;
    std::atomic<size_t> wrapped_quiet_{ 0 };
    std::mutex cost_mutex_;
    std::deque<CmdCost> costs_;
    std::vector<std::string> cost_names_;
    std::vector<uint64_t> cost_base_;
};
// Order-safe for static initializers in other files; g_command_observer is the same object for everyone else.
CommandObserver& CommandObserverInstance();
extern CommandObserver& g_command_observer;

// Their IniHandler (port_ini.cpp) and Override (port_override.cpp) wrap the lists they build through these.
void WrapObservedCommands(std::vector<CommandList*>& lists);
void WrapObservedList(CommandList* list);

} // namespace MigotoPort
