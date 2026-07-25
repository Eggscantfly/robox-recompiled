// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) LynRecomp contributors
//
// main.cpp -- LynRecomp bring-up entry point.
//
// Responsibilities:
//   1. Open an SDL3 window.
//   2. Create an SDL_GPUDevice.
//   3. Allocate guest memory (MEM1 + MEM2) and load the recompiled binary.
//   4. Set up the FIFO interpreter to receive MMIO writes.
//   5. Call the recompiled game's entry point.
//   6. Pump SDL events on a side thread / between frames.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <cstdio>

// Globals referenced by other translation units.
SDL_GPUDevice*       g_gpu_device   = nullptr;
SDL_GPUCommandBuffer* g_current_cmd  = nullptr;
SDL_GPURenderPass*    g_current_pass = nullptr;
uint8_t*             g_guest_memory = nullptr;

// Forward decls.
extern "C" void recomp_run_entry(void);          // emitted by WiiRecomp
extern void fifo_process(const uint8_t* p, size_t len);

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("LynRecomp", 1280, 720,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) { std::fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    g_gpu_device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV,
        /* debug */ true, /* name */ nullptr);
    if (!g_gpu_device) { std::fprintf(stderr, "CreateGPUDevice: %s\n", SDL_GetError()); return 1; }

    if (!SDL_ClaimWindowForGPUDevice(g_gpu_device, window)) {
        std::fprintf(stderr, "ClaimWindowForGPUDevice: %s\n", SDL_GetError());
        return 1;
    }

    // TODO: allocate guest memory, load recompiled binary, set up FIFO MMIO,
    // call recomp_run_entry() on a worker thread.
    std::fprintf(stderr, "[lynrecomp] init complete; recompiler entry not yet wired\n");

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
        }

        // Clear-to-blue per frame -- proves the GPU device is alive.
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_gpu_device);
        SDL_GPUTexture* swap = nullptr;
        Uint32 sw = 0, sh = 0;
        SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh);
        if (swap) {
            SDL_GPUColorTargetInfo ct{};
            ct.texture = swap;
            ct.clear_color = {0.05f, 0.10f, 0.20f, 1.0f};
            ct.load_op  = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    SDL_ReleaseWindowFromGPUDevice(g_gpu_device, window);
    SDL_DestroyGPUDevice(g_gpu_device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
