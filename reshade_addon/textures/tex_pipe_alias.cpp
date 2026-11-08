// tex_pipe_alias.cpp -- see tex_pipe_alias.h.
#include "tex_pipe_alias.h"
#include "tex_identity.h"
#include "tex_place.h"
#include "tex_rules.h"
#include "../hooks/stamp_ledger.h"
#include "../core/log.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>

TexPipeAlias g_tex_pipe_alias;

uint32_t TexPipeAlias::Of(uint32_t hash)
{
    if (!any_.load(std::memory_order_relaxed)) return 0;
    std::shared_lock l(mutex_);
    auto it = alias_.find(hash);
    return it != alias_.end() ? it->second : 0;
}

unsigned TexPipeAlias::Restamp(uint32_t hash, bool on)
{
    std::vector<uint64_t> carriers;
    g_tex_identity.ForEachHash([&](uint64_t res, uint32_t h) {
        if (h == hash) carriers.push_back(res);
    });
    unsigned swapped = 0;
    for (uint64_t r : carriers) {
        if (!on) g_stamps.Unstamp(r, false);
        swapped += g_stamps.Restamp(r);
    }
    return swapped;
}

std::string TexPipeAlias::Command(const std::vector<std::string>& args)
{
    if (args.size() == 2) {
        char* end = nullptr;
        const uint32_t game = (uint32_t)std::strtoul(args[0].c_str(), &end, 16);
        if (!end || *end || !game) return "ERR not a texture hash: " + args[0];
        if (args[1] == "off") {
            bool had;
            {
                std::unique_lock l(mutex_);
                had = alias_.erase(game) != 0;
                any_.store(!alias_.empty());
            }
            if (!had) return "ERR no texture alias for " + args[0];
            const unsigned swapped = Restamp(game, false);
            logger::write("PIPE: texalias %08x off (%u slot(s) swap under the texture's own rule)", game, swapped);
            return "removed " + args[0];
        }
        const uint32_t rule = (uint32_t)std::strtoul(args[1].c_str(), &end, 16);
        if (!end || *end || !rule) return "ERR not a texture hash: " + args[1];
        if (!g_tex_rules.Now().hash_to_rule.count(rule)) return "ERR no texture rule has hash " + args[1];
        {
            std::unique_lock l(mutex_);
            alias_[game] = rule;
            any_.store(true);
        }
        const unsigned swapped = Restamp(game, true);
        logger::write("PIPE: texalias game %08x -> rule %08x (%u live slot(s) swapped)", game, rule, swapped);
        g_tex_place.RetryMissed("texalias");
        return args[0] + " -> " + args[1] + " (" + std::to_string(swapped) + " live slot(s) swapped)";
    }
    if (!args.empty()) return "ERR texalias, texalias <game hash> <rule hash> or texalias <game hash> off";
    std::string out;
    char b[64];
    std::shared_lock l(mutex_);
    for (const auto& [game, rule] : alias_) {
        std::snprintf(b, sizeof b, "%08x -> %08x\n", game, rule);
        out += b;
    }
    return out.empty() ? "no texture alias" : out;
}
