#pragma once
#include <reshade.hpp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "diag_gate.h"

// The shader bytecode of every pipeline, kept for the K6 dump (offline DXIL disassembly), and the D14 SHADERDIFF:
// the shaders that drew in one 60-frame window and not in the other.
class ShaderCapture {
public:
    WWMI_DEBUG_FN void Note(uint64_t hash, const void* code, size_t code_size, const char* stage) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN size_t Count() WWMI_DEBUG_BODY(return 0;);
    WWMI_DEBUG_FN void Dump() WWMI_DEBUG_BODY();
    // f(hash, stage, bytes) for every cached shader, under the lock.
    template <class F> void ForEach(F f)
    {
        std::lock_guard lock(cache_mutex_);
        for (const auto& [hash, c] : cache_) f(hash, c.stage, c.code.size());
    }
    // A copy of one cached shader's bytecode and its stage; false when it is not cached.
    WWMI_DEBUG_FN bool Find(uint64_t hash, std::vector<uint8_t>* code, std::string* stage)
        WWMI_DEBUG_BODY(return false;);
    // Every draw while the SHADERDIFF arm is up.
    WWMI_DEBUG_FN void DiffNote(reshade::api::command_list* cmd_list, uint64_t vs_hash, uint32_t index_count)
        WWMI_DEBUG_BODY();
    // Once a frame: the arm counts down, and the window closes at zero.
    WWMI_DEBUG_FN void DiffTick() WWMI_DEBUG_BODY();

private:
    struct Cached {
        std::vector<uint8_t> code;
        char stage[4]; // "vs" / "ps" / "cs"
    };
    struct Seen {
        char stage;
        uint32_t draws;
        uint32_t max_indices;
        uint64_t partner;
    };
    void DiffAdd(uint64_t h, char stage, uint64_t partner, uint32_t index_count);
    void DiffFinish();

    std::mutex cache_mutex_;
    std::unordered_map<uint64_t, Cached> cache_;
    std::mutex diff_mutex_;
    std::unordered_map<uint64_t, Seen> diff_now_;
    std::unordered_set<uint64_t> diff_base_;
    bool diff_have_base_ = false;
};
WWMI_DEBUG_VAR ShaderCapture g_shader_capture;
