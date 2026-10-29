// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "ffx_sssr_resources.h"

#if defined(FFX_GPU)
#include "ffx_core.h"

#ifndef FFX_PREFER_WAVE64
#define FFX_PREFER_WAVE64
#endif // #ifndef FFX_PREFER_WAVE64

#if defined(SSSR_BIND_CB_SSSR)
    layout (set = 0, binding = SSSR_BIND_CB_SSSR, std140) uniform cbSSSR_t
    {
        FfxFloat32Mat4  invViewProjection;
        FfxFloat32Mat4  projection;
        FfxFloat32Mat4  invProjection;
        FfxFloat32Mat4  viewMatrix;
        FfxFloat32Mat4  invView;
        FfxFloat32Mat4  prevViewProjection;
        FfxUInt32x2     renderSize;
        FfxFloat32x2    inverseRenderSize;
        FfxFloat32      normalsUnpackMul;
        FfxFloat32      normalsUnpackAdd;
        FfxUInt32       roughnessChannel;
        FfxBoolean      isRoughnessPerceptual;
        FfxFloat32      iblFactor;
        FfxFloat32      temporalStabilityFactor;
        FfxFloat32      depthBufferThickness;
        FfxFloat32      roughnessThreshold;
        FfxFloat32      varianceThreshold;
        FfxUInt32       frameIndex;
        FfxUInt32       maxTraversalIntersections;
        FfxUInt32       minTraversalOccupancy;
        FfxUInt32       mostDetailedMip;
        FfxUInt32       samplesPerQuad;
        FfxUInt32       temporalVarianceGuidedTracingEnabled;
    } cbSSSR;

FfxFloat32Mat4 InvViewProjection()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.invViewProjection;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32Mat4 Projection()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.projection;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32Mat4 InvProjection()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.invProjection;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32Mat4 ViewMatrix()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.viewMatrix;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32Mat4 InvView()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.invView;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32Mat4 PrevViewProjection()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.prevViewProjection;
#else
    return FfxFloat32Mat4(0.0f);
#endif
}

FfxFloat32 NormalsUnpackMul()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.normalsUnpackMul;
#else
    return 0.0f;
#endif
}

FfxFloat32 NormalsUnpackAdd()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.normalsUnpackAdd;
#else
    return 0.0f;
#endif
}

FfxUInt32 RoughnessChannel()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.roughnessChannel;
#else
    return 0;
#endif
}

FfxBoolean IsRoughnessPerceptual()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.isRoughnessPerceptual;
#else
    return false;
#endif
}

FfxUInt32x2 RenderSize()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.renderSize;
#else
    return FfxUInt32x2(0);
#endif
}

FfxFloat32x2 InverseRenderSize()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.inverseRenderSize;
#else
    return FfxFloat32x2(0.0f);
#endif
}

FfxFloat32 IBLFactor()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.iblFactor;
#else
    return 0.0f;
#endif
}

FfxFloat32 TemporalStabilityFactor()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.temporalStabilityFactor;
#else
    return 0.0f;
#endif
}

FfxFloat32 DepthBufferThickness()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.depthBufferThickness;
#else
    return 0.0f;
#endif
}

FfxFloat32 RoughnessThreshold()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.roughnessThreshold;
#else
    return 0.0f;
#endif
}

FfxFloat32 VarianceThreshold()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.varianceThreshold;
#else
    return 0.0f;
#endif
}

FfxUInt32 FrameIndex()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.frameIndex;
#else
    return 0;
#endif
}

FfxUInt32 MaxTraversalIntersections()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.maxTraversalIntersections;
#else
    return 0;
#endif
}

FfxUInt32 MinTraversalOccupancy()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.minTraversalOccupancy;
#else
    return 0;
#endif
}

FfxUInt32 MostDetailedMip()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.mostDetailedMip;
#else
    return 0;
#endif
}

