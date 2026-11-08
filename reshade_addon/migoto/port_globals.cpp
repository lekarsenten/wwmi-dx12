// ===========================================================================
//  port_globals -- OUR file. PORT-LEAF.
//
//  Supplies the handful of definitions their engine links against that live in D3D11Wrapper.cpp, a
//  file we do NOT take. Below is a line-by-line accounting of all 1,207 lines of that file, because
//  "we omitted it" is not a reason -- every omission needs one.
//
//  ---------------------------------------------------------------------------------------------
//  D3D11Wrapper.cpp, 1207 lines: what we take and what we do not
//  ---------------------------------------------------------------------------------------------
//   :1-33    includes / comments                    OMIT - boilerplate; we include what we need.
//   :34-35   Globals StaticG; Globals *G = &StaticG  TAKE - reproduced verbatim below. This is the
//                                                   engine's entire global state.
//   :37-39   LogFile, gLogDebug, gLogVerbosity      TAKE - their log.h writes through these.
//   :50      resource_creation_mode_lock            TAKE - held around resource creation; their
//                                                   CommandList code takes it. (:383 initialises it.)
//   :60-93   verify_intended_target_late()          OMIT - checks the DLL was loaded into the
//                                                   intended game exe. ReShade decides what loads us.
//   :95-126  InitializeDLL()                        OMIT the body, TAKE LoadConfigFile() (:110). The
//                                                   rest is DLL bootstrap: the init guard, the
//                                                   default-locale record, the target-exe check, the
//                                                   init delay.
//   :128-130 DestroyDLL()                           OMIT - empty. Ours still saves the settings on
//                                                   exit (PortApi::SaveSettingsOnExit).
//   :132-299 ~28x D3DKMT* forwarders                OMIT - these exist ONLY because 3dmigoto ships as
//                                                   a replacement d3d11.dll and must re-export every
//                                                   symbol the real one does, forwarding to it. We are
//                                                   a ReShade addon and export no d3d11 entry points,
//                                                   so there is nothing to forward.
//   :301-373 D3D10DDI typedefs, loader plumbing     OMIT - same reason.
//   :375-479 InitD311(), function-pointer loading   OMIT - loads the system d3d11.dll to forward into.
//                                                   We never create or wrap a D3D11 device.
//   :381-384 FOUR InitializeCriticalSectionPretty   TAKE three - G->mCriticalSection, G->mResourcesLock
//                                                   and resource_creation_mode_lock. Their parser
//                                                   takes G->mCriticalSection while registering command
//                                                   lists, and entering an uninitialised
//                                                   CRITICAL_SECTION is an access violation.
//                                                   G->mShaderBindingsLock: nothing we compile takes it.
//   :481-497 D3D11On12CreateDevice export           OMIT - and proven actively harmful here: calling
//                                                   it against this game's device crashes UE's present
//                                                   path with E_NOTIMPL. See memory
//                                                   [[d3d11on12-not-viable]].
//   :499-586 OpenAdapter10 / D3D11Core* / D3DKMT*    OMIT - more re-exports.
//   :596-603 EnableDebugFlags()                     OMIT - adds the D3D11 debug layer at device
//                                                   creation; the game owns device creation.
//   :611-628 ShowDebugInfo()                        OMIT - logs D3D11 device caps.
//   :668-697 ForceDX11()                            OMIT - clamps feature levels at creation time.
//   :699-785 wrap_d3d11_device_and_context()        OMIT - constructs HackerDevice/HackerContext
//                                                   around a real D3D11 device. Our equivalent is the
//                                                   DX12-backed shim; this is THE leaf boundary.
//   :824-904   D3D11CreateDevice()                  OMIT - the game creates a D3D12 device itself.
//   :944-1033  D3D11CreateDeviceAndSwapChain()      OMIT - likewise.
//   :1045-1181 ReplaceOnMatch / Hooked_LoadLibraryExW  OMIT - LoadLibrary interception used to get
//                                                   their DLL in front of the game's. ReShade already
//                                                   put us in the process.
//   :1204-1207 CBTProc()                            OMIT - CBT window hook, part of the same
//                                                   injection mechanism.
//
//  Net: a handful of lines are load-bearing for the engine; the rest exist to make a DLL impersonate
//  d3d11.dll -- a job ReShade already does for us.
// ===========================================================================

