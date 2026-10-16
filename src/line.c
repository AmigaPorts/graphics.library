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

static unsigned outside(const struct Rectangle* bounds, LONG x, LONG y)
{
    return (x < bounds->MinX ? 1U : x > bounds->MaxX ? 4U : 0U) | (y < bounds->MinY ? 2U : y > bounds->MaxY ? 8U : 0U);
}

static LONG intersectionOffset(LONG delta, LONG distance, LONG divisor)
{
    LONG product = delta * distance;
    BOOL negative = (product < 0) != (divisor < 0);

    if (product < 0) {
        product = -product;
    }

    if (divisor < 0) {
        divisor = -divisor;
    }

    LONG offset = (product + divisor / 2) / divisor;

    return negative ? -offset : offset;
}

static BOOL intersectsClip(const struct Line* line, const struct Rectangle* bounds, BOOL reverse)
{
    LONG dx = line->step_x * (line->major_x ? line->major : line->minor);
    LONG dy = line->step_y * (line->major_x ? line->minor : line->major);
    LONG origin_x = line->x + (reverse ? dx : 0);
    LONG origin_y = line->y + (reverse ? dy : 0);
    LONG x = origin_x, y = origin_y;
    LONG end_x = line->x + (reverse ? 0 : dx);
    LONG end_y = line->y + (reverse ? 0 : dy);
    unsigned start_code = outside(bounds, x, y);
    unsigned end_code = outside(bounds, end_x, end_y);

    while (start_code || end_code) {
        if (start_code & end_code) {
            return FALSE;
        }

        unsigned code = start_code ? start_code : end_code;
        LONG clipped_x, clipped_y;

        if (code & 5) {
            clipped_x = code & 1 ? bounds->MinX : bounds->MaxX;
            clipped_y = origin_y + intersectionOffset(dy, clipped_x - origin_x, dx);
        } else {
            clipped_y = code & 2 ? bounds->MinY : bounds->MaxY;
            clipped_x = origin_x + intersectionOffset(dx, clipped_y - origin_y, dy);
        }

        if (start_code) {
            x = clipped_x;
            y = clipped_y;
            start_code = outside(bounds, x, y);
        } else {
            end_x = clipped_x;
            end_y = clipped_y;
            end_code = outside(bounds, end_x, end_y);
        }
    }

    return TRUE;
}

static void togglePixel(const struct RastPort* port, const struct BitMap* map, LONG x, LONG y);

static void drawSolidHorizontal(UBYTE* row, LONG first, LONG last, unsigned operation)
{
    if (operation == 2) {
        return;
    }

    UBYTE* dest = row + (first >> 3);
    UBYTE* end = row + (last >> 3);
    UBYTE first_mask = 0xffU >> (first & 7);
    UBYTE last_mask = 0xffU << (7 - (last & 7));
    UBYTE keep = operation == 1 ? 0xff : 0;
    UBYTE toggle = operation & 1 ? 0xff : 0;

    if (dest == end) {
        first_mask &= last_mask;
    }

    *dest = (*dest & (keep | ~first_mask)) ^ (toggle & first_mask);

    if (dest == end) {
        return;
    }

    // Draw's signed-word delta limits the interior to at most 4095 bytes.
    UWORD count = end - dest - 1;

    ++dest;

    // Interior bytes are fully selected. Only complement needs their old value.
    if (operation == 1) {
        while (count--) {
            *dest++ ^= 0xff;
        }
    } else {
        while (count--) {
            *dest++ = toggle;
        }
    }

    *end = (*end & (keep | ~last_mask)) ^ (toggle & last_mask);
}

