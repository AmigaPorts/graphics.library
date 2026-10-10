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

static BOOL validFramebuffer(const struct GraphicsFramebuffer* framebuffer)
{
    if (!framebuffer || !framebuffer->pixels) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);

    if (!bytes || mode->pixel_bytes != bytes || mode->width > framebuffer->stride / bytes ||
        !framebuffer->stride || mode->height > framebuffer->size / framebuffer->stride) {
        return FALSE;
    }

    return TRUE;
}

static void fillFramebuffer(struct GraphicsFramebuffer* framebuffer, ULONG rgb,
    ULONG left, ULONG top, ULONG width, ULONG height)
{
    unsigned bytes = framebuffer->mode.pixel_bytes;
    ULONG value = pixelValue(framebuffer->mode.format, rgb);
    UBYTE pixel[4];

    for (unsigned byte = 0; byte < bytes; ++byte) {
        unsigned shift = 8 * (framebuffer->mode.little_endian ? byte : bytes - byte - 1);
        pixel[byte] = value >> shift;
    }

    for (ULONG y = 0; y < height; ++y) {
        UBYTE* target = framebuffer->pixels + (top + y) * framebuffer->stride + left * bytes;

        for (ULONG x = 0; x < width; ++x) {
            for (unsigned byte = 0; byte < bytes; ++byte) {
                *target++ = pixel[byte];
            }
        }
    }
}

BOOL pcFillGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, ULONG rgb)
{
    if (!validFramebuffer(framebuffer)) {
        return FALSE;
    }

    fillFramebuffer(framebuffer, rgb, 0, 0, framebuffer->mode.width, framebuffer->mode.height);

    return TRUE;
}

static BOOL validArea(const struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const struct GraphicsFramebufferArea* area)
{
    if (!validFramebuffer(framebuffer) || !bitmap || !area ||
        bitmap->Depth > 8 || (bitmap->Flags & BMF_HIJACKED)) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    ULONG source_width = bitmapWidth(bitmap);

    if (area->source_x > source_width || area->width > source_width - area->source_x ||
        area->source_y > bitmap->Rows || area->height > bitmap->Rows - area->source_y ||
        area->dest_x > mode->width || area->width > mode->width - area->dest_x ||
        area->dest_y > mode->height || area->height > mode->height - area->dest_y) {
        return FALSE;
    }

    return TRUE;
}

static BOOL overlapsFramebuffer(const struct GraphicsFramebuffer* framebuffer, const void* data, ULONG size)
{
    uintptr_t source = (uintptr_t)data;
    uintptr_t output = (uintptr_t)framebuffer->pixels;

    return source <= output ? output - source < size : source - output < framebuffer->size;
}

static BOOL scaledExtent(ULONG count, int shift, ULONG* result)
{
    if (shift < -2 || shift > 2) {
        return FALSE;
    }

    if (shift >= 0) {
        if (count > (UINT32_MAX >> shift)) {
            return FALSE;
        }

        *result = count << shift;
    } else {
        *result = count ? ((count - 1) >> -shift) + 1 : 0;
    }

    return TRUE;
}

static ULONG clipScaledAxis(LONG origin, ULONG extent, ULONG limit, ULONG* first, ULONG* skip)
{
    // Unsigned magnitude also handles LONG_MIN without signed overflow.
    *skip = origin < 0 ? 0UL - (ULONG)origin : 0;
    *first = origin < 0 ? 0 : (ULONG)origin;

    if (*skip >= extent || *first >= limit) {
        return 0;
    }

    extent -= *skip;
    ULONG remaining = limit - *first;

    return extent < remaining ? extent : remaining;
}

BOOL pcFillGraphicsFramebufferArea(struct GraphicsFramebuffer* framebuffer, ULONG rgb,
    LONG x, LONG y, ULONG width, ULONG height)
{
    if (!validFramebuffer(framebuffer)) {
        return FALSE;
    }

    ULONG left, top, skip;
    width = clipScaledAxis(x, width, framebuffer->mode.width, &left, &skip);
    height = clipScaledAxis(y, height, framebuffer->mode.height, &top, &skip);

    if (width && height) {
        fillFramebuffer(framebuffer, rgb, left, top, width, height);
    }

    return TRUE;
}

