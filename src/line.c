/* SPDX-License-Identifier: MPL-2.0 */
// Independent CPU implementation of the public planar line drawing APIs.
#include "internal/line.h"
#include "internal/backend.h"
#include "internal/blitter.h"
#include "internal/native.h"

#include <graphics/clip.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

struct Line {
    LONG x, y;
    LONG major, minor;
    int step_x, step_y;
    BOOL major_x;
    UBYTE phase;
};

static LONG minorPosition(const struct Line* line, LONG position)
{
    return line->major ? (line->minor * position + line->major / 2) / line->major : 0;
}

// First major-axis step whose rounded minor-axis displacement reaches distance.
// Clipping retains the original line's rounding and pattern, rather than
// restarting interpolation at the edge of each destination rectangle.
static LONG firstMinor(const struct Line* line, LONG distance)
{
    if (distance <= 0) {
	    return 0;
    }
	
    if (!line->minor) {
	    return line->major + 1;
    }
	
    return (distance * line->major - line->major / 2 + line->minor - 1) / line->minor;
}

static BOOL clipAxis(const struct Line* line, LONG start, int direction, BOOL major,
    LONG low, LONG high, LONG* first, LONG* last)
{
    LONG minimum = direction > 0 ? low - start : start - high;
    LONG maximum = direction > 0 ? high - start : start - low;
    LONG extent = major ? line->major : line->minor;
	
    if (maximum < 0 || minimum > extent || low > high) {
	    return FALSE;
    }
	
    if (minimum < 0) {
	    minimum = 0;
    }
	
    if (maximum > extent) {
	    maximum = extent;
    }
	
    LONG begin = major ? minimum : firstMinor(line, minimum);
    LONG end = major ? maximum : firstMinor(line, maximum + 1) - 1;
	
    if (begin > *first) {
	    *first = begin;
    }
	
    if (end < *last) {
	    *last = end;
    }
	
    return *first <= *last;
}

static void togglePixel(const struct RastPort* port, const struct BitMap* map, LONG x, LONG y);

static void drawSection(const struct RastPort* port, const struct BitMap* map, const struct Line* line,
    LONG first, LONG last, LONG storage_x, LONG storage_y, BOOL one_dot,
    BOOL continuation, struct GfxBase* graphics)
{
    BOOL chipset = pcUsesChipset(graphics);

    if (chipset) {
        pcOwnBlitter(graphics);
        pcWaitBlit(graphics);
    }

    // On the unlayered path the shared complement endpoint belongs to the
    // same owned operation. Clipped paths handle that endpoint separately.
    if (continuation) {
        togglePixel(port, map, line->x, line->y);
    }

    LONG other = minorPosition(line, first);
    LONG x = line->x + line->step_x * (line->major_x ? first : other) - storage_x;
    LONG y = line->y + line->step_y * (line->major_x ? other : first) - storage_y;
    LONG error = 2 * (line->minor * first - line->major * other) + 2 * line->minor - line->major;

    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        if (!(port->Mask & (1U << plane))) {
            continue;
        }

        UBYTE minterm = port->minterms[plane];

        if ((minterm & 0xf0) == 0xa0) {
            continue;
        }

        UBYTE* data = map->Planes[plane];
        LONG pixel_x = x, pixel_y = y, previous_y = y - line->step_y;
        LONG remainder = error;
        unsigned phase = line->phase - first;

        for (LONG i = first; i <= last; ++i) {
            if (!one_dot || pixel_y != previous_y) {
                UBYTE* dest = data + pixel_y * map->BytesPerRow + (pixel_x >> 3);
                UBYTE bit = 0x80 >> (pixel_x & 7);
                unsigned set = (port->LinePtrn >> (phase & 15)) & 1;

                switch ((minterm >> (4 + 2 * set)) & 3) {
                    case 0: *dest &= ~bit; break;
                    case 1: *dest ^= bit; break;
                    case 2: break;
                    case 3: *dest |= bit; break;
                }
            }

            previous_y = pixel_y;
            --phase;

            if (remainder >= 0) {
                if (line->major_x) {
                    pixel_y += line->step_y;
                }
                else {
                    pixel_x += line->step_x;
                }

                remainder -= 2 * line->major;
            }

            if (line->major_x) {
                pixel_x += line->step_x;
            }
            else {
                pixel_y += line->step_y;
            }

            remainder += 2 * line->minor;
        }
    }

    if (chipset) {
        pcDisownBlitter(graphics);
    }
}

static BOOL contains(const struct Rectangle* bounds, LONG x, LONG y)
{
    return (x >= bounds->MinX && x <= bounds->MaxX && y >= bounds->MinY && y <= bounds->MaxY);
}

static void togglePixel(const struct RastPort* port, const struct BitMap* map, LONG x, LONG y)
{
    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        if (port->Mask & (1U << plane)) {
            map->Planes[plane][y * map->BytesPerRow + (x >> 3)] ^= 0x80 >> (x & 7);
        }
    }
}

static void toggleOwnedPixel(const struct RastPort* port, const struct BitMap* map,
    LONG x, LONG y, struct GfxBase* graphics)
{
    BOOL chipset = pcUsesChipset(graphics);

    if (chipset) {
        pcOwnBlitter(graphics);
        pcWaitBlit(graphics);
    }

    togglePixel(port, map, x, y);

    if (chipset) {
        pcDisownBlitter(graphics);
    }
}

