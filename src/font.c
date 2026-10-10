/* SPDX-License-Identifier: MPL-2.0 */
// Independent font state and lifetime handling through the public Amiga APIs.
#include "internal/font.h"

#include <exec/alerts.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <stddef.h>

// Older graphics font matching also recognizes this opaque tag. It is not
// TA_CharSet (TAG_USER + 3), and must not be interpreted as that newer tag.
#define FONT_TAG_LEGACY_MATCH (TAG_USER + 2)

static const struct TagItem tall_tags[] = {{TA_DeviceDPI, (100UL << 16) | 50}, {TAG_DONE, 0}};
static const struct TagItem wide_tags[] = {{TA_DeviceDPI, (50UL << 16) | 100}, {TAG_DONE, 0}};

static struct TextFontExtension* fontExtension(struct TextFont* font)
{
    struct TextFontExtension* extension = (APTR)font->tf_Extension;
	
    if (extension && extension->tfe_MatchWord == FONT_EXTENSION_MATCH && extension->tfe_BackPtr == font) {
        return extension;
    }
	
    return NULL;
}

static const struct TagItem* requestedTags(const struct TextAttr* attributes)
{
    if (!(attributes->ta_Style & FSF_TAGGED)) {
	    return NULL;
    }
	
    const struct TagItem* tags = ((const struct TTextAttr*)attributes)->tta_Tags;
	
    // Preserve the legacy TTextAttr sentinel convention.
    return (LONG)tags < 0 ? NULL : tags;
}

static BOOL requestsTaggedFont(const struct TextAttr* attributes, struct GfxBase* graphics)
{
    const struct TagItem* tags = requestedTags(attributes);
	
    if (!tags) {
	    return FALSE;
    }
	
    struct UtilityBase* UtilityBase = (APTR)graphics->UtilBase;
	
    return GetTagData(TA_DeviceDPI, 0, tags) || GetTagData(FONT_TAG_LEGACY_MATCH, 0, tags);
}

WORD pcWeighTAMatch(const struct TextAttr* requested asm("a0"), const struct TextAttr* target asm("a1"),
    const struct TagItem* tags asm("a2"), struct GfxBase* graphics asm("a6"))
{
    if ((WORD)requested->ta_YSize <= 0 || (WORD)target->ta_YSize <= 0) {
	    return 0;
    }
	
    struct UtilityBase* UtilityBase = (APTR)graphics->UtilBase;
    LONG adjustment = 0;
	const struct TagItem* request_tags = requestedTags(requested);
	
    if (request_tags) {
        ULONG request_dpi = GetTagData(TA_DeviceDPI, 0, request_tags);
        ULONG target_dpi = tags ? GetTagData(TA_DeviceDPI, (32UL << 16) | 32, tags) : (32UL << 16) | 32;
        ULONG first = (request_dpi >> 16) * (UWORD)target_dpi;
        ULONG second = (UWORD)request_dpi * (target_dpi >> 16);
    	
        if (first && second) {
            ULONG larger = first > second ? first : second;
            ULONG smaller = first > second ? second : first;
            // Retain the original score arithmetic, including its WORD result
            // and 32-bit shift. Do not change which existing strike wins.
            adjustment = (LONG)((larger << 5) / smaller) - 32;
        }
    	
        ULONG match = GetTagData(FONT_TAG_LEGACY_MATCH, 0, request_tags);
    	
        if (match && match != (tags ? GetTagData(FONT_TAG_LEGACY_MATCH, 0, tags) : 0)) {
	        --adjustment;
        }
    }
	
    LONG difference = (LONG)requested->ta_YSize - target->ta_YSize;
    LONG score = MAXFONTMATCHWEIGHT + adjustment - (difference < 0 ? -difference * 128 : difference * 32);
    UBYTE missing = requested->ta_Style & ~target->ta_Style;
    UBYTE extra = target->ta_Style & ~requested->ta_Style;
	
    if (missing & FSF_UNDERLINED) { score -= 4; }
    if (missing & FSF_BOLD) { score -= 8; }
    if (missing & FSF_ITALIC) { score -= 16; }
    if (extra & FSF_UNDERLINED) { score -= 2048; }
    if (extra & FSF_BOLD) { score -= 512; }
    if (extra & FSF_ITALIC) { score -= 1024; }
	
    UBYTE flags = target->ta_Flags;
	
    if (flags & (FPF_ROMFONT | FPF_DISKFONT)) { flags |= FPF_DESIGNED; }
    if ((requested->ta_Flags & FPF_DESIGNED) && !(flags & FPF_DESIGNED)) { score -= MAXFONTMATCHWEIGHT; }
    if ((requested->ta_Flags ^ flags) & FPF_REVPATH) { score -= MAXFONTMATCHWEIGHT; }
	
    return score < 0 ? 0 : (WORD)score;
}

