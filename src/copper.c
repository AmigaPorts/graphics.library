/* SPDX-License-Identifier: MPL-2.0 */
// The original allocator retains the legacy SDK fields, including SLRepeat and
// Flags after the four old frame pointers. Keep that allocation layout locally.
#define V1_3
#include "internal/copper.h"
#include "internal/display.h"
#include "internal/monitor.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

_Static_assert(sizeof(struct CopList) == COPLIST_STORAGE_SIZE, "legacy CopList allocation");

static struct CopList* initCopList(struct CopList* list, LONG count)
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    BOOL allocated = FALSE;
    if (!list) {
        list = AllocMem(sizeof(*list), MEMF_PUBLIC | MEMF_CLEAR);
        if (!list) { return NULL; }
        allocated = TRUE;
    }
    if (!list->CopIns || list->MaxCount < count) {
        if (list->CopIns) {
            FreeMem(list->CopIns, (ULONG)(LONG)list->MaxCount * sizeof(struct CopIns));
            list->CopIns = NULL;
        }
        list->Count = 0;
        struct CopIns* instructions = AllocMem((ULONG)count * sizeof(*instructions), MEMF_PUBLIC);
        if (!instructions) {
            if (allocated) {
                FreeMem(list, sizeof(*list));
                return NULL;
            }
            list->CopPtr = list->CopIns;
            list->MaxCount = 0;
            list->CopIns = NULL;
            list->CopLStart = NULL;
            list->CopSStart = NULL;
            return list;
        }
        list->CopIns = instructions;
        list->MaxCount = count;
    }
    list->CopPtr = list->CopIns;
    list->Count = 0;
    list->SLRepeat = 1;
    list->Flags = 0;
    return list;
}

static struct CopList* __attribute__((used, noinline)) initUserList(struct UCopList* list asm("a0"),
    LONG count asm("d0"))
{
    if (!list) { return NULL; }
    struct CopList* first = initCopList(list->FirstCopList, count);
    if (first) { list->FirstCopList = first; list->CopList = first; }
    return first;
}

// The SDK declares a WORD count, but the original allocation calculation uses
// all of D0. Keep the public declaration and preserve the incoming register.
__asm__(".globl _pcUCopperListInit\n.set _pcUCopperListInit,_initUserList\n");

static LONG writeInstruction(struct UCopList* list, UWORD opcode, UWORD first, UWORD second)
{
    if (!list) { return 0; }
    struct CopList* current = list->CopList;
    if (!current) { current = initUserList(list, 16); }
    if (!current || !current->MaxCount || current->Count > current->MaxCount) { return 0; }
    struct CopIns* instruction = current->CopPtr;
    instruction->OpCode = opcode;
    instruction->DESTADDR = first;
    instruction->DESTDATA = second;
    return 1;
}

LONG pcCMove(struct UCopList* list asm("a1"), APTR destination asm("d0"), WORD data asm("d1"))
{
    return writeInstruction(list, COPPER_MOVE, (ULONG)destination, data);
}

LONG pcCWait(struct UCopList* list asm("a1"), WORD vertical asm("d0"), WORD horizontal asm("d1"))
{
    return writeInstruction(list, COPPER_WAIT, vertical, horizontal);
}

void pcCBump(struct UCopList* list asm("a1"))
{
    if (!list) { return; }
    struct CopList* current = list->CopList;
    struct CopIns* following = current->CopPtr + 1;
    current->Count = (UWORD)current->Count + 1;
    if (current->Count >= current->MaxCount) {
        struct ExecBase* SysBase = *(struct ExecBase**)4;
        struct CopList* next = current->Next;
        if (!next) {
            next = AllocMem(sizeof(*next), MEMF_PUBLIC);
            current->Next = next;
            if (!next) {
                current->Count = (UWORD)current->Count - 1;
                current->CopPtr = following;
                return;
            }
            struct CopIns* instructions = AllocMem(16 * sizeof(*instructions), MEMF_PUBLIC);
            if (!instructions) {
                FreeMem(next, sizeof(*next));
                current->Next = NULL;
                current->Count = (UWORD)current->Count - 1;
                current->CopPtr = following;
                return;
            }
            next->CopIns = instructions;
            next->MaxCount = 16;
            next->Next = NULL;
        }
        list->CopList = next;
        next->CopIns->OpCode = current->CopPtr->OpCode;
        next->CopIns->NXTLIST = current->CopPtr->NXTLIST;
        current->CopPtr->OpCode = CPRNXTBUF;
        current->CopPtr->NXTLIST = next;
        next->Count = 1;
        following = next->CopIns + 1;
    }
    list->CopList->CopPtr = following;
}

