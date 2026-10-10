/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/node.h"

#include <exec/alerts.h>
#include <exec/execbase.h>
#include <graphics/gfxnodes.h>
#include <graphics/monitor.h>
#include <graphics/view.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

static unsigned nodeHash(CONST_APTR associate)
{
    ULONG address = (ULONG)associate;
    return (address + (address >> 8) + (address >> 16) + (address >> 24)) & 7;
}

static APTR* nodeAssociation(struct ExtendedNode* node)
{
    // ViewExtra.View and ViewPortExtra.ViewPort immediately follow the common
    // ExtendedNode header. Association and lookup use this shared layout.
    return (APTR*)(node + 1);
}

void pcGfxAssociate(APTR associate asm("a0"), APTR node asm("a1"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ExtendedNode* extra = node;
    struct ExtendedNode** table = (APTR)graphics->hash_table;
    unsigned bucket = nodeHash(associate);
    *nodeAssociation(extra) = associate;
    ObtainSemaphore(graphics->HashTableSemaphore);
    struct ExtendedNode* head = table[bucket];
    if (head) {
        extra->xln_Succ = (APTR)head;
        head->xln_Pred = node;
    }
    // The original API relies on GfxNew's cleared links. It does not reset
    // the predecessor, or the successor when adding to an empty bucket.
    table[bucket] = extra;
    ReleaseSemaphore(graphics->HashTableSemaphore);
}

APTR pcGfxLookUp(CONST_APTR associate asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ExtendedNode** table = (APTR)graphics->hash_table;
    unsigned bucket = nodeHash(associate);
    ObtainSemaphore(graphics->HashTableSemaphore);
    struct ExtendedNode* node = table[bucket];
    while (node && *nodeAssociation(node) != associate) { node = (APTR)node->xln_Succ; }
    ReleaseSemaphore(graphics->HashTableSemaphore);
    return node;
}

static void removeAssociation(struct GfxBase* graphics, struct ExtendedNode* node)
{
    // Preserve the current public vector owner, including a lookup that
    // selects another associated node. Freeing still applies to the caller's
    // node, as in the original API.
    struct ExtendedNode* associated = GfxLookUp(*nodeAssociation(node));
    if (!associated) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->HashTableSemaphore);
    struct ExtendedNode* previous = (APTR)associated->xln_Pred;
    struct ExtendedNode* next = (APTR)associated->xln_Succ;
    if (previous) { previous->xln_Succ = (APTR)next; }
    else {
        struct ExtendedNode** table = (APTR)graphics->hash_table;
        table[nodeHash(*nodeAssociation(associated))] = next;
    }
    if (next) { next->xln_Pred = (APTR)previous; }
    ReleaseSemaphore(graphics->HashTableSemaphore);
}

void pcGfxFree(APTR node asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ExtendedNode* extra = node;
    ULONG size = 0;
    if (extra->xln_Type == NT_GRAPHICS && extra->xln_Subsystem == SS_GRAPHICS) {
        switch (extra->xln_Subtype) {
            case VIEW_EXTRA_TYPE: size = sizeof(struct ViewExtra); break;
            case VIEWPORT_EXTRA_TYPE: size = sizeof(struct ViewPortExtra); break;
            case SPECIAL_MONITOR_TYPE: size = sizeof(struct SpecialMonitor); break;
            case MONITOR_SPEC_TYPE: size = sizeof(struct MonitorSpec); break;
        }
    }
    if (!size) { Alert(AN_GfxFreeError); return; }
    if (extra->xln_Subtype == VIEW_EXTRA_TYPE || extra->xln_Subtype == VIEWPORT_EXTRA_TYPE) {
        removeAssociation(graphics, extra);
    }
    FreeMem(node, size);
}
