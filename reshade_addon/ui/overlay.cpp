// overlay.cpp -- see overlay.h. The frame, the MASTER section, the id-carrying controls and the dock.
#include <imgui.h>
#include "overlay.h"
#include "panel_registry.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/addon.h"
#include "../addon/settings.h"
#include "../mods/mod_registry.h"
#include "../present/present_toggles.h"
#include "../textures/tex_toggles.h"
#include "../build_stamp.h"
#include "../textures/tex_place.h"
#include "../textures/tex_lod.h" // TexLodCache::SetResemblance (A13), SetCache (A14)
#include "../textures/tex_crawl.h" // the gallery crawl (A15)
#include "../textures/tex_gc.h" // the video memory reading under A19
#include "../hooks/stamp_ledger.h" // SetAddonDisabled (A1)
#include "../migoto/port_toggles.h" // g_buffers_in_vram
#include "../migoto/port_api.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "../mods/mod_set.h"

using namespace reshade::api;

OverlayPanel g_overlay;

static void IdLabel(char* buf, size_t n, const char* id, const char* label)
{
    static std::unordered_map<std::string, std::string> s_seen; // id -> first label that claimed it
    static std::set<std::string> s_reported;
    auto it = s_seen.find(id);
    if (it == s_seen.end())
        s_seen.emplace(id, label ? label : "");
    else if (it->second != (label ? label : "") && s_reported.insert(id).second)
        logger::write("OVERLAY WARNING: duplicate control id '%s' -- \"%s\" and \"%s\". "
                      "Referring to it by id is now ambiguous.",
            id, it->second.c_str(), label ? label : "");
    snprintf(buf, n, "%s. %s", id, label);
}
bool NCheckbox(const char* id, const char* label, bool* v)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    ImGui::PushID(id);
    bool changed = ImGui::Checkbox("##v", v);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemClicked()) {
        *v = !*v;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}
bool NButton(const char* id, const char* label)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    const bool pressed = ImGui::Button(id);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    return pressed;
}
bool NSliderInt(const char* id, const char* label, int* v, int lo, int hi, const char* fmt)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PushID(id);
    const bool changed = ImGui::SliderInt("##v", v, lo, hi, fmt);
    ImGui::PopID();
    return changed;
}
bool NSliderFloat(const char* id, const char* label, float* v, float lo, float hi, const char* fmt)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PushID(id);
    const bool changed = ImGui::SliderFloat("##v", v, lo, hi, fmt);
    ImGui::PopID();
    return changed;
}
bool NInputInt(const char* id, const char* label, int* v)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::InputInt(buf, v);
}
bool NInputText(const char* id, const char* label, char* v, size_t n, ImGuiInputTextFlags f)
{
    char buf[352];
    IdLabel(buf, sizeof(buf), id, label);
    return ImGui::InputText(buf, v, n, f);
}

struct OverlayWrapScope {
    OverlayWrapScope() { ImGui::PushTextWrapPos(0.0f); }
    ~OverlayWrapScope() { ImGui::PopTextWrapPos(); }
};

static const char kRtBlockedText[] =
    "Ray tracing is ON in the game, and this version does not support it: mods are OFF.\n"
    "Turn ray tracing off in the game's graphics settings, then restart the game.";

static const char kFaultedText[] = "The add-on hit an error and stopped: mods are OFF until the game restarts.\n"
                                   "Please report it with wwmi_dx12_crash.log from the game's Win64 folder.";

const char* OverlayPanel::StoppedText()
{
    return g_addon.faulted ? kFaultedText : kRtBlockedText;
}

