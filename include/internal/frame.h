/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>

ULONG pcGraphicsFrame(struct GfxBase* base asm("a1"));
void pcWaitTOF(struct GfxBase* base asm("a6"));
