/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/scroll.h"
#include "internal/bitmap.h"
#include "internal/memory.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>

static WORD minimum(WORD a, WORD b) { return a < b ? a : b; }
static WORD maximum(WORD a, WORD b) { return a > b ? a : b; }

static BOOL intersect(struct Rectangle* a, const struct Rectangle* b)
{
    a->MinX = maximum(a->MinX, b->MinX);
    a->MinY = maximum(a->MinY, b->MinY);
    a->MaxX = minimum(a->MaxX, b->MaxX);
    a->MaxY = minimum(a->MaxY, b->MaxY);
    return a->MinX <= a->MaxX && a->MinY <= a->MaxY;
}

static void translate(struct Rectangle* bounds, WORD x, WORD y)
{
    bounds->MinX += x; bounds->MaxX += x;
    bounds->MinY += y; bounds->MaxY += y;
}

static void scrollDamage(struct Layer* layer, const struct Rectangle* bounds, WORD dx, WORD dy,
    WORD absolute_x, WORD absolute_y, struct GfxBase* graphics)
{
    struct Region* damage = layer->DamageList;
    if (!(layer->Flags & LAYERSIMPLE) || !damage) { return; }
    struct Region* visible = NewRegion();
    if (!visible) { return; }
    struct Region* shifted = NewRegion();
    if (!shifted) { DisposeRegion(visible); return; }
    for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        if (clip->obscured) { continue; }
        struct Rectangle part = clip->bounds;
        translate(&part, -layer->bounds.MinX, -layer->bounds.MinY);
        if (!intersect(&part, bounds)) { continue; }
        OrRectRegion(visible, &part);
        OrRectRegion(shifted, &part);
    }
    // Lost source pixels and pre-existing damage move into the destination's
    // visible area. Keep existing damage, including outside the scrolled area.
    translate(&shifted->bounds, dx, dy);
    if (XorRegionRegion(visible, shifted)) {
        if (damage->RegionRectangle) { OrRegionRegion(damage, shifted); }
        translate(&shifted->bounds, -dx, -dy);
        if (AndRegionRegion(shifted, visible)) {
            struct Rectangle copied = *bounds;
            if (dy > 0) { copied.MaxY = maximum(bounds->MinY, (WORD)(bounds->MaxY - dy)); }
            if (dy < 0) { copied.MinY = minimum(bounds->MaxY, (WORD)(bounds->MinY + absolute_y)); }
            if (dx > 0) { copied.MaxX = maximum(bounds->MinX, (WORD)(bounds->MaxX - dx)); }
            if (dx < 0) { copied.MinX = minimum(bounds->MaxX, (WORD)(bounds->MinX + absolute_x)); }
            AndRectRegion(visible, &copied);
            if (damage->RegionRectangle) { OrRegionRegion(visible, damage); }
            else { layer->DamageList = visible; visible = damage; }
            if (layer->DamageList->RegionRectangle) {
                layer->Flags |= LAYERREFRESH | LAYERIREFRESH | LAYERIREFRESH2;
            }
        }
    }
    DisposeRegion(shifted);
    DisposeRegion(visible);
}

static void copyClip(struct RastPort* port, const struct ClipRect* source, const struct ClipRect* dest,
    const struct Rectangle* bounds, WORD dx, WORD dy, UWORD* temporary, struct GfxBase* graphics)
{
    WORD sx = bounds->MinX + dx, sy = bounds->MinY + dy;
    WORD tx = bounds->MinX, ty = bounds->MinY;
    const struct BitMap* source_bitmap = source->obscured ? source->BitMap : port->BitMap;
    struct BitMap* dest_bitmap = dest->obscured ? dest->BitMap : port->BitMap;
    if (!source_bitmap) {
        if (!dest->obscured && port->Layer->DamageList) {
            struct Rectangle lost = *bounds;
            translate(&lost, -port->Layer->bounds.MinX, -port->Layer->bounds.MinY);
            OrRectRegion(port->Layer->DamageList, &lost);
            port->Layer->Flags |= LAYERREFRESH | LAYERIREFRESH | LAYERIREFRESH2;
        }
        return;
    }
    if (source->obscured) { sx -= source->bounds.MinX & ~15; sy -= source->bounds.MinY; }
    if (dest->obscured) { tx -= dest->bounds.MinX & ~15; ty -= dest->bounds.MinY; }
    BltBitMap(source_bitmap, sx, sy, dest_bitmap, tx, ty,
        (WORD)(bounds->MaxX - bounds->MinX + 1), (WORD)(bounds->MaxY - bounds->MinY + 1),
        0xca, port->Mask, (APTR)temporary);
}

