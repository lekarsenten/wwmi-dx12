#pragma once

#include <d3d12.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

class FrameAnalysisContext;
struct ID3D11Resource;

namespace PortApi {

bool Load(const wchar_t* ini_root);
void SaveSettingsOnExit();

void SetFrameContext(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* queue,
    unsigned rt_width, unsigned rt_height,
    DXGI_FORMAT rt_format, DXGI_FORMAT ds_format,
    ID3D12Resource* back_buffer, D3D12_RESOURCE_STATES back_buffer_state,
    void (*restore_game_state)(void*), void*           restore_user = nullptr);

class DrawListScope {
public:
    DrawListScope(ID3D12GraphicsCommandList* cl, void (*restore_game_state)(void*), void* restore_user,
        int (*vs_cb_root)(void*, unsigned) = nullptr, void* vs_cb_root_user = nullptr);
    ~DrawListScope();
    DrawListScope(const DrawListScope&) = delete;
    DrawListScope& operator=(const DrawListScope&) = delete;
};

void PresentPre();
void PresentPost();

// Their Input.cpp: edge-detects every registered [Key] binding, including the XInput ones.
void TickInput();

unsigned TakeRefusedNoCl();

void SetKeyGate(bool (*fn)(const wchar_t* ns));

struct DrawInfo {
    unsigned vertex_count = 0;
    unsigned index_count = 0;
    unsigned instance_count = 0;
    unsigned first_vertex = 0;
    unsigned first_index = 0;
    unsigned first_instance = 0;
    bool     indexed = true;
    bool     dispatch = false;
    unsigned thread_group_x = 0;
    unsigned thread_group_y = 0;
    unsigned thread_group_z = 0;
    bool     skip = false;