void pcFreeCopList(struct CopList* list asm("a0"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    while (list) {
        FreeMem(list->CopIns, (ULONG)(LONG)list->MaxCount * sizeof(struct CopIns));
        struct CopList* next = list->Next;
        FreeMem(list, COPLIST_STORAGE_SIZE);
        list = next;
    }
}

void pcFreeCprList(struct cprlist* list asm("a0"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    while (list) {
        FreeMem(list->start, (ULONG)(LONG)list->MaxCount * sizeof(ULONG));
        struct cprlist* next = list->Next;
        FreeMem(list, sizeof(*list));
        list = next;
    }
}

void pcFreeVPortCopLists(struct ViewPort* viewport asm("a0"))
{
    if (!viewport) { return; }
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    pcFreeCopList(viewport->DspIns);
    pcFreeCopList(viewport->ClrIns);
    pcFreeCopList(viewport->SprIns);
    struct UCopList* list = viewport->UCopIns;
    while (list) {
        pcFreeCopList(list->FirstCopList);
        struct UCopList* next = list->Next;
        FreeMem(list, sizeof(*list));
        list = next;
    }
    viewport->DspIns = NULL;
    viewport->ClrIns = NULL;
    viewport->SprIns = NULL;
    viewport->UCopIns = NULL;
}

ULONG pcCalcIVG(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    (void)view;
    ULONG display_id = GetVPModeID(viewport);

    if (display_id == (ULONG)INVALID_ID) {
        display_id = pcViewportModes(viewport);

        if (!(display_id & EXTENDED_MODE)) {
            display_id &= 0xffff9efdUL;
        } else if ((display_id & MONITOR_ID_MASK) == EXTENDED_MODE) {
            display_id &= ~EXTENDED_MODE;
        }
    }

    const struct ColorMap* map = viewport->ColorMap;

    if (map && map->Type) {
        const struct GraphicsDisplayDataRecord* record = map->CoerceDisplayInfo;

        if (!record) {
            record = map->NormalDisplayInfo;
        }

        if (record) {
            display_id = record->record.id;
        }

        if (map->cm_vpe && (WORD)pcViewportFetchMode(viewport, graphics) < 0) {
            return 0;
        }
    }

    // The original reuses one query buffer, asking for only the display prefix
    // after reading the monitor timing. Query return counts do not gate this
    // calculation; keep the public vector calls and their lengths unchanged.
    union {
        struct MonitorInfo monitor;
        struct DisplayInfo display;
    } info;
    GetDisplayInfoData(NULL, (APTR)&info, sizeof(info.monitor), DTAG_MNTR, display_id);
    UWORD clocks = (WORD)info.monitor.TotalColorClocks >> 1;
    GetDisplayInfoData(NULL, (APTR)&info, 28, DTAG_DISP, display_id);

    struct CopList* list = viewport->DspIns;

    if (!list) {
        return 0;
    }

    UWORD count = 0;

    for (;;) {
        count += list->Count;

        if (!list->Next) {
            break;
        }

        list = list->Next;
    }

    count -= list->SLRepeat;
    ULONG lines = (ULONG)count * 2;

    // DIVU.W leaves quotient/remainder packed together, and leaves the dividend
    // unchanged on quotient overflow. The original signed comparison below
    // determines rounding and EXACT_LINE from that complete result.
    __asm__ volatile("divu.w %1,%0" : "+d"(lines) : "d"(clocks) : "cc");

    if ((LONG)lines > 0xffff) {
        lines = (lines & 0xffff0000UL) | (UWORD)(lines + 1);
    } else {
        list->Flags |= EXACT_LINE;
    }

    if (!(graphics->ChipRevBits0 & GFXF_AA_ALICE) && (WORD)lines < 2) {
        lines = 2;
    }

    if (info.display.PropertyFlags & DIPF_IS_LACE) {
        return (UWORD)(lines * 2);
    }

    if (info.display.PropertyFlags & DIPF_IS_SCANDBL) {
        list->Flags |= ((UWORD)lines & 1) * HALF_LINE;
        lines = (WORD)(lines + 1) >> 1;
    }

    return (UWORD)lines;
}
