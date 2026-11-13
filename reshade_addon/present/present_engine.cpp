// present_engine.cpp -- see present_engine.h.
#include "../addon_common.h"
#include "../mods/ib_hash.h"
#include "../mods/wwmi_runtime.h" // WwmiReadFile -- BufCopy.cso handed to the port
#include "../instrumentation/tex_timing.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/sk_ring.h"
#include "../mods/character_recognition.h"
#include "../instrumentation/chain_trace.h"
#include "../instrumentation/gpu_timer.h"
#include "../instrumentation/hot_path.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../instrumentation/watchdogs.h"
#include "../migoto/dx12_wrap.h" // BackBufferRestore -- the [Present] list leaves the back buffer as it found it
#include "../migoto/dx12_bridge.h"
#include "../core/gpu_queue.h"
#include "../core/gpu_progress.h"
#include "present_engine.h"
#include "presence_tick.h"
#include "../migoto/frame_analysis_driver.h" // FrameAnalysisArm -- Numpad 2
#include "../core/fnv.h" // Fnv1a32 -- KEYGATE logs each namespace once
#include "../addon/addon.h"
#include "../draw/draw_toggles.h"
#include "../instrumentation/diag_arms.h"
#include "frame_sync.h"
#include "present_toggles.h"
#include "../textures/tex_place.h"
#include "../textures/tex_slot_log.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/port_api.h"
#include "../mods/skeleton_dump.h"
#include "../instrumentation/profiler.h"
#include "../mods/buffer_relocation.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

EngineFrame g_engine_frame;

bool EngineFrame::Pass::Init(ID3D12Device* dev)
{
    if (ready) return true;
    if (!dev) return false;
    for (UINT i = 0; i < kRing; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&cl))))
        return false;
    // NAME IT: a DRED breadcrumb dump prints the list's debug name, and every node read '(unnamed)'.
    cl->SetName(L"wwmi PresentPass");
    cl->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready = (ev != nullptr);
    return ready;
}

ID3D12GraphicsCommandList* EngineFrame::Pass::Begin()
{
    if (!ready) return nullptr;
    slot = (UINT)(counter % kRing);
    // blocks only when we are kRing frames ahead
    const DWORD wait = WaitForFence(fence.Get(), slot_fence[slot], ev, 2000);
    if (wait != WAIT_OBJECT_0) {
        if (skipped++ == 0)
            logger::report("PRESENTPASS: the GPU has not finished frame %llu (done %llu, wait %lu) -- the present "
                           "pass skips frames until it has",
                (unsigned long long)slot_fence[slot], (unsigned long long)fence->GetCompletedValue(), wait);
        return nullptr;
    }
    if (skipped) {
        logger::report("PRESENTPASS: the GPU caught up; %u frame(s) of the present pass skipped", skipped);
        skipped = 0;
    }
    if (FAILED(alloc[slot]->Reset())) return nullptr;
    if (FAILED(cl->Reset(alloc[slot].Get(), nullptr))) return nullptr;
    RemovalPin::OnReset(cl.Get()); // a native Reset: ReShade's reset_command_list does not fire for it
    MigotoPort::ListBuffersForget(cl.Get());
    HotPath::GpuOwnListBegin(cl.Get());
    return cl.Get();
}

