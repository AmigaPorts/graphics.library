/* SPDX-License-Identifier: MPL-2.0 */
// Independent CPU implementation of the Amiga planar bitmap operations.
#include "internal/bitmap.h"
#include "internal/backend.h"
#include "internal/blitter.h"
#include "internal/memory.h"
#include "internal/native.h"
#include "internal/rastport.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <graphics/rastport.h>
#include <graphics/gfxbase.h>
#include <graphics/scale.h>
#include <utility/hooks.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/layers.h>

static UWORD combine(UWORD source, UWORD dest, UBYTE minterm)
{
    switch (minterm) {
        case 0x00: return 0;
        case 0x10: return ~(source | dest);
        case 0x20: return ~source & dest;
        case 0x30: return ~source;
        case 0x40: return source & ~dest;
        case 0x50: return ~dest;
        case 0x60: return source ^ dest;
        case 0x70: return ~(source & dest);
        case 0x80: return source & dest;
        case 0x90: return ~(source ^ dest);
        case 0xa0: return dest;
        case 0xb0: return ~source | dest;
        case 0xc0: return source;
        case 0xd0: return source | ~dest;
        case 0xe0: return source | dest;
        default: return 0xffff;
    }
}

// Return count source bits in the high bits of a word. Read the next word only
// when those pixels cross a word boundary; never read beyond a final scanline.
static UWORD sourceBits(const UWORD* source, ULONG x, unsigned count)
{
    if (!source) {
	    return 0;
    }
	
    if ((ULONG)source == 0xffffffff) {
	    return 0xffff;
    }
	
    unsigned shift = x & 15;
    source += x >> 4;
    UWORD value = *source << shift;
	
    if (shift + count > 16) {
	    value |= source[1] >> (16 - shift);
    }
	
    return value;
}

static void copyRow(const UWORD* source, UWORD* dest, ULONG source_x, ULONG dest_x,
    ULONG width, UBYTE minterm, int backward)
{
    // Whole-word copies need neither alignment masks nor minterm dispatch.
    if (minterm == 0xc0 && source && (ULONG)source != 0xffffffff && !((source_x | dest_x | width) & 15)) {
        source += source_x >> 4;
        dest += dest_x >> 4;
        ULONG words = width >> 4;
    	
        if (backward) {
            source += words;
            dest += words;
        	
            while (words--) {
	            *--dest = *--source;
            }
        } else {
            while (words--) {
	            *dest++ = *source++;
            }
        }
    	
        return;
    }
	
    ULONG remaining = width;
	
    while (remaining) {
        unsigned count;
        ULONG offset;
    	
        if (backward) {
            count = ((dest_x + remaining - 1) & 15) + 1;
        	
            if (count > remaining) {
	            count = remaining;
            }
        	
            offset = remaining - count;
        } else {
            offset = width - remaining;
            count = 16 - ((dest_x + offset) & 15);
        	
            if (count > remaining) {
	            count = remaining;
            }
        }
    	
        unsigned shift = (dest_x + offset) & 15;
        UWORD mask = (0xffffUL >> (16 - count)) << (16 - shift - count);
        UWORD bits = sourceBits(source, source_x + offset, count) >> shift;
        UWORD* word = dest + ((dest_x + offset) >> 4);
        UWORD old = *word;
        *word = (old & ~mask) | (combine(bits, old, minterm) & mask);
        remaining -= count;
    }
}

// Overlapping rasters retain the traversal selected for the complete operation.
// Read the next output's source and destination before storing the current
// output, including across row boundaries where the rasters can overlap.
static void copyAliasedRows(const UWORD* source, UWORD* dest, ULONG source_x, ULONG dest_x, ULONG width, ULONG height,
    LONG source_step, LONG dest_step, UBYTE minterm, BOOL backward, BOOL extra_word)
{
    UWORD* pending = NULL;
    UWORD pending_bits = 0;
    BOOL constant = !source || (ULONG)source == 0xffffffff;

    for (ULONG row = 0; row < height; ++row) {
        const UWORD* saved_source = NULL;
        UWORD saved_bits = 0;
        ULONG remaining = width;
        BOOL extra = extra_word;

        while (remaining || extra) {
            // A shifted template can span one more source word than its
            // destination. Its masked-off output still participates in the
            // read/write ordering when adjacent destination rows overlap.
            if (extra && (backward || !remaining)) {
                UWORD* word = dest + ((dest_x + width - 1) >> 4) + 1;
                UWORD old = *word;

                if (pending) {
                    *pending = pending_bits;
                }

                pending = word;
                pending_bits = old;
                extra = FALSE;

                continue;
            }

            unsigned count;
            ULONG offset;

            if (backward) {
                count = ((dest_x + remaining - 1) & 15) + 1;

                if (count > remaining) {
                    count = remaining;
                }

                offset = remaining - count;
            } else {
                offset = width - remaining;
                count = 16 - ((dest_x + offset) & 15);

                if (count > remaining) {
                    count = remaining;
                }
            }

            UWORD bits = source ? 0xffff : 0;

            if (!constant) {
                const UWORD* input = source + ((source_x + offset) >> 4);
                unsigned source_shift = (source_x + offset) & 15;
                UWORD first = input == saved_source ? saved_bits : *input;
                bits = first << source_shift;

                if (source_shift + count > 16) {
                    UWORD second = input + 1 == saved_source ? saved_bits : input[1];

                    bits |= second >> (16 - source_shift);
                    saved_source = backward ? input : input + 1;
                    saved_bits = backward ? first : second;
                } else {
                    saved_source = input;
                    saved_bits = first;
                }
            }

            unsigned shift = (dest_x + offset) & 15;
            UWORD* word = dest + ((dest_x + offset) >> 4);
            UWORD old = *word;
            UWORD mask = (0xffffUL >> (16 - count)) << (16 - shift - count);

            if (pending) {
                *pending = pending_bits;
            }

            pending = word;
            pending_bits = (old & ~mask) | (combine(bits >> shift, old, minterm) & mask);
            remaining -= count;
        }

        if (row + 1 < height) {
            if (!constant) {
                source = (const UWORD*)((const UBYTE*)source + source_step);
            }

            dest = (UWORD*)((UBYTE*)dest + dest_step);
        }
    }

    if (pending) {
        *pending = pending_bits;
    }
}

ULONG pcBltBitMap(const struct BitMap* source asm("a0"), UWORD source_x asm("d0"),
    UWORD source_y asm("d1"), struct BitMap* dest asm("a1"), UWORD dest_x asm("d2"),
    UWORD dest_y asm("d3"), UWORD width asm("d4"), UWORD height asm("d5"),
    UBYTE minterm asm("d6"), UBYTE planes asm("d7"), UWORD* temporary asm("a2"),
    struct GfxBase* graphics asm("a6"))
{
    // CPU traversal handles horizontal overlap without a temporary A plane.
    // Traversal order is shared by every selected plane, as in the original library.
    (void)temporary;

    if (!width || !height) {
        return 0;
    }

    unsigned depth = source->Depth < dest->Depth ? source->Depth : dest->Depth;
    ULONG count = 0;
    minterm &= 0xf0;

    BOOL chipset = depth && (planes & ((1UL << depth) - 1)) && pcUsesChipset(graphics);

    // The original owns through the public vectors, then waits internally.
    // Observe the previous owner's DMA before reading or writing any plane.
    if (chipset) {
        OwnBlitter();
        pcWaitBlit(graphics);
    }

    BOOL reverse_rows = FALSE;
    BOOL reverse_words = FALSE;

