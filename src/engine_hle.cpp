// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) LynRecomp contributors
//
// engine_hle.cpp -- Engine-level HLE intercepts for the LyN engine.
//
// Instead of interpreting GX FIFO commands written by recompiled K3D code,
// we intercept the K3D/WGL functions themselves and route them straight
// to SDL_GPU. This is dramatically faster than GX-level translation and
// produces correct results for the engine paths we understand from the
// LynWiiRetail.elf debug symbols.
//
// Workflow:
//   1. Pick an engine function (e.g. K3D_RenderManager_DrawMesh).
//   2. Look up its address in LynWiiRetail.elf (it's a named symbol).
//   3. Add the address to recomp_config.json's `platform_funcs`.
//   4. Write an hle_<function> handler here.
//   5. Map the address → handler in WiiRecomp's hle_name_map (or a
//      LyN-specific extension of it).
//
// The recompiler will emit a stub for that function that calls our handler
// instead of executing the recompiled body. Arguments come in via the
// PPC argument-passing convention (r3-r10 for ints, f1-f8 for floats).

// HLE_ARG_* / HLE_RET / ppc_host_ptr come from the recompiler's runtime header.
#include "../runtime/hle.h"

#include <SDL3/SDL_gpu.h>
#include <cstdio>

extern SDL_GPUDevice* g_gpu_device;

// ─────────────────────────────────────────────────────────────────────────
// Example: K3D_RenderManager_DrawMesh
//
// From the LynWiiRetail.elf symbol table you'll see something like:
//   void K3D_RenderManager_WII::DrawMesh(K3D_Mesh*, const K3D_Transform*)
//
// PPC ABI: this = r3, mesh = r4, transform = r5.
// ─────────────────────────────────────────────────────────────────────────
void hle_K3D_RenderManager_DrawMesh(void) {
    uint32_t this_va     = HLE_ARG_U32(0);
    uint32_t mesh_va     = HLE_ARG_U32(1);
    uint32_t transform_va = HLE_ARG_U32(2);

    if (!mesh_va) { HLE_RET(0); return; }

    // TODO: read the K3D_Mesh struct fields. From your RE work you know
    // the offsets for vertex_buffer, index_buffer, vertex_count, index_count,
    // material, etc. Translate those into SDL_GPU calls.
    //
    // const K3D_Mesh* mesh = (const K3D_Mesh*)ppc_host_ptr(mesh_va);
    // SDL_BindGPUGraphicsPipeline(g_current_pass, pipeline_for_material(mesh->material));
    // SDL_BindGPUVertexBuffers(g_current_pass, 0, ..., 1);
    // SDL_DrawGPUIndexedPrimitives(g_current_pass, mesh->index_count, 1, 0, 0, 0);

    static int count;
    if (count < 30) {
        std::fprintf(stderr, "[engine_hle] DrawMesh this=0x%08x mesh=0x%08x xform=0x%08x\n",
                     this_va, mesh_va, transform_va);
        count++;
    }

    HLE_RET(0);
}

// ─────────────────────────────────────────────────────────────────────────
// Example: WGL_HW_States_SetCullMode(int mode)
//
// Trivial intercept -- just translate the enum and update SDL_GPU rasterizer
// state for the next draw. mode = r3.
// ─────────────────────────────────────────────────────────────────────────
void hle_WGL_HW_States_SetCullMode(void) {
    uint32_t mode = HLE_ARG_U32(0);
    // TODO: track current cull mode, fold into pipeline cache key.
    (void)mode;
    HLE_RET(0);
}

// ─────────────────────────────────────────────────────────────────────────
// Adding more intercepts:
//
// Priority order (from docs/ROADMAP.md phase 4):
//   - K3D_RenderManager_*::DrawMesh*
//   - K3D_TextureManager_*::Bind / Unbind
//   - K3D_RenderTarget_*::SetActive
//   - WGL_HW_States_*::SetCullMode / SetBlendMode / SetDepthMode
//   - K3D_Camera_*::SetView / SetProjection
//
// Each intercepted function:
//   1. Skips the entire recompiled body of that function.
//   2. Skips all the GX FIFO writes that body would have made.
//   3. Skips fifo_to_bp.cpp's parsing of those writes.
//   4. Skips Dolphin's shader generation for those draws.
//   5. Goes straight from "engine asked to draw a mesh" → SDL_GPU draw.
//
// This is the speed *and* correctness win of engine HLE.
// ─────────────────────────────────────────────────────────────────────────
