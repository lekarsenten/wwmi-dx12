// rt/rt_dumps.cpp -- RTCSDUMP / RTVSDUMP (debug): the game's skin-cache compute shaders and pass-through
// vertex shaders, saved to wwmi_dx12\rt_dump for offline study.
#include "rt_internal.h"

using namespace rt_portable;

namespace {

// At most this many shaders are saved in one run.
constexpr uint32_t kCsDumpCap = 64;
std::atomic<uint32_t> s_cs_dumped{ 0 };

/**
 * Tells whether a shader's bytecode names a skin-cache entry point. Unreal's GPU skin cache shaders keep their
 * entry point name ("SkinCacheUpdateBatchCS", "...RecomputeTangents...") as plain text in the DXIL container.
 *
 * @param code      The shader bytecode.
 * @param code_size Its size in bytes.
 * @return true when the text "SkinCache" or "GpuSkin" or "RecomputeTangents" occurs in it.
 */
bool LooksLikeSkinCacheShader(const void* code, size_t code_size)
{
    const std::string_view bytes(static_cast<const char*>(code), code_size);
    return bytes.find("SkinCache") != std::string_view::npos || bytes.find("GpuSkin") != std::string_view::npos ||
           bytes.find("RecomputeTangents") != std::string_view::npos;
}

} // namespace

void RtNoteComputeShader(uint64_t cs_hash, const void* code, size_t code_size)
{
    if (!code || code_size == 0) return;
    const bool skin_cache = LooksLikeSkinCacheShader(code, code_size);
    // Debug: also saved for study -- the shader that runs only with ray tracing on (the morphs?), and any shader
    // whose bytecode names morphs.
    const std::string_view bytes(static_cast<const char*>(code), code_size);
    const bool studied = cs_hash == 0xa7155a3e69aaafbfull || bytes.find("Morph") != std::string_view::npos;
    if (!skin_cache && !studied) return;
    if (skin_cache) NoteSkinCbLayout(cs_hash, code, code_size);
    if (!g_debug) return;
    if (s_cs_dumped.fetch_add(1, std::memory_order_relaxed) >= kCsDumpCap) return;

    const std::wstring folder = g_addon.data_dir + L"\\rt_dump\\";
    CreateDirectoryW(g_addon.data_dir.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    wchar_t name[40];
    swprintf_s(name, L"%016llx.cso", (unsigned long long)cs_hash);
    const std::wstring path = folder + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        logger::write("RTCSDUMP cs=%016llx: could not write %S (error %lu)", (unsigned long long)cs_hash, path.c_str(),
            GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(file, code, (DWORD)code_size, &written, nullptr);
    CloseHandle(file);
    logger::write("RTCSDUMP cs=%016llx: %zu bytes saved to %S", (unsigned long long)cs_hash, code_size, path.c_str());
}

/**
 * Debug: saves the vertex shader of each pass-through PSO (the draws PTSKIN feeds) to rt_dump\vs_<hash>.cso, so
 * what it fetches by vertex id, outside its input layout, can be read from its disassembly.
 *
 * @param pso     The new graphics pipeline.
 * @param vs_hash Its vertex shader's hash.
 * @param vs_code The vertex shader's bytecode.
 * @param vs_size Its size in bytes.
 */
void RtNoteGraphicsPipeline(uint64_t pso, uint64_t vs_hash, const void* vs_code, size_t vs_size)
{
    // A handle can be a destroyed PSO's: forget what IsPassThroughPso remembered for it.
    ForgetPassThroughPso(pso);
    if (!vs_code || vs_size == 0) return;
    char layout[1024] = {};
    MigotoPort::DescribeInputLayout(pso, layout, sizeof(layout));
    static std::atomic<int> s_calls{ 0 };
    if (s_calls.fetch_add(1) < 3)
        logger::write("RTVSDUMP probe: debug=%d vs=%016llx layout '%s'", (int)g_debug, (unsigned long long)vs_hash, layout);
    if (!g_debug || !strstr(layout, "ATTRIBUTE14@s4+0:f6")) return;
    static std::mutex s_mutex;
    static std::set<uint64_t> s_saved;
    {
        std::lock_guard lock(s_mutex);
        if (s_saved.size() >= 1000 || !s_saved.insert(vs_hash).second) return;
    }
    const std::wstring folder = g_addon.data_dir + L"\\rt_dump\\";
    CreateDirectoryW(g_addon.data_dir.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    wchar_t name[48];
    swprintf_s(name, L"vs_%016llx.cso", (unsigned long long)vs_hash);
    const std::wstring path = folder + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        logger::write("RTVSDUMP vs=%016llx: could not write %S (error %lu)", (unsigned long long)vs_hash, path.c_str(),
            GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(file, vs_code, (DWORD)vs_size, &written, nullptr);
    CloseHandle(file);
    logger::write("RTVSDUMP vs=%016llx pso=%llx: pass-through vertex shader, %zu bytes saved to %S",
        (unsigned long long)vs_hash, (unsigned long long)pso, vs_size, path.c_str());
}
