/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>

// Internal startup storage for a zeroed, unpublished GfxBase with ExecBase set.
// This does not publish a library, initialize a monitor or install interrupts.
BOOL pcInitGraphicsState(struct GfxBase* base);
// Release only this storage, before publication or after its users are removed.
void pcFreeGraphicsState(struct GfxBase* base);
