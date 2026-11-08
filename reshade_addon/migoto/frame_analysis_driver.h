// ===========================================================================================
//  frame_analysis_driver.h -- OURS. The calls that let the addon drive THEIR ported
//  frame-analysis engine. Everything the dump actually does lives in frame_analysis_engine.cpp.
// ===========================================================================================
#pragma once

struct DrawCallInfo;

namespace MigotoPort {

// log_only: log.txt alone, whatever d3dx.ini's analyse_options ask for, and no hold.
void FrameAnalysisArm(int frames, bool char_only, bool log_only);

// Should this GAME draw be dumped? Our injected draws are always dumped, in both modes.
bool FrameAnalysisWantGameDraw(bool claimed, bool char_band);
bool FrameAnalysisArmed();
const wchar_t* FrameAnalysisFolder();

void FrameAnalysisAfterDraw(bool compute, DrawCallInfo* call_info, unsigned long long vs_hash,
    unsigned long long ps_hash, unsigned long long cs_hash);

unsigned FrameAnalysisDrawIndex();

void FrameAnalysisNote(const char* line);

// Tick the armed window at present.
void FrameAnalysisFrameEnd();

struct FrameAnalysisDrawScope {
    FrameAnalysisDrawScope();
    ~FrameAnalysisDrawScope();
    FrameAnalysisDrawScope(const FrameAnalysisDrawScope&) = delete;
    FrameAnalysisDrawScope& operator=(const FrameAnalysisDrawScope&) = delete;

private:
    bool held_ = false;
};

} // namespace MigotoPort
