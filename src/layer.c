/* SPDX-License-Identifier: MPL-2.0 */
// Public graphics layer locks use the layer's ordinary Exec semaphore.
#include "internal/layer.h"

#include <exec/execbase.h>
#include <graphics/gfxbase.h>
#define __NOLIBBASE__
#include <proto/exec.h>

static void obtainLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6")) __attribute__((used));
static void releaseLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6")) __attribute__((used));
static BOOL attemptLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6")) __attribute__((used));

static void obtainLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
	
    ObtainSemaphore(&layer->Lock);
}

static void releaseLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
	
    ReleaseSemaphore(&layer->Lock);
}

static BOOL attemptLayer(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
	
    return AttemptSemaphore(&layer->Lock);
}

// A5 is the public argument register and may also be a compiler frame register.
// Keep that boundary in assembly. These vectors preserve scratch registers too:
// drawing callers retain their coordinates and RastPort across lock/unlock.
__asm__(
    ".text\n"
    ".globl _pcLockLayerRom\n"
    "_pcLockLayerRom:\n"
    "movem.l d0-d1/a0-a1,-(sp)\n"
    "move.l a5,a0\n"
    "jsr _obtainLayer\n"
    "movem.l (sp)+,d0-d1/a0-a1\n"
    "rts\n"
    ".globl _pcUnlockLayerRom\n"
    "_pcUnlockLayerRom:\n"
    "movem.l d0-d1/a0-a1,-(sp)\n"
    "move.l a5,a0\n"
    "jsr _releaseLayer\n"
    "movem.l (sp)+,d0-d1/a0-a1\n"
    "rts\n"
    ".globl _pcAttemptLockLayerRom\n"
    "_pcAttemptLockLayerRom:\n"
    "movem.l d1/a0-a1,-(sp)\n"
    "move.l a5,a0\n"
    "jsr _attemptLayer\n"
    "movem.l (sp)+,d1/a0-a1\n"
    "rts\n"
);
