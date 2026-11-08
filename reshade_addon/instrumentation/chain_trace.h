#pragma once
#include <cstdio>
#include <cstdint>
#include "diag_gate.h"

class ChainTrace {
public:
    WWMI_DEBUG_FN void Open() WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void Close() WWMI_DEBUG_BODY();
    // One executed command.
    WWMI_DEBUG_FN void Line(const char* fn, const wchar_t* fq_section, const wchar_t* line) WWMI_DEBUG_BODY();
    // Every variable their engine holds, at a frame boundary.
    WWMI_DEBUG_FN void Vars(const char* when) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void SyncGlobalObserver() WWMI_DEBUG_BODY();
    void SetFrame(uint32_t frame) { frame_ = frame; }
    bool IsOpen() const { return fp_ != nullptr; }

private:
    static void GlobalObserver(const wchar_t* line, void* user);
    FILE* fp_ = nullptr;
    uint32_t frame_ = 0;
};
WWMI_DEBUG_VAR ChainTrace g_chain_trace;