static void scrollRaster(struct RastPort* port, WORD dx, WORD dy, struct Rectangle bounds,
    BOOL backfill, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Layer* layer = port->Layer;
    UWORD* temporary = NULL;
    BOOL borrowed = FALSE;
    if (!dy) {
        if (port->TmpRas && (ULONG)port->TmpRas->Size >= 4096) { temporary = (APTR)port->TmpRas->RasPtr; }
        borrowed = temporary != NULL;
        if (!borrowed) { temporary = pcAllocScratch(graphics, 4096, MEMF_PUBLIC | MEMF_CHIP); }
    }
    WORD absolute_x = dx < 0 ? -dx : dx, absolute_y = dy < 0 ? -dy : dy;
    WORD source_x = dx < 0 ? 0 : absolute_x, source_y = dy < 0 ? 0 : absolute_y;
    WORD dest_x = dx < 0 ? absolute_x : 0, dest_y = dy < 0 ? absolute_y : 0;
    struct Rectangle effective = bounds;
    if (!layer) {
        effective.MaxX = minimum(effective.MaxX, (WORD)(port->BitMap->BytesPerRow * 8 - 1));
        effective.MaxY = minimum(effective.MaxY, (WORD)(port->BitMap->Rows - 1));
        WORD width = effective.MaxX - bounds.MinX - absolute_x + 1;
        WORD height = effective.MaxY - bounds.MinY - absolute_y + 1;
        // The original unlayered path returns before filling when nothing
        // remains to copy, even when the displacement exposes the entire area.
        if (width <= 0 || height <= 0) { goto done; }
        BltBitMap(port->BitMap, (WORD)(bounds.MinX + source_x), (WORD)(bounds.MinY + source_y),
            port->BitMap, (WORD)(bounds.MinX + dest_x), (WORD)(bounds.MinY + dest_y),
            width, height, 0xca, port->Mask, (APTR)temporary);
    } else {
        ObtainSemaphore(&layer->Lock);
        if (layer->SuperBitMap) {
            pcSyncSBitMap(layer, graphics);
            struct RastPort plain = *port;
            plain.Layer = NULL;
            plain.BitMap = layer->SuperBitMap;
            scrollRaster(&plain, dx, dy, bounds, backfill, graphics);
            pcCopySBitMap(layer, graphics);
            ReleaseSemaphore(&layer->Lock);
            goto done;
        }
        effective.MaxX = minimum(effective.MaxX, (WORD)(layer->bounds.MaxX - layer->bounds.MinX));
        effective.MaxY = minimum(effective.MaxY, (WORD)(layer->bounds.MaxY - layer->bounds.MinY));
        scrollDamage(layer, &effective, dx, dy, absolute_x, absolute_y, graphics);
        struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
        SortLayerCR(layer, dx, dy);
        struct Rectangle screen = bounds;
        translate(&screen, layer->bounds.MinX, layer->bounds.MinY);
        // Process destination clips in scroll order. Copy within a clip first,
        // then bring pixels from the later clips before their storage changes.
        for (const struct ClipRect* dest = layer->ClipRect; dest; dest = dest->Next) {
            if (dest->obscured && !dest->BitMap) { continue; }
            struct Rectangle target = screen;
            if (!intersect(&target, &dest->bounds)) { continue; }
            WORD width = target.MaxX - target.MinX - absolute_x + 1;
            WORD height = target.MaxY - target.MinY - absolute_y + 1;
            if (width > 0 && height > 0) {
                struct Rectangle self = {target.MinX + dest_x, target.MinY + dest_y,
                    target.MinX + dest_x + width - 1, target.MinY + dest_y + height - 1};
                copyClip(port, dest, dest, &self, dx, dy, temporary, graphics);
            }
            for (const struct ClipRect* source = dest->Next; source; source = source->Next) {
                struct Rectangle part = source->bounds;
                if (layer->DamageList && !intersect(&part, &screen)) { continue; }
                translate(&part, -dx, -dy);
                if (intersect(&part, &target)) { copyClip(port, source, dest, &part, dx, dy, temporary, graphics); }
            }
        }
        ReleaseSemaphore(&layer->Lock);
    }

    struct RastPort fill;
    InitRastPort(&fill);
    fill.BitMap = port->BitMap; fill.Layer = layer; fill.Mask = port->Mask;
    SetAPen(&fill, (UBYTE)port->BgPen);
    struct Rectangle strips[2] = {effective, effective};
    if (dy > 0) { strips[0].MinY = maximum(bounds.MinY, (WORD)(effective.MaxY - dy + 1)); }
    else { strips[0].MaxY = bounds.MinY + absolute_y - 1; }
    if (dx > 0) { strips[1].MinX = maximum(bounds.MinX, (WORD)(effective.MaxX - dx + 1)); }
    else { strips[1].MaxX = bounds.MinX + absolute_x - 1; }
    for (unsigned i = 0; i < 2; ++i) {
        if (!(i ? dx : dy)) { continue; }
        const struct Rectangle* strip = &strips[i];
        if (backfill) { pcEraseRect(&fill, strip->MinX, strip->MinY, strip->MaxX, strip->MaxY, graphics); }
        else { pcRectFill(&fill, strip->MinX, strip->MinY, strip->MaxX, strip->MaxY, graphics); }
    }
done:
    if (!dy && !borrowed) { WaitBlit(); pcFreeScratch(graphics, temporary, 4096); }
}

void pcScrollRaster(struct RastPort* port asm("a1"), WORD dx asm("d0"), WORD dy asm("d1"),
    WORD min_x asm("d2"), WORD min_y asm("d3"), WORD max_x asm("d4"), WORD max_y asm("d5"),
    struct GfxBase* graphics asm("a6"))
{
    struct Rectangle bounds = {min_x, min_y, max_x, max_y};
    scrollRaster(port, dx, dy, bounds, FALSE, graphics);
}

void pcScrollRasterBF(struct RastPort* port asm("a1"), WORD dx asm("d0"), WORD dy asm("d1"),
    WORD min_x asm("d2"), WORD min_y asm("d3"), WORD max_x asm("d4"), WORD max_y asm("d5"),
    struct GfxBase* graphics asm("a6"))
{
    struct Rectangle bounds = {min_x, min_y, max_x, max_y};
    scrollRaster(port, dx, dy, bounds, TRUE, graphics);
}
