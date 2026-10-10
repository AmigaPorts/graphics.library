/* SPDX-License-Identifier: MPL-2.0 */
// Independent implementation of the public planar RastPort state APIs.
#include "internal/rastport.h"

#include <stddef.h>
#include <graphics/clip.h>
#include <graphics/rpattr.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>
#include <proto/utility.h>

void pcUpdateMinterms(struct RastPort* port)
{
    UBYTE mode = port->DrawMode;
    BOOL inverse = (mode & INVERSVID) != 0;
    for (unsigned plane = 0; plane < 8; ++plane) {
        UBYTE foreground = ((UBYTE)port->FgPen >> plane) & 1;
        UBYTE background = ((UBYTE)port->BgPen >> plane) & 1;
        UBYTE value;
        // The low nibble preserves destination C outside the selected area A.
        // Within A, B selects foreground/background (or the affected pixels
        // for JAM1/complement). These are software drawing truth tables, not
        // blitter programming or a register model.
        if (mode & COMPLEMENT) {
            value = (mode & JAM2) ? 0x5a : (inverse ? 0x9a : 0x6a);
        } else if (mode & JAM2) {
            if (inverse) {
                UBYTE temporary = foreground;
                foreground = background;
                background = temporary;
            }
            value = 0x0a | (foreground ? 0xc0 : 0) | (background ? 0x30 : 0);
        } else {
            value = inverse ? (foreground ? 0xba : 0x8a) : (foreground ? 0xea : 0x2a);
        }
        port->minterms[plane] = value;
    }
}

static void restartPattern(struct RastPort* port)
{
    port->linpatcnt = 15;
    port->Flags |= FRST_DOT;
}

