#pragma once
#include <cstdint>

struct IDXGIAdapter3;
struct ID3D12Device;

struct VramReading {
    uint64_t budget = 0; // LOCAL: what Windows lets the process use before it starts freezing it
    uint64_t used = 0; // ... and what the process uses, game and add-on together
    uint64_t shared_budget = 0; // NON_LOCAL
    uint64_t shared_used = 0;
};

// The process's video memory, as Windows accounts it. Present thread.
class VramMeter {
public:
    bool Read(VramReading* out); // false until the adapter is found
    bool BudgetChanged(); // Windows changed the budget since the last call

private:
    void Open();

    IDXGIAdapter3* adapter_ = nullptr;
    ID3D12Device* for_ = nullptr;
    void* event_ = nullptr;
    unsigned long cookie_ = 0;
};
extern VramMeter g_vram;
