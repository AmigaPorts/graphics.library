/* SPDX-License-Identifier: MPL-2.0 */
// Software-owned graphics startup state. Hardware discovery and display setup
// are separate responsibilities; no chipset identity or timing is assumed here.
#include "internal/state.h"
#include "internal/region.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/copper.h>
#include <graphics/display.h>
#define __NOLIBBASE__
#include <proto/exec.h>

// Public memory-type encodings select shifts, not a detected chipset. Keep
// these available before displayable bitmap allocation or database creation.
static const UBYTE bandwidth_shifts[] = {0, 1, 1, 2};

// Each resolution has four memory-bandwidth entries, indexed by
// (resolution << arraywidth) + bandwidth. A fetch takes at least eight
// clocks; wider memory words increase that unit at the lower resolutions.
static const UWORD start_fetch_masks[] = {
    -8, -16, -16, -32,
    -8, -8, -8, -16,
    -8, -8, -8, -8
};

static const UWORD stop_fetch_masks[] = {
    -8, -16, -16, -32,
    -8, -8, -8, -16,
    -8, -8, -8, -8
};

// The final word occupies 8 * 2^(bandwidth shift - resolution) clocks.
// Its stop phase is limited to the eight-clock bitplane scheduling period.
static const UWORD fetch_overrun[] = {
    -8, -16, -16, -32,
    -4, -8, -8, -16,
    -2, -4, -4, -8
};

static const WORD real_stops[] = {
    8, 8, 8, 8,
    4, 8, 8, 8,
    2, 4, 4, 8
};

static void initList(struct List* list)
{
    list->lh_Head = (struct Node*)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node*)&list->lh_Head;
}

static void displayRecord(UWORD* record, UWORD selector, UWORD value)
{
    record[0] = selector;
    record[1] = value;
}

struct GraphicsChipRevision pcApplyChipRevision(ULONG requested, UBYTE detected,
    UBYTE memory_type, struct GfxBase* base)
{
    if (requested == SETCHIPREV_BEST) {
        requested = base->WantChips;
    } else if ((base->ChipRevBits0 & SETCHIPREV_AA) == SETCHIPREV_AA) {
        // Once the original has enabled AA, explicit lower requests do not
        // downgrade it. BEST still follows the saved WantChips selection.
        return (struct GraphicsChipRevision){SETCHIPREV_AA, FALSE};
    }

    UBYTE selected = detected & (requested | GFXF_AA_MLISA);

    if (selected == base->ChipRevBits0) {
        return (struct GraphicsChipRevision){selected, FALSE};
    }

    base->ChipRevBits0 = selected;

    if (!(selected & GFXF_HR_AGNUS)) {
        base->ChipRevBits0 &= ~(GFXF_HR_DENISE | GFXF_AA_MLISA);
    }

    if (selected & GFXF_AA_LISA) {
        base->MemType = memory_type;
        base->ColorMask = 0xff000000;
    } else {
        base->ColorMask = 0xf0000000;
    }

    if (!(selected & GFXF_HR_DENISE)) {
        // These are the RAM display-list records already owned by graphics.
        // Preserve the other startup entries and all live display ownership.
        struct copinit* list = base->copinit;
        displayRecord(list->diagstrt + 10, 0x1fe, 0x0c00);
        displayRecord(list->fm0, 0x1fe, 0);

        for (unsigned word = 0; word < 10; word += 2) {
            displayRecord(list->diwstart + word, 0x1fe, 0);
        }

        displayRecord(list->bplcon2, 0x1fe, 0);
    }

    // The original reports the selection before dependent bits are cleared.
    return (struct GraphicsChipRevision){selected, TRUE};
}

BOOL pcInitStartupDisplayLists(struct GfxBase* base)
{
    struct ExecBase* SysBase = (APTR)base->ExecBase;
    struct copinit* list = AllocMem(sizeof(*list), MEMF_PUBLIC | MEMF_CHIP | MEMF_CLEAR);

    if (!list) {
        return FALSE;
    }

    // These are the original public RAM-list records, not MMIO accesses. The
    // original startup colour is retained in Exec's ex_Pad0 word. The native
    // renderer interprets list state without running a chipset Copper.
    displayRecord(list->vsync_hblank, 0x0e0, 0);
    displayRecord(list->diagstrt, 0x100, 0x200);
    displayRecord(list->diagstrt + 2, 0x106, 0x0c00);
    displayRecord(list->diagstrt + 4, 0x180, SysBase->ex_Pad0);
    displayRecord(list->diagstrt + 6, 0x1fe, 0x0e00);
    displayRecord(list->diagstrt + 8, 0x1fe, SysBase->ex_Pad0);
    displayRecord(list->diagstrt + 10, 0x1fe, 0x0c00);

    BOOL extended = (base->ChipRevBits0 & GFXF_HR_DENISE) != 0;
    displayRecord(list->fm0, extended ? 0x1fc : 0x1fe, 0);
    displayRecord(list->diwstart, extended ? 0x08e : 0x1fe, extended ? 0x0181 : 0);
    displayRecord(list->diwstart + 2, extended ? 0x090 : 0x1fe, extended ? 0x0281 : 0);
    displayRecord(list->diwstart + 4, extended ? 0x1e4 : 0x1fe, 0);
    displayRecord(list->diwstart + 6, extended ? 0x092 : 0x1fe, extended ? 0x0018 : 0);
    displayRecord(list->diwstart + 8, extended ? 0x094 : 0x1fe, extended ? 0x0020 : 0);
    displayRecord(list->bplcon2, extended ? 0x104 : 0x1fe, extended ? 0x0024 : 0);

    ULONG blank = (ULONG)list->sprstop;

    for (unsigned channel = 0; channel < 8; ++channel) {
        displayRecord(list->sprfix + 2 * channel, 0x140 + 8 * channel, 0);
        displayRecord(list->sprstrtup + 4 * channel, 0x120 + 4 * channel, blank >> 16);
        displayRecord(list->sprstrtup + 4 * channel + 2, 0x122 + 4 * channel, blank);
    }

    displayRecord(list->wait14, 0x0c01, 0xfffe);
    displayRecord(list->norm_hblank, 0x0c01, 0xfffe);
    displayRecord(list->jump, 0x08a, 0);
    displayRecord(list->wait_forever, 0x106, 0x0c00);
    displayRecord(list->wait_forever + 2, 0xffff, 0xfffe);

    base->copinit = list;
    base->LOFlist = list->wait_forever;
    base->SHFlist = list->wait_forever;
    base->TopLine = 0x0c00;

    return TRUE;
}