namespace {

constexpr ImGuiID kPanelSplitNode = 0x57570D01;
constexpr ImGuiID kPanelNode = 0x57570D02;
constexpr const char* kPanelWindow = "WWMI-DX12###wwmi-dx12-panel";
constexpr const char* kPanelSettings = "[Window][###wwmi-dx12-panel]";

std::vector<std::string> ConfigLines(const char* section, const char* key)
{
    size_t size = 0;
    if (!reshade::get_config_value(nullptr, section, key, nullptr, &size) || size < 2) return {};
    std::string value(size, '\0');
    if (!reshade::get_config_value(nullptr, section, key, value.data(), &size)) return {};
    value.resize(size < value.size() ? size : value.size());
    std::vector<std::string> out;
    for (size_t at = 0; at < value.size();) {
        size_t end = value.find('\0', at);
        if (end == std::string::npos) end = value.size();
        if (end > at) out.emplace_back(value, at, end - at);
        at = end + 1;
    }
    return out;
}

void SetConfigLines(const char* section, const char* key, const std::vector<std::string>& lines)
{
    std::string joined;
    for (const std::string& l : lines) {
        joined += l;
        joined += '\0';
    }
    reshade::set_config_value(nullptr, section, key, joined.c_str(), joined.size());
}

struct DockLine {
    size_t index = 0;
    unsigned id = 0;
    unsigned parent = 0;
    int w = 0;
    int h = 0;
    bool central = false;
    bool split_x = false;

    bool Read(const std::string& s, size_t i)
    {
        const char* p = s.c_str();
        while (*p == ' ') ++p;
        if (strncmp(p, "DockNode", 8) != 0 && strncmp(p, "DockSpace", 9) != 0) return false;
        index = i;
        if (const char* f = strstr(p, " ID=0x")) sscanf_s(f, " ID=0x%X", &id);
        if (const char* f = strstr(p, "Parent=0x")) sscanf_s(f, "Parent=0x%X", &parent);
        if (const char* f = strstr(p, "SizeRef=")) sscanf_s(f, "SizeRef=%d,%d", &w, &h);
        central = strstr(p, "CentralNode=1") != nullptr;
        split_x = strstr(p, "Split=X") != nullptr;
        return id != 0;
    }
};

const DockLine* FindDock(const std::vector<DockLine>& nodes, unsigned id)
{
    for (const DockLine& d : nodes)
        if (d.id == id) return &d;
    return nullptr;
}

std::string ReplaceField(std::string line, const char* field, const std::string& value)
{
    const size_t at = line.find(field);
    if (at == std::string::npos) return line;
    size_t end = line.find(' ', at);
    if (end == std::string::npos) end = line.size();
    return line.replace(at, end - at, std::string(field) + value);
}

unsigned WindowDockNode(const std::vector<std::string>& windows, const char* header)
{
    unsigned node = 0;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i] != header) continue;
        for (size_t j = i + 1; j < windows.size() && windows[j].rfind("[Window][", 0) != 0; ++j)
            sscanf_s(windows[j].c_str(), "DockId=0x%X", &node);
    }
    return node;
}

void PointPanelAt(std::vector<std::string>& windows, unsigned node)
{
    char dock[48];
    std::snprintf(dock, sizeof dock, "DockId=0x%08X,0", node);
    for (size_t i = 0; i < windows.size(); ++i) {
        if (windows[i] != kPanelSettings) continue;
        size_t j = i + 1;
        for (; j < windows.size() && windows[j].rfind("[Window][", 0) != 0; ++j)
            if (windows[j].rfind("DockId=", 0) == 0) {
                windows[j] = dock;
                return;
            }
        windows.insert(windows.begin() + j, dock);
        return;
    }
    windows.push_back(kPanelSettings);
    windows.push_back("Collapsed=0");
    windows.push_back(dock);
}

