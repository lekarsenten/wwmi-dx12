#pragma once
#include "tex_look.h"
#include <d3d12.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// The look-alike search off the upload thread: a texture's patches cut as it uploads, decoded and compared on this
// worker, which also writes the caches back when the game is quiet.
class TexLookWorker {
public:
    void Queue(ID3D12Resource* dest, uint32_t crc, uint32_t w, uint32_t h, uint32_t fmt, bool is_target,
        const std::vector<uint8_t>& bc);
    void Stop();
    ~TexLookWorker()
    {
        if (thread_.joinable()) thread_.detach();
    }

private:
    struct Job {
        ID3D12Resource* dest;
        uint32_t crc;
        bool is_target;
        TexLookCut cut;
    };
    void Run();
    std::deque<Job> queue_; // under mtx_
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false; // under mtx_
    std::thread thread_; // started and joined under mtx_
};
extern TexLookWorker g_tex_look_worker;
