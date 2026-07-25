// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) LynRecomp contributors
//
// texture_bridge.cpp -- Translate guest texture VAs to host SDL_GPU textures.
//
// When the engine asks to bind a texture at guest VA 0x90400000, we:
//   1. Check our cache for an SDL_GPUTexture keyed on (va, format, w, h).
//   2. If miss: read the guest bytes via ppc_host_ptr(), decode using
//      Dolphin's TexDecoder_Decode(), upload to SDL_GPU.
//   3. Bind the resulting handle.
//
// Cache invalidation is the hard part. The engine sometimes overwrites
// texture data in-place (e.g. for animated textures). For now we re-decode
// on every reference -- correct but slow. Optimize later with a hash check
// or invalidation hook in the engine HLE layer.

#include "vendor/dolphin/VideoCommon/TextureDecoder.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

extern SDL_GPUDevice* g_gpu_device;
extern uint8_t*       g_guest_memory;   // base of guest virtual memory

struct TexKey {
    uint32_t va;
    uint32_t format;
    uint16_t width;
    uint16_t height;
    bool operator==(const TexKey& o) const {
        return va == o.va && format == o.format && width == o.width && height == o.height;
    }
};
struct TexKeyHash {
    size_t operator()(const TexKey& k) const noexcept {
        return ((size_t)k.va * 0x9E3779B97F4A7C15ULL) ^
               ((size_t)k.format << 32) ^ ((size_t)k.width << 16) ^ k.height;
    }
};

static std::unordered_map<TexKey, SDL_GPUTexture*, TexKeyHash> g_tex_cache;

SDL_GPUTexture* texture_bridge_get(uint32_t va, uint32_t format, uint16_t w, uint16_t h,
                                    uint32_t tlut_va, uint32_t tlut_format) {
    TexKey key{va, format, w, h};
    if (auto it = g_tex_cache.find(key); it != g_tex_cache.end())
        return it->second;

    // Decode using Dolphin.
    std::vector<uint8_t> rgba(w * h * 4);
    const uint8_t* src   = g_guest_memory + va;
    const uint8_t* tlut  = tlut_va ? g_guest_memory + tlut_va : nullptr;
    TexDecoder_Decode(rgba.data(), src, w, h,
                      static_cast<TextureFormat>(format),
                      tlut, static_cast<TLUTFormat>(tlut_format));

    // TODO: SDL_CreateGPUTexture, SDL_GPUTransferBuffer, SDL_UploadToGPUTexture.
    // For first triangle, return nullptr and rely on caller to handle missing.
    SDL_GPUTexture* tex = nullptr;
    g_tex_cache[key] = tex;
    return tex;
}
