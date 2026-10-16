/* SPDX-License-Identifier: MPL-2.0 */
// Native Amiga line DMA. The caller owns the blitter and has waited for it.
#include "internal/chipset.h"
#include "internal/line.h"

#include <exec/memory.h>
#include <hardware/custom.h>
#define __NOLIBBASE__
#include <proto/exec.h>

static volatile struct Custom* const custom = (APTR)0xdff000;

BOOL pcDrawChipsetLine(const struct RastPort* port, const struct BitMap* map,
    const struct Line* line, LONG x, LONG y, LONG error, UWORD count, BOOL one_dot,
    BOOL foreground, struct GfxBase* graphics)
{
    LONG stride = map->BytesPerRow;
    LONG diagonal_step = 4 * (line->minor - line->major);
    LONG minor_step = 4 * line->minor;
    LONG accumulator = 2 * error;

    // OCS line length and signed word modulos apply even to clipped sections.
    // Larger geometry and non-DMA storage retain the complete CPU rasterizer.
    if (!count || count > 1024 || (stride & 1) || stride > 32767 ||
        diagonal_step < -32768 || minor_step > 32767 ||
        accumulator < -32768 || accumulator > 32767 || map->Depth > 8) {
        return FALSE;
    }

    LONG steps = count - 1;
    LONG minor_steps = steps ?
        (error + 2 * line->minor * (steps - 1) + 2 * line->major) / (2 * line->major) : 0;
    LONG end_x = x + line->step_x * (line->major_x ? steps : minor_steps);
    LONG end_y = y + line->step_y * (line->major_x ? minor_steps : steps);
    LONG start_offset = y * stride + (x >> 4) * 2;
    LONG end_offset = end_y * stride + (end_x >> 4) * 2;
    unsigned shift = foreground ? 6 : 4;
    UBYTE* starts[8];
    UBYTE operations[8];
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    // Validate every participating plane before touching any of them. Mixed
    // Chip/Fast storage then falls back as one operation, preserving alias order.
    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        unsigned operation = (port->Mask & (1U << plane)) ? (port->minterms[plane] >> shift) & 3 : 2;
        operations[plane] = operation;

        if (operation == 2) {
            continue;
        }

        UBYTE* start = map->Planes[plane] + start_offset;
        UBYTE* end = map->Planes[plane] + end_offset;

        if (((ULONG)start & 1) || !(TypeOfMem(start) & MEMF_CHIP) || !(TypeOfMem(end + 1) & MEMF_CHIP)) {
            return FALSE;
        }

        starts[plane] = start;
    }

    UWORD control = 1;

    if (line->major_x) {
        control |= 0x10 | (line->step_x < 0 ? 4 : 0) | (line->step_y < 0 ? 8 : 0);
    } else {
        control |= (line->step_x < 0 ? 8 : 0) | (line->step_y < 0 ? 4 : 0);
    }

    if (accumulator < 0) {
        control |= 0x40;
    }

    if (one_dot) {
        control |= 2;
    }

    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        unsigned operation = operations[plane];

        if (operation == 2) {
            continue;
        }

        UBYTE* start = starts[plane];

        custom->bltcon0 = ((x & 15) << 12) | 0x0b00 | (operation << 6) | 0x2a;
        custom->bltcon1 = control;
        custom->bltafwm = 0xffff;
        custom->bltalwm = 0xffff;
        custom->bltamod = diagonal_step;
        custom->bltbmod = minor_step;
        custom->bltcmod = stride;
        custom->bltdmod = stride;
        custom->bltapt = (APTR)accumulator;
        custom->bltcpt = (APTR)start;
        custom->bltdpt = (APTR)start;
        custom->bltadat = 0x8000;
        custom->bltbdat = 0xffff;
        custom->bltsize = (count << 6) | 2;

        // Preserve ordered writes when bitplanes share storage.
        pcWaitChipsetBlit();
    }

    return TRUE;
}
