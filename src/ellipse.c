/* SPDX-License-Identifier: MPL-2.0 */
// Integer ellipse rasterization through the public pixel owner.
#include "internal/ellipse.h"
#include "internal/backend.h"
#include "internal/native.h"

#include <stdint.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

static void pixel(struct RastPort* port, WORD x, WORD y, BOOL edge_mask, struct GfxBase* graphics)
{
    if (edge_mask) {
        // Area-fill masks use a single, unlayered XOR plane. This CPU path
        // retains the same bit operation without requiring a hardware blitter.
        UBYTE* dest = port->BitMap->Planes[0] + (LONG)y * (WORD)port->BitMap->BytesPerRow + (x >> 3);
        *dest ^= 0x80U >> (x & 7);
    } else { WritePixel(port, x, y); }
}

static void quadrants(struct RastPort* port, WORD center_x, WORD center_y, LONG x, LONG y,
    BOOL edge_mask, struct GfxBase* graphics)
{
    WORD right = center_x + x, left = center_x - x;
    WORD bottom = center_y + y, top = center_y - y;
    pixel(port, right, bottom, edge_mask, graphics);
    if ((port->Flags & ONE_DOT) || left < right) { pixel(port, left, bottom, edge_mask, graphics); }
    if (top < bottom) {
        pixel(port, right, top, edge_mask, graphics);
        if ((port->Flags & ONE_DOT) || left < right) { pixel(port, left, top, edge_mask, graphics); }
    }
}

void pcDrawEllipse(struct RastPort* port asm("a1"), WORD center_x asm("d0"), WORD center_y asm("d1"),
    WORD radius_x asm("d2"), WORD radius_y asm("d3"), struct GfxBase* graphics asm("a6"))
{
    BOOL one_dot = (port->Flags & ONE_DOT) != 0;
    BOOL edge_mask = !port->Layer && port->minterms[0] == 0x6a && (port->Mask & 1) && port->BitMap->Depth == 1;

    // The direct XOR path bypasses WritePixel. Finish preceding DMA before
    // changing its raster, including when the caller already owns the blitter.
    if (edge_mask && pcUsesChipset(graphics)) {
        pcWaitBlit(graphics);
    }

    LONG x = radius_x, y = 0;
    ULONG square_x = (LONG)radius_x * radius_x;
    ULONG square_y = (LONG)radius_y * radius_y;
    // Two midpoint decisions describe the steep and shallow portions. The
    // squared radii and their products require more than 32 bits for legal
    // WORD radii; keep only integer additions in the rasterization loops.
    int64_t twice_x = (int64_t)square_x * 2, twice_y = (int64_t)square_y * 2;
    int64_t delta_x = twice_y * (UWORD)radius_x * 2, delta_y = 0;
    int64_t steep = twice_x + square_y - delta_x / 2;
    int64_t shallow = twice_y + square_x - delta_x;
    while (shallow < 0) {
        if (!one_dot || (WORD)(center_x + x) != (WORD)(center_x - x)) {
            quadrants(port, center_x, center_y, x, y, edge_mask, graphics);
        }
        ++y;
        delta_y += twice_x * 2;
        if (steep < 0) {
            steep += delta_y + twice_x;
            shallow += delta_y;
        } else {
            --x;
            delta_x -= twice_y * 2;
            steep += delta_y + twice_x - delta_x;
            shallow += delta_y + twice_y - delta_x;
        }
    }
    BOOL row_drawn = FALSE;
    while (x >= 0) {
        if (!one_dot || (!row_drawn && (WORD)(center_x + x) != (WORD)(center_x - x))) {
            quadrants(port, center_x, center_y, x, y, edge_mask, graphics);
            row_drawn = TRUE;
        }
        --x;
        delta_x -= twice_y * 2;
        if (shallow < 0) {
            ++y;
            row_drawn = FALSE;
            delta_y += twice_x * 2;
            shallow += twice_y + delta_y - delta_x;
        } else { shallow += twice_y - delta_x; }
    }
    if (!one_dot) {
        // Very narrow ellipses can leave a vertical tail after x reaches zero.
        if (row_drawn) { ++y; }
        for (; y <= radius_y; ++y) {
            WORD top = center_y - y, bottom = center_y + y;
            if (top < bottom) { pixel(port, center_x, top, edge_mask, graphics); }
            pixel(port, center_x, bottom, edge_mask, graphics);
        }
    }
}