#include "globals.h"
#include "Overlay.h"       // LogLevel, Overlay
#include "ShaderRegex.h"   // ShaderRegexGroup, ShaderRegexPattern
#include "nvprofile.h"     // ProfileSettings
#include "Hunting.h"       // MigotoIncludeHandler, ParseHuntingSection
#include "IniHandler.h"    // GetIniStringAndLog -- their own ini reader, for analyse_options
#include "input.h"
#include "util.h"          // parse_enum_option_string
#include "DLLMainHook.h"   // cHookMgr
#include "../core/log.h" // our logger, which LogOverlay is routed to
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

Globals  StaticG;
Globals* G = &StaticG;

FILE*        LogFile = nullptr;
bool         gLogDebug = false;
LogVerbosity gLogVerbosity = LogVerbosity::INVALID;

CRITICAL_SECTION resource_creation_mode_lock;

HINSTANCE migoto_handle = nullptr;

void LogOverlay(LogLevel level, char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    // Trailing newlines are theirs; our logger adds its own.
    for (int i = (int)strnlen(buf, sizeof(buf)) - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); --i)
        buf[i] = '\0';
    if (level == LOG_DIRE || level == LOG_WARNING || level == LOG_WARNING_MONOSPACE)
        logger::write("migoto: %s", buf);
    else
        logger::info("migoto: %s", buf);
}

void LogOverlayW(LogLevel level, wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wchar_t wbuf[2048];
    _vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, fmt, ap);
    va_end(ap);
    for (int i = (int)wcsnlen(wbuf, _countof(wbuf)) - 1; i >= 0 && (wbuf[i] == L'\n' || wbuf[i] == L'\r'); --i)
        wbuf[i] = L'\0';
    if (level == LOG_DIRE || level == LOG_WARNING || level == LOG_WARNING_MONOSPACE)
        logger::write("migoto: %S", wbuf);
    else
        logger::info("migoto: %S", wbuf);
}

std::map<std::wstring, ShaderRegexGroup> shader_regex_groups;
std::vector<ShaderRegexGroup*>           shader_regex_group_index;
unsigned int                             shader_regex_hash = 0;

ProfileSettings profile_settings;

void ParseHuntingSection()
{
    wchar_t buf[MAX_PATH];

    // theirs: Hunting.cpp ParseHuntingSection, verbatim apart from the log line.
    if (GetIniStringAndLog(L"Hunting", L"analyse_options", 0, buf, MAX_PATH)) {
        G->def_analyse_options = parse_enum_option_string<wchar_t*, FrameAnalysisOptions>(FrameAnalysisOptionNames, buf, NULL);
    } else {
        G->def_analyse_options = FrameAnalysisOptions::INVALID;
    }
    logger::write("PORT: [Hunting] analyse_options = 0x%08x (from d3dx.ini)",
        (unsigned)G->def_analyse_options);
    G->config_reloadable = RegisterIniKeyBinding(L"Hunting", L"reload_config", FlagConfigReload, NULL, 0, NULL);
}

// CleanupShaderMaps drops a released shader from their hunting maps. We keep no such maps.
void CleanupShaderMaps(ID3D11DeviceChild*) {}

// theirs: Hunting.cpp MigotoIncludeHandler -- an #include resolves against the including file's directory
// (recursive_include) or the first file's, else the working directory; their file is not built here.
MigotoIncludeHandler::MigotoIncludeHandler(const char* path)
{
    push_dir(path);
}

void MigotoIncludeHandler::push_dir(const char* path)
{
    const char* const bs = strrchr(path, '\\');
    const char* const fs = strrchr(path, '/');
    const char* const dir_sep = !bs ? fs : !fs ? bs : (bs > fs ? bs : fs);
    dir_stack.push_back(dir_sep ? std::string(path, (size_t)(dir_sep - path + 1)) : std::string());
}

