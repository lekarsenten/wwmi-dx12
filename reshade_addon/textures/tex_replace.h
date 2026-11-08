#pragma once
#include "../migoto/port_api.h" // LoadedTexture
#include "../hooks/stamp_ledger.h" // StampTally
#include "tex_encode.h" // TexEncoder::Stream
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The texture behind a mod's [Resource] swapped for another while it is on screen; and the slots of a mod texture
// that is about to be freed given back, whatever freed it.
class TexReplace {
public:
    enum class Pass : uint8_t {
        None,
        Same, // every texture of the mods on screen loaded again, as it is
        Half, // ... at half size, from the mips in its file
        Full, // ... the half-size ones at full size again
        Sized // the textures a client ordered, each at the size it asked for
    };
    struct Order {
        PortApi::LoadedTexture was; // its reference moves in; was.load.maxsize is what the loader gets
        unsigned shift; // > 0: re-encoded from it on the graphics card, this many levels down, instead
        uint64_t bytes; // the new one's
        bool evict = false; // none instead: the [Resource] lets go of it (Swapped hears nullptr)
        bool from_file = false; // shift counts from the file, read and re-encoded a band at a time
    };
    class Client {
    public:
        virtual bool Owns(const std::wstring& fq_name) const = 0; // the test passes leave it alone
        virtual void Swapped(const std::wstring& fq_name, ID3D12Resource* now) = 0;
        virtual void Kept(const std::wstring& fq_name) = 0; // its re-encode refused: the texture was not at rest
        virtual void Held(const std::wstring& fq_name) = 0; // not let go: another [Resource] holds it by `= ref`
        virtual void Ended() = 0;

    protected:
        ~Client() = default;
    };
    void SetClient(Client* c) { client_ = c; }
    bool Start(std::vector<Order>& orders); // a Sized pass; false, orders untouched, while another runs
    void Request(Pass p) { request_.store(p, std::memory_order_release); }
    void SetStress(bool on) { stress_.store(on, std::memory_order_release); }
    bool Stress() const { return stress_.load(std::memory_order_acquire); }
    bool Busy() const { return busy_.load(std::memory_order_acquire); }
    std::chrono::steady_clock::time_point LastEnd() const { return last_end_; } // present thread
    bool Halved() const { return halved_any_.load(std::memory_order_acquire); }
    bool HalfSize(const std::wstring& fq_name) const { return halved_.count(fq_name) != 0; } // present thread
    void Tick(ID3D12CommandQueue* queue); // present thread, after ModTexturesTick
    void ClearForParse(); // present thread, at a parse's set build: every texture out of the store, its slot quarantined
    void Stop();
    ~TexReplace()
    {
        if (thread_.joinable()) thread_.detach();
    }

private:
    struct Job {
        PortApi::LoadedTexture was; // its reference held
        unsigned gen;
        uint64_t bytes;
        unsigned shift = 0; // as the Order's
        bool from_file = false; // as the Order's
    };
    struct Loaded {
        Job job;
        ID3D11Resource* fresh; // a reference held; null = the load failed
        uint64_t stamp; // GpuStamp() once its copy was on a command list; 0 = not yet
        uint64_t enc = 0; // re-encoded: the encoder's fence value it waits for
        std::shared_ptr<TexEncoder::Stream> stream; // from_file: read and re-encoded band by band
        const char* why = nullptr; // from_file: why it was not read or not finished
    };
    struct PassTally {
        Pass pass = Pass::None;
        unsigned gen = 0;
        unsigned mods = 0;
        unsigned queued = 0;
        unsigned swapped = 0;
        unsigned failed = 0;
        unsigned stale = 0; // the [Resource] no longer held what was loaded
        unsigned encoded = 0;
        unsigned banded = 0; // ... of them from their files
        unsigned in_use = 0; // not re-encoded: the game had it in another state
        unsigned evicted = 0;
        uint64_t bytes = 0;
        StampTally slots;
        std::chrono::steady_clock::time_point t0;
    };
    struct RetireWatch {
        unsigned replaced = 0;
        unsigned retired = 0;
        unsigned most_frames = 0;
        uint32_t last_swap_frame = 0;
        StampTally late; // slots still stamped with one when it died
    };
    struct QuarantinedSrv {
        D3D12_CPU_DESCRIPTOR_HANDLE srv;
        unsigned epoch;
        uint32_t frame;
    };
    static void OnRetire(ID3D12Resource* res);
    void Run();
    void SweepDying();
    void StartPass(Pass p);
    void Queue(std::vector<Job>& jobs);
    void SubmitEncodes(ID3D12CommandQueue* queue);
    void StartStream(Loaded& x, std::vector<Loaded>& ready);
    void EndStreams(unsigned gen);
    void Evict();
    void SwapReady(std::vector<Loaded>& ready);
    void EndPass(const char* why);
    void Quarantine(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch);
    bool Forget(ID3D12Resource* res); // out of the texture store, its SRV slot quarantined
    void ReportRetired(bool timeout);

    std::atomic<Pass> request_{ Pass::None };
    std::atomic<bool> stress_{ false };
    std::atomic<bool> busy_{ false };
    std::atomic<bool> halved_any_{ false }; // halved_ is not empty
    bool observing_ = false; // present thread from here down
    unsigned seen_gen_ = 0;
    PassTally pass_;
    std::chrono::steady_clock::time_point last_end_{};
    std::unordered_set<std::wstring> halved_; // [Resource]s now holding a half-size copy
    Client* client_ = nullptr;
    std::vector<Job> encodes_; // to re-encode, not yet submitted
    std::vector<Loaded> streaming_; // from_file, its bands being read and re-encoded
    std::vector<Loaded> encoding_; // submitted to the encoder
    std::vector<Job> evicts_; // to let go of
    std::unordered_map<ID3D12Resource*, uint32_t> replaced_; // swapped out, not yet dead: -> the frame of the swap
    RetireWatch watch_;
    std::vector<QuarantinedSrv> quarantine_;

    std::mutex dying_mtx_;
    std::unordered_set<ID3D12Resource*> watched_; // under dying_mtx_: swapped out, their store key already gone
    std::unordered_set<ID3D12Resource*> dying_; // under dying_mtx_: mod textures whose last wrapper reference dropped

    std::mutex mtx_; // the worker's side
    std::condition_variable cv_;
    std::deque<Job> jobs_; // under mtx_
    std::vector<Loaded> loaded_; // under mtx_
    uint64_t pending_bytes_ = 0; // under mtx_: loaded, not yet swapped in
    unsigned working_ = 0; // under mtx_: jobs taken, not yet in loaded_
    bool stop_ = false; // under mtx_
    std::thread thread_; // started under mtx_
};
extern TexReplace g_tex_replace;
