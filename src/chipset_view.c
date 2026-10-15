/* SPDX-License-Identifier: APL-1.1 */
/* Fetch timing and scroll encoding adapted from AROS amigavideo_chipset.c.
 * Copyright (C) 1995-2020, The AROS Development Team. All rights reserved.
 * See CHIPSET.md and LICENSE.APL for provenance and license terms.
 */
#define V1_3
#include "internal/chipset.h"
#include "internal/view.h"
#include "internal/copper.h"
#include "internal/library.h"
#include "internal/monitor.h"

#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

_Static_assert(sizeof(struct CopList) == COPLIST_STORAGE_SIZE, "legacy CopList allocation");

static struct CopIns* move(struct CopIns* instruction, UWORD flags, UWORD address, UWORD data)
{
    instruction->OpCode = COPPER_MOVE | flags;
    instruction->DESTADDR = address;
    instruction->DESTDATA = data;

    return instruction + 1;
}

static struct CopIns* planePointer(struct CopIns* instruction, UWORD flags, unsigned plane, ULONG address)
{
    instruction = move(instruction, flags, 0xe0 + 4 * plane, address >> 16);

    return move(instruction, flags, 0xe2 + 4 * plane, address);
}

static struct CopIns* waitLine(struct CopIns* instruction, UWORD flags, WORD line)
{
    instruction->OpCode = COPPER_WAIT | flags;
    instruction->VWAITPOS = line;
    instruction->HWAITPOS = 0;

    return instruction + 1;
}

static void backgroundRecord(UWORD* record, UWORD address, UWORD value)
{
    record[0] = address;
    record[1] = value;
}

static UWORD viewPortPriority(const struct ColorMap* map, const struct GfxBase* graphics)
{
    if (!map || !map->Type) {
        return 0x24;
    }

    const struct ViewPort* viewport = map->cm_vp;
    ULONG modes = viewport ? pcViewportModes(viewport) : 0;
    UWORD control = map->Flags & COLORMAP_TRANSPARENCY ? 0x0400 : 0;

    if (map->Flags & COLORPLANE_TRANSPARENCY) {
        UWORD plane = map->TransparencyPlane;

        if ((modes & HAM) && viewport->RasInfo && viewport->RasInfo->BitMap &&
            viewport->RasInfo->BitMap->Depth == 8) {
            plane = (UBYTE)(plane + 2) & 7;
        }

        control |= 0x0800 | (UWORD)((plane >> 4) | (plane << 12));
    }

    UBYTE priority = viewport ? viewport->SpritePriorities : 0x24;

    if (viewport && (modes & SUPERHIRES) && !(graphics->ChipRevBits0 & GFXF_AA_MLISA)) {
        priority = (priority >> 1) & ~4;
    }

    return control | priority;
}

void pcUpdateChipsetBackground(const struct ViewPort* viewport, ULONG modes,
    const struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    struct copinit* list = graphics->copinit;
    BOOL advanced = (graphics->ChipRevBits0 & GFXF_AA_LISA) != 0;
    BOOL extended = (graphics->ChipRevBits0 & GFXF_HR_DENISE) != 0;

    if (graphics->ActiView && !viewport) {
        backgroundRecord(list->diwstart, 0x08e, 0x14ff);
        backgroundRecord(list->diwstart + 2, 0x1e4, 0);
    } else if (viewport) {
        const struct ColorMap* map = viewport->ColorMap;

        if (map) {
            UWORD high = *(const UWORD*)map->ColorTable;
            UWORD transparent = map->Type && (map->Flags & COLORMAP_TRANSPARENCY) ? high & 0x8000 : 0;

            // ECS superhires packs the same border colour into both halves
            // of a palette word. AA retains ordinary high/low RGB nibbles.
            if ((modes & SUPERHIRES) && !(graphics->ChipRevBits0 & GFXF_AA_MLISA) && !advanced) {
                high &= 0x0ccc;
                high |= high >> 2;
                transparent |= transparent >> 1;
            }

            UWORD display, sprites;
            pcReadViewPortListControls(&display, &sprites, map, graphics);
            UWORD border = display | 0x0c00;

            if (monitor && (monitor->BeamCon0 & 8)) {
                border |= 1;
            }

            backgroundRecord(list->diagstrt + 2, 0x106, border);
            backgroundRecord(list->diagstrt + 4, 0x180, high | transparent);
            UWORD low = map->Type == COLORMAP_TYPE_V39 && advanced ?
                *(const UWORD*)map->LowColorBits : high;

            if ((modes & SUPERHIRES) && !advanced) {
                low &= 0x0ccc;
                low |= low >> 2;
                transparent |= transparent >> 1;
            }

            backgroundRecord(list->diagstrt + 6, advanced ? 0x106 : 0x1fe, border | 0x0200);
            backgroundRecord(list->diagstrt + 8, advanced ? 0x180 : 0x1fe, low | transparent);

            if (monitor) {
                UWORD control = graphics->system_bplcon0 | (graphics->ActiView->Modes & LACE);
                control |= modes & (HIRES | HAM | DUALPF);

                if (modes & SUPERHIRES) {
                    control |= 0x0040;
                }

                if (extended) {
                    control |= 1;
                    UWORD priority = viewPortPriority(map, graphics) | (modes & PFBA);

                    if (map->Type && (map->Flags & COLORPLANE_TRANSPARENCY)) {
                        priority |= (UWORD)map->TransparencyPlane << 12;
                    }

                    if (monitor->BeamCon0 & 8) {
                        display |= 1;
                    }

                    list->fm0[1] = graphics->SpriteFMode |
                        (monitor->ms_Flags & MSF_DOUBLE_SPRITES ? 0x8000 : 0);
                    backgroundRecord(list->bplcon2, 0x104, priority);
                    backgroundRecord(list->diagstrt + 10, advanced ? 0x106 : 0x1fe, display);
                }

                backgroundRecord(list->diagstrt, 0x100, control);
            }
        } else {
            // A viewport without a ColorMap leaves the hardware palette
            // alone. Replace both bank/colour pairs with harmless records.
            for (unsigned word = 2; word < 10; word += 2) {
                backgroundRecord(list->diagstrt + word, 0x0e0, 0);
            }
        }

        if (extended) {
            backgroundRecord(list->diwstart, 0x08e, 0x0181);
            backgroundRecord(list->diwstart + 2, 0x090, 0x0281);
        }
    }

    if (!advanced) {
        list->diagstrt[8] = 0x1fe;
    }
}

