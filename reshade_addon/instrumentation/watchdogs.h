#pragma once
// The two watchdog threads: the stall watchdog (no present for a second while focused -> STALL + the present
// thread's stack) and the removal poller that writes a device removal (DRED, the debug layer) into our log.
#include <d3d12.h>
#include <reshade.hpp>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

class Watchdogs {
public:
    // Every present: the heartbeat the stall thread watches; the first tick starts it.
    void StallTick(bool focused);
    // Once, from the first present with a device.
    void StartRemoval(reshade::api::swapchain* sc);
    // destroy_swapchain / destroy_device / uninit: no thread of this DLL may outlive its mapping.
    void Stop();

private:
    struct WatchdogThread {
        std::thread t;
        ~WatchdogThread()
        {
            if (t.joinable()) t.detach();
        }
    };
    static void DumpDeviceRemoval(ID3D12Device* d3d, HRESULT rr);
    void StallRun();

    std::atomic<uint32_t> ticks_{ 0 };
    std::atomic<unsigned long> present_tid_{ 0 };
    std::atomic<bool> focused_{ false };
    std::atomic<bool> stall_started_{ false };
    std::atomic<bool> stall_stop_{ false };
    WatchdogThread stall_thread_;
    std::mutex stall_thread_mutex_; // start (present thread) vs stop (the destroy / uninit thread)
    std::atomic<bool> removal_started_{ false };
    std::atomic<bool> removal_stop_{ false };
};
extern Watchdogs g_watchdogs;
