/* SPDX-License-Identifier: MPL-2.0 */
// Shared ownership protocol. Hardware submission and completion are separate
// from the task queue; this path neither starts nor waits for a transfer.
#include "internal/blitter.h"

#include <stddef.h>
#include <exec/execbase.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME base
#include <proto/exec.h>
#include <proto/graphics.h>

enum {
    BLITTER_QUEUE_OWNER = 1U << 1,
    BLITTER_PENDING_GRANT = 1U << 2,
    BLITTER_FINAL_TRANSFER = 1U << 7
};

// Capture the callback's condition codes before C can change them. D0 is not
// its continuation result: client assembly is allowed to return Z separately.
ULONG invokeBlitter(APTR function asm("a2"), APTR registers asm("a0"),
    struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"));

BOOL pcDispatchBlitter(APTR registers, struct GfxBase* base)
{
    struct bltnode* node = base->blthd;

    if (!node || !(base->Flags & BLITTER_QUEUE_OWNER) ||
        (base->Flags & BLITTER_FINAL_TRANSFER) || ((UWORD)node->beamsync & 0x8000)) {
        return FALSE;
    }

    if (invokeBlitter((APTR)node->function, registers, node, base)) {
        base->Flags |= BLITTER_FINAL_TRANSFER;
    }

    // A callback can start DMA or skip a stage. Even a final callback keeps
    // its node and ownership until the backend confirms that hardware is idle.
    return TRUE;
}

BOOL pcCompleteBlitter(APTR registers, struct GfxBase* base)
{
    struct bltnode* node = base->blthd;

    if (!node || !(base->Flags & BLITTER_QUEUE_OWNER)) {
        return FALSE;
    }

    if (!(base->Flags & BLITTER_FINAL_TRANSFER)) {
        return TRUE;
    }

    base->Flags &= ~BLITTER_FINAL_TRANSFER;

    if (node->stat & CLEANUP) {
        invokeBlitter((APTR)node->cleanup, registers, node, base);
        node = base->blthd;
    }

    // Cleanup may append another request. Read the link only after it returns.
    base->blthd = node->n;

    if (base->blthd && base->BlitLock == 0) {
        return TRUE;
    }

    // Give a waiting task its turn between nodes. Hide the remaining queue
    // during the public release call so it cannot immediately reclaim the
    // grant, then retain its single reference for the next task release.
    BOOL queued = base->blthd != NULL;
    base->blthd = NULL;
    base->Flags &= ~BLITTER_QUEUE_OWNER;
    DisownBlitter();

    if (queued) {
        base->blthd = node->n;
        base->BlitLock = (UWORD)base->BlitLock + 1;
    }

    return FALSE;
}

static void __attribute__((used)) ownBlitter(struct GfxBase* base asm("a6"))
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    struct Node waiter;
    waiter.ln_Name = (char*)SysBase->ThisTask;

    SetSignal(0, SIGF_BLIT);
    Disable();

    // A release can precede our insertion. Its grant remains in Flags until
    // consumed here, so clearing a stale signal cannot lose that wake-up.
    if (base->Flags & BLITTER_PENDING_GRANT) {
        base->Flags &= ~BLITTER_PENDING_GRANT;
        Enable();
        return;
    }

    AddTail(&base->BlitWaitQ, &waiter);
    Enable();
    Wait(SIGF_BLIT);
}

static void __attribute__((used)) disownBlitter(struct GfxBase* base asm("a6"))
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    Disable();

    if (base->blthd) {
        // CPU drawing has completed before returning to its caller. Resume
        // the queue through the library's interrupt server; a task must not
        // receive the grant still held by that queued request.
        base->Flags |= BLITTER_QUEUE_OWNER;
        Cause(&base->bltsrv);
    } else {
        struct Node* waiter = RemHead(&base->BlitWaitQ);

        if (waiter) {
            Signal((struct Task*)waiter->ln_Name, SIGF_BLIT);
        } else {
            // OwnBlitter has incremented the counter but has not yet linked
            // its stack node. Leave the grant for its interrupt-safe check.
            base->Flags |= BLITTER_PENDING_GRANT;
        }
    }

    Enable();
}

static void queueBlit(struct bltnode* node, struct GfxBase* base)
{
    struct ExecBase* SysBase = (struct ExecBase*)base->ExecBase;
    BOOL acquired = FALSE;
    node->n = NULL;

    Disable();

    if (base->blthd) {
        base->blttl->n = node;
    } else {
        base->blthd = node;
        base->BlitLock = (UWORD)base->BlitLock + 1;
        acquired = base->BlitLock == 0;

        if (acquired) {
            base->Flags |= BLITTER_QUEUE_OWNER;
        }
    }

    base->blttl = node;

    if (acquired) {
        Cause(&base->bltsrv);
    }

    Enable();
}

void pcQueueBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"))
{
    node->beamsync &= 0x7fff;
    queueBlit(node, base);
}

void pcQueueBSBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"))
{
    // Both calls use the same FIFO. The high bit requests synchronization;
    // the remaining value is clamped using the original unsigned comparison.
    UWORD line = node->beamsync;

    if (line > base->MaxDisplayRow) {
        line = base->MaxDisplayRow;
    }

    node->beamsync = line | 0x8000;
    queueBlit(node, base);
}

// The public GfxBase layout supplies the assembly counter displacement.
_Static_assert(offsetof(struct GfxBase, BlitLock) == 170, "BlitLock assembly offset");

// Ownership and queued requests share an atomically updated signed counter.
// Uncontended calls need no scratch registers or C stack frame. Preserve all
// registers around the slow paths, as the original ownership entries do.
__asm__(
    ".text\n"
    "_invokeBlitter:\n"
    "movem.l d2-d7/a2-a6,-(sp)\n"
    "move.l a2,a5\n"
    "jsr (a5)\n"
    "seq d0\n"
    "and.l #255,d0\n"
    "movem.l (sp)+,d2-d7/a2-a6\n"
    "rts\n"
    ".globl _pcOwnBlitter\n"
    "_pcOwnBlitter:\n"
    "addq.w #1,170(a6)\n"
    "beq.s 1f\n"
    "movem.l d0-d1/a0-a1,-(sp)\n"
    "jsr _ownBlitter\n"
    "movem.l (sp)+,d0-d1/a0-a1\n"
    "1:\n"
    "rts\n"
    ".globl _pcDisownBlitter\n"
    "_pcDisownBlitter:\n"
    "subq.w #1,170(a6)\n"
    "blt.s 2f\n"
    "movem.l d0-d1/a0-a1,-(sp)\n"
    "jsr _disownBlitter\n"
    "movem.l (sp)+,d0-d1/a0-a1\n"
    "2:\n"
    "rts\n"
);
