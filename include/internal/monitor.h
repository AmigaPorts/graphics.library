/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/monitor.h>

struct MonitorSpec* pcOpenMonitor(CONST_STRPTR name asm("a1"), ULONG display_id asm("d0"),
    struct GfxBase* graphics asm("a6"));
BOOL pcCloseMonitor(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"));
LONG pcGetVPModeID(const struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"));
LONG pcModeNotAvailable(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"));
LONG pcMonitorTransform(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"));
LONG pcMonitorTranslate(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"));
LONG pcMonitorScale(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"));
LONG pcMonitorMaxOverscan(struct MonitorSpec* monitor asm("a0"), struct Rectangle* rectangle asm("a1"),
    ULONG modes asm("d0"), struct GfxBase* graphics asm("a6"));
LONG pcMonitorVideoScan(struct MonitorSpec* monitor asm("a0"), struct Rectangle* rectangle asm("a1"),
    ULONG modes asm("d0"), struct GfxBase* graphics asm("a6"));

LONG pcViewportFetchMode(const struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"));
ULONG pcCoerceMode(struct ViewPort* viewport asm("a0"), ULONG monitor_id asm("d0"),
    ULONG flags asm("d1"), struct GfxBase* graphics asm("a6"));
ULONG pcBestModeIDA(const struct TagItem* tags asm("a0"), struct GfxBase* graphics asm("a6"));

ULONG pcViewportModes(const struct ViewPort* viewport);