void pcInitRastPort(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"))
{
    // Preserve the original initialization extent: the later reserved tail
    // was not part of the initial RastPort ABI. Use its SDK field boundary.
    UBYTE* data = (UBYTE*)port;
    for (ULONG i = 0; i < offsetof(struct RastPort, wordreserved); ++i) { data[i] = 0; }
    port->Mask = 0xff;
    port->FgPen = -1;
    port->AOlPen = -1;
    port->LinePtrn = 0xffff;
    port->DrawMode = JAM2;
    pcUpdateMinterms(port);
    // Keep the public call so subsequent owners of SetFont remain in control.
    SetFont(port, graphics->DefaultFont);
}

void pcMove(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"))
{
    port->cp_x = x;
    port->cp_y = y;
    restartPattern(port);
}

void pcSetAPen(struct RastPort* port asm("a1"), UBYTE pen asm("d0"))
{
    port->FgPen = pen;
    restartPattern(port);
    pcUpdateMinterms(port);
}

void pcSetBPen(struct RastPort* port asm("a1"), UBYTE pen asm("d0"))
{
    port->BgPen = pen;
    restartPattern(port);
    pcUpdateMinterms(port);
}

void pcSetDrMd(struct RastPort* port asm("a1"), UBYTE mode asm("d0"))
{
    port->DrawMode = mode;
    restartPattern(port);
    pcUpdateMinterms(port);
}

void pcSetABPenDrMd(struct RastPort* port asm("a1"), ULONG foreground asm("d0"),
    ULONG background asm("d1"), ULONG mode asm("d2"))
{
    if ((UBYTE)port->FgPen == (UBYTE)foreground && (UBYTE)port->BgPen == (UBYTE)background &&
        (UBYTE)port->DrawMode == (UBYTE)mode) { return; }
    port->FgPen = foreground;
    port->BgPen = background;
    port->DrawMode = mode;
    restartPattern(port);
    pcUpdateMinterms(port);
}

ULONG pcGetAPen(struct RastPort* port asm("a0")) { return (UBYTE)port->FgPen; }
ULONG pcGetBPen(struct RastPort* port asm("a0")) { return (UBYTE)port->BgPen; }
ULONG pcGetDrMd(struct RastPort* port asm("a0")) { return (UBYTE)port->DrawMode; }
ULONG pcGetOutlinePen(struct RastPort* port asm("a0")) { return (UBYTE)port->AOlPen; }

ULONG pcSetOutlinePen(struct RastPort* port asm("a0"), ULONG pen asm("d0"))
{
    ULONG previous = (UBYTE)port->AOlPen;
    port->AOlPen = pen;
    port->Flags |= AREAOUTLINE;
    return previous;
}

ULONG pcSetWriteMask(struct RastPort* port asm("a0"), ULONG mask asm("d0"))
{
    port->Mask = mask;
    return ~0UL;
}

void pcSetMaxPen(struct RastPort* port asm("a0"), ULONG pen asm("d0"))
{
    UBYTE highest = pen;
    UBYTE mask = highest ? 1 : 0xff;
    while ((highest >>= 1)) { mask = (mask << 1) | 1; }
    port->Mask = mask;
}

void pcSetRPAttrsA(struct RastPort* port asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct TagItem* cursor = (APTR)tags;
    for (;;) {
        struct Library* UtilityBase = graphics->UtilBase;
        struct TagItem* tag = NextTagItem(&cursor);
        if (!tag) { return; }
        switch (tag->ti_Tag) {
            case RPTAG_Font: SetFont(port, (APTR)tag->ti_Data); break;
            case RPTAG_APen: SetAPen(port, (UBYTE)tag->ti_Data); break;
            case RPTAG_BPen: SetBPen(port, (UBYTE)tag->ti_Data); break;
            case RPTAG_DrMd: SetDrMd(port, (UBYTE)tag->ti_Data); break;
            case RPTAG_OutlinePen: SetOutlinePen(port, tag->ti_Data); break;
            case RPTAG_WriteMask: SetWriteMask(port, tag->ti_Data); break;
            case RPTAG_MaxPen: SetMaxPen(port, tag->ti_Data); break;
        }
    }
}

static void drawBounds(struct RastPort* port, struct Rectangle* bounds, struct GfxBase* graphics)
{
    struct Layer* layer = port->Layer;
    if (!layer || layer->SuperBitMap) {
        bounds->MinX = 0;
        bounds->MinY = 0;
        struct BitMap* bitmap = layer ? layer->SuperBitMap : port->BitMap;
        bounds->MaxX = GetBitMapAttr(bitmap, BMA_WIDTH) - 1;
        bounds->MaxY = GetBitMapAttr(bitmap, BMA_HEIGHT) - 1;
        return;
    }
    LockLayerRom(layer);
    WORD min_x = 32767, min_y = 32767, max_x = -32767, max_y = -32767;
    for (struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        if (clip->obscured && !clip->BitMap) { continue; }
        WORD left = clip->bounds.MinX - layer->bounds.MinX + layer->Scroll_X;
        WORD right = clip->bounds.MaxX - layer->bounds.MinX + layer->Scroll_X;
        WORD top = clip->bounds.MinY - layer->bounds.MinY + layer->Scroll_Y;
        WORD bottom = clip->bounds.MaxY - layer->bounds.MinY + layer->Scroll_Y;
        if (left < min_x) { min_x = left; }
        if (right > max_x) { max_x = right; }
        if (top < min_y) { min_y = top; }
        if (bottom > max_y) { max_y = bottom; }
    }
    bounds->MinX = min_x;
    bounds->MinY = min_y;
    bounds->MaxX = max_x;
    bounds->MaxY = max_y;
    UnlockLayerRom(layer);
}

void pcGetRPAttrsA(struct RastPort* port asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct TagItem* cursor = (APTR)tags;
    for (;;) {
        struct Library* UtilityBase = graphics->UtilBase;
        struct TagItem* tag = NextTagItem(&cursor);
        if (!tag) { return; }
        ULONG* result = (APTR)tag->ti_Data;
        switch (tag->ti_Tag) {
            case RPTAG_Font: *result = (ULONG)port->Font; break;
            case RPTAG_APen: *result = GetAPen(port); break;
            case RPTAG_BPen: *result = GetBPen(port); break;
            case RPTAG_DrMd: *result = GetDrMd(port); break;
            case RPTAG_OutlinePen: *result = GetOutlinePen(port); break;
            case RPTAG_WriteMask: *result = port->Mask; break;
            case RPTAG_MaxPen: {
                UBYTE mask = port->Mask;
                ULONG limit = 256;
                while (limit && (BYTE)mask > 0) { limit >>= 1; mask <<= 1; }
                *result = limit;
                break;
            }
            case RPTAG_DrawBounds: drawBounds(port, (APTR)result, graphics); break;
        }
    }
}
