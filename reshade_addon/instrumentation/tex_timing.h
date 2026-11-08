#pragma once
#include <atomic>
#include <cstdint>

struct ID3D12Resource;

// Texture load timing, armed by a button for a fixed window: when a mod texture was first needed (a game texture
// with a rule found no mod texture ready), how long creating it took and on which thread, when its upload was
// recorded on a command list, and when it was first swapped in. One report at the end of the window.
namespace TexTiming {
inline std::atomic<bool> g_armed{ false };
inline bool Armed()
{
    return g_armed.load(std::memory_order_relaxed);
}
void Arm(double seconds);
void Stop();
void Tick();
void NoteNeeded(uint32_t rule);
void NoteCreate(
    ID3D12Resource* res, uint32_t w, uint32_t h, uint32_t mips, uint32_t fmt, double alloc_ms, double queue_ms);
void NoteFlushed(ID3D12Resource* res, unsigned long long bytes);
void NoteSwapped(ID3D12Resource* res, uint32_t rule);
void NoteClaimed(const wchar_t* ns);
}
