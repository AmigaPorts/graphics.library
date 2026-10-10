/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/rastport.h>

struct GfxBase;
void pcDrawEllipse(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    WORD radius_x asm("d2"), WORD radius_y asm("d3"), struct GfxBase* graphics asm("a6"));
