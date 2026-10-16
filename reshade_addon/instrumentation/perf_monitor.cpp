#include "perf_monitor.h"
#include <algorithm>
#include <dxgi1_4.h>
#include <psapi.h>
#include <windows.h>
#include "diag.h"
#include "../addon_state.h"
#include "../build_stamp.h"
#include "../config.h"
#include "../migoto/port_toggles.h"
#include "../../src/mod_loader.h"

PerfMonitor g_perf;

namespace {

constexpr double kMB = 1024.0 * 1024.0;
constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

const char* OnOff(bool v)
{
    return v ? "on" : "off";
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int   n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring LogDir()
{
    const size_t slash = g_addon_ini_path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : g_addon_ini_path.substr(0, slash + 1);
}

const char* StepName(int i)
{
    const char* nm = kProfSteps[i].name;
    while (*nm == ' ') ++nm;
    return nm;
}

double StepNet(const PerfStep& s)
{
    const double net = s.self_ms - s.calls * g_prof_percall_ns / 1e6;
    return net > 0.0 ? net : 0.0;
}

double ThreadNet(const PerfThread& t)
{
    const double net = t.self_ms - t.scopes * g_prof_percall_ns / 1e6;
    return net > 0.0 ? net : 0.0;
}

} // namespace

void PerfMemory::Max(const PerfMemory& o)
{
    addon.Max(o.addon);
    vram_used = (std::max)(vram_used, o.vram_used);
    vram_budget = (std::max)(vram_budget, o.vram_budget);
    shared_used = (std::max)(shared_used, o.shared_used);
    shared_budget = (std::max)(shared_budget, o.shared_budget);
    game_private = (std::max)(game_private, o.game_private);
    game_working_set = (std::max)(game_working_set, o.game_working_set);
}

void PerfSpan::Reset()
{
    *this = PerfSpan{};
}

void PerfSpan::Add(const PerfFrame& f)
{
    ++frames;
    const double inv = 1.0 / (double)frames;
    frame_ms += (f.frame_ms - frame_ms) * inv;
    if (frames == 1 || f.frame_ms > slowest_ms) slowest_ms = f.frame_ms;
    if (frames == 1 || f.frame_ms < fastest_ms) fastest_ms = f.frame_ms;
    for (int i = 0; i < PROF_COUNT; ++i) {
        PerfStep&       s = steps[i];
        const PerfStep& x = f.steps[i];
        s.incl_ms += (x.incl_ms - s.incl_ms) * inv;
        s.self_ms += (x.self_ms - s.self_ms) * inv;
        s.calls += (x.calls - s.calls) * inv;
        if (x.incl_ms > s.max_ms) s.max_ms = x.incl_ms;
    }
    if (f.n_threads > n_threads) n_threads = f.n_threads;
    for (int t = 0; t < n_threads; ++t) {
        PerfThread&       m = threads[t];
        const PerfThread& x = f.threads[t];
        const bool        here = t < f.n_threads;
        if (here) m.tid = x.tid;
        m.self_ms += ((here ? x.self_ms : 0.0) - m.self_ms) * inv;
        m.scopes += ((here ? x.scopes : 0.0) - m.scopes) * inv;
        for (int i = 0; i < PROF_COUNT; ++i)
            m.step_ms[i] += ((here ? x.step_ms[i] : 0.0) - m.step_ms[i]) * inv;
    }
    for (int k = 0; k < GT_COUNT; ++k) {
        gpu_ns[k] += f.gpu_ns[k];
        gpu_n[k] += f.gpu_n[k];
    }
    gpu_frames += f.gpu_frames;
}

PerfFigures PerfFigures::Of(const PerfSpan& s, double seconds)
{
    PerfFigures r;
    r.frames = s.frames;
    r.seconds = seconds;
    r.fps = seconds > 0.0 ? (double)s.frames / seconds : 0.0;
    r.frame_ms = s.frame_ms;
    r.slowest_fps = s.slowest_ms > 0.0 ? 1000.0 / s.slowest_ms : 0.0;
    r.fastest_fps = s.fastest_ms > 0.0 ? 1000.0 / s.fastest_ms : 0.0;
    for (int t = 0; t < s.n_threads; ++t) {
        const PerfThread& th = s.threads[t];
        const double      net = ThreadNet(th);
        r.timers_ms += th.scopes * g_prof_percall_ns / 1e6;
        r.cpu_all_ms += net;
        if (th.self_ms > 0.001) ++r.cpu_threads;
        if (net > r.cpu_busiest_ms) {
            r.cpu_busiest_ms = net;
            r.cpu_busiest_tid = th.tid;
        }
    }
    for (int i = 0; i < PROF_COUNT; ++i) {
        r.steps[i] = s.steps[i];
        r.area_ms[kProfSteps[i].area] += StepNet(s.steps[i]);
    }
    r.gpu_read = s.gpu_frames > 0;
    if (r.gpu_read) {
        for (int k = 0; k < GT_COUNT; ++k) {
            r.gpu_step_ms[k] = s.gpu_ns[k] / 1e6 / (double)s.gpu_frames;
            r.gpu_step_n[k] = s.gpu_n[k] / (double)s.gpu_frames;
        }
        for (int a = 0; a < kPerfGpuAreaCount; ++a) {
            r.gpu_area_ms[a] = r.gpu_step_ms[kPerfGpuAreas[a].bucket];
            r.gpu_ms += r.gpu_area_ms[a];
        }
    }
    return r;
}

double PerfFpsGain(double frame_ms, double cost_ms)
{
    if (frame_ms <= 0.0 || cost_ms <= 0.0) return 0.0;
    const double rest = (std::max)(frame_ms - cost_ms, frame_ms * 0.05);
    return 1000.0 / rest - 1000.0 / frame_ms;
}

void PerfMonitor::RequestReport()
{
    report_request_.store(true, std::memory_order_relaxed);
}

bool PerfMonitor::ReportRunning() const
{
    return report_.active;
}

double PerfMonitor::ReportSecondsLeft() const
{
    return report_left_;
}

const std::string& PerfMonitor::LastReport() const
{
    return last_report_;
}

bool PerfMonitor::HaveShown() const
{
    return have_shown_;
}

const PerfFigures& PerfMonitor::Shown() const
{
    return shown_;
}

bool PerfMonitor::HaveMemory() const
{
    return have_mem_;
}

const PerfMemory& PerfMonitor::MemoryNow() const
{
    return mem_;
}

const PerfMemory& PerfMonitor::MemoryPeak() const
{
    return mem_peak_;
}

void PerfMonitor::Restart(int64_t now)
{
    Drain(0.0);
    second_.Reset();
    second_start_ = now;
    have_shown_ = false;
    have_mem_ = false;
    mem_last_ = 0;
}

void PerfMonitor::FrameEnd()
{
    const int64_t freq = prof_qpc_freq();
    const int64_t now = prof_qpc_now();
    const double  frame_ms = last_frame_ ? (double)(now - last_frame_) * 1000.0 / (double)freq : 0.0;
    last_frame_ = now;
    if (g_prof_enabled != was_enabled_) {
        was_enabled_ = g_prof_enabled;
        if (g_prof_enabled) {
            ProfCalibrate();
            Restart(now);
            return;
        }
        if (report_.active) EndReport();
    }
    if (!g_prof_enabled) {
        report_request_.store(false, std::memory_order_relaxed);
        return;
    }

    Drain(frame_ms);
    second_.Add(frame_);
    SampleMemory(now, freq);
    if (now - second_start_ >= freq) {
        shown_ = PerfFigures::Of(second_, (double)(now - second_start_) / (double)freq);
        have_shown_ = true;
        second_.Reset();
        second_start_ = now;
    }

    if (report_request_.exchange(false, std::memory_order_relaxed) && !report_.active) StartReport(now);
    if (report_.active) {
        report_.span.Add(frame_);
        report_.frame_ms.push_back(frame_ms);
        const double elapsed = (double)(now - report_.start) / (double)freq;
        report_left_ = kReportSeconds - elapsed;
        if (elapsed >= kReportSeconds) FinishReport(now, freq);
    }
}

void PerfMonitor::Drain(double frame_ms)
{
    PerfFrame& f = frame_;
    f.frame_ms = frame_ms;
    for (PerfStep& s : f.steps) s = PerfStep{};
    const int n_thr = g_prof_thread_n.load(std::memory_order_relaxed);
    f.n_threads = n_thr;
    for (int t = 0; t < n_thr; ++t) {
        PerfThread& th = f.threads[t];
        th.tid = g_prof_tid[t].load(std::memory_order_relaxed);
        th.self_ms = 0.0;
        th.scopes = 0.0;
        for (int i = 0; i < PROF_COUNT; ++i) {
            const double incl = (double)g_prof_ns_t[t][i].exchange(0, std::memory_order_relaxed) / 1e6;
            const double self = (double)g_prof_self_ns_t[t][i].exchange(0, std::memory_order_relaxed) / 1e6;
            const double calls = (double)g_prof_calls_t[t][i].exchange(0, std::memory_order_relaxed);
            th.step_ms[i] = self;
            th.self_ms += self;
            th.scopes += calls;
            f.steps[i].incl_ms += incl;
            f.steps[i].self_ms += self;
            f.steps[i].calls += calls;
        }
    }
    for (PerfStep& s : f.steps) s.max_ms = s.incl_ms;
    for (int k = 0; k < GT_COUNT; ++k) {
        f.gpu_ns[k] = (double)g_gt_ns[k].exchange(0, std::memory_order_relaxed);
        f.gpu_n[k] = (double)g_gt_n[k].exchange(0, std::memory_order_relaxed);
    }
    f.gpu_frames = g_gt_frames_read.exchange(0, std::memory_order_relaxed);
}

void PerfMonitor::SampleMemory(int64_t now, int64_t freq)
{
    if (mem_last_ && now - mem_last_ < freq) return;
    mem_last_ = now;

    static IDXGIAdapter3* s_adapter = nullptr;
    static ID3D12Device*  s_adapter_for = nullptr;
    if (g_d3d12_device && s_adapter_for != g_d3d12_device) {
        if (s_adapter) {
            s_adapter->Release();
            s_adapter = nullptr;
        }
        s_adapter_for = g_d3d12_device;
        IDXGIFactory4* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter1* a1 = nullptr;
            if (SUCCEEDED(fac->EnumAdapterByLuid(g_d3d12_device->GetAdapterLuid(), IID_PPV_ARGS(&a1))) && a1) {
                a1->QueryInterface(IID_PPV_ARGS(&s_adapter));
                a1->Release();
            }
            fac->Release();
        }
    }
    PerfMemory m;
    if (s_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
        if (SUCCEEDED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) {
            m.vram_budget = vi.Budget;
            m.vram_used = vi.CurrentUsage;
        }
        if (SUCCEEDED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vi))) {
            m.shared_budget = vi.Budget;
            m.shared_used = vi.CurrentUsage;
        }
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.game_private = pmc.PrivateUsage;
        m.game_working_set = pmc.WorkingSetSize;
    }
    m.addon = MigotoPort::ResourceMemory().Snapshot();
    if (report_.active) report_.addon_peak.Max(m.addon);
    mem_ = m;
    if (have_mem_)
        mem_peak_.Max(m);
    else
        mem_peak_ = m;
    have_mem_ = true;
}

