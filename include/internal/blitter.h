/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <hardware/blit.h>

void pcOwnBlitter(struct GfxBase* base asm("a6"));
void pcDisownBlitter(struct GfxBase* base asm("a6"));

// Public backend entries. The framebuffer backend silently declines these
// hardware-only requests without modifying nodes or signaling completion.
void pcQBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"));
void pcQBSBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"));

// Shared submission for a backend that supplies the real blitter and server.
// The framebuffer's public entries do not submit to this queue.
void pcQueueBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"));
void pcQueueBSBlit(struct bltnode* node asm("a1"), struct GfxBase* base asm("a6"));

// Shared queue execution, called with interrupt exclusion by the output
// backend. It must supply the callback's real register context, establish
// transfer-idle before dispatch/completion, and satisfy/clear beamsync before
// dispatch. Neither routine polls hardware or schedules a completion event.
// Dispatch TRUE means a callback ran, not that its operation finished.
// Complete TRUE means queue ownership is retained and dispatch may resume.
// Arbitrary chipset-programming callbacks have no PC backend yet; do not wire
// these routines to a dummy register block or infer completion from return.
BOOL pcDispatchBlitter(APTR registers, struct GfxBase* base);
BOOL pcCompleteBlitter(APTR registers, struct GfxBase* base);
