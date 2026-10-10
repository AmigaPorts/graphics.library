/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/rastport.h>
#include <graphics/text.h>

// Internal planar text scratch storage, owned by the Text caller. Each plane
// has bytes_per_row * Font->tf_YSize bytes. left is relative to the cursor;
// width is the measured drawing span, excluding word-alignment padding.
// Color-remapped fonts need at least ctf_Depth planes, even when the eventual
// destination has fewer: all input color bits participate in remapping.
struct TextRaster {
    UBYTE* planes[8];
    UWORD bytes_per_row;
    UWORD width;
    WORD left;
    UBYTE depth;
};

void pcComposeText(const struct RastPort* port, CONST_STRPTR string, UWORD count, struct TextRaster* raster);

struct GfxBase;
void pcClearEOL(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"));
void pcClearScreen(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"));
LONG pcText(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"),
    struct GfxBase* graphics asm("a6"));
WORD pcTextLength(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"));
WORD pcTextExtent(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), WORD count asm("d0"),
    struct TextExtent* extent asm("a2"));
void pcFontExtent(const struct TextFont* font asm("a0"), struct TextExtent* extent asm("a1"));
ULONG pcTextFit(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"),
    const struct TextExtent* extent asm("a2"), const struct TextExtent* constraint asm("a3"),
    WORD direction asm("d1"), UWORD width asm("d2"), UWORD height asm("d3"));
