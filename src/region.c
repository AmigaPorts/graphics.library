/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/region.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#include <proto/exec.h>

static void releasePool(struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    while (graphics->Scratch) {
        struct RegionRectangle* node = graphics->Scratch;
        graphics->Scratch = node->Next;
        FreeMem(node, sizeof(*node));
    }
    graphics->ScratchSize = 0;
}

void pcFlushRegionPool(struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();
    releasePool(graphics);
    Permit();
}

void pcInitRegionPool(struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    while (graphics->ScratchSize < 32) {
        struct RegionRectangle* node = AllocMem(sizeof(*node), MEMF_PUBLIC);

        if (!node) {
            break;
        }

        node->Next = graphics->Scratch;
        graphics->Scratch = node;
        ++graphics->ScratchSize;
    }
}

struct RegionRectangle* pcAllocRegionNode(struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();
    struct RegionRectangle* node = graphics->Scratch;
    if (node) {
        graphics->Scratch = node->Next;
        --graphics->ScratchSize;
    }
    Permit();
    if (!node) { return AllocMem(sizeof(*node), MEMF_PUBLIC | MEMF_CLEAR); }
    node->Next = NULL; node->Prev = NULL;
    node->bounds.MinX = 0; node->bounds.MinY = 0;
    node->bounds.MaxX = 0; node->bounds.MaxY = 0;
    return node;
}

void pcFreeRegionNode(struct GfxBase* graphics, struct RegionRectangle* node)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();
    node->Next = graphics->Scratch;
    graphics->Scratch = node;
    if (++graphics->ScratchSize >= 128) {
        releasePool(graphics);
        pcInitRegionPool(graphics);
    }
    Permit();
}

struct Region* pcNewRegion(struct GfxBase* graphics asm("a6"))
{
    return (APTR)pcAllocRegionNode(graphics);
}

static void clearRectangles(struct GfxBase* graphics, struct Region* region)
{
    struct RegionRectangle* node = region->RegionRectangle;
    while (node) {
        struct RegionRectangle* next = node->Next;
        pcFreeRegionNode(graphics, node);
        node = next;
    }
    region->RegionRectangle = NULL;
}

void pcClearRegion(struct Region* region asm("a0"), struct GfxBase* graphics asm("a6"))
{
    region->bounds.MinX = 0; region->bounds.MinY = 0;
    region->bounds.MaxX = 0; region->bounds.MaxY = 0;
    clearRectangles(graphics, region);
}

void pcDisposeRegion(struct Region* region asm("a0"), struct GfxBase* graphics asm("a6"))
{
    clearRectangles(graphics, region);
    pcFreeRegionNode(graphics, (APTR)region);
}

static struct Rectangle intersectBounds(const struct Rectangle* first, const struct Rectangle* second)
{
    struct Rectangle result;
    result.MinX = first->MinX > second->MinX ? first->MinX : second->MinX;
    result.MinY = first->MinY > second->MinY ? first->MinY : second->MinY;
    result.MaxX = first->MaxX < second->MaxX ? first->MaxX : second->MaxX;
    result.MaxY = first->MaxY < second->MaxY ? first->MaxY : second->MaxY;
    return result;
}

static BOOL intersects(const struct Rectangle* first, const struct Rectangle* second)
{
    return first->MinX <= second->MaxX && first->MinY <= second->MaxY &&
        first->MaxX >= second->MinX && first->MaxY >= second->MinY;
}

static BOOL contains(const struct Rectangle* outer, const struct Rectangle* inner)
{
    return outer->MinX <= inner->MinX && outer->MinY <= inner->MinY &&
        outer->MaxX >= inner->MaxX && outer->MaxY >= inner->MaxY;
}

static void subtractOrigin(struct Rectangle* bounds, WORD x, WORD y)
{
    bounds->MinX -= x; bounds->MaxX -= x;
    bounds->MinY -= y; bounds->MaxY -= y;
}

static void translateRectangles(struct Region* region, WORD x, WORD y)
{
    for (struct RegionRectangle* node = region->RegionRectangle; node; node = node->Next) {
        subtractOrigin(&node->bounds, x, y);
    }
}