void PerfMonitor::StartReport(int64_t now)
{
    mem_last_ = 0;
    SampleMemory(now, prof_qpc_freq());
    PerfReportRun& r = report_;
    r.active = true;
    r.start = now;
    r.span.Reset();
    r.frame_ms.clear();
    PortApi::GetGpuWorkTotals(&r.gw0);
    for (int k = 0; k < PGW_COUNT; ++k) {
        r.gw_n0[k] = g_gw_n[k].load(std::memory_order_relaxed);
        r.gw_bytes0[k] = g_gw_bytes[k].load(std::memory_order_relaxed);
    }
    r.th_all0 = g_th_all.load(std::memory_order_relaxed);
    r.th_rehash0 = g_th_rehash.load(std::memory_order_relaxed);
    r.th_same0 = g_th_same.load(std::memory_order_relaxed);
    r.th_bytes0 = g_th_bytes.load(std::memory_order_relaxed);
    r.gt_dropped0 = g_gt_dropped.load(std::memory_order_relaxed);
    r.gt_untimed0 = g_gt_untimed.load(std::memory_order_relaxed);
    PortApi::CommandCostMark();
    r.mem_start = mem_;
    r.addon_peak = mem_.addon;
    r.have_mem_start = have_mem_;
    r.debug_was = g_debug;
    if (g_debug) SetDiagnostics(false, g_log_verbosity);
    report_left_ = kReportSeconds;
}