struct TextFont* pcOpenFont(const struct TextAttr* attributes asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct TextFont* best = NULL;
    WORD best_weight = 0;
	
    Forbid();
	
    struct TextFont* font = (APTR)FindName(&graphics->TextFonts, attributes->ta_Name);
	
    while (font) {
        if (ExtendFont(font, NULL)) {
            struct TextFontExtension* extension = (APTR)font->tf_Extension;
            struct TextAttr target = {(STRPTR)font->tf_Message.mn_Node.ln_Name, font->tf_YSize, font->tf_Style, font->tf_Flags};
            WORD weight = WeighTAMatch(attributes, &target, extension->tfe_Tags);
        	
            if ((extension->tfe_Flags0 & FONT_EXTENSION_TAGGED_ONLY) && !requestsTaggedFont(attributes, graphics)) {
                weight = 0;
            }
        	
            if (weight > best_weight) {
	            best_weight = weight;
            	best = font;
            }
        	
            if (weight == MAXFONTMATCHWEIGHT) {
	            break;
            }
        }
    	
        // FindName also accepts a node as its starting point, continuing with
        // its successor. This preserves list priority when weights are equal.
        font = (APTR)FindName((struct List*)&font->tf_Message.mn_Node, attributes->ta_Name);
    }
	
    if (best) {
	    ++best->tf_Accessors;
    }
	
    Permit();
	
    return best;
}

