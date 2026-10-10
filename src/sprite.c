/* SPDX-License-Identifier: MPL-2.0 */
// Public sprite storage and bitmap conversion. This does not claim or program
// a hardware sprite, or change the current display.
#include "internal/sprite.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#define __NOLIBBASE__
#include <proto/exec.h>
#include <proto/utility.h>

static void expandRow(UWORD* row)
{
    // Double each source pixel. Work backwards so the expanded words cannot
    // overwrite source pixels still needed for this row.
    for (int word = 7; word >= 0; --word) {
        UWORD source = row[word];
        ULONG expanded = 0;
        for (unsigned bit = 0; bit < 16; ++bit) {
            expanded = (expanded << 2) | ((source & 0x8000) ? 3 : 0);
            source <<= 1;
        }
        row[2 * word] = expanded >> 16;
        row[2 * word + 1] = expanded;
    }
}

static void contractRow(UWORD* row)
{
    // Retain every other pixel, starting with the most significant bit.
    UBYTE* output = (APTR)row;
    for (unsigned word = 0; word < 16; ++word) {
        UWORD source = row[word];
        UBYTE contracted = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            contracted = (contracted << 1) | (source >> 15);
            source <<= 2;
        }
        output[word] = contracted;
    }
    for (unsigned word = 8; word < 16; ++word) { row[word] = 0; }
}

struct ExtSprite* pcAllocSpriteDataA(const struct BitMap* bitmap asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    UWORD height = bitmap->Rows, width = 1, old_format = 0;
    WORD x_replication = 0, y_replication = 0;
    BOOL attached = FALSE;
    ULONG source_bytes = bitmap->BytesPerRow;
    if (bitmap->pad == 0x805c) { source_bytes = (ULONG)bitmap->Planes[1] - (ULONG)bitmap->Planes[0]; }
    UWORD source_words = source_bytes >> 1;
    struct TagItem* cursor = (APTR)tags;
    struct TagItem* tag;
    while ((tag = NextTagItem(&cursor))) {
        switch (tag->ti_Tag) {
            case SPRITEA_Width:
                width = (UBYTE)tag->ti_Data ? (UWORD)tag->ti_Data : 16;
                width = (UWORD)(width + 15) >> 4;
                if (width == 3) { width = 4; }
                break;
            case SPRITEA_XReplication: x_replication = tag->ti_Data; break;
            case SPRITEA_YReplication: y_replication = tag->ti_Data; break;
            case SPRITEA_OutputHeight: height = tag->ti_Data; break;
            case SPRITEA_Attached: if (tag->ti_Data) { attached = TRUE; } break;
            case SPRITEA_OldDataFormat: old_format = tag->ti_Data; break;
        }
    }

    ULONG stride = old_format ? 4 : bitmap->BytesPerRow;
    if (old_format) { source_words = 1; }
    // OutputHeight is the requested row count. Negative Y replication skips
    // input rows; a positive value does not synthesize additional rows here.
    UWORD skip = (WORD)-y_replication > 0 ? -y_replication : 0;
    skip &= 63;
    stride = skip >= 32 ? 0 : stride << skip;
    UWORD required = width, shift = (x_replication < 0 ? -x_replication : x_replication) & 63;
    if (x_replication < 0) { required = shift >= 16 ? 0 : (UWORD)(required << shift); }
    else {
        required = shift >= 16 ? 0 : required >> shift;
        if (!required) { required = 1; }
    }
    if (source_words > required) { source_words = required; }

    struct ExtSprite* sprite = AllocMem(sizeof(*sprite), 0);
    if (!sprite) { return NULL; }
    sprite->es_wordwidth = width;
    sprite->es_SimpleSprite.height = height;
    if (old_format && !x_replication && !y_replication && width == 1) {
        sprite->es_SimpleSprite.posctldata = (APTR)bitmap;
        // Only the high byte is set by the original borrowed-data path. The
        // remaining SimpleSprite fields and low flag byte retain their allocation contents.
        ((UBYTE*)&sprite->es_flags)[0] = 0xff;
        return sprite;
    }
    sprite->es_flags = 0;
    ULONG padding = (width & 63) >= 32 ? 0 : 2UL << (width & 63);
    ULONG bytes = ((ULONG)height * width * 2 + padding) * 2 + 4;
    UBYTE* storage = AllocVec(bytes, MEMF_CHIP);
    if (!storage) { FreeMem(sprite, sizeof(*sprite)); return NULL; }
    UWORD* output = (APTR)(storage + 4);
    sprite->es_SimpleSprite.posctldata = output;
    UWORD control = attached ? SPRITE_ATTACHED : 0;
    *output++ = 0; *output++ = control;
    if (width >= 2) { *output++ = control; *output++ = control; }
    if (width >= 4) { *output++ = control; *output++ = 0; *output++ = 0; *output++ = 0; }
    const UBYTE* planes[2];
    if (old_format) { planes[0] = (const UBYTE*)bitmap + 4; planes[1] = (const UBYTE*)bitmap + 6; }
    else {
        planes[0] = bitmap->Planes[attached ? 2 : 0];
        planes[1] = bitmap->Planes[attached ? 3 : 1];
    }
    for (ULONG line = 0; line < (UWORD)(height * 2); ++line) {
        UWORD row[16] = {0};
        unsigned plane = line & 1;
        const UWORD* source = (APTR)planes[plane];
        for (unsigned word = 0; word < source_words; ++word) { row[word] = source[word]; }
        if (x_replication < 0) {
            contractRow(row);
            if (!(x_replication & 1)) { contractRow(row); }
        } else if (x_replication) {
            expandRow(row);
            if (x_replication & 2) { expandRow(row); }
        }
        for (unsigned word = 0; word < width; ++word) { *output++ = row[word]; }
        planes[plane] += stride;
    }
    *output++ = 0; *output++ = 0;
    if (width >= 2) { *output++ = 0; *output++ = 0; }
    if (width >= 4) { *output++ = 0; *output++ = 0; *output++ = 0; *output++ = 0; }
    return sprite;
}

void pcFreeSpriteData(struct ExtSprite* sprite asm("a2"), struct GfxBase* graphics asm("a6"))
{
    if (!sprite) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (!sprite->es_flags) { FreeVec((UBYTE*)sprite->es_SimpleSprite.posctldata - 4); }
    FreeMem(sprite, sizeof(*sprite));
}