    bool     is_pixel_shader = false;
    uint64_t partner_shader_hash = 0;
    bool     depth_active = false;
    bool     depth_known = false; // false = caller did not say; the filter then does nothing
    uint32_t srv_gen_at_entry = 0;
    uint64_t pso = 0;
};

// [ShaderOverride] dispatch by shader hash. Returns true if a section matched and ran.
bool DispatchShaderOverride(uint64_t shader_hash, DrawInfo* draw);

void RunPendingPost();

// [ShaderOverride] membership.
bool ShaderOverrideMatches(uint64_t shader_hash);
// The section that first declared this shader hash (their first_ini_section), for identification.
void ShaderOverrideSection(uint64_t shader_hash, wchar_t* out, size_t out_chars);

void SetBoundShader(char stage, uint64_t ini_hash);

float BoundShaderFilter(char stage, uint64_t* out_hash, int* out_have_handle, int* out_in_mshaders);

void RegisterShaderHash(uint64_t ini_hash);

// A shader the add-on patches itself in place of [ShaderRegex<group>] of namespace `ns`: that group's filter_index
// and command lists are linked to it, as their regex match links them (filter_index for `ps == x`).
bool LinkShaderRegexGroup(uint64_t ini_hash, const wchar_t* ns, const wchar_t* group);
bool ShaderRegexLinked(uint64_t ini_hash);
bool ShaderRegexGroupLoaded(const wchar_t* ns, const wchar_t* group);

int DispatchTextureOverride(uint32_t texture_hash, DrawInfo* draw);

bool RunSectionByName(const wchar_t* fq_section, DrawInfo* draw);
bool RunSectionThenPost(const wchar_t* fq_section, DrawInfo* draw);

// Does a section of that name exist? The name index behind it is built once per load generation.
bool HasSection(const wchar_t* fq_section);

bool GetSectionValue(const wchar_t* fq_section, const wchar_t* key, wchar_t* out, size_t out_chars);

void ForEachSectionKey(const wchar_t* fq_section,
    void (*cb)(const wchar_t* key, const wchar_t* val, void* user), void* user);

void ForEachSectionLine(const wchar_t* fq_section, void (*cb)(const wchar_t* line, void* user), void* user);
// Inspect parsed if/elif expressions, including nested and linked command lists.
bool SectionReadsShaderConditions(const wchar_t* fq_section);

void ForEachSectionInNs(const wchar_t* ns, void (*cb)(const wchar_t* fq_section, void* user), void* user);

bool SectionFolder(const wchar_t* fq_section, wchar_t* out, size_t out_chars);

void GetIniParams(float* out_rgba, int texels);
bool GetIniParam(unsigned texel, float out_rgba[4]);

void PushGameConstantBuffer(char stage, unsigned slot, ID3D12Resource* res,
    unsigned long long gpu_va, unsigned dx11_byte_width);

void PushGameConstantBufferRecord(char stage, unsigned slot, ID3D12Resource* res,
    unsigned long long gpu_va, unsigned dx11_byte_width);
bool PipeStateCbBound(char stage, unsigned slot);

int PushGameTexture(char stage, unsigned slot, ID3D12Resource* res, unsigned content_hash, unsigned view_format = 0);
// Clears <stage>-tN only while it still holds what PushGameTexture put there.
void UnpushGameTexture(char stage, unsigned slot);
// Some [TextureOverride] section is keyed on this hash (true before the first parse).
bool TextureHashHasSections(uint32_t hash);

void PushGameVertexBuffer(unsigned slot, ID3D12Resource* res, unsigned long long gpu_va,
    unsigned dx11_byte_width, unsigned stride, unsigned offset);

void PushGameIndexBuffer(ID3D12Resource* res, unsigned long long gpu_va_base, unsigned dx11_byte_width,
    unsigned index_bytes, unsigned offset);

bool PipeStateVbBound(unsigned slot);

enum PushUavResult {
    kPushUavPublished = 0,   // bound at cs-uN; `cs-u0` is now answerable
    kPushUavCleared = 1,     // res was null: slot cleared, as asked
    kPushUavOverMod = 2,     // published OVER a mod's UAV an earlier list left there
    kPushUavNoContext = 3,   // no pass-through context: the setter would have done nothing
    kPushUavNotLoaded = 4,   // ini engine not loaded
    kPushUavBadSlot = 5,     // slot >= kMaxUAV
};
// PushGameTexture's exits.
enum PushTexResult {
    kPushTexPublished = 0, // bound at <stage>-tN
    kPushTexOverMod = 1,   // published OVER a mod's `ps-tN =` bind: on DX12 the game's PSO never read it
    kPushTexNotLoaded = 2, // ini engine not loaded
    kPushTexBadSlot = 3,   // unknown stage, or slot >= kMaxSRV
};
unsigned BridgeStackDepth();

unsigned GameBufferHashSearch(unsigned byte_width, unsigned ini_stride, bool* out_ambiguous);

void ForEachTextureOverrideConfig(void (*cb)(unsigned hash, int byte_width, int num_elements,
                                      int match_priority, const wchar_t* section, void* user),
    void* user);
bool AllowBufferResize();

int PushGameComputeUav(unsigned slot, ID3D12Resource* res, unsigned byte_width, unsigned long long gpu_va);

bool GetShimVertexBuffer(unsigned slot, void** out_res, unsigned long long* out_va,
    unsigned* out_size, unsigned* out_stride, unsigned* out_state);

unsigned GameComputeUavBufferHash(unsigned byte_width);

bool ModDeclaresHash(const wchar_t* ns, unsigned hash);

bool ListNests(const void* outer_section, const void* inner_section);

void ForEachBufferResize(void (*cb)(unsigned hash, unsigned stride, unsigned count, const wchar_t* section,
                             void* user),
    void* user);

unsigned ModDeclaredShapeKeyHash(const wchar_t* ns);

unsigned ModDeclaredShapeKeySectionHash(const wchar_t* ns, const wchar_t* which);

bool TextureOverrideDeclaresHash(unsigned hash);

bool ShapeKeyCallbackDeclaresHash(unsigned hash);

void PushGameDepthTarget(ID3D12Resource* res);

FrameAnalysisContext* FrameAnalysisCtx();

void PushGameRenderTarget(unsigned slot, ID3D12Resource* res);

class EngineScope {
public:
    EngineScope();
    ~EngineScope();
    EngineScope(const EngineScope&) = delete;
    EngineScope& operator=(const EngineScope&) = delete;
};

class SkelCopyScope {
public:
    SkelCopyScope(const wchar_t* ns, unsigned copy);
    ~SkelCopyScope();
    SkelCopyScope(const SkelCopyScope&) = delete;
    SkelCopyScope& operator=(const SkelCopyScope&) = delete;

private:
    void* swapped_ = nullptr;
};

unsigned GameConstantBufferHash(unsigned dx11_byte_width);

unsigned Crc32c(unsigned seed, const void* data, size_t n);

bool NamespaceForIni(const wchar_t* abs_ini_path, wchar_t* out, size_t out_chars);
void ForEachModIni(void (*cb)(const wchar_t* abs_ini, const wchar_t* ns, const wchar_t* abs_root, void* user),
    void* user);

bool ResolveSection(const wchar_t* ns, const wchar_t* ns_hint2, const wchar_t* section,
    wchar_t* out, size_t out_chars);

struct ModDraw {
    unsigned index_count = 0;
    unsigned start_index = 0;
    int      base_vertex = 0;
    unsigned instance_count = 1;
    unsigned start_instance = 0;
    bool     indexed = true;
};
void SetDrawCallback(void (*fn)(const ModDraw&, void* user), void* user);

void SetCommandObserver(void (*fn)(const wchar_t* ini_line, void* user), void* user);

struct ObservedCommand {
    enum class Kind { Other,
        RunList,
        ResourceCopy };
    Kind           kind = Kind::Other;
    bool           post = false;
    const wchar_t* run_target = nullptr; // RunList: THEIR map key, reversed out of their own index
    const void*    run_explicit = nullptr;
    bool           dst_is_custom = false; // ResourceCopy: dst is a [Resource*] section
    bool           dst_is_srv = false;    // ResourceCopy: dst is a shader-resource slot (ps-tN / vs-tN)
    bool           dst_is_vb = false;
    bool           dst_is_ib = false;
    bool           dst_is_this = false; // ResourceCopy: `this = ...`, into the slot a texture check is looking at
    char           dst_shader = 0;      // 'p', 'v', 'c', ... when dst_is_srv
    unsigned       dst_slot = 0;
    const wchar_t* dst_resource = nullptr;
    bool           src_is_custom = false;
    const wchar_t* src_resource = nullptr; // null for `= null` (an unbind, which is meaningful)
    const void*    src_custom = nullptr;
};
void SetTypedObserver(void (*fn)(const ObservedCommand&, void* user), void* user);

void RunResolvedCommandList(const void* explicit_section, DrawInfo* draw);

ID3D12Resource* SelectTextureOverride(uint32_t texture_hash, bool* cacheable = nullptr);
ID3D12Resource* SelectTextureOverrideNs(uint32_t texture_hash, const wchar_t* ns, bool* cacheable = nullptr);
bool            TextureOverrideInNs(uint32_t texture_hash, const wchar_t* ns);
uint32_t        SwitchOfResource(const wchar_t* res_fq);
// Every .dds [Resource*] of this namespace not yet loaded; and load one now, as its first assignment would.
void ForEachUnsubstantiatedTexture(const wchar_t* ns, void (*cb)(const wchar_t* fq_name, const wchar_t* filename, void* user),
    void*                                         user);
bool SubstantiateTexture(const wchar_t* fq_name);
struct DeferredTexture {
    std::wstring fq_name;
    std::wstring filename;
    unsigned     bind_flags;
    unsigned     misc_flags;
    bool         force_srgb;
};
void            BeginDeferredTextures(const wchar_t* ns, std::vector<DeferredTexture>& out);
ID3D11Resource* LoadDeferredTexture(const DeferredTexture& t);
bool            PublishDeferredTexture(const wchar_t* fq_name, ID3D11Resource* res);

void TextureOverrideSection(uint32_t texture_hash, wchar_t* out, size_t out_chars);

void ForEachTextureOverride(void (*cb)(uint32_t hash, const wchar_t* fq_section,
                                bool           has_match_first_index,
                                const wchar_t* mod_folder, void* user),
    void* user);

void ForEachSkipTextureOverride(void (*cb)(uint32_t hash, const wchar_t* fq_section, void* user), void* user);

float GetVar(const wchar_t* fq_name, float def);
float GetVarNs(const wchar_t* ns, const wchar_t* name, float def);
bool  SetVar(const wchar_t* fq_name, float value);
bool  SetVarNs(const wchar_t* ns, const wchar_t* name, float value);
// The variable's storage, for a caller that writes it often: valid until LoadGeneration() changes.
float* VarSlotNs(const wchar_t* ns, const wchar_t* name);
void   SetVarSlot(float* slot, float value);
bool   VarSlotHolds(const float* slot, unsigned gen, float value);

void ForEachVariable(void (*cb)(const wchar_t* fq_name, float value, bool persist, void* user), void* user);

ID3D12Resource* FindCustomResource(const wchar_t* fq_name);
ID3D12Resource* CustomResourceDx12(const void* custom);

struct BufferResourceInfo {
    ID3D12Resource*    dx12 = nullptr;
    unsigned long long gpu_va = 0;
    unsigned           byte_width = 0; // their D3D11_BUFFER_DESC.ByteWidth -- the real size
    unsigned           stride = 0;     // their `stride =`, else the desc's StructureByteStride
};
bool GetBufferResource(const wchar_t* fq_name, BufferResourceInfo* out);
size_t BufferInitialData(const wchar_t* fq_name, void* out, size_t cap);
bool   UploadBufferResource(const wchar_t* fq_name, const void* data, size_t bytes);

void* AcquireBufferObject(const wchar_t* fq_name);

void ForEachCustomResource(void (*cb)(const wchar_t* fq_name, const wchar_t* filename, int type,
                               bool substantiated, ID3D12Resource* dx12, void* user),
    void* user,
    bool (*want)(ID3D12Resource* dx12, void* user) = nullptr);

// ---- what got loaded, for the overlay and for tests -------------------------------------------
struct Stats {
    size_t command_lists = 0;
    size_t custom_shaders = 0;
    size_t custom_resources = 0;
    size_t texture_overrides = 0;
    size_t shader_overrides = 0;
    int    ini_params = 0;
    bool   loaded = false;
};
unsigned LoadGeneration();
void     CommandCostMark();
void     CommandCostReport(FILE* f, int frames);
// Every resource wrapper the shim has made. A new one can be a newly substantiated [Resource*].
long WrapResourcesCreated();
// Wrappers made plus wrappers released: moves whenever a resource the engine can name appears or goes.
long     WrapChurn();
unsigned SelectionInputsGen();
// False while frame analysis or a command observer wants every selection run in full.
bool SelectionCacheOk();

Stats GetStats();

struct GpuWorkTotals {
    unsigned iniparams_updates = 0;
    unsigned full_copies = 0;
    unsigned reference_copies = 0; // every non-copy ResourceCopyOperation: binds AND `ref`
    unsigned region_copies = 0;
    unsigned inter_device_copies = 0;
    unsigned msaa_resolutions = 0;
    unsigned views_cleared = 0;
    unsigned resources_created = 0;
    unsigned pool_swaps = 0;
    unsigned max_copies_exceeded = 0;
    unsigned injected_draws = 0; // draw AND dispatch commands (CommandList.cpp DrawCommand::run)
    unsigned max_executions_exceeded = 0;
    unsigned replay_top_replayed = 0; // a section's list run from its recorded trace
    unsigned replay_top_recorded = 0; //   run by theirs while recording one
    unsigned replay_top_plain = 0;    //   not replayable: run by theirs
    unsigned replay_sub_replayed = 0; // the same for `run = CommandList...` targets
    unsigned replay_sub_recorded = 0;
    unsigned replay_sub_plain = 0;
};
void GetGpuWorkTotals(GpuWorkTotals* out);
void ReplayCounts(unsigned* top_replayed, unsigned* top_recorded, unsigned* top_plain,
    unsigned* sub_replayed, unsigned* sub_recorded, unsigned* sub_plain);

} // namespace PortApi