ULONG pcExtendFont(struct TextFont* font asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    if (fontExtension(font)) {
	    return 1;
    }
	
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
	
    // An allocation may reschedule; hold an accessor until extension finishes.
    ++font->tf_Accessors;
	
    struct TextFontExtension* extension = AllocMem(sizeof(*extension), MEMF_PUBLIC | MEMF_CLEAR);
	
    if (!extension) {
	    --font->tf_Accessors; 
    	
    	return 0;
    }
	
    extension->tfe_MatchWord = FONT_EXTENSION_MATCH;
    extension->tfe_BackPtr = font;
    extension->tfe_OrigReplyPort = font->tf_Extension;
	
    if (!tags) {
        UBYTE aspect = font->tf_Flags & (FPF_TALLDOT | FPF_WIDEDOT);
    	
        if (aspect == FPF_TALLDOT) { tags = tall_tags; }
        else if (aspect == FPF_WIDEDOT) { tags = wide_tags; }
    }
	
    extension->tfe_Tags = (struct TagItem*)tags;

    // CharLoc contains bit offset/width pairs. Normalize the legacy cases in
    // which only one spacing array exists, or an implicit fixed cell is too
    // small for a glyph. Leave caller-owned arrays and raster data untouched.
    ULONG count = (UBYTE)(font->tf_HiChar - font->tf_LoChar) + 1UL;
    const UWORD* locations = font->tf_CharLoc;
    WORD* spacing = font->tf_CharSpace;
    WORD* kerning = font->tf_CharKern;
    BOOL oversized = FALSE;
	
    if (!spacing && !kerning) {
        for (ULONG i = 0; i < count; ++i) {
            if ((WORD)locations[2 * i + 1] > (WORD)font->tf_XSize) { oversized = TRUE; break; }
        }
    }
	
    if ((!kerning && spacing) || oversized) {
        extension->tfe_OFontPatchK = AllocVec(count * sizeof(WORD), MEMF_CLEAR);
        if (!extension->tfe_OFontPatchK) { goto fail; }
        kerning = (WORD*)extension->tfe_OFontPatchK;
    }
	
    if (!spacing && kerning) {
        extension->tfe_OFontPatchS = AllocVec(count * sizeof(WORD), MEMF_CLEAR);
        if (!extension->tfe_OFontPatchS) { goto fail; }
        spacing = (WORD*)extension->tfe_OFontPatchS;
        for (ULONG i = 0; i < count; ++i) { spacing[i] = font->tf_XSize; }
    }

    BOOL fixed_cell = TRUE;
    BOOL byte_cell = !(font->tf_Flags & FPF_PROPORTIONAL) && !(font->tf_Style & FSF_COLORFONT) &&
        font->tf_XSize == 8 && !spacing && !kerning;
    WORD row_bits = (UWORD)(font->tf_Modulo << 3);
	
    for (ULONG i = 0; i < count; ++i) {
        WORD offset = locations[2 * i];
        WORD width = locations[2 * i + 1];
        if (offset >= row_bits || (offset & 7) || width != 8) { byte_cell = FALSE; }
        if (kerning && (kerning[i] < 0 || (WORD)(kerning[i] + spacing[i]) != (WORD)font->tf_XSize ||
            spacing[i] < width)) { fixed_cell = FALSE; }
    }
	
    // Older font strikes may describe a final nonempty glyph outside the row.
    // As in the original normalization, shorten only that final glyph range.
    // Earlier invalid glyphs do not justify changing the declared high char.
    if (!byte_cell) {
        for (ULONG i = 0; i < count; ++i) {
            WORD width = locations[2 * i + 1];
            if (width && (WORD)(locations[2 * i] + width) > row_bits) {
                if (i + 1 == count) { --font->tf_HiChar; }
                break;
            }
        }
    }
	
    if (byte_cell) { extension->tfe_Flags0 = FONT_EXTENSION_BYTE_CELL | FONT_EXTENSION_FIXED_CELL; }
    else if (fixed_cell) { extension->tfe_Flags0 = FONT_EXTENSION_FIXED_CELL; }
	
    font->tf_CharSpace = spacing;
    font->tf_CharKern = kerning;
    font->tf_Extension = (struct MsgPort*)extension;
    --font->tf_Accessors;
	
    return 1;
fail:
    Alert(AN_ObsoleteFont);
    FreeVec(extension->tfe_OFontPatchS);
    FreeVec(extension->tfe_OFontPatchK);
    FreeMem(extension, sizeof(*extension));
    --font->tf_Accessors;
	
    return 0;
}

void pcStripFont(struct TextFont* font asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct TextFontExtension* extension = fontExtension(font);
    if (!extension) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    font->tf_Extension = extension->tfe_OrigReplyPort;
    if (extension->tfe_OFontPatchS) {
        font->tf_CharSpace = NULL;
        FreeVec(extension->tfe_OFontPatchS);
    }
    if (extension->tfe_OFontPatchK) {
        font->tf_CharKern = NULL;
        FreeVec(extension->tfe_OFontPatchK);
    }
    FreeMem(extension, sizeof(*extension));
}

LONG pcSetFont(struct RastPort* port asm("a1"), struct TextFont* font asm("a0"),
    struct GfxBase* graphics asm("a6"))
{
    if (!port) { return 0; }
    if (font && !ExtendFont(font, NULL)) { return 0; }
    port->Font = font;
    port->AlgoStyle = 0;
    port->TxHeight = font ? font->tf_YSize : 0;
    port->TxWidth = font ? font->tf_XSize : 0;
    port->TxBaseline = font ? font->tf_Baseline : 0;
    return font ? 1 : 0;
}