static void normalizeOrigin(struct Region* region)
{
    if (!region->RegionRectangle) { return; }
    WORD x = 32767, y = 32767;
    for (struct RegionRectangle* node = region->RegionRectangle; node; node = node->Next) {
        if (node->bounds.MinX < x) { x = node->bounds.MinX; }
        if (node->bounds.MinY < y) { y = node->bounds.MinY; }
    }
    translateRectangles(region, x, y);
    // The original moves both corners; it does not recompute the far bounds
    // from the remaining rectangles after clipping.
    region->bounds.MinX += x; region->bounds.MaxX += x;
    region->bounds.MinY += y; region->bounds.MaxY += y;
}

static void prependRectangle(struct Region* region, struct RegionRectangle* node)
{
    node->Next = region->RegionRectangle;
    node->Prev = (APTR)&region->RegionRectangle;
    if (node->Next) { node->Next->Prev = node; }
    region->RegionRectangle = node;
}

static void appendRectangle(struct Region* region, struct RegionRectangle* node)
{
    struct RegionRectangle* last = region->RegionRectangle;
    node->Next = NULL;
    if (!last) {
        node->Prev = (APTR)&region->RegionRectangle;
        region->RegionRectangle = node;
        return;
    }
    while (last->Next) { last = last->Next; }
    node->Prev = last;
    last->Next = node;
}

static void unlinkRectangle(struct Region* region, struct RegionRectangle* node)
{
    if (node->Next) { node->Next->Prev = node->Prev; }
    if (node->Prev == (APTR)&region->RegionRectangle) { region->RegionRectangle = node->Next; }
    else { node->Prev->Next = node->Next; }
}

static void replaceRegion(struct GfxBase* graphics, struct Region* destination, struct Region* source)
{
    pcClearRegion(destination, graphics);
    destination->bounds = source->bounds;
    destination->RegionRectangle = source->RegionRectangle;
    source->RegionRectangle = NULL;
    if (destination->RegionRectangle) {
        destination->RegionRectangle->Prev = (APTR)&destination->RegionRectangle;
    }
    normalizeOrigin(destination);
}

static struct Rectangle expandBounds(struct Region* region, const struct Rectangle* rectangle)
{
    struct Rectangle bounds = *rectangle;
    if (region->RegionRectangle) {
        if (region->bounds.MinX < bounds.MinX) { bounds.MinX = region->bounds.MinX; }
        if (region->bounds.MinY < bounds.MinY) { bounds.MinY = region->bounds.MinY; }
        if (region->bounds.MaxX > bounds.MaxX) { bounds.MaxX = region->bounds.MaxX; }
        if (region->bounds.MaxY > bounds.MaxY) { bounds.MaxY = region->bounds.MaxY; }
        if (bounds.MinX != region->bounds.MinX || bounds.MinY != region->bounds.MinY) {
            translateRectangles(region, bounds.MinX - region->bounds.MinX, bounds.MinY - region->bounds.MinY);
        }
    }
    region->bounds = bounds;
    return bounds;
}

static BOOL appendBounds(struct GfxBase* graphics, struct Region* region, const struct Rectangle* bounds)
{
    struct RegionRectangle* node = pcAllocRegionNode(graphics);
    if (!node) { return FALSE; }
    node->bounds = *bounds;
    appendRectangle(region, node);
    return TRUE;
}

static BOOL subtractRectangle(struct GfxBase* graphics, struct Region* result,
    const struct Rectangle* removed, struct Rectangle remainder)
{
    if (!intersects(removed, &remainder)) { return appendBounds(graphics, result, &remainder); }
    // Strip order is observable in RegionRectangle lists: bottom, top, right,
    // left. Each subsequent strip uses the vertically/horizontally reduced box.
    if (remainder.MaxY > removed->MaxY) {
        struct Rectangle strip = remainder;
        strip.MinY = removed->MaxY + 1;
        if (!appendBounds(graphics, result, &strip)) { return FALSE; }
        remainder.MaxY = removed->MaxY;
    }
    if (remainder.MinY < removed->MinY) {
        struct Rectangle strip = remainder;
        strip.MaxY = removed->MinY - 1;
        if (!appendBounds(graphics, result, &strip)) { return FALSE; }
        remainder.MinY = removed->MinY;
    }
    if (remainder.MaxX > removed->MaxX) {
        struct Rectangle strip = remainder;
        strip.MinX = removed->MaxX + 1;
        if (!appendBounds(graphics, result, &strip)) { return FALSE; }
        remainder.MaxX = removed->MaxX;
    }
    if (remainder.MinX < removed->MinX) {
        struct Rectangle strip = remainder;
        strip.MaxX = removed->MinX - 1;
        if (!appendBounds(graphics, result, &strip)) { return FALSE; }
    }
    return TRUE;
}