    for (unsigned plane = depth; plane > 0; --plane) {
        if (!(planes & (1UL << (plane - 1)))) {
            continue;
        }

        // Overlap setup searches from the highest selected plane. A horizontal
        // overlap may still traverse rows forward while copying words backward.
        // Address ordering is signed, including the all-ones source sentinel.
        ULONG input = (ULONG)source->Planes[plane - 1] + (ULONG)source_y * source->BytesPerRow +
            (source_x >> 4) * 2;
        ULONG output = (ULONG)dest->Planes[plane - 1] + (ULONG)dest_y * dest->BytesPerRow +
            (dest_x >> 4) * 2;

        if (source->BytesPerRow != dest->BytesPerRow) {
            reverse_rows = (dest_x & 15) >= (source_x & 15);
            reverse_words = !reverse_rows;
        } else if ((LONG)output >= (LONG)input) {
            ULONG last_word = input + (((source_x & 15) + width - 1) >> 4) * 2;

            reverse_rows = (dest_x & 15) <= (source_x & 15) || (LONG)output > (LONG)last_word;
            reverse_words = (dest_x & 15) <= (source_x & 15) || (LONG)output <= (LONG)last_word;
        } else {
            ULONG last_word = output + (((dest_x & 15) + width - 1) >> 4) * 2;

            reverse_words = (dest_x & 15) < (source_x & 15) && (LONG)last_word < (LONG)input;
        }

        break;
    }

    for (unsigned plane = 0; plane < depth; ++plane) {
        if (!(planes & (1UL << plane))) {
            continue;
        }

        ++count;

        if (minterm == 0xa0) {
            continue;
        }

        const UBYTE* input = source->Planes[plane];
        UBYTE* output = dest->Planes[plane] + (ULONG)dest_y * dest->BytesPerRow;
        int constant = !input || (ULONG)input == 0xffffffff;

        if (!constant) {
            input += (ULONG)source_y * source->BytesPerRow;
        }

        // The usual direction needs no read-ahead buffer. Opposing planes
        // and overlapping unequal-pitch rasters retain the shared read order.
        int backward = !constant &&
            ((ULONG)output + (dest_x >> 3) > (ULONG)input + (source_x >> 3) ||
                ((ULONG)output + (dest_x >> 3) == (ULONG)input + (source_x >> 3) && (dest_x & 7) > (source_x & 7)));
        BOOL read_ahead = !constant && backward != reverse_words;
        LONG source_step = source->BytesPerRow, dest_step = dest->BytesPerRow;

        if (!constant && (read_ahead || source_step != dest_step)) {
            ULONG first_source = (ULONG)input + (source_x >> 4) * 2;
            ULONG last_source = (ULONG)input + ((source_x + width - 1) >> 4) * 2;
            ULONG first_dest = (ULONG)output + (dest_x >> 4) * 2;
            ULONG last_dest = (ULONG)output + ((dest_x + width - 1) >> 4) * 2;

            // Equal-pitch rows can overlap a later row of the other raster
            // even when their first-row spans do not intersect.
            last_source += (height - 1) * source_step;
            last_dest += (height - 1) * dest_step;

            read_ahead = first_source <= last_dest && first_dest <= last_source;
        }

        // Narrow declared rows can share destination words even when the
        // source is separate or constant. Preserve the destination read order.
        ULONG dest_words = (((dest_x & 15) + width - 1) >> 4) + 1;

        if (height > 1 && dest_words * 2 > dest->BytesPerRow) {
            read_ahead = TRUE;
        }

        if (reverse_rows) {
            if (!constant) {
                input += (ULONG)(height - 1) * source_step;
            }

            output += (ULONG)(height - 1) * dest_step;
            source_step = -source_step;
            dest_step = -dest_step;
        }

        if (read_ahead) {
            copyAliasedRows((const UWORD*)input, (UWORD*)output, source_x, dest_x, width, height, source_step,
                dest_step, minterm, reverse_words, FALSE);

            continue;
        }

        for (unsigned row = 0; row < height; ++row) {
            copyRow((const UWORD*)input, (UWORD*)output, source_x, dest_x, width, minterm, reverse_words);

            if (row + 1 < height) {
                if (!constant) {
                    input += source_step;
                }

                output += dest_step;
            }
        }
    }

    if (chipset) {
        DisownBlitter();
    }

    return count;
}

void pcBltClear(APTR memory asm("a1"), ULONG byte_count asm("d0"), ULONG flags asm("d1"),
    struct GfxBase* graphics asm("a6"))
{
    BOOL chipset = pcUsesChipset(graphics);

    // The original acquires internally, even for a zero byte count. Observe
    // both the owning task and its last DMA before writing the destination.
    if (chipset) {
        pcOwnBlitter(graphics);
        pcWaitBlit(graphics);
    }

    if (flags & 2) {
        byte_count = (byte_count >> 16) * (byte_count & 0xffff);
    }

    UWORD pattern = flags & 4 ? flags >> 16 : 0;
    UWORD* dest = memory;

    // The public interface takes an even address and byte count. Completion is
    // synchronous, including when the caller does not request a wait (bit zero).
    for (ULONG words = byte_count >> 1; words; --words) {
        *dest++ = pattern;
    }

    if (chipset) {
        pcDisownBlitter(graphics);
    }
}

// Documented layers.library DoHookClipRects message. Bounds are in the
// selected bitmap; offsets identify the corresponding logical layer point.
struct FillMessage {
    struct Layer* layer;
    struct Rectangle bounds;
    LONG offset_x;
    LONG offset_y;
};

static void setBitmap(struct BitMap* map, UBYTE pen, UBYTE mask, struct GfxBase* graphics)
{
    ULONG stride = map->BytesPerRow;
    ULONG plane_stride = stride;
    BOOL interleaved = FALSE;
	
    // AllocBitMap's extended planar descriptor identifies interleaved planes
    // with this format tag. BytesPerRow is then the distance between scanlines,
    // while adjacent plane pointers delimit the part belonging to one plane.
    if (map->pad == 0x805c) {
        ULONG distance = (ULONG)map->Planes[1] - (ULONG)map->Planes[0];
    	
        if (distance <= stride) {
	        plane_stride = distance; interleaved = TRUE;
        }
    }
	
    BOOL chipset = interleaved && pcUsesChipset(graphics);

    // Planar fills use BltClear's ownership. Interleaved storage has its own
    // native boundary, including calls with no selected planes.
    if (chipset) {
        pcOwnBlitter(graphics);
    }

    for (unsigned plane = 0; plane < map->Depth; ++plane) {
        if (!(mask & (1U << plane))) {
	        continue;
        }
    	
        if (!interleaved) {
            BltClear(map->Planes[plane], ((ULONG)map->Rows << 16) | stride,
                pen & (1U << plane) ? 0xffff0006UL : 2);
        } else {
            UWORD value = pen & (1U << plane) ? 0xffff : 0;

            if (chipset) {
                pcWaitBlit(graphics);
            }
        	
            for (ULONG y = 0; y < map->Rows; ++y) {
                UWORD* row = (UWORD*)(map->Planes[plane] + y * stride);
            	
                for (ULONG word = 0; word < plane_stride / 2; ++word) {
	                row[word] = value;
                }
            }
        }
    }

    if (chipset) {
        pcDisownBlitter(graphics);
    }
}