static void toggleStart(const struct RastPort* port, const struct Layer* layer, const struct Line* line,
    WORD offset_x, WORD offset_y, struct GfxBase* graphics)
{
    // Continuation in plain COMPLEMENT removes the shared starting point.
    // Like WritePixel, select only the first storage area containing it.
    if (!layer->ClipRect) {
        return;
    }

    for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        if (!contains(&clip->bounds, line->x, line->y)) {
            continue;
        }

        if (!clip->obscured) {
            toggleOwnedPixel(port, port->BitMap, line->x, line->y, graphics);
        }

        else if (clip->BitMap) {
            toggleOwnedPixel(port, clip->BitMap, line->x - (clip->bounds.MinX & ~15),
                line->y - clip->bounds.MinY, graphics);
        }

        return;
    }

    if (!layer->SuperBitMap) {
        return;
    }

    WORD x = line->x - offset_x, y = line->y - offset_y;

    for (const struct ClipRect* clip = layer->SuperClipRect; clip; clip = clip->Next) {
        if (contains(&clip->bounds, x, y)) {
            toggleOwnedPixel(port, layer->SuperBitMap, x, y, graphics);
            return;
        }
    }
}

static BOOL drawClip(const struct RastPort* port, const struct BitMap* map, const struct Line* line,
    const struct ClipRect* clip, struct GfxBase* graphics)
{
    LONG end_x = line->x + line->step_x * (line->major_x ? line->major : line->minor);
    LONG end_y = line->y + line->step_y * (line->major_x ? line->minor : line->major);
    BOOL whole = contains(&clip->bounds, line->x, line->y) && contains(&clip->bounds, end_x, end_y);

    if (clip->obscured && !clip->BitMap) {
        return whole;
    }

    LONG first = 0, last = line->major;

    if (!whole && (!clipAxis(line, line->x, line->step_x, line->major_x,
        clip->bounds.MinX, clip->bounds.MaxX, &first, &last) ||
        !clipAxis(line, line->y, line->step_y, !line->major_x,
        clip->bounds.MinY, clip->bounds.MaxY, &first, &last))) {
        return FALSE;
    }

    LONG storage_x = 0, storage_y = 0;

    if (clip->obscured) {
        map = clip->BitMap;
        storage_x = clip->bounds.MinX & ~15;
        storage_y = clip->bounds.MinY;
    }

    // The original clipped path draws every selected pixel. ONE_DOT applies
    // only when the entire line is accepted by a clip, or has no layer.
    drawSection(port, map, line, first, last, storage_x, storage_y,
        whole && (port->Flags & ONE_DOT), FALSE, graphics);

    return whole;
}

void pcDraw(struct RastPort* port asm("a1"), WORD end_x asm("d0"), WORD end_y asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Layer* layer = port->Layer;

    if (layer) {
        LockLayerRom(layer);
    }

    LONG dx = (WORD)(end_x - port->cp_x), dy = (WORD)(end_y - port->cp_y);
    struct Line line = {port->cp_x, port->cp_y, 0, 0, dx < 0 ? -1 : 1, dy < 0 ? -1 : 1, FALSE,
        (UBYTE)port->linpatcnt};

    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }

    line.major_x = dx > dy;
    line.major = line.major_x ? dx : dy;
    line.minor = line.major_x ? dy : dx;
    BOOL continuation = !(port->Flags & FRST_DOT) && port->DrawMode == COMPLEMENT;

    if (!layer) {
        drawSection(port, port->BitMap, &line, 0, line.major, 0, 0, port->Flags & ONE_DOT,
            continuation, graphics);
    } else {
        WORD offset_x = layer->bounds.MinX - layer->Scroll_X;
        WORD offset_y = layer->bounds.MinY - layer->Scroll_Y;
        line.x = (WORD)(line.x + offset_x);
        line.y = (WORD)(line.y + offset_y);

        if (continuation) {
            toggleStart(port, layer, &line, offset_x, offset_y, graphics);
        }

        BOOL whole = FALSE;

        for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
            if (drawClip(port, port->BitMap, &line, clip, graphics)) {
                whole = TRUE;
                break;
            }
        }

        if (!whole && layer->SuperBitMap) {
            line.x = port->cp_x;
            line.y = port->cp_y;
            for (const struct ClipRect* clip = layer->SuperClipRect; clip; clip = clip->Next) {
                if (drawClip(port, layer->SuperBitMap, &line, clip, graphics)) {
                    break;
                }
            }
        }

        UnlockLayerRom(layer);
    }

    port->cp_x = end_x;
    port->cp_y = end_y;
    port->Flags &= ~FRST_DOT;
    port->linpatcnt = line.phase - line.major;
}

void pcPolyDraw(struct RastPort* port asm("a1"), WORD count asm("d0"), const WORD* points asm("a0"),
    struct GfxBase* graphics asm("a6"))
{
    for (ULONG i = 0; i < (UWORD)count; ++i) { Draw(port, points[2 * i], points[2 * i + 1]); }
}