static BOOL andRectRegion(struct GfxBase* graphics, struct Region* region, const struct Rectangle* rectangle)
{
    if (!region->RegionRectangle) { return TRUE; }
    struct Region result;
    result.bounds = intersectBounds(&region->bounds, rectangle);
    result.RegionRectangle = NULL;
    if (result.bounds.MinX != region->bounds.MinX || result.bounds.MinY != region->bounds.MinY) {
        translateRectangles(region, result.bounds.MinX - region->bounds.MinX,
            result.bounds.MinY - region->bounds.MinY);
    }
    region->bounds = result.bounds;
    struct Rectangle clip = *rectangle;
    subtractOrigin(&clip, region->bounds.MinX, region->bounds.MinY);

    for (struct RegionRectangle* node = region->RegionRectangle; node;) {
        struct RegionRectangle* next = node->Next;
        if (intersects(&node->bounds, &clip)) {
            if (contains(&node->bounds, &clip)) {
                struct RegionRectangle* clipped = pcAllocRegionNode(graphics);
                if (!clipped) { clearRectangles(graphics, &result); return FALSE; }
                clipped->bounds = clip;
                result.RegionRectangle = clipped;
                break;
            } else if (contains(&clip, &node->bounds)) {
                unlinkRectangle(region, node);
                prependRectangle(&result, node);
            } else {
                struct RegionRectangle* clipped = pcAllocRegionNode(graphics);
                if (!clipped) { clearRectangles(graphics, &result); return FALSE; }
                clipped->bounds = intersectBounds(&node->bounds, &clip);
                appendRectangle(&result, clipped);
            }
        }
        node = next;
    }
    // Allocation failure above deliberately retains the original's partial
    // mutations. The public SDK declares this operation VOID.
    replaceRegion(graphics, region, &result);
    return TRUE;
}

void pcAndRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    // The public API is VOID; region intersection also needs the internal
    // allocation result, as in the original implementation.
    (void)andRectRegion(graphics, region, rectangle);
}

BOOL pcClearRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Rectangle removed = *rectangle;
    struct Region result;
    result.bounds = expandBounds(region, rectangle);
    result.RegionRectangle = NULL;
    subtractOrigin(&removed, result.bounds.MinX, result.bounds.MinY);
    for (struct RegionRectangle* node = region->RegionRectangle; node; node = node->Next) {
        if (!contains(&removed, &node->bounds) && !subtractRectangle(graphics, &result, &removed, node->bounds)) {
            clearRectangles(graphics, &result);
            return FALSE;
        }
    }
    replaceRegion(graphics, region, &result);
    return TRUE;
}

BOOL pcOrRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Region added;
    added.bounds = expandBounds(region, rectangle);
    added.RegionRectangle = NULL;
    if (!appendBounds(graphics, &added, rectangle)) { return FALSE; }
    subtractOrigin(&added.RegionRectangle->bounds, region->bounds.MinX, region->bounds.MinY);
    for (struct RegionRectangle* existing = region->RegionRectangle; existing; existing = existing->Next) {
        for (struct RegionRectangle* node = added.RegionRectangle; node;) {
            struct RegionRectangle* next = node->Next;
            if (intersects(&existing->bounds, &node->bounds)) {
                if (!contains(&existing->bounds, &node->bounds)) {
                    if (!subtractRectangle(graphics, &added, &existing->bounds, node->bounds)) {
                        clearRectangles(graphics, &added);
                        return FALSE;
                    }
                    // A split of the last pending rectangle appended new
                    // pieces; include those in the current traversal too.
                    if (!next) { next = node->Next; }
                }
                unlinkRectangle(&added, node);
                pcFreeRegionNode(graphics, node);
            }
            node = next;
        }
    }
    struct RegionRectangle* node = added.RegionRectangle;
    while (node) {
        struct RegionRectangle* next = node->Next;
        prependRectangle(region, node);
        node = next;
    }
    return TRUE;
}