static void restrictPalette(struct CopList* list, struct GfxBase* graphics)
{
    if (graphics->ChipRevBits0 & GFXF_AA_LISA) {
        return;
    }

    // OCS/ECS have one four-bit-per-component palette. AGA's low-nibble
    // pass would overwrite that palette rather than extend its precision.
    struct CopIns* output = list->CopIns;
    BOOL low = FALSE;

    for (unsigned index = 0; index < (UWORD)list->Count; ++index) {
        const struct CopIns* input = &list->CopIns[index];

        if (input->OpCode == COPPER_MOVE) {
            if (input->DESTADDR == 0x106) {
                low = (input->DESTDATA & 0xe200) != 0;
                // Palette-bank selection is AGA-only. ECS display controls
                // are emitted with the display-window setup below.
                continue;
            }

            if (input->DESTADDR == 0x10c || (low && input->DESTADDR >= 0x180 && input->DESTADDR <= 0x1be)) {
                continue;
            }
        }

        *output++ = *input;
    }

    list->Count = output - list->CopIns;
}

struct ChipsetRasterRegisters {
    UWORD fetch_start, fetch_stop, scroll;
    UWORD fetch_mode;
    UWORD modulo[2];
    ULONG planes[8];
};

struct ChipsetPlanePhase {
    WORD fetch_start;
    LONG column;
    UWORD delay;
    BOOL clamped;
};

static struct ChipsetPlanePhase planePhase(WORD fetch_start, WORD display_start, LONG source_x,
    unsigned resolution, unsigned fetch_shift, unsigned unit, unsigned slots)
{
    unsigned phase_shift = fetch_shift + 2 - resolution;
    unsigned pipeline = 2U << (phase_shift < 2 ? phase_shift : 2);
    unsigned quantum = 256U >> (resolution + 2 - fetch_shift);
    unsigned word_bytes = 2U << fetch_shift;
    WORD displacement = -(source_x * (1L << (2 - resolution)));
    WORD phase = ((ULONG)(UWORD)display_start << (2 - resolution)) -
        (((ULONG)(UWORD)fetch_start + pipeline) << 3) - 4 + displacement;
    struct ChipsetPlanePhase output;
    output.fetch_start = fetch_start;
    output.column = (-(LONG)phase / (LONG)quantum) * word_bytes;
    output.delay = ((UWORD)phase + quantum) & (quantum - 1);
    output.clamped = FALSE;

    // Fine scrolling can require an earlier word. At the left DMA boundary
    // the pointer, rather than the fetch start, absorbs the missing word.
    if ((phase < 0 || displacement > 0) && output.delay) {
        output.fetch_start = (WORD)(fetch_start - unit);

        if (output.fetch_start < 24) {
            output.fetch_start = 24;
            output.clamped = TRUE;

            if (displacement <= 0) {
                output.column += word_bytes;
            }
        } else if (slots < 8 || displacement > 0) {
            output.column -= ((unit >> slots) + 1) * word_bytes;
        }
    }

    return output;
}

