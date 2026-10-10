/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/framebuffer.h"
#include "internal/color.h"

#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

void pcReadPlayfieldPaletteLayout(struct PlayfieldPaletteLayout* layout, const struct ColorMap* map)
{
    layout->index_xor = 0;
    layout->second_offset = map ? 0 : 8;

    if (map && map->Type) {
        // Preserve the original WORD packing, including high bits from an
        // out-of-range odd sprite base overlapping the playfield mask.
        layout->index_xor = (UBYTE)map->Bp_0_base | (map->SpriteBase_Odd >> 12);
        UBYTE difference = map->Bp_1_base - map->Bp_0_base;
        unsigned shift = 0;

        while (difference > 1) {
            difference >>= 1;
            ++shift;
        }

        layout->second_offset = shift ? 1U << shift : 0;
    }
}

void pcResolvePreparedPalette(struct GraphicsFramebufferPalette* output,
    const struct GraphicsFramebufferPalette* preceding, const struct GraphicsFramebufferPalette* prepared,
    const ULONG* mask)
{
    for (unsigned bank = 0; bank < 8; ++bank) {
        ULONG selected = mask[bank];

        for (unsigned bit = 0; bit < 32; ++bit) {
            unsigned index = bank * 32 + bit;
            output->colors[index] = selected & (0x80000000UL >> bit) ?
                prepared->colors[index] : preceding->colors[index];
        }
    }
}

void pcReadPreparedPalette(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    const ULONG* mask)
{
    const UWORD* high = map ? map->ColorTable : NULL;
    const UWORD* low = map && map->Type == COLORMAP_TYPE_V39 ? map->LowColorBits : high;

    for (unsigned index = 0; index < 256; ++index) {
        if (!(mask[index >> 5] & (0x80000000UL >> (index & 31)))) {
            continue;
        }

        UWORD upper = map ? high[index] : pcDefaultColor(index);
        UWORD lower = map ? low[index] : upper;
        ULONG red = ((upper >> 4) & 0xf0) | ((lower >> 8) & 15);
        ULONG green = (upper & 0xf0) | ((lower >> 4) & 15);
        ULONG blue = ((upper << 4) & 0xf0) | (lower & 15);
        palette->colors[index] = (red << 16) | (green << 8) | blue;
    }
}

BOOL pcReadFramebufferPaletteMask(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    const ULONG* mask, struct GfxBase* graphics)
{
    if (!palette || !mask) {
        return FALSE;
    }

    ULONG first = 0;

    while (first < 256) {
        if (!(mask[first >> 5] & (0x80000000UL >> (first & 31)))) {
            ++first;
            continue;
        }

        ULONG end = first + 1;

        while (end < 256 && (mask[end >> 5] & (0x80000000UL >> (end & 31)))) {
            ++end;
        }

        if (!pcReadFramebufferPalette(palette, map, first, end - first, graphics)) {
            return FALSE;
        }

        first = end;
    }

    return TRUE;
}

BOOL pcReadFramebufferPalette(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    ULONG first, ULONG count, struct GfxBase* graphics)
{
    if (!palette || first > 256 || count > 256 - first) {
        return FALSE;
    }

    if (!count) {
        return TRUE;
    }

    if (!map) {
        return FALSE;
    }

    // Keep stack use bounded when called from the display/palette methods.
    // GetRGB32 remains the public owner of component precision and map layout.
    ULONG components[16 * 3];
    ULONG* destination = palette->colors + first;

    while (count) {
        ULONG entries = count < 16 ? count : 16;
        GetRGB32(map, first, entries, components);

        for (ULONG index = 0; index < entries; ++index) {
            const ULONG* color = components + index * 3;
            *destination++ = (color[0] & 0xff000000) >> 8 |
                (color[1] & 0xff000000) >> 16 | color[2] >> 24;
        }

        first += entries;
        count -= entries;
    }

    return TRUE;
}