void pcSetRast(struct RastPort* port asm("a1"), UBYTE pen asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct Layer* layer = port->Layer;
	
    if (!layer) {
	    setBitmap(port->BitMap, pen, port->Mask, graphics); return;
    }
	
    LockLayerRom(layer);
	
    for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        if (clip->obscured) {
            // Obscured backing storage is cleared in full, including its word
            // padding, rather than just the visible clip's logical rectangle.
            if (clip->BitMap) {
	            setBitmap(clip->BitMap, pen, port->Mask, graphics);
            }
        	
            continue;
        }
    	
        const struct Rectangle* bounds = &clip->bounds;
    	
        if (bounds->MaxX < bounds->MinX || bounds->MaxY < bounds->MinY) {
	        continue;
        }
    	
        struct BitMap* map = port->BitMap;

        // The visible-clip path owns and waits even with an empty write mask.
        // Keep the layer locked until the corresponding bitmap writes finish.
        BOOL chipset = pcUsesChipset(graphics);

        if (chipset) {
            pcOwnBlitter(graphics);
            pcWaitBlit(graphics);
        }
    	
        for (unsigned plane = 0; plane < map->Depth; ++plane) {
            if (!(port->Mask & (1U << plane))) {
	            continue;
            }
        	
            UBYTE minterm = pen & (1U << plane) ? 0xf0 : 0;
        	
            for (LONG y = bounds->MinY; y <= bounds->MaxY; ++y) {
                UWORD* row = (UWORD*)(map->Planes[plane] + y * map->BytesPerRow);
                copyRow(NULL, row, 0, bounds->MinX, (LONG)bounds->MaxX - bounds->MinX + 1, minterm, FALSE);
            }
        }

        if (chipset) {
            pcDisownBlitter(graphics);
        }
    }
	
    if (layer->SuperBitMap) {
	    setBitmap(layer->SuperBitMap, pen, port->Mask, graphics);
    }
	
    UnlockLayerRom(layer);
}

struct TemplateData {
    const UWORD* source;
    WORD source_x;
    WORD source_stride;
    WORD dest_x;
    WORD dest_y;
    struct GfxBase* graphics;
};

struct BitmapData {
    const struct BitMap* source;
    struct GfxBase* graphics;
    UWORD* temporary;
    WORD source_x;
    WORD source_y;
    WORD dest_x;
    WORD dest_y;
    UBYTE minterm;
};

static ULONG bitmapRegion(struct Hook* hook asm("a0"), const struct FillMessage* message asm("a1"),
    const struct RastPort* port asm("a2"))
{
    const struct BitmapData* data = hook->h_Data;
    struct GfxBase* graphics = data->graphics;
    const struct Rectangle* bounds = &message->bounds;
	
    // Use the public bitmap vector, retaining installed RTG implementations.
    WORD source_x = data->source_x, source_y = data->source_y;
	
    if (message->layer) {
        source_x += message->offset_x - data->dest_x;
        source_y += message->offset_y - data->dest_y;
    }
	
    BltBitMap(data->source, source_x, source_y, port->BitMap, bounds->MinX, bounds->MinY,
        (LONG)bounds->MaxX - bounds->MinX + 1, (LONG)bounds->MaxY - bounds->MinY + 1,
        data->minterm, port->Mask, (APTR)data->temporary);
	
    return 0;
}

static void clipBitmapToPort(const struct BitMap* source, WORD source_x, WORD source_y,
    struct RastPort* dest, WORD dest_x, WORD dest_y, WORD width, WORD height, UBYTE minterm,
    UWORD* temporary, struct GfxBase* graphics)
{
    struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
    struct BitmapData data = {source, graphics, temporary, source_x, source_y, dest_x, dest_y, minterm};
    
	struct Hook hook = {0};
    hook.h_Entry = (ULONG (*)())bitmapRegion;
    hook.h_Data = &data;
	
    struct Rectangle bounds = {dest_x, dest_y, dest_x + width - 1, dest_y + height - 1};
	
    DoHookClipRects(&hook, dest, &bounds);
}

static BOOL intersectRect(const struct Rectangle* a, const struct Rectangle* b, struct Rectangle* result)
{
    result->MinX = a->MinX > b->MinX ? a->MinX : b->MinX;
    result->MinY = a->MinY > b->MinY ? a->MinY : b->MinY;
    result->MaxX = a->MaxX < b->MaxX ? a->MaxX : b->MaxX;
    result->MaxY = a->MaxY < b->MaxY ? a->MaxY : b->MaxY;
	
    return result->MinX <= result->MaxX && result->MinY <= result->MaxY;
}

// Both layers are already locked by ClipBlit, or by the caller of the public
// superbitmap synchronization functions. Keep one scratch allocation for the
// entire operation, including every source clip and destination hook call.
static void clipPort(struct RastPort* source, WORD source_x, WORD source_y, struct RastPort* dest,
    WORD dest_x, WORD dest_y, WORD width, WORD height, UBYTE minterm, struct GfxBase* graphics)
{
    UWORD* temporary = NULL;
	
    if (source->TmpRas && (ULONG)source->TmpRas->Size >= 4096) {
	    temporary = (APTR)source->TmpRas->RasPtr;
    }
	
    BOOL borrowed = temporary != NULL;
	
    if (!borrowed) {
	    temporary = pcAllocScratch(graphics, 4096, MEMF_PUBLIC | MEMF_CHIP);
    }
	
    struct Layer* layer = source->Layer;
	
    if (!layer) {
        clipBitmapToPort(source->BitMap, source_x, source_y, dest, dest_x, dest_y, width, height, minterm, temporary, graphics);
    } else {
        struct Rectangle logical = {source_x, source_y, source_x + width - 1, source_y + height - 1};
        struct Rectangle target = {dest_x, dest_y, dest_x + width - 1, dest_y + height - 1};
        struct Rectangle intersection;
    	
        if (source == dest && intersectRect(&logical, &target, &intersection)) {
            struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
            SortLayerCR(layer, source_x - dest_x, source_y - dest_y);
        }
    	
        WORD shift_x = layer->bounds.MinX - layer->Scroll_X;
        WORD shift_y = layer->bounds.MinY - layer->Scroll_Y;
    	
        struct Rectangle screen = {logical.MinX + shift_x, logical.MinY + shift_y,
            logical.MaxX + shift_x, logical.MaxY + shift_y};
    	
        for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
            if (!intersectRect(&screen, &clip->bounds, &intersection)) {
	            continue;
            }
        	
            const struct BitMap* map = clip->obscured ? clip->BitMap : source->BitMap;
        	
            if (!map) {
	            continue;
            }
        	
            WORD sx = intersection.MinX, sy = intersection.MinY;
        	
            if (clip->obscured) {
	            sx -= clip->bounds.MinX & ~15;
            	sy -= clip->bounds.MinY;
            }
        	
            clipBitmapToPort(map, sx, sy, dest,
                dest_x + intersection.MinX - screen.MinX, dest_y + intersection.MinY - screen.MinY,
                intersection.MaxX - intersection.MinX + 1, intersection.MaxY - intersection.MinY + 1,
                minterm, temporary, graphics);
        }
    	
        if (layer->SuperBitMap) {
            for (const struct ClipRect* clip = layer->SuperClipRect; clip; clip = clip->Next) {
                if (!intersectRect(&logical, &clip->bounds, &intersection)) {
	                continue;
                }
            	
                clipBitmapToPort(layer->SuperBitMap, intersection.MinX, intersection.MinY, dest,
                    dest_x + intersection.MinX - logical.MinX, dest_y + intersection.MinY - logical.MinY,
                    intersection.MaxX - intersection.MinX + 1, intersection.MaxY - intersection.MinY + 1,
                    minterm, temporary, graphics);
            }
        }
    }
	
    if (!borrowed) {
	    WaitBlit();
    	pcFreeScratch(graphics, temporary, 4096);
    }
}

void pcSyncSBitMap(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct RastPort dest;
    InitRastPort(&dest);
    struct BitMap* bitmap = layer->SuperBitMap;
    dest.BitMap = bitmap;
    layer->SuperBitMap = NULL;
    clipPort(layer->rp, 0, 0, &dest, 0, 0, bitmap->BytesPerRow * 8, bitmap->Rows, 0xca, graphics);
    layer->SuperBitMap = bitmap;
}

void pcCopySBitMap(struct Layer* layer asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct BitMap* bitmap = layer->SuperBitMap;
    layer->SuperBitMap = NULL;
    clipBitmapToPort(bitmap, 0, 0, layer->rp, 0, 0, bitmap->BytesPerRow * 8, bitmap->Rows, 0xca, NULL, graphics);
    layer->SuperBitMap = bitmap;
}

