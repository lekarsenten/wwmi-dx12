#pragma once
#include <cstdint>

// Lineage: a texture copied, whole or streamed down, is the same texture: the destination inherits the source's
// identity (a belief until its own mip0 says otherwise), and a stream copy between two sizes is a lower->target
// link the LOD cache learns.
class TexLineage {
public:
    void Propagate(uint64_t srcH, uint64_t dstH, bool same_image);
    void Alias(uint64_t smallerH, uint64_t biggerH, uint32_t steps);
};
extern TexLineage g_tex_lineage;
