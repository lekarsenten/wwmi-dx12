// RtPtSkin.hlsl -- PTSKIN for the portable RT module (rt/rt_portable.cpp).
//
// With ray tracing on, the game skins characters in a compute pass (Unreal's GPU skin cache) and draws them
// through a vertex shader that only reads the result. A mod's own vertex buffers are in bind pose, so this pass
// skins them the way the game's SkinCacheUpdateBatchCS does, with the bone palette the game used for the same
// mesh section this frame. One thread per mod vertex. A second dispatch with the previous frame's palette and
// g_writeTangents = 0 gives the previous positions the game's motion vectors are made from.
//
// Inputs are the mod's own buffers, bound as raw root SRVs:
//   t0  the game's bone palette for the section: 3 float4 rows per bone (a 3x4 matrix)
//   t1  the mod's positions (vb0): float3 per vertex
//   t2  the mod's tangent frame (vb1): TangentX then TangentZ, each R8G8B8A8_SNORM
//   t3  the mod's skin weights (vb4): g_weightsPerVertex bone indices (uint8), then as many weights (unorm8);
//       8 of each, or 4 in mods made with 4 weights per vertex
//   t4  the mod's vertex colors (vb3): R8G8B8A8_UNORM
// Outputs, in the layout the game's pass-through vertex shader reads:
//   u0  skinned positions: float3 per vertex
//   u1  skinned tangent frame: TangentX then TangentZ, each R16G16B16A16_SNORM
//   u2  vertex colors as B8G8R8A8_UNORM: the game's layout reads color with red and blue swapped

ByteAddressBuffer   Bones          : register(t0);
ByteAddressBuffer   Positions      : register(t1);
ByteAddressBuffer   Vectors        : register(t2);
ByteAddressBuffer   Weights        : register(t3);
ByteAddressBuffer   Colors         : register(t4);
RWByteAddressBuffer OutPositions   : register(u0);
RWByteAddressBuffer OutTangents    : register(u1);
RWByteAddressBuffer OutColors      : register(u2);

cbuffer Params : register(b0)
{
    uint  g_vertexCount; // vertices to skin
    uint  g_boneCount;   // bones the palette holds; an index past it is ignored
    float g_scale;       // the game's own position scale for the section (1 almost always)
    uint  g_writeTangents; // 0: positions only (the previous-frame pass); u1 and u2 are not written
    uint  g_weightsPerVertex; // 8, or 4: bone indices and weights each vertex has in t3
};

/**
 * Decodes four signed-normalized bytes, as R8G8B8A8_SNORM does.
 *
 * @param packed The four bytes, x in the lowest.
 * @return The four values in [-1, 1].
 */
float4 DecodeSnorm8(uint packed)
{
    int4 bytes = int4(packed << 24, packed << 16, packed << 8, packed) >> 24;
    return max(float4(bytes) / 127.0, -1.0);
}

/**
 * Encodes two values as signed-normalized 16-bit numbers in one 32-bit word, as R16G16_SNORM does.
 *
 * @param a The value stored in the low half.
 * @param b The value stored in the high half.
 * @return The packed word.
 */
uint EncodeSnorm16Pair(float a, float b)
{
    int ia = (int)round(clamp(a, -1.0, 1.0) * 32767.0);
    int ib = (int)round(clamp(b, -1.0, 1.0) * 32767.0);
    return ((uint)ia & 0xFFFF) | ((uint)ib << 16);
}

/**
 * Normalizes a vector, leaving a zero vector as it is.
 *
 * @param v The vector.
 * @return v with length 1, or v when it has no length.
 */
float3 SafeNormalize(float3 v)
{
    float length_squared = dot(v, v);
    return length_squared > 0.0 ? v * rsqrt(length_squared) : v;
}

[numthreads(64, 1, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    uint vertex = thread_id.x;
    if (vertex >= g_vertexCount) {
        return;
    }

    // The blended bone matrix, row by row, as the game's skin shader builds it.
    uint indices_low = 0;
    uint indices_high = 0;
    uint weights_low = 0;
    uint weights_high = 0;
    if (g_weightsPerVertex == 4) {
        indices_low = Weights.Load(vertex * 8 + 0);
        weights_low = Weights.Load(vertex * 8 + 4);
    } else {
        indices_low = Weights.Load(vertex * 16 + 0);
        indices_high = Weights.Load(vertex * 16 + 4);
        weights_low = Weights.Load(vertex * 16 + 8);
        weights_high = Weights.Load(vertex * 16 + 12);
    }
    float4 row0 = 0;
    float4 row1 = 0;
    float4 row2 = 0;
    for (uint influence = 0; influence < 8; ++influence) {
        uint shift = (influence & 3) * 8;
        uint bone = ((influence < 4 ? indices_low : indices_high) >> shift) & 0xFF;
        float weight = (float)(((influence < 4 ? weights_low : weights_high) >> shift) & 0xFF) / 255.0;
        if (weight == 0.0 || bone >= g_boneCount) {
            continue;
        }
        uint bone_offset = bone * 48;
        row0 += weight * asfloat(Bones.Load4(bone_offset + 0));
        row1 += weight * asfloat(Bones.Load4(bone_offset + 16));
        row2 += weight * asfloat(Bones.Load4(bone_offset + 32));
    }

    float3 position = asfloat(Positions.Load3(vertex * 12));
    float3 skinned = float3(
        dot(row0.xyz, position) + row0.w,
        dot(row1.xyz, position) + row1.w,
        dot(row2.xyz, position) + row2.w);
    OutPositions.Store3(vertex * 12, asuint(skinned * g_scale));
    if (g_writeTangents == 0) {
        return;
    }

    float4 tangent_x = DecodeSnorm8(Vectors.Load(vertex * 8 + 0));
    float4 tangent_z = DecodeSnorm8(Vectors.Load(vertex * 8 + 4));
    float3 skinned_x = SafeNormalize(float3(dot(row0.xyz, tangent_x.xyz), dot(row1.xyz, tangent_x.xyz),
        dot(row2.xyz, tangent_x.xyz)));
    float3 skinned_z = SafeNormalize(float3(dot(row0.xyz, tangent_z.xyz), dot(row1.xyz, tangent_z.xyz),
        dot(row2.xyz, tangent_z.xyz)));
    OutTangents.Store4(vertex * 16, uint4(
        EncodeSnorm16Pair(skinned_x.x, skinned_x.y), EncodeSnorm16Pair(skinned_x.z, tangent_x.w),
        EncodeSnorm16Pair(skinned_z.x, skinned_z.y), EncodeSnorm16Pair(skinned_z.z, tangent_z.w)));

    uint color = Colors.Load(vertex * 4);
    OutColors.Store(vertex * 4, (color & 0xFF00FF00) | ((color & 0xFF) << 16) | ((color >> 16) & 0xFF));
}