FfxUInt32 SamplesPerQuad()
{
#if defined SSSR_BIND_CB_SSSR
    return cbSSSR.samplesPerQuad;
#else
    return 0;
#endif
}

FfxBoolean TemporalVarianceGuidedTracingEnabled()
{
#if defined SSSR_BIND_CB_SSSR
    return FfxBoolean(cbSSSR.temporalVarianceGuidedTracingEnabled);
#else
    return false;
#endif
}

#endif // #if defined(SSSR_BIND_CB_SSSR)

layout (set = 0, binding = 1000) uniform sampler s_EnvironmentMapSampler;
layout (set = 0, binding = 1001) uniform sampler s_LinearSampler;

// SRVs
#if defined SSSR_BIND_SRV_INPUT_COLOR
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_COLOR)                   uniform texture2D r_input_color;
#endif
#if defined SSSR_BIND_SRV_INPUT_DEPTH
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_DEPTH)                   uniform texture2D r_input_depth;
#endif
#if defined SSSR_BIND_SRV_DEPTH_HIERARCHY
    layout (set = 0, binding = SSSR_BIND_SRV_DEPTH_HIERARCHY)               uniform texture2D r_depth_hierarchy;
#endif
#if defined SSSR_BIND_SRV_INPUT_NORMAL
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_NORMAL)                  uniform texture2D r_input_normal;
#endif
#if defined SSSR_BIND_SRV_INPUT_MATERIAL_PARAMETERS
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_MATERIAL_PARAMETERS)     uniform texture2D r_input_material_parameters;
#endif
#if defined SSSR_BIND_SRV_INPUT_ENVIRONMENT_MAP
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_ENVIRONMENT_MAP)         uniform texture2D r_input_environment_map;   // ao-vk: rvk's probe atlas
#endif
#if defined SSSR_BIND_SRV_RADIANCE
    layout (set = 0, binding = SSSR_BIND_SRV_RADIANCE)                      uniform texture2D r_radiance;
#endif
#if defined SSSR_BIND_SRV_RADIANCE_HISTORY
    layout (set = 0, binding = SSSR_BIND_SRV_RADIANCE_HISTORY)              uniform texture2D r_radiance_history;
#endif
#if defined SSSR_BIND_SRV_VARIANCE
    layout (set = 0, binding = SSSR_BIND_SRV_VARIANCE)                      uniform texture2D r_variance;
#endif
#if defined SSSR_BIND_SRV_EXTRACTED_ROUGHNESS
    layout (set = 0, binding = SSSR_BIND_SRV_EXTRACTED_ROUGHNESS)           uniform texture2D r_extracted_roughness;
#endif
#if defined SSSR_BIND_SRV_SOBOL_BUFFER
    layout (set = 0, binding = SSSR_BIND_SRV_SOBOL_BUFFER)                  uniform utexture2D r_sobol_buffer;
#endif
#if defined SSSR_BIND_SRV_SCRAMBLING_TILE_BUFFER
    layout (set = 0, binding = SSSR_BIND_SRV_SCRAMBLING_TILE_BUFFER)        uniform utexture2D r_scrambling_tile_buffer;
#endif
#if defined SSSR_BIND_SRV_BLUE_NOISE_TEXTURE
    layout (set = 0, binding = SSSR_BIND_SRV_BLUE_NOISE_TEXTURE)            uniform texture2D r_blue_noise_texture;
#endif
#if defined SSSR_BIND_SRV_INPUT_BRDF_TEXTURE
    layout (set = 0, binding = SSSR_BIND_SRV_INPUT_BRDF_TEXTURE)            uniform texture2D r_input_brdf_texture;
#endif

// UAVs
#if defined SSSR_BIND_UAV_OUTPUT
        layout (set = 0, binding = SSSR_BIND_UAV_OUTPUT, rgba32f)           uniform image2D rw_output;
#endif
#if defined SSSR_BIND_UAV_RADIANCE
        layout (set = 0, binding = SSSR_BIND_UAV_RADIANCE, rgba32f)         uniform image2D rw_radiance;
