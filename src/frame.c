/* SPDX-License-Identifier: MPL-2.0 */
// Native OS frame completion; no beam, Copper, blitter or CIA register model.
#include "internal/frame.h"
#include "internal/monitor.h"

#include <stddef.h>
#include <exec/execbase.h>
#include <graphics/view.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME base
#include <proto/exec.h>
#include <proto/graphics.h>

void pcWaitBOVP(struct ViewPort* viewport asm("a0"), struct GfxBase* base asm("a6"))
{
    UWORD height = viewport->DHeight;
    struct View* view = base->ActiView;

    if (!view) {
        return;
    }

    ULONG modes = pcViewportModes(viewport);

    if (modes & DOUBLESCAN) {
        height *= 2;
    }

    BOOL interlaced = (modes & LACE) != 0;
    struct MonitorSpec* monitor = base->natural_monitor;

    if (view->Modes & EXTENDED_MODE) {
        const struct ViewExtra* extra = GfxLookUp(view);

        if (extra && extra->Monitor) {
            monitor = extra->Monitor;
        }
    }

    WORD last_row = monitor->total_rows - 1;
    WORD origin = monitor->ms_Flags & MSF_REQUEST_A2024 ? 44 : 0;

    for (;;) {
        // Height, mode and monitor selection are captured once. Offsets can
        // change while waiting; retain the original WORD wrap before shifting
        // and after adding the active View's origin.
        WORD bottom = (UWORD)viewport->DyOffset + height;

        if (interlaced) {
            bottom >>= 1;
        }

        bottom = (UWORD)bottom + (origin ? origin : view->DyOffset);

        if (bottom > last_row) {
            bottom = last_row;
        }

        if (bottom <= (WORD)VBeamPos()) {
            return;
        }
    }
}

// DBList links either half of a DBufInfo by its Link/Count/Message prefix.
// The alias attribute permits access to both public halves through this view.
struct FrameReply {
    APTR next;
    ULONG frames;
    struct Message message;
} __attribute__((may_alias));
_Static_assert(offsetof(struct FrameReply, frames) == offsetof(struct DBufInfo, dbi_Count1), "DBufInfo count");
_Static_assert(offsetof(struct FrameReply, message) == offsetof(struct DBufInfo, dbi_SafeMessage), "DBufInfo message");
_Static_assert(offsetof(struct FrameReply, frames) ==
    offsetof(struct DBufInfo, dbi_Count2) - offsetof(struct DBufInfo, dbi_Link2), "DBufInfo second count");
_Static_assert(offsetof(struct FrameReply, message) ==
    offsetof(struct DBufInfo, dbi_DispMessage) - offsetof(struct DBufInfo, dbi_Link2), "DBufInfo second message");

ULONG pcGraphicsFrame(struct GfxBase* base asm("a1"))
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    ++base->VBCounter;
    base->VBlank = 0;
    APTR* link = &base->DBList;
    struct FrameReply* reply;
    while ((reply = *link)) {
        if (--reply->frames == 0) {
            ReplyMsg(&reply->message);
            *link = reply->next;
        } else {
            link = &reply->next;
        }
    }
    for (struct Node* node = base->TOF_WaitQ.lh_Head; node->ln_Succ; node = node->ln_Succ) {
        Signal((struct Task*)node->ln_Name, 1UL << ((UBYTE)node->ln_Pri & 31));
    }
    // Other VERTB servers, including timer.device's completion, must still run.
    return 0;
}

void pcWaitTOF(struct GfxBase* base asm("a6"))
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    struct Node waiter;
    waiter.ln_Type = NT_UNKNOWN;
    waiter.ln_Pri = SIGB_SINGLE;
    waiter.ln_Name = (char*)SysBase->ThisTask;
    SetSignal(0, SIGF_SINGLE);
    Disable();
    AddTail(&base->TOF_WaitQ, &waiter);
    // Exec Wait releases Disable/Forbid while asleep, restoring the nesting
    // when this task resumes. Keep insertion and removal interrupt-safe.
    Wait(SIGF_SINGLE);
    Remove(&waiter);
    Enable();
}
