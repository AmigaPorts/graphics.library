/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/framebuffer.h"

#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

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