void pcClipBlit(struct RastPort* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* dest asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Layer* source_layer = source->Layer;
    struct Layer* dest_layer = dest->Layer;
    struct Layer_Info* info = NULL;
	
    if (source_layer && dest_layer && source_layer != dest_layer &&
        source_layer->LayerInfo == dest_layer->LayerInfo) {
        info = source_layer->LayerInfo;
        ObtainSemaphore(&info->Lock);
    }
	
    if (source_layer) {
	    ObtainSemaphore(&source_layer->Lock);
    }
	
    if (dest_layer) {
	    ObtainSemaphore(&dest_layer->Lock);
    }
	
    if (info) {
	    ReleaseSemaphore(&info->Lock);
    }
	
    if (source == dest && source_layer && source_layer->SuperBitMap) {
        SyncSBitMap(source_layer);
        struct BitMap* bitmap = source->BitMap;
        source->Layer = NULL;
        source->BitMap = source_layer->SuperBitMap;
        clipPort(source, source_x, source_y, dest, dest_x, dest_y, width, height, minterm, graphics);
        source->Layer = source_layer;
        source->BitMap = bitmap;
        CopySBitMap(source_layer);
    } else {
        clipPort(source, source_x, source_y, dest, dest_x, dest_y, width, height, minterm, graphics);
    }
	
    if (source_layer) {
	    ReleaseSemaphore(&source_layer->Lock);
    }
	
    if (dest_layer) {
	    ReleaseSemaphore(&dest_layer->Lock);
    }
}

void pcBltBitMapRastPort(const struct BitMap* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), struct GfxBase* graphics asm("a6"))
{
    if (width <= 0 || height <= 0) {
	    return;
    }
	
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Layer* layer = port->Layer;
	
    if (layer) {
	    ObtainSemaphore(&layer->Lock);
    }
    
	UWORD* temporary = NULL;
	
    if (port->TmpRas && (ULONG)port->TmpRas->Size >= 4096) {
	    temporary = (APTR)port->TmpRas->RasPtr;
    }
	
    BOOL borrowed = temporary != NULL;
	
    if (!borrowed) {
	    temporary = pcAllocScratch(graphics, 4096, MEMF_PUBLIC | MEMF_CHIP);
    }
	
    if (!layer) {
        BltBitMap(source, source_x, source_y, port->BitMap, dest_x, dest_y, width, height, minterm, port->Mask, (APTR)temporary);
    } else {
        struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
        struct BitmapData data = {source, graphics, temporary, source_x, source_y, dest_x, dest_y, minterm};
        struct Hook hook = {0};
        hook.h_Entry = (ULONG (*)())bitmapRegion;
        hook.h_Data = &data;
        struct Rectangle bounds = {dest_x, dest_y, (WORD)(dest_x + width - 1), (WORD)(dest_y + height - 1)};
        DoHookClipRects(&hook, port, &bounds);
    }
	
    if (!borrowed) {
        WaitBlit();
        pcFreeScratch(graphics, temporary, 4096);
    }
	
    if (layer) {
	    ReleaseSemaphore(&layer->Lock);
    }
}

struct MaskBitmapData {
    struct BitmapData bitmap;
    // The public wrapper accepts up to 32 planes from GetBitMapAttr, including
    // descriptors supplied by an installed RTG owner.
    struct BitMap mask;
    PLANEPTR extra_planes[24];
};

static ULONG maskBitmapRegion(struct Hook* hook asm("a0"), const struct FillMessage* message asm("a1"),
    const struct RastPort* port asm("a2"))
{
    const struct MaskBitmapData* mask_data = hook->h_Data;
    const struct BitmapData* data = &mask_data->bitmap;
    struct GfxBase* graphics = data->graphics;
    const struct Rectangle* bounds = &message->bounds;
    WORD source_x = data->source_x, source_y = data->source_y;
	
    if (message->layer) {
        source_x += message->offset_x - data->dest_x;
        source_y += message->offset_y - data->dest_y;
    }
	
    UWORD width = bounds->MaxX - bounds->MinX + 1;
    UWORD height = bounds->MaxY - bounds->MinY + 1;
	
    // Preserve all three public calls: P96 can own BltBitMap independently of
    // this vector. The caller's minterm applies to the repeated mask planes
    // between two source/destination equivalence operations.
    BltBitMap(data->source, source_x, source_y, port->BitMap, bounds->MinX, bounds->MinY,
        width, height, 0x99, port->Mask, (APTR)data->temporary);
    BltBitMap(&mask_data->mask, source_x, source_y, port->BitMap, bounds->MinX, bounds->MinY,
        width, height, data->minterm, port->Mask, (APTR)data->temporary);
    BltBitMap(data->source, source_x, source_y, port->BitMap, bounds->MinX, bounds->MinY,
        width, height, 0x99, port->Mask, (APTR)data->temporary);
    return 0;
}

void pcBltMaskBitMapRastPort(const struct BitMap* source asm("a0"), WORD source_x asm("d0"), WORD source_y asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), UBYTE minterm asm("d6"), PLANEPTR mask asm("a2"),
    struct GfxBase* graphics asm("a6"))
{
    if (width <= 0 || height <= 0 || !source || !port) {
	    return;
    }
	
    ULONG source_width = GetBitMapAttr(source, BMA_WIDTH);
    ULONG source_height = GetBitMapAttr(source, BMA_HEIGHT);
    ULONG depth = GetBitMapAttr(source, BMA_DEPTH);
	
    if (depth >= 32) {
	    depth = 32;
    }
	
    struct MaskBitmapData data = {0};
    InitBitMap(&data.mask, depth, source_width, source_height);
	
    for (ULONG plane = 0; plane < depth; ++plane) {
        if (plane < 8) {
	        data.mask.Planes[plane] = mask;
        }
        else {
	        data.extra_planes[plane - 8] = mask;
        }
    }
	
    UWORD* temporary = NULL;
	
    if (port->TmpRas && (ULONG)port->TmpRas->Size >= 4096) {
	    temporary = (APTR)port->TmpRas->RasPtr;
    }
	
    BOOL borrowed = temporary != NULL;
	
    if (!borrowed) {
	    temporary = pcAllocScratch(graphics, 4096, MEMF_PUBLIC | MEMF_CHIP);
    }
	
    data.bitmap = (struct BitmapData){source, graphics, temporary, source_x, source_y, dest_x, dest_y, minterm};
    
	struct Hook hook = {0};
    hook.h_Entry = (ULONG (*)())maskBitmapRegion;
    hook.h_Data = &data;
	
    struct Rectangle bounds = {dest_x, dest_y, dest_x + width - 1, dest_y + height - 1};
    struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
	
    DoHookClipRects(&hook, port, &bounds);
	
    if (!borrowed) {
	    WaitBlit();
    	pcFreeScratch(graphics, temporary, 4096);
    }
}

static void drawTemplate(const UWORD* source, LONG source_x, LONG source_y,
    const struct RastPort* port, UWORD dest_x, UWORD dest_y, UWORD width, UWORD height, LONG source_stride,
    struct GfxBase* graphics)
{
    struct BitMap* dest = port->BitMap;

    if (!width || !height) {
        return;
    }

    // The original acquires for every nonempty bitmap, including a zero
    // plane mask. Each clipped rectangle owns and waits independently.
    BOOL chipset = dest->Depth && pcUsesChipset(graphics);

    if (chipset) {
        OwnBlitter();
        pcWaitBlit(graphics);
    }

    // Text anchors its scratch raster at logical x=0 and passes a negative
    // left extent for kerned or italic glyphs. Normalize that signed coordinate
    // before the unsigned word extraction loop.
    source = (const UWORD*)((const UBYTE*)source + (LONG)source_y * source_stride + ((LONG)source_x >> 4) * 2);
    source_x &= 15;

