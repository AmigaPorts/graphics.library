/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

// Standalone diagnostic compilations retain the established framebuffer path.
// CMake supplies both values for the selectable production configurations.
#ifndef GRAPHICS_FRAMEBUFFER
#define GRAPHICS_FRAMEBUFFER 1
#endif
#ifndef GRAPHICS_CHIPSET
#define GRAPHICS_CHIPSET 0
#endif

#include <graphics/gfxbase.h>

#if GRAPHICS_CHIPSET
BOOL pcSelectGraphicsBackend(struct GfxBase* graphics);
BOOL pcUsesChipset(const struct GfxBase* graphics);
#else
static inline BOOL pcSelectGraphicsBackend(struct GfxBase* graphics)
{
    (void)graphics;
    return TRUE;
}

static inline BOOL pcUsesChipset(const struct GfxBase* graphics)
{
    (void)graphics;
    return FALSE;
}
#endif
