/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/view.h>

void pcInitBitMap(struct BitMap* bitmap asm("a0"), BYTE depth asm("d0"),
    WORD width asm("d1"), WORD height asm("d2"));
void pcInitArea(struct AreaInfo* area asm("a0"), APTR buffer asm("a1"), WORD capacity asm("d0"));
struct TmpRas* pcInitTmpRas(struct TmpRas* raster asm("a0"), PLANEPTR buffer asm("a1"), LONG size asm("d0"));
void pcInitVPort(struct ViewPort* viewport asm("a0"));
void pcInitView(struct View* view asm("a1"));
