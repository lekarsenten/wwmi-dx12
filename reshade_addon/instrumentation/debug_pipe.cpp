// debug_pipe.cpp -- see debug_pipe.h. One listener thread serves one client at a time: it reads a command line,
// queues it for the present thread, and writes back "<bytes>\n<reply>".
#include "debug_pipe.h"
#include "shader_capture.h"
#include "../migoto/port_api.h"
#include "../migoto/dx12_bridge.h" // DumpDir
#include "../draw/draw_parts.h" // SmallMeshReport
#include "../draw/draw_sectex.h" // g_tex_draw_watch
#include "../mods/shader_alias.h" // ShaderAliasCommand
#include "../mods/mod_set.h"
#include "opacity_watch.h"
#include "../textures/tex_pipe_alias.h"
#include "../textures/tex_ref_take.h"
#include "../textures/tex_slot_log.h"
#include "../core/log.h"
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\wwmi_dx12_debug";
constexpr size_t kMaxCommand = 64 * 1024;
constexpr DWORD kReplyWaitMs = 30000; // no present for this long: the game is not drawing
constexpr DWORD kCloseWaitMs = 5000; // the client's own close after it read the reply

struct Request {
    std::string command;
    std::string reply;
    bool done = false;
};

struct Listener {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Request>> queue;
    std::atomic<bool> has_work{ false };
    std::atomic<bool> stopping{ false };
    std::thread thread;
    HANDLE stop = nullptr;
};
Listener g_listener;

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<std::string> Words(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string Rest(const std::string& line, const std::string& word)
{
    const size_t at = line.find(word);
    if (at == std::string::npos) return {};
    const size_t from = line.find_first_not_of(" \t", at + word.size());
    return from == std::string::npos ? std::string() : line.substr(from);
}

std::string Num(float v)
{
    char b[48];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

const char* const kHelp =
    "ping                 the frame number\n"
    "log <text>           a PIPE line in the add-on log\n"
    "get <variable>       an ini variable, e.g. get $\\RabbitFX\\menu\n"
    "set <variable> <n>   writes it\n"
    "run <section>        runs a section by its full name, as a key would\n"
    "shaders              every cached shader: stage, hash, bytes\n"
    "shader <hash>        writes that shader's bytecode to the dump folder\n"
    "smallmesh [count]    small draws seen by the mesh-hash check, with their hash; count filters by index count\n"
    "texdraws <texhash>   watches draws, indexed or not, for that texture at ps-t0..t8 (earlier finds cleared)\n"
    "texdraws all         every draw's ps-t0..t8 textures, for about two frames\n"
    "texdraws [off]       what was found: ps, vs, register, texture; off stops watching\n"
    "texrefs take         texture-match references of every texture game character draws read over two frames (A21)\n"
    "texrefs              what the last take did\n"
    "texalias             every game texture hash taken as a mod target from the pipe\n"
    "texalias <game> <rule>  a game texture with that hash swaps as that mod target; texalias <game> off removes it\n"
    "alias                every DX12 -> DX11 shader-hash alias [ShaderOverride] sections see\n"
    "alias <dx12> <dx11>  adds one, ahead of the .shaderpairs files; alias <dx12> off removes it\n"
    "ovrpicks [texhash]   what checked draws' sections left per game texture, and how often it changed since "
    "the last ovrpicks\n"
    "textrace <texhash> [frames]  every write to that texture's slots for the next frames (240 by default): who, "
    "which thread, which variables its sections read are set\n"
    "cbrows <ps> <reg> <row> [rows]  that game pixel shader's cb<reg> rows (8) at its draws, a CBROWS line when "
    "they change; cbrows: the latest; cbrows off\n"
    "opacity [frames]     claimed draws of decensored shaders within that many frames (120): their fade switch and "
    "fixed opacity as read from the game's material CB (needs A2 debug and A4 decensor); unmodded skinned draws are "
    "mod -1 with their index count as the component\n";

std::string ListShaders()
{
    std::ostringstream out;
    size_t n = 0;
    g_shader_capture.ForEach([&](uint64_t hash, const char* stage, size_t bytes) {
        char b[64];
        std::snprintf(b, sizeof b, "%s %016llx %zu\n", stage, (unsigned long long)hash, bytes);
        out << b;
        ++n;
    });
    out << n << " shader(s)";
    return out.str();
}

std::string DumpShader(const std::string& text)
{
    char* end = nullptr;
    const uint64_t hash = std::strtoull(text.c_str(), &end, 16);
    if (!end || *end) return "ERR not a hex hash: " + text;
    std::vector<uint8_t> code;
    std::string stage;
    if (!g_shader_capture.Find(hash, &code, &stage)) return "ERR no cached shader " + text;
    const std::wstring dir = MigotoPort::DumpDir() + L"\\pipe";
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\shader_%S_%016llx.bin", dir.c_str(), stage.c_str(), (unsigned long long)hash);
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (!f) return "ERR cannot write " + Narrow(path);
    const size_t wrote = std::fwrite(code.data(), 1, code.size(), f);
    std::fclose(f);
    logger::write("PIPE: shader %s %016llx -> %zu bytes -> %S", stage.c_str(), (unsigned long long)hash, wrote, path);
    return Narrow(path) + " (" + std::to_string(wrote) + " bytes)";
}

std::string Execute(const std::string& line)
{
    ModSetRef set_ref;
    const std::vector<std::string> w = Words(line);
    if (w.empty()) return "ERR empty command";
    const std::string& c = w[0];
    if (c == "help") return kHelp;
    if (c == "ping") return "pong f=" + std::to_string(g_frame_id);
    if (c == "log") {
        const std::string text = Rest(line, "log");
        logger::write("PIPE: %s", text.c_str());
        return "ok";
    }
    if (c == "get" && w.size() == 2) {
        const float v = PortApi::GetVar(Wide(w[1]).c_str(), NAN);
        return std::isnan(v) ? "ERR no variable " + w[1] : w[1] + " = " + Num(v);
    }
    if (c == "set" && w.size() == 3) {
        char* end = nullptr;
        const float v = std::strtof(w[2].c_str(), &end);
        if (!end || *end) return "ERR not a number: " + w[2];
        const std::wstring name = Wide(w[1]);
        const float was = PortApi::GetVar(name.c_str(), NAN);
        if (!PortApi::SetVar(name.c_str(), v)) return "ERR no variable " + w[1];
        logger::write("PIPE: set %s %g (was %g)", w[1].c_str(), v, was);
        return w[1] + ": " + Num(was) + " -> " + Num(v);
    }
    if (c == "run" && w.size() == 2) {
        const std::wstring section = Wide(w[1]);
        if (!PortApi::HasSection(section.c_str())) return "ERR no section " + w[1];
        logger::write("PIPE: run %s", w[1].c_str());
        return PortApi::RunSectionThenPost(section.c_str(), nullptr) ? "ran " + w[1] : "ERR " + w[1] + " did not run";
    }
    if (c == "shaders" && w.size() == 1) return ListShaders();
    if (c == "shader" && w.size() == 2) return DumpShader(w[1]);
    if (c == "smallmesh" && w.size() <= 2)
        return SmallMeshReport(w.size() == 2 ? (uint32_t)std::strtoul(w[1].c_str(), nullptr, 10) : 0u);
    if (c == "texdraws" && w.size() <= 2)
        return g_tex_draw_watch.Command(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "texrefs" && w.size() <= 2)
        return g_tex_ref_take.Command(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "texalias" && w.size() <= 3)
        return g_tex_pipe_alias.Command(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "alias" && w.size() <= 3) return ShaderAliasCommand(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "cbrows" && w.size() <= 5) return g_cb_rows.Command(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "opacity" && w.size() <= 2)
        return g_opacity_watch.Command(std::vector<std::string>(w.begin() + 1, w.end()));
    if (c == "ovrpicks" && w.size() <= 2)
        return OverridePickReport(w.size() == 2 ? (uint32_t)std::strtoul(w[1].c_str(), nullptr, 16) : 0u);
    if (c == "textrace" && (w.size() == 2 || w.size() == 3))
        return g_tex_trace.Arm((uint32_t)std::strtoul(w[1].c_str(), nullptr, 16),
            w.size() == 3 ? (uint32_t)std::strtoul(w[2].c_str(), nullptr, 10) : 240u);
    return "ERR unknown command: " + line + "\n" + kHelp;
}

bool AwaitIo(HANDLE pipe, OVERLAPPED& ov, DWORD* bytes, DWORD timeout_ms)
{
    HANDLE hs[2] = { ov.hEvent, g_listener.stop };
    const DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
    if (w != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, bytes, FALSE) != 0;
}

bool ReadSome(HANDLE pipe, HANDLE ev, char* buf, DWORD cap, DWORD* got, DWORD timeout_ms)
{
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    *got = 0;
    if (!ReadFile(pipe, buf, cap, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
    return AwaitIo(pipe, ov, got, timeout_ms) && *got > 0;
}

bool WriteAll(HANDLE pipe, HANDLE ev, const std::string& data)
{
    size_t at = 0;
    while (at < data.size()) {
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        const size_t left = data.size() - at;
        const DWORD chunk = left > (1u << 20) ? (1u << 20) : (DWORD)left;
        if (!WriteFile(pipe, data.data() + at, chunk, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
        if (!AwaitIo(pipe, ov, &n, kReplyWaitMs) || !n) return false;
        at += n;
    }
    return true;
}

bool SendReply(HANDLE pipe, HANDLE ev, const std::string& reply)
{
    return WriteAll(pipe, ev, std::to_string(reply.size()) + "\n" + reply);
}

std::string Answer(const std::string& command)
{
    auto req = std::make_shared<Request>();
    req->command = command;
    std::unique_lock l(g_listener.mtx);
    g_listener.queue.push_back(req);
    g_listener.has_work.store(true, std::memory_order_release);
    g_listener.cv.wait_for(l, std::chrono::milliseconds(kReplyWaitMs),
        [&] { return req->done || g_listener.stopping.load(std::memory_order_acquire); });
    if (req->done) return req->reply;
    for (auto it = g_listener.queue.begin(); it != g_listener.queue.end(); ++it)
        if (*it == req) {
            g_listener.queue.erase(it);
            break;
        }
    return g_listener.stopping.load() ? "ERR the pipe is closing" : "ERR no present for 30 s: the command was dropped";
}

void Converse(HANDLE pipe, HANDLE ev)
{
    std::string line;
    char buf[4096];
    while (line.find('\n') == std::string::npos) {
        DWORD got = 0;
        if (!ReadSome(pipe, ev, buf, sizeof buf, &got, kReplyWaitMs)) return;
        line.append(buf, got);
        if (line.size() > kMaxCommand) {
            SendReply(pipe, ev, "ERR command longer than 64 KB");
            return;
        }
    }
    line.resize(line.find('\n'));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!SendReply(pipe, ev, Answer(line))) return;
    DWORD got = 0;
    ReadSome(pipe, ev, buf, sizeof buf, &got, kCloseWaitMs);
}

PSECURITY_DESCRIPTOR UserOnlyAccess()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return nullptr;
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> user(n);
    PSECURITY_DESCRIPTOR sd = nullptr;
    LPWSTR sid = nullptr;
    if (n && GetTokenInformation(tok, TokenUser, user.data(), n, &n) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + std::wstring(sid) + L")S:(ML;;NW;;;ME)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            sd = nullptr;
        LocalFree(sid);
    }
    CloseHandle(tok);
    return sd;
}

void Serve()
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const PSECURITY_DESCRIPTOR sd = UserOnlyAccess();
    if (!sd) logger::write("PIPE: no access rule for this user (error %lu) -- the default one applies", GetLastError());
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    while (WaitForSingleObject(g_listener.stop, 0) == WAIT_TIMEOUT) {
        HANDLE pipe =
            CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0,
                sd ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            logger::write("PIPE: cannot create %S (error %lu) -- not listening", kPipeName, GetLastError());
            break;
        }
        OVERLAPPED ov = {};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD n = 0;
        bool connected = ConnectNamedPipe(pipe, &ov) != 0;
        if (!connected) {
            const DWORD e = GetLastError();
            connected = e == ERROR_PIPE_CONNECTED || (e == ERROR_IO_PENDING && AwaitIo(pipe, ov, &n, INFINITE));
        }
        if (connected) Converse(pipe, ev);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (sd) LocalFree(sd);
    CloseHandle(ev);
}

} // namespace

namespace DebugPipe {

void SetListening(bool on)
{
    if (on == g_listener.thread.joinable()) return;
    if (on) {
        g_listener.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_listener.stopping.store(false);
        g_listener.thread = std::thread(Serve);
        logger::write("PIPE: listening on %S", kPipeName);
        return;
    }
    {
        std::lock_guard l(g_listener.mtx);
        g_listener.stopping.store(true);
    }
    SetEvent(g_listener.stop);
    g_listener.cv.notify_all();
    g_listener.thread.join();
    CloseHandle(g_listener.stop);
    g_listener.stop = nullptr;
    {
        std::lock_guard l(g_listener.mtx);
        g_listener.queue.clear();
        g_listener.has_work.store(false);
    }
    logger::write("PIPE: closed");
}

void RunQueued()
{
    if (!g_listener.has_work.load(std::memory_order_acquire)) return;
    std::deque<std::shared_ptr<Request>> work;
    {
        std::lock_guard l(g_listener.mtx);
        work.swap(g_listener.queue);
        g_listener.has_work.store(false, std::memory_order_relaxed);
    }
    for (const std::shared_ptr<Request>& r : work) {
        std::string reply = Execute(r->command);
        std::lock_guard l(g_listener.mtx);
        r->reply = std::move(reply);
        r->done = true;
    }
    g_listener.cv.notify_all();
}

void Shutdown()
{
    SetListening(false);
}

} // namespace DebugPipe