static void prepareRasterRegisters(struct ChipsetRasterRegisters* output,
    const struct ViewPortBuildState* state, const struct ViewPortPlaneFetch* planes, BOOL dual,
    const struct GfxBase* graphics, ULONG caller_d5, BOOL adjust_bandwidth,
    const struct ChipsetPlanePhase* retained_second)
{
    const struct ViewPortBuildMode* mode = state->mode_data;
    unsigned bandwidth = state->fetch_mode;
    unsigned fetch_shift = graphics->bwshifts[bandwidth];
    unsigned resolution = mode->horizontal_shift;
    unsigned index = (resolution << graphics->arraywidth) + bandwidth;
    WORD mask = graphics->StrtFetchMasks[index];
    unsigned unit = -mask;
    unsigned slots = graphics->RealStops[index];
    unsigned word_bytes = 2U << fetch_shift;

    // The original ScrollVPort replaces only the low word of its retained
    // D5 before scaling the display origin. Preserve that observable WORD/
    // LONG boundary, including signed alignment and left-edge clamping.
    LONG origin = (LONG)((caller_d5 & 0xffff0000UL) | (UWORD)state->start_x) >> resolution;
    LONG phase = ((LONG)((ULONG)origin - 1) >> 1) - slots;
    WORD fetch_start = ((WORD)(phase - 24) & mask) + 24;

    if (fetch_start < 24) {
        fetch_start = 24;
    }

    unsigned word_pixels = 16U << fetch_shift;

    if (slots < 8) {
        word_pixels *= unit / slots;
    }

    ULONG width = (ULONG)(UWORD)state->stop_x - (UWORD)state->start_x;
    UWORD words = (width - 1) / word_pixels + 1;
    WORD fetch_stop = fetch_start + words * unit;
    LONG right_limit = (LONG)state->monitor->total_colorclocks - slots;

    if (fetch_stop >= right_limit) {
        fetch_stop = ((right_limit - 25) & mask) + 24;

        if (fetch_stop == fetch_start) {
            fetch_start = (WORD)(fetch_start - unit);
        }
    }

    struct ChipsetPlanePhase fields[2];

    for (unsigned field = 0; field < 2; ++field) {
        fields[field] = planePhase(fetch_start, state->start_x, state->source_x[dual ? field : 0],
            resolution, fetch_shift, unit, slots);
    }

    // Both playfields share DDFSTART. If only one needs an earlier fetch,
    // move the other's pointer back by one memory word as well.
    fetch_start = fields[0].fetch_start;

    if (retained_second) {
        fields[1] = *retained_second;
    } else if ((UWORD)fields[0].fetch_start < (UWORD)fields[1].fetch_start) {
        fields[1].column -= word_bytes;
    } else if ((UWORD)fields[1].fetch_start < (UWORD)fields[0].fetch_start) {
        fetch_start = fields[1].fetch_start;
        fields[0].column -= word_bytes;
    }

    if (adjust_bandwidth) {
        unsigned selected = bandwidth;
        unsigned selected_unit = unit;
        ULONG selected_phase = caller_d5;

        // Wide words cannot supply every fine-scroll phase at the earliest
        // DMA start. Select a narrower word and recalculate the addresses.
        if (fields[0].clamped || fields[1].clamped) {
            unsigned delay = fields[0].delay;

            if (resolution == 1 && bandwidth == BANDWIDTH_4X && delay >= 112) {
                selected = BANDWIDTH_2XNML;
            } else if (!resolution && bandwidth == BANDWIDTH_4X) {
                if (delay >= 240 || (delay >= 112 && delay < 128)) {
                    selected = BANDWIDTH_1X;
                } else if (delay >= 128) {
                    selected = BANDWIDTH_2XNML;
                }
            } else if (!resolution && bandwidth && delay >= 112) {
                selected = BANDWIDTH_1X;
            }

            if (selected != bandwidth) {
                selected_unit = -(WORD)graphics->StrtFetchMasks[
                    (resolution << graphics->arraywidth) + selected];
                selected_phase = 0;
            }
        }

        if ((UWORD)state->stop_x < 256 && selected_unit == 8 &&
            !(graphics->ChipRevBits0 & GFXF_HR_DENISE)) {
            fetch_stop = (WORD)(fetch_stop - selected_unit);
        }

        // Reserved sprite slots consume the clocks before bitplane DMA.
        // Use the last occupied slot, including holes in the reservation mask.
        if (graphics->SpriteReserved > 1 && (selected == bandwidth || selected)) {
            unsigned last_sprite = 0;

            for (unsigned reserved = graphics->SpriteReserved; reserved >>= 1;) {
                ++last_sprite;
            }

            LONG sprite_stop = 24 + 4 * last_sprite;
            WORD available = (WORD)(phase - fetch_start);
            unsigned half_unit = selected_unit / 2;

            if (sprite_stop != fetch_start && selected_unit > 8 && available >= (LONG)half_unit) {
                selected = selected == BANDWIDTH_4X ? BANDWIDTH_2XNML : BANDWIDTH_1X;
                available = (WORD)(available - half_unit);

                if (fetch_start + (LONG)half_unit < sprite_stop && half_unit > 8 &&
                    available >= (LONG)(half_unit / 2)) {
                    selected = BANDWIDTH_1X;
                }
            }
        }

        if (selected != bandwidth) {
            struct ViewPortBuildState narrowed = *state;
            narrowed.fetch_mode = selected;

            // The original narrows the first playfield after reconciling the
            // shared fetch start. The second retains its earlier phase and
            // column; only its modulo and final alignment use the new width.
            prepareRasterRegisters(output, &narrowed, planes, dual, graphics, selected_phase,
                FALSE, dual ? &fields[1] : NULL);
            return;
        }
    }

    UWORD scroll = 0;

    for (unsigned field = 0; field < 2; ++field) {
        UWORD value = fields[field].delay;
        UWORD bits = (value >> 2) & 15;

        if (graphics->ChipRevBits0 & GFXF_AA_LISA) {
            bits |= ((value >> 6) << 10) | ((value & 3) << 8);
        }

        scroll |= bits << (4 * field);
    }

    output->fetch_start = fetch_start;
    output->fetch_stop = fetch_stop;
    output->scroll = scroll;
    output->fetch_mode = bandwidth | graphics->SpriteFMode;

    LONG overrun = -(WORD)graphics->Overrun[index];
    LONG fetch_words = (((LONG)(UWORD)fetch_stop - (UWORD)fetch_start) * (1L << fetch_shift)) / overrun;
    fetch_words += (8 / (overrun < 8 ? overrun : 8)) << fetch_shift;

    for (unsigned field = 0; field < 2; ++field) {
        UWORD stride = planes[dual ? field : 0].bytes_per_row;
        UWORD modulo = (((ULONG)stride + 1) / 2 - fetch_words) * 2;
        modulo &= (UWORD)-(LONG)word_bytes;
        output->modulo[field] = modulo + (state->interlaced ? stride : 0);
    }

    for (unsigned plane = 0; plane < 8; ++plane) {
        const struct ViewPortPlaneFetch* fetch = &planes[plane];
        unsigned field = dual ? plane & 1 : 0;
        ULONG offset = (ULONG)fetch->source_y * fetch->bytes_per_row + (ULONG)fetch->row_offset;
        offset = (offset + fields[field].column) & -(ULONG)word_bytes;
        output->planes[plane] = (ULONG)fetch->plane + offset;
    }
}

