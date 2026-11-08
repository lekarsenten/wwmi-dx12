#pragma once
#include "../migoto/port_api.h" // DeferredTexture
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// A mod texture loaded on a worker, off the engine lock, the first time a draw asks for it; the draw keeps the game's
// texture until it is in. With A12 off none is taken: each loads where it is first asked for, as on DX11.
class TexDefer {
public:
    void Start(); // hands the port its sink
    bool Pending(int mod) const { return Count(mod) != 0; } // a texture of this mod's namespace is still loading
    bool Loading() const { return in_flight_.load(std::memory_order_acquire) != 0; } // ... of any
    unsigned Loaded() const { return loaded_.load(std::memory_order_acquire); } // textures it made, ever
    void ReturnAt(const std::wstring& fq_name, unsigned maxsize); // let go of: its next load skips mips over this side
    void Stop();
    ~TexDefer()
    {
        if (thread_.joinable()) thread_.detach();
    }

private:
    static bool Offer(const PortApi::DeferredTexture& t, void* user); // drawing thread, under the engine lock
    void Run();
    struct Table {
        std::unique_ptr<std::atomic<uint32_t>[]> v;
        int n;
    };
    uint32_t Count(int mod) const
    {
        const Table* t = table_.load(std::memory_order_acquire);
        return (t && mod >= 0 && mod < t->n) ? t->v[mod].load(std::memory_order_acquire) : 0;
    }
    std::atomic<uint32_t>& Slot(int mod); // under mtx_; grows the table, the old one is kept
    std::atomic<Table*> table_{ nullptr };
    std::vector<std::unique_ptr<Table>> tables_; // under mtx_: every table ever published
    std::atomic<unsigned> gen_{ 0 }; // written under mtx_: the ini load the counts belong to
    std::atomic<unsigned> loaded_{ 0 };
    std::atomic<unsigned> in_flight_{ 0 }; // offered, not yet published or dropped
    struct Job {
        PortApi::DeferredTexture tex;
        int mod; // -1: no mod draws from its namespace
        unsigned gen;
    };
    std::deque<Job> queue_; // under mtx_
    struct Return {
        unsigned maxsize;
        unsigned gen;
    };
    std::unordered_map<std::wstring, Return> returns_; // under mtx_
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false; // under mtx_
    std::thread thread_; // started under mtx_
};
extern TexDefer g_tex_defer;

// The namespaces of the mods on screen with none of their textures still loading, each once.
std::vector<std::wstring> NamespacesOnScreen();