const char* SplitBesideReShade(std::vector<std::string>& docking, unsigned home_node)
{
    if (docking.empty() || docking[0] != "[Docking][Data]") return "no saved docking";
    std::vector<DockLine> nodes;
    for (size_t i = 0; i < docking.size(); ++i) {
        DockLine d;
        if (d.Read(docking[i], i)) nodes.push_back(d);
    }
    if (FindDock(nodes, kPanelNode)) return nullptr;
    const DockLine* home = FindDock(nodes, home_node);
    const DockLine* central = nullptr;
    for (const DockLine& d : nodes)
        if (d.central) central = &d;
    if (!home || !central || !central->parent || home->parent != central->parent)
        return "ReShade's tabs are not beside the free area";
    const DockLine* split = FindDock(nodes, central->parent);
    if (!split || !split->split_x) return "ReShade's tabs are not left or right of the free area";
    const int panel_w = home->w;
    const int rest_w = central->w - panel_w;
    if (panel_w < 100 || rest_w < 200) return "the free area is too narrow";
    std::string rest =
        ReplaceField(docking[central->index], "SizeRef=", std::to_string(rest_w) + "," + std::to_string(central->h));
    char parent[16];
    std::snprintf(parent, sizeof parent, "0x%08X", kPanelSplitNode);
    rest = ReplaceField(rest, "Parent=", parent);
    char outer[160];
    std::snprintf(outer, sizeof outer, "  DockNode  ID=0x%08X Parent=0x%08X SizeRef=%d,%d Split=X", kPanelSplitNode,
        central->parent, central->w, central->h);
    char panel[160];
    std::snprintf(panel, sizeof panel, "    DockNode  ID=0x%08X Parent=0x%08X SizeRef=%d,%d", kPanelNode,
        kPanelSplitNode, panel_w, central->h);
    const bool reshade_left = home->index < central->index;
    std::vector<std::string> out(docking.begin(), docking.begin() + central->index);
    out.push_back(outer);
    out.push_back(reshade_left ? rest : std::string(panel));
    out.push_back(reshade_left ? std::string(panel) : rest);
    out.insert(out.end(), docking.begin() + central->index + 1, docking.end());
    docking.swap(out);
    return nullptr;
}

} // namespace

void OverlayPanel::DockOnce()
{
    int docked = 0;
    if (reshade::get_config_value(nullptr, "wwmi-dx12", "PanelDocked", docked) && docked) return;
    std::vector<std::string> windows = ConfigLines("OVERLAY", "Window");
    std::vector<std::string> docking = ConfigLines("OVERLAY", "Docking");
    const unsigned home_node = WindowDockNode(windows, "[Window][###home]");
    const char* why = home_node ? SplitBesideReShade(docking, home_node) : "ReShade's window is not docked";
    if (why) {
        logger::write(
            "PANELDOCK: the add-on's window is not docked (%s) -- it opens on the right, placed by hand", why);
        return;
    }
    PointPanelAt(windows, kPanelNode);
    SetConfigLines("OVERLAY", "Docking", docking);
    SetConfigLines("OVERLAY", "Window", windows);
    reshade::set_config_value(nullptr, "wwmi-dx12", "PanelDocked", 1);
    logger::write(
        "PANELDOCK: the add-on's window docked on the side of the free area away from ReShade's tabs -- once; "
        "a later move is kept");
}

void OverlayPanel::DrawPanel(effect_runtime* runtime)
{
    if (!open_.load(std::memory_order_relaxed)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x * 0.27f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w - 8.f, 8.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, io.DisplaySize.y - 16.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(kPanelWindow)) Draw(runtime);
    ImGui::End();
}

void DockOverlayPanelOnce()
{
    g_overlay.DockOnce();
}

bool OnOverlayOpen(effect_runtime*, bool open, input_source)
{
    g_overlay.SetOpen(open);
    return false;
}

void DrawOverlayPanel(effect_runtime* runtime)
{
    ModSetRef set_ref;
    g_overlay.DrawPanel(runtime);
}

void DrawStoppedNotice(effect_runtime*)
{
    ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "wwmi-dx12: %s", OverlayPanel::StoppedText());
}

