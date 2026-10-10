/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "internal/view.h"
#include "internal/display.h"

#include <exec/execbase.h>
#include <exec/resident.h>
#include <graphics/gfxbase.h>

// The public base remains the original ABI prefix. Native display ownership
// belongs to this library instance, outside the public GfxBase layout.
struct GraphicsLibrary {
    struct GfxBase graphics;
    struct PreparedViewPorts prepared;
    struct MergedViews merged;
    struct ActiveViewFields active;
    struct NativeDisplayDatabase display;
    struct NativeGraphicsDisplay* native;
    struct MonitorSpec* initial_monitor;
    UBYTE initial_frequency;
    struct TextFont* initial_fonts[2];
};

static inline struct PreparedViewPorts* pcPreparedViewPorts(struct GfxBase* graphics)
{
    return &((struct GraphicsLibrary*)graphics)->prepared;
}

static inline struct MergedViews* pcMergedViews(struct GfxBase* graphics)
{
    return &((struct GraphicsLibrary*)graphics)->merged;
}

static inline struct ActiveViewFields* pcActiveViewFields(struct GfxBase* graphics)
{
    return &((struct GraphicsLibrary*)graphics)->active;
}

extern const char deviceName[];
extern const char deviceIdString[];
extern const struct Resident RomTag;
// Includes the four lifecycle vectors; the last SDK vector is -1056.
#define GRAPHICS_VECTOR_COUNT 176
extern const APTR graphicsFunctions[GRAPHICS_VECTOR_COUNT + 1];

struct GfxBase* Init(ULONG segment asm("a0"), struct ExecBase* SysBase asm("a6"));
struct GfxBase* pcGraphicsOpen(struct GfxBase* base asm("a6"));
ULONG pcGraphicsClose(void);
ULONG pcGraphicsExpunge(struct GfxBase* base asm("a6"));
ULONG pcGraphicsReserved(void);
ULONG pcGfxInternal3(void);
ULONG pcGfxSpare1(void);
ULONG pcGfxSpare2(void);
BOOL pcGraphicsVectorsComplete(void);
