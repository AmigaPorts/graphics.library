/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gels.h>
#include <graphics/rastport.h>

void pcInitGels(struct VSprite* head asm("a0"), struct VSprite* tail asm("a1"), struct GelsInfo* info asm("a2"));
void pcAddVSprite(struct VSprite* sprite asm("a0"), struct RastPort* port asm("a1"));
void pcAddBob(struct Bob* bob asm("a0"), struct RastPort* port asm("a1"));
void pcRemVSprite(struct VSprite* sprite asm("a0"));
struct GfxBase;
struct ViewPort;
void pcRemIBob(struct Bob* bob asm("a0"), struct RastPort* port asm("a1"), struct ViewPort* viewport asm("a2"),
    struct GfxBase* graphics asm("a6"));
void pcSetCollision(ULONG number asm("d0"), VOID (*routine)(struct VSprite*, struct VSprite*) asm("a0"),
    struct GelsInfo* info asm("a1"));
void pcSortGList(struct RastPort* port asm("a1"));
void pcDrawGList(struct RastPort* port asm("a1"), struct ViewPort* viewport asm("a0"),
    struct GfxBase* graphics asm("a6"));
void pcDoCollision(struct RastPort* port asm("a1"));
void pcInitMasks(struct VSprite* sprite asm("a0"));
void pcInitGMasks(struct AnimOb* object asm("a0"));
void pcAddAnimOb(struct AnimOb* object asm("a0"), struct AnimOb** key asm("a1"), struct RastPort* port asm("a2"));
void pcAnimate(struct AnimOb** key asm("a0"), struct RastPort* port asm("a1"));
BOOL pcGetGBuffers(struct AnimOb* object asm("a0"), struct RastPort* port asm("a1"), BOOL double_buffer asm("d0"));
void pcFreeGBuffers(struct AnimOb* object asm("a0"), struct RastPort* port asm("a1"), BOOL double_buffer asm("d0"));

// Internal boundary calculation for DoCollision; not a public library vector.
WORD pcGelBorderHits(const struct VSprite* sprite, const struct GelsInfo* info);

// Internal GEL bitmap transfers. Coordinates have already been clipped by the
// drawing/restoration traversal; these are not new public library vectors.
void pcGelSaveBitmap(const struct VSprite* sprite, const struct BitMap* bitmap,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height, UBYTE mask);
void pcGelDrawBitmap(const struct VSprite* sprite, struct BitMap* bitmap,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height, UBYTE mask);

// Internal DrawGList transfer over the caller-locked layer and its backing
// bitmaps. The rectangle is already clipped to the RastPort bitmap dimensions.
void pcGelTransferRastPort(const struct VSprite* sprite, struct RastPort* port, BOOL save,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height);

// Internal Bob traversal and frame completion. The caller owns the layer lock
// and dispatches VSPRITE nodes separately; these do not replace DrawGList's API.
void pcGelProcessBob(struct VSprite* sprite, struct RastPort* port, struct VSprite** last,
    struct GfxBase* graphics);
void pcGelFinishFrame(struct RastPort* port, struct VSprite* last, struct GfxBase* graphics);

// Internal VSprite slot assignment after clipping. Retains the caller-visible
// nextLine/lastColor state; does not access hardware or emit display lists.
WORD pcGelAssignSprite(struct VSprite* sprite, struct GelsInfo* info, WORD top, WORD height, UBYTE reserved);