static BOOL scaleFramebuffer(struct GraphicsFramebuffer* destination,
    const struct GraphicsFramebuffer* source, const struct GraphicsFramebufferScaleArea* area,
    BOOL field_rows, BOOL short_field)
{
    if (!validFramebuffer(destination) || !validFramebuffer(source) || !area ||
        source->mode.format != destination->mode.format ||
        !!source->mode.little_endian != !!destination->mode.little_endian ||
        area->source_x > source->mode.width || area->width > source->mode.width - area->source_x ||
        area->source_y > source->mode.height || area->height > source->mode.height - area->source_y) {
        return FALSE;
    }

    uintptr_t input = (uintptr_t)source->pixels;
    uintptr_t output = (uintptr_t)destination->pixels;

    if (input <= output ? output - input < source->size : input - output < destination->size) {
        return FALSE;
    }

    ULONG width, height;

    if (!scaledExtent(area->width, area->x_shift, &width) ||
        !scaledExtent(area->height, area->y_shift, &height)) {
        return FALSE;
    }

    ULONG rows = destination->mode.height;
    unsigned parity = short_field ? 1 : 0;

    if (field_rows) {
        rows = rows / 2 + (!parity && (rows & 1));
    }

    ULONG dest_x, dest_y, skip_x, skip_y;
    width = clipScaledAxis(area->dest_x, width, destination->mode.width, &dest_x, &skip_x);
    height = clipScaledAxis(area->dest_y, height, rows, &dest_y, &skip_y);

    if (!width || !height) {
        return TRUE;
    }

    unsigned bytes = source->mode.pixel_bytes;
    unsigned x_repeat = area->x_shift > 0 ? area->x_shift : 0;
    unsigned y_repeat = area->y_shift > 0 ? area->y_shift : 0;
    unsigned x_sample = area->x_shift < 0 ? -area->x_shift : 0;
    unsigned y_sample = area->y_shift < 0 ? -area->y_shift : 0;

    for (ULONG y = 0; y < height; ++y) {
        ULONG source_y = area->source_y + (((skip_y + y) >> y_repeat) << y_sample);
        const UBYTE* row = source->pixels + source_y * source->stride;
        ULONG output_y = field_rows ? (dest_y + y) * 2 + parity : dest_y + y;
        UBYTE* target = destination->pixels + output_y * destination->stride + dest_x * bytes;

        for (ULONG x = 0; x < width; ++x) {
            ULONG source_x = area->source_x + (((skip_x + x) >> x_repeat) << x_sample);
            const UBYTE* pixel = row + source_x * bytes;
            target[0] = pixel[0];
            target[1] = pixel[1];

            if (bytes >= 3) {
                target[2] = pixel[2];
            }

            if (bytes == 4) {
                target[3] = pixel[3];
            }

            target += bytes;
        }
    }

    return TRUE;
}

BOOL pcScaleFramebufferArea(struct GraphicsFramebuffer* destination,
    const struct GraphicsFramebuffer* source, const struct GraphicsFramebufferScaleArea* area)
{
    return scaleFramebuffer(destination, source, area, FALSE, FALSE);
}

BOOL pcScaleFramebufferField(struct GraphicsFramebuffer* destination,
    const struct GraphicsFramebuffer* source, const struct GraphicsFramebufferScaleArea* area, BOOL short_field)
{
    return scaleFramebuffer(destination, source, area, TRUE, short_field);
}

BOOL pcScaleSpriteOcclusion(const struct GraphicsFramebuffer* destination,
    UBYTE* output, ULONG stride, ULONG size, const struct GraphicsFramebuffer* source,
    const struct GraphicsSpriteOcclusion* input, const struct GraphicsFramebufferScaleArea* area)
{
    if (!validFramebuffer(destination) || !validFramebuffer(source) || !area || !output ||
        !input || !input->pixels || !stride || !input->stride ||
        destination->mode.width > stride || destination->mode.height > size / stride ||
        source->mode.width > input->stride || source->mode.height > input->size / input->stride ||
        area->source_x > source->mode.width || area->width > source->mode.width - area->source_x ||
        area->source_y > source->mode.height || area->height > source->mode.height - area->source_y ||
        overlapsFramebuffer(destination, output, size) || overlapsFramebuffer(source, output, size) ||
        overlapsFramebuffer(destination, input->pixels, input->size) ||
        overlapsFramebuffer(source, input->pixels, input->size)) {
        return FALSE;
    }

    uintptr_t first = (uintptr_t)input->pixels;
    uintptr_t second = (uintptr_t)output;

    if (first <= second ? second - first < input->size : first - second < size) {
        return FALSE;
    }

    ULONG width, height;

    if (!scaledExtent(area->width, area->x_shift, &width) ||
        !scaledExtent(area->height, area->y_shift, &height)) {
        return FALSE;
    }

    ULONG dest_x, dest_y, skip_x, skip_y;
    width = clipScaledAxis(area->dest_x, width, destination->mode.width, &dest_x, &skip_x);
    height = clipScaledAxis(area->dest_y, height, destination->mode.height, &dest_y, &skip_y);

    if (!width || !height) {
        return TRUE;
    }

    unsigned x_repeat = area->x_shift > 0 ? area->x_shift : 0;
    unsigned y_repeat = area->y_shift > 0 ? area->y_shift : 0;
    unsigned x_sample = area->x_shift < 0 ? -area->x_shift : 0;
    unsigned y_sample = area->y_shift < 0 ? -area->y_shift : 0;

    for (ULONG y = 0; y < height; ++y) {
        ULONG source_y = area->source_y + (((skip_y + y) >> y_repeat) << y_sample);
        const UBYTE* row = input->pixels + source_y * input->stride;
        UBYTE* target = output + (dest_y + y) * stride + dest_x;

        for (ULONG x = 0; x < width; ++x) {
            ULONG source_x = area->source_x + (((skip_x + x) >> x_repeat) << x_sample);
            target[x] = row[source_x];
        }
    }

    return TRUE;
}

