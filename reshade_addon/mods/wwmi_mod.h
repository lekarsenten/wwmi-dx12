#pragma once
// wwmi_mod -- the per-mod data the DX12 runtime drives a WWMI mod with (filled by wwmi_mod_engine.cpp
// from the loaded engine's sections).
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <d3d12.h>
#include "../migoto/port_api.h"

namespace WwmiMod {

struct SubDraw {
    uint32_t count;
    uint32_t start;
    std::wstring cond;
};

struct Component {
    int index = -1; // N in [TextureOverrideComponentN]; a part named otherwise is numbered after them
    std::wstring tail; // the section's name under its namespace: Component0, Component0_ib0, ...
    uint32_t ib_hash = 0; // section `hash =` (the game IB's 3dmigoto hash) -- DX11 scopes
    // match_first_index to THIS; used for (ib_hash, first_index) matching
    uint32_t match_first_index = 0;
    uint32_t match_index_count = 0;
    int match_priority = 0;
    uint32_t vg_offset = 0; // $\WWMIv1\vg_offset (merge destination)
    uint32_t vg_count = 0; // $\WWMIv1\vg_count (merge source count)
    bool remapped = false; // true if its RemappedSkeletonComponentN is actually produced
    int remap_id = -1; // blend_remap_id (if remapped); else -1 (binds MergedSkeleton)
    uint32_t remap_vg = 0; // vg_count used by the remap dispatch (if remapped)
    std::vector<SubDraw> draws;
};

struct Resource {
    std::wstring name; // e.g. "ResourcePositionBuffer" (their key, with the prefix restored)
    std::wstring fq; // their fully-qualified key, e.g. "resource\\somemod\\positionbuffer"
    std::wstring filename; // absolute, as their loader resolved it (empty if not from a file)
    uint32_t stride = 0; // their parsed `stride =`
    ID3D12Resource* res = nullptr;
    uint64_t gpu_va = 0;
    uint32_t size = 0; // their D3D11 ByteWidth, not the 256-aligned DX12 width
};

struct Mod {
    std::wstring dir; // mod folder (for resolving filenames)
    std::vector<Component> comps;
    std::vector<Resource> resources;
    // forward-map file (BlendRemapForward.buf) name, if present, for the remapper.
    std::wstring forward_map_file;
    int object_guid = 0;
    uint32_t mesh_vertex_count = 0;
    bool queries_ps_slots = false;
};

struct NsEntries {
    std::vector<std::wstring> ranged_overrides;
    std::vector<Resource> resources;
};

std::unordered_map<std::wstring, NsEntries> GroupByNamespace();
int ReadMod(const std::wstring& ns, const std::unordered_map<std::wstring, NsEntries>& groups, Mod& out);
int ReadComponentsFromEngine(const std::wstring& ns, Mod& out);

int LoadResources(Mod& m, std::vector<PortApi::HeldBuffer>& held);

// Convenience: find a loaded resource by name (case-insensitive), or nullptr.
const Resource* Find(const Mod& m, const wchar_t* name);

// Log a human-readable summary of a mod's layout to the addon log.
void LogSelfCheck(const Mod& m);

} // namespace WwmiMod