HRESULT MigotoIncludeHandler::Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID,
    LPCVOID* ppData, UINT* pBytes)
{
    if (!ppData || !pBytes) return E_INVALIDARG;
    *ppData = nullptr;
    *pBytes = 0;
    std::string apath = (G->recursive_include ? dir_stack.back() : dir_stack.front()) + pFileName;
    FILE*       f = nullptr;
    if ((fopen_s(&f, apath.c_str(), "rb") != 0 || !f) && !G->recursive_include) {
        apath = pFileName;
        fopen_s(&f, apath.c_str(), "rb");
    }
    if (!f) return E_FAIL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return E_FAIL;
    }
    char*  buf = new char[(size_t)len + 1];
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    *ppData = buf;
    *pBytes = (UINT)got;
    push_dir(apath.c_str());
    return S_OK;
}

HRESULT MigotoIncludeHandler::Close(LPCVOID pData)
{
    delete[] (char*)pData;
    dir_stack.pop_back();
    return S_OK;
}

void Overlay::DrawOverlay(void) {}
void ClearNotices() {}

CNktHookLib cHookMgr;

DWORD tls_idx = TLS_OUT_OF_INDEXES;

// NvAPIOverride re-arms NVAPI's stereo state before a driver call. No NVAPI here.
void NvAPIOverride() {}

int parse_ini_profile_line(std::wstring*, std::wstring*)
{
    return 0;
}

ShaderRegexPattern::ShaderRegexPattern()
    : regex(nullptr),
      do_replace(false)
{}
ShaderRegexPattern::~ShaderRegexPattern()
{
    if (regex) pcre2_code_free(regex);
}

bool ShaderRegexPattern::compile(std::string*)
{
    LogOverlay(LOG_NOTICE, "[ShaderRegex] patterns are not applied on DX12: shader assembly cannot be rewritten "
                           "because DXIL is signed bitcode. The section's filter_index and command lists are kept "
                           "for the shaders the add-on patches itself.\n");
    return true;
}

// PORTED from ShaderRegex.cpp ShaderRegexGroup::link_command_lists_and_filter_index :366-426.
void ShaderRegexGroup::link_command_lists_and_filter_index(UINT64 shader_hash)
{
    ShaderOverride*                         shader_override = NULL;
    std::wstring                            match_section;
    std::wstring                            ini_line;
    CommandList::Commands::reverse_iterator i;

    if (command_list.noop() && post_command_list.noop() && filter_index == FLT_MAX)
        return;

    shader_override = &G->mShaderOverrideMap[shader_hash];

    if (shader_override->command_list.ini_section.empty()) {
        match_section = command_list.ini_section + L".Match";
        shader_override->command_list.ini_section = match_section;
        shader_override->post_command_list.ini_section = match_section;
        shader_override->post_command_list.post = true;
    }

    if (shader_override->filter_index == FLT_MAX)
        shader_override->filter_index = filter_index;

    if (link) {
        for (i = shader_override->command_list.commands.rbegin();
             i != shader_override->command_list.commands.rend(); i++) {
            if (*i == link)
                return;
        }
        shader_override->command_list.commands.push_back(link);
        if (post_link)
            shader_override->post_command_list.commands.push_back(post_link);
        return;
    } else if (post_link) {
        for (i = shader_override->post_command_list.commands.rbegin();
             i != shader_override->post_command_list.commands.rend(); i++) {
            if (*i == post_link)
                return;
        }
        shader_override->post_command_list.commands.push_back(post_link);
        return;
    }

    ini_line = L"[" + command_list.ini_section + L".Match] run = linked command list";

    if (!command_list.noop())
        link = LinkCommandLists(&shader_override->command_list, &command_list, &ini_line);

    if (!post_command_list.noop())
        post_link = LinkCommandLists(&shader_override->post_command_list, &post_command_list, &ini_line);
}

bool ShaderRegexPattern::named_group_overlaps(ShaderRegexTemps&)
{
    return false;
}

namespace MigotoPort {

// ---- :381-383: the critical sections worth keeping out of InitD311 -------------------------------
void InitEngineGlobals()
{
    static bool once = false;
    if (once) return;
    once = true;
    InitializeCriticalSection(&G->mCriticalSection);         // their :381
    InitializeCriticalSection(&G->mResourcesLock);           // their :382
    InitializeCriticalSection(&resource_creation_mode_lock); // their :383
}

} // namespace MigotoPort