struct SpritePlacement {
    ULONG x, y, width, height, skip_x, skip_y;
};

BOOL pcRenderSpriteFramebufferArea(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const ULONG* palette,
    const struct GraphicsSpriteOcclusion* occlusion,
    LONG dest_x, LONG dest_y, ULONG area_width, ULONG area_height)
{
    if (!validFramebuffer(framebuffer) || !sprites || !palette || sprites->attached_pairs > 15 ||
        sprites->even_bank > 15 || sprites->odd_bank > 15) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;

    if (occlusion && occlusion->pixels &&
        (!occlusion->stride || mode->width > occlusion->stride ||
         mode->height > occlusion->size / occlusion->stride ||
         overlapsFramebuffer(framebuffer, occlusion->pixels, occlusion->size))) {
        return FALSE;
    }

    struct SpritePlacement placements[8];
    ULONG left = mode->width, top = mode->height, right = 0, bottom = 0;

    for (unsigned channel = 0; channel < 8; ++channel) {
        const struct GraphicsFramebufferSprite* sprite = &sprites->images[channel];
        struct SpritePlacement* placement = &placements[channel];
        ULONG width, height;

        if (!scaledExtent(sprite->width, sprite->x_shift, &width) ||
            !scaledExtent(sprite->height, sprite->y_shift, &height)) {
            return FALSE;
        }

        if (sprite->width && sprite->height) {
            ULONG row_bytes = sprite->width / 4;

            if ((sprite->width != 16 && sprite->width != 32 && sprite->width != 64) ||
                !sprite->data || sprite->stride < row_bytes || sprite->size < row_bytes ||
                sprite->height - 1UL > (sprite->size - row_bytes) / sprite->stride ||
                overlapsFramebuffer(framebuffer, sprite->data, sprite->size)) {
                return FALSE;
            }
        }

        placement->width = clipScaledAxis(sprite->dest_x, width, mode->width,
            &placement->x, &placement->skip_x);
        placement->height = clipScaledAxis(sprite->dest_y, height, mode->height,
            &placement->y, &placement->skip_y);

        if (placement->width && placement->height) {
            if (left > placement->x) { left = placement->x; }
            if (top > placement->y) { top = placement->y; }
            if (right < placement->x + placement->width) { right = placement->x + placement->width; }
            if (bottom < placement->y + placement->height) { bottom = placement->y + placement->height; }
        }
    }

    ULONG clip_x, clip_y, skipped;
    area_width = clipScaledAxis(dest_x, area_width, mode->width, &clip_x, &skipped);
    area_height = clipScaledAxis(dest_y, area_height, mode->height, &clip_y, &skipped);

    // Keep placements in framebuffer coordinates. Moving an image's origin
    // to the clip edge would restart enlarged pixels and subsampling there.
    if (left < clip_x) { left = clip_x; }
    if (top < clip_y) { top = clip_y; }
    if (right > clip_x + area_width) { right = clip_x + area_width; }
    if (bottom > clip_y + area_height) { bottom = clip_y + area_height; }

    if (left >= right || top >= bottom) {
        return TRUE;
    }

    unsigned bytes = mode->pixel_bytes;
    UBYTE colors[2][16][4];

    for (unsigned bank = 0; bank < 2; ++bank) {
        unsigned base = (bank ? sprites->odd_bank : sprites->even_bank) * 16;

        for (unsigned index = 0; index < 16; ++index) {
            ULONG pixel = pixelValue(mode->format, palette[base + index]);

            for (unsigned byte = 0; byte < bytes; ++byte) {
                unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
                colors[bank][index][byte] = pixel >> shift;
            }
        }
    }

    for (ULONG y = top; y < bottom; ++y) {
        const UBYTE* rows[8];

        for (unsigned channel = 0; channel < 8; ++channel) {
            const struct GraphicsFramebufferSprite* sprite = &sprites->images[channel];
            const struct SpritePlacement* placement = &placements[channel];
            rows[channel] = NULL;

            if (placement->width && y >= placement->y && y - placement->y < placement->height) {
                ULONG sy = placement->skip_y + y - placement->y;
                sy = sprite->y_shift < 0 ? sy << -sprite->y_shift : sy >> sprite->y_shift;
                rows[channel] = sprite->data + sy * sprite->stride;
            }
        }

        UBYTE* output = framebuffer->pixels + y * framebuffer->stride + left * bytes;

        for (ULONG x = left; x < right; ++x, output += bytes) {
            for (unsigned pair = 0; pair < 4; ++pair) {
                unsigned indices[2] = {0, 0};

                for (unsigned member = 0; member < 2; ++member) {
                    unsigned channel = pair * 2 + member;
                    const struct GraphicsFramebufferSprite* sprite = &sprites->images[channel];
                    const struct SpritePlacement* placement = &placements[channel];
                    const UBYTE* row = rows[channel];

                    if (row && x >= placement->x && x - placement->x < placement->width) {
                        ULONG sx = placement->skip_x + x - placement->x;
                        sx = sprite->x_shift < 0 ? sx << -sprite->x_shift : sx >> sprite->x_shift;
                        unsigned shift = 7 - (sx & 7);
                        unsigned offset = sx >> 3;
                        indices[member] = ((row[offset] >> shift) & 1) |
                            (((row[offset + sprite->width / 8] >> shift) & 1) << 1);
                    }
                }

                unsigned index, bank;

                if (sprites->attached_pairs & (1U << pair)) {
                    index = indices[0] | (indices[1] << 2);
                    bank = 1;
                } else {
                    bank = indices[0] ? 0 : 1;
                    index = indices[bank];

                    if (index) {
                        index += pair * 4;
                    }
                }

                if (!index) {
                    continue;
                }

                // Resolve sprite priority before occlusion. Never infer
                // transparency from a palette entry, even when it is black.
                if (!occlusion || !occlusion->pixels ||
                    !(occlusion->pixels[y * occlusion->stride + x] & (1U << pair))) {
                    const UBYTE* color = colors[bank][index];
                    output[0] = color[0];
                    output[1] = color[1];

                    if (bytes >= 3) { output[2] = color[2]; }
                    if (bytes == 4) { output[3] = color[3]; }
                }

                break;
            }
        }
    }

    return TRUE;
}