    // Template setup chooses one traversal from the first destination plane,
    // then retains it for every selected plane, including shared storage.
    ULONG first_source = (ULONG)source;
    ULONG first_dest = (ULONG)dest->Planes[0] + (ULONG)dest_y * dest->BytesPerRow + (dest_x >> 4) * 2;
    BOOL reverse_rows = FALSE;
    BOOL reverse_words = FALSE;

    if (source_stride != dest->BytesPerRow) {
        reverse_rows = (dest_x & 15) >= source_x;
        reverse_words = !reverse_rows;
    } else if (first_dest >= first_source) {
        ULONG last_word = first_source + ((source_x + width - 1) >> 4) * 2;

        reverse_rows = (dest_x & 15) <= source_x || first_dest > last_word;
        reverse_words = (dest_x & 15) <= source_x || first_dest <= last_word;
    } else {
        ULONG last_word = first_dest + (((dest_x & 15) + width - 1) >> 4) * 2;

        reverse_words = (dest_x & 15) < source_x && last_word < first_source;
    }

    // Reverse-row setup uses an unsigned WORD product for the starting row,
    // then the signed row step. Preserve that address even for disjoint data,
    // where the CPU loop can otherwise keep its forward traversal.
    if (reverse_rows && source_stride < 0) {
        ULONG row_bias = (ULONG)(UWORD)source_stride - (ULONG)source_stride;

        source = (const UWORD*)((const UBYTE*)source + (height - 1) * row_bias);
    }

    for (unsigned plane = 0; plane < dest->Depth; ++plane) {
        if (!(port->Mask & (1UL << plane))) {
            continue;
        }

        UBYTE minterm = port->minterms[plane] & 0xf0;

        if (minterm == 0xa0) {
            continue;
        }

        const UBYTE* input = (const UBYTE*)source;
        UBYTE* output = dest->Planes[plane] + (ULONG)dest_y * dest->BytesPerRow;

        ULONG source_start = (ULONG)input;
        ULONG source_end = source_start + (LONG)(height - 1) * source_stride;

        if (source_stride < 0) {
            ULONG swap = source_start;

            source_start = source_end;
            source_end = swap;
        }

        source_end += ((source_x + width - 1) >> 4) * 2;

        ULONG dest_start = (ULONG)output + (dest_x >> 4) * 2;
        ULONG dest_end = (ULONG)output + (ULONG)(height - 1) * dest->BytesPerRow + ((dest_x + width - 1) >> 4) * 2;

        // Destination rows can share words even with a separate stencil.
        // Preserve their traversal and destination read order as for aliases.
        ULONG dest_words = (((dest_x & 15) + width - 1) >> 4) + 1;
        ULONG source_words = ((source_x + width - 1) >> 4) + 1;
        ULONG copy_words = source_words > dest_words ? source_words : dest_words;
        BOOL shared_rows = height > 1 && copy_words * 2 > dest->BytesPerRow;

        if ((source_start <= dest_end && dest_start <= source_end) || shared_rows) {
            LONG source_step = source_stride;
            LONG dest_step = dest->BytesPerRow;

            if (reverse_rows) {
                input += (LONG)(height - 1) * source_step;
                output += (LONG)(height - 1) * dest_step;
                source_step = -source_step;
                dest_step = -dest_step;
            }

            copyAliasedRows((const UWORD*)input, (UWORD*)output, source_x, dest_x, width, height, source_step,
                dest_step, minterm, reverse_words, shared_rows && source_words > dest_words);

            continue;
        }

        for (unsigned row = 0; row < height; ++row) {
            copyRow((const UWORD*)input, (UWORD*)output, source_x, dest_x, width, minterm, 0);

            if (row + 1 < height) {
                input += source_stride;
                output += dest->BytesPerRow;
            }
        }
    }

    if (chipset) {
        DisownBlitter();
    }
}

static ULONG templateRegion(struct Hook* hook asm("a0"), const struct FillMessage* message asm("a1"),
    const struct RastPort* port asm("a2"))
{
    const struct TemplateData* data = hook->h_Data;
    const struct Rectangle* bounds = &message->bounds;

    drawTemplate(data->source, data->source_x + message->offset_x - data->dest_x,
        message->offset_y - data->dest_y, port, bounds->MinX, bounds->MinY,
        (LONG)bounds->MaxX - bounds->MinX + 1, (LONG)bounds->MaxY - bounds->MinY + 1,
        data->source_stride, data->graphics);

    return 0;
}

void pcBltTemplate(CONST PLANEPTR source asm("a0"), WORD source_x asm("d0"), WORD source_stride asm("d1"),
    struct RastPort* port asm("a1"), WORD dest_x asm("d2"), WORD dest_y asm("d3"),
    WORD width asm("d4"), WORD height asm("d5"), struct GfxBase* graphics asm("a6"))
{
    if (width <= 0 || height <= 0) {
        return;
    }

    if (!port->Layer) {
        drawTemplate((const UWORD*)source, source_x, 0, port, dest_x, dest_y, width, height, source_stride, graphics);

        return;
    }

    struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
    struct TemplateData data = {(const UWORD*)source, source_x, source_stride, dest_x, dest_y, graphics};

    struct Hook hook = {0};
    hook.h_Entry = (ULONG (*)())templateRegion;
    hook.h_Data = &data;

    struct Rectangle bounds = {dest_x, dest_y, (WORD)(dest_x + width - 1), (WORD)(dest_y + height - 1)};

    DoHookClipRects(&hook, port, &bounds);
}

static UWORD* preparePatternMask(
    const UWORD* mask, UWORD stride, WORD x, WORD width, WORD rows, ULONG* size, struct GfxBase* graphics)
{
    *size = (LONG)(WORD)stride * rows;
    UWORD* prepared = pcAllocScratch(graphics, *size, MEMF_CHIP);

    if (!prepared) {
        return NULL;
    }

    BltClear((PLANEPTR)prepared, *size, 0);

    // Both descriptors use InitBitMap's WORD width rounding. The later mask
    // traversal still uses the caller's pitch, rather than this rounded value.
    struct BitMap source;
    struct BitMap dest;
    InitBitMap(&source, 1, (UWORD)(stride << 3), rows);
    InitBitMap(&dest, 1, (UWORD)(stride << 3), rows);
    source.Planes[0] = (PLANEPTR)mask;
    dest.Planes[0] = (PLANEPTR)prepared;
    BltBitMap(&source, x, 0, &dest, x, 0, width, rows, 0xca, 1, NULL);

    return prepared;
}

// Pattern and mask coordinates are independent: patterns follow the layer's
// logical origin; the stencil follows the caller's requested rectangle.
static void drawPattern(const struct RastPort* port, struct BitMap* dest, const UWORD* mask, UWORD mask_stride,
    const struct Rectangle* bounds, LONG pattern_x, LONG pattern_y, LONG mask_x, LONG mask_y, BOOL clip_mask,
    struct GfxBase* graphics)
{
    if (bounds->MaxX < bounds->MinX || bounds->MaxY < bounds->MinY) {
        return;
    }

    BOOL chipset = pcUsesChipset(graphics);
    ULONG width = (LONG)bounds->MaxX - bounds->MinX + 1;
    ULONG height = (LONG)bounds->MaxY - bounds->MinY + 1;

    unsigned pattern_power = port->AreaPtSz < 0 ? -port->AreaPtSz : port->AreaPtSz;
    ULONG pattern_rows = port->AreaPtrn ? 1UL << pattern_power : 1;
    unsigned rotation = ((ULONG)bounds->MinX - pattern_x) & 15;

    // A direct stencil supplies complete words, including its padding. The
    // original masks its last fetched word according to the supplied pitch;
    // horizontal clipping has separate edge handling. A layer alone does not
    // select that path, and neither does clipping only the vertical bounds.
    BOOL direct_mask = mask && !clip_mask;
    ULONG mask_words = (bounds->MaxX >> 4) - (bounds->MinX >> 4) + 1;
    ULONG prepared_size = 0;
    UWORD* prepared = NULL;
    LONG mask_shift = (bounds->MinX & 15) - (mask_x & 15);

