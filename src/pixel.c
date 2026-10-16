/* SPDX-License-Identifier: MPL-2.0 */
// Planar pixel access using the public Layer and ClipRect storage descriptions.
#include "internal/pixel.h"
#include "internal/backend.h"
#include "internal/blitter.h"
#include "internal/native.h"

#include <graphics/clip.h>
#include <graphics/gfxbase.h>
#include <utility/hooks.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>
#include <proto/layers.h>

LONG pcReadPixelLine8(struct RastPort* port asm("a0"), UWORD x asm("d0"), UWORD y asm("d1"),
    UWORD width asm("d2"), UBYTE* array asm("a2"), struct RastPort* temporary asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    if (port->Layer) {
        struct BitMap* map = temporary->BitMap;
        unsigned count = map->pad == 0x805c ? 1 : map->Depth;
        for (unsigned plane = 0; plane < count; ++plane) {
            UWORD* row = (UWORD*)map->Planes[plane];
            for (unsigned word = 0; word < map->BytesPerRow / 2; ++word) { row[word] = 0; }
        }
    }
    ClipBlit(port, x, y, temporary, 0, 0, width, 1, 0xc0);
    WaitBlit();
    // Conversion follows completion and observes the temporary RastPort's
    // current bitmap. The output includes the final 16-pixel word's padding.
    const struct BitMap* map = temporary->BitMap;
    unsigned words = (UWORD)(width + 15) >> 4;
    for (unsigned word = 0; word < words; ++word) {
        UBYTE* pixels = array + word * 16;
        for (unsigned bit = 0; bit < 16; ++bit) { pixels[bit] = 0; }
        for (unsigned plane = 0; plane < map->Depth && plane < 8; ++plane) {
            UWORD value = ((const UWORD*)map->Planes[plane])[word];
            if (!value) { continue; }
            for (unsigned bit = 0; bit < 16; ++bit) {
                if (value & (0x8000U >> bit)) { pixels[bit] |= 1U << plane; }
            }
        }
    }
    return width;
}

LONG pcReadPixelArray8(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"),
    struct RastPort* temporary asm("a1"), struct GfxBase* graphics asm("a6"))
{
    LONG width = (WORD)(x_stop - x_start) + 1L;
    WORD stride = (UWORD)(width + 15) & 0xfff0;
    LONG height = (WORD)(y_stop - y_start) + 1L;
    if (stride <= 0 || height <= 0) { return -1; }
    ULONG count = 0;
    for (LONG row = 0; row < height; ++row) {
        count += ReadPixelLine8(port, x_start, (UWORD)(y_start + row), width, array, temporary);
        array += stride;
    }
    return count;
}

struct PixelMessage {
    struct Layer* layer;
    struct Rectangle bounds;
    LONG offset_x;
    LONG offset_y;
};

struct ChunkyData {
    const UBYTE* array;
    UWORD stride;
    WORD x;
    WORD y;
};

static ULONG chunkyRegion(struct Hook* hook asm("a0"), const struct PixelMessage* message asm("a1"),
    const struct RastPort* port asm("a2"))
{
    const struct ChunkyData* data = hook->h_Data;
    const struct Rectangle* bounds = &message->bounds;
    const UBYTE* input = data->array;
    if (message->layer) {
        input += message->offset_x - data->x + (message->offset_y - data->y) * data->stride;
    }
    struct BitMap* map = port->BitMap;
    for (LONG y = bounds->MinY; y <= bounds->MaxY; ++y, input += data->stride) {
        const UBYTE* pixels = input;
        LONG x = bounds->MinX;
        while (x <= bounds->MaxX) {
            unsigned first = x & 15;
            unsigned count = 16 - first;
            if (count > (ULONG)(bounds->MaxX - x + 1)) { count = bounds->MaxX - x + 1; }
            UWORD mask = (0xffffU >> first) & (0xffffU << (16 - first - count));
            for (unsigned plane = 0; plane < map->Depth && plane < 8; ++plane) {
                if (!(port->Mask & (1U << plane))) { continue; }
                UWORD value = 0;
                for (unsigned i = 0; i < count; ++i) {
                    value = (value << 1) | ((pixels[i] >> plane) & 1);
                }
                value <<= 16 - first - count;
                UWORD* dest = (UWORD*)(map->Planes[plane] + y * map->BytesPerRow + (x >> 4) * 2);
                *dest = (*dest & ~mask) | value;
            }
            pixels += count;
            x += count;
        }
    }
    return 0;
}

static void writeChunky(struct RastPort* port, const struct Rectangle* bounds, const UBYTE* array,
    UWORD stride, struct GfxBase* graphics)
{
    struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
    struct ChunkyData data = {array, stride, bounds->MinX, bounds->MinY};
    struct Hook hook = {0};
    hook.h_Entry = (ULONG (*)())chunkyRegion;
    hook.h_Data = &data;
    // layers.library owns clipping, locking and selection of visible, saved
    // or superbitmap storage. Pens, drawing modes and patterns do not apply.
    DoHookClipRects(&hook, port, bounds);
}

