/* SPDX-License-Identifier: MPL-2.0 */
// Software-owned graphics startup state. Hardware discovery and display setup
// are separate responsibilities; no chipset identity or timing is assumed here.
#include "internal/state.h"
#include "internal/region.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#include <proto/exec.h>

static void initList(struct List* list)
{
    list->lh_Head = (struct Node*)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node*)&list->lh_Head;
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
    return TRUE;
fail:
    pcFreeGraphicsState(base);
    return FALSE;
}
