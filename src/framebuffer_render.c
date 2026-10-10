/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/framebuffer.h"

#include <graphics/gfx.h>
#include <stdint.h>

static unsigned pixelBytes(enum FramebufferFormat format)
{
    switch (format) {
        case FRAMEBUFFER_RGB565:
        case FRAMEBUFFER_RGB555:
            return 2;
        case FRAMEBUFFER_RGB888:
            return 3;
        case FRAMEBUFFER_XRGB8888:
        case FRAMEBUFFER_XBGR8888:
            return 4;
        default:
            return 0;
    }
}

static ULONG pixelValue(enum FramebufferFormat format, ULONG rgb)
{
    switch (format) {
        case FRAMEBUFFER_RGB565:
            return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f);
        case FRAMEBUFFER_RGB555:
            return ((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x03e0) | ((rgb >> 3) & 0x001f);
        case FRAMEBUFFER_XBGR8888:
            return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
        default:
            return rgb & 0xffffff;
    }
}

// Read only the bytes containing the requested pixels. Constant planes have
// the same meaning as in the software bitmap operations.
static UBYTE planeBits(const UBYTE* plane, ULONG row, ULONG x, unsigned count)
{
    if (!plane) {
        return 0;
    }

    if ((uintptr_t)plane == UINT32_MAX) {
        return 0xff;
    }

    const UBYTE* source = plane + row + (x >> 3);
    unsigned shift = x & 7;
    UBYTE bits = source[0] << shift;

    if (shift + count > 8) {
        bits |= source[1] >> (8 - shift);
    }

    return bits;
}

static ULONG bitmapWidth(const struct BitMap* bitmap)
{
    // Match GetBitMapAttr's logical width for our interleaved allocations.
    // BytesPerRow remains the distance between successive rows of each plane.
    ULONG row_bytes = bitmap->BytesPerRow;

    if (bitmap->pad == 0x805c && bitmap->Depth >= 2) {
        ULONG distance = (ULONG)((uintptr_t)bitmap->Planes[1] - (uintptr_t)bitmap->Planes[0]);

        if (distance && distance <= (ULONG)(LONG)(WORD)bitmap->BytesPerRow) {
            row_bytes = distance;
        }
    }

    return (UWORD)(row_bytes * 8);
}

static BOOL validArea(const struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const struct GraphicsFramebufferArea* area)
{
    if (!framebuffer || !framebuffer->pixels || !bitmap || !area ||
        bitmap->Depth > 8 || (bitmap->Flags & BMF_HIJACKED)) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);

    if (!bytes || mode->pixel_bytes != bytes || mode->width > framebuffer->stride / bytes ||
        !framebuffer->stride || mode->height > framebuffer->size / framebuffer->stride) {
        return FALSE;
    }

    ULONG source_width = bitmapWidth(bitmap);

    if (area->source_x > source_width || area->width > source_width - area->source_x ||
        area->source_y > bitmap->Rows || area->height > bitmap->Rows - area->source_y ||
        area->dest_x > mode->width || area->width > mode->width - area->dest_x ||
        area->dest_y > mode->height || area->height > mode->height - area->dest_y) {
        return FALSE;
    }

    return TRUE;
}

static ULONG clipAxis(LONG source, LONG dest, ULONG count, ULONG source_limit, ULONG dest_limit,
    ULONG* clipped_source, ULONG* clipped_dest)
{
    // Magnitudes use unsigned arithmetic, including LONG_MIN. Removing the
    // greater negative prefix brings both origins into the nonnegative range.
    ULONG skip = source < 0 ? 0UL - (ULONG)source : 0;
    ULONG dest_skip = dest < 0 ? 0UL - (ULONG)dest : 0;

    if (dest_skip > skip) {
        skip = dest_skip;
    }

    if (skip >= count) {
        return 0;
    }

    // Each sum fits ULONG: nonnegative input is at most LONG_MAX and skip
    // is at most LONG_MAX + 1. A negative origin cancels its unsigned prefix.
    ULONG first_source = (ULONG)source + skip;
    ULONG first_dest = (ULONG)dest + skip;

    if (first_source >= source_limit || first_dest >= dest_limit) {
        return 0;
    }

    count -= skip;

    if (count > source_limit - first_source) {
        count = source_limit - first_source;
    }

    if (count > dest_limit - first_dest) {
        count = dest_limit - first_dest;
    }

    *clipped_source = first_source;
    *clipped_dest = first_dest;

    return count;
}

