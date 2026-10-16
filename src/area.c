/* SPDX-License-Identifier: MPL-2.0 */
// Public area construction and independent CPU edge-mask filling.
#include "internal/area.h"
#include "internal/ellipse.h"
#include "internal/line.h"
#include "internal/rastport.h"

#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

// SDK AreaInfo flag-table entries describe vertical edges, draw endpoints,
// requested outlines and ellipses. Ellipses occupy two coordinate entries.
#define AREA_VERTICAL 1
#define AREA_DRAW 2
#define AREA_OUTLINE 4
#define AREA_ELLIPSE 128

static LONG closeArea(struct RastPort* port, struct GfxBase* graphics)
{
    struct AreaInfo* area = port->AreaInfo;
	
    if (!area->Count) {
	    return 0;
    }
	
    if (!area->FlagPtr[-1]) {
        // A move without a following edge is replaced by the next shape.
        --area->Count;
        --area->FlagPtr;
        area->VctrPtr -= 2;
        return 0;
    }
	
    if (area->Count >= 2 && (area->FlagPtr[-2] & AREA_ELLIPSE)) {
	    return 0;
    }
	
    if (area->VctrPtr[-2] == area->FirstX && area->VctrPtr[-1] == area->FirstY) {
	    return 0;
    }
	
    // Respect the installed public vector, including failure to append the
    // closing edge. A completed closure is retained if the next shape is full.
    return AreaDraw(port, area->FirstX, area->FirstY);
}

LONG pcAreaMove(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct AreaInfo* area = port->AreaInfo;
	
    if (closeArea(port, graphics) < 0 || (WORD)(area->Count + 1) > area->MaxCount) {
	    return -1;
    }
	
    ++area->Count;
    *area->VctrPtr++ = area->FirstX = x;
    *area->VctrPtr++ = area->FirstY = y;
    *area->FlagPtr++ = 0;
	
    return 0;
}

