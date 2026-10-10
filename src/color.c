/* SPDX-License-Identifier: MPL-2.0 */
// Software palette storage through the public ColorMap layout. ViewPort
// presentation and palette ownership are separate public operations.
#include "internal/color.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

static UWORD defaultColor(unsigned index)
{
    if (index < 8) {
        return (index & 1 ? 0x0f00 : 0) | (index & 2 ? 0x00f0 : 0) | (index & 4 ? 0x000f : 0);
    }
    if (index < 16) {
        // Additional legacy colours between the primaries and grey ramp.
        static const UWORD accents[] = {0x620, 0xe50, 0x9f1, 0xeb0, 0x55f, 0x92f, 0x0f8, 0xccc};
        return accents[index - 8];
    }
    if (index < 32) { return (index - 16) * 0x111; }
    if (index >= 256) { return 0; }
    static const UBYTE levels[] = {16, 8, 12, 4};
    static const UBYTE scales[] = {4, 2, 6, 3, 1, 5, 7};
    unsigned group = (index - 32) / 7;
    unsigned scale = scales[(index - 32) % 7];
    unsigned red = levels[group % 4] * scale / 4;
    unsigned green = levels[(group / 4) % 4] * scale / 4;
    unsigned blue = (group < 16 ? 16 : 8) * scale / 4;
    if (red > 15) { red = 15; }
    if (green > 15) { green = 15; }
    if (blue > 15) { blue = 15; }
    return (red << 8) | (green << 4) | blue;
}

struct ColorMap* pcGetColorMap(LONG entries asm("d0"), struct GfxBase* graphics asm("a6"))
{
    // Count is a UWORD. Reject sizes that cannot be represented or released
    // through the public structure instead of wrapping allocation arithmetic.
    if (entries <= 0 || (ULONG)entries > 65535) { return NULL; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    APTR* allocation = AllocMem(sizeof(APTR) + sizeof(struct ColorMap), MEMF_PUBLIC | MEMF_CLEAR);
    if (!allocation) { return NULL; }
    *allocation = allocation;
    struct ColorMap* map = (APTR)(allocation + 1);
    map->Type = COLORMAP_TYPE_V39;
    map->SpriteBase_Even = map->SpriteBase_Odd = 16;
    map->Bp_1_base = 8;
    map->SpriteResolution = (UBYTE)SPRITERESN_DEFAULT;
    map->LowColorBits = AllocMem((ULONG)entries * sizeof(UWORD), MEMF_PUBLIC | MEMF_CLEAR);
    if (!map->LowColorBits) { goto fail; }
    map->Count = entries;
    map->ColorTable = AllocMem((ULONG)entries * sizeof(UWORD), MEMF_PUBLIC);
    if (!map->ColorTable) { goto fail; }
    for (unsigned i = 0; i < (unsigned)entries; ++i) {
        UWORD color = defaultColor(i);
        ((UWORD*)map->ColorTable)[i] = color;
        ((UWORD*)map->LowColorBits)[i] = color;
    }
    map->VPModeID = ~0UL;
    return map;
fail:
    // Retain the public cleanup owner, including installed RTG replacements.
    FreeColorMap(map);
    return NULL;
}

void pcFreeColorMap(struct ColorMap* map asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!map) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ULONG table_size = (ULONG)map->Count * sizeof(UWORD);
    if (map->ColorTable) { FreeMem(map->ColorTable, table_size); }
    if (map->LowColorBits) { FreeMem(map->LowColorBits, table_size); }
    if (map->PalExtra) { FreeMem(map->PalExtra, sizeof(*map->PalExtra) + 3UL * map->Count); }
    // GetColorMap's allocation has one pointer-sized prefix before the public
    // structure. Do not free ViewPort/DisplayInfo pointers owned by callers.
    FreeMem((UBYTE*)map - sizeof(APTR), sizeof(APTR) + sizeof(*map));
}

