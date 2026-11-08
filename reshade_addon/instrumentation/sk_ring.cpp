// sk_ring.cpp -- see sk_ring.h.
#include "../addon_common.h"
#include "sk_ring.h"
#include <mutex>

namespace {
struct Entry {
    uint32_t frame;
    uint32_t tick;
    uint8_t kind;
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
};
constexpr unsigned kRing = 8192; // ~530 kept entries/s with one modded character on screen -> ~15 s
std::mutex g_mtx;
Entry g_ring[kRing];
uint64_t g_seq = 0;
}

namespace SkRing {

void Note(Kind k, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    std::lock_guard<std::mutex> l(g_mtx);
    Entry& e = g_ring[g_seq % kRing];
    e.frame = g_frame_id;
    e.tick = (uint32_t)GetTickCount64();
    e.kind = (uint8_t)k;
    e.a = a;
    e.b = b;
    e.c = c;
    e.d = d;
    ++g_seq;
}

void Dump(const char* why)
{
    std::lock_guard<std::mutex> l(g_mtx);
    const uint32_t now = (uint32_t)GetTickCount64();
    const uint64_t n = g_seq < kRing ? g_seq : kRing;
    logger::write("SKRING DUMP (%s): %llu entr%s, oldest first; age is ms before this line", why ? why : "?",
        (unsigned long long)n, n == 1 ? "y" : "ies");
    for (uint64_t i = 0; i < n; ++i) {
        const Entry& e = g_ring[(g_seq - n + i) % kRing];
        const uint32_t age = now - e.tick;
        switch (e.kind) {
        case Resolve: {
            const unsigned geo = (unsigned)(e.d & 0xF);
            const unsigned why_ = (unsigned)((e.d >> 4) & 0xF);
            const unsigned live = (unsigned)((e.d >> 8) & 1);
            const unsigned decl = (unsigned)((e.d >> 9) & 1);
            const int push = (int)((e.d >> 16) & 0xFF) - 128;
            const int owner = (int)((e.d >> 24) & 0xFF) - 128;
            const unsigned claims = (unsigned)((e.d >> 32) & 0xFF);
            const unsigned gy = (unsigned)((e.d >> 40) & 0xFFFFF);
            logger::write(
                "SKRING f=%u -%u ms RESOLVE cs=%016llx u0=%p size=%u desc=%08x geo=%u(%s) why=%u live=%u decl=%u "
                "push=%d owner=%d claims=%u groups.y=%u",
                e.frame, age, (unsigned long long)e.a, (void*)e.b, (unsigned)(e.c & 0xFFFFFFFFu), (unsigned)(e.c >> 32),
                geo,
                geo == 0       ? "no-range"
                    : geo == 1 ? "record-fits"
                    : geo == 2 ? "REPLACED-by-live"
                    : geo == 3 ? "DROPPED-none-fits"
                               : "DROPPED-ambiguous",
                why_, live, decl, push, owner, claims, gy);
            break;
        }
        case U1: {
            const int ident = (int)(e.d & 0xFF) - 1;
            logger::write("SKRING f=%u -%u ms U1 owner=%08x wantScale=%08x u1=%p bytes=%u ident=%d(%s) live1=%u",
                e.frame, age, (unsigned)(e.a & 0xFFFFFFFFu), (unsigned)(e.a >> 32), (void*)e.b, (unsigned)e.c, ident,
                ident == 1        ? "USING-IT"
                    : ident == 0  ? "none-live"
                    : ident == -1 ? "record-or-off"
                                  : "AMBIGUOUS",
                (unsigned)((e.d >> 8) & 1));
            break;
        }
        case Reloc:
            logger::write("SKRING f=%u -%u ms RELOC hash=%08x game-buffer=%p -> %llu B (relocation #%llu)", e.frame,
                age, (unsigned)e.a, (void*)e.b, (unsigned long long)e.c, (unsigned long long)e.d);
            break;
        case U1Bind:
            logger::write("SKRING f=%u -%u ms U1BIND mod=%llu u1=%p bytes=%llu %s", e.frame, age,
                (unsigned long long)e.a, (void*)e.b, (unsigned long long)e.c, e.d ? "bound" : "NOT CAPTURED");
            break;
        case U1Reg: {
            const unsigned step = (unsigned)((e.d >> 24) & 0xFF);
            logger::write("SKRING f=%u -%u ms U1REG owner=%08x wantScale=%08x step=%u(%s) param=%u table=%u heap=%llx "
                          "slot=%u slot-res=%p slot-size=%llu",
                e.frame, age, (unsigned)(e.a & 0xFFFFFFFFu), (unsigned)(e.a >> 32), step,
                step == 0        ? "resolved-foreign"
                    : step == 2  ? "no-layout"
                    : step == 3  ? "no-regmap"
                    : step == 5  ? "reg-unmapped"
                    : step == 6  ? "root-nothing-pushed"
                    : step == 7  ? "root-destroyed"
                    : step == 8  ? "no-table"
                    : step == 9  ? "no-heap"
                    : step == 10 ? "SLOT-NO-RECORD"
                    : step == 11 ? "slot-res-destroyed"
                                 : "?",
                (unsigned)((e.d >> 32) & 0xFF), (unsigned)((e.d >> 40) & 1), (unsigned long long)e.b,
                (unsigned)(e.d & 0xFFFFFF), (void*)e.c, (unsigned long long)((e.d >> 41) << 4));
            break;
        }
        default:
            logger::write("SKRING f=%u -%u ms kind=%u a=%llx b=%llx c=%llx d=%llx", e.frame, age, (unsigned)e.kind,
                (unsigned long long)e.a, (unsigned long long)e.b, (unsigned long long)e.c, (unsigned long long)e.d);
        }
    }
    logger::write("SKRING DUMP end");
}

} // namespace SkRing