static struct Rectangle absoluteBounds(const struct Region* region, const struct RegionRectangle* node)
{
    struct Rectangle bounds = node->bounds;
    bounds.MinX += region->bounds.MinX; bounds.MaxX += region->bounds.MinX;
    bounds.MinY += region->bounds.MinY; bounds.MaxY += region->bounds.MinY;
    return bounds;
}

BOOL pcOrRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    for (const struct RegionRectangle* node = source->RegionRectangle; node;) {
        const struct RegionRectangle* next = node->Next;
        struct Rectangle bounds = absoluteBounds(source, node);
        if (!pcOrRectRegion(destination, &bounds, graphics)) { return FALSE; }
        node = next;
    }
    return TRUE;
}

BOOL pcAndRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct Region work = {0};
    struct Region result = {0};
    for (const struct RegionRectangle* node = source->RegionRectangle; node;) {
        const struct RegionRectangle* next = node->Next;
        if (!pcOrRegionRegion(destination, &work, graphics)) { goto failed; }
        struct Rectangle bounds = absoluteBounds(source, node);
        if (!andRectRegion(graphics, &work, &bounds) || !pcOrRegionRegion(&work, &result, graphics)) {
            goto failed;
        }
        pcClearRegion(&work, graphics);
        node = next;
    }
    clearRectangles(graphics, &work);
    replaceRegion(graphics, destination, &result);
    clearRectangles(graphics, &result);
    return TRUE;

failed:
    clearRectangles(graphics, &work);
    clearRectangles(graphics, &result);
    return FALSE;
}

static BOOL xorFailure(struct GfxBase* graphics, struct Region* active, struct RegionRectangle* pending)
{
    clearRectangles(graphics, active);
    // Compatibility with the original allocation-failure path: it treats the
    // current rectangle as a Region, taking its first coordinate longword as
    // a list pointer. This can corrupt the cache and leaks pending storage.
    // Keep the observed behavior explicit; do not silently fix the public API.
    struct RegionRectangle* node = (APTR)(((ULONG)(UWORD)pending->bounds.MinX << 16) |
        (UWORD)pending->bounds.MinY);
    while (node) {
        struct RegionRectangle* next = node->Next;
        pcFreeRegionNode(graphics, node);
        node = next;
    }
    pending->bounds.MinX = 0; pending->bounds.MinY = 0;
    return FALSE;
}

BOOL pcXorRectRegion(struct Region* region asm("a0"), const struct Rectangle* rectangle asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    if (!region->RegionRectangle) { return pcOrRectRegion(region, rectangle, graphics); }
    struct Region result;
    result.bounds = expandBounds(region, rectangle);
    result.RegionRectangle = NULL;
    struct Region added = result;
    struct RegionRectangle* pending = pcAllocRegionNode(graphics);
    if (!pending) { return FALSE; }
    pending->bounds = *rectangle;
    subtractOrigin(&pending->bounds, region->bounds.MinX, region->bounds.MinY);
    for (struct RegionRectangle* node = region->RegionRectangle; node; node = node->Next) {
        if (!subtractRectangle(graphics, &result, &pending->bounds, node->bounds)) {
            return xorFailure(graphics, &result, pending);
        }
    }
    prependRectangle(&added, pending);
    for (struct RegionRectangle* node = region->RegionRectangle; node; node = node->Next) {
        pending = added.RegionRectangle;
        added.RegionRectangle = NULL;
        while (pending) {
            if (!subtractRectangle(graphics, &added, &node->bounds, pending->bounds)) {
                return xorFailure(graphics, &added, pending);
            }
            struct RegionRectangle* next = pending->Next;
            pcFreeRegionNode(graphics, pending);
            pending = next;
        }
    }
    pending = added.RegionRectangle;
    while (pending) {
        struct RegionRectangle* next = pending->Next;
        unlinkRectangle(&added, pending);
        prependRectangle(&result, pending);
        pending = next;
    }
    replaceRegion(graphics, region, &result);
    return TRUE;
}

BOOL pcXorRegionRegion(const struct Region* source asm("a0"), struct Region* destination asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    for (const struct RegionRectangle* node = source->RegionRectangle; node;) {
        const struct RegionRectangle* next = node->Next;
        struct Rectangle bounds = absoluteBounds(source, node);
        if (!pcXorRectRegion(destination, &bounds, graphics)) { return FALSE; }
        node = next;
    }
    return TRUE;
}
