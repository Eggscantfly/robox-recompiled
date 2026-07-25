// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) LynRecomp contributors
//
// fifo_to_bp.cpp -- The bridge between our recompiler's MMIO writes
// (the game writes to 0xCC008000 to push GX FIFO commands) and Dolphin's
// `bpmem` / `xfmem` / `cpmem` globals that the shader generators read.
//
// The recompiled PPC code never touches Dolphin types directly. It writes
// raw bytes to the guest GP-FIFO; we parse those bytes here and update
// the Dolphin-shape register state. This keeps the recompiler output
// completely Dolphin-agnostic.

#include "vendor/dolphin/VideoCommon/BPMemory.h"
#include "vendor/dolphin/VideoCommon/XFMemory.h"
#include "vendor/dolphin/VideoCommon/CPMemory.h"

#include <cstdint>
#include <cstdio>

// GX FIFO opcodes -- subset; see YAGCD chapter 5 for the full list.
enum FifoOpcode : uint8_t {
    FIFO_NOP            = 0x00,
    FIFO_LOAD_CP_REG    = 0x08,   // 1-byte reg, 4-byte value
    FIFO_LOAD_XF_REG    = 0x10,   // 4-byte hdr, then payload
    FIFO_LOAD_INDX_A    = 0x20,
    FIFO_LOAD_INDX_B    = 0x28,
    FIFO_LOAD_INDX_C    = 0x30,
    FIFO_LOAD_INDX_D    = 0x38,
    FIFO_CALL_DL        = 0x40,   // 4-byte addr, 4-byte size
    FIFO_INVAL_VC       = 0x48,
    FIFO_LOAD_BP_REG    = 0x61,   // 1-byte reg, 3-byte value (PACKED)
    FIFO_DRAW_QUADS     = 0x80,
    FIFO_DRAW_TRIANGLES = 0x90,
    FIFO_DRAW_TSTRIP    = 0x98,
    FIFO_DRAW_TFAN      = 0xA0,
    FIFO_DRAW_LINES     = 0xA8,
    FIFO_DRAW_LSTRIP    = 0xB0,
    FIFO_DRAW_POINTS    = 0xB8,
};

// Forward decl: emit a draw to the host renderer once state is up to date.
extern void shader_bridge_draw(uint8_t primitive, uint8_t vat, uint16_t count,
                                const uint8_t* vertex_data);

// ─────────────────────────────────────────────────────────────────────────
// BP register write -- writes 24-bit values into a 32-bit register file.
// The reg is the high byte of the FIFO word; the low 24 bits are the value.
// ─────────────────────────────────────────────────────────────────────────
static void HandleBPWrite(uint8_t reg, uint32_t value) {
    // BPMemory is 256 32-bit registers laid out as a packed struct.
    auto* regs = reinterpret_cast<uint32_t*>(&bpmem);
    regs[reg] = value;

    // TODO: invalidate dependent pipeline cache entries when stage state
    // (TEV, blend, alpha test, etc.) changes. For now, the cache key is
    // built from the full UID at draw time, so stale cache entries are
    // harmless -- just wasteful.
}

// ─────────────────────────────────────────────────────────────────────────
// XF register write -- transforms unit (matrices, lighting).
// ─────────────────────────────────────────────────────────────────────────
static void HandleXFWrite(uint16_t base_addr, uint16_t count, const uint32_t* values) {
    // XFMemory is a 0x1058-word array spanning XF memory + XF registers.
    auto* xf = reinterpret_cast<uint32_t*>(&xfmem);
    for (uint16_t i = 0; i < count; ++i) {
        if (base_addr + i < 0x1058)
            xf[base_addr + i] = values[i];
    }
}

// ─────────────────────────────────────────────────────────────────────────
// CP register write -- vertex format, attribute pointers.
// ─────────────────────────────────────────────────────────────────────────
static void HandleCPWrite(uint8_t reg, uint32_t value) {
    // TODO: route to Dolphin's CPMemory equivalent. CPMemory.h defines the
    // layout but the global isn't a single struct pointer like bpmem -- the
    // CP state is split between vcd_lo/vcd_hi/vat/array_bases. Implement
    // the dispatch here per YAGCD §5.10.
    (void)reg; (void)value;
}

// ─────────────────────────────────────────────────────────────────────────
// FIFO entry point -- called by runtime.c when the recompiled code writes
// to the GP-FIFO MMIO range (0xCC008000-0xCC008100). Pumps commands until
// the supplied buffer is exhausted.
// ─────────────────────────────────────────────────────────────────────────
void fifo_process(const uint8_t* p, size_t len) {
    const uint8_t* end = p + len;
    while (p < end) {
        const uint8_t op = *p++;
        switch (op) {
        case FIFO_NOP:
            break;

        case FIFO_LOAD_BP_REG: {
            // 1 byte reg + 3 bytes value, packed into one 32-bit word.
            const uint32_t word = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                                  (uint32_t)p[2] << 8  | (uint32_t)p[3];
            p += 4;
            HandleBPWrite(word >> 24, word & 0x00FFFFFFu);
            break;
        }

        case FIFO_LOAD_CP_REG: {
            const uint8_t reg = *p++;
            const uint32_t value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                                   (uint32_t)p[2] << 8  | (uint32_t)p[3];
            p += 4;
            HandleCPWrite(reg, value);
            break;
        }

        case FIFO_LOAD_XF_REG: {
            const uint16_t count    = ((uint16_t)p[0] << 8 | p[1]) + 1;
            const uint16_t base_addr = ((uint16_t)p[2] << 8 | p[3]);
            p += 4;
            // Big-endian payload -- byteswap on copy.
            uint32_t buf[64];
            for (uint16_t i = 0; i < count && i < 64; ++i) {
                buf[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                         (uint32_t)p[2] << 8  | (uint32_t)p[3];
                p += 4;
            }
            HandleXFWrite(base_addr, count, buf);
            break;
        }

        case FIFO_DRAW_QUADS:
        case FIFO_DRAW_TRIANGLES:
        case FIFO_DRAW_TSTRIP:
        case FIFO_DRAW_TFAN:
        case FIFO_DRAW_LINES:
        case FIFO_DRAW_LSTRIP:
        case FIFO_DRAW_POINTS: {
            const uint8_t  vat   = op & 0x07;
            const uint16_t count = (uint16_t)p[0] << 8 | p[1];
            p += 2;
            // Vertex data follows; size depends on VCD/VAT registers we
            // already absorbed via CP writes. shader_bridge_draw consumes
            // the next `count * stride` bytes itself.
            shader_bridge_draw(op & 0xF8, vat, count, p);
            // TODO: advance p by count*stride once VertexLoader is wired.
            break;
        }

        default:
            // TODO: handle FIFO_CALL_DL (recursive splice), FIFO_LOAD_INDX_*,
            // and any other opcodes the game emits. See WiiBrew GX command list.
            std::fprintf(stderr, "[fifo] unhandled opcode 0x%02x\n", op);
            return;  // bail out -- continuing without alignment is corruption.
        }
    }
}