BOOL pcRenderSpriteFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const ULONG* palette,
    const struct GraphicsSpriteOcclusion* occlusion)
{
    return framebuffer && pcRenderSpriteFramebufferArea(framebuffer, sprites, palette, occlusion,
        0, 0, framebuffer->mode.width, framebuffer->mode.height);
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

BOOL pcClipFramebufferPair(const struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const struct GraphicsFramebufferPlacement* first_placement,
    const struct GraphicsFramebufferPlacement* second_placement,
    struct GraphicsFramebufferArea* first_area, struct GraphicsFramebufferArea* second_area)
{
    struct GraphicsFramebufferArea areas[2];

    // Resolve both inputs before publishing either result, including when
    // the first raster is entirely clipped but the second layout is invalid.
    if (!first_area || !second_area || first_area == second_area ||
        !pcClipFramebufferArea(framebuffer, first, first_placement, &areas[0]) ||
        !pcClipFramebufferArea(framebuffer, second, second_placement, &areas[1])) {
        return FALSE;
    }

    ULONG left = areas[0].dest_x > areas[1].dest_x ? areas[0].dest_x : areas[1].dest_x;
    ULONG top = areas[0].dest_y > areas[1].dest_y ? areas[0].dest_y : areas[1].dest_y;
    ULONG right = areas[0].dest_x + areas[0].width;
    ULONG bottom = areas[0].dest_y + areas[0].height;
    ULONG second_right = areas[1].dest_x + areas[1].width;
    ULONG second_bottom = areas[1].dest_y + areas[1].height;

    // These sums are bounded by the already-validated framebuffer extents.
    if (second_right < right) {
        right = second_right;
    }

    if (second_bottom < bottom) {
        bottom = second_bottom;
    }

    for (unsigned field = 0; field < 2; ++field) {
        struct GraphicsFramebufferArea* area = &areas[field];

        if (left >= right || top >= bottom) {
            *area = (struct GraphicsFramebufferArea){0};
        } else {
            area->source_x += left - area->dest_x;
            area->source_y += top - area->dest_y;
            area->dest_x = left;
            area->dest_y = top;
            area->width = right - left;
            area->height = bottom - top;
        }
    }

    *first_area = areas[0];
    *second_area = areas[1];

    return TRUE;
}

// A raster's bounds describe the common readable coordinate space. Its
// planes may come from different allocations with independent row pitches
// and origins; they do not redefine the prepared depth or pixel decoder.
BOOL pcReadFramebufferRaster(struct GraphicsFramebufferRaster* raster, const struct BitMap* bitmap)
{
    if (!raster || !bitmap || bitmap->Depth > 8 || (bitmap->Flags & BMF_HIJACKED)) {
        return FALSE;
    }

    raster->width = bitmapWidth(bitmap);
    raster->height = bitmap->Rows;
    raster->depth = bitmap->Depth;

    // Capture all slots, including the extra sources used by seven-plane HAM8
    // and combined playfields with unequal depths. Never read plane storage.
    for (unsigned plane = 0; plane < 8; ++plane) {
        raster->planes[plane].data = bitmap->Planes[plane];
        raster->planes[plane].stride = bitmap->BytesPerRow;
        raster->planes[plane].source_x = 0;
        raster->planes[plane].source_y = 0;
        raster->planes[plane].row_offset = 0;
    }

    return TRUE;
}

static BOOL validRasterArea(const struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* raster, const struct GraphicsFramebufferArea* area)
{
    if (!validFramebuffer(framebuffer) || !raster || !area || raster->depth > 8) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;

    if (area->source_x > raster->width || area->width > raster->width - area->source_x ||
        area->source_y > raster->height || area->height > raster->height - area->source_y ||
        area->dest_x > mode->width || area->width > mode->width - area->dest_x ||
        area->dest_y > mode->height || area->height > mode->height - area->dest_y) {
        return FALSE;
    }

    return TRUE;
}

struct FramebufferRow {
    unsigned depth;
    const UBYTE* planes[8];
    ULONG source_x[8];
};

static void readRasterRow(struct FramebufferRow* row, const struct GraphicsFramebufferRaster* raster, ULONG y)
{
    row->depth = raster->depth;

    // Resolve each independent pitch once per scanline, outside the grouped
    // pixel loop. Preserve constant planes before doing any pointer arithmetic.
    for (unsigned plane = 0; plane < raster->depth; ++plane) {
        const struct GraphicsFramebufferPlane* source = &raster->planes[plane];
        const UBYTE* data = source->data;

        if (data && (uintptr_t)data != UINT32_MAX) {
            uintptr_t address = (uintptr_t)data + (intptr_t)source->row_offset;
            data = (const UBYTE*)(address + (source->source_y + y) * source->stride);
        }

        row->planes[plane] = data;
        row->source_x[plane] = source->source_x;
    }
}

static void planarIndices(const struct FramebufferRow* row, ULONG x, unsigned count, ULONG indices[2])
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

    for (unsigned plane = 0; plane < row->depth; ++plane) {
        UBYTE bits = planeBits(row->planes[plane], 0, row->source_x[plane] + x, count);
        indices[0] |= spread[bits >> 4] << plane;
        indices[1] |= spread[bits & 15] << plane;
    }
}

