/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/rastport.h>

void pcScrollRaster(struct RastPort* port asm("a1"), WORD dx asm("d0"), WORD dy asm("d1"),
    WORD min_x asm("d2"), WORD min_y asm("d3"), WORD max_x asm("d4"), WORD max_y asm("d5"),
    struct GfxBase* graphics asm("a6"));
void pcScrollRasterBF(struct RastPort* port asm("a1"), WORD dx asm("d0"), WORD dy asm("d1"),
    WORD min_x asm("d2"), WORD min_y asm("d3"), WORD max_x asm("d4"), WORD max_y asm("d5"),
    struct GfxBase* graphics asm("a6"));
