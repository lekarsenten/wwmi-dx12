// ===========================================================================================
//  frame_analysis_driver.cpp -- OURS, and deliberately tiny.
//
//  The ported engine (frame_analysis_engine.cpp, from their FrameAnalysis.cpp) decides everything:
//  which slots to dump, how to name files, how to dedupe, what a draw call means. This only does what
//  their Hunting.cpp / HackerDXGI.cpp plumbing did -- own an instance, arm it, stop it at the frame
//  boundary, and call FrameAnalysisAfterDraw once per draw.
//
//  BOTH halves are PORTED, not invented:
//
//    arm  -- Hunting.cpp AnalyseFrame                     :1221-1270
//    stop -- HackerDXGI.cpp HackerSwapChain::RunFrameActions :214-228
//
//  Their ordering is load-bearing. The stop check runs at the TOP of Present; arming happens later,
//  from input dispatch. So an arm during frame N is stopped at the Present of frame N+1 -- exactly one
//  frame of draws. ReShade runs the overlay INSIDE present, so arming from it directly would let the
//  same present that armed the capture also close it. The deferred arm below reproduces their ordering
//  deterministically instead of depending on where ReShade runs the overlay.
// ===========================================================================================
// MUST precede frame_analysis_engine.h. Their header declares IID_FrameAnalysisContext and
// InputLayoutDescGuid with DEFINE_GUID, which only EMITS the definitions in a TU that has included
// <initguid.h> first. InputLayoutDescGuid is load-bearing: DumpVBs fetches the input-layout blob with
// it (theirs stores that blob as private data on the ID3D11InputLayout).
#include <initguid.h>
#include "frame_analysis_engine.h"
#include "frame_analysis_driver.h"
#include "port_api.h" // PortApi::FrameAnalysisCtx -- the ONE context, see Get() below
#include "globals.h"
#include "util.h" // CreateDirectoryEnsuringAccess
#include "dx12_pipeline.h" // PipeState/State() -- what the dump will read = what is bound
#include "dx12_wrap.h" // AsWrapView: view -> our resource wrapper -> the DX12 resource
#include "dx12_dump_readback.h" // PreDrain*: the coalescing
#include "dump_watchdog.h" // DumpStage / DumpWatchdog*: a hung capture names its position
#include "../core/log.h"

#include <mutex>
#include <time.h>

