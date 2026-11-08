#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

struct TexRules {
    std::unordered_map<uint32_t, int> hash_to_rule; // tex hash -> occurrence index of its [TextureOverride*] that assigns this
    std::set<uint32_t> targets; // vanilla hashes the mods replace (sections and t=<hash> filenames)
};

TexRules BuildTexRules();

class TexRuleSet {
public:
    struct PerSet;
    bool Ready() const { return ready_.load(std::memory_order_acquire); } // a parse's rules are in the mod set
    const TexRules& Now() const;
    bool IsTarget(uint32_t hash) const { return Now().hash_to_rule.count(hash) != 0; }
    void NoteSwap(uint32_t rule); // TexPlacement::Swap increments; a stamp is rare, ~3 a frame
    std::unordered_set<uint32_t> RulesOfNs(const wchar_t* ns) const;
    void TargetAudit();

private:
    std::atomic<bool> ready_{ false };
    std::mutex swap_mutex_;
    std::unordered_map<uint32_t, uint32_t> swap_by_hash_; // stamps per rule hash
};
extern TexRuleSet g_tex_rules;