HRESULT EngineFrame::Pass::Submit(ID3D12CommandQueue* game_queue)
{
    if (!ready || !game_queue) return E_POINTER;
    HotPath::GpuOwnListEnd(cl.Get(), game_queue);
    MigotoPort::ListBuffersToCommon(cl.Get());
    const HRESULT hr = cl->Close();
    if (FAILED(hr)) return hr;
    if (game_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) { // a graphics list there removes the device
        static std::atomic<bool> once{ false };
        if (!once.exchange(true))
            logger::write("PRESENTPASS: not submitted -- queue %p is not a graphics queue (type %d)", (void*)game_queue,
                (int)game_queue->GetDesc().Type);
        return E_INVALIDARG;
    }
    ID3D12CommandList* lists[] = { cl.Get() };
    RemovalPin::OnExecute(g_addon.d3d12_device, cl.Get(), (int)D3D12_COMMAND_LIST_TYPE_DIRECT,
        (int)game_queue->GetDesc().Type, "present-pass submit");
    game_queue->ExecuteCommandLists(1, lists);
    slot_fence[slot] = ++counter;
    const HRESULT signal_hr = game_queue->Signal(fence.Get(), slot_fence[slot]);
    if (FAILED(signal_hr)) {
        static std::atomic<bool> once{ false };
        if (!once.exchange(true))
            logger::report("PRESENTPASS: Signal of frame %llu failed (%08lX)", (unsigned long long)slot_fence[slot],
                (unsigned long)signal_hr);
    }
    IbHash::Submitted(fence.Get(), slot_fence[slot]); // the hash recorded into this list completes with it
    g_gpu_timer.FrameSubmitted(fence.Get(), slot_fence[slot], game_queue);
    HotPath::GpuFrameSubmitted(fence.Get(), slot_fence[slot], game_queue);
    return hr;
}

void EngineFrame::ProbeTick(command_queue* queue, swapchain* sc)
{
    const unsigned cur_gen = PortApi::LoadGeneration();
    if (!port_tried_ || port_built_for_ != cur_gen) {
        port_tried_ = true;
        port_built_for_ = cur_gen;
        auto* dev12 = (ID3D12Device*)sc->get_device()->get_native();
        auto* q12 = (ID3D12CommandQueue*)queue->get_native();
        PortApi::SetFrameContext(dev12, nullptr, q12, 0, 0, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, nullptr,
            D3D12_RESOURCE_STATE_COMMON, nullptr);
        const bool ok = PortApi::Load(g_addon.wwmi_dir.c_str());
        {
            const auto cso = WwmiReadFile((g_addon.cso_dir + L"\\BufCopy.cso").c_str());
            if (cso.empty())
                logger::write("BUFCOPY: BufCopy.cso missing -- buffer copies stay CopyResource");
            else
                MigotoPort::SetBufCopyShader(cso.data(), cso.size());
        }
        const PortApi::Stats st = PortApi::GetStats();
        logger::write("PORT-PROBE: Load()=%d  command_lists=%zu custom_shaders=%zu resources=%zu "
                      "texture_overrides=%zu shader_overrides=%zu ini_params=%d",
            (int)ok, st.command_lists, st.custom_shaders, st.custom_resources, st.texture_overrides,
            st.shader_overrides, st.ini_params);
        logger::write("PORT-PROBE: mod tree came from d3dx.ini's include_recursive");

        {
            wchar_t v[64] = {};
            if (PortApi::GetSectionValue(L"Rendering", L"allow_buffer_resize", v, std::size(v)) && v[0]) {
                g_present_toggles.allow_buffer_resize = (wcstol(v, nullptr, 10) != 0);
                logger::write("BUFRESIZE: d3dx.ini [Rendering] allow_buffer_resize = %ls -> %s", v,
                    g_present_toggles.allow_buffer_resize ? "relocating" : "binding the game's buffer as-is");
            } else {
                logger::write("BUFRESIZE: d3dx.ini states no allow_buffer_resize -- keeping the built-in "
                              "default (%d). NOT an error, but it means the ini is not driving this.",
                    (int)g_present_toggles.allow_buffer_resize);
            }
        }
        g_buffer_reloc.ClearDeclared();
        PortApi::ForEachBufferResize(
            [](unsigned hash, unsigned stride, unsigned count, const wchar_t* section, void*) {
                g_buffer_reloc.Declare(hash, BufResize{ stride, count });
                if (logger::enabled(1))
                    logger::write("BUFRESIZE: %08x -> %u x %u = %llu B   [%ls]", hash, count, stride,
                        (unsigned long long)stride * count, section);
            },
            nullptr);
        logger::write("BUFRESIZE: %zu buffer(s) declare a resize. allow_buffer_resize honoured=%d "
                      "(0 = we still bind the game's vanilla-sized buffer)",
            g_buffer_reloc.DeclaredCount(), (int)g_present_toggles.allow_buffer_resize);
    }
}

