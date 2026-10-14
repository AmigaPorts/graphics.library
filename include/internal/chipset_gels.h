/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/gels.h>
#include <graphics/rastport.h>

WORD pcGelBeamLine(const struct VSprite* sprite, const struct RastPort* port,
    struct ViewPort* viewport, BOOL restore, struct GfxBase* graphics);
void pcQueueGelCopy(struct GelsInfo* info, const struct BitMap* source, struct BitMap* dest,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height,
    UBYTE mask, WORD beam, struct GfxBase* graphics);
void pcQueueGelDraw(struct GelsInfo* info, const struct VSprite* sprite, struct BitMap* dest,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height,
    UBYTE mask, WORD beam, struct GfxBase* graphics);
void pcFinishGelQueue(struct GelsInfo* info, struct GfxBase* graphics);
