/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/monitor.h>

typedef LONG (*MonitorProgramFunction)(struct MonitorSpec* monitor);

// Startup only: select the standard monitor requested by DisplayFlags on a
// fresh, unpublished library. State and display database must already exist.
BOOL pcInitDefaultMonitor(struct GfxBase* graphics);
// Roll back that selection after freeing its display database and view users.
// This is not the public CloseMonitor operation or a live monitor removal API.
void pcFreeDefaultMonitor(struct GfxBase* graphics);

// Initialize the software portion of a MonitorSpec, as its ExtendedNode
// initializer does. Retain caller-owned callbacks and identity. Special-monitor
// allocation/release uses the public GfxNew/GfxFree vectors; program is the
// backend's do_monitor method, installed but never called by initialization.
// The standard geometry is AmigaOS coordinate metadata, not a description of
// the PC's physical scanout. Native presentation obtains that separately.
UWORD pcInitMonitorSpec(struct MonitorSpec* monitor, UWORD flags, MonitorProgramFunction program,
    struct GfxBase* graphics);

struct MonitorSpec* pcOpenMonitor(CONST_STRPTR name asm("a1"), ULONG display_id asm("d0"),
    struct GfxBase* graphics asm("a6"));
BOOL pcCloseMonitor(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"));
BOOL pcSetDefaultMonitor(UWORD monitor_id asm("d0"), struct GfxBase* graphics asm("a6"));
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
