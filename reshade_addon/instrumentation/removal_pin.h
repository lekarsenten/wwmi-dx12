#pragma once
// REMOVAL PIN -- which submission removed the device, stated rather than inferred.
//
// INVALID_CALL (887a0001) is CPU-side: the runtime removes the device INSIDE the call it rejects, and DRED
// breadcrumbs are empty for it by design. The DRED watchdog says WHEN to within a frame or two and nothing
// about WHICH call.
//
// The runtime's one removal site for an invalid command stream is ExecuteCommandLists: a list whose
// Close failed, or that recorded a call its type does not allow, is rejected THERE and the device goes
// with it. ReShade fires execute_command_list per list BEFORE the native call, so a device that is
// already removed at that entry was removed by a call between the previous entry and this one -- in
// practice the previous ExecuteCommandLists. This keeps a ring of those entries (list, its type, the
// queue's type, frame, age) and, for every list, a tally of what OUR addon recorded onto it in the
// current recording (replacement draws, port draws / dispatches / barriers / copies, relocation seeds,
// shapekey copies). At the first failed check the ring is written newest first with each entry's tally:
// the newest entry is the rejected submission, its tally is what we put there, and a graphics draw
// tallied on a list whose type is not DIRECT would be the answer in one line.
//
// The runtime also fails Close for a list that recorded something illegal for it (a query left open, a
// call its type does not allow, a render pass left open), and the engine asserts on that on the RHI
// thread -- no device removal, no DRED, no REMOVAL-PIN. So the pin also keeps a ring of the last CLOSES
// (ReShade's close_command_list fires before the native Close; the thread, list, type and tally are
// recorded) and the STALL watchdog dumps both rings and the unexecuted tallies at every stall it reports.
// The failing Close is the newest close entry on the stalled thread.
//
// A game buffer the chain names in a copy or a dispatch on the game's list is freed by the game on ITS
// fence, which covers the game's last use of it and not ours. A submission that names a freed allocation
// is rejected by the kernel and the device goes with INVALID_CALL, with no breadcrumb, no page fault and
// no debug-layer message. So every game buffer we name is recorded (Refs) with the list and the frame;
// on_destroy_resource asks the pin, and a destruction within a few frames of that naming is logged as
// DEAD-REF.
//
// Diagnostics: built only with WWMI_DEBUG and, in that build, live only while the debug log is on (every
// entry returns at once otherwise). Nothing here changes what is drawn. GetDeviceRemovedReason is a
// dxgkrnl round trip (~40 us): the watchdog thread (watchdogs.cpp) raises g_device_removed, from a fence
// event the runtime signals at removal or its 200 ms poll, and the pin asks the device only once that is up.
#include <d3d12.h>
#include <atomic>
#include <cstdint>
// No reshade.hpp: dx12_pipeline.cpp (the migoto lib) includes this.
namespace reshade::api {
struct command_list;
}

namespace RemovalPin {
inline std::atomic<bool> g_device_removed{ false };
#ifdef WWMI_DEBUG
void Ours(const void* native_list, const char* what);
void Mark(const void* native_list, const char* what);
// The list was Reset: its tally belongs to a recording that is gone.
void OnReset(const void* native_list);
void OnClose(const void* native_list);
void Refs(const void* native_list, const void* resource, const char* what);
void OnDestroyResource(const void* resource);
void OnExecute(ID3D12Device* dev, const void* native_list, int list_type, int queue_type, const char* who);
// A check with no submission (present entry): the same report if the device has gone since the last one.
void Check(ID3D12Device* dev, const char* where);
void Describe(const void* native_list, char* out, size_t cap);
void DumpOnStall(unsigned long stalled_tid);
#else
inline void Ours(const void*, const char*) {}
inline void Mark(const void*, const char*) {}
inline void OnReset(const void*) {}
inline void OnClose(const void*) {}
inline void Refs(const void*, const void*, const char*) {}
inline void OnDestroyResource(const void*) {}
inline void OnExecute(ID3D12Device*, const void*, int, int, const char*) {}
inline void Check(ID3D12Device*, const char*) {}
inline void Describe(const void*, char* out, size_t cap)
{
    if (out && cap) out[0] = 0;
}
inline void DumpOnStall(unsigned long) {}
#endif
}