#endif
#if defined SSSR_BIND_UAV_VARIANCE
        layout (set = 0, binding = SSSR_BIND_UAV_VARIANCE, r32f)            uniform image2D rw_variance;
#endif
#if defined SSSR_BIND_UAV_RAY_LIST
        layout (set = 0, binding = SSSR_BIND_UAV_RAY_LIST, std430)          buffer rw_ray_list_t
        {
            FfxUInt32 data[];
        } rw_ray_list; 
#endif
#if defined SSSR_BIND_UAV_DENOISER_TILE_LIST
        layout (set = 0, binding = SSSR_BIND_UAV_DENOISER_TILE_LIST, std430) buffer rw_denoiser_tile_list_t
        {
            FfxUInt32 data[];
        } rw_denoiser_tile_list;
#endif
#if defined SSSR_BIND_UAV_RAY_COUNTER
        layout (set = 0, binding = SSSR_BIND_UAV_RAY_COUNTER, std430) buffer rw_ray_counter_t
        {
            FfxUInt32 data[];
        } rw_ray_counter;
#endif
#if defined SSSR_BIND_UAV_INTERSECTION_PASS_INDIRECT_ARGS
        layout (set = 0, binding = SSSR_BIND_UAV_INTERSECTION_PASS_INDIRECT_ARGS, std430) buffer rw_intersection_pass_indirect_args_t
        {
            FfxUInt32 data[];
        } rw_intersection_pass_indirect_args; 
#endif
#if defined SSSR_BIND_UAV_EXTRACTED_ROUGHNESS
        layout (set = 0, binding = SSSR_BIND_UAV_EXTRACTED_ROUGHNESS, r32f) uniform image2D rw_extracted_roughness;
#endif
#if defined SSSR_BIND_UAV_BLUE_NOISE_TEXTURE
        layout (set = 0, binding = SSSR_BIND_UAV_BLUE_NOISE_TEXTURE, rg32f) uniform image2D rw_blue_noise_texture;
#endif
#if defined SSSR_BIND_UAV_DEPTH_HIERARCHY
        layout (set = 0, binding = SSSR_BIND_UAV_DEPTH_HIERARCHY, r32f)     uniform image2D rw_depth_hierarchy[13];
#endif

// SPD UAV
#if defined SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC
        layout (set = 0, binding = SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC, std430) buffer rw_spd_global_atomic_t
        {
            FfxUInt32 data[];
        } rw_spd_global_atomic;
#endif

#if FFX_HALF

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)
FfxFloat16x4 SpdLoadH(FfxInt32x2 coordinate, FfxUInt32 slice)
{
    return FfxFloat16x4(imageLoad(rw_depth_hierarchy[6], coordinate).x);   // 5 -> 6 as we store a copy of the depth buffer at index 0
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)

#if defined (SSSR_BIND_SRV_INPUT_DEPTH)
FfxFloat16x4 SpdLoadSourceImageH(FfxInt32x2 coordinate, FfxUInt32 slice)
{
    return FfxFloat16x4(texelFetch(r_input_depth, coordinate, 0).x);
}
#endif // #if defined (SSSR_BIND_SRV_INPUT_DEPTH)

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)
void SpdStoreH(FfxInt32x2 pix, FfxFloat16x4 outValue, FfxUInt32 coordinate, FfxUInt32 slice)
{
    imageStore(rw_depth_hierarchy[coordinate + 1], pix, FfxFloat16x4(outValue.x));    // + 1 as we store a copy of the depth buffer at index 0
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)

#endif // #if defined(FFX_HALF)

#if defined(SSSR_BIND_SRV_INPUT_NORMAL)
FfxFloat32x3 FFX_SSSR_LoadWorldSpaceNormal(FfxInt32x2 pixel_coordinate)
{
    // Normals are 
    return normalize(NormalsUnpackMul() * texelFetch(r_input_normal, pixel_coordinate, 0).xyz + NormalsUnpackAdd());
}
#endif // #if defined(SSSR_BIND_SRV_INPUT_NORMAL)