BOOL pcClipFramebufferArea(const struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const struct GraphicsFramebufferPlacement* placement, struct GraphicsFramebufferArea* area)
{
    struct GraphicsFramebufferArea clipped = {0};

    // An empty rectangle at the origin checks the shared layout contract
    // without rejecting a placement that simply falls outside either image.
    if (!placement || !area || !validArea(framebuffer, bitmap, &clipped)) {
        return FALSE;
    }

    clipped.width = clipAxis(placement->source_x, placement->dest_x, placement->width,
        bitmapWidth(bitmap), framebuffer->mode.width, &clipped.source_x, &clipped.dest_x);
    clipped.height = clipAxis(placement->source_y, placement->dest_y, placement->height,
        bitmap->Rows, framebuffer->mode.height, &clipped.source_y, &clipped.dest_y);

    if (!clipped.width || !clipped.height) {
        clipped = (struct GraphicsFramebufferArea){0};
    }

    *area = clipped;

    return TRUE;
}

static void planarIndices(const struct BitMap* bitmap, ULONG row, ULONG x, unsigned count, ULONG indices[2])
{
    // Each nibble expands into four byte-sized indices. Accumulate one plane
    // at a time without a conditional branch for every individual source bit.
    static const ULONG spread[16] = {
        0x00000000, 0x00000001, 0x00000100, 0x00000101,
        0x00010000, 0x00010001, 0x00010100, 0x00010101,
        0x01000000, 0x01000001, 0x01000100, 0x01000101,
        0x01010000, 0x01010001, 0x01010100, 0x01010101
    };

    indices[0] = indices[1] = 0;

    for (unsigned plane = 0; plane < bitmap->Depth; ++plane) {
        UBYTE bits = planeBits(bitmap->Planes[plane], row, x, count);
        indices[0] |= spread[bits >> 4] << plane;
        indices[1] |= spread[bits & 15] << plane;
    }
}

BOOL pcRenderIndexedFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area)
{
    if (!palette || !validArea(framebuffer, bitmap, area)) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);

    if (!area->width || !area->height) {
        return TRUE;
    }

    // Pack once per palette entry rather than converting RGB for every pixel.
    // The output byte order is the firmware format, independent of the 68k CPU.
    UBYTE colors[256][4];
    unsigned entries = 1U << bitmap->Depth;

    for (unsigned index = 0; index < entries; ++index) {
        ULONG value = pixelValue(mode->format, palette[index]);

        for (unsigned byte = 0; byte < bytes; ++byte) {
            unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
            colors[index][byte] = value >> shift;
        }
    }

    for (ULONG y = 0; y < area->height; ++y) {
        ULONG source_row = (area->source_y + y) * bitmap->BytesPerRow;
        UBYTE* dest = framebuffer->pixels + (area->dest_y + y) * framebuffer->stride + area->dest_x * bytes;
        ULONG remaining = area->width;
        ULONG x = area->source_x;

        while (remaining) {
            unsigned count = remaining < 8 ? remaining : 8;
            ULONG indices[2];
            planarIndices(bitmap, source_row, x, count, indices);

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned shift = 24 - 8 * (pixel & 3);
                unsigned index = (indices[pixel >> 2] >> shift) & 255;
                const UBYTE* color = colors[index];

                // Firmware formats contain two, three or four bytes. Keep
                // these small copies inline in the freestanding library.
                dest[0] = color[0];
                dest[1] = color[1];

                if (bytes >= 3) {
                    dest[2] = color[2];
                }

                if (bytes == 4) {
                    dest[3] = color[3];
                }

                dest += bytes;
            }

            x += count;
            remaining -= count;
        }
    }

    return TRUE;
}

BOOL pcRenderEhbFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision)
{
    if (!palette || !validArea(framebuffer, bitmap, area) || bitmap->Depth != 6 ||
        (precision != GRAPHICS_EHB_RGB4 && precision != GRAPHICS_EHB_RGB8)) {
        return FALSE;
    }

    if (!area->width || !area->height) {
        return TRUE;
    }

    ULONG colors[64];

    for (unsigned index = 0; index < 32; ++index) {
        ULONG rgb = palette[index];
        colors[index] = rgb;

        // Reduce components at palette precision. Halving an expanded RGB4
        // byte instead would turn white into 0x7f rather than the correct 0x77.
        if (precision == GRAPHICS_EHB_RGB4) {
            colors[index + 32] = ((rgb >> 5) & 0x070707) * 17;
        } else {
            colors[index + 32] = (rgb >> 1) & 0x7f7f7f;
        }
    }

    // Resolve the extra plane once in the palette, keeping the indexed
    // renderer's grouped conversion and avoiding per-pixel EHB branching.
    return pcRenderIndexedFramebuffer(framebuffer, bitmap, colors, area);
}