void PerfMonitor::EndReport()
{
    report_.active = false;
    report_left_ = 0.0;
    if (report_.debug_was) SetDiagnostics(true, g_log_verbosity);
}

void PerfMonitor::FinishReport(int64_t now, int64_t freq)
{
    mem_last_ = 0;
    SampleMemory(now, freq);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"wwmi_dx12_perf_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
        st.wMinute, st.wSecond);
    const std::wstring path = LogDir() + name;
    FILE*              f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
        WriteReport(f, (double)(now - report_.start) / (double)freq);
        std::fclose(f);
        last_report_ = Utf8(path);
    } else {
        last_report_ = "could not write " + Utf8(path);
    }
    EndReport();
}

void PerfMonitor::WriteReport(FILE* f, double seconds) const
{
    const PerfReportRun& r = report_;
    const PerfFigures    s = PerfFigures::Of(r.span, seconds);
    SYSTEMTIME           st;
    GetLocalTime(&st);
    std::fprintf(f, "WWMI-DX12 performance report\n");
    std::fprintf(f, "Add-on %s (build %s), %04d-%02d-%02d %02d:%02d:%02d\n", WWMI_VERSION, WWMI_BUILD_SHA, st.wYear,
        st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::fprintf(f, "Measured for %.1f s: %d frames.%s\n\n", seconds, s.frames,
        r.debug_was ? " The debug log was off while measuring and is back on." : "");

    std::vector<double> slow = r.frame_ms;
    std::sort(slow.begin(), slow.end(), [](double a, double b) { return a > b; });
    const size_t n1 = (std::max<size_t>)(1, slow.size() / 100);
    double       low_ms = 0.0;
    for (size_t i = 0; i < n1 && i < slow.size(); ++i) low_ms += slow[i];
    low_ms = slow.empty() ? 0.0 : low_ms / (double)(std::min)(n1, slow.size());
    static const double kBandFps[] = { 100.0, 75.0, 50.0, 30.0 };
    static const char*  kBandName[] = { "over 100 FPS", "75 to 100", "50 to 75", "30 to 50", "under 30" };
    double              band_ms[5] = {};
    double              total_ms = 0.0;
    for (double ms : r.frame_ms) {
        const double fps = ms > 0.0 ? 1000.0 / ms : 0.0;
        int          b = 4;
        for (int k = 0; k < 4; ++k)
            if (fps > kBandFps[k]) {
                b = k;
                break;
            }
        band_ms[b] += ms;
        total_ms += ms;
    }
    std::fprintf(f, "FRAME RATE\n");
    std::fprintf(f, "  Average          %6.1f FPS\n", s.fps);
    std::fprintf(f, "  1%% low           %6.1f FPS   the slowest 1%% of frames\n", low_ms > 0.0 ? 1000.0 / low_ms : 0.0);
    std::fprintf(f, "  Slowest frame    %6.1f FPS\n", s.slowest_fps);
    std::fprintf(f, "  Fastest frame    %6.1f FPS\n", s.fastest_fps);
    std::fprintf(f, "  Time spent at   ");
    for (int k = 0; k < 5; ++k)
        std::fprintf(f, "%s %s %.0f%%", k ? " |" : "", kBandName[k], total_ms > 0.0 ? 100.0 * band_ms[k] / total_ms : 0.0);
    std::fprintf(f, "\n\n");

    std::fprintf(f, "WHAT THE ADD-ON COSTS\n");
    std::fprintf(f, "  Processor      %.2f ms a frame on its busiest thread -- without it, up to %.1f FPS more\n",
        s.cpu_busiest_ms, PerfFpsGain(s.frame_ms, s.cpu_busiest_ms));
    std::fprintf(f, "                 %.2f ms a frame over the %d threads it ran on\n", s.cpu_all_ms, s.cpu_threads);
    if (s.gpu_read)
        std::fprintf(f, "  Graphics card  %.2f ms a frame -- without it, up to %.1f FPS more\n", s.gpu_ms,
            PerfFpsGain(s.frame_ms, s.gpu_ms));
    else
        std::fprintf(f, "  Graphics card  no reading\n");
    std::fprintf(f, "  Measuring      %.2f ms a frame over all threads, left out of the processor figures\n", s.timers_ms);
    std::fprintf(f, "  \"Up to\": the processor and the graphics card work side by side, and a frame rate cap hides any gain.\n\n");

    std::fprintf(f, "WHERE THE PROCESSOR TIME GOES (ms a frame, all threads)\n");
    for (int a = 0; a < PA_COUNT; ++a)
        std::fprintf(f, "  %-46s %7.3f\n", kPerfAreaNames[a], s.area_ms[a]);
    std::fprintf(f, "\nWHERE THE GRAPHICS CARD TIME GOES (ms a frame)\n");
    for (int a = 0; a < kPerfGpuAreaCount; ++a)
        std::fprintf(f, "  %-52s %7.3f\n", kPerfGpuAreas[a].name, s.gpu_area_ms[a]);
    const uint32_t untimed = g_gt_untimed.load(std::memory_order_relaxed) - r.gt_untimed0;
    const uint32_t dropped = g_gt_dropped.load(std::memory_order_relaxed) - r.gt_dropped0;
    std::fprintf(f, "  Read for %u of %d frames", r.span.gpu_frames, s.frames);
    if (dropped) std::fprintf(f, "; %u frame(s) came back too late to read", dropped);
    if (untimed) std::fprintf(f, "; %u step(s) went untimed, too many in one frame -- the figures are low", untimed);
    std::fprintf(f, "\n\n");

    std::fprintf(f, "MEMORY              now          at the start   highest while measured\n");
    const PerfMemory& m0 = r.mem_start;
    std::fprintf(f, "  Video memory     %6.2f GB    %6.2f GB      %6.2f GB    of %.2f GB available (%.0f%%)\n",
        mem_.vram_used / kGB, r.have_mem_start ? m0.vram_used / kGB : 0.0, mem_peak_.vram_used / kGB,
        mem_.vram_budget / kGB, mem_.vram_budget ? 100.0 * (double)mem_.vram_used / (double)mem_.vram_budget : 0.0);
    std::fprintf(f, "  Shared memory    %6.2f GB    %6.2f GB      %6.2f GB    system memory the graphics card uses\n",
        mem_.shared_used / kGB, r.have_mem_start ? m0.shared_used / kGB : 0.0, mem_peak_.shared_used / kGB);
    std::fprintf(f, "  Game memory      %6.2f GB    %6.2f GB      %6.2f GB    in use %.2f GB\n",
        mem_.game_private / kGB, r.have_mem_start ? m0.game_private / kGB : 0.0, mem_peak_.game_private / kGB,
        mem_.game_working_set / kGB);
    std::fprintf(f, "\n");

    std::fprintf(f, "MOD RESOURCE ALLOCATIONS   now MiB / count   start MiB / count   sampled peak MiB / count\n");
    static const char* const memory_names[] = {
        "Active buffers", "Active textures", "Retired buffers", "Retired textures"
    };
    for (unsigned i = 0; i < 4; ++i) {
        const auto& current = mem_.addon.rows[i];
        const auto& start = r.mem_start.addon.rows[i];
        const auto& peak = r.addon_peak.rows[i];
        std::fprintf(f, "  %-18s %9.2f / %-7llu %9.2f / %-7llu %9.2f / %llu\n",
            memory_names[i], current.bytes / kMB, current.count,
            start.bytes / kMB, start.count, peak.bytes / kMB, peak.count);
        if (peak.unknown)
            std::fprintf(f, "    Unknown allocation sizes (now/start/peak): %llu / %llu / %llu; excluded from MiB\n",
                current.unknown, start.unknown, peak.unknown);
    }
    std::fprintf(f,
        "  Allocation sizes for resources created by the mod scripting device. Residency may vary.\n"
        "  Includes default, upload and readback heaps; staging textures backed by buffers count as buffers.\n"
        "  Retired resources await release through GPU retirement/readback after wrapper destruction.\n"
        "  Excludes game resources, shared upload/staging pools, CPU copies, descriptors and other addon caches.\n"
        "  Each column has its own peak for this report. Samples: once a second and at start/end.\n\n");

    int mods = 0;
    ModLoader::GetLastStats(mods);
    std::fprintf(f, "SETTINGS\n");
    std::fprintf(f, "  Active mods: %d\n", mods);
    std::fprintf(f, "  A2 Debug log: %s\n", OnOff(r.debug_was));
    std::fprintf(f, "  A4 Decensor: %s\n", OnOff(g_decensor));
    std::fprintf(f, "  A5 Hide UID: %s\n", OnOff(g_hide_uid));
    std::fprintf(f, "  A12 Mod textures in the background: %s\n", OnOff(g_tex_deferred));
    std::fprintf(f, "  A13 Look-alike texture matches: %s\n", OnOff(g_texlod_resemblance));
    std::fprintf(f, "  A14 Texture match cache: %s\n", OnOff(g_texlod_cache));
    std::fprintf(f, "  A16 Mod buffers in video memory: %s\n\n", OnOff(MigotoPort::g_buffers_in_vram));

    PortApi::GpuWorkTotals g1;
    PortApi::GetGpuWorkTotals(&g1);
    const double inv = s.frames > 0 ? 1.0 / (double)s.frames : 0.0;
    auto         pf = [&](unsigned a1, unsigned a0) { return (double)(unsigned)(a1 - a0) * inv; };
    std::fprintf(f, "MOD SCRIPTS, a frame (the counters 3DMigoto shows on DX11)\n");
    std::fprintf(f, "  Values sent to shaders (IniParams)    %8.1f\n", pf(g1.iniparams_updates, r.gw0.iniparams_updates));
    std::fprintf(f, "  Resource copies                        %8.1f\n", pf(g1.full_copies, r.gw0.full_copies));
    std::fprintf(f, "  Resources bound by reference           %8.1f\n", pf(g1.reference_copies, r.gw0.reference_copies));
    std::fprintf(f, "  Buffer region copies                   %8.1f\n", pf(g1.region_copies, r.gw0.region_copies));
    std::fprintf(f, "  Resources cleared                      %8.1f\n", pf(g1.views_cleared, r.gw0.views_cleared));
    std::fprintf(f, "  Resources created                      %8.1f\n", pf(g1.resources_created, r.gw0.resources_created));
    std::fprintf(f, "  Resource pool swaps                    %8.1f\n", pf(g1.pool_swaps, r.gw0.pool_swaps));
    std::fprintf(f, "  Draws and compute added by mods        %8.1f\n", pf(g1.injected_draws, r.gw0.injected_draws));
    std::fprintf(f, "  Section scripts   reused %7.1f   recorded again %7.1f   run in full %7.1f\n",
        pf(g1.replay_top_replayed, r.gw0.replay_top_replayed), pf(g1.replay_top_recorded, r.gw0.replay_top_recorded),
        pf(g1.replay_top_plain, r.gw0.replay_top_plain));
    std::fprintf(f, "  Called scripts    reused %7.1f   recorded again %7.1f   run in full %7.1f\n\n",
        pf(g1.replay_sub_replayed, r.gw0.replay_sub_replayed), pf(g1.replay_sub_recorded, r.gw0.replay_sub_recorded),
        pf(g1.replay_sub_plain, r.gw0.replay_sub_plain));

    static const char* const kGwNames[PGW_COUNT] = {
        "Buffer copies by mod scripts",
        "Texture copies by mod scripts",
        "Buffer uploads",
        "Texture uploads",
        "Shape key copies",
        "Views created",
        "State changes (barriers)",
        "State changes taken implicitly, no barrier",
        "Waits after compute writes (UAV barriers)",
        "Waits after compute writes a state change made",
        "Reads of a buffer already readable, no barrier",
        "Texture uploads merged into one still waiting",
    };
    static const bool kGwBytes[PGW_COUNT] = { true, false, true, true, true, false, false, false, false, false, false, false };
    std::fprintf(f, "WORK GIVEN TO THE GRAPHICS CARD, a frame\n");
    for (int k = 0; k < PGW_COUNT; ++k) {
        const double n = (double)(g_gw_n[k].load(std::memory_order_relaxed) - r.gw_n0[k]) * inv;
        if (kGwBytes[k])
            std::fprintf(f, "  %-48s %8.1f  %8.3f MB\n", kGwNames[k], n,
                (double)(g_gw_bytes[k].load(std::memory_order_relaxed) - r.gw_bytes0[k]) * inv / kMB);
        else
            std::fprintf(f, "  %-48s %8.1f\n", kGwNames[k], n);
    }
    std::fprintf(f, "  Game textures read to recognise them: %llu (%.1f MB), of them %llu read again (%llu unchanged)\n\n",
        (unsigned long long)(g_th_all.load() - r.th_all0), (double)(g_th_bytes.load() - r.th_bytes0) / kMB,
        (unsigned long long)(g_th_rehash.load() - r.th_rehash0), (unsigned long long)(g_th_same.load() - r.th_same0));

    std::fprintf(f, "DETAILS FOR BUG REPORTS\n\n");
    std::fprintf(f, "Every measured step, ms a frame over all threads. \"With inner\" includes the steps indented under it.\n");
    std::fprintf(f, "  %-22s %10s %10s %13s %12s\n", "step", "with inner", "own", "slowest frame", "calls a frame");
    for (int i = 0; i < PROF_COUNT; ++i) {
        const PerfStep& p = s.steps[i];
        std::fprintf(f, "  %-22s %10.3f %10.3f %13.3f %12.0f\n", kProfSteps[i].name, p.incl_ms, p.self_ms, p.max_ms,
            p.calls);
    }
    std::fprintf(f, "\nGraphics card steps, ms a frame (indented rows run inside the row above)\n");
    std::fprintf(f, "  %-46s %9s %11s\n", "step", "ms", "per frame");
    for (int k = 0; k < GT_COUNT; ++k)
        std::fprintf(f, "  %-46s %9.3f %11.1f\n", g_gt_names[k], s.gpu_step_ms[k], s.gpu_step_n[k]);

    std::fprintf(f, "\nBusiest threads, ms a frame, each with its largest steps\n");
    int order[kProfThreads];
    int n = 0;
    for (int t = 0; t < r.span.n_threads; ++t)
        if (r.span.threads[t].self_ms > 0.001) order[n++] = t;
    std::sort(order, order + n, [&](int a, int b) { return r.span.threads[a].self_ms > r.span.threads[b].self_ms; });
    for (int k = 0; k < n && k < 6; ++k) {
        const PerfThread& th = r.span.threads[order[k]];
        if (th.tid == 0xFFFFFFFFu)
            std::fprintf(f, "  other threads  %6.3f:", ThreadNet(th));
        else
            std::fprintf(f, "  thread %-7u %6.3f:", th.tid, ThreadNet(th));
        int top[PROF_COUNT];
        for (int i = 0; i < PROF_COUNT; ++i) top[i] = i;
        std::sort(top, top + PROF_COUNT, [&](int a, int b) { return th.step_ms[a] > th.step_ms[b]; });
        for (int i = 0; i < 6 && th.step_ms[top[i]] >= 0.005; ++i)
            std::fprintf(f, "%s %s %.3f", i ? "," : "", StepName(top[i]), th.step_ms[top[i]]);
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "\n");
    PortApi::CommandCostReport(f, s.frames);
}