LONG pcAttachPalExtra(struct ColorMap* map asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();
    if (!map->PalExtra) {
        struct PaletteExtra* palette = AllocMem(sizeof(*palette) + 3UL * map->Count,
            MEMF_PUBLIC | MEMF_CLEAR);
        map->PalExtra = palette;
        if (palette) {
            InitSemaphore(&palette->pe_Semaphore);
            palette->pe_ViewPort = viewport;
            palette->pe_FirstShared = 0xffff;
            palette->pe_RefCnt = (UBYTE*)(palette + 1);
            palette->pe_AllocList = palette->pe_RefCnt + 2UL * map->Count;
            for (ULONG i = 0; i < map->Count; ++i) { palette->pe_AllocList[i] = i - 1; }
            if (viewport->RasInfo && viewport->RasInfo->BitMap) {
                WORD depth = viewport->RasInfo->BitMap->Depth;
                if (viewport->Modes & HAM) { depth -= 2; }
                if (viewport->Modes & EXTRA_HALFBRITE) { --depth; }
                // Match the original register shift, including a zero result
                // for a shift of 32 or more; no host-language undefined shift.
                unsigned shift = (UWORD)depth & 63;
                UWORD count = shift < 32 ? 1UL << shift : 0;
                palette->pe_NFree = count;
                palette->pe_FirstFree = count - 1;
                palette->pe_SharableColors = count - 1;
            }
        }
    }
    Permit();
    return map->PalExtra ? 0 : 1;
}