#if defined(SSSR_BIND_SRV_DEPTH_HIERARCHY)
FfxFloat32 FFX_SSSR_LoadDepth(FfxInt32x2 pixel_coordinate, FfxInt32 mip)
{
    return texelFetch(r_depth_hierarchy, pixel_coordinate, mip).x;
}
#endif // #if defined(SSSR_BIND_SRV_DEPTH_HIERARCHY)

#if defined(SSSR_BIND_SRV_BLUE_NOISE_TEXTURE)
FfxFloat32x2 FFX_SSSR_SampleRandomVector2D(FfxUInt32x2 pixel)
{
    return texelFetch(r_blue_noise_texture, FfxInt32x2(pixel.xy % 128), 0).xy;
}
#endif // #if defined(SSSR_BIND_SRV_BLUE_NOISE_TEXTURE)

#if defined(SSSR_BIND_SRV_INPUT_ENVIRONMENT_MAP)
// ao-vk: the environment is rvk's probe (rvk/shaders/env_common.glsl), not a cube map: an octahedral atlas of five
// levels prefiltered for roughness 0.08, 0.3, 0.5, 0.75, 1 (192 x 128; world space, +y the pole) - the two levels
// round the roughness blended. Directions it hasn't seen (alpha: its sureness) are dark.
FfxFloat32x2 FFX_SSSR_EnvAtlasUv(FfxFloat32x3 d, FfxInt32 level)
{
    FfxFloat32x3 t = level == 0 ? FfxFloat32x3(0.0, 0.0, 128.0) : level == 1 ? FfxFloat32x3(128.0, 0.0, 64.0)
                   : level == 2 ? FfxFloat32x3(128.0, 64.0, 32.0) : level == 3 ? FfxFloat32x3(160.0, 64.0, 16.0)
                   : FfxFloat32x3(176.0, 64.0, 8.0);
    d /= abs(d.x) + abs(d.y) + abs(d.z);
    FfxFloat32x2 e = d.y >= 0.0 ? d.xz : (1.0 - abs(d.zx)) * FfxFloat32x2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    FfxFloat32x2 uv = clamp((e * 0.5 + 0.5) * t.z, FfxFloat32x2(0.5), FfxFloat32x2(t.z - 0.5));
    return (t.xy + uv) / FfxFloat32x2(192.0, 128.0);
}

FfxFloat32x3 FFX_SSSR_SampleEnvironmentMap(FfxFloat32x3 direction, FfxFloat32 preceptualRoughness)
{
    const FfxFloat32 r = clamp(preceptualRoughness, 0.08, 1.0);
    const FfxInt32 lo = r < 0.3 ? 0 : r < 0.5 ? 1 : r < 0.75 ? 2 : 3;
    const FfxFloat32 r0 = lo == 0 ? 0.08 : lo == 1 ? 0.3 : lo == 2 ? 0.5 : 0.75;
    const FfxFloat32 r1 = lo == 0 ? 0.3 : lo == 1 ? 0.5 : lo == 2 ? 0.75 : 1.0;
    FfxFloat32x3 a = textureLod(sampler2D(r_input_environment_map, s_EnvironmentMapSampler), FFX_SSSR_EnvAtlasUv(direction, lo), 0.0).xyz;
    FfxFloat32x3 b = textureLod(sampler2D(r_input_environment_map, s_EnvironmentMapSampler), FFX_SSSR_EnvAtlasUv(direction, lo + 1), 0.0).xyz;
    return mix(a, b, clamp((r - r0) / (r1 - r0), 0.0, 1.0)) * IBLFactor();
}
#endif // #if defined(SSSR_BIND_SRV_INPUT_ENVIRONMENT_MAP)