void OverlayPanel::Draw(effect_runtime*)
{
    OverlayWrapScope wrap_every_line;
    ImGui::Text("wwmi-dx12 %s (build %s)", WWMI_VERSION, WWMI_BUILD_SHA);
#ifdef WWMI_DEBUG
    ImGui::TextDisabled("Compiled: %s %s", __DATE__, __TIME__);

    {
        if (g_frame_id == live_frame_) {
            if (live_stalls_ < 1000) ++live_stalls_;
        } else {
            live_stalls_ = 0;
        }
        live_frame_ = g_frame_id;
        const bool live = (live_stalls_ < 30); // ~30 overlay frames with no present tick = not just paused
        ImGui::TextColored(live ? ImVec4{ 0.6f, 1, 0.6f, 1 } : ImVec4{ 1, 0.4f, 0.4f, 1 },
            "on_present frame: %u %s | log level: %d%s", g_frame_id,
            live ? "(live)" : "(STALLED -- present hook not running)", logger::g_level,
            logger::g_level == 0 ? "  <- silent by design; tick A2 to log" : "");
    }
#endif

    ImGui::Separator();
    if (g_addon.rt_blocked || g_addon.faulted) {
        ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "%s", StoppedText());
        return;
    }
    if (!DrawMaster()) return;
    for (const PanelEntry& p : kOverlayPanels) p.draw();
}

bool OverlayPanel::DrawMaster()
{
#ifdef WWMI_DEBUG
    ImGui::TextColored({ 1, 1, 0.6f, 1 }, "*** MASTER ***");
#endif
    // Master bypass: every hot handler early-outs, for an A/B against a vanilla game with the addon loaded.
    {
        bool dis = g_addon.disabled.load();
        if (NCheckbox("A1", "Disable addon (F6 toggles)", &dis)) SetAddonDisabled(dis);
        if (dis) {
            ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "ADDON DISABLED -- no mods, no overrides, no skinning.");
            return false;
        }
    }

    if (NCheckbox("A2", "Debug log (wwmi_dx12_addon.log -- attach it to bug reports)", &g_debug)) {
        SetDiagnostics(g_debug, g_log_verbosity);
        g_settings.Save(L"debug", g_debug);
    }
    if (g_debug) {
        if (NSliderInt("A3", "Debug level (1 = events/errors, 2 = +info)", &g_log_verbosity, 1, 2)) {
            SetDiagnostics(g_debug, g_log_verbosity);
            g_settings.Save(L"log_level", g_log_verbosity);
        }
    }
    if (NCheckbox("A4", "Decensor", &g_settings.decensor)) g_settings.Save(L"decensor", g_settings.decensor);
    if (NCheckbox("A5", "Hide UID (your player number: bottom right, Esc menu, Friends list)", &g_settings.hide_uid))
        g_settings.Save(L"hide_uid", g_settings.hide_uid);
#ifdef WWMI_DEBUG
    if (NCheckbox("A8", "Hide mod outlines", &g_settings.hide_outline))
        g_settings.Save(L"hide_outline", g_settings.hide_outline);
