#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <reshade.hpp>
#include "../migoto/port_api.h" // PortApi::DrawInfo

extern thread_local uint64_t t_cur_vs_hash;
extern thread_local uint64_t t_cur_ps_hash;

// The hash a stage publishes as bound: the shader's own while any [ShaderOverride] has it, else its DX11 partner.
uint64_t AliasShaderHash(uint64_t h);
uint64_t AliasShaderHashAlways(uint64_t h);

// Does any [ShaderOverride*] target this hash? Optionally returns the first section name.
bool ShaderOverrideMatches(uint64_t shader_hash, std::wstring* first_section);
// Run them: the sections of the shader's own hash, then those of its DX11 partner. True means a section cancelled the
// game's own draw or dispatch.
bool RunShaderOverrides(
    uint64_t shader_hash, const PortApi::DrawInfo* draw = nullptr, reshade::api::command_list* cmd_list = nullptr);
#ifdef WWMI_DEBUG
// The debug pipe's `alias`: lists every alias, or sets / removes one looked up ahead of the .shaderpairs files.
std::string ShaderAliasCommand(const std::vector<std::string>& args);
// The mesh a .shaderpairs line paired this shader on (a component section's `hash =`); 0 when it names none.
uint32_t ShaderPairMesh(uint64_t dx12);
#endif