#if defined (SSSR_BIND_UAV_RAY_COUNTER)
void IncrementRayCounter(FfxUInt32 value, FFX_PARAMETER_OUT FfxUInt32 original_value)
{
    original_value = atomicAdd(rw_ray_counter.data[0], value);
}
#endif // #if defined (SSSR_BIND_UAV_RAY_COUNTER)

#if defined (SSSR_BIND_UAV_RAY_COUNTER)
void IncrementDenoiserTileCounter(FFX_PARAMETER_OUT FfxUInt32 original_value)
{
    original_value = atomicAdd(rw_ray_counter.data[2], 1);
}
#endif // #if defined (SSSR_BIND_UAV_RAY_COUNTER)

FfxUInt32 PackRayCoords(FfxUInt32x2 ray_coord, FfxBoolean copy_horizontal, FfxBoolean copy_vertical, FfxBoolean copy_diagonal)
{
    FfxUInt32 ray_x_15bit = ray_coord.x & 32767;    // 0b111111111111111
    FfxUInt32 ray_y_14bit = ray_coord.y & 16383;    // 0b11111111111111;
    FfxUInt32 copy_horizontal_1bit = copy_horizontal ? 1 : 0;
    FfxUInt32 copy_vertical_1bit = copy_vertical ? 1 : 0;
    FfxUInt32 copy_diagonal_1bit = copy_diagonal ? 1 : 0;

    FfxUInt32 packed = (copy_diagonal_1bit << 31) | (copy_vertical_1bit << 30) | (copy_horizontal_1bit << 29) | (ray_y_14bit << 15) | (ray_x_15bit << 0);
    return packed;
}

#if defined (SSSR_BIND_UAV_RAY_LIST)
void StoreRay(FfxInt32 index, FfxUInt32x2 ray_coord, FfxBoolean copy_horizontal, FfxBoolean copy_vertical, FfxBoolean copy_diagonal)
{
    FfxUInt32 packedRayCoords = PackRayCoords(ray_coord, copy_horizontal, copy_vertical, copy_diagonal); // Store out pixel to trace
    rw_ray_list.data[index] = packedRayCoords;
}
#endif // #if defined (SSSR_BIND_UAV_RAY_LIST)

#if defined (SSSR_BIND_UAV_DENOISER_TILE_LIST)
void StoreDenoiserTile(FfxInt32 index, FfxUInt32x2 tile_coord)
{ 
    rw_denoiser_tile_list.data[index] = ((tile_coord.y & 0xffffu) << 16) | ((tile_coord.x & 0xffffu) << 0); // Store out pixel to trace
}
#endif // #if defined (SSSR_BIND_UAV_DENOISER_TILE_LIST)

#if defined (SSSR_BIND_SRV_DEPTH_HIERARCHY)
FfxBoolean IsReflectiveSurface(FfxUInt32x2 pixel_coordinate, FfxFloat32 roughness)
{
#if FFX_SSSR_OPTION_INVERTED_DEPTH
    const FfxFloat32 far_plane = 0.0f;
    return texelFetch(r_depth_hierarchy, FfxInt32x2(pixel_coordinate), 0).r > far_plane;
#else //  FFX_SSSR_OPTION_INVERTED_DEPTH
    const FfxFloat32 far_plane = 1.0f;
    return texelFetch(r_depth_hierarchy, FfxInt32x2(pixel_coordinate), 0).r < far_plane;
#endif //  FFX_SSSR_OPTION_INVERTED_DEPTH
}
#endif // #if defined (SSSR_BIND_SRV_DEPTH_HIERARCHY)

#if defined (SSSR_BIND_UAV_EXTRACTED_ROUGHNESS)
void StoreExtractedRoughness(FfxUInt32x2 coordinate, FfxFloat32 roughness)
{
    imageStore(rw_extracted_roughness, FfxInt32x2(coordinate), FfxFloat32x4(roughness));
}
#endif // #if defined (SSSR_BIND_UAV_EXTRACTED_ROUGHNESS)