    if (mask && clip_mask) {
        ULONG source_words = (((mask_x & 15) + width - 1) >> 4) + 1;
        ULONG copy_words = source_words > mask_words ? source_words : mask_words;
        BOOL prepare = mask_shift < 0;

        if (copy_words * 2 == mask_stride) {
            prepare |= width < mask_stride * 8UL;
        } else {
            prepare |= mask_stride != 2;
        }

        if (prepare) {
            prepared = preparePatternMask(mask, mask_stride, mask_x, width, mask_y + height, &prepared_size, graphics);

            if (!prepared) {
                return;
            }

            mask = prepared;
            mask_words = copy_words;
        }
    }

    // Preparation may call the public bitmap vectors, which take ownership
    // themselves. Acquire for drawing only after that transfer has been issued.
    // Even an empty plane mask retains this ownership/wait boundary.
    if (chipset) {
        pcOwnBlitter(graphics);
        pcWaitBlit(graphics);
    }

    BOOL reverse_mask = prepared && mask_shift < 0;
    BOOL word_mask = direct_mask || prepared;
    ULONG row_step = word_mask ? pattern_rows : 1;
    ULONG phases = row_step < height ? row_step : height;
    UWORD last_mask = 0;
    LONG mask_step = mask_stride;

    if (direct_mask) {
        if (mask_words * 2 == mask_stride) {
            last_mask = 0xffffUL << (15 - ((width - 1) & 15));
        }

        // Each pattern phase starts at an unsigned row offset, then repeats
        // through the original signed-WORD row displacement.
        mask_step = (WORD)(mask_stride * row_step - mask_words * 2) + mask_words * 2;
    } else if (prepared) {
        last_mask = reverse_mask || mask_stride >= mask_words * 2 ? 0xffff : 0;
        mask_step = reverse_mask ? -(WORD)(-mask_stride * row_step - mask_words * 2) - mask_words * 2
                                 : (WORD)(mask_stride * row_step - mask_words * 2) + mask_words * 2;
    }

    const UWORD* next_pattern = port->AreaPtrn;

    for (unsigned plane = 0; plane < dest->Depth; ++plane) {
        if (!(port->Mask & (1U << plane))) {
            continue;
        }

        // Per-plane patterns follow the enabled planes, including a plane
        // whose minterm leaves its destination unchanged.
        const UWORD* pattern = next_pattern;

        if (pattern && port->AreaPtSz < 0) {
            next_pattern += pattern_rows;
        }

        UBYTE minterm = port->minterms[plane] & 0xf0;

        if (minterm == 0xa0) {
            continue;
        }

        for (ULONG phase = 0; phase < phases; ++phase) {
            UBYTE* output = dest->Planes[plane] + ((LONG)bounds->MinY + phase) * dest->BytesPerRow;
            const UBYTE* input = (const UBYTE*)mask;
            UWORD carry = 0;

            if (input) {
                // Vertical clipping uses a signed pitch before the unsigned
                // pattern-phase offset is added to the direct stencil.
                LONG clip_stride = word_mask ? (WORD)mask_stride : (LONG)mask_stride;

                input += mask_y * clip_stride;
                input += phase * mask_stride;

                if (prepared) {
                    input += (mask_x >> 4) * 2;
                }
            }

            for (ULONG y = phase; y < height; y += row_step) {
                UWORD bits = pattern ? pattern[((ULONG)pattern_y + y) & (pattern_rows - 1)] : 0xffff;

                if (rotation) {
                    bits = (bits >> rotation) | (bits << (16 - rotation));
                }

                ULONG remaining = word_mask ? mask_words * 16 : width;
                LONG x = word_mask ? bounds->MinX & ~15 : bounds->MinX;
                const UWORD* stencil = (const UWORD*)input;

                if (reverse_mask) {
                    x += (mask_words - 1) * 16;
                    stencil += mask_words;
                }

                while (remaining) {
                    unsigned shift = x & 15;
                    unsigned count = 16 - shift;

                    if (count > remaining) {
                        count = remaining;
                    }

                    UWORD selected = (0xffffUL >> (16 - count)) << (16 - shift - count);

                    if (prepared) {
                        UWORD next = reverse_mask ? *--stencil : *stencil++;

                        if (remaining == 16) {
                            next &= last_mask;
                        }

                        if (reverse_mask) {
                            unsigned offset = -mask_shift;
                            selected = (next << offset) | (carry >> (16 - offset));
                        } else {
                            selected = mask_shift ? (carry << (16 - mask_shift)) | (next >> mask_shift) : next;
                        }

                        carry = next;
                    } else if (direct_mask) {
                        UWORD next = *stencil++;
                        unsigned offset = bounds->MinX & 15;

                        if (remaining == 16) {
                            next &= last_mask;
                        }

                        // Preserve the previous word across rows of one phase:
                        // nonzero stencil padding can also select left-edge pixels.
                        selected = offset ? (carry << (16 - offset)) | (next >> offset) : next;
                        carry = next;
                    } else if (input) {
                        selected &= sourceBits((const UWORD*)input, mask_x + width - remaining, count) >> shift;
                    }

                    UWORD* word = (UWORD*)output + (x >> 4);
                    UWORD old = *word;
                    *word = (old & ~selected) | (combine(bits, old, minterm) & selected);
                    x += reverse_mask ? -(LONG)count : (LONG)count;
                    remaining -= count;
                }

                if (y + row_step < height) {
                    output += row_step * dest->BytesPerRow;

                    if (input) {
                        input += mask_step;
                    }
                }
            }
        }
    }

    if (chipset) {
        pcDisownBlitter(graphics);
    }

    if (prepared) {
        pcFreeScratch(graphics, prepared, prepared_size);
    }
}

static ULONG fillRegion(struct Hook* hook asm("a0"), const struct FillMessage* message asm("a1"),
    const struct RastPort* port asm("a2"))
{
    struct GfxBase* graphics = hook->h_Data;

    drawPattern(
        port, port->BitMap, NULL, 0, &message->bounds, message->offset_x, message->offset_y, 0, 0, FALSE, graphics);

    return 0;
}