// Draw's signed-word deltas limit an axis to at most 32769 pixels.
static void drawSolidVertical(UBYTE* dest, UWORD count, LONG stride, UBYTE bit, unsigned operation)
{
    // Select the operation once; the bit position is identical in every row.
    switch (operation) {
        case 0:
            bit = ~bit;

            for (;;) {
                *dest &= bit;

                if (!--count) {
                    break;
                }

                dest += stride;
            }
            break;

        case 1:
            for (;;) {
                *dest ^= bit;

                if (!--count) {
                    break;
                }

                dest += stride;
            }
            break;

        case 3:
            for (;;) {
                *dest |= bit;

                if (!--count) {
                    break;
                }

                dest += stride;
            }
            break;

        default:
            break;
    }
}

// A solid line has one Boolean operation per plane, independent of phase.
static inline void drawSolidSloped(UBYTE* row, LONG x, UWORD count, LONG row_step,
    const struct Line* line, LONG remainder, BOOL one_dot, unsigned operation)
{
    UBYTE* dest = row + (x >> 3);
    UBYTE bit = 0x80 >> (x & 7);
    BOOL next_row = TRUE;
    BOOL major_x = line->major_x;
    LONG minor_step = 2 * line->minor;
    LONG diagonal_step = minor_step - 2 * line->major;
    LONG step_x = line->step_x;
    unsigned rotation = step_x > 0 ? 7 : 1;
    UBYTE boundary = step_x > 0 ? 0x80 : 1;

    // Clearing uses the inverse mask, which rotates just like the set bit.
    if (operation == 0) {
        bit = ~bit;
        boundary = ~boundary;
    }

    for (;;) {
        if (!one_dot || next_row) {
            if (operation == 0) {
                *dest &= bit;
            } else if (operation == 1) {
                *dest ^= bit;
            } else {
                *dest |= bit;
            }
        }

        if (!--count) {
            break;
        }

        BOOL minor_advance = remainder >= 0;

        next_row = !major_x || minor_advance;

        if (next_row) {
            dest += row_step;
        }

        if (major_x || minor_advance) {
            // Rotate within the byte, advancing storage only when it wraps.
            bit = (bit << rotation) | (bit >> (8 - rotation));

            if (bit == boundary) {
                dest += step_x;
            }
        }

        if (minor_advance) {
            remainder += diagonal_step;
        } else {
            remainder += minor_step;
        }
    }
}

// Resolve the solid operation before entering the pixel loop. Callers also
// select ONE_DOT here so ordinary lines need no per-pixel dot suppression.
static inline void drawSolidLine(UBYTE* row, LONG x, UWORD count, LONG row_step,
    const struct Line* line, LONG remainder, BOOL one_dot, unsigned operation)
{
    switch (operation) {
        case 0:
            drawSolidSloped(row, x, count, row_step, line, remainder, one_dot, 0);
            break;

        case 1:
            drawSolidSloped(row, x, count, row_step, line, remainder, one_dot, 1);
            break;

        case 3:
            drawSolidSloped(row, x, count, row_step, line, remainder, one_dot, 3);
            break;

        default:
            break;
    }
}

