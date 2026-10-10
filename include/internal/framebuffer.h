/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>

enum FramebufferFormat {
    FRAMEBUFFER_RGB565,
    FRAMEBUFFER_RGB555,
    FRAMEBUFFER_RGB888,
    FRAMEBUFFER_XRGB8888,
    FRAMEBUFFER_XBGR8888
};

struct FramebufferMode {
    ULONG address, size;
    ULONG width, height, stride;
    enum FramebufferFormat format;
    UBYTE pixel_bytes;
    BOOL little_endian;
};

struct ExecBase;

struct GraphicsFramebuffer {
    struct FramebufferMode mode;
    UBYTE* pixels;
    ULONG stride;
    ULONG size;
};

// The display owner serializes these operations. Start with zeroed state;
// retain it until close succeeds. Opening discovers Emu68's already-active
// framebuffer and allocates staging storage without changing visible pixels.
BOOL pcOpenGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase);
BOOL pcCloseGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase);

// Transfer staging pixels in the discovered framebuffer format, like the
// standalone applications. Call only while the native boot display is active;
// P96 mode activation must stop native presentation. This backend neither
// claims PCI devices nor changes hardware modes or installs a refresh task.
BOOL pcPresentGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer);

struct BitMap;
struct ColorMap;
struct GfxBase;

// Palette state for a prepared native display, separate from the caller's
// editable ColorMap. The display owner initializes it from the preceding
// display/global palette and selects which entries an update replaces.
struct GraphicsFramebufferPalette {
    ULONG colors[256];
};

// Import an explicit range through public GetRGB32 into effective RGB8 colors.
// Unselected entries retain their previous values. A zero range makes no call;
// an invalid destination range leaves all state unchanged. As with GetRGB32,
// the caller supplies readable ColorMap storage for the selected source range.
// This does not activate a View, render pixels or claim ownership of scanout.
BOOL pcReadFramebufferPalette(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    ULONG first, ULONG count, struct GfxBase* graphics);

struct GraphicsFramebufferArea {
    ULONG source_x, source_y;
    ULONG dest_x, dest_y;
    ULONG width, height;
};

struct GraphicsFramebufferPlacement {
    LONG source_x, source_y;
    LONG dest_x, dest_y;
    ULONG width, height;
};

// Clip a one-to-one pixel placement to both bitmap and staging bounds, keeping
// the source/destination relationship. The owner first converts View/monitor
// coordinates into framebuffer pixels. Invalid inputs leave area unchanged;
// a valid fully clipped placement returns TRUE with a zeroed area. No pixel,
// palette or hardware access occurs, and neither input structure is modified.
BOOL pcClipFramebufferArea(const struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const struct GraphicsFramebufferPlacement* placement, struct GraphicsFramebufferArea* area);

// Convert indexed planar pixels into staging memory only. The display owner
// supplies a nonoverlapping bitmap, a 0x00RRGGBB palette with at least 2^Depth
// entries, and a rectangle already clipped to both images. Reject invalid
// rectangles/layouts without modifying staging storage. No hardware access,
// scaling, HAM/dual-playfield decoding, sprite composition or mode activation.
BOOL pcRenderIndexedFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area);

enum GraphicsEhbPrecision {
    GRAPHICS_EHB_RGB4,
    GRAPHICS_EHB_RGB8
};

// Decode a six-plane EHB bitmap using 32 effective RGB8 palette entries.
// RGB4 palettes must already have their nibbles replicated to RGB8; dimming
// takes place at the selected precision, before output-format quantization.
// As with indexed conversion, this writes only the clipped staging rectangle.
BOOL pcRenderEhbFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision);

// Compose the two RasInfo bitmaps into staging memory. Each has up to four
// planes, its own source rectangle and resolved RGB8 palette. Both rectangles
// must cover the same destination. Index zero is transparent in either field;
// when both are zero, use first_palette[0]. second_in_front corresponds to
// ViewPort PFBA. The owner resolves palette banks before calling (including
// the classic second-playfield bank starting at color 8).
BOOL pcRenderDualFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front);


enum GraphicsHamFormat {
    GRAPHICS_HAM6,
    GRAPHICS_HAM8
};

// Decode HAM into staging memory, with the same bounds/ownership contract as
// indexed conversion. HAM6 accepts five or six planes and 16 palette entries;
// HAM8 requires eight planes and 64 entries. Palettes contain effective RGB8
// colors (expand legacy RGB4 before calling). HAM6 modifications expand RGB4;
// HAM8 modifications retain the low two component bits of the held color.
// origin_x is the source pixel at the displayed scanline's left edge before
// output clipping. Decode from there, resetting to palette[0] on each row.
BOOL pcRenderHamFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format);