void EngineFrame::GameClient(float& rw, float& rh, bool& focused)
{
    if (HWND fg = GetForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            focused = true;
            game_hwnd_ = fg;
        } // latch it while we can
    }
    if (!game_hwnd_) { // never focused yet (launched to background): find our own top-level window
        struct E {
            DWORD pid;
            HWND hit;
        } e{ GetCurrentProcessId(), nullptr };
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto* s = (E*)lp;
                DWORD p = 0;
                GetWindowThreadProcessId(h, &p);
                if (p == s->pid && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
                    s->hit = h;
                    return FALSE;
                }
                return TRUE;
            },
            (LPARAM)&e);
        game_hwnd_ = e.hit;
    }
    if (game_hwnd_) {
        RECT rc;
        if (GetClientRect(game_hwnd_, &rc) && rc.right > 0 && rc.bottom > 0) {
            rw = (float)rc.right;
            rh = (float)rc.bottom; // valid focused OR not
        }
    }
}

void EngineFrame::Redraw(command_queue* queue, swapchain* sc)
{
    PROF(PROF_EngineTick);
    if (!g_draw_toggles.wwmi_geom || !present_pass_.Init(g_addon.d3d12_device)) return;
    float rw = 0;
    float rh = 0;
    bool focused = false;
    GameClient(rw, rh, focused);
    ID3D12GraphicsCommandList* const present_cl = present_pass_.Begin();
    if (!present_cl) return;
    std::lock_guard l(mutex_);
    auto* const dev12 = (ID3D12Device*)sc->get_device()->get_native();
    auto* const q12 = (ID3D12CommandQueue*)queue->get_native();
    PortApi::SetFrameContext(dev12, present_cl, q12, (unsigned)rw, (unsigned)rh, DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN, (ID3D12Resource*)sc->get_current_back_buffer().handle, D3D12_RESOURCE_STATE_PRESENT,
        nullptr);
    const unsigned long long own_draws_before = MigotoPort::OwnListDrawsIssued();
    PortApi::PresentRedraw();
    ID3D12CommandQueue* subq = q12;
    if (g_present_toggles.on_game_queue)
        if (const void* gq = g_frames.claim_exec_queue.load(std::memory_order_relaxed))
            subq = reinterpret_cast<ID3D12CommandQueue*>(const_cast<void*>(gq));
    if (g_present_toggles.flush_immediate_first && MigotoPort::OwnListDrawsIssued() != own_draws_before)
        queue->flush_immediate_command_list();
    MigotoPort::FlushPendingUploads(present_cl);
    MigotoPort::BackBufferRestore(present_cl, D3D12_RESOURCE_STATE_PRESENT);
    present_pass_.Submit(subq);
    PortApi::SetFrameContext(dev12, nullptr, q12, (unsigned)rw, (unsigned)rh, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
        nullptr, D3D12_RESOURCE_STATE_COMMON, nullptr);
}

void EngineFrame::InstallKeyGate()
{
    if (gate_installed_) return;
    gate_installed_ = true;
    PortApi::SetKeyGate([](const wchar_t* ns, const wchar_t* section) -> bool {
        { // Each namespace once, unconditionally: which namespace WWMI's own keys arrive under is
            // what the block below has to match.
            static std::mutex m;
            static std::unordered_set<uint32_t> seen;
            const uint32_t h = ns ? Fnv1a32(ns, wcslen(ns) * sizeof(wchar_t)) : 0u;
            std::lock_guard l(m);
            if (seen.insert(h).second)
                logger::write("KEYGATE: first key press from namespace '%ls'", ns ? ns : L"(null)");
        }
        if (!ns || !*ns) return true;
        if (section && _wcsicmp(section, L"Key\\WWMIv1\\HideNotification") == 0) return true;
        if (_wcsicmp(ns, L"WWMIv1") == 0 || _wcsicmp(ns, L"Debugger") == 0) return false;
        const int md = Mods().IdOfNs(ns);
        if (md < 0) return true; // unknown ns: fire, never swallow silently
        return Mods().OnScreen(md);
    });
}

