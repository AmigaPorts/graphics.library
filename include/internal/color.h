/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/view.h>

struct GfxBase;
struct TagItem;
struct MonitorSpec;

// RGB4 defaults shared by ColorMap allocation and absent-map display lists.
UWORD pcDefaultColor(unsigned index);

// Expand a legacy color into the native RGB8 palette. A normal RGB4 write
// replaces both nibbles of each component; a low-nibble write preserves the
// high nibble. Keep this shared by retained and live RAM lists. See COLORxx:
// https://www.devili.iki.fi/mirrors/haynie/chips/aa/regbits.pdf (page 10).
static inline ULONG pcApplyRGB4Color(ULONG previous, UWORD color, BOOL low)
{
    ULONG value = ((ULONG)(color & 0xf00) << 8) | ((color & 0xf0) << 4) | (color & 15);

    if (low) {
        return (previous & ~0x0f0f0fUL) | value;
    }

    return (previous & 0xff000000UL) | (value * 17);
}

// Native preparation's palette selection: eight MSB-first groups of 32 colors.
// The caller supplies a valid display layout (at most 256 indexed colors;
// dual fields have at most four planes each). This selects entries only; it
// neither reads palette storage nor changes the caller's ColorMap/ViewPort.
void pcViewPortColorMask(const struct ViewPort* viewport, ULONG* mask, struct GfxBase* graphics);

typedef void (*ViewPortPaletteFunction)(struct ViewPort* viewport asm("a2"),
    struct GfxBase* graphics asm("a6"));

// A viewport driver table overrides the native palette method. Keep this
// dispatch shared with the legacy palette entry points.
void pcUpdateViewportPalette(struct ViewPort* viewport, ViewPortPaletteFunction default_palette,
    struct GfxBase* graphics);
void pcLoadViewportRGB32(struct ViewPort* viewport, const ULONG* colors,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics);

void pcLoadRGB32(struct ViewPort* viewport asm("a0"), const ULONG* colors asm("a1"),
    struct GfxBase* graphics asm("a6"));

// The native palette method finishes by updating the active View's background,
// even when this viewport is hidden or has no prepared list. The display owner
// supplies that operation; it runs inside the same update exclusion as the RAM
// lists. This helper is not a published vector or a complete display driver.
typedef void (*ViewBackgroundFunction)(struct MonitorSpec* monitor, struct GfxBase* graphics);

void pcUpdateNativeViewportPalette(struct ViewPort* viewport, ViewBackgroundFunction background,
    struct GfxBase* graphics);

// Native RGB8 display background. Capture the topmost ViewPort's color zero,
// using effective vertical modes for ordering, under the owner's exclusion.
// A missing View, viewport chain or selected ColorMap retains previous state.
// Native output uses the same V39 low-nibble rule as prepared RGB8 palettes;
// this does not generate chipset control lists or present framebuffer pixels.
void pcUpdateNativeViewBackground(struct MonitorSpec* monitor, struct GfxBase* graphics);
void pcNativeViewportPalette(struct ViewPort* viewport asm("a2"), struct GfxBase* graphics asm("a6"));

// Native owner for the legacy color paths without a ColorMap. A NULL viewport
// selects the global palette; otherwise update that viewport's display colors.
// Global LoadRGB4 passes the whole input WORD; other paths pass RGB4 nibbles.
typedef void (*ViewPortRGB4Function)(struct ViewPort* viewport, UWORD index, UWORD color,
    struct GfxBase* graphics);

// Native non-NULL, absent-ColorMap RGB4 owner. Call with native display
// exclusion already held. This preserves the legacy visible/list checks and
// wrapped index, without RGB32's background callback or supervisor guard.
// NULL/global colors require the separate global owner operation.
void pcUpdateNativeViewportColor4(struct ViewPort* viewport, UWORD index, UWORD color,
    struct GfxBase* graphics);

void pcLoadViewportRGB4(struct ViewPort* viewport, const UWORD* colors, ULONG count,
    ViewPortPaletteFunction default_palette, ViewPortRGB4Function default_color, struct GfxBase* graphics);
void pcSetViewportRGB4(struct ViewPort* viewport, ULONG index, ULONG red, ULONG green, ULONG blue,
    ViewPortRGB4Function default_color, struct GfxBase* graphics);

void pcLoadRGB4(struct ViewPort* viewport asm("a0"), const UWORD* colors asm("a1"), ULONG count asm("d0"),
    struct GfxBase* graphics asm("a6"));
void pcSetRGB4(struct ViewPort* viewport asm("a0"), ULONG index asm("d0"), ULONG red asm("d1"),
    ULONG green asm("d2"), ULONG blue asm("d3"), struct GfxBase* graphics asm("a6"));

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
