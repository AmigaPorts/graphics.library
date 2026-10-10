/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/rastport.h>

struct GfxBase;
LONG pcReadPixelLine8(struct RastPort* port asm("a0"), UWORD x asm("d0"), UWORD y asm("d1"),
    UWORD width asm("d2"), UBYTE* array asm("a2"), struct RastPort* temporary asm("a1"),
    struct GfxBase* graphics asm("a6"));
LONG pcReadPixelArray8(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"),
    struct RastPort* temporary asm("a1"), struct GfxBase* graphics asm("a6"));

LONG pcWritePixelLine8(struct RastPort* port asm("a0"), UWORD x asm("d0"), UWORD y asm("d1"),
    UWORD width asm("d2"), UBYTE* array asm("a2"), struct RastPort* temporary asm("a1"),
    struct GfxBase* graphics asm("a6"));
LONG pcWritePixelArray8(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"),
    struct RastPort* temporary asm("a1"), struct GfxBase* graphics asm("a6"));
void pcWriteChunkyPixels(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"), LONG stride asm("d4"),
    struct GfxBase* graphics asm("a6"));

ULONG pcReadPixel(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"));
LONG pcWritePixel(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"));
