/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/memory.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/utility.h>

struct DBufInfo* pcAllocDBufInfo(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"))
{
    (void)viewport;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    return AllocMem(sizeof(struct DBufInfo), MEMF_PUBLIC | MEMF_CLEAR);
}

void pcFreeDBufInfo(struct DBufInfo* info asm("a1"), struct GfxBase* graphics asm("a6"))
{
    if (!info) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    FreeMem(info, sizeof(*info));
}

static void freePlanes(struct GfxBase* graphics, struct BitMap* bitmap)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    unsigned count = bitmap->pad == 0x805c ? 1 : bitmap->Depth;
    ULONG size = (ULONG)bitmap->BytesPerRow * bitmap->Rows;
    PLANEPTR* planes = bitmap->Planes;
    for (unsigned i = 0; i < count; ++i) { FreeMem(planes[i], size); }
}

struct BitMap* pcAllocBitMap(ULONG width asm("d0"), ULONG height asm("d1"), ULONG depth asm("d2"),
    ULONG flags asm("d3"), const struct BitMap* friend_bitmap asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (BITMAPFLAGS_ARE_EXTENDED(flags)) {
        struct Library* UtilityBase = graphics->UtilBase;
        const struct TagItem* tags = (const void*)friend_bitmap;
        depth = GetTagData(BMATags_Depth, depth, tags);
        ULONG clear = GetTagData(BMATags_Clear, flags & BMF_CLEAR ? 255 : 0, tags);
        ULONG displayable = GetTagData(BMATags_Displayable, flags & BMF_DISPLAYABLE ? 255 : 0, tags);
        friend_bitmap = (APTR)GetTagData(BMATags_Friend, 0, tags);
        flags = (flags & 255 & ~(BMF_CLEAR | BMF_DISPLAYABLE)) |
            (clear ? BMF_CLEAR : 0) | (displayable ? BMF_DISPLAYABLE : 0);
    }
    if (flags >= 256) { return NULL; }
    if (friend_bitmap && friend_bitmap->Depth == (UBYTE)depth && friend_bitmap->pad == 0x805c) {
        flags |= BMF_INTERLEAVED;
    }

    // MINPLANES permits a header with fewer than eight plane pointers. Larger
    // depths have trailing pointers beyond the SDK's fixed-size declaration.
    UWORD count = depth;
    UWORD header_size = 8 + 4 * ((flags & BMF_MINPLANES) || count > 8 ? count : 8);
    struct BitMap* bitmap = AllocVec(header_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!bitmap) { return NULL; }
    UWORD alignment = 16;
    if (flags & BMF_DISPLAYABLE) { alignment <<= graphics->bwshifts[graphics->MemType]; }
    UWORD stride = ((UWORD)(width + alignment - 1) & (UWORD)-alignment) >> 3;
    ULONG memory_flags = MEMF_PUBLIC | MEMF_CHIP | (flags & BMF_CLEAR ? MEMF_CLEAR : 0);
    bitmap->Rows = height;
    PLANEPTR* planes = bitmap->Planes;
    if ((WORD)count >= 2 && (graphics->ChipRevBits0 & GFXF_BIG_BLITS) &&
        stride <= 4093 && (WORD)height <= 4093 && (flags & BMF_INTERLEAVED)) {
        bitmap->BytesPerRow = stride * count;
        PLANEPTR memory = AllocMem((ULONG)bitmap->BytesPerRow * bitmap->Rows, memory_flags);
        if (memory) {
            bitmap->Depth = count;
            bitmap->pad = 0x805c;
            for (unsigned i = 0; i < count; ++i) { planes[i] = memory + i * stride; }
            return bitmap;
        }
        // Interleaving is a preference. Failure must still try separate planes.
    }
    bitmap->BytesPerRow = stride;
    ULONG plane_size = (ULONG)stride * bitmap->Rows;
    for (unsigned i = 0; i < count; ++i) {
        planes[i] = AllocMem(plane_size, memory_flags);
        if (!planes[i]) {
            freePlanes(graphics, bitmap);
            FreeVec(bitmap);
            return NULL;
        }
        ++bitmap->Depth;
    }
    return bitmap;
}

void pcFreeBitMap(struct BitMap* bitmap asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!bitmap) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    freePlanes(graphics, bitmap);
    FreeVec(bitmap);
}

ULONG pcGetBitMapAttr(const struct BitMap* bitmap asm("a0"), ULONG attribute asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    if (attribute < BMA_DEPTH) { return bitmap->Rows; }
    if (attribute < BMA_WIDTH) { return bitmap->Depth; }
    ULONG flags = BMF_STANDARD;
    ULONG stride = bitmap->BytesPerRow;
    if (bitmap->pad == 0x805c && (BYTE)bitmap->Depth >= 2) {
        ULONG distance = (ULONG)bitmap->Planes[1] - (ULONG)bitmap->Planes[0];
        if (distance && distance <= (ULONG)(LONG)(WORD)bitmap->BytesPerRow) {
            flags |= BMF_INTERLEAVED;
            stride = distance;
        }
    }
    if (attribute < BMA_FLAGS) { return (UWORD)(stride * 8); }
    if (attribute >= BMA_FLAGS + 4) { return 0; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    UWORD mask = (2U << graphics->bwshifts[graphics->MemType]) - 1;
    if (bitmap->BytesPerRow & mask) { return flags; }
    const PLANEPTR* planes = bitmap->Planes;
    for (unsigned i = 0; i < bitmap->Depth; ++i) {
        ULONG address = (ULONG)planes[i];
        if (address >= SysBase->MaxLocMem || (address & mask)) { return flags; }
    }
    return flags | BMF_DISPLAYABLE;
}

static ULONG rasterSize(UWORD width, UWORD height)
{
    // Preserve the native API's word arithmetic, also used by InitBitMap.
    UWORD stride = (WORD)(UWORD)(width + 15) >> 3;
    return (ULONG)(stride & 0xfffe) * height;
}

PLANEPTR pcAllocRaster(UWORD width asm("d0"), UWORD height asm("d1"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    return AllocMem(rasterSize(width, height), MEMF_PUBLIC | MEMF_CHIP);
}

void pcFreeRaster(PLANEPTR memory asm("a0"), UWORD width asm("d0"), UWORD height asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    FreeMem(memory, rasterSize(width, height));
}

APTR pcAllocScratch(struct GfxBase* graphics, ULONG size, ULONG flags)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (size <= 4096 && AttemptSemaphore(graphics->LastChanceMemory)) { return graphics->LCMptr; }
    APTR memory = AllocMem(size, flags);
    if (memory || size > 4096) { return memory; }
    ObtainSemaphore(graphics->LastChanceMemory);
    return graphics->LCMptr;
}

void pcFreeScratch(struct GfxBase* graphics, APTR memory, ULONG size)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (memory == graphics->LCMptr) { ReleaseSemaphore(graphics->LastChanceMemory); }
    else if (memory) { FreeMem(memory, size); }
}