static void drawSection(const struct RastPort* port, const struct BitMap* map, const struct Line* line, LONG first,
    LONG last, LONG storage_x, LONG storage_y, BOOL one_dot, BOOL continuation, UBYTE initial_phase,
    struct GfxBase* graphics)
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
    LONG row_step = line->step_y * map->BytesPerRow;
    unsigned shift = 15 - (initial_phase & 15);
    UWORD pattern = port->LinePtrn;

    // Start at the clipped phase; each rotation selects the next lower bit.
    pattern = (pattern << shift) | (pattern >> (16 - shift));
    BOOL solid = (pattern == 0xffff || !pattern) && first <= last;

    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        if (!(port->Mask & (1U << plane))) {
            continue;
        }

        UBYTE minterm = port->minterms[plane];

        if ((minterm & 0xf0) == 0xa0) {
            continue;
        }

        // Advance the row with the rasterizer instead of multiplying the
        // row coordinate for every pixel, including skipped ONE_DOT pixels.
        UBYTE* row = map->Planes[plane] + y * map->BytesPerRow;
        LONG pixel_x = x;
        BOOL next_row = TRUE;
        LONG remainder = error;
        UWORD bits = pattern;
        unsigned foreground = (minterm >> 6) & 3;
        unsigned background = (minterm >> 4) & 3;

        if (solid && !line->minor) {
            unsigned operation = pattern ? foreground : background;

            if (line->major_x) {
                LONG end_x = one_dot ? x : x + line->step_x * (last - first);

                drawSolidHorizontal(row, x < end_x ? x : end_x, x > end_x ? x : end_x, operation);
            }
            else {
                drawSolidVertical(row + (x >> 3), last - first + 1, row_step, 0x80 >> (x & 7), operation);
            }

            continue;
        }

        if (solid) {
            unsigned operation = pattern ? foreground : background;

            if (one_dot) {
                drawSolidLine(row, x, last - first + 1, row_step, line, error, TRUE, operation);
            } else {
                drawSolidLine(row, x, last - first + 1, row_step, line, error, FALSE, operation);
            }

            continue;
        }

        for (LONG i = first; i <= last; ++i) {
            if (!one_dot || next_row) {
                UBYTE* dest = row + (pixel_x >> 3);
                UBYTE bit = 0x80 >> (pixel_x & 7);

                switch (bits & 0x8000 ? foreground : background) {
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

            next_row = FALSE;
            bits = (bits << 1) | (bits >> 15);

            if (remainder >= 0) {
                if (line->major_x) {
                    row += row_step;
                    next_row = TRUE;
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
                row += row_step;
                next_row = TRUE;
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

    // The original rejects a clip using rounded line intersections before
    // choosing rasterized pixels. A corner pixel can pass the pixel bounds
    // while both endpoint-relative intersection passes reject the rectangle.
    if (!whole && !intersectsClip(line, &clip->bounds, FALSE) && !intersectsClip(line, &clip->bounds, TRUE)) {
        return FALSE;
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

    UBYTE phase = line->phase;

    if (!whole) {
        LONG start = (line->major_x ? line->x : line->y) + (line->major_x ? line->step_x : line->step_y) * first;
        WORD cursor = line->major_x ? port->cp_x : port->cp_y;
        WORD origin = line->major_x ? port->Layer->bounds.MinX : port->Layer->bounds.MinY;
        LONG storage = line->major_x ? storage_x : storage_y;
        WORD adjustment = -origin;

        // Clipped patterns measure from the original cursor and layer origin,
        // even after scrolling or when drawing into the superbitmap. Saved
        // rectangles additionally account for their own raster coordinates.
        if (clip->obscured) {
            WORD clip_end = line->major_x ? clip->bounds.MaxX : clip->bounds.MaxY;
            adjustment = start < cursor ? storage + origin + 2 * (cursor - clip_end) : storage - origin;
        }

        LONG distance = start - storage - cursor + adjustment;

        if (distance < 0) {
            distance = -distance;
        }

        phase -= distance;
    }

    // The original clipped path draws every selected pixel. ONE_DOT applies
    // only when the entire line is accepted by a clip, or has no layer.
    drawSection(
        port, map, line, first, last, storage_x, storage_y, whole && (port->Flags & ONE_DOT), FALSE, phase, graphics);

    return whole;
}

void pcDraw(
    struct RastPort* port asm("a1"), WORD end_x asm("d0"), WORD end_y asm("d1"), struct GfxBase* graphics asm("a6"))
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

    line.major_x = dx >= dy;
    line.major = line.major_x ? dx : dy;
    line.minor = line.major_x ? dy : dx;
    BOOL continuation = !(port->Flags & FRST_DOT) && port->DrawMode == COMPLEMENT;

    if (!layer) {
        drawSection(
            port, port->BitMap, &line, 0, line.major, 0, 0, port->Flags & ONE_DOT, continuation, line.phase, graphics);
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
