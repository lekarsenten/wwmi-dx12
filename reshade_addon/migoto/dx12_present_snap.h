#pragma once
// [Present] under frame generation. The step that runs it is at the game's frame end, with no back buffer, so what
// reaches the screen is PresentRedraw at each present, which skips dispatches, copies, uploads and UAV clears. A
// resource one of those writes after a draw of the same step read it holds only its last content by the redraw;
// once seen, such a resource is copied at each of its reads in the step, and the redraw binds the copy for the read
// of the same number. WWMI's text printer lays out every text into one buffer before drawing it.
#include <d3d11.h>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MigotoPort {

struct PipeState;

class PresentSnapshots {
public:
    enum class Mode { Off, Capture, Replay };
    // A step's [Present] pre list (Capture when its draws reach no screen, else Off) or a redraw (Replay); the reads
    // count from zero. A new load generation lets every copy go.
    void Begin(Mode mode, unsigned generation);
    void Resume(); // the step's post list, after its pre list
    void End();
    void NoteWrite(ID3D11Resource* res); // dispatch UAVs, UAV clears, copy, resolve and upload destinations
    void BeforeDraw(ID3D11DeviceContext* ctx, PipeState& st);
    void AfterDraw(PipeState& st);

private:
    struct View {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        ID3D11ShaderResourceView* view = nullptr;
    };
    struct Copy {
        ID3D11Resource* res = nullptr;
        D3D11_BUFFER_DESC buffer{};
        D3D11_TEXTURE2D_DESC texture{};
        std::vector<View> views;
    };
    struct Swap {
        unsigned stage;
        unsigned slot;
        ID3D11ShaderResourceView* was;
    };
    bool Mine() const;
    void Forget();
    Copy* CopyFor(ID3D11Resource* src, unsigned nth);
    ID3D11ShaderResourceView* ViewOf(Copy& c, ID3D11ShaderResourceView* like);

    std::atomic<Mode> mode_{ Mode::Off };
    Mode step_ = Mode::Off;
    std::atomic<unsigned long> thread_{ 0 };
    unsigned generation_ = ~0u;
    std::unordered_set<ID3D11Resource*> read_; // this step: read by a draw since its last write
    std::unordered_set<ID3D11Resource*> rewritten_; // written after a read, in any step: copied at each read
    std::unordered_map<ID3D11Resource*, unsigned> reads_; // this pass: draws that read it so far
    std::unordered_map<ID3D11Resource*, std::vector<Copy>> copies_;
    std::unordered_map<ID3D11Resource*, unsigned> captured_; // the last step's copies, per resource
    std::vector<Swap> swaps_;
};
extern PresentSnapshots& g_present_snapshots;

} // namespace MigotoPort
