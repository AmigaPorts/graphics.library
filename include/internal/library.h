/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/execbase.h>
#include <exec/resident.h>
#include <graphics/gfxbase.h>

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
BOOL pcGraphicsVectorsComplete(void);