#if defined (SSSR_BIND_SRV_INPUT_MATERIAL_PARAMETERS)
FfxFloat32 LoadRoughnessFromMaterialParametersInput(FfxUInt32x3 coordinate)
{
    FfxFloat32 rawRoughness = texelFetch(r_input_material_parameters, FfxInt32x2(coordinate.xy), FfxInt32(coordinate.z))[RoughnessChannel()];
    if (IsRoughnessPerceptual())
    {
        rawRoughness *= rawRoughness;
    }

    return rawRoughness;
}
#endif // #if defined (SSSR_BIND_SRV_INPUT_MATERIAL_PARAMETERS)

#if defined (SSSR_BIND_UAV_RAY_COUNTER)
FfxBoolean IsRayIndexValid(FfxUInt32 ray_index)
{
    return ray_index < rw_ray_counter.data[1];
}
#endif // #if defined (SSSR_BIND_UAV_RAY_COUNTER)

#if defined (SSSR_BIND_UAV_RAY_LIST)
FfxUInt32 GetRaylist(FfxUInt32 ray_index)
{
    return rw_ray_list.data[ray_index];
}
#endif // #if defined (SSSR_BIND_UAV_RAY_LIST)

#if defined (SSSR_BIND_UAV_BLUE_NOISE_TEXTURE)
void FFX_SSSR_StoreBlueNoiseSample(FfxUInt32x2 coordinate, FfxFloat32x2 blue_noise_sample)
{ 
    imageStore(rw_blue_noise_texture, FfxInt32x2(coordinate), FfxFloat32x4(blue_noise_sample, 0.0f, 0.0f));
}
#endif // #if defined (SSSR_BIND_UAV_BLUE_NOISE_TEXTURE)

#if defined (SSSR_BIND_SRV_VARIANCE)
FfxFloat32 FFX_SSSR_LoadVarianceHistory(FfxInt32x3 coordinate)
{
    return texelFetch(r_variance, coordinate.xy, coordinate.z).x;
}
#endif // #if defined (SSSR_BIND_SRV_VARIANCE)

#if defined (SSSR_BIND_UAV_RADIANCE)
void FFX_SSSR_StoreRadiance(FfxUInt32x2 coordinate, FfxFloat32x4 radiance)
{
    imageStore(rw_radiance, FfxInt32x2(coordinate), radiance);
}
#endif // #if defined (SSSR_BIND_UAV_RADIANCE)

#if defined (SSSR_BIND_SRV_SOBOL_BUFFER)
FfxUInt32 FFX_SSSR_GetSobolSample(FfxUInt32x3 coordinate)
{
    return FfxUInt32(texelFetch(r_sobol_buffer, FfxInt32x2(coordinate.xy), FfxInt32(coordinate.z)).r);
}
#endif // #if defined (SSSR_BIND_SRV_SOBOL_BUFFER)

#if defined (SSSR_BIND_SRV_SCRAMBLING_TILE_BUFFER)
FfxUInt32 FFX_SSSR_GetScramblingTile(FfxUInt32x3 coordinate)
{ 
    return FfxUInt32(texelFetch(r_scrambling_tile_buffer, FfxInt32x2(coordinate.xy), FfxInt32(coordinate.z)).r);
}
#endif // #if defined (SSSR_BIND_SRV_SCRAMBLING_TILE_BUFFER)

#if defined (SSSR_BIND_UAV_INTERSECTION_PASS_INDIRECT_ARGS)
void FFX_SSSR_WriteIntersectIndirectArgs(FfxUInt32 index, FfxUInt32 data)
{
    rw_intersection_pass_indirect_args.data[index] = data;
}
#endif // #if defined (SSSR_BIND_UAV_INTERSECTION_PASS_INDIRECT_ARGS)

#if defined (SSSR_BIND_UAV_RAY_COUNTER)
void FFX_SSSR_WriteRayCounter(FfxUInt32 index, FfxUInt32 data)
{
    rw_ray_counter.data[index] = data;
}
#endif // #if defined (SSSR_BIND_UAV_RAY_COUNTER)

