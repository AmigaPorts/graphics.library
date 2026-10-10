/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>

void pcGfxAssociate(APTR associate asm("a0"), APTR node asm("a1"), struct GfxBase* graphics asm("a6"));
APTR pcGfxLookUp(CONST_APTR associate asm("a0"), struct GfxBase* graphics asm("a6"));
void pcGfxFree(APTR node asm("a0"), struct GfxBase* graphics asm("a6"));
