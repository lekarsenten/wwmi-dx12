#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct FrameSync {
    std::atomic<const void*> claim_last_list{ nullptr };
    std::atomic<const void*> claim_exec_queue{ nullptr };
    std::atomic<uint32_t> claim_thread{ 0 };
    std::atomic<uint64_t> claim_exec_seen{ 0 };
    std::atomic<bool> game_frame_submitted{ false }; // a list with a game draw went to the GPU since the last step
    std::atomic<uint32_t> game_submit_tid{ 0 }; // the thread that submits the game's drawing lists
    std::atomic<uint64_t> game_submit_queue{ 0 }; // ...and the queue they go to (native)
    std::atomic<uint32_t> presenter_tid{ 0 };
    std::atomic<void*> present_queue{ nullptr }; // native: the redraw's lists run there
    std::atomic<bool> frame_end_armed{
        false
    }; // presents come from another thread: the game's frame end is watched for
    std::atomic<uint32_t> bb_width{ 0 };
    std::atomic<uint32_t> bb_height{ 0 };
    std::atomic<uint64_t> frame_end_step_ms{ 0 }; // when the step last ran at the game's frame end
    std::atomic<bool> frame_end_stepped{ false }; // ...and that it did since the last present
};
extern FrameSync g_frames;
