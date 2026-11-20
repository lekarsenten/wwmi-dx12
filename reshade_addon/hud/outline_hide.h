// outline_hide.h -- OURS. A6: the game's character outline pass laid on the mesh (docs/notes/core/outline_hide.md).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <reshade.hpp>

class OutlineHide {
public:
    // A pipeline that may draw a character's outline: its input layout carries bone indices and weights, and its
    // pixel shader has the outline pass's interface. Cheap: the creation hook asks it of every pipeline.
    static bool Candidate(
        const reshade::api::input_element* layout, uint32_t layout_count, const void* ps, size_t ps_size);
    // Reads a candidate's pixel shader off the creation thread; PipelineRegistry::SetOutline marks the pipeline when
    // the shader stores the outline's G-buffer constants. Once per pixel shader; later pipelines take the verdict.
    // With A6 off or the add-on disabled nothing is read: the candidate is noted, and ReadNoted reads it.
    void Queue(uint64_t pso, uint64_t ps_hash, const void* ps, size_t ps_size);
    // While A6 is on: reads the noted candidates, their shaders taken from the pipeline clones.
    void ReadNoted()
    {
        if (noted_any_.load(std::memory_order_acquire)) QueueNoted();
    }
    // Builds, off the drawing thread, the pipeline bound in a marked pipeline's place: its clone with the vertex
    // shader's outline offset left out (RfxDxilZeroOffset), registered under the source's shader hashes. It settles
    // into PipelineRegistry::SetOutlineStandIn.
    void QueueStandIn(reshade::api::device* dev, uint64_t pso);
    // Ends the worker and waits for it; nothing is queued after. The device is going, or the add-on.
    void Stop();
    // The K7 `outline` answer: pixel shaders read, vertex shaders patched or not and why, stand-ins.
    std::string Report();

#ifdef WWMI_DEBUG
    std::atomic<uint64_t> bound_stand_in{ 0 }; // binds of a marked pipeline that set its stand-in
    std::atomic<uint64_t> bound_source{ 0 }; // binds of a marked pipeline that kept it (no stand-in yet, or none)
    std::atomic<uint64_t> dropped{ 0 }; // game draws HideOutlineDraw dropped
#endif

private:
    struct Job {
        uint64_t pso = 0;
        uint64_t ps_hash = 0;
        std::vector<uint8_t> code; // empty: a noted candidate, read from its clone
        reshade::api::device* dev = nullptr; // set: a stand-in build
    };
    struct Noted {
        uint64_t pso = 0;
        uint64_t ps_hash = 0;
    };
    struct Patched {
        std::vector<uint8_t> code; // empty: the vertex shader could not be patched
        std::string why;
    };
    struct Thread {
        std::thread t;
        ~Thread()
        {
            if (t.joinable()) t.detach();
        }
    };
    void StartLocked();
    void QueueNoted();
    void Run();
    void BuildStandIn(const Job& job);

    std::atomic<bool> noted_any_{ false };
    std::mutex mutex_; // everything below
    std::condition_variable cv_;
    bool stop_ = false;
    std::deque<Job> queue_;
    std::vector<Noted> noted_; // candidates created with A6 off or the add-on disabled, not read yet
    std::unordered_map<uint64_t, bool> verdicts_; // pixel shader hash -> stores the outline's constants
    std::unordered_map<uint64_t, std::shared_ptr<const Patched>> patched_; // vertex shader hash -> its patch
    std::unordered_set<uint64_t> pending_; // sources whose stand-in is queued or in flight
    std::unordered_map<uint64_t, uint64_t> stand_ins_; // source -> stand-in
    unsigned stand_ins_failed_ = 0;
    std::unordered_map<int, unsigned> depth_funcs_; // the stand-ins' depth tests (compare_op, -1 = off) -> count
    Thread worker_;
};

inline OutlineHide g_outline_hide;
