#pragma once
// A22: a mod's shape-key batch numbers, measured on DX11, answered with the game's DX12 ones while a pass runs for that
// game batch. See docs/notes/core/sk_batch_fix.md.
#include <cstdint>
#include <string>

// What the game's cb0 says of the batch its dispatch runs: the checksum WWMI's overrider takes (cb0[0] summed) and the
// vertex offset the batch starts at (cb0[65].y).
struct GameBatch {
    uint32_t checksum = 0;
    uint32_t vertex_offset = 0;
};

GameBatch GameBatchOf(const uint32_t row0[4], const uint32_t row65[4]);

// The game batch of the pass this thread runs, for the scope's life; `game` null = none known, nothing is answered.
class SkBatchPass {
public:
    SkBatchPass(const GameBatch* game, unsigned tgy, const std::wstring& ns);
    ~SkBatchPass();
    SkBatchPass(const SkBatchPass&) = delete;
    SkBatchPass& operator=(const SkBatchPass&) = delete;

    static void OnAssigned(int tag, float* var);

private:
    void Note(int batch, bool offset, float was, float now) const;

    const SkBatchPass* outer_;
    GameBatch game_;
    bool have_;
    unsigned tgy_;
    const std::wstring& ns_;
};
