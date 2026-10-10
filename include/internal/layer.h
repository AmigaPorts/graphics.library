/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <graphics/clip.h>

// Vector-table entry symbols: layer in A5, graphics base in A6. C callers use
// the SDK public-vector macros, which preserve the compiler's live A5 value.
void pcLockLayerRom(void);
void pcUnlockLayerRom(void);
BOOL pcAttemptLockLayerRom(void);
