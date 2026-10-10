/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>

ULONG pcGraphicsFrame(struct GfxBase* base asm("a1"));
void pcWaitTOF(struct GfxBase* base asm("a6"));

// Use the installed VBeamPos owner. This does not synthesize scan position
// from native presentation cadence; a display backend must supply that vector.
void pcWaitBOVP(struct ViewPort* viewport asm("a0"), struct GfxBase* base asm("a6"));
