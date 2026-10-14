/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfx.h>

struct GfxBase;
struct RastPort;
struct Layer;
void pcClipBlit(struct RastPort* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* dest asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), struct GfxBase* graphics asm("a6"));
void pcSyncSBitMap(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"));
void pcCopySBitMap(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"));
void pcSetRast(struct RastPort* port asm("a1"), UBYTE pen asm("d0"), struct GfxBase* graphics asm("a6"));
void pcBltBitMapRastPort(const struct BitMap* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), struct GfxBase* graphics asm("a6"));
void pcBltMaskBitMapRastPort(const struct BitMap* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), PLANEPTR mask asm("a2"),
    struct GfxBase* graphics asm("a6"));
void pcBltTemplate(CONST PLANEPTR source asm("a0"), WORD source_x asm("d0"), WORD source_stride asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), struct GfxBase* graphics asm("a6"));

ULONG pcBltBitMap(const struct BitMap* source asm("a0"), UWORD source_x asm("d0"),
    UWORD source_y asm("d1"), struct BitMap* dest asm("a1"), UWORD dest_x asm("d2"),
    UWORD dest_y asm("d3"), UWORD width asm("d4"), UWORD height asm("d5"),
    UBYTE minterm asm("d6"), UBYTE planes asm("d7"), UWORD* temporary asm("a2"),
    struct GfxBase* graphics asm("a6"));
void pcBltClear(APTR memory asm("a1"), ULONG byte_count asm("d0"), ULONG flags asm("d1"),
    struct GfxBase* graphics asm("a6"));

void pcBltPattern(struct RastPort* port asm("a1"), CONST PLANEPTR mask asm("a0"),
    WORD x_min asm("d0"), WORD y_min asm("d1"), WORD x_max asm("d2"), WORD y_max asm("d3"),
    UWORD mask_stride asm("d4"), struct GfxBase* graphics asm("a6"));
void pcRectFill(struct RastPort* port asm("a1"), WORD x_min asm("d0"), WORD y_min asm("d1"),
    WORD x_max asm("d2"), WORD y_max asm("d3"), struct GfxBase* graphics asm("a6"));

void pcEraseRect(struct RastPort* port asm("a1"), WORD x_min asm("d0"), WORD y_min asm("d1"),
    WORD x_max asm("d2"), WORD y_max asm("d3"), struct GfxBase* graphics asm("a6"));

struct BitScaleArgs;
void pcBitMapScale(struct BitScaleArgs* args asm("a0"), struct GfxBase* graphics asm("a6"));
UWORD pcScalerDiv(UWORD factor asm("d0"), UWORD numerator asm("d1"), UWORD denominator asm("d2"));
