/* SPDX-License-Identifier: MPL-2.0 */
// Native OS frame completion; no beam, Copper, blitter or CIA register model.
#include "internal/frame.h"

#include <stddef.h>
#include <exec/execbase.h>
#include <graphics/view.h>
#define __NOLIBBASE__
#include <proto/exec.h>

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
