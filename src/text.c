/* SPDX-License-Identifier: MPL-2.0 */
// Software text metrics and glyph composition. Neither clipping nor the
// framebuffer affects measurements or the intermediate planar text raster.
#include "internal/text.h"
#include "internal/font.h"
#include "internal/memory.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#include <utility/hooks.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>

struct TextMeasure {
    WORD advance;
    WORD left;
    WORD right;  // Exclusive edge; TextExtent publishes an inclusive maximum.
};

static WORD textRightEdge(const struct BitMap* map)
{
    ULONG bytes = map->BytesPerRow;
	
    // The extended planar format describes an interleaved row by its plane
    // spacing, rather than by the distance to the next scanline.
    if (map->pad == 0x805c) {
	    bytes = (ULONG)map->Planes[1] - (ULONG)map->Planes[0];
    }
	
    return (WORD)(bytes * 8 - 1);
}

static void clearTextRectangle(struct RastPort* port, WORD left, WORD top, WORD right, WORD bottom,
    struct GfxBase* graphics)
{
    UBYTE foreground = port->FgPen;
    struct GelsInfo* gels = port->GelsInfo;
	
    // These text APIs retain the current mode, pattern and outline selection.
    // Only plain JAM2 chooses the background pen; other modes select pen zero.
    SetAPen(port, port->DrawMode == JAM2 ? (UBYTE)port->BgPen : 0);
    RectFill(port, left, top, right, bottom);
    port->GelsInfo = gels;
    SetAPen(port, foreground);
}