BOOL pcRenderDualFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front)
{
    if (!first_palette || !second_palette || !validArea(framebuffer, first, first_area) ||
        !validArea(framebuffer, second, second_area)) {
        return FALSE;
    }

    if (first->Depth > 4 || second->Depth > 4 ||
        first_area->dest_x != second_area->dest_x || first_area->dest_y != second_area->dest_y ||
        first_area->width != second_area->width || first_area->height != second_area->height) {
        return FALSE;
    }

    if (!first_area->width || !first_area->height) {
        return TRUE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);
    UBYTE colors[2][16][4];

    for (unsigned field = 0; field < 2; ++field) {
        const ULONG* palette = field ? second_palette : first_palette;
        unsigned entries = 1U << (field ? second->Depth : first->Depth);

        for (unsigned index = 0; index < entries; ++index) {
            ULONG value = pixelValue(mode->format, palette[index]);

            for (unsigned byte = 0; byte < bytes; ++byte) {
                unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
                colors[field][index][byte] = value >> shift;
            }
        }
    }

    for (ULONG y = 0; y < first_area->height; ++y) {
        ULONG first_row = (first_area->source_y + y) * first->BytesPerRow;
        ULONG second_row = (second_area->source_y + y) * second->BytesPerRow;
        UBYTE* dest = framebuffer->pixels + (first_area->dest_y + y) * framebuffer->stride +
            first_area->dest_x * bytes;
        ULONG x = 0;

        while (x < first_area->width) {
            ULONG remaining = first_area->width - x;
            unsigned count = remaining < 8 ? remaining : 8;
            ULONG first_indices[2];
            ULONG second_indices[2];
            planarIndices(first, first_row, first_area->source_x + x, count, first_indices);
            planarIndices(second, second_row, second_area->source_x + x, count, second_indices);

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned shift = 24 - 8 * (pixel & 3);
                unsigned first_index = (first_indices[pixel >> 2] >> shift) & 255;
                unsigned second_index = (second_indices[pixel >> 2] >> shift) & 255;
                const UBYTE* color;

                // Transparency belongs to the unbanked pixel index, not to
                // the palette color. A nonzero black pixel remains opaque.
                if (second_index && (second_in_front || !first_index)) {
                    color = colors[1][second_index];
                } else {
                    color = colors[0][first_index];
                }

                dest[0] = color[0];
                dest[1] = color[1];

                if (bytes >= 3) {
                    dest[2] = color[2];
                }

                if (bytes == 4) {
                    dest[3] = color[3];
                }

                dest += bytes;
            }

            x += count;
        }
    }

    return TRUE;
}

BOOL pcRenderHamFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format)
{
    if (!palette || !validArea(framebuffer, bitmap, area) || origin_x > area->source_x) {
        return FALSE;
    }

    if (format == GRAPHICS_HAM6) {
        if (bitmap->Depth != 5 && bitmap->Depth != 6) {
            return FALSE;
        }
    } else if (format == GRAPHICS_HAM8) {
        if (bitmap->Depth != 8) {
            return FALSE;
        }
    } else {
        return FALSE;
    }

    if (!area->width || !area->height) {
        return TRUE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);
    ULONG end_x = area->source_x + area->width;

    for (ULONG y = 0; y < area->height; ++y) {
        ULONG source_row = (area->source_y + y) * bitmap->BytesPerRow;
        UBYTE* dest = framebuffer->pixels + (area->dest_y + y) * framebuffer->stride + area->dest_x * bytes;
        ULONG rgb = palette[0];
        ULONG x = origin_x;

        // The held color starts anew at the display's left edge on every row.
        // Decode the prefix hidden by output clipping without writing it.
        while (x < end_x) {
            unsigned count = end_x - x < 8 ? end_x - x : 8;
            ULONG indices[2];
            planarIndices(bitmap, source_row, x, count, indices);

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned code = (indices[pixel >> 2] >> (24 - 8 * (pixel & 3))) & 255;
                unsigned command;
                unsigned data;
                ULONG mask;

                if (format == GRAPHICS_HAM8) {
                    command = code & 3;
                    data = code >> 2;
                    mask = 0xfc;
                } else {
                    command = code >> 4;
                    data = code & 15;
                    mask = 0xff;
                }

                if (!command) {
                    rgb = palette[data];
                } else {
                    unsigned shift = command == 1 ? 0 : command == 2 ? 16 : 8;
                    ULONG component = format == GRAPHICS_HAM8 ? data << 2 : data * 17;

                    // HAM8 retains the two low bits from the held component.
                    rgb = (rgb & ~(mask << shift)) | (component << shift);
                }

                if (x + pixel >= area->source_x) {
                    ULONG value = pixelValue(mode->format, rgb);

                    for (unsigned byte = 0; byte < bytes; ++byte) {
                        unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
                        *dest++ = value >> shift;
                    }
                }
            }

            x += count;
        }
    }

    return TRUE;
}