static void rasterRegisters(struct ChipsetRasterRegisters* output,
    const struct ViewPortBuildState* state, const struct ViewPortPlaneFetch* planes, BOOL dual,
    const struct GfxBase* graphics, ULONG caller_d5)
{
    prepareRasterRegisters(output, state, planes, dual, graphics, caller_d5, TRUE, NULL);
}

static unsigned viewPortDepth(const struct ViewPort* viewport, ULONG modes)
{
    unsigned depth = viewport->RasInfo->BitMap->Depth;

    if ((modes & DUALPF) && viewport->RasInfo->Next) {
        unsigned second = viewport->RasInfo->Next->BitMap->Depth;
        depth = depth > second ? depth * 2 - 1 : second * 2;
    }

    return depth;
}

static UWORD bitplaneControl(unsigned depth, const struct ViewPortBuildState* state, UWORD view_modes,
    UWORD display_control, const struct GfxBase* graphics)
{
    const struct ViewPortBuildMode* mode = state->mode_data;
    UWORD control = (depth & 7) << 12;
    control |= depth == 8 ? 0x10 : 0;
    control |= state->modes & (HAM | DUALPF | LACE);
    // A noninterlaced screen can share a View with an interlaced screen.
    // Keep field timing enabled across every viewport in that View.
    control |= view_modes & LACE;
    // ViewPort SUPERHIRES is bit 5, but BPLCON0 SHRES is bit 6. The
    // hardware HIRES bit must also be clear for the super-high mode.
    control |= mode->horizontal_shift == 2 ? 0x0040 : mode->horizontal_shift == 1 ? 0x8000 : 0;
    control |= graphics->system_bplcon0;

    // ECSENA gates extended border/genlock controls, not the chipset's
    // presence. Palette banks and sprite resolution alone do not enable it.
    if (display_control & 0x0037) {
        control |= 1;
    }

    return control;
}

ULONG pcBuildChipsetViewPortList(
    struct View* view, struct ViewPort* viewport, const struct ViewPortBuildState* state, struct GfxBase* graphics)
{
    const struct ViewPortBuildMode* mode = state->mode_data;
    BOOL dual = (state->modes & DUALPF) && viewport->RasInfo->Next;
    unsigned depth = viewPortDepth(viewport, state->modes);

    struct ViewPortPlaneFetch planes[8];
    pcReadViewPortScrollFetches(planes, viewport, state->modes, state->source_x, state->source_y, state->source_stride);
    struct ChipsetRasterRegisters raster;
    rasterRegisters(&raster, state, planes, dual, graphics, 0);
    LONG start_x = state->start_x >> mode->horizontal_shift;
    LONG stop_x = state->stop_x >> mode->horizontal_shift;
    LONG start_y = state->start_y >> state->interlaced;
    LONG stop_y = state->stop_y >> state->interlaced;
    LONG long_stop_y = stop_y;
    BOOL extended = (graphics->ChipRevBits0 & GFXF_HR_DENISE) != 0;