#if defined (SSSR_BIND_UAV_RAY_COUNTER)
FfxUInt32 FFX_SSSR_GetRayCounter(FfxUInt32 index)
{
    return rw_ray_counter.data[index];
}
#endif // #if defined (SSSR_BIND_UAV_RAY_COUNTER)

#if defined (SSSR_BIND_SRV_INPUT_DEPTH)
void FFX_SSSR_GetInputDepthDimensions(FFX_PARAMETER_OUT FfxFloat32x2 image_size)
{
    image_size = textureSize(r_input_depth, 0);
}
#endif // #if defined (SSSR_BIND_SRV_INPUT_DEPTH)

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)
void FFX_SSSR_GetDepthHierarchyMipDimensions(FfxUInt32 mip, FFX_PARAMETER_OUT FfxFloat32x2 image_size)
{
    image_size = imageSize(rw_depth_hierarchy[mip]);
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)

#if defined (SSSR_BIND_SRV_INPUT_DEPTH)
FfxFloat32 FFX_SSSR_GetInputDepth(FfxUInt32x2 coordinate)
{
    return texelFetch(r_input_depth, FfxInt32x2(coordinate), 0).r;
}

FfxFloat32x4 SpdLoadSourceImage(FfxInt32x2 coordinate, FfxUInt32 slice)
{
    return FFX_SSSR_GetInputDepth(coordinate).xxxx;
}
#endif // #if defined (SSSR_BIND_SRV_INPUT_DEPTH)

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY) 
void FFX_SSSR_WriteDepthHierarchy(FfxUInt32 index, FfxUInt32x2 coordinate, FfxFloat32 data)
{
    imageStore(rw_depth_hierarchy[index], FfxInt32x2(coordinate), FfxFloat32x4(data));
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY) 

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)
FfxFloat32x4 SpdLoad(FfxInt32x2 coordinate, FfxUInt32 slice)
{
    return FfxFloat32x4(imageLoad(rw_depth_hierarchy[6], coordinate).x);   // 5 -> 6 as we store a copy of the depth buffer at index 0
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)

#if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)
void SpdStore(FfxInt32x2 pix, FfxFloat32x4 outValue, FfxUInt32 coordinate, FfxUInt32 slice)
{
    imageStore(rw_depth_hierarchy[coordinate + 1], pix, FfxFloat32x4(outValue.x));    // + 1 as we store a copy of the depth buffer at index 0
}
#endif // #if defined (SSSR_BIND_UAV_DEPTH_HIERARCHY)

#if defined (SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC)
void SpdResetAtomicCounter(FfxUInt32 slice)
{
    rw_spd_global_atomic.data[0] = 0;
}
#endif // #if defined (SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC)

#if defined (SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC)
void FFX_SSSR_SPDIncreaseAtomicCounter(FFX_PARAMETER_INOUT FfxUInt32 spdCounter)
{
    spdCounter = atomicAdd(rw_spd_global_atomic.data[0], 1);
}
#endif // #if defined (SSSR_BIND_UAV_SPD_GLOBAL_ATOMIC)

#if defined(SSSR_BIND_SRV_INPUT_COLOR)
FfxFloat32x3 FFX_SSSR_LoadInputColor(FfxInt32x3 coordinate)
{
    return texelFetch(r_input_color, coordinate.xy, coordinate.z).xyz;
}
#endif // #if defined(SSSR_BIND_SRV_INPUT_COLOR)

#if defined(SSSR_BIND_SRV_EXTRACTED_ROUGHNESS)
FfxFloat32 FFX_SSSR_LoadExtractedRoughness(FfxInt32x3 coordinate)
{
    return texelFetch(r_extracted_roughness, coordinate.xy, coordinate.z).x;
}
#endif // #if defined(SSSR_BIND_SRV_EXTRACTED_ROUGHNESS)

#endif // #if defined(FFX_GPU)
