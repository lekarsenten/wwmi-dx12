#pragma once
#include <cstdint>
#include <mutex>
#include <reshade.hpp>
#include "morph_dump.h" // MorphDump::GameDrawRecord

// X23: the J1 game-draw table. A game draw's entry state is read BEFORE the hook body -- the pass as the GAME set
// it, untouched by anything the port binds -- and the outcome AFTER, from the body's return value and the t_x23_*
// flags. X25: the mesh buffers CLAIMED draws bound, remembered so a draw of the same buffers through an index range
// the ini does not declare can be listed too (comp = -1, same_mesh). Only an armed J1 reaches this. A handful of
// entries: one character = one vb0 and one IB.
class GameDrawRecorder {
public:
    // False: not a row (neither a component nor a claimed character's mesh); `r` is then not to be recorded.
    bool Begin(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
        uint32_t first_index, MorphDump::GameDrawRecord* r);

private:
    struct Mesh {
        uint64_t vb0;
        uint64_t ib;
        int mod;
    };
    std::mutex mutex_;
    Mesh meshes_[16];
    int n_ = 0;
};
extern GameDrawRecorder g_game_draw_recorder;
