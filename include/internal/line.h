/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/rastport.h>

struct Line {
    LONG x, y;
    LONG major, minor;
    int step_x, step_y;
    BOOL major_x;
    UBYTE phase;
};

void pcDraw(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"));
void pcPolyDraw(struct RastPort* port asm("a1"), WORD count asm("d0"), const WORD* points asm("a0"),
    struct GfxBase* graphics asm("a6"));
