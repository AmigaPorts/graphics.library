/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/rastport.h>

// Return the original full D0 Boolean, including when ReadPixel returned -1.
LONG pcFlood(struct RastPort* port asm("a1"), ULONG mode asm("d2"), LONG x asm("d0"), LONG y asm("d1"),
    struct GfxBase* graphics asm("a6"));
