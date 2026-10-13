#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "profiler.h"
#include "gpu_timer.h"
#include "../migoto/port_api.h"
#include "../migoto/resource_memory.h"

struct PerfMemory {
    MigotoPort::ResourceMemorySnapshot addon;
    uint64_t vram_used = 0;
    uint64_t vram_budget = 0;
    uint64_t shared_used = 0;
    uint64_t shared_budget = 0;
    uint64_t game_private = 0;
    uint64_t game_working_set = 0;
    void     Max(const PerfMemory& o);
};

struct PerfStep {
    double incl_ms = 0.0;
    double self_ms = 0.0;
    double max_ms = 0.0;
    double calls = 0.0;
};

struct PerfThread {
    uint32_t tid = 0;
    double   self_ms = 0.0;
    double   scopes = 0.0;
    double   step_ms[PROF_COUNT] = {};
};

struct PerfFrame {
    double     frame_ms = 0.0;
    PerfStep   steps[PROF_COUNT];
    PerfThread threads[kProfThreads];
    int        n_threads = 0;
    double     gpu_ns[GT_COUNT] = {};
    double     gpu_n[GT_COUNT] = {};
    uint32_t   gpu_frames = 0;
};

class PerfSpan {
public:
    void Reset();
    void Add(const PerfFrame& f);

    int        frames = 0;
    double     frame_ms = 0.0;
    double     slowest_ms = 0.0;
    double     fastest_ms = 0.0;
    PerfStep   steps[PROF_COUNT];
    PerfThread threads[kProfThreads];
    int        n_threads = 0;
    double     gpu_ns[GT_COUNT] = {};
    double     gpu_n[GT_COUNT] = {};
    uint32_t   gpu_frames = 0;
};

struct PerfGpuArea {
    GpuTimerBucket bucket;
    const char*    name;
};
inline constexpr PerfGpuArea kPerfGpuAreas[] = {
    { GT_ClaimedDraw, "Drawing modded characters, with their shape keys" },
    { GT_Present, "Mod scripts once a frame" },
    { GT_DispatchHook, "Shape keys at the game's own compute" },
};
inline constexpr int kPerfGpuAreaCount = (int)(sizeof(kPerfGpuAreas) / sizeof(kPerfGpuAreas[0]));

struct PerfFigures {
    int      frames = 0;
    double   seconds = 0.0;
    double   fps = 0.0;
    double   frame_ms = 0.0;
    double   slowest_fps = 0.0;
    double   fastest_fps = 0.0;
    double   cpu_busiest_ms = 0.0;
    uint32_t cpu_busiest_tid = 0;
    double   cpu_all_ms = 0.0;
    int      cpu_threads = 0;
    double   timers_ms = 0.0;
    double   area_ms[PA_COUNT] = {};
    bool     gpu_read = false;
    double   gpu_ms = 0.0;
    double   gpu_area_ms[kPerfGpuAreaCount] = {};
    double   gpu_step_ms[GT_COUNT] = {};
    double   gpu_step_n[GT_COUNT] = {};
    PerfStep steps[PROF_COUNT];

    static PerfFigures Of(const PerfSpan& s, double seconds);
};

double PerfFpsGain(double frame_ms, double cost_ms);

struct PerfReportRun {
    bool                   active = false;
    int64_t                start = 0;
    PerfSpan               span;
    std::vector<double>    frame_ms;
    PortApi::GpuWorkTotals gw0;
    uint64_t               gw_n0[PGW_COUNT] = {};
    uint64_t               gw_bytes0[PGW_COUNT] = {};
    uint64_t               th_all0 = 0;
    uint64_t               th_rehash0 = 0;
    uint64_t               th_same0 = 0;
    uint64_t               th_bytes0 = 0;
    uint32_t               gt_dropped0 = 0;
    uint32_t               gt_untimed0 = 0;
    PerfMemory             mem_start;
    MigotoPort::ResourceMemorySnapshot addon_peak;
    bool                   have_mem_start = false;
    bool                   debug_was = false;
};

class PerfMonitor {
public:
    static constexpr double kReportSeconds = 10.0;

    void FrameEnd();
    void RequestReport();
    bool ReportRunning() const;
    double ReportSecondsLeft() const;
    const std::string& LastReport() const;
    bool HaveShown() const;
    const PerfFigures& Shown() const;
    bool HaveMemory() const;
    const PerfMemory& MemoryNow() const;
    const PerfMemory& MemoryPeak() const;

private:
    void Restart(int64_t now);
    void Drain(double frame_ms);
    void SampleMemory(int64_t now, int64_t freq);
    void StartReport(int64_t now);
    void EndReport();
    void FinishReport(int64_t now, int64_t freq);
    void WriteReport(FILE* f, double seconds) const;

    PerfFrame         frame_;
    PerfSpan          second_;
    int64_t           second_start_ = 0;
    PerfFigures       shown_;
    bool              have_shown_ = false;
    bool              was_enabled_ = false;
    int64_t           last_frame_ = 0;
    PerfMemory        mem_;
    PerfMemory        mem_peak_;
    bool              have_mem_ = false;
    int64_t           mem_last_ = 0;
    std::atomic<bool> report_request_{ false };
    PerfReportRun     report_;
    double            report_left_ = 0.0;
    std::string       last_report_;
};
extern PerfMonitor g_perf;
