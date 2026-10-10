/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/view.h>

struct GfxBase;
struct TagItem;

typedef void (*ViewPortPaletteFunction)(struct ViewPort* viewport asm("a2"),
    struct GfxBase* graphics asm("a6"));

// The display owner supplies the native palette method. A viewport driver
// table overrides it. These helpers do not publish LoadRGB32 or substitute a
// no-op for the native presentation which that public entry must complete.
void pcUpdateViewportPalette(struct ViewPort* viewport, ViewPortPaletteFunction default_palette,
    struct GfxBase* graphics);
void pcLoadViewportRGB32(struct ViewPort* viewport, const ULONG* colors,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics);

// Native owner for the legacy color paths without a ColorMap. A NULL viewport
// selects the global palette; otherwise update that viewport's display colors.
// Global LoadRGB4 passes the whole input WORD; other paths pass RGB4 nibbles.
typedef void (*ViewPortRGB4Function)(struct ViewPort* viewport, UWORD index, UWORD color,
    struct GfxBase* graphics);

void pcLoadViewportRGB4(struct ViewPort* viewport, const UWORD* colors, ULONG count,
    ViewPortPaletteFunction default_palette, ViewPortRGB4Function default_color, struct GfxBase* graphics);
void pcSetViewportRGB4(struct ViewPort* viewport, ULONG index, ULONG red, ULONG green, ULONG blue,
    ViewPortRGB4Function default_color, struct GfxBase* graphics);

struct ColorMap* pcGetColorMap(LONG entries asm("d0"), struct GfxBase* graphics asm("a6"));
void pcFreeColorMap(struct ColorMap* map asm("a0"), struct GfxBase* graphics asm("a6"));
LONG pcAttachPalExtra(struct ColorMap* map asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"));
ULONG pcObtainPen(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"), LONG flags asm("d4"),
    struct GfxBase* graphics asm("a6"));
void pcReleasePen(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    struct GfxBase* graphics asm("a6"));
LONG pcObtainBestPenA(struct ColorMap* map asm("a0"), ULONG red asm("d1"),
    ULONG green asm("d2"), ULONG blue asm("d3"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
LONG pcFindColor(struct ColorMap* map asm("a3"), ULONG red asm("d1"),
    ULONG green asm("d2"), ULONG blue asm("d3"), LONG maximum asm("d4"));
ULONG pcGetRGB4(struct ColorMap* map asm("a0"), LONG index asm("d0"));
void pcSetRGB4CM(struct ColorMap* map asm("a0"), WORD index asm("d0"),
    UBYTE red asm("d1"), UBYTE green asm("d2"), UBYTE blue asm("d3"));
void pcSetRGB32CM(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"));
void pcGetRGB32(const struct ColorMap* map asm("a0"), ULONG first asm("d0"), ULONG count asm("d1"),
    ULONG* colors asm("a1"), struct GfxBase* graphics asm("a6"));

void pcSetRGB32(struct ViewPort* viewport asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"),
    struct GfxBase* graphics asm("a6"));
