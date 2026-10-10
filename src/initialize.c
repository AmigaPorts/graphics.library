/* SPDX-License-Identifier: MPL-2.0 */
// Independent implementations of the Amiga graphics.library initialization
// interfaces. These initialize caller-owned structures, without allocation,
// hardware access or library publication.
#include "internal/initialize.h"

void pcInitBitMap(struct BitMap* bitmap asm("a0"), BYTE depth asm("d0"),
    WORD width asm("d1"), WORD height asm("d2"))
{
    // Preserve the original WORD rounding. Planes[] belongs to the caller;
    // even a header followed by fewer than eight planes is a valid layout.
    LONG rounded = (WORD)((UWORD)width + 15);
    bitmap->BytesPerRow = (UWORD)((rounded >> 4) * 2);
    bitmap->Rows = height;
    bitmap->Flags = 0;
    bitmap->Depth = depth;
    bitmap->pad = 0;
}

void pcInitArea(struct AreaInfo* area asm("a0"), APTR buffer asm("a1"), WORD capacity asm("d0"))
{
    area->VctrTbl = buffer;
    area->VctrPtr = buffer;
    area->FlagTbl = (BYTE*)buffer + (ULONG)(UWORD)capacity * 2 * sizeof(WORD);
    area->FlagPtr = area->FlagTbl;
    area->Count = 0;
    area->MaxCount = capacity;
    // FirstX/FirstY are established by AreaMove, not by InitArea. The caller's
    // vector and flag storage is likewise not cleared here.
}

struct TmpRas* pcInitTmpRas(struct TmpRas* raster asm("a0"), PLANEPTR buffer asm("a1"), LONG size asm("d0"))
{
    raster->RasPtr = (BYTE*)buffer;
    raster->Size = size;
	
    return raster;
}

void pcInitVPort(struct ViewPort* viewport asm("a0"))
{
    UBYTE* data = (UBYTE*)viewport;
    
	for (ULONG i = 0; i < sizeof(*viewport); ++i) {
		data[i] = 0;
	}
	
    viewport->SpritePriorities = 0x24;
}

void pcInitView(struct View* view asm("a1"))
{
    UBYTE* data = (UBYTE*)view;
	
    for (ULONG i = 0; i < sizeof(*view); ++i) {
	    data[i] = 0;
    }
    
	// Original default positioning in the public View coordinate system.
    view->DyOffset = 44;
    view->DxOffset = 129;
}