ULONG pcObtainPen(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"), LONG flags asm("d4"),
    struct GfxBase* graphics asm("a6"))
{
    if (!map || (WORD)index > (WORD)map->Count || !map->PalExtra) { return ~0UL; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct PaletteExtra* palette = map->PalExtra;
    // The public pointer is byte-typed, but each reference count is a WORD.
    UWORD* references = (APTR)palette->pe_RefCnt;
    UBYTE* links = palette->pe_AllocList;
    ULONG pen = ~0UL;
    ObtainSemaphore(&palette->pe_Semaphore);
    if (palette->pe_NFree) {
        if ((LONG)index < 0) {
            pen = palette->pe_FirstFree;
            ++references[pen];
            --palette->pe_NFree;
            palette->pe_FirstFree = (palette->pe_FirstFree & 0xff00) | links[pen];
        } else if ((UWORD)index <= palette->pe_SharableColors && !references[index]) {
            pen = index;
            ++references[pen];
            if (--palette->pe_NFree) {
                UWORD previous = palette->pe_FirstFree;
                if (previous == pen) {
                    palette->pe_FirstFree = (previous & 0xff00) | links[pen];
                } else {
                    while (links[previous] != (UBYTE)pen) { previous = links[previous]; }
                    links[previous] = links[pen];
                }
            }
        }
        if (pen != ~0UL) {
            if (!(flags & PENF_EXCLUSIVE)) {
                ++palette->pe_NShared;
                links[pen] = palette->pe_FirstShared;
                palette->pe_FirstShared = pen;
            }
            // Dispatch through the installed public owner, including P96.
            // The palette remains locked while its colour is changed.
            if (!(flags & PENF_NO_SETCOLOR)) { SetRGB32(palette->pe_ViewPort, pen, red, green, blue); }
        }
    }
    ReleaseSemaphore(&palette->pe_Semaphore);
    return pen;
}

void pcReleasePen(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    struct GfxBase* graphics asm("a6"))
{
    if ((LONG)index < 0 || !map) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct PaletteExtra* palette = map->PalExtra;
    UWORD* references = (APTR)palette->pe_RefCnt;
    UBYTE* links = palette->pe_AllocList;
    ObtainSemaphore(&palette->pe_Semaphore);
    if (!--references[index]) {
        ++palette->pe_NFree;
        if (palette->pe_NShared) {
            UWORD previous = palette->pe_FirstShared;
            if (previous == index) {
                palette->pe_FirstShared = (previous & 0xff00) | links[index];
                --palette->pe_NShared;
            } else {
                for (unsigned remaining = palette->pe_NShared - 1; remaining; --remaining) {
                    if (links[previous] == (UBYTE)index) {
                        links[previous] = links[index];
                        --palette->pe_NShared;
                        break;
                    }
                    previous = links[previous];
                }
            }
        }
        links[index] = palette->pe_FirstFree;
        palette->pe_FirstFree = index;
    }
    ReleaseSemaphore(&palette->pe_Semaphore);
}

static UWORD* colorEntry(APTR table, ULONG index)
{
    // The single-entry interfaces use a signed WORD table displacement.
    return (UWORD*)((UBYTE*)table + (WORD)(index * sizeof(UWORD)));
}

static ULONG colorDistance(const struct ColorMap* map, UWORD pen, ULONG red, ULONG green,
    ULONG blue, ULONG mask)
{
    UWORD high = *colorEntry(map->ColorTable, pen);
    UWORD low = *colorEntry(map->LowColorBits, pen);
    ULONG requested[] = {red, green, blue};
    ULONG difference[3];
    for (unsigned i = 0; i < 3; ++i) {
        unsigned shift = 8 - i * 4;
        ULONG component = (ULONG)((((high >> shift) & 15) << 4) | ((low >> shift) & 15)) << 24;
        ULONG first = requested[i] >> 6, second = (component & mask) >> 6;
        difference[i] = (first > second ? first - second : second - first) >> 16;
    }
    // Preserve the supplied original's weighted distance. Quantization follows
    // subtraction, so discarded component bits can still affect a boundary.
    ULONG largest = difference[0], middle = difference[1], smallest = difference[2];
    if (largest < middle) { ULONG swap = largest; largest = middle; middle = swap; }
    if (largest < smallest) { ULONG swap = largest; largest = smallest; smallest = swap; }
    if (middle < smallest) { ULONG swap = middle; middle = smallest; smallest = swap; }
    return 4 * largest + 2 * middle + smallest;
}

LONG pcObtainBestPenA(struct ColorMap* map asm("a0"), ULONG red asm("d1"),
    ULONG green asm("d2"), ULONG blue asm("d3"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    if (!map) { return -1; }
    struct UtilityBase* UtilityBase = (APTR)graphics->UtilBase;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    LONG precision = PRECISION_IMAGE;
    BOOL fail_if_bad = FALSE;
    struct TagItem* cursor = (APTR)tags;
    struct TagItem* tag;
    while ((tag = NextTagItem(&cursor))) {
        if (tag->ti_Tag == OBP_Precision) { precision = tag->ti_Data; }
        if (tag->ti_Tag == OBP_FailIfBad) { fail_if_bad = tag->ti_Data != 0; }
    }
    // As with the original, callers attach PalExtra before using this API.
    struct PaletteExtra* palette = map->PalExtra;
    ULONG tolerance = 0, limit = ~0UL;
    if (precision >= 0) {
        if (palette->pe_NFree) {
            ULONG scaled = (ULONG)(LONG)(WORD)((ULONG)precision + palette->pe_SharableColors) << 3;
            ULONG quotient = scaled / palette->pe_NFree;
            // The original WORD division leaves its dividend on overflow.
            UWORD value = quotient <= 65535 ? quotient : scaled;
            tolerance = (ULONG)value * value;
        } else { tolerance = ~0UL; }
    }
    if (fail_if_bad) {
        UWORD value = precision < 0 ? 0 : (((ULONG)precision + 1) << 5);
        limit = (ULONG)value * value;
    }
    ULONG mask = graphics->ColorMask;
    red &= mask; green &= mask; blue &= mask;
    ULONG distance = ~0UL;
    LONG best = -1;
    ObtainSemaphore(&palette->pe_Semaphore);
    unsigned pen = palette->pe_FirstShared;
    for (unsigned remaining = palette->pe_NShared; remaining; --remaining) {
        ULONG candidate = colorDistance(map, pen, red, green, blue, mask);
        if (candidate < distance) { distance = candidate; best = pen; }
        if (!candidate) { break; }
        pen = palette->pe_AllocList[pen];
    }
    if (distance) {
        if (best >= 0 && distance <= tolerance) {
            if (distance > limit) { best = -1; }
        } else if (palette->pe_NFree) {
            best = palette->pe_FirstFree;
            --palette->pe_NFree;
            palette->pe_FirstFree = palette->pe_AllocList[best];
            ++palette->pe_NShared;
            palette->pe_AllocList[best] = palette->pe_FirstShared;
            palette->pe_FirstShared = best;
            SetRGB32(palette->pe_ViewPort, best, red, green, blue);
        } else if (fail_if_bad) { best = -1; }
    }
    if (best >= 0) { ++((UWORD*)palette->pe_RefCnt)[best]; }
    ReleaseSemaphore(&palette->pe_Semaphore);
    return best;
}

LONG pcFindColor(struct ColorMap* map asm("a3"), ULONG red asm("d1"),
    ULONG green asm("d2"), ULONG blue asm("d3"), LONG maximum asm("d4"))
{
    if (maximum < 0) {
        // The original treats Count itself as inclusive without PalExtra.
        // Retain that observable extra-entry read, as explicitly requested.
        maximum = map->PalExtra ? map->PalExtra->pe_SharableColors : map->Count;
    }
    ULONG best_distance = ~0UL;
    LONG best = -1;
    ULONG pen = maximum;
    for (ULONG remaining = (UWORD)maximum + 1UL; remaining; --remaining, --pen) {
        // Unlike ObtainBestPen, this search does not mask component values,
        // consult ownership lists, take the palette lock or acquire a pen.
        ULONG distance = colorDistance(map, pen, red, green, blue, ~0UL);
        if (distance < best_distance) { best_distance = distance; best = pen; }
    }
    return best;
}

ULONG pcGetRGB4(struct ColorMap* map asm("a0"), LONG index asm("d0"))
{
    if ((WORD)index >= (WORD)map->Count) { return ~0UL; }
    return *colorEntry(map->ColorTable, index) & 0x0fff;
}

void pcSetRGB4CM(struct ColorMap* map asm("a0"), WORD index asm("d0"),
    UBYTE red asm("d1"), UBYTE green asm("d2"), UBYTE blue asm("d3"))
{
    if (!map || (UWORD)index >= map->Count) { return; }
    UWORD color = ((UWORD)red << 8) | ((UWORD)green << 4) | blue;
    UWORD* high = colorEntry(map->ColorTable, index);
    *high = (*high & 0x8000) | color;
    if (map->Type && map->LowColorBits) { *colorEntry(map->LowColorBits, index) = color; }
}

void pcSetRGB32CM(struct ColorMap* map asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"))
{
    if (!map || (WORD)index >= (WORD)map->Count) { return; }
    UWORD* high = colorEntry(map->ColorTable, index);
    UWORD* low = map->Type ? colorEntry(map->LowColorBits, index) : high;
    *high = (*high & 0x8000) | ((red >> 20) & 0x0f00) |
        ((green >> 24) & 0x00f0) | (blue >> 28);
    *low = ((red >> 16) & 0x0f00) | ((green >> 20) & 0x00f0) | ((blue >> 24) & 0x000f);
}

void pcGetRGB32(const struct ColorMap* map asm("a0"), ULONG first asm("d0"), ULONG count asm("d1"),
    ULONG* colors asm("a1"), struct GfxBase* graphics asm("a6"))
{
    if (!map) { return; }
    const UWORD* high = (const UWORD*)map->ColorTable + first;
    const UWORD* low = high;
    if ((graphics->ChipRevBits0 & GFXF_AA_LISA) && map->Type && map->LowColorBits) {
        low = (const UWORD*)map->LowColorBits + first;
    }
    // Keep the original WORD count, including its zero-count wrap. The caller
    // supplies storage for the requested range; ColorMap.Count does not clip it.
    ULONG remaining = (UWORD)(count - 1) + 1UL;
    do {
        UWORD upper = *high++, lower = *low++;
        for (int shift = 8; shift >= 0; shift -= 4) {
            ULONG component = (((upper >> shift) & 15) << 4) | ((lower >> shift) & 15);
            *colors++ = component * 0x01010101UL;
        }
    } while (--remaining);
}

void pcSetRGB32(struct ViewPort* viewport asm("a0"), ULONG index asm("d0"),
    ULONG red asm("d1"), ULONG green asm("d2"), ULONG blue asm("d3"),
    struct GfxBase* graphics asm("a6"))
{
    // Preserve the original complete D0 value in the header, including upper
    // bits outside a valid pen index. LoadRGB32 owns validation/presentation.
    // Dispatch through its public vector so an installed P96 owner receives it.
    const ULONG colors[] = {index | 0x10000UL, red, green, blue, 0};
    LoadRGB32(viewport, colors);
}

void pcUpdateViewportPalette(struct ViewPort* viewport, ViewPortPaletteFunction default_palette,
    struct GfxBase* graphics)
{
    struct ColorMap* map = viewport->ColorMap;

    if (!map) {
        return;
    }

    ViewPortPaletteFunction update = default_palette;

    if (map->Type && map->cm_vpe && map->cm_vpe->VecTable) {
        // Preserve the existing driver's fifth method and its A2/A6 ABI.
        update = ((ViewPortPaletteFunction*)map->cm_vpe->VecTable)[4];
    }

    update(viewport, graphics);
}

void pcLoadViewportRGB32(struct ViewPort* viewport, const ULONG* colors,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics)
{
    if (!colors) {
        return;
    }

    struct ColorMap* map = viewport->ColorMap;
    UWORD* upper = map->ColorTable;
    UWORD* lower = map->Type ? map->LowColorBits : upper;
    typedef UWORD ColorWord __attribute__((may_alias));
    const ColorWord* entry = (const void*)colors;
    UWORD count;

    while ((count = *entry++) != 0) {
        UWORD first = *entry++;
        colors = (const void*)entry;
        WORD excess = (UWORD)(count + first - map->Count);
        ULONG skipped = excess > 0 ? excess : 0;
        count = (UWORD)(count - skipped);
        UWORD* high = colorEntry(upper, first);
        UWORD* low = colorEntry(lower, first);

        // Both the clipping calculation and table displacement use WORDs in
        // the original. Retain their wrap instead of adding a second clamp.
        while (count--) {
            typedef ULONG ColorComponent __attribute__((may_alias));
            const volatile ColorComponent* source = (const void*)colors;
            ULONG red = source[0], green = source[1], blue = source[2];

            *low++ = ((red >> 16) & 0x0f00) | ((green >> 20) & 0x00f0) | ((blue >> 24) & 0x000f);

            // Low nibbles are stored first, including when the two tables
            // overlap (always for a V1.2 map). Reload the source as well, as
            // the original does, before preserving bit 15 of the high entry.
            red = source[0];
            green = source[1];
            blue = source[2];
            *high = (*high & 0x8000) | ((red >> 20) & 0x0f00) |
                ((green >> 24) & 0x00f0) | (blue >> 28);
            ++high;
            colors += 3;
        }

        colors += skipped * 3;
        entry = (const void*)colors;
    }

    // Even an empty non-NULL list dispatches once, after all table updates.
    pcUpdateViewportPalette(viewport, default_palette, graphics);
}

void pcLoadViewportRGB4(struct ViewPort* viewport, const UWORD* colors, ULONG count,
    ViewPortPaletteFunction default_palette, ViewPortRGB4Function default_color, struct GfxBase* graphics)
{
    count = (UWORD)count;

    if (!count) {
        return;
    }

    struct ColorMap* map = viewport ? viewport->ColorMap : NULL;

    if (map) {
        if (count > map->Count) {
            count = map->Count;
        }

        UWORD* high = map->ColorTable;
        UWORD* low = map->Type && map->LowColorBits ? map->LowColorBits : high;

        // The original enters this loop before testing its WORD counter.
        // A zero-sized ColorMap therefore performs 65536 stores, rather than
        // sharing the early return for an initially zero input count.
        count = (UWORD)(count - 1) + 1UL;

        do {
            UWORD color = *colors++ & 0x7fff;
            *high = (*high & 0x8000) | color;
            ++high;
            *low = (*low & 0x8000) | color;
            ++low;
        } while (--count);

        pcUpdateViewportPalette(viewport, default_palette, graphics);

        return;
    }

    if (viewport) {
        // The old no-ColorMap path updates count + 1 display-list colors,
        // after clamping count to 32. It is distinct from the global path.
        count = (count > 32 ? 32 : count) + 1;
    }

    for (ULONG index = 0; index < count; ++index) {
        UWORD color = *colors++;

        if (viewport) {
            color &= 0x0fff;
        }

        default_color(viewport, index, color, graphics);
    }
}

static ULONG expandRGB4Component(ULONG component)
{
    // Keep the original SWAP/shift result, including input bits outside the
    // documented nibble. Masking those bits changes what SetRGB32 receives.
    ULONG expanded = (component << 24) | ((component >> 8) & 0x00ffff00);

    return expanded | (expanded << 4);
}

void pcSetViewportRGB4(struct ViewPort* viewport, ULONG index, ULONG red, ULONG green, ULONG blue,
    ViewPortRGB4Function default_color, struct GfxBase* graphics)
{
    index = (UWORD)index;

    if (viewport && viewport->ColorMap) {
        SetRGB32(viewport, index, expandRGB4Component(red), expandRGB4Component(green),
            expandRGB4Component(blue));

        return;
    }

    UWORD color = ((red & 15) << 8) | ((green & 15) << 4) | (blue & 15);
    default_color(viewport, index, color, graphics);
}
