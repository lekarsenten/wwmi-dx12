#pragma once
// SHAPEKEY DECISION RING -- the chain's last ~15 s of choices, dumped AFTER the fact.
//
// J1 records 240 frames from the press, so it catches an ult's cast only if the press comes first; an ult
// is over before anyone can arm it. This is the other way round: every decision the chain makes is written into a fixed ring as it
// is made -- which buffer u0 resolved to and by which rule, which u1 and whether it was found by identity
// or left unbound, every relocation, every u1 bind result -- at no cost worth measuring (a mutex and 40
// bytes per dispatch), and a key or button dumps the ring once the break has been SEEN. The dump is the
// last kRing entries oldest first with the frame and the age in ms, so the cast's frames can be found.
//
// HOTKEYS (present_engine.cpp): Numpad 0 toggles CAPTURE MODE (logged); while it is on, Numpad 1 dumps
// this ring and writes a USER-MARK line. Nothing fires with the mode off, so ordinary play cannot trigger
// a dump by accident. Overlay: J3 dumps it too. Further capture keys hang off the same mode.
#include <cstdint>
#include "diag_gate.h"

namespace SkRing {
enum Kind : uint8_t {
    Resolve = 1, // a = cs hash, b = u0 resource, c = size | desc hash << 32,
    U1 = 2, // a = owner hash | want_scale << 32, b = u1 resource, c = u1 bytes, d = ident verdict + 1 | live1 << 8
    Reloc = 3, // a = hash, b = the game's buffer, c = bytes after, d = nth relocation of this hash
    U1Bind = 4, // a = mod id, b = u1 resource, c = bytes, d = 1 bound / 0 not captured
    U1Reg = 5, // a = owner hash | want_scale << 32, b = heap, c = resource in the slot,
    // d = slot | step << 24 | root param << 32 | is table << 40 | size >> 4 << 41 (ResolveBound's BindTrace)
};
WWMI_DEBUG_FN void Note(Kind k, uint64_t a, uint64_t b, uint64_t c, uint64_t d) WWMI_DEBUG_BODY();
// The whole ring to the log, oldest first, under a header naming who asked.
WWMI_DEBUG_FN void Dump(const char* why) WWMI_DEBUG_BODY();
}