void pcClearEOL(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"))
{
    WORD top = port->cp_y - port->TxBaseline;
    WORD bottom = top + port->Font->tf_YSize - 1;
    clearTextRectangle(port, port->cp_x, top, textRightEdge(port->BitMap), bottom, graphics);
}

void pcClearScreen(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"))
{
    ClearEOL(port);
    // Read the current state after the public call: an installed ClearEOL
    // owner, or an enabled rectangle outline, may have moved the cursor.
    WORD top = port->cp_y - port->TxBaseline + port->Font->tf_YSize;
    ULONG bottom = (UWORD)(port->BitMap->Rows - 1);
	
    if ((LONG)bottom >= top) {
        clearTextRectangle(port, 0, top, textRightEdge(port->BitMap), (WORD)bottom, graphics);
    }
}

static UBYTE inkByte(const struct TextRaster* raster, ULONG row, unsigned byte)
{
    if (byte >= ((raster->width + 7U) >> 3)) {
	    return 0;
    }
	
    UBYTE value = 0;
	
    for (unsigned plane = 0; plane < raster->depth; ++plane) {
        value |= raster->planes[plane][row + byte];
    }
	
    return value;
}

static UBYTE rasterMask(const struct TextRaster* raster, unsigned byte)
{
    unsigned remaining = raster->width - byte * 8;
    return remaining >= 8 ? 0xff : (UBYTE)(0xffU << (8 - remaining));
}

// OR a run of font bits into the scratch raster. Assemble byte-sized chunks
// instead of testing/writing each destination pixel. Read a second source
// byte only if the requested bits cross that byte boundary.
static void composeRow(UBYTE* dest, const UBYTE* source, unsigned source_x, LONG dest_x,
    unsigned width, unsigned raster_width)
{
    if (dest_x < 0) {
        unsigned skip = -dest_x;
    	
        if (skip >= width) {
	        return;
        }
    	
        source_x += skip;
        width -= skip;
        dest_x = 0;
    }
	
    if ((ULONG)dest_x >= raster_width) {
	    return;
    }
	
    if (width > raster_width - dest_x) {
	    width = raster_width - dest_x;
    }
	
    while (width) {
        unsigned offset = dest_x & 7;
        unsigned bits = 8 - offset;
    	
        if (bits > width) {
	        bits = width;
        }
    	
        unsigned shift = source_x & 7;
        UBYTE value = source[source_x >> 3] << shift;
    	
        if (shift + bits > 8) {
	        value |= source[(source_x >> 3) + 1] >> (8 - shift);
        }
    	
        UBYTE mask = (0xffU << (8 - bits)) >> offset;
        dest[dest_x >> 3] |= (value >> offset) & mask;
        source_x += bits;
        dest_x += bits;
        width -= bits;
    }
}

void pcComposeText(const struct RastPort* port, CONST_STRPTR string, UWORD count, struct TextRaster* raster)
{
    if (!count) {
	    return;
    }
	
    const struct TextFont* font = port->Font;
    BOOL color = (font->tf_Style & FSF_COLORFONT) != 0;
    const struct ColorTextFont* color_font = (const void*)font;
    unsigned depth = color ? color_font->ctf_Depth : 1;
	
    if (depth > raster->depth) {
	    depth = raster->depth;
    }
	
    for (unsigned plane = 0; plane < raster->depth; ++plane) {
        ULONG size = (ULONG)raster->bytes_per_row * font->tf_YSize;
    	
        for (ULONG i = 0; i < size; ++i) {
	        raster->planes[plane][i] = 0;
        }
    }
	
    const WORD* kerning = font->tf_CharKern;
    const WORD* spacing = font->tf_CharSpace;
    const UWORD* locations = font->tf_CharLoc;
    unsigned fallback = (UBYTE)(font->tf_HiChar - font->tf_LoChar) + 1;
    BOOL italic = (port->AlgoStyle & FSF_ITALIC) && font->tf_Baseline < font->tf_YSize;
    unsigned smear = port->AlgoStyle & FSF_BOLD ? font->tf_BoldSmear : 0;
    WORD advance = 0;
	
    for (ULONG i = 0; i < count; ++i) {
        unsigned character = string[i];
        unsigned glyph = character < font->tf_LoChar || character > font->tf_HiChar ?
            fallback : character - font->tf_LoChar;
        WORD left = (UWORD)(advance + (kerning ? kerning[glyph] : 0));
        advance = (UWORD)(left + (spacing ? spacing[glyph] : font->tf_XSize) + port->TxSpacing);
        unsigned source_x = locations[2 * glyph];
        unsigned width = locations[2 * glyph + 1];
    	
        if (!width) {
	        continue;
        }
    	
        for (unsigned plane = 0; plane < depth; ++plane) {
            const UBYTE* source = color ? color_font->ctf_CharData[plane] : font->tf_CharData;
            UBYTE* dest = raster->planes[plane];
        	
            for (unsigned y = 0; y < font->tf_YSize; ++y) {
                LONG x = (LONG)left - raster->left;
            	
                if (italic) {
	                x += ((LONG)font->tf_Baseline - (LONG)y) >> 1;
                }
            	
                composeRow(dest, source, source_x, x, width, raster->width);
            	
                if (smear) {
	                composeRow(dest, source, source_x, x + smear, width, raster->width);
                }
            	
                source += font->tf_Modulo;
                dest += raster->bytes_per_row;
            }
        }
    }

    // Remapping follows glyph merging and software bold/italic: overlapping
    // color glyphs combine their plane bits before the foreground comparison.
    if (color && (color_font->ctf_Flags & CTF_MAPCOLOR) && color_font->ctf_FgColor >= color_font->ctf_Low &&
        color_font->ctf_FgColor <= color_font->ctf_High && color_font->ctf_FgColor < (1U << raster->depth)) {
        for (unsigned y = 0; y < font->tf_YSize; ++y) {
            ULONG row = (ULONG)y * raster->bytes_per_row;
        	
            for (unsigned byte = 0; byte < ((raster->width + 7U) >> 3); ++byte) {
                UBYTE selected = rasterMask(raster, byte);
            	
                for (unsigned plane = 0; plane < raster->depth; ++plane) {
                    UBYTE bits = raster->planes[plane][row + byte];
                    selected &= color_font->ctf_FgColor & (1U << plane) ? bits : ~bits;
                }
            	
                for (unsigned plane = 0; plane < raster->depth; ++plane) {
                    UBYTE* dest = &raster->planes[plane][row + byte];
                	
                    if ((UBYTE)port->FgPen & (1U << plane)) {
	                    *dest |= selected;
                    } else {
	                    *dest &= ~selected;
                    }
                }
            }
        }
    }
	
    unsigned underline = font->tf_Baseline + 1U;
	
    if ((port->AlgoStyle & FSF_UNDERLINED) && underline < font->tf_YSize) {
        ULONG row = (ULONG)underline * raster->bytes_per_row;
        UBYTE previous = 0;
        UBYTE current = inkByte(raster, row, 0);
        UBYTE pen = color ? (UBYTE)port->FgPen : 1;
    	
        for (unsigned byte = 0; byte < ((raster->width + 7U) >> 3); ++byte) {
            UBYTE next = inkByte(raster, row, byte + 1);
            UBYTE occupied = current | (current << 1) | (current >> 1) | (previous << 7) | (next >> 7);
            UBYTE selected = ~occupied & rasterMask(raster, byte);
        	
            for (unsigned plane = 0; plane < raster->depth; ++plane) {
                if (pen & (1U << plane)) {
	                raster->planes[plane][row + byte] |= selected;
                }
            }
        	
            previous = current;
            current = next;
        }
    }
}

static void styleExtent(const struct RastPort* port, struct TextMeasure* measure)
{
    const struct TextFont* font = port->Font;
	
    if (port->AlgoStyle & FSF_BOLD) {
	    measure->right = (UWORD)(measure->right + font->tf_BoldSmear);
    }
	
    if (port->AlgoStyle & FSF_ITALIC) {
        UWORD below = font->tf_YSize - font->tf_Baseline + 1;
        measure->left = (UWORD)(measure->left - (below >> 1));
        measure->right = (UWORD)(measure->right + (font->tf_Baseline >> 1));
    }
}

static struct TextMeasure measureText(const struct RastPort* port, CONST_STRPTR string, UWORD count)
{
    const struct TextFont* font = port->Font;
    const struct TextFontExtension* extension = (const void*)font->tf_Extension;
    struct TextMeasure measure = {0, 0, 0};
	
    // SetFont/ExtendFont establish these classifications from the glyph data.
    // They allow fixed-cell measurements without reading each character.
    if (!port->TxSpacing && (extension->tfe_Flags0 & FONT_EXTENSION_BYTE_CELL)) {
        measure.advance = (UWORD)(count * 8UL);
        measure.right = measure.advance;
    } else if (extension->tfe_Flags0 & FONT_EXTENSION_FIXED_CELL) {
        WORD step = (UWORD)(font->tf_XSize + port->TxSpacing);
    	
        if (step < 0) {
	        return measure;
        }
    	
        measure.advance = (UWORD)(count * (ULONG)(UWORD)step);
        measure.right = measure.advance;
    	
        if (port->TxSpacing < 0) {
	        measure.right = (UWORD)(measure.right - port->TxSpacing);
        }
    } else {
        const WORD* spacing = font->tf_CharSpace;
        const WORD* kerning = font->tf_CharKern;
        const UWORD* locations = font->tf_CharLoc;
        unsigned fallback = (UBYTE)(font->tf_HiChar - font->tf_LoChar) + 1;
    	
        for (ULONG i = 0; i < count; ++i) {
            unsigned character = string[i];
            unsigned glyph = character < font->tf_LoChar || character > font->tf_HiChar ?
                fallback : character - font->tf_LoChar;
            WORD left = (UWORD)(measure.advance + kerning[glyph]);
            WORD right = (UWORD)(left + locations[2 * glyph + 1]);
            measure.advance = (UWORD)(left + spacing[glyph] + port->TxSpacing);
        	
            if (left < measure.left) {
	            measure.left = left;
            }
        	
            if (right > measure.right) {
	            measure.right = right;
            }
        	
            if (measure.advance > measure.right) {
	            measure.right = measure.advance;
            }
        	
            if (measure.advance < measure.left) {
	            measure.left = measure.advance;
            }
        }
    }
	
    styleExtent(port, &measure);
	
    return measure;
}

static ULONG visibleText(struct Hook* hook asm("a0"))
{
    *(BOOL*)hook->h_Data = TRUE;
	
    return 0;
}

static BOOL textVisible(struct RastPort* port, WORD x, WORD y, UWORD width, UWORD height,
    struct GfxBase* graphics)
{
    if (!port->Layer) {
	    return TRUE;
    }
	
    // Let layers resolve visibility and backing storage; do not walk private
    // clipping state or allocate a raster for text with no drawable rectangle.
    struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
    BOOL visible = FALSE;
    struct Hook hook = {0};
    hook.h_Entry = (ULONG (*)())visibleText;
    hook.h_Data = &visible;
    struct Rectangle bounds = {x, y, (WORD)(x + width - 1), (WORD)(y + height - 1)};
    DoHookClipRects(&hook, port, &bounds);
	
    return visible;
}

static APTR temporaryRaster(const struct RastPort* port, ULONG size)
{
    return port->TmpRas && port->TmpRas->Size >= (LONG)size ? port->TmpRas->RasPtr : NULL;
}

static BOOL colorText(struct RastPort* port, CONST_STRPTR string, UWORD count, struct TextRaster* raster,
    WORD x, WORD y, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    const struct ColorTextFont* font = (const void*)port->Font;
    unsigned source_depth = font->ctf_Depth;
    unsigned dest_depth = port->BitMap->Depth;
    BOOL remap = (font->ctf_Flags & CTF_MAPCOLOR) && font->ctf_FgColor >= font->ctf_Low &&
        font->ctf_FgColor <= font->ctf_High;
	
    if (!remap && source_depth > dest_depth) {
	    source_depth = dest_depth;
    }
	
    raster->depth = source_depth > dest_depth ? source_depth : dest_depth;
	
    if (!raster->depth) {
	    return FALSE;
    }
	
    ULONG size = (ULONG)raster->bytes_per_row * font->ctf_TF.tf_YSize;
    unsigned allocated = 0;
	
    while (allocated < raster->depth) {
        raster->planes[allocated] = AllocMem(size, MEMF_PUBLIC | MEMF_CHIP);
    	
        if (!raster->planes[allocated]) {
	        break;
        }
    	
        ++allocated;
    }
	
    UBYTE* mask = NULL;
    BOOL borrowed = FALSE;
	
    if (allocated == raster->depth) {
        mask = temporaryRaster(port, size);
        borrowed = mask != NULL;
    	
        if (!borrowed) {
	        mask = AllocMem(size, MEMF_PUBLIC | MEMF_CHIP);
        }
    }
	
    if (!mask) {
        while (allocated) {
	        FreeMem(raster->planes[--allocated], size);
        }
    	
        return FALSE;
    }

    // A borrowed TmpRas may still be the source of a preceding queued blit.
    WaitBlit();
    pcComposeText(port, string, count, raster);
	
    struct BitMap bitmap = {0};
    bitmap.BytesPerRow = raster->bytes_per_row;
    bitmap.Rows = font->ctf_TF.tf_YSize;
    bitmap.Depth = raster->depth;
	
    for (unsigned plane = 0; plane < raster->depth; ++plane) {
        bitmap.Planes[plane] = raster->planes[plane];
    	
        if (port->DrawMode & INVERSVID) {
            for (ULONG i = 0; i < size; ++i) { raster->planes[plane][i] ^= 0xff; }
        }
    }
	
    unsigned mode = port->DrawMode & (JAM2 | COMPLEMENT);
	
    if (!mode) {
        // JAM1 first clears the union of nonzero color bits, then ORs the
        // composed color planes. Repeat the mask plane at every depth.
        for (ULONG i = 0; i < size; ++i) {
            UBYTE bits = 0;
        	
            for (unsigned plane = 0; plane < raster->depth; ++plane) {
	            bits |= raster->planes[plane][i];
            }
        	
            mask[i] = bits;
        }
    	
        struct BitMap coverage = bitmap;
    	
        for (unsigned plane = 0; plane < raster->depth; ++plane) {
	        coverage.Planes[plane] = mask;
        }
    	
        BltBitMapRastPort(&coverage, 0, 0, port, x, y, raster->width, bitmap.Rows, 0x2a);
    }
	
    static const UBYTE minterms[] = {0xea, 0xca, 0x6a, 0x5a};
    BltBitMapRastPort(&bitmap, 0, 0, port, x, y, raster->width, bitmap.Rows, minterms[mode]);
    WaitBlit();
	
    if (!borrowed) {
	    FreeMem(mask, size);
    }
	
    while (allocated) {
	    FreeMem(raster->planes[--allocated], size);
    }
	
    return TRUE;
}

static void drawText(struct RastPort* port, CONST_STRPTR string, UWORD count, struct GfxBase* graphics)
{
    const struct TextFont* font = port->Font;
    struct TextMeasure measure = measureText(port, string, count);
    struct TextRaster raster = {0};
    raster.left = measure.left;
    raster.width = (UWORD)(measure.right - measure.left);
	
    WORD x = (UWORD)(port->cp_x + measure.left);
    WORD y = (UWORD)(port->cp_y - font->tf_Baseline);
	
    if (!font->tf_YSize || !raster.width || !textVisible(port, x, y, raster.width, font->tf_YSize, graphics)) {
        port->cp_x = (UWORD)(port->cp_x + measure.advance);
    	
        return;
    }
	
    const struct TextFontExtension* extension = (const void*)font->tf_Extension;
    BOOL byte_cell = extension && extension->tfe_MatchWord == FONT_EXTENSION_MATCH &&
        extension->tfe_BackPtr == font && (extension->tfe_Flags0 & FONT_EXTENSION_BYTE_CELL) &&
        !port->TxSpacing && !(port->AlgoStyle & (FSF_BOLD | FSF_ITALIC | FSF_UNDERLINED));
	
    // Retain the original scratch sizes and TmpRas acceptance thresholds.
    // The software composer needs no separate left-side alignment anchor.
    UWORD padded = raster.width + (byte_cell ? 8 : 79 + font->tf_Baseline);
    raster.bytes_per_row = (padded >> 4) * 2;
	
    if (!byte_cell && (font->tf_Style & FSF_COLORFONT) &&
        colorText(port, string, count, &raster, x, y, graphics)) {
        port->cp_x = (UWORD)(port->cp_x + measure.advance);
    	
        return;
    }
	
    ULONG size = (ULONG)raster.bytes_per_row * font->tf_YSize;
    raster.depth = 1;
    raster.planes[0] = temporaryRaster(port, size);
    BOOL borrowed = raster.planes[0] != NULL;
	
    if (!borrowed) {
	    raster.planes[0] = pcAllocScratch(graphics, size, MEMF_CHIP);
    }
	
    if (!raster.planes[0]) {
        // A failed single glyph does not move the cursor. Longer strings fall
        // back to individual glyphs, each with its own allocation and advance.
        if (count > 1) {
            for (ULONG i = 0; i < count; ++i) {
	            drawText(port, string + i, 1, graphics);
            }
        }
    	
        return;
    }
	
    WaitBlit();
	
    if (font->tf_Style & FSF_COLORFONT) {
        // Color allocation failure uses the font's monochrome strike. Only
        // this local composition view changes; caller font/port state does not.
        struct TextFont mono_font = *font;
        mono_font.tf_Style &= ~FSF_COLORFONT;
        struct RastPort mono_port = *port;
        mono_port.Font = &mono_font;
        pcComposeText(&mono_port, string, count, &raster);
    } else {
	    pcComposeText(port, string, count, &raster);
    }
	
    BltTemplate(raster.planes[0], 0, raster.bytes_per_row, port, x, y, raster.width, font->tf_YSize);
	
    if (!borrowed) {
        WaitBlit();
        pcFreeScratch(graphics, raster.planes[0], size);
    }
	
    port->cp_x = (UWORD)(port->cp_x + measure.advance);
}

LONG pcText(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"),
    struct GfxBase* graphics asm("a6"))
{
    if (!count) {
	    return 0;
    }
	
    struct Layer* layer = port->Layer;
	
    if (layer) {
	    LockLayerRom(layer);
    }
	
    drawText(port, string, count, graphics);
	
    if (layer) {
	    UnlockLayerRom(layer);
    }
	
    // Text has no documented result; the NDK retains its historical LONG ABI.
    return 0;
}

static void verticalExtent(const struct TextFont* font, struct TextExtent* extent)
{
    extent->te_Height = font->tf_YSize;
    extent->te_Extent.MinY = (UWORD)-font->tf_Baseline;
    extent->te_Extent.MaxY = (UWORD)(font->tf_YSize - font->tf_Baseline - 1);
}

WORD pcTextLength(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"))
{
    // V36+ promises a sign-extended D0 despite the WORD C prototype. The
    // compiler's ordinary WORD return leaves the upper half unspecified.
    register WORD result asm("d0") = count ? measureText(port, string, count).advance : 0;
    __asm__ volatile("ext.l %0" : "+d"(result));
	
    return result;
}

WORD pcTextExtent(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), WORD count asm("d0"),
    struct TextExtent* extent asm("a2"))
{
    if (!count) {
        extent->te_Width = 0;
        extent->te_Height = 0;
        extent->te_Extent.MinX = 0;
        extent->te_Extent.MinY = 0;
        extent->te_Extent.MaxX = -1;
        extent->te_Extent.MaxY = -1;
        return 0;
    }
	
    struct TextMeasure measure = measureText(port, string, count);
    extent->te_Width = measure.advance;
    extent->te_Extent.MinX = measure.left;
    extent->te_Extent.MaxX = (UWORD)(measure.right - 1);
	
    verticalExtent(port->Font, extent);
	
    // The NDK declares a WORD result; the original returns the font height.
    return extent->te_Height;
}

void pcFontExtent(const struct TextFont* font asm("a0"), struct TextExtent* extent asm("a1"))
{
    const WORD* spacing = font->tf_CharSpace;
    const WORD* kerning = font->tf_CharKern;
    const UWORD* locations = font->tf_CharLoc;
    WORD advance = 0;
    WORD left = kerning ? 32767 : 0;
    WORD right = -32768;
    ULONG count = (UBYTE)(font->tf_HiChar - font->tf_LoChar) + 1UL;
	
    for (ULONG i = 0; i < count; ++i) {
        WORD glyph_left = kerning ? kerning[i] : 0;
        WORD glyph_right = (UWORD)(glyph_left + locations[2 * i + 1]);
        WORD step = (UWORD)(glyph_left + (spacing ? spacing[i] : font->tf_XSize));
    	
        if (glyph_left < left) {
	        left = glyph_left;
        }
    	
        if (glyph_right > right) {
	        right = glyph_right;
        }
    	
        if (font->tf_Flags & FPF_REVPATH) {
            if (step < advance) {
	            advance = step;
            }
        } else if (step > advance) {
	        advance = step;
        }
    }
	
    extent->te_Width = advance;
    extent->te_Extent.MinX = left;
    extent->te_Extent.MaxX = (UWORD)(right - 1);
	
    verticalExtent(font, extent);
}

ULONG pcTextFit(struct RastPort* port asm("a1"), CONST_STRPTR string asm("a0"), UWORD count asm("d0"),
    const struct TextExtent* extent asm("a2"), const struct TextExtent* constraint asm("a3"),
    WORD direction asm("d1"), UWORD width asm("d2"), UWORD height asm("d3"))
{
    // The NDK declares this pointer const, although the documented argument
    // is caller-owned output storage. Keep its declaration and register ABI.
    struct TextExtent* result = (struct TextExtent*)extent;
	
    if (!count || (WORD)height < (WORD)port->TxHeight) {
	    goto empty;
    }
	
    WORD min_x = -32768;
    WORD max_x = 32767;
    WORD max_advance = 32767;
	
    if (constraint) {
        min_x = constraint->te_Extent.MinX;
        max_x = constraint->te_Extent.MaxX;
        max_advance = constraint->te_Width;
        WORD top = (UWORD)-port->TxBaseline;
        WORD bottom = (UWORD)(top + port->TxHeight - 1);
    	
        if (top < constraint->te_Extent.MinY || bottom > constraint->te_Extent.MaxY) {
	        goto empty;
        }
    }
	
    const struct TextFont* font = port->Font;
	
    // Reserve the software-style overhang once, before testing individual
    // glyphs. The original fitting limits compare exclusive right edges.
    struct TextMeasure overhang = {0, 0, 0};
    styleExtent(port, &overhang);
	
    max_x = (UWORD)(max_x - overhang.right);
    min_x = (UWORD)(min_x - overhang.left);
    WORD available = (UWORD)(width - overhang.right + overhang.left);
	
    if (max_x < 0 || min_x > 0) {
	    goto empty;
    }
	
    const struct TextFontExtension* extension = (const void*)font->tf_Extension;
    struct TextMeasure measure = {0, 0, 0};
    UWORD fitted = 0;
	
    if (extension->tfe_Flags0 & FONT_EXTENSION_FIXED_CELL) {
        WORD step = (UWORD)(font->tf_XSize + port->TxSpacing);
    	
        if (step < 0) {
	        goto empty;
        }

        // Negative constraints retain their signed word value when selecting
        // the fitted count; only nonnegative advances are divided by the step.
        UWORD advance_count = (UWORD)max_advance;

        if (step && max_advance >= 0) {
            advance_count = (UWORD)max_advance / (UWORD)step;
            if (!advance_count) {
	            goto empty;
            }
        }
    	
        WORD pixels = max_x < available ? max_x : available;
        fitted = count;
    	
        if (step) {
            if (port->TxSpacing < 0) {
	            pixels = (UWORD)(pixels + port->TxSpacing);
            }
        	
            UWORD capacity = (UWORD)pixels / (UWORD)step;
        	
            if (advance_count && (WORD)advance_count <= (WORD)capacity) {
	            capacity = advance_count;
            }
        	
            if ((WORD)capacity <= (WORD)fitted) {
	            fitted = capacity;
            }
        } else if ((WORD)(pixels + port->TxSpacing) < 0) {
	        goto empty;
        }
    	
        measure = measureText(port, NULL, fitted);
    } else {
        const WORD* spacing = font->tf_CharSpace;
        const WORD* kerning = font->tf_CharKern;
        const UWORD* locations = font->tf_CharLoc;
        unsigned fallback = (UBYTE)(font->tf_HiChar - font->tf_LoChar) + 1;
        unsigned last_glyph = 0;
    	
        for (; fitted < count; ++fitted) {
            unsigned character = *string;
            unsigned glyph = character < font->tf_LoChar || character > font->tf_HiChar ?
                fallback : character - font->tf_LoChar;
        	
            WORD step = (UWORD)(kerning[glyph] + spacing[glyph] + port->TxSpacing);
            WORD advance = (UWORD)(measure.advance + step);
        	
            if (max_advance < 0 ? (advance < 0 && advance < max_advance) : advance > max_advance) {
	            break;
            }
        	
            struct TextMeasure candidate = measure;
            WORD glyph_left;
        	
            if (direction < 0) {
                // Prepend the new glyph: shift the accepted suffix by its
                // advance, then merge the new glyph's own origin and bounds.
                candidate.left = (UWORD)(candidate.left + step);
                candidate.right = (UWORD)(candidate.right + step);
            	
                if (candidate.right > available || candidate.right > max_x || candidate.left < min_x) {
	                break;
                }
            	
                glyph_left = kerning[glyph];
            } else {
	            glyph_left = (UWORD)(measure.advance + kerning[glyph]);
            }
        	
            WORD glyph_right = (UWORD)(glyph_left + locations[2 * glyph + 1]);
            candidate.advance = advance;
        	
            if (glyph_left < candidate.left) {
	            candidate.left = glyph_left;
            }
        	
            if (glyph_right > candidate.right) {
	            candidate.right = glyph_right;
            }
        	
            if (advance < candidate.left) {
	            candidate.left = advance;
            }
        	
            if (advance > candidate.right) {
	            candidate.right = advance;
            }
        	
            if (candidate.left < min_x || candidate.right > max_x ||
                (WORD)(candidate.right - candidate.left) > available) {
	            break;
            }
        	
            measure = candidate;
            last_glyph = glyph;
        	
            // Avoid even forming a before-start pointer after the final byte.
            if (fitted + 1 < count) {
	            string += direction < 0 ? -1 : 1;
            }
        }
    	
        if (direction < 0 && fitted) {
            if (kerning[last_glyph] >= 0) {
	            measure.left = (UWORD)(measure.left - kerning[last_glyph]);
            }
        	
            if (spacing[last_glyph] < 0) {
	            measure.left = 0;
            }
        }
    	
        styleExtent(port, &measure);
    }
	
    result->te_Width = measure.advance;
    result->te_Extent.MinX = measure.left;
    result->te_Extent.MaxX = (UWORD)(measure.right - 1);
	
    verticalExtent(font, result);
	
    return fitted;
empty:
    pcTextExtent(NULL, NULL, 0, result);
	
    return 0;
}