LONG pcWritePixelLine8(struct RastPort* port asm("a0"), UWORD x asm("d0"), UWORD y asm("d1"),
    UWORD width asm("d2"), UBYTE* array asm("a2"), struct RastPort* temporary asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    (void)temporary;
    struct Rectangle bounds = {x, y, x + width - 1, y};
    writeChunky(port, &bounds, array, 0, graphics);
    return width;
}

LONG pcWritePixelArray8(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"),
    struct RastPort* temporary asm("a1"), struct GfxBase* graphics asm("a6"))
{
    (void)temporary;
    WORD dx = x_stop - x_start, dy = y_stop - y_start;
    if (dx < 0 || dy < 0) { return -1; }
    UWORD width = dx + 1, height = dy + 1;
    struct Rectangle bounds = {x_start, y_start, x_stop, y_stop};
    writeChunky(port, &bounds, array, (UWORD)(width + 15) & 0xfff0, graphics);
    return (ULONG)width * height;
}

void pcWriteChunkyPixels(struct RastPort* port asm("a0"), UWORD x_start asm("d0"), UWORD y_start asm("d1"),
    UWORD x_stop asm("d2"), UWORD y_stop asm("d3"), UBYTE* array asm("a2"), LONG stride asm("d4"),
    struct GfxBase* graphics asm("a6"))
{
    if ((WORD)(x_stop - x_start) < 0 || (WORD)(y_stop - y_start) < 0) { return; }
    struct Rectangle bounds = {x_start, y_start, x_stop, y_stop};
    writeChunky(port, &bounds, array, stride, graphics);
}

static BOOL contains(const struct Rectangle* bounds, WORD x, WORD y)
{
    return x >= bounds->MinX && x <= bounds->MaxX && y >= bounds->MinY && y <= bounds->MaxY;
}

static struct BitMap* pixelStorage(
    const struct RastPort* port asm("a0"), WORD* x asm("a1"), WORD* y asm("a2"))
{
    const struct Layer* layer = port->Layer;
    if (!layer) { return port->BitMap; }
    if (!layer->ClipRect) { return NULL; }
    WORD screen_x = *x + layer->bounds.MinX - layer->Scroll_X;
    WORD screen_y = *y + layer->bounds.MinY - layer->Scroll_Y;
    for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        if (!contains(&clip->bounds, screen_x, screen_y)) { continue; }
        *x = screen_x; *y = screen_y;
        if (!clip->obscured) { return port->BitMap; }
        *x -= clip->bounds.MinX & ~15;
        *y -= clip->bounds.MinY;
        // A matching but unsaved obscured pixel stays inaccessible. Do not
        // select a later clip or superbitmap as a substitute for missing storage.
        return clip->BitMap;
    }
    if (layer->SuperBitMap) {
        for (const struct ClipRect* clip = layer->SuperClipRect; clip; clip = clip->Next) {
            if (contains(&clip->bounds, *x, *y)) { return layer->SuperBitMap; }
        }
    }
    return NULL;
}

ULONG pcReadPixel(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Layer* layer = port->Layer;
    const struct BitMap* map;

    if (layer) {
        LockLayerRom(layer);
        map = pixelStorage(port, &x, &y);
    } else {
        map = port->BitMap;
    }

    LONG result = -1;

    if (map) {
        // Reading waits for pending DMA without taking ownership. In
        // particular it remains valid while the calling task owns the blitter.
        if (pcUsesChipset(graphics)) {
            pcWaitDrawing(graphics);
        }

        LONG offset = (LONG)y * (WORD)map->BytesPerRow + (x >> 3);
        UBYTE bit = 0x80U >> (x & 7);
        result = 0;

        for (UWORD plane = map->Depth; plane > 0; --plane) {
            result = (ULONG)result << 1;

            if (map->Planes[plane - 1][offset] & bit) {
                result = (ULONG)result + 1;
            }
        }
    }

    if (layer) {
        UnlockLayerRom(layer);
    }

    return result;
}

LONG pcWritePixel(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Layer* layer = port->Layer;
    struct BitMap* map;

    if (layer) {
        LockLayerRom(layer);
        map = pixelStorage(port, &x, &y);
    } else {
        map = port->BitMap;
    }

    LONG result = -1;

    if (map) {
        // The original owns even when the plane mask selects nothing.
        // Select layer storage first, so an inaccessible pixel never waits.
        BOOL chipset = pcUsesChipset(graphics);

        if (chipset) {
            pcOwnBlitter(graphics);
            pcWaitDrawing(graphics);
        }

        LONG offset = (LONG)y * (WORD)map->BytesPerRow + (x >> 3);
        UBYTE bit = 0x80U >> (x & 7);
        UBYTE mask = port->Mask;
        UWORD depth = map->Depth;

        for (UWORD plane = 0; plane < depth; ++plane, mask >>= 1) {
            if (!(mask & 1)) {
                continue;
            }

            UBYTE* dest = map->Planes[plane] + offset;

            switch (port->minterms[plane] >> 6) {
                case 0:
                    *dest &= ~bit;
                    break;
                case 1:
                    *dest ^= bit;
                    break;
                case 2:
                    break;
                case 3:
                    *dest |= bit;
                    break;
            }
        }

        if (chipset) {
            pcDisownBlitter(graphics);
        }

        result = 0;
    }

    if (layer) {
        UnlockLayerRom(layer);
    }

    return result;
}