void pcFreeStartupDisplayLists(struct GfxBase* base)
{
    if (base->copinit) {
        struct ExecBase* SysBase = (APTR)base->ExecBase;
        FreeMem(base->copinit, sizeof(*base->copinit));
        base->copinit = NULL;
        base->LOFlist = NULL;
        base->SHFlist = NULL;
    }
}

void pcFreeGraphicsState(struct GfxBase* base)
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    if (base->Scratch) { pcFlushRegionPool(base); }
    if (base->MonitorListSemaphore) {
        FreeMem(base->MonitorListSemaphore, sizeof(*base->MonitorListSemaphore));
        base->MonitorListSemaphore = NULL;
    }
    if (base->LastChanceMemory) {
        FreeMem(base->LastChanceMemory, sizeof(*base->LastChanceMemory));
        base->LastChanceMemory = NULL;
    }
    if (base->LCMptr) {
        FreeMem(base->LCMptr, 4096);
        base->LCMptr = NULL;
    }
    if (base->ActiViewCprSemaphore) {
        FreeMem(base->ActiViewCprSemaphore, sizeof(*base->ActiViewCprSemaphore));
        base->ActiViewCprSemaphore = NULL;
    }
    if (base->HashTableSemaphore) {
        FreeMem(base->HashTableSemaphore, sizeof(*base->HashTableSemaphore));
        base->HashTableSemaphore = NULL;
    }
    if (base->hash_table) {
        FreeMem(base->hash_table, 8 * sizeof(*base->hash_table));
        base->hash_table = NULL;
    }
    if (base->SimpleSprites) {
        FreeMem(base->SimpleSprites, 8 * sizeof(*base->SimpleSprites));
        base->SimpleSprites = NULL;
    }
}

BOOL pcInitGraphicsState(struct GfxBase* base)
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    // Retain the original allocations and memory classes. In particular the
    // last-chance raster is Chip RAM obtained from Exec, not a low PC address.
    base->SimpleSprites = AllocMem(8 * sizeof(*base->SimpleSprites), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->SimpleSprites) { goto fail; }
    base->hash_table = AllocMem(8 * sizeof(*base->hash_table), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->hash_table) { goto fail; }
    base->HashTableSemaphore = AllocMem(sizeof(*base->HashTableSemaphore), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->HashTableSemaphore) { goto fail; }
    InitSemaphore(base->HashTableSemaphore);
    base->ActiViewCprSemaphore = AllocMem(sizeof(*base->ActiViewCprSemaphore), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->ActiViewCprSemaphore) { goto fail; }
    InitSemaphore(base->ActiViewCprSemaphore);
    base->LCMptr = AllocMem(4096, MEMF_PUBLIC | MEMF_CHIP);
    if (!base->LCMptr) { goto fail; }
    base->LastChanceMemory = AllocMem(sizeof(*base->LastChanceMemory), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->LastChanceMemory) { goto fail; }
    InitSemaphore(base->LastChanceMemory);
    base->MonitorListSemaphore = AllocMem(sizeof(*base->MonitorListSemaphore), MEMF_PUBLIC | MEMF_CLEAR);
    if (!base->MonitorListSemaphore) { goto fail; }
    InitSemaphore(base->MonitorListSemaphore);

    initList(&base->TextFonts);
    initList(&base->BlitWaitQ);
    initList(&base->TOF_WaitQ);
    initList(&base->MonitorList);
    base->BlitLock = -1;
    base->BlitNest = -1;
    base->bwshifts = (UBYTE*)bandwidth_shifts;
    base->StrtFetchMasks = (UWORD*)start_fetch_masks;
    base->StopFetchMasks = (UWORD*)stop_fetch_masks;
    base->Overrun = (UWORD*)fetch_overrun;
    base->RealStops = (WORD*)real_stops;
    base->NormalDisplayColumns = 640;
    base->system_bplcon0 = COLORON;

    // The original publishes this legacy line-duration value for both PAL
    // and NTSC. It does not describe the framebuffer's physical scan timing.
    base->MicrosPerLine = 0x3f9d;

    // Baseline software defaults precede any display-backend selection. Sprite
    // widths count 16-bit words; FreeSprite restores DefaultSpriteWidth.
    base->SpriteWidth = 1;
    base->DefaultSpriteWidth = 1;
    base->arraywidth = 2;
    base->WantChips = 0xff;
    base->ColorMask = 0xf0000000;

    return TRUE;
fail:
    pcFreeGraphicsState(base);
    return FALSE;
}