static BOOL validOcclusionArea(ULONG stride, ULONG size,
    const struct GraphicsFramebufferRaster* raster, const struct GraphicsFramebufferArea* area)
{
    if (!stride || !raster || !area || raster->depth > 8) {
        return FALSE;
    }

    ULONG rows = size / stride;

    return area->source_x <= raster->width && area->width <= raster->width - area->source_x &&
        area->source_y <= raster->height && area->height <= raster->height - area->source_y &&
        area->dest_x <= stride && area->width <= stride - area->dest_x &&
        area->dest_y <= rows && area->height <= rows - area->dest_y;
}

static UBYTE opaquePixels(const struct FramebufferRow* row, ULONG x, unsigned count)
{
    UBYTE bits = 0;

    // Visibility needs only the union of the source planes, not full indices
    // or palette colors. Resolve pitches outside this grouped pixel loop.
    for (unsigned plane = 0; plane < row->depth; ++plane) {
        bits |= planeBits(row->planes[plane], 0, row->source_x[plane] + x, count);
    }

    return bits;
}

BOOL pcWriteSpriteOcclusion(UBYTE* output, ULONG stride, ULONG size,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    UBYTE first_mask, UBYTE second_mask, BOOL second_in_front)
{
    if (!output || first_mask > 15 || second_mask > 15 ||
        !validOcclusionArea(stride, size, first, first_area)) {
        return FALSE;
    }

    if (second && (!validOcclusionArea(stride, size, second, second_area) ||
        first_area->dest_x != second_area->dest_x || first_area->dest_y != second_area->dest_y ||
        first_area->width != second_area->width || first_area->height != second_area->height)) {
        return FALSE;
    }

    if (!first_area->width || !first_area->height) {
        return TRUE;
    }

    if (!first_mask && (!second || !second_mask)) {
        // The normal default places all sprites in front. Clear the target
        // mask without fetching bitplanes whose contents cannot change it.
        for (ULONG y = 0; y < first_area->height; ++y) {
            UBYTE* dest = output + (first_area->dest_y + y) * stride + first_area->dest_x;

            for (ULONG x = 0; x < first_area->width; ++x) {
                dest[x] = 0;
            }
        }

        return TRUE;
    }

    for (ULONG y = 0; y < first_area->height; ++y) {
        struct FramebufferRow first_row, second_row;
        readRasterRow(&first_row, first, first_area->source_y + y);

        if (second) {
            readRasterRow(&second_row, second, second_area->source_y + y);
        }

        UBYTE* dest = output + (first_area->dest_y + y) * stride + first_area->dest_x;
        ULONG x = 0;

        while (x < first_area->width) {
            ULONG remaining = first_area->width - x;
            unsigned count = remaining < 8 ? remaining : 8;
            UBYTE first_bits = opaquePixels(&first_row, first_area->source_x + x, count);
            UBYTE second_bits = second ? opaquePixels(&second_row, second_area->source_x + x, count) : 0;

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned bit = 128U >> pixel;
                BOOL first_opaque = (first_bits & bit) != 0;
                BOOL second_opaque = (second_bits & bit) != 0;
                dest[pixel] = second_opaque && (second_in_front || !first_opaque) ? second_mask :
                    first_opaque ? first_mask : 0;
            }

            dest += count;
            x += count;
        }
    }

    return TRUE;
}