void pcBltPattern(struct RastPort* port asm("a1"), CONST PLANEPTR mask asm("a0"),
    WORD x_min asm("d0"), WORD y_min asm("d1"), WORD x_max asm("d2"), WORD y_max asm("d3"),
    UWORD mask_stride asm("d4"), struct GfxBase* graphics asm("a6"))
{
    if (x_max < x_min || y_max < y_min) {
        return;
    }

    struct Rectangle bounds = {x_min, y_min, x_max, y_max};
    struct Layer* layer = port->Layer;

    if (!layer) {
        drawPattern(port, port->BitMap, (const UWORD*)mask, mask_stride, &bounds, x_min, y_min, 0, 0, FALSE, graphics);

        return;
    }

    if (!mask && !port->AreaPtrn) {
        struct Library* LayersBase = (APTR)graphics->gb_LayersBase;

        struct Hook hook = {0};
        hook.h_Entry = (ULONG (*)())fillRegion;
        hook.h_Data = graphics;

        DoHookClipRects(&hook, port, &bounds);

        return;
    }

    // The pattern path updates the complete requested superbitmap rectangle,
    // including its visible pixels. DoHookClipRects only visits SuperClipRect,
    // so it cannot replace this traversal without changing those pixels.
    // Read the SDK layer/clip structures under their normal semaphore; never
    // modify the layer, its lists or the caller's RastPort while selecting storage.
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(&layer->Lock);

    WORD offset_x = layer->bounds.MinX - layer->Scroll_X;
    WORD offset_y = layer->bounds.MinY - layer->Scroll_Y;
    struct Rectangle screen = {(WORD)(x_min + offset_x), (WORD)(y_min + offset_y),
        (WORD)(x_max + offset_x), (WORD)(y_max + offset_y)};

    for (const struct ClipRect* clip = layer->ClipRect; clip; clip = clip->Next) {
        struct Rectangle selected = screen;
        if (selected.MinX < clip->bounds.MinX) {
            selected.MinX = clip->bounds.MinX;
        }
        if (selected.MinY < clip->bounds.MinY) {
            selected.MinY = clip->bounds.MinY;
        }
        if (selected.MaxX > clip->bounds.MaxX) {
            selected.MaxX = clip->bounds.MaxX;
        }
        if (selected.MaxY > clip->bounds.MaxY) {
            selected.MaxY = clip->bounds.MaxY;
        }
        if (selected.MaxX < selected.MinX || selected.MaxY < selected.MinY) {
            continue;
        }

        BOOL clip_mask = selected.MinX != screen.MinX || selected.MaxX != screen.MaxX;

        LONG pattern_x = (WORD)(selected.MinX - offset_x);
        LONG pattern_y = (WORD)(selected.MinY - offset_y);
        LONG mask_x = (WORD)(selected.MinX - screen.MinX);
        LONG mask_y = (WORD)(selected.MinY - screen.MinY);
        struct BitMap* dest = port->BitMap;

        if (clip->obscured) {
            if (!clip->BitMap) {
                continue;
            }

            dest = clip->BitMap;
            WORD aligned_x = clip->bounds.MinX & ~15;
            selected.MinX -= aligned_x;
            selected.MaxX -= aligned_x;
            selected.MinY -= clip->bounds.MinY;
            selected.MaxY -= clip->bounds.MinY;
        }

        drawPattern(port, dest, (const UWORD*)mask, mask_stride, &selected, pattern_x, pattern_y, mask_x, mask_y,
            clip_mask, graphics);
    }

    if (layer->SuperBitMap) {
        drawPattern(
            port, layer->SuperBitMap, (const UWORD*)mask, mask_stride, &bounds, x_min, y_min, 0, 0, FALSE, graphics);
    }

    ReleaseSemaphore(&layer->Lock);
}

void pcRectFill(struct RastPort* port asm("a1"), WORD x_min asm("d0"), WORD y_min asm("d1"),
    WORD x_max asm("d2"), WORD y_max asm("d3"), struct GfxBase* graphics asm("a6"))
{
    if (!(port->Flags & AREAOUTLINE)) {
        BltPattern(port, NULL, x_min, y_min, x_max, y_max, 0);
        return;
    }
    if (port->DrawMode & COMPLEMENT) {
        WORD left = x_min + 1, top = y_min + 1, right = x_max - 1, bottom = y_max - 1;
        if (right >= left && bottom >= top) { BltPattern(port, NULL, left, top, right, bottom, 0); }
    } else {
        BltPattern(port, NULL, x_min, y_min, x_max, y_max, 0);
    }
    UBYTE foreground = port->FgPen, mode = port->DrawMode;
    UBYTE minterms[8];
    for (unsigned i = 0; i < 8; ++i) { minterms[i] = port->minterms[i]; }
    port->FgPen = port->AOlPen;
    port->DrawMode = JAM1;
    pcUpdateMinterms(port);
    // The outline is ordinary line drawing, with its usual pen movement and
    // pattern progression. Retain public-vector dispatch for installed owners.
    Move(port, x_min, y_min);
    Draw(port, x_min, y_max);
    Draw(port, x_max, y_max);
    Draw(port, x_max, y_min);
    Draw(port, x_min, y_min);
    port->FgPen = foreground;
    port->DrawMode = mode;
    for (unsigned i = 0; i < 8; ++i) { port->minterms[i] = minterms[i]; }
}

static void __attribute__((used, noinline)) eraseRect(struct RastPort* port asm("a1"),
    ULONG x_min asm("d0"), ULONG y_min asm("d1"), ULONG x_max asm("d2"), ULONG y_max asm("d3"),
    struct GfxBase* graphics asm("a6"))
{
    struct Layer* layer = port->Layer;
    if (layer) {
        LockLayerRom(layer);
        struct Library* LayersBase = (APTR)graphics->gb_LayersBase;
        struct Rectangle bounds = {x_min, y_min, x_max, y_max};
        DoHookClipRects(layer->BackFill, port, &bounds);
        UnlockLayerRom(layer);
    } else {
        // Keep the original full-register arithmetic at the public ClipBlit
        // boundary. The SDK inline would truncate the extent to a signed WORD.
        register struct RastPort* source asm("a0") = port;
        register struct RastPort* dest asm("a1") = port;
        register ULONG source_x asm("d0") = x_min;
        register ULONG source_y asm("d1") = y_min;
        register ULONG dest_x asm("d2") = x_min;
        register ULONG dest_y asm("d3") = y_min;
        register ULONG width asm("d4") = x_max - x_min + 1;
        register ULONG height asm("d5") = y_max - y_min + 1;
        register ULONG minterm asm("d6") = 0;
        __asm__ volatile ("jsr -552(a6)"
            : "+a"(source), "+a"(dest), "+d"(source_x), "+d"(source_y)
            : "a"(graphics), "d"(dest_x), "d"(dest_y), "d"(width), "d"(height), "d"(minterm)
            : "fp0", "fp1", "cc", "memory");
    }
}

// Preserve the SDK declaration while retaining all incoming coordinate bits
// for installed ClipBlit owners, as the original public entry does.
__asm__(".globl _pcEraseRect\n.set _pcEraseRect,_eraseRect\n");

// Each scaled axis advances between rounded pixel boundaries. Keeping this
// error local avoids division per pixel and any dependency on a ROM workspace.
struct ScaleAxis {
    LONG error;
    UWORD increment;
    UWORD decrement;
};

static struct ScaleAxis scaleAxis(UWORD source, UWORD dest)
{
    UWORD small = source < dest ? source : dest;
    UWORD large = source < dest ? dest : source;
    struct ScaleAxis axis = {small - 2L * large, 2 * small, 2 * large};
    return axis;
}

static UWORD scaleStep(struct ScaleAxis* axis)
{
    UWORD count = 0;
    do {
        ++count;
        axis->error += axis->increment;
    } while (axis->error <= 0);
    axis->error -= axis->decrement;
    return count;
}

static UWORD scaleRowWords(UWORD x, UWORD width)
{
    // The inclusive row endpoint wraps before rounding to whole words.
    UWORD last = (x & 15) + width - 1;

    return (last >> 4) + 1;
}

static void scaleCopyRows(const UWORD* source, UWORD* dest, UWORD source_x, UWORD dest_x, UWORD width, UWORD rows,
    UWORD source_stride, UWORD dest_stride)
{
    // Scaling's original row copies encode width and height in one 16-bit
    // extent. Preserve the resulting pixels and row steps when either field
    // wraps, while the caller retains its full reported dimensions.
    unsigned words = scaleRowWords(dest_x, width);
    UWORD extent = words | (rows << 6);
    unsigned columns = extent & 63;
    unsigned copies = extent >> 6;

    if (!columns) {
        columns = 64;
    }

    if (!copies) {
        copies = 1024;
    }

    LONG copied_width = (LONG)columns * 16 - (dest_x & 15) - ((-(dest_x + width)) & 15);
    LONG source_step = (WORD)(source_stride - words * 2) + (LONG)columns * 2;
    LONG dest_step = (WORD)(dest_stride - words * 2) + (LONG)columns * 2;

    if (copied_width <= 0) {
        return;
    }

    for (unsigned row = 0; row < copies; ++row) {
        copyRow(source, dest, source_x, dest_x, copied_width, 0xc0, 0);
        source = (const UWORD*)((const UBYTE*)source + source_step);
        dest = (UWORD*)((UBYTE*)dest + dest_step);
    }
}