#endif
    {
        bool lookalike = g_settings.texlod_resemblance;
        if (NCheckbox("A13",
                "Use look-alike texture matches (a game texture that looks like a mod's texture gets it, as if it had the same hash)",
                &lookalike)) {
            g_tex_lod.SetResemblance(lookalike);
            g_tex_place.LookAlikesSwitched(lookalike);
            g_settings.Save(L"lod_resemblance", lookalike);
        }
    }
    {
        bool cache = g_settings.texlod_cache;
        if (NCheckbox("A14",
                "Use texture match cache (mod textures on the game's distant copies; off: nothing is learned, read or saved)",
                &cache)) {
            g_tex_lod.SetCache(cache);
            g_settings.Save(L"lod_cache", cache);
        }
    }
    if (NCheckbox("A12", "Load mod textures in the background", &g_settings.tex_deferred))
        g_settings.Save(L"tex_background", g_settings.tex_deferred);
    ImGui::TextDisabled(
        "  on = no stutter when a modded character first shows; its own textures appear a moment later.\n"
        "  off = the mod's textures from the first frame; the game pauses briefly while they load.\n"
        "  Applies to characters that show after the change.");
    if (NSliderInt("A19", "Mod texture budget", &g_settings.tex_mod_budget, 0, kTexBudgetSteps - 1,
            TexBudgetName(g_settings.tex_mod_budget)))
        g_settings.Save(L"tex_mod_budget", g_settings.tex_mod_budget);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "  How much video memory mod textures may take. Auto keeps them within what the game leaves free: "
        "while they fit nothing changes; when memory runs short, or a texture on screen needs room to come "
        "back sharp, textures not on screen go to a lower size, the least used first, and out if that is not "
        "enough. A size caps each mod as well. Original never changes a texture.");
    {
        const TexGc::Reading& r = g_tex_gc.Now();
        if (r.valid) {
            ImGui::TextDisabled("  Video memory: %.1f of %.1f GB in use", (double)r.used / 1073741824.0,
                (double)r.budget / 1073741824.0);
            ImGui::TextDisabled("  Mod textures: %.1f of %.1f GB allowed", (double)r.mods / 1073741824.0,
                (double)r.room / 1073741824.0);
            if (g_tex_gc.Tight())
                ImGui::TextColored({ 1, 1, 0, 1 },
                    TexCollects(g_settings.tex_mod_budget)
                        ? "  Short of video memory: mod textures not on screen are let go first."
                        : "  Short of video memory: Auto would let go of mod textures not on screen.");
        }
    }
    ImGui::PopTextWrapPos();
    if (NCheckbox("A20", "Mod textures follow the game's distance sizes (experimental)", &g_settings.tex_distance))
        g_settings.Save(L"tex_distance", g_settings.tex_distance);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "  on = where the game draws a character's own texture a size or more down, as it does further from "
        "the camera, the mod's texture goes down as many sizes, and back up when the game's comes back. "
        "Saves video memory. With Auto or a size on A19.");
    ImGui::PopTextWrapPos();
    if (NCheckbox("A16", "Mod buffers (meshes, shape keys) in video memory", &MigotoPort::g_buffers_in_vram))
        g_settings.Save(L"buffers_in_vram", MigotoPort::g_buffers_in_vram);
    ImGui::TextDisabled(
        "  on = faster drawing of modded characters.\n"
        "  off = kept in system memory, for cards short on video memory; costs frame rate with large mods.\n"
        "  Applies at the next launch.");
    ImGui::Text("Active mods: %d", Mods().DiscoveredCount());
    {
        if (NButton("A11", "Clear caches at next launch (patched shaders, texture matches)") &&
            !g_settings.ini_path.empty()) {
            g_settings.Save(L"clear_cache", true);
            clear_requested_ = true;
        }
        if (clear_requested_) ImGui::TextColored({ 1, 1, 0, 1 }, "Caches will be cleared at the next launch.");
    }
#ifdef WWMI_DEBUG
    {
        bool crawl = g_tex_crawl.On();
        if (NCheckbox("A15",
                "Gallery crawl: record every character texture's mips and where each character draw binds it; saved 10 s after the last new texture (at least once a minute), when switched off and at exit",
                &crawl))
            g_tex_crawl.Set(crawl);
    }
    NCheckbox("A9", "Skip the engine tick: no [Present] lists, no present pass (perf A/B only -- mods stop updating)",
        &g_present_toggles.skip_engine_tick);
    if (NButton(
            "A10", "Verbose log for 600 frames: every line, no rate limit, per-frame telemetry, shader patch dumps"))
        VerboseArm(600);
    if (logger::unlimited()) ImGui::TextColored({ 1, 1, 0, 1 }, "verbose log ON");
#endif
    if (NButton("B1", "Reload mods (re-reads every mod ini)")) {
        PortApi::RequestReload();
        logger::write("B1: reload requested -- the inis are parsed again at the next frame boundary");
    }
    return true;
}
