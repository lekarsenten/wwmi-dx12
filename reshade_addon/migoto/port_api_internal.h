#pragma once
#include "port_api.h"
#include <DirectXMath.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

class HackerDevice;
struct TextureOverride;
class ExplicitCommandListSection;
class CustomResource;

namespace PortApi {

// The one device/context pair their engine runs against, and whether a config is loaded (port_api.cpp).
extern HackerDevice g_dev;
extern FrameAnalysisContext g_ctx;
extern bool g_loaded;
extern unsigned g_generation; // bumped on every (re)load

// Their engine assumes the immediate context's single thread; every call in holds this.
std::recursive_mutex& EngineLock();

// Parse-time snapshots and indexes (port_parse_index.cpp). Rebuilt after every parse, read lock-free.
void RebuildParseSnapshots();
bool OverrideHashKnown(uint64_t h);
bool TextureOverrideHashKnown(uint32_t h); // true before the first parse: unknown, ask the engine
std::unordered_map<std::wstring, TextureOverride*>& SectionIndex();
bool TextureOverrideAssignsThis(TextureOverride* to);
void TextureOverrideReadVars(TextureOverride* to, std::vector<const float*>& vars);
std::unordered_map<std::wstring, ExplicitCommandListSection*>& ExplicitIndex();
std::unordered_map<const void*, std::wstring>& ExplicitNameIndex();
std::unordered_map<const void*, std::wstring>& CustomResourceNameIndex();

// Each extra copy drawn this frame gets its mod's [CommandListUpdateMergedSkeleton] run on its own state (port_skel_copies.cpp).
void RunSkelCopyUpdates();

// A .dds the background loader has not published yet, loaded now: a copy or a compute bind is about to read it
// (port_vars.cpp).
void LoadDeferredNow(CustomResource* r);
// ... and one a draw's slot is about to ask for, offered to the deferral sink if not loaded yet (port_vars.cpp).
void DeferTexture(CustomResource* r);
// ... and whether the background loader still has it.
bool DeferredPending(CustomResource* r);

// Every variable's value, the ini params and each custom shader's runs this frame, put back after a run that is to
// leave none of its writes (port_vars.cpp). Caller holds the engine lock across both.
struct EngineValues {
    std::vector<float> vars;
    std::vector<DirectX::XMFLOAT4> params;
    std::vector<std::pair<unsigned, int>> shader_runs; // frame_no, executions_this_frame
    void Take();
    void PutBack() const;
};

} // namespace PortApi
