/* SPDX-License-Identifier: APL-1.1 */
/* Hardware identification derived from AROS arch/m68k-amiga/graphics/setchiprev.c.
 * Copyright (C) 1995-2025, The AROS Development Team. All rights reserved.
 * See CHIPSET.md and LICENSE.APL for provenance and license terms.
 */
#define ECS_SPECIFIC
#include "internal/chipset.h"
#include "internal/library.h"
#include "internal/state.h"
#include "internal/frame.h"
#include "internal/blitter.h"
#include "internal/monitor.h"

#include <exec/memory.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#define __NOLIBBASE__
#include <proto/exec.h>

// The custom-chip address is part of the real Amiga hardware specification.
// This translation unit is absent from a framebuffer-only build.
static volatile struct Custom* const custom = (APTR)0xdff000;

ULONG pcChipsetBeamPosition(void)
{
    // Reading VPOSR again detects the low-byte carry between the two reads.
    // Do not mix the previous high part with a new VHPOSR after row 255.
    UWORD high, low, following;

    do {
        high = custom->vposr;
        low = custom->vhposr;
        following = custom->vposr;
    } while ((high ^ following) & 7);

    return ((high & 7) << 8) | (low >> 8);
}

void pcWaitChipsetBlit(void)
{
    // Older Agnus does not assert busy until the first DMA slot is granted.
    // Chip-bus reads before the loop provide the documented startup interlock.
    (void)custom->dmaconr;
    (void)custom->dmaconr;

    while (custom->dmaconr & DMAF_BLTDONE) {
    }
}

