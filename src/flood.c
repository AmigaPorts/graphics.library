/* SPDX-License-Identifier: MPL-2.0 */
// Planar flood filling through public graphics vectors and Exec allocation.
#include "internal/flood.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

struct FloodSpan {
    struct FloodSpan* next;
    UWORD left, right, y;
    WORD direction;
};
struct FloodPool {
    struct FloodPool* next;
    UWORD reserved;
    struct FloodSpan spans[8];
};
struct Flood {
    struct FloodSpan* free;
    struct FloodSpan* pending;
    struct FloodPool* pools;
    struct RastPort* port;
    struct GfxBase* graphics;
    WORD max_x, max_y;
};

static BOOL pushSpan(struct Flood* fill, LONG left, LONG right, LONG y, WORD direction)
{
    if (!fill->free) {
        struct ExecBase* SysBase = (APTR)fill->graphics->ExecBase;
        struct FloodPool* pool = AllocMem(sizeof(*pool), MEMF_PUBLIC);
        if (!pool) { return FALSE; }
        pool->next = fill->pools;
        fill->pools = pool;
        for (unsigned i = 0; i < 8; ++i) {
            pool->spans[i].next = fill->free;
            fill->free = &pool->spans[i];
        }
    }
    struct FloodSpan* span = fill->free;
    fill->free = span->next;
    span->left = left; span->right = right; span->y = y; span->direction = direction;
    span->next = fill->pending;
    fill->pending = span;
    return TRUE;
}

static struct FloodSpan popSpan(struct Flood* fill)
{
    struct FloodSpan* span = fill->pending;
    struct FloodSpan result = *span;
    fill->pending = span->next;
    span->next = fill->free;
    fill->free = span;
    return result;
}

static void freeSpans(struct Flood* fill)
{
    struct ExecBase* SysBase = (APTR)fill->graphics->ExecBase;
    while (fill->pools) {
        struct FloodPool* pool = fill->pools;
        fill->pools = pool->next;
        FreeMem(pool, sizeof(*pool));
    }
}

static LONG readPixel(struct Flood* fill, LONG x, LONG y)
{
    struct GfxBase* graphics = fill->graphics;
    return ReadPixel(fill->port, (WORD)x, (WORD)y);
}

static BOOL findSpan(struct Flood* fill, LONG x, LONG y, LONG* left, LONG* right)
{
    LONG boundary = fill->port->AOlPen;
    LONG pixel = readPixel(fill, x, y);
    if (pixel == boundary) {
        do {
            if (x > fill->max_x) { return FALSE; }
            pixel = readPixel(fill, ++x, y);
        } while (pixel == boundary);
    } else {
        while (x > 0 && readPixel(fill, x - 1, y) != boundary) { --x; }
    }
    *left = x;
    while (x < fill->max_x && readPixel(fill, x + 1, y) != boundary) { ++x; }
    *right = x;
    return TRUE;
}

static void scanSpans(struct Flood* fill, LONG first, LONG end, LONG y, WORD direction)
{
    for (LONG x = first; x < end; ++x) {
        LONG pixel = readPixel(fill, x, y);
        if (pixel == fill->port->FgPen || pixel == fill->port->AOlPen) { continue; }
        LONG left, right;
        if (findSpan(fill, x, y, &left, &right)) {
            pushSpan(fill, left, right, y, direction);
            x = right + 1;
        }
    }
}

static void directFill(struct Flood* fill, LONG x, LONG y)
{
    struct GfxBase* graphics = fill->graphics;
    LONG pixel = readPixel(fill, x, y);
    if (pixel == -1 || pixel == fill->port->AOlPen) { return; }
    LONG left, right, next_left, next_right;
    findSpan(fill, x, y, &left, &right);
    pushSpan(fill, left, right, y, 1);
    if (y > 0 && findSpan(fill, left, y - 1, &next_left, &next_right) &&
        next_left <= right && next_right >= left) {
        pushSpan(fill, next_left, next_right, y - 1, -1);
        if (next_left < left || next_right > right) { pushSpan(fill, next_left, next_right, y - 1, 1); }
        if (next_right < right) {
            scanSpans(fill, next_right, right, y, 1);
            scanSpans(fill, next_right, right, y - 1, -1);
        }
        if (next_left > left) {
            scanSpans(fill, left, next_left, y, 1);
            scanSpans(fill, left, next_left, y - 1, -1);
        }
    }
    while (fill->pending) {
        struct FloodSpan span = popSpan(fill);
        Move(fill->port, span.left, span.y);
        Draw(fill->port, span.right, span.y);
        LONG next_y = (LONG)span.y + span.direction;
        if (next_y < 0 || next_y > fill->max_y ||
            !findSpan(fill, span.left, next_y, &next_left, &next_right) || next_left > span.right ||
            readPixel(fill, next_left, next_y) == fill->port->FgPen) { continue; }
        if (next_right < span.right) { scanSpans(fill, next_right, span.right, next_y, -span.direction); }
        if (next_left > span.left) { scanSpans(fill, span.left, next_left, next_y, -span.direction); }
        if (next_left < (LONG)span.left - 1) {
            scanSpans(fill, next_left, (LONG)span.left - 1, span.y, -span.direction);
        }
        if (next_right > (LONG)span.right + 1) {
            scanSpans(fill, (LONG)span.right + 1, next_right, span.y, -span.direction);
        }
        if (next_right < (LONG)span.right - 1) {
            scanSpans(fill, next_right + 2, span.right, next_y, span.direction);
        }
        pushSpan(fill, next_left, next_right, next_y, span.direction);
        if (next_left < (LONG)span.left - 1 || next_right > span.right) {
            pushSpan(fill, next_left, next_right, next_y, -span.direction);
        }
    }
}