    if (extended) {
        long_stop_y = (state->stop_y + state->interlaced) >> state->interlaced;
    } else {
        // OCS cannot encode the ECS vertical window range. Its display window
        // spans the monitor; Copper waits gate the bitplanes for each viewport.
        UWORD column = (UWORD)((UWORD)state->start_x << (2 - mode->horizontal_shift));
        start_x = (column < 1023 ? column : 1023) >> 2;
        start_y = state->monitor->min_row;

        LONG bottom = state->monitor->total_rows << state->interlaced;
        bottom -= (bottom - state->start_y) & state->interlaced;
        stop_y = long_stop_y = bottom >> state->interlaced;
    }

    UWORD display_control, sprite_control;
    pcReadViewPortListControls(&display_control, &sprite_control, viewport->ColorMap, graphics);
    UWORD control = bitplaneControl(depth, state, view->Modes, display_control, graphics);
    UWORD priority = viewPortPriority(viewport->ColorMap, graphics);
    priority |= state->modes & PFBA ? 0x40 : 0;

    // AGA disables extra-half-brite decoding in every other display mode.
    if ((graphics->ChipRevBits0 & GFXF_AA_LISA) && !(state->modes & EXTRA_HALFBRITE)) {
        priority |= 0x0200;
    }

    struct CopList* list = viewport->DspIns;
    restrictPalette(list, graphics);
    struct CopIns* instruction = list->CopIns + list->Count;
    instruction = move(instruction, 0, 0x8e, (start_y << 8) | (start_x & 255));

    // The preceding viewport can still be fetching during this setup.
    // Select the new bitplane controls before the display-window end write;
    // OCS keeps its planes disabled until the viewport's start wait.
    instruction = move(instruction, 0, 0x100, extended ? control : control & 0x0fef);
    instruction = move(instruction, 0, 0x104, priority);

    if (extended) {
        instruction = move(instruction, 0, 0x106, display_control);
    }

    // An odd ECS display end includes one more raster line in the long field.
    // Preserve that distinction in both halves of the display-window end.
    if (long_stop_y != stop_y) {
        instruction = move(instruction, CPR_NT_SHT, 0x90, (stop_y << 8) | (stop_x & 255));
        instruction = move(instruction, CPR_NT_LOF, 0x90, (long_stop_y << 8) | (stop_x & 255));
    } else {
        instruction = move(instruction, 0, 0x90, (stop_y << 8) | (stop_x & 255));
    }

    instruction = move(instruction, 0, 0x92, raster.fetch_start);
    instruction = move(instruction, 0, 0x94, raster.fetch_stop);
    instruction = move(instruction, 0, 0x102, raster.scroll);
    instruction = move(instruction, 0, 0x108, raster.modulo[0]);
    instruction = move(instruction, 0, 0x10a, raster.modulo[1]);

    for (unsigned plane = 0; plane < depth; ++plane) {
        ULONG address = raster.planes[plane];
        instruction = planePointer(instruction, state->interlaced ? CPR_NT_LOF : 0, plane, address);

        if (state->interlaced) {
            instruction = planePointer(instruction, CPR_NT_SHT, plane, address + planes[plane].bytes_per_row);

            if (!extended) {
                list->SLRepeat += 2;
            }
        }
    }

    if (graphics->ChipRevBits0 & GFXF_HR_DENISE) {
        UWORD high = ((start_y >> 8) & 7) | (((stop_y >> 8) & 7) << 8);
        high |= ((start_x >> 8) & 1) << 5;
        high |= ((stop_x >> 8) & 1) << 13;
        UWORD long_high = (high & ~0x0700) | (((long_stop_y >> 8) & 7) << 8);

        if (long_high != high) {
            instruction = move(instruction, CPR_NT_LOF, 0x1e4, long_high);
            instruction = move(instruction, CPR_NT_SHT, 0x1e4, high);
        } else {
            instruction = move(instruction, 0, 0x1e4, high);
        }
    }

    if (graphics->ChipRevBits0 & GFXF_AA_ALICE) {
        instruction = move(instruction, 0, 0x1fc, raster.fetch_mode);
    }

    list->Count = instruction - list->CopIns;

    return MVP_OK;
}

ULONG pcFinishChipsetViewPortList(
    struct View* view, struct ViewPort* viewport, const struct ViewPortBuildState* state, struct GfxBase* graphics)
{
    struct CopList* list = viewport->DspIns;

    if (!list) {
        return MVP_NO_DSPINS;
    }

    UWORD display_control, sprite_control;
    pcReadViewPortListControls(&display_control, &sprite_control, viewport->ColorMap, graphics);
    unsigned depth = viewPortDepth(viewport, state->modes);
    UWORD control = bitplaneControl(depth, state, view->Modes, display_control, graphics);