static UBYTE detectChips(struct GfxBase* graphics)
{
    UWORD agnus = custom->vposr & 0x7f00;

    if (agnus & 0x0200) {
        return SETCHIPREV_AA | GFXF_AA_MLISA;
    }

    if (!(agnus & 0x2000)) {
        return 0;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Disable();
    UWORD first = custom->deniseid & 255;
    custom->deniseid = custom->dmaconr;
    UWORD second = custom->deniseid & 255;
    custom->deniseid = custom->dmaconr ^ 0x8000;
    UWORD third = custom->deniseid & 255;
    Enable();

    return GFXF_HR_AGNUS | ((first == 0xfc && second == first && third == first) ? GFXF_HR_DENISE : 0);
}

void pcInitChipsetRevision(struct GfxBase* graphics)
{
    // VPOSR's Agnus identification contains the motherboard PAL/NTSC
    // selection. Exec's frequency may still be uncalibrated at cold start.
    UWORD identification = custom->vposr;
    graphics->DisplayFlags = identification & 0x1000 ? NTSC : PAL;

    // Original Agnus needs the interlaced field swap when light-pen input is
    // enabled. Keep this hardware capability independent of SetChipRev.
    if (!(identification & 0x2000)) {
        graphics->DisplayFlags |= LPEN_SWAP_FRAMES;
    } else if (!(identification & 0x1000)) {
        graphics->DisplayFlags |= REALLY_PAL;
    }

    // Original startup reports ECS-compatible operation even on AA hardware.
    // SetChipRev enables the additional capabilities when requested by the OS;
    // displayable bitmap alignment must follow that same public selection.
    graphics->ChipRevBits0 = detectChips(graphics) & ~(GFXF_AA_ALICE | GFXF_AA_LISA);

    // AGA's monitor workarounds are initialized even while the public
    // chipset selection remains ECS-compatible.
    graphics->Bugs = graphics->ChipRevBits0 & GFXF_AA_MLISA ? 0x25 : 0;
    graphics->MemType = BANDWIDTH_1X;
    graphics->ColorMask = 0xf0000000;
}

ULONG pcSetChipsetRevision(ULONG requested, struct GfxBase* graphics)
{
    UBYTE chips = detectChips(graphics);
    UBYTE memory = chips & GFXF_AA_LISA ? ((UWORD)~custom->deniseid >> 8) & 3 : BANDWIDTH_1X;
    struct GraphicsChipRevision revision = pcApplyChipRevision(requested, chips, memory, graphics);

    if (revision.changed && graphics->DisplayInfoDataBase) {
        pcRefreshDisplayRevision(revision.reported, graphics);
    }

    return revision.reported;
}

void pcChipsetColor4(UWORD index, UWORD color)
{
    // The NULL-ViewPort API addresses the current hardware palette directly.
    custom->color[index] = color;
}

LONG pcProgramChipsetMonitor(struct MonitorSpec* monitor)
{
    struct GfxBase* graphics = monitor->ms_Node.xln_Library;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    const struct SpecialMonitor* timing = monitor->ms_Special;
    UWORD clocks = monitor->total_colorclocks;

    custom->htotal = clocks;
    custom->hbstrt = timing->hblank.asi_Start;
    custom->hbstop = timing->hblank.asi_Stop;
    custom->hsstrt = timing->hsync.asi_Start;
    custom->hsstop = timing->hsync.asi_Stop;
    custom->hcenter = (clocks >> 1) + timing->hsync.asi_Start;
    custom->vsstrt = timing->vsync.asi_Start / clocks;
    custom->vsstop = timing->vsync.asi_Stop / clocks;

    // Vertical intervals are expressed in color clocks. Retain the logical
    // blanking rows here; the frame interrupt programs the selected field.
    if (graphics->Bugs & 4) {
        graphics->MonitorVBlank.asi_Start = monitor->total_rows;
        graphics->MonitorVBlank.asi_Stop = monitor->min_row - 1;
    } else {
        graphics->MonitorVBlank.asi_Start = timing->vblank.asi_Start / clocks;
        graphics->MonitorVBlank.asi_Stop = timing->vblank.asi_Stop / clocks;
    }

    custom->bplcon0 = 0;
    custom->bplcon1 = 0;
    custom->bplcon2 = 0x24;
    custom->fmode = 0;

    // Establish the original temporary window before the new Copper field.
    // The calculations retain the MonitorSpec's signed fixed-point ratios.
    ULONG horizontal = monitor->ratioh;
    ULONG vertical = monitor->ratiov;
    UWORD left = ((LONG)(horizontal * 129) >> 4) & 255;
    ULONG top = (ULONG)((LONG)(vertical * 44) >> 4) << 8;
    ULONG bottom = (ULONG)((LONG)(vertical * 244) >> 4) << 8;
    UWORD right = ((LONG)(horizontal * 193) >> 4) & 255;
    custom->diwstrt = top | left;
    custom->diwstop = bottom || right;
    custom->ddfstrt = (LONG)(horizontal * 56) >> 4;
    custom->ddfstop = (LONG)(horizontal * 208) >> 4;

    Disable();

    while ((custom->vhposr & 255) >= 48) {
    }

    custom->vposw = custom->vposr | 0x8000;
    Enable();

    return 1;
}

void pcSetChipsetBeamControl(UWORD control)
{
    custom->beamcon0 = control;
}

void pcResetChipsetBlanking(void)
{
    custom->vbstrt = 0;
}

static void programFrame(struct GfxBase* graphics)
{
    const struct MonitorSpec* monitor = graphics->current_monitor;
    UWORD start = 0;
    UWORD stop = 0;
    UWORD beam = 0;
    BOOL external_sync = (graphics->system_bplcon0 & 8) && (graphics->ChipRevBits0 & GFXF_HR_AGNUS);

    if (monitor) {
        custom->vtotal = monitor->total_rows;
        beam = monitor->BeamCon0;

        if (external_sync || (!(graphics->system_bplcon0 & 8) && (graphics->Bugs & 0x10))) {
            custom->beamcon0 = beam | (external_sync ? 0x2000 : 0);
            graphics->Bugs &= ~0x10;
        }

        start = graphics->MonitorVBlank.asi_Start;
        stop = graphics->MonitorVBlank.asi_Stop;
    }

    BOOL short_field = (graphics->Modes & LACE) && !(custom->vposr & 0x8000);

    if (!short_field && (graphics->Bugs & 4)) {
        ++start;
    }

    if (!(graphics->Modes & LACE) && !(graphics->system_bplcon0 & LACE) && !(custom->vposr & 0x8000)) {
        custom->vposw = custom->vposr | 0x8000;
    }

    custom->vbstrt = start;
    custom->vbstop = stop;

    if (monitor && external_sync) {
        custom->beamcon0 = beam & ~0x2000;
    }
}

static void selectField(struct GfxBase* graphics)
{
    BOOL short_field = (graphics->Modes & LACE) && !(custom->vposr & 0x8000);
    custom->cop2lc = (ULONG)(short_field ? graphics->SHFlist : graphics->LOFlist);
}

void pcLoadChipsetView(struct GfxBase* graphics, BOOL select_monitor)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Disable();

    // A flagless monitor or blank View uses the motherboard's standard timing.
    // Other monitor programming belongs to the selection/totals change path.
    // A palette refresh does not select a monitor or reset its hardware state.
    if (select_monitor) {
        const struct MonitorSpec* monitor = graphics->current_monitor;

        if (!monitor || !monitor->ms_Flags) {
            custom->beamcon0 = graphics->DisplayFlags & PAL ? STANDARD_PAL_BEAMCON : STANDARD_NTSC_BEAMCON;
        }
    }

    if (!pcActiveViewFields(graphics)->owned) {
        Enable();
        return;
    }

    selectField(graphics);
    custom->cop1lc = (ULONG)graphics->copinit;
    custom->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE;
    Enable();
}

