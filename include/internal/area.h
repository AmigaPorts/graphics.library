/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <graphics/rastport.h>

struct GfxBase;
LONG pcAreaEnd(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"));
LONG pcAreaMove(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"));
LONG pcAreaDraw(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"));
LONG pcAreaEllipse(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    WORD radius_x asm("d2"), WORD radius_y asm("d3"), struct GfxBase* graphics asm("a6"));

// Internal edge-mask fill helper; not a graphics.library public vector.
void pcFillArea(UWORD* mask asm("a1"), UWORD stride asm("d4"), UWORD rows asm("d1"));
