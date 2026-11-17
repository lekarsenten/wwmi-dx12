#pragma once
#include "../migoto/port_api.h" // DeferredTexture
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// A mod texture loaded on one of a few workers, off the engine lock, the first time a draw asks for it; the draw
// keeps the game's texture until it is in. With A12 off none is taken: each loads where it is first asked for, as on DX11.
class TexDefer {
public:
    void Start(); // hands the port its sink
    bool Pending(int mod) const { return Count(mod) != 0; } // a texture of this mod's namespace is still loading
    bool Loading() const { return in_flight_.load(std::memory_order_acquire) != 0; } // ... of any
    unsigned Loaded() const { return loaded_.load(std::memory_order_acquire); } // textures it made, ever
    void ReturnAt(const std::wstring& fq_name, unsigned maxsize); // let go of: its next load skips mips over this side

    /**
     * Tells whether a draw of a mod stays hidden because the mod has just come on screen and its textures are
     * still loading (ini tex_hide_loading). Call it for every draw a mod's section issues, after the section's
     * texture binds. Once the mod has been shown, later loads (an outfit toggle, a texture coming back at
     * another size) never hide it again until it has left the screen.
     *
     * @param mod The mod's registry id.
     * @return True when the draw must not be issued.
     */
    bool HidesDraw(int mod);

    /**
     * Tells whether the game's own draw that a mod's section ran for stays hidden: during the first frames of
     * the mod's appearance (its section may not draw yet) and while HidesDraw hides the mod. Without it the
     * game's model would flash before the mod shows. Call it once per section run, after the section.
     *
     * @param mod The mod's registry id.
     * @return True when the game's draw must not be issued.
     */
    bool HidesGameDraw(int mod);
    void Stop();
    ~TexDefer()
    {
        for (std::thread& worker : workers_)
            if (worker.joinable()) worker.detach();
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
    /** One mod's time on screen, for HidesDraw. Frames are g_frame_id values. */
    struct Appearance {
        std::atomic<uint32_t> last_draw{ 0 }; // the last frame a draw of the mod was asked about
        std::atomic<uint32_t> first_draw{ 0 }; // the frame this appearance began in
        std::atomic<uint32_t> last_pending{ 0 }; // the last frame a texture was loading in; 0 = none yet
        std::atomic<uint32_t> hidden_since{ 0 }; // the frame the mod was first hidden in; 0 = it was not
        std::atomic<bool> shown{ false }; // the mod has been drawn: it is not hidden again
    };
    static constexpr int kAppearances = 1024; // mods with a higher id are never hidden
    static constexpr uint32_t kGoneFrames = 30; // not drawn for this long: the next draw starts a new appearance
    static constexpr uint32_t kSettleFrames = 3; // loads of one appearance may start a few frames apart
    Appearance* TouchAppearance(int mod);
    std::array<Appearance, kAppearances> appearances_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false; // under mtx_
    // Textures loaded at the same time. 4 made a burst about 2.8 times shorter than one worker, but each load
    // slower and one frame-rate drop while switching teams quickly; 2 is the user's choice (2026-11-17).
    static constexpr unsigned kWorkers = 2;
    std::vector<std::thread> workers_; // started under mtx_
    /** What the workers loaded since the queue was last empty, for one log line. Under mtx_. */
    struct Burst {
        unsigned working = 0; // jobs being loaded right now
        unsigned jobs = 0; // jobs finished
        unsigned made = 0;
        unsigned stale = 0;
        unsigned smaller = 0;
        long long start = 0; // performance counter when the first job was taken
        long long locked = 0; // counter ticks spent publishing, under the engine lock
        long long longest = 0; // counter ticks of the slowest load
        long long load_sum = 0; // counter ticks of all loads added up
        std::wstring first_namespace;
        bool several_namespaces = false;
    };
    Burst burst_;
};
extern TexDefer g_tex_defer;

// The namespaces of the mods on screen with none of their textures still loading, each once.
std::vector<std::wstring> NamespacesOnScreen();
