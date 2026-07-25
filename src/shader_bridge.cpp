// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) LynRecomp contributors
//
// shader_bridge.cpp -- Glue between Dolphin's PixelShaderGen/VertexShaderGen
// and SDL_GPU's pipeline objects. On every draw, derive a UID from the
// current bpmem/xfmem state, look up (or generate) a pipeline keyed on
// that UID, bind it, and issue the draw.

#include "vendor/dolphin/VideoCommon/PixelShaderGen.h"
#include "vendor/dolphin/VideoCommon/VertexShaderGen.h"
#include "vendor/dolphin/VideoCommon/ShaderGenCommon.h"
#include "vendor/dolphin/VideoCommon/BPMemory.h"
#include "vendor/dolphin/VideoCommon/XFMemory.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <cstdio>
#include <unordered_map>

extern SDL_GPUDevice*       g_gpu_device;
extern SDL_GPUCommandBuffer* g_current_cmd;
extern SDL_GPURenderPass*    g_current_pass;

// Cache key: (pixel UID, vertex UID, vertex format hash).
// For now we ignore vertex format and use only the shader UIDs -- correct
// once all draws use the same vertex layout, wrong otherwise. Fix when
// VertexLoader is wired.
struct PipelineKey {
    PixelShaderUid  ps;
    VertexShaderUid vs;
    bool operator==(const PipelineKey& o) const {
        return ps == o.ps && vs == o.vs;
    }
};
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        // PixelShaderUid / VertexShaderUid expose GetUidData() which is
        // a packed POD -- hash its raw bytes.
        const auto* ps_bytes = reinterpret_cast<const uint8_t*>(&k.ps);
        const auto* vs_bytes = reinterpret_cast<const uint8_t*>(&k.vs);
        size_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < sizeof(PixelShaderUid); ++i)
            h = (h ^ ps_bytes[i]) * 1099511628211ULL;
        for (size_t i = 0; i < sizeof(VertexShaderUid); ++i)
            h = (h ^ vs_bytes[i]) * 1099511628211ULL;
        return h;
    }
};

static std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, PipelineKeyHash>
    g_pipeline_cache;

// Stub host-config -- many Dolphin features just need to be off for our
// purposes. Fill in real values from SDL_GetGPUDeviceProperties later.
static ShaderHostConfig MakeHostConfig() {
    ShaderHostConfig cfg{};
    // TODO: populate per current GPU caps. For initial bring-up, defaults
    // (everything false / zero) produce a vanilla shader that should work
    // anywhere SDL_GPU does.
    return cfg;
}

static SDL_GPUGraphicsPipeline* CreatePipeline(const PipelineKey& key) {
    const ShaderHostConfig host = MakeHostConfig();

    // Generate HLSL/GLSL source from Dolphin.
    ShaderCode ps_code = GeneratePixelShaderCode(APIType::D3D, host, key.ps.GetUidData());
    ShaderCode vs_code = GenerateVertexShaderCode(APIType::D3D, host, key.vs.GetUidData());

    // TODO: hand source to SDL_CreateGPUShader. SDL_GPU on Windows accepts
    // pre-compiled DXIL via SDL_GPU_SHADERFORMAT_DXIL, OR raw HLSL via
    // SDL_GPU_SHADERFORMAT_HLSL if SDL was built with shader compilation
    // support. Easiest: ship dxc with the binary, compile HLSL → DXIL at
    // runtime, hand DXIL to SDL.
    //
    //   SDL_GPUShaderCreateInfo info = {
    //       .code_size = ps_code.GetBuffer().size(),
    //       .code = (const Uint8*)ps_code.GetBuffer().data(),
    //       .entrypoint = "main",
    //       .format = SDL_GPU_SHADERFORMAT_HLSL,
    //       .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
    //       .num_samplers = 8,        // fill from PixelShaderUid
    //       .num_storage_textures = 0,
    //       .num_storage_buffers = 0,
    //       .num_uniform_buffers = 1, // PSConstants
    //   };
    //   SDL_GPUShader* ps = SDL_CreateGPUShader(g_gpu_device, &info);
    //
    // Then build SDL_GPUGraphicsPipelineCreateInfo with vertex layout from
    // CPMemory + render target format from current EFB binding + blend
    // state from bpmem.blendmode.

    std::fprintf(stderr, "[shader] would compile pipeline; PS source = %zu bytes, VS source = %zu bytes\n",
                 ps_code.GetBuffer().size(), vs_code.GetBuffer().size());

    return nullptr;  // TODO: return real pipeline
}

// Called from fifo_to_bp.cpp on every draw command.
void shader_bridge_draw(uint8_t primitive, uint8_t vat, uint16_t count,
                         const uint8_t* vertex_data) {
    PipelineKey key{
        .ps = GetPixelShaderUid(),
        .vs = GetVertexShaderUid(),
    };

    auto it = g_pipeline_cache.find(key);
    if (it == g_pipeline_cache.end()) {
        SDL_GPUGraphicsPipeline* p = CreatePipeline(key);
        it = g_pipeline_cache.emplace(key, p).first;
    }

    SDL_GPUGraphicsPipeline* pipeline = it->second;
    if (!pipeline) {
        // Pipeline creation failed earlier; skip this draw.
        return;
    }

    // TODO:
    //   1. Bind pipeline (SDL_BindGPUGraphicsPipeline).
    //   2. Upload vertex data to a transfer buffer, copy to a vertex buffer.
    //   3. Bind vertex/index buffers (SDL_BindGPUVertexBuffers).
    //   4. Bind textures (texture_bridge.cpp).
    //   5. Push uniform constants (PSConstants/VSConstants from bpmem/xfmem).
    //   6. SDL_DrawGPUPrimitives.

    (void)primitive; (void)vat; (void)count; (void)vertex_data;
}