static BOOL blocked(const UBYTE* mask, UWORD stride, LONG x, LONG y)
{
    return !!(mask[y * stride + (x >> 3)] & (0x80U >> (x & 7)));
}

static void mark(UBYTE* mask, UWORD stride, LONG x, LONG y)
{
    mask[y * stride + (x >> 3)] |= 0x80U >> (x & 7);
}

static BOOL maskFill(struct Flood* fill, UBYTE* mask, UBYTE* selected, UWORD stride, LONG x, LONG y)
{
    if (blocked(mask, stride, x, y)) { return TRUE; }
    BOOL good = pushSpan(fill, x, x, y, 0);
    while (fill->pending) {
        struct FloodSpan span = popSpan(fill);
        x = span.left; y = span.y;
        if (blocked(mask, stride, x, y)) { continue; }
        while (x > 0 && !blocked(mask, stride, x - 1, y)) { --x; }
        LONG left = x;
        do {
            mark(mask, stride, x, y);
            mark(selected, stride, x, y);
        } while (++x <= fill->max_x && !blocked(mask, stride, x, y));
        LONG right = x - 1;
        for (WORD direction = -1; direction <= 1; direction += 2) {
            LONG next_y = y + direction;
            if (next_y < 0 || next_y > fill->max_y) { continue; }
            BOOL in_span = FALSE;
            for (x = left; x <= right; ++x) {
                if (blocked(mask, stride, x, next_y)) { in_span = FALSE; }
                else if (!in_span) {
                    if (!pushSpan(fill, x, x, next_y, 0)) { good = FALSE; }
                    in_span = TRUE;
                }
            }
        }
    }
    return good;
}

static BOOL temporaryFill(struct RastPort* port, ULONG mode, LONG x, LONG y, struct GfxBase* graphics)
{
    LONG target = mode ? (LONG)ReadPixel(port, (WORD)x, (WORD)y) : port->AOlPen;
    if (mode && target == -1) { return TRUE; }
    UWORD stride = port->BitMap->BytesPerRow, rows = port->BitMap->Rows;
    ULONG bytes = port->TmpRas->Size;
    if (port->Layer) {
        const struct Rectangle* bounds = &port->Layer->bounds;
        stride = (WORD)(((bounds->MaxX + 16) & ~15) - (bounds->MinX & ~15)) >> 3;
        rows = bounds->MaxY - bounds->MinY + 1;
        ULONG required = (ULONG)stride * rows;
        if (required > bytes) { return FALSE; }
        bytes = required;
    }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    UBYTE* selected = AllocMem(bytes, MEMF_PUBLIC | MEMF_CHIP | MEMF_CLEAR);
    if (!selected) { return FALSE; }
    UBYTE* mask = (APTR)port->TmpRas->RasPtr;

    // BltClear takes an even byte count; retain the complete TmpRas size.
    BltClear(mask, bytes & ~1UL, 1);
    if (bytes & 1) { mask[bytes - 1] = 0; }

    // The comparison raster treats unsaved simple-refresh pixels as absent,
    // not as a readable background pen. Complement only for color fill.
    for (LONG row = 0; row < rows; ++row) {
        for (LONG column = 0; column < (LONG)stride * 8; ++column) {
            LONG pixel = ReadPixel(port, (WORD)column, (WORD)row);
            BOOL equal = pixel != -1 && ((ULONG)pixel == ((ULONG)target & ((1UL << port->BitMap->Depth) - 1)));
            if (mode == 1 ? !equal : equal) { mark(mask, stride, column, row); }
        }
    }
    struct Flood fill = {0};
    fill.port = port; fill.graphics = graphics;
    fill.max_x = stride * 8 - 1; fill.max_y = rows - 1;
    BOOL good = maskFill(&fill, mask, selected, stride, x, y);
    freeSpans(&fill);
    if (good) { BltPattern(port, selected, 0, 0, fill.max_x, fill.max_y, stride); }
    WaitBlit();
    FreeMem(selected, bytes);
    return good;
}

// Return the original full D0 Boolean, including when ReadPixel returned -1.
LONG pcFlood(struct RastPort* port asm("a1"), ULONG mode asm("d2"), LONG x asm("d0"), LONG y asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Layer* layer = port->Layer;
    if (layer) { ObtainSemaphore(&layer->Lock); }
    BOOL result = FALSE;
    if (layer && layer->SuperBitMap) {
        if (port->TmpRas) {
            SyncSBitMap(layer);
            struct RastPort temporary = *port;
            temporary.Layer = NULL;
            temporary.BitMap = layer->SuperBitMap;
            result = temporaryFill(&temporary, mode, x, y, graphics);
            CopySBitMap(layer);
        }
    } else if (port->TmpRas) { result = temporaryFill(port, mode, x, y, graphics); }
    else if (!mode) {
        struct Flood fill = {0};
        fill.port = port; fill.graphics = graphics;
        fill.max_x = port->BitMap->BytesPerRow * 8 - 1;
        fill.max_y = port->BitMap->Rows - 1;
        directFill(&fill, x, y);
        freeSpans(&fill);
        // The original direct path ignores span-allocation failure.
        result = TRUE;
    }
    if (layer) { ReleaseSemaphore(&layer->Lock); }
    return result;
}