LONG pcAreaDraw(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"))
{
    struct AreaInfo* area = port->AreaInfo;
	
    if (!area->Count || (WORD)(area->Count + 1) > area->MaxCount) {
	    return -1;
    }
	
    UBYTE flags = AREA_DRAW;
	
    if (port->Flags & AREAOUTLINE) {
	    flags |= AREA_OUTLINE;
    }
	
    if (y != area->VctrPtr[-1]) {
	    flags |= AREA_VERTICAL;
    }
	
    ++area->Count;
    *area->VctrPtr++ = x;
    *area->VctrPtr++ = y;
    *area->FlagPtr++ = flags;
	
    return 0;
}

LONG pcAreaEllipse(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"),
    WORD radius_x asm("d2"), WORD radius_y asm("d3"), struct GfxBase* graphics asm("a6"))
{
    struct AreaInfo* area = port->AreaInfo;
	
    if (closeArea(port, graphics) < 0 || (WORD)(area->Count + 2) > area->MaxCount) {
	    return -1;
    }
	
    UBYTE flags = AREA_ELLIPSE | AREA_DRAW | AREA_VERTICAL;
	
    if (port->Flags & AREAOUTLINE) {
	    flags |= AREA_OUTLINE;
    }
	
    area->Count += 2;
    *area->VctrPtr++ = x - radius_x;
    *area->VctrPtr++ = y - radius_y;
    *area->VctrPtr++ = x + radius_x;
    *area->VctrPtr++ = y + radius_y;
    *area->FlagPtr++ = flags;
    *area->FlagPtr++ = AREA_DRAW;
    return 0;
}

// Each row starts outside the polygon at its right edge. Edge construction,
// boundary drawing, and the destination's pattern and clipping are separate.
void pcFillArea(UWORD* mask asm("a1"), UWORD stride asm("d4"), UWORD rows asm("d1"))
{
    // A byte stride can contain at most 32767 complete words.
    UWORD words = stride >> 1;

    while (rows--) {
        unsigned carry = 0;
        UWORD x = words;

        while (x >= 2) {
            // Join two words explicitly so the prefix operation is independent
            // of host byte order and does not require longword-aligned storage.
            ULONG bits = ((ULONG)mask[x - 2] << 16) | mask[x - 1];

            // Carry enters at the rightmost bit and propagates through the prefix.
            bits ^= carry;
            bits ^= bits << 1;
            bits ^= bits << 2;
            bits ^= bits << 4;
            bits ^= bits << 8;
            bits ^= bits << 16;

            carry = bits >> 31;
            mask[x - 1] = bits;
            mask[x - 2] = bits >> 16;
            x -= 2;
        }

        if (x) {
            // Parallel prefix parity: each result bit includes every edge
            // bit to its right, including itself. Carry spans word boundaries
            // but resets on the next row. This processes sixteen pixels at once.
            UWORD bits = mask[x - 1];

            bits ^= carry;
            bits ^= bits << 1;
            bits ^= bits << 2;
            bits ^= bits << 4;
            bits ^= bits << 8;

            mask[x - 1] = bits;
        }

        mask += words;
    }
}

static void areaEllipse(struct RastPort* port, const WORD* points, struct GfxBase* graphics)
{
    WORD x = (WORD)(points[0] + points[2]) >> 1;
    WORD y = (WORD)(points[1] + points[3]) >> 1;
    DrawEllipse(port, x, y, (WORD)(points[2] - x), (WORD)(points[3] - y));
}

static void outlineArea(struct RastPort* port, WORD origin_x, WORD origin_y, struct GfxBase* graphics)
{
    UBYTE minterms[8];
	
    for (unsigned i = 0; i < 8; ++i) {
	    minterms[i] = port->minterms[i];
    }
	
    UWORD pattern = port->LinePtrn;
    BYTE foreground = port->FgPen, mode = port->DrawMode;
    struct GelsInfo* gels = port->GelsInfo;
    port->FgPen = port->AOlPen;
    port->DrawMode = JAM1;
    pcUpdateMinterms(port);
    port->LinePtrn = 0xffff;
    port->GelsInfo = NULL;
    const struct AreaInfo* area = port->AreaInfo;
	
    for (WORD i = 0; i < area->Count; ++i) {
        const WORD* points = area->VctrTbl + 2 * i;
        UBYTE flags = area->FlagTbl[i];
    	
        if (flags & AREA_ELLIPSE) {
            UBYTE second = area->FlagTbl[++i];
        	
            if ((flags & AREA_OUTLINE) && (second == 1 || second == 2)) {
                WORD x = ((LONG)points[0] + points[2]) >> 1;
                WORD y = ((LONG)points[1] + points[3]) >> 1;
                pcDrawEllipse(port, (WORD)(x + origin_x), (WORD)(y + origin_y),
                    (WORD)(points[2] - x), (WORD)(points[3] - y), graphics);
            }
        } else if (flags & AREA_OUTLINE) {
            Draw(port, (WORD)(points[0] + origin_x), (WORD)(points[1] + origin_y));
        } else {
            Move(port, (WORD)(points[0] + origin_x), (WORD)(points[1] + origin_y));
        }
    }
	
    port->GelsInfo = gels;
    port->LinePtrn = pattern;
    port->FgPen = foreground;
    port->DrawMode = mode;
	
    for (unsigned i = 0; i < 8; ++i) {
	    port->minterms[i] = minterms[i];
    }
}

LONG pcAreaEnd(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"))
{
    struct AreaInfo* area = port->AreaInfo;
	
    if (!area) {
	    return -1;
    }
	
    // The empty-list path leaves the AreaInfo address in the return register.
    LONG result = (LONG)area;
	
    if (!area->Count || (area->Count == 1 && !area->FlagPtr[-1])) {
	    goto finished;
    }
	
    if (area->FlagPtr[-1] && !(area->Count >= 2 && (area->FlagPtr[-2] & AREA_ELLIPSE)) &&
        (area->VctrPtr[-2] != area->FirstX || area->VctrPtr[-1] != area->FirstY)) {
        if (AreaDraw(port, area->FirstX, area->FirstY) < 0) {
	        result = -1; goto finished;
        }
    }
	
    WORD min_x = area->VctrTbl[0], max_x = min_x;
    WORD min_y = area->VctrTbl[1], max_y = min_y;
	
    for (WORD i = 1; i < area->Count; ++i) {
        WORD x = area->VctrTbl[2 * i], y = area->VctrTbl[2 * i + 1];
        if (x < min_x) { min_x = x; }
        if (x > max_x) { max_x = x; }
        if (y < min_y) { min_y = y; }
        if (y > max_y) { max_y = y; }
    }
	
    min_x &= ~15;
	
    for (WORD i = 0; i < area->Count; ++i) {
        area->VctrTbl[2 * i] -= min_x;
        area->VctrTbl[2 * i + 1] -= min_y;
    }
	
    UWORD stride = (UWORD)(((max_x + 16) & ~15) - min_x) >> 3;
    UWORD rows = max_y - min_y + 1;
    PLANEPTR mask = (PLANEPTR)port->TmpRas->RasPtr;
	
    BltClear(mask, ((ULONG)rows << 16) | stride, 2);
	
    struct BitMap map;
    InitBitMap(&map, 1, stride * 8, rows);
	
    map.Planes[0] = mask;
    struct RastPort edge = {0};
    edge.BitMap = &map;
    edge.Mask = 0xff;
    edge.LinePtrn = 0xffff;
    edge.Flags = ONE_DOT;
    edge.DrawMode = COMPLEMENT;
    edge.minterms[0] = 0x6a;
	
    int previous = 0, first = 1;
	
    for (WORD i = 0; i < area->Count; ++i) {
        const WORD* points = area->VctrTbl + 2 * i;
        UBYTE flags = area->FlagTbl[i];
        edge.Flags |= FRST_DOT;
    	
        if (flags & AREA_VERTICAL) {
            if (flags & AREA_ELLIPSE) {
                if (area->FlagTbl[++i] == AREA_DRAW) {
	                areaEllipse(&edge, points, graphics);
                }
            } else {
                // Consecutive edges with the same vertical direction share
                // one crossing. Turning points instead retain both crossings.
                if (!previous) {
                    if (points[1] > edge.cp_y) {
	                    edge.Flags &= ~FRST_DOT;
                    }
                    else {
	                    previous = 1;
                    }
                } else if (previous > 0) {
                    if (points[1] < edge.cp_y) {
	                    edge.Flags &= ~FRST_DOT;
                    }
                    else {
	                    previous = 0;
                    }
                } else {
	                previous = points[1] < edge.cp_y;
                }
            	
                pcDraw(&edge, points[0], points[1], graphics);
            	
                if (first < 0) {
	                first = previous;
                }
            }
        } else {
            if (!(flags & (AREA_DRAW | AREA_ELLIPSE))) {
                if (previous == first && previous >= 0) {
	                WritePixel(&edge, edge.cp_x, edge.cp_y);
                }
            	
                previous = first = -1;
            }
        	
            pcMove(&edge, points[0], points[1]);
        }
    }
	
    edge.Flags = FRST_DOT;
    edge.linpatcnt = 15;
	
    if (previous == first && previous >= 0) {
	    WritePixel(&edge, edge.cp_x, edge.cp_y);
    }
	
    pcFillArea((UWORD*)mask, stride, rows);

    // Retain the original combined DrawMode/AreaPtSz condition: JAM1 with
    // a four-row area pattern excludes the boundary from the filled mask.
    edge.DrawMode = JAM1;
    edge.minterms[0] = port->DrawMode == JAM1 && port->AreaPtSz == 2 ? 0x0a : 0xfa;
	
    for (WORD i = 0; i < area->Count; ++i) {
        const WORD* points = area->VctrTbl + 2 * i;
        UBYTE flags = area->FlagTbl[i];
    	
        if (flags & AREA_DRAW) {
            if (flags & AREA_ELLIPSE) {
                if (area->FlagTbl[++i] == AREA_DRAW) {
	                areaEllipse(&edge, points, graphics);
                }
            } else {
	            pcDraw(&edge, points[0], points[1], graphics);
            }
        } else {
	        pcMove(&edge, points[0], points[1]);
        }
    }
	
    BltPattern(port, mask, min_x, min_y, max_x, max_y, stride);
	
    if (port->Flags & AREAOUTLINE) {
	    outlineArea(port, min_x, min_y, graphics);
    }
	
    result = 0;
	
finished:
    area->Count = 0;
    area->VctrPtr = area->VctrTbl;
    area->FlagPtr = area->FlagTbl;
	
    return result;
}
