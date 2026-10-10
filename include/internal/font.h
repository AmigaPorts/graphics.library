/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

// TextFontExtension's validation cookie and software-renderer classifications.
#define FONT_EXTENSION_MATCH 0x4e1b
#define FONT_EXTENSION_BYTE_CELL 2
#define FONT_EXTENSION_FIXED_CELL 4
// Compatibility flag used by diskfont extensions for tag-specific strikes.
#define FONT_EXTENSION_TAGGED_ONLY 128

struct TextFont* pcOpenFont(const struct TextAttr* attributes asm("a0"), struct GfxBase* graphics asm("a6"));
WORD pcWeighTAMatch(const struct TextAttr* requested asm("a0"), const struct TextAttr* target asm("a1"),
    const struct TagItem* tags asm("a2"), struct GfxBase* graphics asm("a6"));

LONG pcSetFont(struct RastPort* port asm("a1"), struct TextFont* font asm("a0"),
    struct GfxBase* graphics asm("a6"));
void pcAskFont(struct RastPort* port asm("a1"), struct TextAttr* attributes asm("a0"));
ULONG pcAskSoftStyle(struct RastPort* port asm("a1"));
ULONG pcSetSoftStyle(struct RastPort* port asm("a1"), ULONG style asm("d0"), ULONG enable asm("d1"),
    struct GfxBase* graphics asm("a6"));
void pcAddFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"));
void pcRemFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"));
void pcCloseFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"));
ULONG pcExtendFont(struct TextFont* font asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
void pcStripFont(struct TextFont* font asm("a0"), struct GfxBase* graphics asm("a6"));
