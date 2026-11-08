#pragma once
#include <atomic>
#include <cstdint>
#include <reshade.hpp>

// The present path: the frame's own bookkeeping, the [Present] command list, and everything that only makes sense
// once per frame. The per-frame step runs once per game frame: at a present on the game's own thread, or at the
// game's frame end on its thread while presents come from another one (frame generation), where [Present]'s draws
// to the back buffer are left to the presents' redraw.
class FrameStepper {
public:
    void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
    // The game submitted the list that ends its frame, on its own thread while presents come from another one: the
    // per-frame step runs here, before that list reaches the queue.
    void FrameEnd(reshade::api::command_queue* queue);
    // The swapchain is gone: no step reaches it any more.
    void ForgetSwapchain();

private:
    void Step(reshade::api::command_queue* queue, reshade::api::swapchain* sc, bool at_present);
    std::atomic<reshade::api::swapchain*> step_sc_{ nullptr };
    bool other_thread_ = false; // FRAMESTEP: where the step runs, as last logged
    unsigned long long notice_until_ = 0; // the stopped notice: shown until this tick, ~0 once taken down
    int geom_lines_ = 0; // WwmiGeom: the first frames' sub-draw counts, logged
};
extern FrameStepper g_frame_step;

// FrameStepper::FrameEnd inside the SEH guard (the guard holds no object that needs unwinding).
void FrameEndStepGuarded(reshade::api::command_queue* queue);
