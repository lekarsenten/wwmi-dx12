// RtPalCopy.hlsl -- PTSKIN for the portable RT module (rt/rt_portable.cpp).
//
// Copies a bone palette, word by word, into a buffer of the add-on. It runs on the game's compute list right
// before the game's own skin dispatch for a mesh section, while the game's palette is bound and valid. The
// game reuses the palette's memory once the frame is done, so the mod draws read this copy instead: on the GPU
// it is always the palette of the frame being drawn, whatever order the command lists were recorded in.
//
//   t0  the palette to copy (the game's, or the add-on's current copy when it becomes the previous one)
//   u0  where it goes
// It uses the PTSKIN root signature: b0 constants, root SRVs t0..t4, root UAVs u0..u2.

ByteAddressBuffer   Source      : register(t0);
RWByteAddressBuffer Destination : register(u0);

cbuffer Params : register(b0)
{
    uint  g_wordCount; // 32-bit words to copy
    uint3 g_unused;
};

[numthreads(64, 1, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    uint word = thread_id.x;
    if (word >= g_wordCount) {
        return;
    }
    Destination.Store(word * 4, Source.Load(word * 4));
}