void pcAskFont(struct RastPort* port asm("a1"), struct TextAttr* attributes asm("a0"))
{
    const struct TextFont* font = port->Font;
    attributes->ta_Name = (STRPTR)font->tf_Message.mn_Node.ln_Name;
    attributes->ta_YSize = font->tf_YSize;
    attributes->ta_Style = font->tf_Style;
    attributes->ta_Flags = font->tf_Flags;
}

static ULONG __attribute__((used, noinline)) askSoftStyleWithState(struct RastPort* port asm("a1"),
    ULONG incoming asm("d1"))
{
    const struct TextFont* font = port->Font;
    if (!font) { return 0; }
    ULONG unavailable = font->tf_Style;
    if (!(unavailable & FSF_UNDERLINED)) {
        // The original replaces only D1's low byte with the baseline's high
        // byte, then compares the incremented signed word against tf_YSize.
        WORD baseline = (incoming & 0xff00) | ((UWORD)font->tf_Baseline >> 8);
        baseline = (WORD)(baseline + 2);
        if (baseline > (WORD)font->tf_YSize) { unavailable |= FSF_UNDERLINED; }
    }
    return ~unavailable;
}

// Keep the public one-argument API while making the original incoming D1
// dependency explicit to the compiler. The alias adds no wrapper instructions.
__asm__(".globl _pcAskSoftStyle\n.set _pcAskSoftStyle,_askSoftStyleWithState\n");

static ULONG querySoftStyle(struct RastPort* port, struct GfxBase* graphics, ULONG style, ULONG enable)
{
    register struct RastPort* argument asm("a1") = port;
    register struct GfxBase* base asm("a6") = graphics;
    register ULONG result asm("d0") = style;
    register ULONG state asm("d1") = enable;
    // SetSoftStyle's original public-vector call passes style/enable through
    // unchanged. AskSoftStyle observes D1 despite its one-argument SDK API.
    __asm__ volatile("jsr -84(a6)" : "+r"(result), "+r"(state), "+r"(argument)
        : "r"(base) : "a0", "cc", "memory");
    return result;
}

ULONG pcSetSoftStyle(struct RastPort* port asm("a1"), ULONG style asm("d0"), ULONG enable asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    if (!port->Font) { return 0; }
    UBYTE mask = enable & querySoftStyle(port, graphics, style, enable);
    port->AlgoStyle = (port->AlgoStyle & ~mask) | (style & mask);
    return port->AlgoStyle | port->Font->tf_Style;
}

void pcAddFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    font->tf_Flags &= ~FPF_REMOVED;
    font->tf_Accessors = 0;
    font->tf_Message.mn_Node.ln_Type = NT_FONT;
    Forbid();
    Enqueue(&graphics->TextFonts, &font->tf_Message.mn_Node);
    Permit();
}

static void releaseFont(struct TextFont* font, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    // The extension borrows mn_ReplyPort. Restore it before returning the
    // message to the owner; graphics.library does not own the font itself.
    StripFont(font);
    ReplyMsg(&font->tf_Message);
}

void pcRemFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"))
{
    struct TextFontExtension* extension = fontExtension(font);
    if (extension && (extension->tfe_Flags0 & TE0F_NOREMFONT)) { return; }
    if (!(font->tf_Flags & FPF_REMOVED)) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        font->tf_Flags |= FPF_REMOVED;
        font->tf_Message.mn_Node.ln_Type = NT_MESSAGE;
        Forbid();
        Remove(&font->tf_Message.mn_Node);
        Permit();
    }
    if ((WORD)font->tf_Accessors <= 0) { releaseFont(font, graphics); }
}

void pcCloseFont(struct TextFont* font asm("a1"), struct GfxBase* graphics asm("a6"))
{
    --font->tf_Accessors;
    if ((WORD)font->tf_Accessors <= 0 && (font->tf_Flags & FPF_REMOVED)) { releaseFont(font, graphics); }
}