static void scaleRow(struct BitScaleArgs* args, const UWORD* source, UWORD* dest)
{
    if (args->bsa_XSrcFactor == args->bsa_XDestFactor) {
        scaleCopyRows(source, dest, args->bsa_SrcX, args->bsa_DestX, args->bsa_SrcWidth, 1,
            args->bsa_SrcBitMap->BytesPerRow, args->bsa_DestBitMap->BytesPerRow);
        args->bsa_DestWidth = args->bsa_SrcWidth;
        args->bsa_XDDA = 0;
        return;
    }
    BOOL shrink = args->bsa_XDestFactor < args->bsa_XSrcFactor;
    struct ScaleAxis axis = scaleAxis(args->bsa_XSrcFactor, args->bsa_XDestFactor);
    UWORD bits = 0, mask = 0, bit = 0x8000U >> (args->bsa_DestX & 15);
    ULONG output = 0;
    dest += args->bsa_DestX >> 4;
    // The original reduction loop tests a signed WORD counter after its
    // first output pixel. Larger extents stop there, rather than consuming
    // the complete unsigned width recorded in BitScaleArgs.
    ULONG source_width = shrink && args->bsa_SrcWidth > 32768 ? 1 : args->bsa_SrcWidth;

    for (ULONG x = 0; x < source_width; ++x) {
        if (shrink && x) {
            axis.error += axis.increment;
            if (axis.error <= 0) { continue; }
            axis.error -= axis.decrement;
        }
        ULONG source_x = args->bsa_SrcX + x;
        BOOL set = (source[source_x >> 4] & (0x8000U >> (source_x & 15))) != 0;
        UWORD count = shrink ? 1 : scaleStep(&axis);
        output += count;
        do {
            mask |= bit;
            if (set) { bits |= bit; }
            bit >>= 1;
            if (!bit) {
                *dest = (*dest & ~mask) | bits;
                ++dest;
                mask = bits = 0;
                bit = 0x8000;
            }
        } while (--count);
    }
    if (mask) { *dest = (*dest & ~mask) | bits; }
    args->bsa_DestWidth = output;
    args->bsa_XDDA = axis.error;
}

void pcBitMapScale(struct BitScaleArgs* args asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (args->bsa_Flags & ~3UL) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        Alert(0x02080000);
        args->bsa_DestWidth = args->bsa_DestHeight = 0;

        return;
    }

    if (!args->bsa_SrcWidth || !args->bsa_SrcHeight) {
        args->bsa_DestWidth = args->bsa_DestHeight = 0;

        return;
    }

    // The low flag bits describe reduction. Equal factors leave them alone;
    // callers should initialize Flags to zero, as specified by the public API.
    if (args->bsa_XSrcFactor != args->bsa_XDestFactor) {
        args->bsa_Flags = (args->bsa_Flags & ~1UL) | (args->bsa_XDestFactor < args->bsa_XSrcFactor);
    }

    BOOL equal_y = args->bsa_YSrcFactor == args->bsa_YDestFactor;
    BOOL shrink_y = args->bsa_YDestFactor < args->bsa_YSrcFactor;

    if (equal_y) {
        args->bsa_YDDA = 0;
    } else {
        args->bsa_Flags = (args->bsa_Flags & ~2UL) | (shrink_y ? 2 : 0);
    }

    const struct BitMap* source = args->bsa_SrcBitMap;
    struct BitMap* dest = args->bsa_DestBitMap;
    unsigned depth = source->Depth < dest->Depth ? source->Depth : dest->Depth;
    BOOL chipset = depth && pcUsesChipset(graphics);
    BOOL chipset_copy = chipset && args->bsa_XSrcFactor == args->bsa_XDestFactor;

    for (unsigned plane = 0; plane < depth; ++plane) {
        struct ScaleAxis axis = scaleAxis(args->bsa_YSrcFactor, args->bsa_YDestFactor);
        ULONG source_y = 0;
        ULONG dest_y = 0;
        UWORD last_count = 1;
        WORD remaining_rows = args->bsa_SrcHeight;
        const UWORD* source_row = (const UWORD*)(source->Planes[plane] + (ULONG)args->bsa_SrcY * source->BytesPerRow);
        UBYTE* dest_row = dest->Planes[plane] + (ULONG)args->bsa_DestY * dest->BytesPerRow;

        while (source_y < args->bsa_SrcHeight) {
            // Equal-width copies observe both preceding DMA and task ownership.
            // Unequal-width CPU scaling retains caller-managed ordering.
            if (chipset_copy) {
                OwnBlitter();
                WaitBlit();
            }

            scaleRow(args, source_row, (UWORD*)dest_row);

            if (chipset_copy) {
                DisownBlitter();
            }

            UWORD count = equal_y ? 1 : scaleStep(&axis);
            last_count = count;

            if (!shrink_y) {
                // Row duplication uses the original's blitter ordering too.
                // Wait after scaling the first row, before reading it back;
                // unequal-width scaling itself does not wait for prior DMA.
                if (chipset && count > 1) {
                    OwnBlitter();
                    WaitBlit();
                }

                if (count > 1) {
                    scaleCopyRows((const UWORD*)dest_row, (UWORD*)(dest_row + (WORD)dest->BytesPerRow), args->bsa_DestX,
                        args->bsa_DestX, args->bsa_DestWidth, count - 1, 0, dest->BytesPerRow);
                }

                if (chipset && count > 1) {
                    DisownBlitter();
                }

                ++source_y;
                dest_y += count;
            } else {
                source_y += count;
                ++dest_y;
            }

            // Reduction tests the stored WORD remainder; equal/enlarged
            // axes instead stop when their signed counter cannot decrement
            // to a positive value. Keep this separate from reported height.
            if (shrink_y) {
                remaining_rows = (WORD)(remaining_rows - count);

                if (remaining_rows <= 0) {
                    break;
                }
            } else {
                if (remaining_rows <= 1) {
                    break;
                }

                --remaining_rows;
            }

            // Single-row advances use a signed stride. Skipping or duplicating
            // multiple rows instead multiplies the unsigned BytesPerRow.
            LONG source_step = shrink_y && count > 1 ? (LONG)count * source->BytesPerRow : (WORD)source->BytesPerRow;
            LONG dest_step = !shrink_y && count > 1 ? (LONG)count * dest->BytesPerRow : (WORD)dest->BytesPerRow;

            source_row = (const UWORD*)((const UBYTE*)source_row + source_step);
            dest_row += dest_step;
        }

        args->bsa_DestHeight = equal_y ? args->bsa_SrcHeight : dest_y;

        if (!equal_y) {
            args->bsa_YDDA = axis.error;

            if (!shrink_y && last_count > 1) {
                // The original reports the final row-copy extent in YDDA
                // when enlargement ends with duplicated rows, not the error.
                UWORD words = scaleRowWords(args->bsa_DestX, args->bsa_DestWidth);
                args->bsa_YDDA = words | ((last_count - 1) << 6);
            }
        }
    }
}

UWORD pcScalerDiv(UWORD factor asm("d0"), UWORD numerator asm("d1"), UWORD denominator asm("d2"))
{
    if (!denominator) {
        return 0;
    }

    ULONG product = (ULONG)factor * numerator;

    // The original word division retains the product on quotient overflow.
    // Within the documented factor range its rounding step then adds one.
    if (product >= ((ULONG)denominator << 16)) {
        return product + 1;
    }

    ULONG quotient = product / denominator;
    ULONG remainder = product % denominator;

    // Reduction counts selected source pixels, including position zero. Its
    // rounding threshold is the numerator, not the denominator used to enlarge.
    if (numerator >= denominator ? 2 * remainder >= denominator : 2 * remainder > numerator) {
        ++quotient;
    }

    return quotient;
}
