/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/interrupts.h>
#include <graphics/gfxbase.h>
#include <graphics/monitor.h>

struct ChipsetGraphicsDisplay {
    struct Interrupt blitter;
    struct Interrupt* previous_blitter;
    struct GfxBase* graphics;
    UWORD saved_dma;
    UWORD saved_interrupts;
    UBYTE saved_alarm_select;
    BOOL transfer;
};

void pcInitChipsetRevision(struct GfxBase* graphics);
BOOL pcOpenChipsetDisplay(struct GfxBase* graphics);
BOOL pcCloseChipsetDisplay(struct GfxBase* graphics);
void pcWaitChipsetBlit(void);
ULONG pcChipsetBeamPosition(void);
ULONG pcSetChipsetRevision(ULONG requested, struct GfxBase* graphics);
LONG pcProgramChipsetMonitor(struct MonitorSpec* monitor);
void pcSetChipsetBeamControl(UWORD control);
void pcResetChipsetBlanking(void);
void pcLoadChipsetView(struct GfxBase* graphics, BOOL select_monitor);
LONG pcKillChipsetView(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"));
void pcChipsetColor4(UWORD index, UWORD color);
void pcUpdateChipsetBackground(const struct ViewPort* viewport, ULONG modes,
    const struct MonitorSpec* monitor, struct GfxBase* graphics);

struct ViewPortBuildState;
ULONG pcBuildChipsetViewPortList(struct ViewPort* viewport,
    const struct ViewPortBuildState* state, struct GfxBase* graphics);

struct ViewPortScrollInfo;
struct ViewPortPlaneFetch;
void pcScrollChipsetViewPort(struct ViewPort* viewport, const struct ViewPortBuildState* state,
    const struct ViewPortScrollInfo* info, const struct ViewPortPlaneFetch* planes,
    struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5);
void pcChangeChipsetVPBitMap(struct ViewPort* viewport, struct BitMap* bitmap,
    struct DBufInfo* info, struct GfxBase* graphics);