    WORD start = (WORD)(state->start_y - viewport->DyOffset - ((UWORD)view->DyOffset << state->interlaced));
    WORD height = viewport->DHeight;
    WORD long_end = (UWORD)height + (((UWORD)viewport->DyOffset + 1) & height & state->interlaced);
    struct CopIns* instruction = list->CopIns + list->Count;
    instruction = waitLine(instruction, 0, start);
    instruction = move(instruction, 0, 0x100, control);
    instruction = waitLine(instruction, CPR_NT_SHT, long_end);
    instruction = waitLine(instruction, CPR_NT_LOF, (UWORD)height & ~1U);
    instruction = move(instruction, 0, 0x100, control & 0x0fef);
    ++list->SLRepeat;
    list->Count = instruction - list->CopIns;

    // OCS measures setup before the final terminator, relative to its actual
    // bitplane-enable wait rather than the unclipped viewport origin.
    UWORD gap = CalcIVG(view, viewport);
    list->CopIns->VWAITPOS = (UWORD)start - gap;
    instruction = waitLine(instruction, 0, 10000);
    instruction[-1].HWAITPOS = 255;
    list->Count = instruction - list->CopIns;

    return MVP_OK;
}

static BOOL rasterValue(UWORD* value, UWORD destination, const struct ChipsetRasterRegisters* raster,
    const struct ViewPortPlaneFetch* planes, BOOL short_field, BOOL signed_stride, ULONG extra)
{
    switch (destination) {
        case 0x1fc:
            *value = raster->fetch_mode;
            return TRUE;
        case 0x92:
            *value = raster->fetch_start;
            return TRUE;
        case 0x94:
            *value = raster->fetch_stop;
            return TRUE;
        case 0x102:
            *value = raster->scroll;
            return TRUE;
        case 0x108:
            *value = raster->modulo[0];
            return TRUE;
        case 0x10a:
            *value = raster->modulo[1];
            return TRUE;
    }

    if (destination < 0xe0 || destination >= 0x100 || (destination & 1)) {
        return FALSE;
    }

    unsigned plane = (destination - 0xe0) >> 2;
    ULONG address = raster->planes[plane] + extra;

    if (short_field) {
        address += signed_stride ? (LONG)(WORD)planes[plane].bytes_per_row : planes[plane].bytes_per_row;
    }

    *value = destination & 2 ? address : address >> 16;

    return TRUE;
}

static void scrollIntermediate(struct CopList* list, const struct ChipsetRasterRegisters* raster,
    const struct ViewPortPlaneFetch* planes)
{
    BOOL raster_block = FALSE;

    for (; list; list = list->Next) {
        for (unsigned index = 0; index < (UWORD)list->Count; ++index) {
            struct CopIns* instruction = &list->CopIns[index];

            if ((instruction->OpCode & 3) != COPPER_MOVE) {
                if (raster_block) {
                    return;
                }

                continue;
            }

            if (instruction->DESTADDR == 0x92) {
                raster_block = TRUE;
            }

            if (raster_block) {
                UWORD value;

                if (!rasterValue(&value, instruction->DESTADDR, raster, planes,
                    !!(instruction->OpCode & CPR_NT_SHT), TRUE, 0)) {
                    return;
                }

                instruction->DESTDATA = value;
            }
        }
    }
}

struct ChipsetScrollTarget {
    const UWORD* binding;
    UWORD* raster;
    const UWORD* end;
};

static struct ChipsetScrollTarget fieldTarget(const struct MergedViews* merged,
    UWORD* public_start, ULONG cached, UWORD destination)
{
    struct ChipsetScrollTarget target = {0};

    if (!public_start) {
        return target;
    }

    const UWORD* selected = cached ? (const UWORD*)cached : public_start;

    // Cached pointers may select a still-live preceding merge. Resolve that
    // owner before scanning; never fall back to a different public field.
    for (const struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (ULONG index = 0; index < entry->display->count; ++index) {
            const struct ViewDisplayPort* port = &entry->display->viewports[index];

            for (unsigned field = 0; field < 2; ++field) {
                const struct ViewFieldPalette* binding = &port->palette[field];

                if (!entry->fields[field] || !binding->start ||
                    (ULONG)selected < (ULONG)binding->start || (ULONG)selected >= (ULONG)binding->end) {
                    continue;
                }

                target.binding = binding->start;
                target.end = binding->end;

                for (const UWORD* instruction = selected; instruction + 1 < target.end; instruction += 2) {
                    if (instruction[0] == destination) {
                        target.raster = (UWORD*)instruction;
                        return target;
                    }

                    if (instruction[0] == 0xffff && instruction[1] == 0xfffe) {
                        return target;
                    }
                }

                return target;
            }
        }
    }

    // Caller-owned streams have no private snapshot or allocation extent.
    // As with the original API, the caller supplies valid Copper memory.
    // Search its instructions directly, respecting the end-of-list WAIT.
    for (const UWORD* instruction = selected; ; instruction += 2) {
        if (instruction[0] == destination) {
            target.raster = (UWORD*)instruction;
            break;
        }

        if (instruction[0] == 0xffff && instruction[1] == 0xfffe) {
            break;
        }
    }

    return target;
}