namespace MigotoPort {

namespace {

FrameAnalysisContext* g_fa = nullptr;
std::recursive_mutex g_fa_mtx; // recursive: FrameAnalysisDrawScope holds it across the publish + dump
bool g_char_only = true;
bool g_log_only = false;
int g_frames = 1; // the frames a capture spans under d3dx.ini's hold: the ones the button asked for

// Pending arm, applied AFTER the stop check in the same present -- see the header comment.
int g_arm_req = 0;
bool g_arm_char = true;
bool g_arm_log_only = false;

FrameAnalysisContext* Get()
{
    if (!g_fa) g_fa = PortApi::FrameAnalysisCtx();
    return g_fa;
}

bool AnalyseFrameStart()
{
    wchar_t path[MAX_PATH];
    wchar_t subdir[MAX_PATH];
    time_t ltime;
    struct tm tm;

    time(&ltime);
    _localtime64_s(&tm, &ltime);
    wcsftime(subdir, MAX_PATH, L"FrameAnalysis-%Y-%m-%d-%H%M%S", &tm);

    if (!DumpDir().empty()) {
        CreateDirectoryW(DumpDir().c_str(), nullptr);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", DumpDir().c_str(), subdir);
    } else {
        wcscpy_s(path, MAX_PATH, subdir);
    }

    if (!CreateDirectoryEnsuringAccess(path)) {
        logger::write("FRAMEANALYSIS: could not create %S (err %u) -- capture ABORTED", path, GetLastError());
        return false;
    }

    wcscpy_s(G->ANALYSIS_PATH, MAX_PATH, path);
    G->cur_analyse_options = g_log_only ? FrameAnalysisOptions::INVALID : G->def_analyse_options;
    G->frame_analysis_seen_rts.clear();
    G->analyse_frame_no = 1;
    G->analyse_frame = true;
    DumpWatchdogStart();

    logger::write("FRAMEANALYSIS: capturing -> %S (analyse_options=0x%08x %s, %s)", path,
        (unsigned)G->cur_analyse_options, g_log_only ? "-- log only, d3dx.ini's not used" : "from d3dx.ini",
        g_char_only ? "CHAR draws only" : "EVERY draw");
    return true;
}

void CopyAddonLogIntoCapture()
{
    if (!G->ANALYSIS_PATH[0]) return;
    wchar_t dst[MAX_PATH];
    _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\addon.log", G->ANALYSIS_PATH);
    const DWORD err = logger::copy_to(dst);
    if (err) logger::write("FRAMEANALYSIS: could not copy the addon log into the capture (err %u)", err);
}

// PORTED from HackerDXGI.cpp RunFrameActions :214-228.
void AnalyseFrameStopAtBoundary()
{
    if (!G->analyse_frame) return;

    if (!g_log_only && (G->def_analyse_options & FrameAnalysisOptions::HOLD) && G->analyse_frame_no < g_frames) {
        G->analyse_frame_no++;
    } else {
        G->analyse_frame = false;
        logger::write("FRAMEANALYSIS: frame analysis saved to %S", G->ANALYSIS_PATH);
        DumpWatchdogStop();
        CopyAddonLogIntoCapture();
    }
}

} // namespace

void FrameAnalysisArm(int frames, bool char_only, bool log_only)
{
    std::lock_guard<std::recursive_mutex> l(g_fa_mtx);
    g_arm_req = frames < 1 ? 1 : frames;
    g_arm_char = char_only;
    g_arm_log_only = log_only;
    logger::write("FRAMEANALYSIS: arm requested (%s) -- starts next frame, %s",
        char_only ? "CHAR draws only" : "EVERY draw", log_only ? "log.txt only" : "analyse_options come from d3dx.ini");
}

namespace {

struct EngineFault {
    unsigned long code = 0;
    void* addr = nullptr;
    void* frames[32] = {};
    unsigned n = 0;
    unsigned long long access = 0; // the VA an access violation touched
    int rw = -1; // 0 read / 1 write
};

int EngineFaultFilter(EXCEPTION_POINTERS* ep, EngineFault* f)
{
    if (ep && ep->ExceptionRecord) {
        f->code = ep->ExceptionRecord->ExceptionCode;
        f->addr = ep->ExceptionRecord->ExceptionAddress;
        if (f->code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            f->rw = (int)ep->ExceptionRecord->ExceptionInformation[0];
            f->access = (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1];
        }
    }
    f->n = RtlCaptureStackBackTrace(0, 32, f->frames, nullptr); // the filter runs on top of the faulting frame
    return EXCEPTION_EXECUTE_HANDLER;
}

// MSVC refuses __try in a function with objects to unwind (C2712): this one holds none.
bool EngineAfterDrawGuarded(FrameAnalysisContext* fa, bool compute, DrawCallInfo* call_info, EngineFault* f)
{
    __try {
        fa->PortEntryAfterDraw(compute, call_info);
        return true;
    } __except (EngineFaultFilter(GetExceptionInformation(), f)) {
        return false;
    }
}

void ReleaseIfHeldByThisThread(CRITICAL_SECTION& cs, const char* name)
{
    unsigned released = 0;
    while (cs.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId() && released < 16) {
        LeaveCriticalSection(&cs);
        ++released;
    }
    if (released) logger::write("FRAMEANALYSIS: released %s x%u, held by the faulting frames", name, released);
}

void AbortCaptureAfterFault(const EngineFault& f)
{
    logger::write(
        "FRAMEANALYSIS: FAULT 0x%08lX at %p (%s VA %llx) inside their engine at draw %06u -- capture ABORTED, the frame continues",
        f.code, f.addr,
        f.rw == 0       ? "reading"
            : f.rw == 1 ? "writing"
                        : "access",
        f.access, Get()->PortDrawIndex());
    DumpLogFrames("FRAMEANALYSIS", f.frames, f.n);
    ReleaseIfHeldByThisThread(G->mCriticalSection, "G->mCriticalSection");
    ReleaseIfHeldByThisThread(G->mResourcesLock, "G->mResourcesLock");
    G->analyse_frame = false;
    DumpWatchdogStop();
    CopyAddonLogIntoCapture();
}

} // namespace

bool FrameAnalysisArmed()
{
    return G->analyse_frame;
}

const wchar_t* FrameAnalysisFolder()
{
    return (G->analyse_frame && G->ANALYSIS_PATH[0]) ? G->ANALYSIS_PATH : nullptr;
}

bool FrameAnalysisWantGameDraw(bool claimed, bool char_band)
{
    if (!G->analyse_frame) return false;
    if (!g_char_only) return true; // theirs' native behaviour: the whole frame
    return claimed || char_band;
}

void FrameAnalysisAfterDraw(bool compute, DrawCallInfo* call_info, unsigned long long vs_hash,
    unsigned long long ps_hash, unsigned long long cs_hash)
{
    if (!G->analyse_frame) return;
    DumpStage("AfterDraw: wait g_fa_mtx", Get()->PortDrawIndex());
    std::lock_guard<std::recursive_mutex> l(g_fa_mtx);
    DumpStage("AfterDraw: pre-drain", Get()->PortDrawIndex());

    FrameAnalysisContext* fa = Get();
    fa->mCurrentVertexShader = vs_hash;
    fa->mCurrentPixelShader = ps_hash;
    fa->mCurrentComputeShader = cs_hash;
    {
        MigotoPort::PreDrainBegin();
        MigotoPort::PipeState& st = MigotoPort::State();
        for (int si = 0; si < 6; ++si)
            for (int slot = 0; slot < MigotoPort::kMaxSRV; ++slot)
                if (MigotoPort::WrapView* v = MigotoPort::AsWrapView(st.srv[si][slot]))
                    if (v->wres) MigotoPort::PreDrainAdd(v->wres->dx12);
        if (compute) {
            for (int slot = 0; slot < MigotoPort::kMaxUAV; ++slot)
                if (MigotoPort::WrapView* v = MigotoPort::AsWrapView(st.uav[slot]))
                    if (v->wres) MigotoPort::PreDrainAdd(v->wres->dx12);
        } else {
            for (int i = 0; i < 8; ++i)
                if (MigotoPort::WrapView* v = MigotoPort::AsWrapView(st.rtv[i]))
                    if (v->wres) MigotoPort::PreDrainAdd(v->wres->dx12);
            if (MigotoPort::WrapView* v = MigotoPort::AsWrapView(st.dsv))
                if (v->wres) MigotoPort::PreDrainAdd(v->wres->dx12);
        }
        MigotoPort::PreDrainFlush();
    }

    DumpStage("AfterDraw: theirs (PortEntryAfterDraw)", Get()->PortDrawIndex());
    EngineFault fault;
    if (!EngineAfterDrawGuarded(Get(), compute, call_info, &fault)) AbortCaptureAfterFault(fault);
    DumpStage("AfterDraw: done", Get()->PortDrawIndex());
}

FrameAnalysisDrawScope::FrameAnalysisDrawScope()
{
    if (!G->analyse_frame) return;
    g_fa_mtx.lock(); // no DumpStage here: a waiter's stage would mask the dumping thread's
    held_ = true;
}

FrameAnalysisDrawScope::~FrameAnalysisDrawScope()
{
    if (held_) g_fa_mtx.unlock();
}

unsigned FrameAnalysisDrawIndex()
{
    if (!G->analyse_frame) return 0;
    return Get()->PortDrawIndex();
}

void FrameAnalysisNote(const char* line)
{
    if (!G->analyse_frame || !line) return;
    DumpStage("Note: wait g_fa_mtx");
    std::lock_guard<std::recursive_mutex> l(g_fa_mtx);
    DumpStage("Note: log");
    // Their FrameAnalysisLog is declared with a non-const format; the text is ours and constant.
    Get()->FrameAnalysisLog(const_cast<char*>("%s\n"), line);
}

void FrameAnalysisFrameEnd()
{
    if (G->analyse_frame) DumpStage("FrameEnd: wait g_fa_mtx");
    std::lock_guard<std::recursive_mutex> l(g_fa_mtx);

    // THEIR stop check, FIRST -- HackerDXGI.cpp RunFrameActions :214.
    AnalyseFrameStopAtBoundary();

    if (g_arm_req) {
        g_frames = g_arm_req;
        g_arm_req = 0;
        g_char_only = g_arm_char;
        g_log_only = g_arm_log_only;
        AnalyseFrameStart();
    }
}

} // namespace MigotoPort