LONG pcKillChipsetView(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"))
{
    (void)monitor;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Disable();
    custom->dmacon = DMAF_COPPER | DMAF_RASTER | DMAF_SPRITE;
    pcClearActiveViewFields(graphics);
    Enable();

    return 0;
}

static void dispatchBlitter(struct ChipsetGraphicsDisplay* chipset)
{
    struct GfxBase* graphics = chipset->graphics;

    if (chipset->transfer) {
        return;
    }

    while (graphics->blthd) {
        struct bltnode* node = graphics->blthd;
        pcWaitChipsetBlit();

        if ((UWORD)node->beamsync & 0x8000) {
            UWORD line = (UWORD)node->beamsync & 0x7fff;

            while (pcChipsetBeamPosition() < line) {
            }

            node->beamsync &= 0x7fff;
        }

        // Retire any completed transfer's interrupt before starting another
        // stage. Otherwise an old pending interrupt could complete new DMA.
        custom->intreq = INTF_BLIT;
        custom->intreq = INTF_BLIT;

        if (!pcDispatchBlitter((APTR)custom, graphics)) {
            return;
        }

        // A callback may skip a masked plane and start no DMA at all. Check
        // real hardware after the Agnus startup interlock; callback Z alone
        // says whether this is the last stage, not whether DMA was submitted.
        (void)custom->dmaconr;
        (void)custom->dmaconr;

        if (custom->dmaconr & DMAF_BLTDONE) {
            chipset->transfer = TRUE;
            return;
        }

        // A very short transfer can also finish before the busy sample.
        // Consume its interrupt before advancing or releasing this node.
        custom->intreq = INTF_BLIT;
        custom->intreq = INTF_BLIT;

        if (!pcCompleteBlitter((APTR)custom, graphics)) {
            return;
        }
    }
}

static ULONG startBlitter(struct GfxBase* graphics asm("a1"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Disable();
    dispatchBlitter(((struct GraphicsLibrary*)graphics)->chipset);
    Enable();

    return 0;
}

static ULONG completeBlitter(struct ChipsetGraphicsDisplay* chipset asm("a1"))
{
    custom->intreq = INTF_BLIT;
    custom->intreq = INTF_BLIT;

    if (chipset->transfer) {
        chipset->transfer = FALSE;

        if (pcCompleteBlitter((APTR)custom, chipset->graphics)) {
            dispatchBlitter(chipset);
        }
    }

    return 0;
}

static ULONG chipsetFrame(struct ChipsetGraphicsDisplay* chipset asm("a1"))
{
    // A queued beam dependency belongs to the frame in which it was added.
    // Once that frame ends, retain the requested row but expire its wait,
    // including requests still blocked behind another blitter owner.
    for (struct bltnode* node = chipset->graphics->blthd; node; node = node->n) {
        node->beamsync &= 0x7fff;
    }

    if (pcActiveViewFields(chipset->graphics)->owned) {
        programFrame(chipset->graphics);
        selectField(chipset->graphics);
    }

    return pcGraphicsFrame(chipset->graphics);
}

BOOL pcOpenChipsetDisplay(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;

    if (library->chipset) {
        return TRUE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ChipsetGraphicsDisplay* chipset = AllocMem(sizeof(*chipset), MEMF_PUBLIC | MEMF_CLEAR);

    if (!chipset) {
        return FALSE;
    }

    chipset->graphics = graphics;
    chipset->frame.is_Node.ln_Type = NT_INTERRUPT;
    chipset->frame.is_Node.ln_Name = "graphics.library chipset frame";
    chipset->frame.is_Data = chipset;
    chipset->frame.is_Code = (APTR)chipsetFrame;
    chipset->blitter.is_Node.ln_Type = NT_INTERRUPT;
    chipset->blitter.is_Node.ln_Name = "graphics.library blitter";
    chipset->blitter.is_Data = chipset;
    chipset->blitter.is_Code = (APTR)completeBlitter;
    graphics->bltsrv.is_Node.ln_Type = NT_INTERRUPT;
    graphics->bltsrv.is_Node.ln_Name = "graphics.library blitter queue";
    graphics->bltsrv.is_Data = graphics;
    graphics->bltsrv.is_Code = (APTR)startBlitter;

    Disable();
    chipset->saved_dma = custom->dmaconr;
    chipset->saved_interrupts = custom->intenar;
    library->chipset = chipset;
    chipset->previous_blitter = SetIntVector(INTB_BLIT, &chipset->blitter);
    custom->intreq = INTF_BLIT;
    custom->intena = INTF_SETCLR | INTF_BLIT;
    custom->dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_BLITTER;
    AddIntServer(INTB_VERTB, &chipset->frame);
    Enable();

    return TRUE;
}

BOOL pcCloseChipsetDisplay(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;
    struct ChipsetGraphicsDisplay* chipset = library->chipset;

    if (!chipset) {
        return TRUE;
    }

    if (graphics->blthd || chipset->transfer) {
        return FALSE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Disable();
    custom->intena = INTF_BLIT;
    RemIntServer(INTB_VERTB, &chipset->frame);
    SetIntVector(INTB_BLIT, chipset->previous_blitter);
    custom->intena = INTF_SETCLR | (chipset->saved_interrupts & INTF_BLIT);
    custom->dmacon = DMAF_BLITTER & ~chipset->saved_dma;
    library->chipset = NULL;
    Enable();
    FreeMem(chipset, sizeof(*chipset));

    return TRUE;
}