static BOOL scrollField(const struct ChipsetScrollTarget* target,
    const struct ChipsetRasterRegisters* raster, const struct ViewPortPlaneFetch* planes,
    BOOL short_field, ULONG extra)
{
    BOOL written = FALSE;

    if (!target->raster) {
        return FALSE;
    }

    for (UWORD* instruction = target->raster; !target->end || instruction + 1 < target->end; instruction += 2) {
        UWORD value;

        if (!rasterValue(&value, instruction[0], raster, planes, short_field, FALSE, extra)) {
            break;
        }

        instruction[1] = value;
        written |= instruction[0] >= 0xe0 && instruction[0] < 0x100;
    }

    return written;
}

void pcScrollChipsetViewPort(struct ViewPort* viewport, const struct ViewPortBuildState* state,
    const struct ViewPortScrollInfo* info, const struct ViewPortPlaneFetch* planes,
    struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5)
{
    struct CopList* list = viewport->DspIns;
    struct ViewPortExtra* extra = info->field_extra;
    const struct MergedViews* merged = pcMergedViews(graphics);
    struct ChipsetScrollTarget long_target = fieldTarget(merged, list->CopLStart, extra ? extra->cop1ptr : 0, 0x92);
    struct ChipsetScrollTarget short_target = fieldTarget(merged, list->CopSStart, extra ? extra->cop2ptr : 0, 0x92);
    struct ChipsetRasterRegisters raster;
    rasterRegisters(&raster, state, planes, (info->modes & DUALPF) && viewport->RasInfo->Next, graphics, caller_d5);

    // Snapshots retain their public-list identity; hardware caches point at
    // the raster block inside that list. Keep both representations in sync
    // without changing a caller's cached field owner or allocating a new list.
    struct ViewPortExtra snapshot_extra = {0};
    snapshot_extra.cop1ptr = long_target.binding ? (ULONG)long_target.binding : extra ? extra->cop1ptr : 0;
    snapshot_extra.cop2ptr = short_target.binding ? (ULONG)short_target.binding : extra ? extra->cop2ptr : 0;
    pcApplyViewPortPlaneScroll(pcPreparedViewPorts(graphics), pcMergedViews(graphics), list,
        &snapshot_extra, planes, state->interlaced, info->view_modes, info->modes,
        info->update_intermediate, caller_d3);

    if (info->update_intermediate) {
        scrollIntermediate(list, &raster, planes);
    }

    BOOL long_written = scrollField(&long_target, &raster, planes, FALSE, 0);

    if (extra && long_written) {
        extra->cop1ptr = (ULONG)long_target.raster;
    }

    if (info->view_modes & LACE) {
        BOOL row = !!(info->modes & LACE);
        ULONG displacement = row && !long_written ? caller_d3 & 0xffff0000UL : 0;

        if (scrollField(&short_target, &raster, planes, row, displacement) && extra) {
            extra->cop2ptr = (ULONG)short_target.raster;
        }
    }
}

static struct CopIns* intermediatePlanes(struct CopList* list)
{
    for (; list; list = list->Next) {
        for (unsigned index = 0; index < (UWORD)list->Count; ++index) {
            struct CopIns* instruction = &list->CopIns[index];

            if (!(instruction->OpCode & 3) && (instruction->DESTADDR & 0xfff) == 0xe0) {
                return instruction;
            }
        }
    }

    return NULL;
}

static struct CopList* intermediateOwner(const struct PreparedViewPorts* prepared, const struct CopIns* pointer)
{
    for (const struct PreparedViewPort* entry = prepared->first; entry; entry = entry->next) {
        for (const struct CopList* list = entry->list; list; list = list->Next) {
            if ((ULONG)pointer >= (ULONG)list->CopIns &&
                (ULONG)pointer < (ULONG)(list->CopIns + (UWORD)list->Count)) {
                return (struct CopList*)entry->list;
            }
        }
    }

    return NULL;
}

static BOOL cacheChipsetBitmap(const struct ViewPort* viewport, struct DBufInfo* info, struct GfxBase* graphics)
{
    if (info->dbi_MatchLong == graphics->SpecialCounter) {
        return TRUE;
    }

    if (!pcCacheNativeDBufInfo(viewport, info, graphics)) {
        return FALSE;
    }

    // The shared cache supplies geometry and selected output owners. Chipset
    // DBufInfo caches the first pointer instruction, as the original does;
    // later hits must not reselect current ViewPort lists or ColorMap policy.
    info->dbi_CopPtr1 = info->dbi_CopPtr1 ? intermediatePlanes(info->dbi_CopPtr1) : NULL;
    info->dbi_CopPtr2 = fieldTarget(pcMergedViews(graphics), info->dbi_CopPtr2, 0, 0xe0).raster;
    info->dbi_CopPtr3 = fieldTarget(pcMergedViews(graphics), info->dbi_CopPtr3, 0, 0xe0).raster;

    return TRUE;
}