void EngineFrame::BeginList(Frame& f)
{
    f.own_draws_before = MigotoPort::OwnListDrawsIssued(); // X34
    if (g_draw_toggles.wwmi_geom && f.pass->Init(g_addon.d3d12_device)) {
        {
            PROF(PROF_ET_Begin);
            f.cl = f.pass->Begin();
        }
        if (f.cl) {
            g_gpu_timer.PresentTick(g_addon.d3d12_device);
            HotPath::GpuTick();
            f.gt = g_gpu_timer.Begin(f.cl, GT_Present);
            f.hp = HotPath::GpuBracketBegin(f.cl, GT_Present);
            auto* dev12 = (ID3D12Device*)f.sc->get_device()->get_native();
            auto* q12 = (ID3D12CommandQueue*)f.queue->get_native();
            PortApi::SetFrameContext(dev12, f.cl, q12, (unsigned)f.rw, (unsigned)f.rh, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_UNKNOWN, f.bb12, D3D12_RESOURCE_STATE_PRESENT, nullptr);
        }
    }
}

void EngineFrame::DebugHotkeys(bool focused)
{
#ifdef WWMI_DEBUG
    static bool s_np0 = false;
    static bool s_np1 = false;
    static bool s_np2 = false;
    static bool s_np3 = false;
    static bool s_np4 = false;
    const bool np4 = focused && (GetAsyncKeyState(VK_NUMPAD4) & 0x8000) != 0;
    if (np4 && !s_np4 && g_addon.capture_mode) {
        logger::write("USER-MARK frame=%u (Numpad 4: texture load timing %s)", g_frame_id,
            TexTiming::Armed() ? "stopped" : "started");
        if (TexTiming::Armed())
            TexTiming::Stop();
        else
            TexTiming::Arm(60.0);
    }
    s_np4 = np4;
    const bool np0 = focused && (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
    const bool np1 = focused && (GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0;
    const bool np2 = focused && (GetAsyncKeyState(VK_NUMPAD2) & 0x8000) != 0;
    const bool np3 = focused && (GetAsyncKeyState(VK_NUMPAD3) & 0x8000) != 0;
    if (np0 && !s_np0) {
        g_addon.capture_mode = !g_addon.capture_mode;
        logger::write("CAPTURE MODE %s (Numpad 0) -- Numpad 1 = shapekey ring, 2 = G2 char dump, 3 = D10 draw log + "
                      "TEXFLIP, 4 = texture load timing (start/stop, 60 s max); each also writes a USER-MARK",
            g_addon.capture_mode ? "ON" : "OFF");
    }
    // The overlay's G2 / D10 from the keyboard, for moments the overlay cannot be opened (an ult, a cutscene).
    if (np2 && !s_np2 && g_addon.capture_mode) {
        logger::write("USER-MARK frame=%u (Numpad 2: G2 char dump)", g_frame_id);
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ true, /*log_only*/ false);
    }
    if (np3 && !s_np3 && g_addon.capture_mode) {
        logger::write("USER-MARK frame=%u (Numpad 3: D10 draw log + TEXFLIP, TEXNOSWAP for 900 frames)", g_frame_id);
        g_diag_arms.log_all_draws.store(2);
        g_diag_arms.texflip_arm.store(8);
        ++g_diag_arms.texflip_gen;
        g_diag_arms.texnoswap_n.store(0);
        ++g_diag_arms.texnoswap_gen;
        g_diag_arms.texnoswap_arm.store(900);
    }
    s_np2 = np2;
    s_np3 = np3;
    if (np1 && !s_np1 && g_addon.capture_mode) {
        static unsigned s_mark = 0;
        logger::write("USER-MARK #%u frame=%u (Numpad 1)", ++s_mark, g_frame_id);
        SkRing::Dump("Numpad 1");
    }
    s_np0 = np0;
    s_np1 = np1;
#else
    (void)focused;
#endif
}

void EngineFrame::PresentPasses(Frame& f, std::vector<ChainProf>& cp)
{
    PROF(PROF_ET_Lists);
    {
        ID3D12CommandQueue* qs[3] = { (ID3D12CommandQueue*)f.queue->get_native(),
            reinterpret_cast<ID3D12CommandQueue*>(
                const_cast<void*>(g_frames.claim_exec_queue.load(std::memory_order_relaxed))),
            reinterpret_cast<ID3D12CommandQueue*>(g_frames.present_queue.load(std::memory_order_relaxed)) };
        GpuProgressTick((ID3D12Device*)f.sc->get_device()->get_native(), qs, 3);
    }
#ifdef WWMI_DEBUG
    if (g_tex_trace.Armed()) g_tex_trace.Line("present: the [Present] pre pass runs");
#endif
    PortApi::PresentPre(); // their [Present] pre pass
    if (f.focused) PortApi::TickInput();
    bool on_screen = false;
    for (GeoMod& gm : Mods().Units()) {
        gm.det_at_keys = gm.wcl_ns.empty() ? -1.f : PortApi::GetVarNs(gm.wcl_ns.c_str(), L"object_detected", -1.f);
        on_screen |= gm.det_at_keys > 0.f;
    }
    if (g_frame_id % 600u == 300u) g_tex_place.Report();
    if (g_frame_id % 10u == 0u)
        for (GeoMod& gm : Mods().Units())
            if (gm.det_at_keys > 0.f) g_tex_place.SnapshotSelections(gm.wcl_ns.c_str());
    if (on_screen && g_frame_id - held_retry_frame_ >= 30) {
        held_retry_frame_ = g_frame_id;
        g_tex_place.RetryMissed("a mod's object is on screen", true);
    }
    g_tex_place.RunRetryBetweenPasses();
    g_tex_place.RestampOnVarChange();
#ifdef WWMI_DEBUG
    if (g_tex_trace.Armed()) g_tex_trace.Line("present: between the passes, done");
#endif
    // Sampled between the passes: what the gate left behind.
    for (ChainProf& c : cp) {
        c.od_mid = PortApi::GetVarNs(c.ns.c_str(), L"object_detected", -1.f);
        c.sid_mid = PortApi::GetVarNs(c.ns.c_str(), L"state_id", -1.f);
    }
    PortApi::PresentPost(); // their [Present] post pass (e.g. WWMI `post $object_detected = 0`)
    if (g_diag_arms.sectex_watch.load(std::memory_order_relaxed) > 0)
        g_diag_arms.sectex_watch.fetch_sub(1, std::memory_order_relaxed);
    bool switched_on = false;
    for (GeoMod& gm : Mods().Units()) {
        const bool was = gm.geom_enabled;
        gm.geom_enabled = gm.wcl_ns.empty() || PortApi::GetVarNs(gm.wcl_ns.c_str(), L"mod_enabled", 1.f) != 0.f;
        switched_on |= !was && gm.geom_enabled;
        if (was && !gm.geom_enabled) {
            g_tex_place.UnstampSwitchedOff(gm.wcl_ns.c_str());
            g_tex_place.WatchSwitchedOff(gm.wcl_ns.c_str());
        }
    }
    if (switched_on) {
        g_tex_place.WatchSwitchedOn();
        g_tex_place.RetryMissed("a mod switched itself back on");
        if (g_debug) g_diag_arms.sectex_watch.store(3, std::memory_order_relaxed);
    }
}

void EngineFrame::SubmitList(Frame& f)
{
    PROF(PROF_ET_Submit);
    auto* dev12 = (ID3D12Device*)f.sc->get_device()->get_native();
    auto* q12 = (ID3D12CommandQueue*)f.queue->get_native();
    ID3D12CommandQueue* subq = q12;
    if (g_present_toggles.on_game_queue) {
        const void* gq = g_frames.claim_exec_queue.load(std::memory_order_relaxed);
        if (gq) subq = reinterpret_cast<ID3D12CommandQueue*>(const_cast<void*>(gq));
    }
    {
        const int now = (subq == q12) ? 0 : 1;
        if (now != x18_last_) {
            x18_last_ = now;
            logger::write(
                "X18: [Present] list now submitted on the %s queue %p", now ? "GAME's draw" : "present", (void*)subq);
        }
    }
    const bool pd_probe = MigotoPort::PresentDrawProbeArmed() && logger::enabled(1);
    ID3D12Resource* pd_bb = pd_probe ? f.bb12 : nullptr;
    if ((int)g_present_toggles.flush_immediate_first != x34_last_) {
        x34_last_ = (int)g_present_toggles.flush_immediate_first;
        logger::write("X34: ReShade's immediate command list is %s before a [Present] list that drew",
            g_present_toggles.flush_immediate_first ? "FLUSHED" : "NOT flushed (old order)");
    }
    const bool x34 = g_present_toggles.flush_immediate_first && MigotoPort::OwnListDrawsIssued() != f.own_draws_before;
    if (x34) f.queue->flush_immediate_command_list();
    MigotoPort::FlushPendingUploads(f.cl);
    IbHash::Record(f.cl); // the frame's one mesh hash rides this list: no second queue, no second signal
    // Leave the back buffer as ReShade's effects and next frame's game barriers expect it.
    MigotoPort::BackBufferRestore(f.cl, D3D12_RESOURCE_STATE_PRESENT);
    HotPath::GpuBracketEnd(f.cl, f.hp);
    g_gpu_timer.End(f.cl, f.gt);
    const HRESULT pd_close = f.pass->Submit(subq);
    if (pd_probe) {
        logger::write("PRESENTDRAW frame f=%u present_cl=%p close_hr=%08lX queue=%p (on_present queue %p) "
                      "bb=%p index=%u observed_state=0x%x swapchain=%p color_space=%u x34_flushed=%d",
            g_frame_id, (void*)f.cl, (unsigned long)pd_close, (void*)subq, (void*)q12, (void*)pd_bb,
            f.sc->get_current_back_buffer_index(), pd_bb ? MigotoPort::QueryResourceState(pd_bb) : 0u,
            (void*)f.sc->get_native(), (unsigned)f.sc->get_color_space(), (int)x34);
    }
    MigotoPort::PresentDrawProbeFrameDone();
    PortApi::SetFrameContext(dev12, nullptr, q12, (unsigned)f.rw, (unsigned)f.rh, DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT_UNKNOWN, nullptr, D3D12_RESOURCE_STATE_COMMON, nullptr);
    {
        static std::atomic<bool> once{ false };
        if (!once.exchange(true))
            logger::write("PORT: [Present] now runs with a DX12 command list bound -- their `run =` "
                          "compute chains can dispatch (%llu issued so far)",
                MigotoPort::DispatchesIssued());
    }
}

void EngineFrame::Tick(command_queue* queue, swapchain* sc, bool bind_back_buffer)
{
    {
        PROF(PROF_EngineTick); // this block is the bulk of "Present(total)" -- our per-frame engine tick, NOT the game
        Frame f{ queue, sc };
        GameClient(f.rw, f.rh, f.focused);
        f.bb12 = bind_back_buffer ? (ID3D12Resource*)sc->get_current_back_buffer().handle : nullptr;
        f.pass = bind_back_buffer ? &present_pass_ : &frame_end_pass_;

        {
            auto* dev12 = (ID3D12Device*)sc->get_device()->get_native();
            auto* q12 = (ID3D12CommandQueue*)queue->get_native();
            PortApi::SetGameWindow(sc->get_hwnd());
            PortApi::SetFrameContext(dev12, nullptr, q12, (unsigned)f.rw, (unsigned)f.rh, DXGI_FORMAT_UNKNOWN,
                DXGI_FORMAT_UNKNOWN, f.bb12, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
        }

        InstallKeyGate();
        BeginList(f);

        std::vector<ChainProf> cp;
        const bool cp_on = logger::enabled(1) && (g_frame_id % 300u == 2u);
        if (cp_on) {
            for (const GeoMod& gm : Mods().Units()) {
                const std::wstring& uns = gm.wcl_ns;
                if (uns.empty()) continue;
                if (PortApi::GetVarNs(uns.c_str(), L"mod_enabled", -1.f) != 1.f) continue;
                cp.push_back({ uns, PortApi::GetVarNs(uns.c_str(), L"object_detected", -1.f),
                    PortApi::GetVarNs(uns.c_str(), L"state_id", -1.f) });
            }
        }
        // H3: arm on request, then trace whole frames until the budget runs out.
        if (g_diag_arms.chaintrace_req.exchange(false)) {
            g_diag_arms.chaintrace_frames.store(10);
            g_chain_trace.Open();
            logger::write("CHAINTRACE armed for 10 frames -> wwmi_dx12_chain_trace.log");
        }
        if (g_diag_arms.chaintrace_frames.load() > 0) {
            g_chain_trace.SetFrame(g_frame_id);
            g_chain_trace.Vars("frame start (before [Present])");
        }

        g_watchdogs.StallTick(f.focused);
        TexTiming::Tick(); // the heartbeat the stall watchdog watches (watchdogs.cpp)
        DebugHotkeys(f.focused);
        // WWMI's first-run guide is dismissed with F10, which is gated off: never let it open.
        if (PortApi::GetVarNs(L"WWMIv1", L"first_run", 0.f) != 0.f) PortApi::SetVarNs(L"WWMIv1", L"first_run", 0.f);
        PresentPasses(f, cp);
        if (f.cl)
            g_skeleton_dump.ChainTick(
                (ID3D12Device*)sc->get_device()->get_native(), f.cl, (ID3D12CommandQueue*)queue->get_native());
        if (f.cl)
            g_skeleton_dump.MergedTick(
                (ID3D12Device*)sc->get_device()->get_native(), f.cl, (ID3D12CommandQueue*)queue->get_native());

        for (const ChainProf& c : cp) {
            const float od_post = PortApi::GetVarNs(c.ns.c_str(), L"object_detected", -1.f);
            const float sid_post = PortApi::GetVarNs(c.ns.c_str(), L"state_id", -1.f);
            const bool ran = (c.sid_pre != sid_post); // UpdateMergedSkeleton toggles it
            logger::write("CHAINPROF f=%u ns='%ls'  object_detected pre=%g mid=%g post=%g | "
                          "state_id pre=%g mid=%g post=%g | UpdateMergedSkeleton %s%s",
                g_frame_id, c.ns.c_str(), c.od_pre, c.od_mid, od_post, c.sid_pre, c.sid_mid, sid_post,
                ran ? "RAN" : "DID NOT RUN",
                (!ran && c.od_pre == 1.f)
                    ? "  <- gate saw object_detected=1 yet the chain did not run: fault is INSIDE [Present]"
                    : (!ran && c.od_pre == 0.f)
                    ? "  <- gate saw object_detected=0: the components' write did not survive to Present"
                    : "");
        }

        if (g_diag_arms.chaintrace_frames.load() > 0) {
            g_chain_trace.Vars("frame end (after [Present] post)");
            if (g_diag_arms.chaintrace_frames.fetch_sub(1) <= 1) {
                g_chain_trace.Close();
                logger::write("CHAINTRACE complete -> wwmi_dx12_chain_trace.log");
            }
        }

        if (f.cl) SubmitList(f);
        {
            PROF(PROF_ET_Presence);
            g_presence.Tick();
            PortApi::RunPendingPost();
#ifdef WWMI_DEBUG
            g_tex_trace.FrameEnd();
#endif
        }
    }
}
