// chain_trace.cpp -- the H3 chain trace (see chain_trace.h).
#include "../addon_common.h"
#include "chain_trace.h"
#include "../migoto/port_api.h"

ChainTrace g_chain_trace;

static constexpr char kLF = 10;

void ChainTrace::Open()
{
    if (fp_) return;
    wchar_t exe[MAX_PATH] = {};
    wchar_t path[MAX_PATH] = L"wwmi_dx12_chain_trace.log";
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                L"%s"
                L"\\"
                L"wwmi_dx12_chain_trace.log",
                exe);
        }
    }
    _wfopen_s(&fp_, path, L"w");
    if (!fp_) return;
    fprintf(fp_, "=== WWMI chain trace: every executed command + every variable, 10 frames ===%c", kLF);
    SyncGlobalObserver(); // trace EVERY list, not just the ones PortRunSection drives
}

void ChainTrace::Close()
{
    if (!fp_) return;
    fprintf(fp_, "=== end of trace ===%c", kLF);
    fclose(fp_);
    fp_ = nullptr;
    SyncGlobalObserver(); // fp_ is now null, so this removes the observer
}

// One executed command.
void ChainTrace::Line(const char* fn, const wchar_t* fq_section, const wchar_t* line)
{
    if (!fp_) return;
    _fprintf_l(fp_, "f%-6u %-22s %-58ls %ls%c", logger::utf8_locale(), frame_, fn ? fn : "?",
        fq_section ? fq_section : L"?", line ? line : L"(null)", kLF);
}

void ChainTrace::GlobalObserver(const wchar_t* line, void* /*user*/)
{
    g_chain_trace.Line("RunCommandList", L"(global)", line);
}

void ChainTrace::SyncGlobalObserver()
{
    if (fp_)
        PortApi::SetCommandObserver(&ChainTrace::GlobalObserver, nullptr);
    else
        PortApi::SetCommandObserver(nullptr, nullptr);
}

// Every variable their engine holds, at a frame boundary.
void ChainTrace::Vars(const char* when)
{
    if (!fp_) return;
    fprintf(fp_, "--- f%u %s: all engine variables ---%c", frame_, when, kLF);
    PortApi::ForEachVariable(
        [](const wchar_t* fq, float v, bool persist, void* user) {
            FILE* fp = static_cast<FILE*>(user);
            if (fp)
                _fprintf_l(fp, "    %-70ls = %g%s%c", logger::utf8_locale(), fq ? fq : L"?", v,
                    persist ? "  (persist)" : "", kLF);
        },
        fp_);
}
