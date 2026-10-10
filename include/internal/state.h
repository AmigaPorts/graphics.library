/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>

// Internal startup storage for a zeroed, unpublished GfxBase with ExecBase set.
// This does not publish a library, initialize a monitor or install interrupts.
BOOL pcInitGraphicsState(struct GfxBase* base);
// Release only this storage, before publication or after its users are removed.
void pcFreeGraphicsState(struct GfxBase* base);

// Initial RAM display list for a fresh, unpublished library. This supplies the
// blank View and empty sprite records consumed by the native display path; it
// does not install a Copper list in hardware or enable DMA. Roll back only
// after active View readers have been detached.
BOOL pcInitStartupDisplayLists(struct GfxBase* base);
void pcFreeStartupDisplayLists(struct GfxBase* base);

struct GraphicsChipRevision {
    ULONG reported;
    BOOL changed;
};

// Selection and base/list state only. The backend supplies discovered chip
// capabilities and memory class; this function does not probe hardware. A
// changed result requires the caller to refresh the display database before
// completing SetChipRev. It is not a replacement for that public entry.
struct GraphicsChipRevision pcApplyChipRevision(ULONG requested, UBYTE detected,
    UBYTE memory_type, struct GfxBase* base);
