#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct PresentLogSubmitter {
    uint32_t tid = 0;
    uint64_t queue = 0;
    uint32_t lists = 0;
    uint32_t claimed = 0;
    int64_t last_qpc = 0;
};

// B23: the next N presents logged with the game's draws and submissions in between (PRESENTLOG, FRAMEEND).
struct PresentLog {
    std::atomic<int> left{ 0 }; // presents still to log
    std::atomic<uint32_t> draws{ 0 };
    std::atomic<uint32_t> execs{ 0 };
    std::atomic<uint32_t> frame_end_draws{ 0 }; // game draws since the last FRAMEEND
    std::atomic<int64_t> qpc{ 0 };
    std::mutex mtx;
    std::vector<PresentLogSubmitter> subs; // who submitted since the last present
    std::vector<uint64_t> bbs; // the swapchain's back buffers
};
extern PresentLog g_present_log;
