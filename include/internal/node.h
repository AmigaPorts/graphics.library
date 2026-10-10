/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/monitor.h>

// Shared GfxNew allocation and software defaults. Backend methods must be
// complete before creating monitor nodes; allocation does not initialize or
// publish them. The returned node keeps method addresses, not this descriptor.
// Keep this separate from the public vector until native activation is wired.
struct GraphicsNodeMethods {
    LONG (*init_monitor)(struct ExtendedNode* node asm("a0"), UWORD flags asm("d0"),
        struct GfxBase* graphics asm("a6"));
    LONG (*program_monitor)(struct MonitorSpec* monitor);
    LONG (*merge_view)(struct View* view, struct GfxBase* graphics asm("a6"));
    LONG (*load_view)(struct View* view, struct GfxBase* graphics asm("a6"));
};

APTR pcAllocGraphicsNode(ULONG type, const struct GraphicsNodeMethods* methods, struct GfxBase* graphics);

APTR pcGfxNew(ULONG type asm("d0"), struct GfxBase* graphics asm("a6"));

void pcGfxAssociate(APTR associate asm("a0"), APTR node asm("a1"), struct GfxBase* graphics asm("a6"));
APTR pcGfxLookUp(CONST_APTR associate asm("a0"), struct GfxBase* graphics asm("a6"));
void pcGfxFree(APTR node asm("a0"), struct GfxBase* graphics asm("a6"));
