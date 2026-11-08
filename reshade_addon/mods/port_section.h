#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

struct CsTrig {
    std::wstring ns; // the section's namespace (the middle of fq)
    std::wstring fq; // their fq section name
    uint32_t hash; // the section's own `hash =`, their G->mTextureOverrideMap key
};

inline const unsigned kDx11SkeletonCbByteWidth = 16384;
inline const unsigned kDx11ObjectCbByteWidth = 1024;

struct PortHooks {
    std::function<void()> handling_skip;
    std::function<void(const std::wstring&, const std::wstring&)> cs_enter;
    std::function<void(const std::wstring&, const std::wstring&)> resource_ref;
    std::function<void(const std::wstring&, const std::wstring&)> run_cmd;
    std::function<void(uint32_t, uint32_t, uint32_t)> drawindexed;
    // Filled by PortRunSection before each drawindexed: DrawInfo::srv_gen_at_entry of this section run.
    uint32_t srv_gen0 = 0;
    // Per ps-tN: the write generation a texture section's `this =` left there; 0 = none wrote it.
    std::vector<uint32_t> this_gen;
};

// Filled from the presence tick, drained at the dispatch. Guarded by g_cs_trigs_mtx.
extern std::vector<CsTrig> g_cs_trigs;
extern std::mutex g_cs_trigs_mtx;
extern std::shared_ptr<const std::vector<CsTrig>> g_cs_trigs_snap;

bool SectionReadsPipelineState(const wchar_t* fq);
std::wstring PortComponentSection(int mod_id, int comp_index, const std::wstring& tail, const std::wstring& ns);
bool PortRunSection(const std::wstring& fq_section, PortHooks& hk, uint32_t index_count, uint32_t first_index,
    uint32_t first_instance, uint64_t pso, bool& skip_orig);
bool LogBudget(const char* tag, const std::wstring& ns, int limit);

inline thread_local bool t_trace_binds = false; // PortRunSection logs every command past its session budget