BOOL pcRenderIndexedRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, UBYTE index_xor)
{
    if (!palette || !validRasterArea(framebuffer, raster, area)) {
        return FALSE;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);

    if (!area->width || !area->height) {
        return TRUE;
    }

    // Resolve palette addressing and pack once per entry, not per pixel.
    // The output byte order is the firmware format, independent of the 68k CPU.
    UBYTE colors[256][4];
    unsigned entries = 1U << raster->depth;

    for (unsigned index = 0; index < entries; ++index) {
        ULONG value = pixelValue(mode->format, palette[index ^ index_xor]);

        for (unsigned byte = 0; byte < bytes; ++byte) {
            unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
            colors[index][byte] = value >> shift;
        }
    }

    for (ULONG y = 0; y < area->height; ++y) {
        struct FramebufferRow source_row;
        readRasterRow(&source_row, raster, area->source_y + y);
        UBYTE* dest = framebuffer->pixels + (area->dest_y + y) * framebuffer->stride + area->dest_x * bytes;
        ULONG remaining = area->width;
        ULONG x = area->source_x;

        while (remaining) {
            unsigned count = remaining < 8 ? remaining : 8;
            ULONG indices[2];
            planarIndices(&source_row, x, count, indices);

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

BOOL pcRenderEhbRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision,
    UBYTE index_xor)
{
    if (!palette || !validRasterArea(framebuffer, raster, area) || raster->depth != 6 ||
        (precision != GRAPHICS_EHB_RGB4 && precision != GRAPHICS_EHB_RGB8)) {
        return FALSE;
    }

    if (!area->width || !area->height) {
        return TRUE;
    }

    ULONG colors[64];

    for (unsigned index = 0; index < 64; ++index) {
        unsigned address = index ^ index_xor;
        BOOL half = address >= 32 && address < 64;
        ULONG rgb = palette[half ? address - 32 : address];

        // Reduce components at palette precision. Halving an expanded RGB4
        // byte instead would turn white into 0x7f rather than the correct 0x77.
        if (half) {
            if (precision == GRAPHICS_EHB_RGB4) {
                rgb = ((rgb >> 5) & 0x070707) * 17;
            } else {
                rgb = (rgb >> 1) & 0x7f7f7f;
            }
        }

        colors[index] = rgb;
    }

    // Resolve the extra plane once in the palette, keeping the indexed
    // renderer's grouped conversion and avoiding per-pixel EHB branching.
    return pcRenderIndexedRaster(framebuffer, raster, colors, area, 0);
}

// Both entry points validate the original raster layouts before decoding.
// Field descriptors may be views of alternating planes; their plane spacing
// no longer describes the allocation's logical interleaved width.
static BOOL renderDual(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front)
{
    if (first->depth > 4 || second->depth > 4 ||
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
        unsigned entries = 1U << (field ? second->depth : first->depth);

        for (unsigned index = 0; index < entries; ++index) {
            ULONG value = pixelValue(mode->format, palette[index]);

            for (unsigned byte = 0; byte < bytes; ++byte) {
                unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
                colors[field][index][byte] = value >> shift;
            }
        }
    }

    for (ULONG y = 0; y < first_area->height; ++y) {
        struct FramebufferRow first_row, second_row;
        readRasterRow(&first_row, first, first_area->source_y + y);
        readRasterRow(&second_row, second, second_area->source_y + y);
        UBYTE* dest = framebuffer->pixels + (first_area->dest_y + y) * framebuffer->stride +
            first_area->dest_x * bytes;
        ULONG x = 0;

        while (x < first_area->width) {
            ULONG remaining = first_area->width - x;
            unsigned count = remaining < 8 ? remaining : 8;
            ULONG first_indices[2];
            ULONG second_indices[2];
            planarIndices(&first_row, first_area->source_x + x, count, first_indices);
            planarIndices(&second_row, second_area->source_x + x, count, second_indices);

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

BOOL pcRenderDualRaster(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front)
{
    if (!first_palette || !second_palette || !validRasterArea(framebuffer, first, first_area) ||
        !validRasterArea(framebuffer, second, second_area)) {
        return FALSE;
    }

    return renderDual(framebuffer, first, second, first_palette, second_palette,
        first_area, second_area, second_in_front);
}

BOOL pcRenderPlanarDualRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* first_palette, const ULONG* second_palette, const struct GraphicsFramebufferArea* area,
    BOOL second_in_front)
{
    if (!first_palette || !second_palette || !validRasterArea(framebuffer, raster, area)) {
        return FALSE;
    }

    struct GraphicsFramebufferRaster first = *raster;
    struct GraphicsFramebufferRaster second = *raster;
    first.depth = (raster->depth + 1) / 2;
    second.depth = raster->depth / 2;

    for (unsigned plane = 0; plane < first.depth; ++plane) {
        first.planes[plane] = raster->planes[2 * plane];
    }

    for (unsigned plane = 0; plane < second.depth; ++plane) {
        second.planes[plane] = raster->planes[2 * plane + 1];
    }

    return renderDual(framebuffer, &first, &second, first_palette, second_palette,
        area, area, second_in_front);
}

BOOL pcRenderCombinedRaster(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second, const ULONG* palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area, UBYTE index_xor)
{
    if (!palette || !validRasterArea(framebuffer, first, first_area) ||
        !validRasterArea(framebuffer, second, second_area)) {
        return FALSE;
    }

    unsigned depth = first->depth + second->depth;

    if (depth > 8 || first_area->dest_x != second_area->dest_x ||
        first_area->dest_y != second_area->dest_y || first_area->width != second_area->width ||
        first_area->height != second_area->height) {
        return FALSE;
    }

    if (!first_area->width || !first_area->height) {
        return TRUE;
    }

    // The original interleaves plane pointers using the total depth, even
    // when that selects a slot beyond one RasInfo raster's declared depth.
    // Keep caller metadata intact, including its logical interleaved width.
    struct GraphicsFramebufferRaster first_planes = *first;
    struct GraphicsFramebufferRaster second_planes = *second;
    first_planes.depth = (depth + 1) / 2;
    second_planes.depth = depth / 2;

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);
    UBYTE colors[256][4];

    // Palette addressing follows the final interleaved index. Applying it to
    // either field before interleaving would move the mask's bit positions.
    for (unsigned index = 0; index < (1U << depth); ++index) {
        ULONG value = pixelValue(mode->format, palette[index ^ index_xor]);

        for (unsigned byte = 0; byte < bytes; ++byte) {
            unsigned shift = 8 * (mode->little_endian ? byte : bytes - byte - 1);
            colors[index][byte] = value >> shift;
        }
    }

    for (ULONG y = 0; y < first_area->height; ++y) {
        struct FramebufferRow first_row, second_row;
        readRasterRow(&first_row, &first_planes, first_area->source_y + y);
        readRasterRow(&second_row, &second_planes, second_area->source_y + y);
        UBYTE* dest = framebuffer->pixels + (first_area->dest_y + y) * framebuffer->stride +
            first_area->dest_x * bytes;
        ULONG x = 0;

        while (x < first_area->width) {
            ULONG remaining = first_area->width - x;
            unsigned count = remaining < 8 ? remaining : 8;
            ULONG first_indices[2];
            ULONG second_indices[2];
            planarIndices(&first_row, first_area->source_x + x, count, first_indices);
            planarIndices(&second_row, second_area->source_x + x, count, second_indices);

            for (unsigned group = 0; group < 2; ++group) {
                // Expand each four-bit index into the even positions within
                // its byte. Masks keep all four pixels independent.
                ULONG a = first_indices[group];
                ULONG b = second_indices[group];
                a = (a | (a << 2)) & 0x33333333;
                b = (b | (b << 2)) & 0x33333333;
                a = (a | (a << 1)) & 0x55555555;
                b = (b | (b << 1)) & 0x55555555;
                first_indices[group] = a | (b << 1);
            }

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned shift = 24 - 8 * (pixel & 3);
                unsigned index = (first_indices[pixel >> 2] >> shift) & 255;
                const UBYTE* color = colors[index];
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

static BOOL renderHamRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format,
    UBYTE index_xor, ULONG* held_color)
{
    if (!palette || !validRasterArea(framebuffer, raster, area) || origin_x > area->source_x ||
        (held_color && area->height > 1)) {
        return FALSE;
    }

    if (format == GRAPHICS_HAM6) {
        if (raster->depth != 5 && raster->depth != 6) {
            return FALSE;
        }
    } else if (format == GRAPHICS_HAM8 || format == GRAPHICS_HAM8_PLANAR) {
        if (raster->depth != 7 && raster->depth != 8) {
            return FALSE;
        }
    } else {
        return FALSE;
    }

    if (!area->width || !area->height) {
        return TRUE;
    }

    struct GraphicsFramebufferRaster seven_planes;

    if (format == GRAPHICS_HAM8_PLANAR && raster->depth == 7) {
        // The original seven-plane permutation reads application slots
        // 6,7,0,1,2,3,4. Keep commands in 6/7 and omit data plane 5.
        // Validate logical/interleaved bounds before changing this local view.
        seven_planes = *raster;
        seven_planes.depth = 8;
        seven_planes.planes[5].data = NULL;
        raster = &seven_planes;
    }

    const struct FramebufferMode* mode = &framebuffer->mode;
    unsigned bytes = pixelBytes(mode->format);
    ULONG end_x = area->source_x + area->width;
    unsigned command_shift = format == GRAPHICS_HAM6 ? 4 : format == GRAPHICS_HAM8_PLANAR ? 6 : 0;
    unsigned data_shift = format == GRAPHICS_HAM8 ? 2 : 0;
    unsigned data_mask = format == GRAPHICS_HAM6 ? 15 : 63;
    ULONG mask = format == GRAPHICS_HAM6 ? 0xff : 0xfc;
    unsigned code_xor = format == GRAPHICS_HAM8_PLANAR ?
        (index_xor >> 2) | ((index_xor & 3) << 6) : index_xor;

    for (ULONG y = 0; y < area->height; ++y) {
        struct FramebufferRow source_row;
        readRasterRow(&source_row, raster, area->source_y + y);
        UBYTE* dest = framebuffer->pixels + (area->dest_y + y) * framebuffer->stride + area->dest_x * bytes;
        ULONG rgb = held_color ? *held_color : palette[0];
        ULONG x = origin_x;

        // Whole-row decoding starts at the display's left edge and replays
        // any clipped prefix. A successive span receives that history from
        // its owner, including colors selected under an earlier palette.
        while (x < end_x) {
            unsigned count = end_x - x < 8 ? end_x - x : 8;
            ULONG indices[2];
            planarIndices(&source_row, x, count, indices);

            for (unsigned pixel = 0; pixel < count; ++pixel) {
                unsigned code = (indices[pixel >> 2] >> (24 - 8 * (pixel & 3))) & 255;
                unsigned addressed = code ^ code_xor;
                unsigned command = (addressed >> command_shift) & 3;
                unsigned data = (addressed >> data_shift) & data_mask;

                // HAM6 modifies with the original nibble, even when XOR
                // changes the operation. HAM8 modifies with the XORed data.
                if (format == GRAPHICS_HAM6 && command) {
                    data = code & 15;
                }

                if (!command) {
                    rgb = palette[data];
                } else {
                    unsigned shift = command == 1 ? 0 : command == 2 ? 16 : 8;
                    ULONG component = format == GRAPHICS_HAM6 ? data * 17 : data << 2;

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

        if (held_color) {
            *held_color = rgb;
        }
    }

    return TRUE;
}

BOOL pcRenderHamRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format,
    UBYTE index_xor)
{
    return renderHamRaster(framebuffer, raster, palette, area, origin_x, format, index_xor, NULL);
}

BOOL pcRenderHamRasterSpan(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsHamFormat format,
    UBYTE index_xor, ULONG* held_color)
{
    if (!area || !held_color) {
        return FALSE;
    }

    return renderHamRaster(framebuffer, raster, palette, area, area->source_x, format, index_xor, held_color);
}

BOOL pcRenderIndexedFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, UBYTE index_xor)
{
    struct GraphicsFramebufferRaster raster;

    return pcReadFramebufferRaster(&raster, bitmap) &&
        pcRenderIndexedRaster(framebuffer, &raster, palette, area, index_xor);
}

BOOL pcRenderEhbFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision,
    UBYTE index_xor)
{
    struct GraphicsFramebufferRaster raster;

    return pcReadFramebufferRaster(&raster, bitmap) &&
        pcRenderEhbRaster(framebuffer, &raster, palette, area, precision, index_xor);
}

BOOL pcRenderHamFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format,
    UBYTE index_xor)
{
    struct GraphicsFramebufferRaster raster;

    return pcReadFramebufferRaster(&raster, bitmap) &&
        pcRenderHamRaster(framebuffer, &raster, palette, area, origin_x, format, index_xor);
}

BOOL pcRenderPlanarDualFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* first_palette, const ULONG* second_palette, const struct GraphicsFramebufferArea* area,
    BOOL second_in_front)
{
    struct GraphicsFramebufferRaster raster;

    return pcReadFramebufferRaster(&raster, bitmap) &&
        pcRenderPlanarDualRaster(framebuffer, &raster, first_palette, second_palette, area, second_in_front);
}

BOOL pcRenderDualFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front)
{
    struct GraphicsFramebufferRaster rasters[2];

    return pcReadFramebufferRaster(&rasters[0], first) && pcReadFramebufferRaster(&rasters[1], second) &&
        pcRenderDualRaster(framebuffer, &rasters[0], &rasters[1], first_palette, second_palette,
            first_area, second_area, second_in_front);
}

BOOL pcRenderCombinedFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const ULONG* palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    UBYTE index_xor)
{
    struct GraphicsFramebufferRaster rasters[2];

    return pcReadFramebufferRaster(&rasters[0], first) && pcReadFramebufferRaster(&rasters[1], second) &&
        pcRenderCombinedRaster(framebuffer, &rasters[0], &rasters[1], palette,
            first_area, second_area, index_xor);
}
