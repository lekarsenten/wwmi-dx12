// shader_capture.cpp -- see shader_capture.h.
#include "shader_capture.h"
#include "diag_arms.h"
#include "../core/log.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../migoto/dx12_bridge.h" // DumpDir
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

ShaderCapture g_shader_capture;

namespace {

std::wstring DumpSubdir()
{
    wchar_t ts[64];
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    wcsftime(ts, 64, L"dump_%Y%m%d_%H%M%S", &lt);
    std::wstring d = MigotoPort::DumpDir() + L"\\" + ts;
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

} // namespace

void ShaderCapture::Note(uint64_t hash, const void* code, size_t code_size, const char* stage)
{
    if (!code || code_size == 0 || code_size > 10u * 1024u * 1024u) return;
    std::lock_guard lock(cache_mutex_);
    if (cache_.count(hash)) return; // already cached
    Cached cs;
    cs.stage[0] = stage[0];
    cs.stage[1] = stage[1];
    cs.stage[2] = 0;
    cs.stage[3] = 0;
    cs.code.resize(code_size);
    memcpy(cs.code.data(), code, code_size);
    cache_[hash] = std::move(cs);
}

size_t ShaderCapture::Count()
{
    std::lock_guard lock(cache_mutex_);
    return cache_.size();
}

bool ShaderCapture::Find(uint64_t hash, std::vector<uint8_t>* code, std::string* stage)
{
    std::lock_guard lock(cache_mutex_);
    const auto it = cache_.find(hash);
    if (it == cache_.end()) return false;
    *code = it->second.code;
    *stage = it->second.stage;
    return true;
}

void ShaderCapture::Dump()
{
    std::wstring sub = DumpSubdir() + L"\\shaders";
    CreateDirectoryW(sub.c_str(), nullptr);
    std::vector<std::pair<uint64_t, Cached>> snapshot;
    {
        std::lock_guard lock(cache_mutex_);
        for (auto& kv : cache_) snapshot.emplace_back(kv.first, kv.second);
    }
    for (auto& kv : snapshot) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\shader_%S_%016llx.bin", sub.c_str(), kv.second.stage, kv.first);
        FILE* f = nullptr;
        _wfopen_s(&f, path, L"wb");
        if (f) {
            fwrite(kv.second.code.data(), 1, kv.second.code.size(), f);
            fclose(f);
        }
        logger::write("Shader dump: %s %016llx -> %u bytes -> %S", kv.second.stage, kv.first,
            (uint32_t)kv.second.code.size(), path);
    }
}

void ShaderCapture::DiffAdd(uint64_t h, char stage, uint64_t partner, uint32_t index_count)
{
    if (!h) return;
    auto it = diff_now_.try_emplace(h, Seen{ stage, 0, 0, partner }).first;
    ++it->second.draws;
    if (index_count > it->second.max_indices) it->second.max_indices = index_count;
}

void ShaderCapture::DiffNote(reshade::api::command_list* cmd_list, uint64_t vs_hash, uint32_t index_count)
{
    if (g_diag_arms.shaderdiff_arm.load(std::memory_order_relaxed) <= 0) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    std::lock_guard l(diff_mutex_);
    DiffAdd(vs_hash, 'v', ps_hash, index_count);
    DiffAdd(ps_hash, 'p', vs_hash, index_count);
}

void ShaderCapture::DiffFinish()
{
    std::lock_guard l(diff_mutex_);
    if (!diff_have_base_) {
        diff_base_.clear();
        for (const auto& kv : diff_now_) diff_base_.insert(kv.first);
        diff_have_base_ = true;
        logger::write("SHADERDIFF baseline: %zu shaders drew in 60 frames -- press D14 again in the other state",
            diff_base_.size());
    } else {
        unsigned fresh = 0;
        for (const auto& [h, s] : diff_now_) {
            if (diff_base_.count(h)) continue;
            ++fresh;
            logger::write("SHADERDIFF new %cs=%016llx draws=%u max_indices=%u with %cs=%016llx", s.stage,
                (unsigned long long)h, s.draws, s.max_indices, s.stage == 'v' ? 'p' : 'v',
                (unsigned long long)s.partner);
        }
        logger::write("SHADERDIFF: %u of %zu shaders drew only in the second state", fresh, diff_now_.size());
        diff_have_base_ = false;
    }
    diff_now_.clear();
}

void ShaderCapture::DiffTick()
{
    if (g_diag_arms.shaderdiff_arm.load() > 0 && g_diag_arms.shaderdiff_arm.fetch_sub(1) == 1) DiffFinish();
}
