/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/view.h>

// Original allocation extent, including the four legacy copper pointers omitted
// by the modern SDK unless V1_3 is defined. This is not sizeof(struct CopList).
#define COPLIST_STORAGE_SIZE 54

struct CopList* pcUCopperListInit(struct UCopList* list asm("a0"), WORD count asm("d0"));
LONG pcCMove(struct UCopList* list asm("a1"), APTR destination asm("d0"), WORD data asm("d1"));
LONG pcCWait(struct UCopList* list asm("a1"), WORD vertical asm("d0"), WORD horizontal asm("d1"));
void pcCBump(struct UCopList* list asm("a1"));

void pcFreeCopList(struct CopList* list asm("a0"));
void pcFreeCprList(struct cprlist* list asm("a0"));
void pcFreeVPortCopLists(struct ViewPort* viewport asm("a0"));

struct GfxBase;
// The public UWORD result is zero-extended in D0 by the original vector.
ULONG pcCalcIVG(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"));
