/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/view.h>

struct DBufInfo* pcAllocDBufInfo(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"));
void pcFreeDBufInfo(struct DBufInfo* info asm("a1"), struct GfxBase* graphics asm("a6"));

struct BitMap* pcAllocBitMap(ULONG width asm("d0"), ULONG height asm("d1"), ULONG depth asm("d2"),
    ULONG flags asm("d3"), const struct BitMap* friend_bitmap asm("a0"), struct GfxBase* graphics asm("a6"));
void pcFreeBitMap(struct BitMap* bitmap asm("a0"), struct GfxBase* graphics asm("a6"));
ULONG pcGetBitMapAttr(const struct BitMap* bitmap asm("a0"), ULONG attribute asm("d1"),
    struct GfxBase* graphics asm("a6"));

PLANEPTR pcAllocRaster(UWORD width asm("d0"), UWORD height asm("d1"), struct GfxBase* graphics asm("a6"));
void pcFreeRaster(PLANEPTR memory asm("a0"), UWORD width asm("d0"), UWORD height asm("d1"),
    struct GfxBase* graphics asm("a6"));

// Internal graphics scratch ownership. Small requests first try the shared
// last-chance buffer; if ordinary allocation fails, they wait for that buffer.
// The caller must finish using it (including queued blits) before release.
APTR pcAllocScratch(struct GfxBase* graphics, ULONG size, ULONG flags);
void pcFreeScratch(struct GfxBase* graphics, APTR memory, ULONG size);