static ULONG planeDifference(UWORD destination, const struct BitMap* previous,
    const struct BitMap* bitmap, UWORD modes)
{
    unsigned rotation = (modes & HAM) && bitmap->Depth == 8 ? 8 : 0;
    unsigned plane = ((destination - rotation) & 28) >> 2;

    return (ULONG)bitmap->Planes[plane] - (ULONG)previous->Planes[plane];
}

static void relocateField(const struct ChipsetScrollTarget* target, const struct BitMap* previous,
    const struct BitMap* bitmap, UWORD modes)
{
    UWORD* instruction = target->raster;

    if (!instruction) {
        return;
    }

    for (unsigned plane = 0; plane < bitmap->Depth &&
        (!target->end || instruction + 3 < target->end); ++plane) {
        ULONG address = ((ULONG)instruction[1] << 16) | instruction[3];
        address += planeDifference(instruction[2], previous, bitmap, modes);
        instruction[3] = address;
        instruction[1] = address >> 16;
        instruction += 4;
    }
}

static void relocateIntermediate(struct CopList* owner, struct CopIns* instruction,
    const struct BitMap* previous, const struct BitMap* bitmap, UWORD modes)
{
    if (!instruction) {
        return;
    }

    for (const struct CopList* list = owner; list; list = list->Next) {
        if ((ULONG)instruction < (ULONG)list->CopIns ||
            (ULONG)instruction >= (ULONG)(list->CopIns + (UWORD)list->Count)) {
            continue;
        }

        const struct CopIns* end = list->CopIns + (UWORD)list->Count;

        while (instruction + 1 < end) {
            UWORD destination = instruction[1].DESTADDR & 0xfff;

            if (destination < 0xe0 || destination >= 0x100) {
                return;
            }

            ULONG address = ((ULONG)(UWORD)instruction[0].DESTDATA << 16) | (UWORD)instruction[1].DESTDATA;
            address += planeDifference(destination, previous, bitmap, modes);
            instruction[1].DESTDATA = address;
            instruction[0].DESTDATA = address >> 16;
            instruction += 2;
        }

        return;
    }
}

void pcChangeChipsetVPBitMap(struct ViewPort* viewport, struct BitMap* bitmap,
    struct DBufInfo* info, struct GfxBase* graphics)
{
    if (!cacheChipsetBitmap(viewport, info, graphics)) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    BOOL hidden = !!(viewport->Modes & VP_HIDE);
    WORD delta;

    for (;;) {
        Disable();
        UWORD beam = pcChipsetBeamPosition();

        if (!(graphics->ChipRevBits0 & GFXF_HR_AGNUS)) {
            beam &= 511;
        }

        delta = (WORD)(beam - info->dbi_BeamPos1);

        // Copper may be about to latch this viewport's pointers. Preserve the
        // original five-line exclusion window, restoring caller interrupt
        // nesting between reads; do not postpone the switch by a whole frame.
        if (delta < -5 || delta >= 0) {
            break;
        }

        Enable();
    }

    if (hidden) {
        delta = 0;
    } else {
        const struct BitMap* previous = viewport->RasInfo->BitMap;
        struct ChipsetScrollTarget long_target = fieldTarget(pcMergedViews(graphics), info->dbi_CopPtr3, 0, 0xe0);
        struct ChipsetScrollTarget short_target = fieldTarget(pcMergedViews(graphics), info->dbi_CopPtr2, 0, 0xe0);
        struct CopList* intermediate = intermediateOwner(pcPreparedViewPorts(graphics), info->dbi_CopPtr1);
        struct DBufInfo snapshot = *info;
        snapshot.dbi_CopPtr1 = intermediate;
        snapshot.dbi_CopPtr2 = (APTR)short_target.binding;
        snapshot.dbi_CopPtr3 = (APTR)long_target.binding;
        pcSwitchNativeViewPortBitMap(viewport, bitmap, &snapshot, graphics);
        relocateField(&long_target, previous, bitmap, viewport->Modes);
        relocateField(&short_target, previous, bitmap, viewport->Modes);
        relocateIntermediate(intermediate, info->dbi_CopPtr1, previous, bitmap, viewport->Modes);
    }

    if (info->dbi_SafeMessage.mn_ReplyPort) {
        // Retain the original signed comparison with cached BeamPos2. It is
        // intentionally not recomputed as a height or sampled again after
        // relocation; the entry beam decides this transaction's reply times.
        if (delta < 0 || delta >= (WORD)info->dbi_BeamPos2) {
            ReplyMsg(&info->dbi_SafeMessage);
        } else {
            info->dbi_Count1 = 1;
            info->dbi_Link1 = graphics->DBList;
            graphics->DBList = &info->dbi_Link1;
        }
    }

    if (info->dbi_DispMessage.mn_ReplyPort) {
        info->dbi_Count2 = delta < 0 ? 1 : 2;
        info->dbi_Link2 = graphics->DBList;
        graphics->DBList = &info->dbi_Link2;
    }

    Enable();
}
