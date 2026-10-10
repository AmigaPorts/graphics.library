/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/sprite.h>
#include <utility/tagitem.h>

struct ExtSprite* pcAllocSpriteDataA(const struct BitMap* bitmap asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
void pcFreeSpriteData(struct ExtSprite* sprite asm("a2"), struct GfxBase* graphics asm("a6"));
