// The [ShaderOverride*] dispatch and the hash alias table it looks through. See shader_alias.h.
#include "../addon_common.h"
#include "shader_alias.h"
#include "shader_pairs_seed.h"
#include "../addon/addon.h"
#include "../draw/draw_toggles.h"
#include "../hooks/command_list_registry.h"
#include "../shapekeys/sk_toggles.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

thread_local uint64_t t_cur_vs_hash = 0;
thread_local uint64_t t_cur_ps_hash = 0;

// Does any [ShaderOverride*] target this shader hash? (optionally return the first section name)
bool ShaderOverrideMatches(uint64_t shader_hash, std::wstring* first_section)
{
    if (!shader_hash) return false;
    if (!PortApi::ShaderOverrideMatches(shader_hash)) return false;
    if (first_section) {
        wchar_t sec[256] = {};
        PortApi::ShaderOverrideSection(shader_hash, sec, std::size(sec));
        *first_section = sec;
    }
    return true;
}

namespace {

struct AliasEntry {
    uint64_t dx11 = 0;
    std::string note; // the line's comment: what the shader is
    uint32_t mesh = 0; // the character's vb0 hash on DX11, 0 when the line names none
    bool built_in = false; // from the build's own rows, not a .shaderpairs file
};

struct ShaderPairLine {
    bool blank = true;
    bool ok = false;
    uint64_t dx12 = 0;
    uint64_t dx11 = 0;
    uint32_t mesh = 0;
    std::string note;
};

bool HexToken(const std::string& t, size_t digits, uint64_t* out)
{
    if (t.size() != digits) return false;
    char* end = nullptr;
    *out = std::strtoull(t.c_str(), &end, 16);
    return end && !*end;
}

// "<dx12 hash> <dx11 hash> [<mesh hash>]", anything after them ignored; '#' or ';' starts the comment, the note.
ShaderPairLine ParseShaderPairLine(const std::string& line)
{
    ShaderPairLine r;
    const size_t comment = line.find_first_of("#;");
    if (comment != std::string::npos) {
        const size_t from = line.find_first_not_of(" \t", comment + 1);
        const size_t to = line.find_last_not_of(" \t\r");
        if (from != std::string::npos && to >= from) r.note = line.substr(from, to - from + 1);
    }
    std::istringstream ss(line.substr(0, comment));
    std::string tok[3];
    int n = 0;
    while (n < 3 && ss >> tok[n]) ++n;
    if (!n) return r;
    r.blank = false;
    uint64_t mesh = 0;
    r.ok = n >= 2 && HexToken(tok[0], 16, &r.dx12) && HexToken(tok[1], 16, &r.dx11) && r.dx12 && r.dx11 &&
        (n < 3 || HexToken(tok[2], 8, &mesh));
    r.mesh = (uint32_t)mesh;
    return r;
}

void LoadShaderPairFiles(std::unordered_map<uint64_t, AliasEntry>& m)
{
    if (g_addon.data_dir.empty()) return;
    const std::wstring dir = g_addon.data_dir + L"\\pairs";
    WIN32_FIND_DATAW fd;
    const HANDLE find = FindFirstFileW((dir + L"\\*.shaderpairs").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    unsigned files = 0;
    unsigned pairs = 0;
    do {
        if (_wcsnicmp(fd.cFileName, L"DISABLED_", 9) == 0) continue;
        std::ifstream in(dir + L"\\" + fd.cFileName);
        if (!in) continue;
        ++files;
        std::string line;
        unsigned n = 0;
        while (std::getline(in, line)) {
            ++n;
            const ShaderPairLine pl = ParseShaderPairLine(line);
            if (pl.blank) continue;
            if (!pl.ok) {
                logger::write("SHADERPAIRS: %S line %u is not 'dx12 dx11 [mesh]' -- skipped", fd.cFileName, n);
                continue;
            }
            const auto [it, added] = m.emplace(pl.dx12, AliasEntry{ pl.dx11, pl.note, pl.mesh });
            if (added)
                ++pairs;
            else if (it->second.dx11 != pl.dx11)
                logger::write("SHADERPAIRS: %S line %u pairs %016llx with %016llx; it is already paired with %016llx, "
                              "which stays",
                    fd.cFileName, n, (unsigned long long)pl.dx12, (unsigned long long)pl.dx11,
                    (unsigned long long)it->second.dx11);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    logger::write("SHADERPAIRS: %u pair(s) from %u file(s) in %S", pairs, files, dir.c_str());
}

// The build's own rows (shader_pairs_seed.h): each pairs a shader no file has paired.
void LoadBuiltInShaderPairs(std::unordered_map<uint64_t, AliasEntry>& m)
{
    unsigned rows = 0;
    unsigned pairs = 0;
    for (const char* line : kShaderPairsSeed) {
        const ShaderPairLine pl = ParseShaderPairLine(line);
        if (!pl.ok) continue;
        ++rows;
        const auto [it, added] = m.emplace(pl.dx12, AliasEntry{ pl.dx11, pl.note, pl.mesh, true });
        if (added)
            ++pairs;
        else if (it->second.dx11 != pl.dx11)
            logger::write("SHADERPAIRS: a file pairs %016llx with %016llx; the built-in pair with %016llx is not used",
                (unsigned long long)pl.dx12, (unsigned long long)it->second.dx11, (unsigned long long)pl.dx11);
    }
    logger::write("SHADERPAIRS: %u of %u built-in pair(s) added, the rest already paired by a file", pairs, rows);
}

const std::unordered_map<uint64_t, AliasEntry>& AliasIndex()
{
    static const std::unordered_map<uint64_t, AliasEntry> index = [] {
        std::unordered_map<uint64_t, AliasEntry> m;
        LoadShaderPairFiles(m);
        LoadBuiltInShaderPairs(m);
        return m;
    }();
    return index;
}

#ifdef WWMI_DEBUG
std::shared_mutex g_pipe_alias_mtx;
std::unordered_map<uint64_t, uint64_t> g_pipe_alias; // set from the debug pipe, looked up ahead of the table
std::atomic<bool> g_pipe_alias_any{ false };
#endif

} // namespace

uint64_t AliasShaderHashAlways(uint64_t h)
{
    const auto& index = AliasIndex();
    const auto it = index.find(h);
    return it == index.end() ? h : it->second.dx11;
}

static uint64_t PairedShaderHash(uint64_t h)
{
    if (!g_sk_toggles.shader_hash_alias) return h;
#ifdef WWMI_DEBUG
    if (g_pipe_alias_any.load(std::memory_order_relaxed)) {
        std::shared_lock l(g_pipe_alias_mtx);
        auto it = g_pipe_alias.find(h);
        if (it != g_pipe_alias.end()) return it->second;
    }
#endif
    const auto& index = AliasIndex();
    const auto it = index.find(h);
    if (it == index.end()) return h;
    const AliasEntry& a = it->second;
    if (g_debug) { // one-shot per entry: proves the alias fired, and which
        static std::mutex m;
        static std::set<uint64_t> seen;
        bool fr;
        {
            std::lock_guard l(m);
            fr = seen.insert(h).second;
        }
        if (fr) {
            if (!a.note.empty())
                logger::write("SHADER-ALIAS: dx12 %016llx -> dx11 %016llx (%s)", (unsigned long long)h,
                    (unsigned long long)a.dx11, a.note.c_str());
            else
                logger::write("SHADER-ALIAS: dx12 %016llx -> dx11 %016llx (a game shader pair, mesh %08x)",
                    (unsigned long long)h, (unsigned long long)a.dx11, a.mesh);
        }
    }
    return a.dx11;
}

uint64_t AliasShaderHash(uint64_t h)
{
    return PortApi::ShaderOverrideMatches(h) ? h : PairedShaderHash(h);
}

#ifdef WWMI_DEBUG
uint32_t ShaderPairMesh(uint64_t dx12)
{
    const auto& index = AliasIndex();
    const auto it = index.find(dx12);
    return it == index.end() ? 0u : it->second.mesh;
}

std::string ShaderAliasCommand(const std::vector<std::string>& args)
{
    if (args.size() == 2) {
        char* end = nullptr;
        const uint64_t dx12 = std::strtoull(args[0].c_str(), &end, 16);
        if (!end || *end || !dx12) return "ERR not a shader hash: " + args[0];
        std::unique_lock l(g_pipe_alias_mtx);
        if (args[1] == "off") {
            const bool had = g_pipe_alias.erase(dx12) != 0;
            g_pipe_alias_any.store(!g_pipe_alias.empty());
            if (had) logger::write("PIPE: alias %016llx off", (unsigned long long)dx12);
            return had ? "removed " + args[0] : "ERR no pipe alias for " + args[0];
        }
        const uint64_t dx11 = std::strtoull(args[1].c_str(), &end, 16);
        if (!end || *end || !dx11) return "ERR not a shader hash: " + args[1];
        g_pipe_alias[dx12] = dx11;
        g_pipe_alias_any.store(true);
        PortApi::RegisterShaderHash(dx11);
        logger::write("PIPE: alias dx12 %016llx -> dx11 %016llx", (unsigned long long)dx12, (unsigned long long)dx11);
        return args[0] + " -> " + args[1] +
            (PortApi::ShaderOverrideMatches(dx11) ? "" : " (no [ShaderOverride] has that hash)");
    }
    if (!args.empty()) return "ERR alias, alias <dx12> <dx11> or alias <dx12> off";
    std::string out;
    char b[192];
    for (const auto& [dx12, a] : AliasIndex()) {
        std::snprintf(b, sizeof b, "%016llx -> %016llx %s, mesh %08x", (unsigned long long)dx12,
            (unsigned long long)a.dx11, a.built_in ? "built in" : "from a .shaderpairs file", a.mesh);
        out += b + (a.note.empty() ? std::string() : ": " + a.note) + "\n";
    }
    std::shared_lock l(g_pipe_alias_mtx);
    for (const auto& [dx12, dx11] : g_pipe_alias) {
        std::snprintf(
            b, sizeof b, "%016llx -> %016llx from the pipe\n", (unsigned long long)dx12, (unsigned long long)dx11);
        out += b;
    }
    return out + (g_sk_toggles.shader_hash_alias ? "" : "(I7 is off: no alias applies)");
}
#endif

bool RunShaderOverrides(uint64_t shader_hash, const PortApi::DrawInfo* draw, command_list* cmd_list)
{
    if (!shader_hash || !g_draw_toggles.wcl_draw) return false;
    PortApi::DrawInfo di = draw ? *draw : PortApi::DrawInfo();
    di.is_pixel_shader = (shader_hash == t_cur_ps_hash);
    di.partner_shader_hash = di.is_pixel_shader ? t_cur_vs_hash : t_cur_ps_hash;
    if (cmd_list) {
        const CmdRT& rt = g_command_lists.Rt(cmd_list);
        if (rt.valid) {
            di.depth_active = rt.has_dsv;
            di.depth_known = true;
        }
    }
    bool ran = PortApi::DispatchShaderOverride(shader_hash, &di);
    const uint64_t paired = PairedShaderHash(shader_hash);
    if (paired != shader_hash) ran |= PortApi::DispatchShaderOverride(paired, &di);
    if (g_debug && paired != shader_hash) {
        static std::mutex m;
        static std::set<uint64_t> seen;
        bool fr;
        {
            std::lock_guard l(m);
            fr = ProbeArmed() && seen.insert(shader_hash).second;
        }
        if (fr)
            logger::write("SHADER-ALIAS-RESULT: dx12 %016llx -> dx11 %016llx  ran=%d skip=%d",
                (unsigned long long)shader_hash, (unsigned long long)paired, (int)ran, (int)di.skip);
    }
    return ran && di.skip;
}
