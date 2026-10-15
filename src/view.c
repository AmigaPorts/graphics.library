/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/view.h"
#include "internal/backend.h"
#if GRAPHICS_CHIPSET
#include "internal/chipset.h"
#endif
#include "internal/library.h"
#include "internal/copper.h"
#include "internal/color.h"
#include "internal/display.h"
#include "internal/monitor.h"
#include "internal/sprite.h"

#include <exec/memory.h>
#include <stdint.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

_Static_assert(sizeof(struct ViewPortBuildState) <= VIEWPORT_BUILD_WORK_SIZE,
    "native preparation state must fit the driver workspace");

static ULONG buildViewPortList(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    (void)view;
    const struct ViewPortBuildState* state = extra->DriverData[0];
    UWORD display_control, sprite_control;
    pcReadViewPortListControls(&display_control, &sprite_control, viewport->ColorMap, graphics);

    // Keep list selectors consistent with the prepared software layout. The
    // field owner consumes these records alongside live palette updates;
    // zero selectors would erase non-default banks, XOR and sprite policy.
    // Framebuffer geometry stays in snapshots. The chipset consumer also
    // encodes those sources into its real Copper field lists below.
    BOOL superhires = (state->modes & SUPERHIRES) && !(graphics->ChipRevBits0 & GFXF_AA_MLISA);
    ULONG result = pcBuildColorList(viewport, state->color_mask, display_control, sprite_control, superhires);

#if GRAPHICS_CHIPSET
    if (result == MVP_OK && pcUsesChipset(graphics)) {
        result = pcBuildChipsetViewPortList(view, viewport, state, graphics);
    }
#endif

    return result;
}

static ULONG finishViewPortList(
    struct View* view, struct ViewPort* viewport, struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    const struct ViewPortBuildState* state = extra->DriverData[0];

#if GRAPHICS_CHIPSET
    if (pcUsesChipset(graphics) && !(graphics->ChipRevBits0 & GFXF_HR_DENISE)) {
        return pcFinishChipsetViewPortList(view, viewport, state, graphics);
    }
#endif

    UWORD first_line = (UWORD)state->start_y >> (state->interlaced ? 1 : 0);

    return pcFinishViewPortList(view, viewport, extra, first_line, state->modes, graphics);
}

const APTR pcNativeViewPortBuildMethods[] = {
    (APTR)pcInitViewPortBuild,
    (APTR)pcBuildViewPortGeometry,
    (APTR)pcBuildViewPortColors,
    (APTR)pcBuildViewPortRasters,
    (APTR)buildViewPortList,
    (APTR)finishViewPortList,
    (APTR)pcCommitViewPortBuild,
    (APTR)pcReleaseViewPortExtra,
    NULL
};

// Native output shares one software renderer across its display modes. Mode
// data supplies geometry and decoding; P96-owned tables retain their own five
// methods and are selected by the existing dispatch rules.
const APTR pcNativeViewPortVectors[5] = {
    (APTR)pcNativeMoveSprite,
    (APTR)pcChangeSprite,
    (APTR)pcNativeScrollVPort,
    (APTR)pcNativeViewPortBuildMethods,
    (APTR)pcNativeViewportPalette
};

static LONG callMonitorView(struct View* view, MonitorViewFunction method, struct GfxBase* graphics)
{
    // Monitor methods take the View on the stack. Retain the original A1/A6
    // context as well; A0 contains the method address at the call boundary.
    register struct View* argument asm("a1") = view;
    register APTR entry asm("a0") = (APTR)method;
    register struct GfxBase* base asm("a6") = graphics;
    register LONG result asm("d0");

    __asm__ volatile(
        "move.l %1,-(sp)\n\t"
        "jsr (%2)\n\t"
        "addq.l #4,sp"
        : "=r"(result), "+r"(argument), "+r"(entry)
        : "r"(base)
        : "d1", "cc", "memory");

    return result;
}

LONG pcDispatchLoadView(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics)
{
    MonitorViewFunction method = default_method;

    if (view) {
        struct ViewExtra* extra = GfxLookUp(view);

        if (extra && extra->Monitor) {
            method = extra->Monitor->ms_LoadView;
        }
    }

    // The selected owner performs activation, locking and any counter update.
    // Do not change ActiView or select GfxBase.default_monitor in this wrapper.
    return callMonitorView(view, method, graphics);
}

LONG pcDispatchMrgCop(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics)
{
    ++graphics->SpecialCounter;

    if (!view) {
        return 0;
    }

    MonitorViewFunction method = default_method;
    struct ViewExtra* extra = GfxLookUp(view);

    if (extra && extra->Monitor) {
        method = extra->Monitor->ms_MrgCop;
    }

    return callMonitorView(view, method, graphics);
}

LONG pcMrgCop(struct View* view asm("a1"), struct GfxBase* graphics asm("a6"))
{
    // The SDK callback type omits its implicit A6 argument. The dispatcher
    // supplies both A6 and the stack View when entering the native method.
    return pcDispatchMrgCop(view, (APTR)pcMergeNativeView, graphics);
}

void pcDispatchScrollVPort(struct ViewPort* viewport, ViewPortScrollFunction default_method,
    struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5)
{
    ViewPortScrollFunction method = default_method;
    const struct ColorMap* map = viewport->ColorMap;

    if (map && map->Type && map->cm_vpe && map->cm_vpe->VecTable) {
        method = ((const ViewPortScrollFunction*)map->cm_vpe->VecTable)[2];
    }

    // Dispatch precedes native visibility/locking checks. A foreign driver
    // receives hidden viewports too, and owns its own synchronization.
    register struct ViewPort* argument asm("a0") = viewport;
    register APTR entry asm("a1") = (APTR)method;
    register struct GfxBase* base asm("a6") = graphics;
    register ULONG field_address asm("d3") = caller_d3;
    register ULONG fetch_phase asm("d5") = caller_d5;

    __asm__ volatile(
        "jsr (%1)"
        : "+r"(argument), "+r"(entry)
        : "r"(base), "r"(field_address), "r"(fetch_phase)
        : "d0", "d1", "cc", "memory");
}

void pcScrollVPort(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"),
    ULONG caller_d3 asm("d3"), ULONG caller_d5 asm("d5"))
{
    pcDispatchScrollVPort(viewport, (APTR)pcNativeScrollVPort, graphics, caller_d3, caller_d5);
}

void pcNativeScrollVPort(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"),
    ULONG caller_d3 asm("d3"), ULONG caller_d5 asm("d5"))
{
    // Geometry, color selection and raster origins precede list construction.
    static const UWORD steps = 3;

    pcScrollNativeViewPort(viewport, (APTR)pcNativeViewPortVectors, (APTR)pcNativeViewPortVectors,
        &steps, graphics, caller_d3, caller_d5);
}

static ULONG normalizeDisplayID(ULONG id)
{
    if (!(id & EXTENDED_MODE)) {
        id &= 0xffff9efdUL;
    } else if ((id & MONITOR_ID_MASK) == EXTENDED_MODE) {
        id &= ~EXTENDED_MODE;
    }

    return id;
}

static ULONG viewPortDisplayID(struct View* view, struct ViewPort* viewport, struct GfxBase* graphics)
{
    const struct ColorMap* map = viewport->ColorMap;
    ULONG id = INVALID_ID;

    if (map && map->Type) {
        const struct GraphicsDisplayDataRecord* record = map->CoerceDisplayInfo;

        if (!record) {
            record = map->NormalDisplayInfo;
        }

        if (record) {
            // A selected display record already contains the complete ID.
            // Do not normalize away any bits or substitute its parent's key.
            id = record->record.id;
        }
    }

    if (id == (ULONG)INVALID_ID) {
        id = normalizeDisplayID(GetVPModeID(viewport));

        if (id == (ULONG)INVALID_ID) {
            id = normalizeDisplayID(pcViewportModes(viewport));

            if (!(view->Modes & EXTENDED_MODE)) {
                // Both paths read the original mode flags, not DisplayFlags.
                id |= graphics->Flags & PAL ? PAL_MONITOR_ID : NTSC_MONITOR_ID;
            }
        }
    }

    return id;
}

BOOL pcGetViewPortBuildInfo(struct View* view, struct ViewPort* viewport, struct VecInfo* info,
    struct GfxBase* graphics)
{
    ULONG id = viewPortDisplayID(view, viewport, graphics);

    // A public query owns resolution and can be supplied by another driver.
    // Keep the original nonzero-count rule, including a partial VecInfo reply.
    return GetDisplayInfoData(NULL, (APTR)info, sizeof(*info), DTAG_VEC, id) != 0;
}

static UWORD buildViewPort(struct View* view, struct ViewPort* viewport, struct ViewPortExtra* extra,
    const struct VecInfo* info, APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics);

ULONG pcMakeVPort(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"), ULONG caller_d6 asm("d6"))
{
    ++graphics->SpecialCounter;
    struct VecInfo info;

    if (view && viewport) {
        ULONG id = viewPortDisplayID(view, viewport, graphics);
        const struct ColorMap* map = viewport->ColorMap;
        struct ViewPortExtra* extra = map && map->Type ? map->cm_vpe : NULL;

        // A query may replace the ColorMap. Preparation receives the extra
        // captured before that callback, while driver selection sees its edits.
        if (GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_VEC, id)) {
            return buildViewPort(view, viewport, extra, &info, (APTR)pcNativeViewPortVectors,
                (APTR)pcNativeViewPortVectors, graphics);
        }
    }

    // The original leaves D6 as its result until preparation starts. Preserve
    // that early-return value instead of inventing a new public error code.
    if (caller_d6 == MVP_OFF_BOTTOM) {
        pcFreeCopList(viewport->DspIns, graphics);
        viewport->DspIns = NULL;

        return MVP_OK;
    }

    return caller_d6;
}

BOOL pcGetViewPortScrollInfo(struct ViewPort* viewport, struct ViewPortScrollInfo* info,
    struct GfxBase* graphics)
{
    if (viewport->Modes & VP_HIDE) {
        return FALSE;
    }

    info->modes = pcViewportModes(viewport);
    const struct ColorMap* map = viewport->ColorMap;
    info->field_extra = map && map->Type ? map->cm_vpe : NULL;
    info->update_intermediate = !map || !map->Type || !(map->AuxFlags & CMAF_NO_INTERMED_UPDATE);
    info->view = graphics->ActiView;

    if (!info->view) {
        return FALSE;
    }

    info->view_modes = info->view->Modes;
    ULONG id = viewPortDisplayID(info->view, viewport, graphics);
    map = viewport->ColorMap;
    info->extra = map && map->Type ? map->cm_vpe : NULL;

    // Resolve the ID first: GetVPModeID may change the map or raster. Fetch
    // rejection here applies only when the current extended map has an extra.
    if (info->extra && (WORD)pcViewportFetchMode(viewport, graphics) == -1) {
        return FALSE;
    }

    // Unlike MakeVPort, native scrolling uses the library's own reader. That
    // reader still calls public FindDisplayInfo and the record's query method.
    return pcGetDisplayInfoData(NULL, (APTR)&info->driver, sizeof(info->driver), DTAG_VEC, id, graphics) != 0;
}

static APTR viewPortBuildVectors(const struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors)
{
    const struct ColorMap* map = viewport->ColorMap;
    const struct ViewPortExtra* extra = map && map->Type ? map->cm_vpe : NULL;
    APTR vectors = NULL;

    // Dual playfields use the queried driver. A single playfield consults
    // that query only after finding an extended map and its ViewPortExtra;
    // absent/legacy maps and absent extras select the native default directly.
    if (viewport->Modes & DUALPF) {
        vectors = info->Vec;
    } else if (extra) {
        vectors = extra->VecTable ? extra->VecTable : info->Vec;
    }

    if (!vectors) {
        vectors = viewport->Modes & DUALPF ? dual_vectors : default_vectors;
    }

    return vectors;
}

void pcPrepareViewPortScroll(struct View* view, struct ViewPort* viewport, struct ViewPortExtra* extra,
    const struct VecInfo* info, APTR default_vectors, APTR dual_vectors, const UWORD* steps,
    APTR work, struct GfxBase* graphics)
{
    APTR vectors = viewPortBuildVectors(viewport, info, default_vectors, dual_vectors);
    const APTR* methods = ((const APTR*)vectors)[3];
    ViewPortBuildInitFunction initialize = (ViewPortBuildInitFunction)*methods++;
    UWORD result = initialize(view, viewport, &extra, info->Data, work, graphics);

    if (!result) {
        // Scrolling runs only the driver's preparation prefix. In contrast
        // to MakeVPort, individual stages do not stop the sequence on error,
        // and an absent VecTable is not bound as a side effect.
        UWORD remaining = *steps;

        while (remaining--) {
            ViewPortBuildFunction method = (ViewPortBuildFunction)*methods++;
            method(view, viewport, extra, graphics);
        }
    }

    // Skip list construction and publication, retaining the last method as
    // cleanup even when initialization failed or selected a different extra.
    ViewPortBuildFunction cleanup;

    do {
        cleanup = (ViewPortBuildFunction)*methods++;
    } while (*methods);

    cleanup(view, viewport, extra, graphics);
}

void pcScrollNativeViewPort(struct ViewPort* viewport, APTR default_vectors, APTR dual_vectors,
    const UWORD* steps, struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5)
{
#if !GRAPHICS_CHIPSET
    (void)caller_d5;
#endif
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ViewPortScrollInfo info;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    if (pcGetViewPortScrollInfo(viewport, &info, graphics)) {
        struct ViewPortBuildState state = {0};
        pcPrepareViewPortScroll(info.view, viewport, info.extra, &info.driver,
            default_vectors, dual_vectors, steps, &state, graphics);

        // Cleanup may release its extra or change the current map, bitmaps
        // and list. Retain the saved output owner/modes, but read source
        // pointers and pitches only now. The workspace belongs to this call,
        // independently of the extra's lifetime or its DriverData pointer.
        struct ViewPortPlaneFetch fetches[8];
        pcReadViewPortScrollFetches(fetches, viewport, info.modes,
            state.source_x, state.source_y, state.source_stride);

        if (viewport->DspIns) {
#if GRAPHICS_CHIPSET
            if (pcUsesChipset(graphics)) {
                pcScrollChipsetViewPort(viewport, &state, &info, fetches, graphics, caller_d3, caller_d5);
            } else
#endif
            {
                pcApplyViewPortPlaneScroll(pcPreparedViewPorts(graphics), pcMergedViews(graphics),
                    viewport->DspIns, info.field_extra, fetches, state.interlaced, info.view_modes,
                    info.modes, info.update_intermediate, caller_d3);
            }
        }
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

UWORD pcBuildViewPort(struct View* view, struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics)
{
    struct ColorMap* map = viewport->ColorMap;
    struct ViewPortExtra* extra = map && map->Type ? map->cm_vpe : NULL;

    return buildViewPort(view, viewport, extra, info, default_vectors, dual_vectors, graphics);
}

static UWORD buildViewPort(struct View* view, struct ViewPort* viewport, struct ViewPortExtra* extra,
    const struct VecInfo* info, APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics)
{
    APTR vectors = viewPortBuildVectors(viewport, info, default_vectors, dual_vectors);

    const APTR* methods = ((const APTR*)vectors)[3];
    ViewPortBuildInitFunction initialize = (ViewPortBuildInitFunction)*methods++;
    UWORD work[VIEWPORT_BUILD_WORK_SIZE / sizeof(UWORD)];
    UWORD result = initialize(view, viewport, &extra, info->Data, work, graphics);

    if (!result) {
        // The initializer may replace the extra pointer. Retain its selected
        // driver; otherwise bind the queried table or single-playfield default.
        if (!extra->VecTable) {
            extra->VecTable = info->Vec ? info->Vec : default_vectors;
        }

        while (*methods) {
            ViewPortBuildFunction method = (ViewPortBuildFunction)*methods;
            result = method(view, viewport, extra, graphics);

            if (result) {
                break;
            }

            ++methods;
        }
    }

    if (*methods) {
        // On failure, skip remaining build stages but still run the final
        // cleanup method. Its result must not replace the original error.
        ViewPortBuildFunction cleanup;

        do {
            cleanup = (ViewPortBuildFunction)*methods++;
        } while (*methods);

        cleanup(view, viewport, extra, graphics);
    }

    if (result == MVP_OFF_BOTTOM) {
        pcFreeCopList(viewport->DspIns, graphics);
        viewport->DspIns = NULL;
        result = MVP_OK;
    }

    return result;
}

ULONG pcInitViewPortBuild(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra** extra, APTR data, APTR work, struct GfxBase* graphics asm("a6"))
{
    WORD fetch = pcViewportFetchMode(viewport, graphics);

    if (fetch == -1) {
        return MVP_NO_DISPLAY;
    }

    UWORD result = pcAcquireViewPortExtra(viewport, extra, graphics);

    if (result != MVP_OK) {
        return result;
    }

    struct ViewExtra* view_extra = NULL;

    if (view->Modes & EXTENDED_MODE) {
        view_extra = GfxLookUp(view);
    }

    struct ViewPortBuildState* state = work;
    state->mode_data = data;
    state->monitor = view_extra && view_extra->Monitor ? view_extra->Monitor : graphics->natural_monitor;
    state->modes = pcViewportModes(viewport);
    state->fetch_mode = fetch;
    state->interlaced = (state->modes & (LACE | DOUBLESCAN)) == LACE;
    (*extra)->DriverData[0] = state;
    (*extra)->Origin[0].x = 0;
    (*extra)->Origin[1].x = 0;

    return MVP_OK;
}

ULONG pcBuildViewPortGeometry(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    struct ViewPortBuildState* state = extra->DriverData[0];
    const struct ViewPortBuildMode* mode = state->mode_data;
    const struct MonitorSpec* monitor = state->monitor;
    const struct ColorMap* map = viewport->ColorMap;
    ULONG id = GetVPModeID(viewport);
    WORD width = viewport->DWidth;
    WORD height = viewport->DHeight;
    WORD view_x = view->DxOffset;
    UWORD interlace = state->interlaced ? 1 : 0;

    if ((id >> 16) == (A2024_MONITOR_ID >> 16)) {
        width = id == A2024TENHERTZ_KEY ? 352 : 512;
        height = graphics->DisplayFlags & PAL ? 256 : 200;

        if (id == A2024TENHERTZ_KEY) {
            view_x = (WORD)(view_x - 16);
            state->fetch_mode = 0;
        }
    }

    state->clip = extra->DisplayClip;
    BOOL clipped = state->clip.MinX || state->clip.MinY || state->clip.MaxX || state->clip.MaxY;

    if (clipped && map && map->Type) {
        const struct GraphicsDisplayDataRecord* record = map->CoerceDisplayInfo;

        if (!record) {
            record = map->NormalDisplayInfo;
        }

        if (record) {
            if (state->clip.MinX < record->bounds.MinX) {
                state->clip.MinX = record->bounds.MinX;
            }

            if (state->clip.MinY < record->bounds.MinY) {
                state->clip.MinY = record->bounds.MinY;
            }

            if (state->clip.MaxX > record->bounds.MaxX) {
                state->clip.MaxX = record->bounds.MaxX;
            }

            if (state->clip.MaxY > record->bounds.MaxY) {
                state->clip.MaxY = record->bounds.MaxY;
            }
        }
    }

    WORD start_x = viewport->DxOffset;
    WORD start_y = (WORD)(viewport->DyOffset + interlace) & ~interlace;
    WORD stop_y = (WORD)(start_y + height);
    WORD stop_x;

    if (clipped) {
        if (start_x < state->clip.MinX) {
            start_x = state->clip.MinX;
        }

        // The original adds the width to the clipped start and compares the
        // full sum before narrowing it. Preserve this ordering at WORD limits.
        LONG right = (LONG)start_x + width;
        LONG limit = (LONG)state->clip.MaxX + 1;
        stop_x = right < limit ? right : limit;

        if (start_y < state->clip.MinY) {
            start_y = state->clip.MinY;
        }
    } else {
        stop_x = (WORD)(start_x + width);
    }

    if (stop_y < start_y) {
        stop_y = start_y;
    }

    if (mode->scan_doubled) {
        start_y = (WORD)(start_y * 2);
        stop_y = (WORD)(stop_y * 2);
    }

    LONG x_scale = 1UL << mode->horizontal_shift;
    LONG y_scale = 1UL << interlace;
    start_x = (WORD)(start_x + view_x * x_scale);
    start_y = (WORD)(start_y + view->DyOffset * y_scale);
    state->stop_x = (WORD)(stop_x + view_x * x_scale);
    stop_y = (WORD)(stop_y + view->DyOffset * y_scale);

    WORD min_x = (WORD)(monitor->DeniseMinDisplayColumn * x_scale);
    WORD min_y = (WORD)(monitor->min_row * y_scale);
    LONG max_y = ((LONG)monitor->total_rows + 1) * y_scale;
    state->start_x = start_x > min_x ? start_x : min_x;
    state->start_y = start_y > min_y ? start_y : min_y;
    state->stop_y = stop_y < max_y ? stop_y : max_y;

    if (!clipped) {
        WORD delta_x = (WORD)(start_x - min_x);
        WORD delta_y = (WORD)(start_y - min_y);
        state->clip.MinX = (WORD)(viewport->DxOffset - (delta_x < 0 ? delta_x : 0));
        state->clip.MinY = (WORD)(viewport->DyOffset - (delta_y < 0 ? delta_y : 0));
    }

    if (!(extra->Flags & VPXF_LAST)) {
        state->stop_y &= ~interlace;
    }

    if (mode->extend_to_monitor_bottom) {
        state->stop_y = (WORD)(monitor->total_rows * y_scale);
    }

    // Hardware backends encode their own display registers. Keep the logical
    // horizontal end here, including windows that cross a monitor boundary.
    return ((UWORD)state->start_y >> interlace) > monitor->total_rows ? MVP_OFF_BOTTOM : MVP_OK;
}

static LONG sourceRow(const struct ViewPort* viewport, const struct RasInfo* raster,
    WORD clip_top, BOOL interlaced)
{
    LONG alignment = interlaced ? 1 : 0;
    LONG origin = ((LONG)viewport->DyOffset + alignment) & ~alignment;
    LONG clipped = origin - clip_top;

    if (clipped > 0) {
        clipped = 0;
    }

    return -(LONG)(WORD)(clipped - raster->RyOffset);
}

static LONG sourceColumn(const struct ViewPort* viewport, const struct RasInfo* raster,
    WORD clip_left, const struct ViewPortBuildMode* mode)
{
    LONG clipped = (LONG)viewport->DxOffset - clip_left;

    if (clipped > 0) {
        clipped = 0;
    }

    // The original narrows horizontal displacement at the finest pixel
    // resolution. Preserve its wrap, then return to this mode's pixel units.
    ULONG scale = 1UL << (2 - mode->horizontal_shift);

    return -(LONG)(WORD)((ULONG)(clipped - raster->RxOffset) * scale) / (LONG)scale;
}

ULONG pcBuildViewPortRasters(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    (void)view;
    (void)graphics;
    struct ViewPortBuildState* state = extra->DriverData[0];
    const struct ViewPortBuildMode* mode = state->mode_data;
    const struct RasInfo* raster = viewport->RasInfo;
    state->source_y[0] = state->source_y[1] = 0;
    state->source_x[0] = state->source_x[1] = 0;
    state->source_stride[0] = state->source_stride[1] = 0;

    if (raster && raster->BitMap) {
        state->source_stride[0] = raster->BitMap->BytesPerRow;
        state->source_y[0] = sourceRow(viewport, raster, state->clip.MinY, state->interlaced);
        state->source_x[0] = sourceColumn(viewport, raster, state->clip.MinX, mode);
    }

    if (raster && (state->modes & DUALPF)) {
        if (raster->Next) {
            raster = raster->Next;
        }

        if (raster->BitMap) {
            state->source_stride[1] = raster->BitMap->BytesPerRow;
            state->source_y[1] = sourceRow(viewport, raster, state->clip.MinY, state->interlaced);
            state->source_x[1] = sourceColumn(viewport, raster, state->clip.MinX, mode);
        }
    }

    return MVP_OK;
}

ULONG pcBuildViewPortColors(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    (void)view;
    struct ViewPortBuildState* state = extra->DriverData[0];
    pcViewPortColorMask(viewport, state->color_mask, graphics);

    return MVP_OK;
}

void pcReadViewPortSpriteLayout(struct ViewPortSpriteLayout* layout, const struct ColorMap* map,
    const struct GfxBase* graphics)
{
    layout->even_bank = 1;
    layout->odd_bank = 1;
    layout->resolution = SPRITERESN_ECS;
    layout->border_sprites = FALSE;
    layout->border_blank = FALSE;

    if (map && map->Type) {
        // The original packs these WORD bases together before selecting
        // banks. High bits of an odd base also contribute to the even bank.
        UWORD banks = (map->SpriteBase_Even & 0xf0) | (map->SpriteBase_Odd >> 4);
        layout->even_bank = (banks >> 4) & 15;
        layout->odd_bank = banks & 15;
    }

    if (map) {
        // Unlike palette bases, these settings also apply to legacy maps.
        // Preserve the byte rotation's border contribution for out-of-range
        // resolution values rather than treating them as a new valid mode.
        UBYTE resolution = (BYTE)map->SpriteResolution < 0 ? map->SpriteResDefault : map->SpriteResolution;
        layout->resolution = resolution & 3;
        layout->border_sprites = (map->Flags & BORDERSPRITES) || (resolution & 8);
        layout->border_blank = (map->Flags & BORDER_BLANKING) || (resolution & 0x80);
    }

    // Global defaults participate even when the viewport has no ColorMap.
    layout->resolution |= graphics->BP3Bits >> 6;
    layout->border_sprites |= (graphics->BP3Bits & 2) != 0;
    layout->border_blank |= (graphics->BP3Bits & 0x20) != 0;
}

void pcReadViewPortListControls(UWORD* display_control, UWORD* sprite_control,
    const struct ColorMap* map, const struct GfxBase* graphics)
{
    struct PlayfieldPaletteLayout palette;
    struct ViewPortSpriteLayout sprites;
    pcReadPlayfieldPaletteLayout(&palette, map);
    pcReadViewPortSpriteLayout(&sprites, map, graphics);

    UWORD control = graphics->BP3Bits;
    unsigned offset = palette.second_offset;
    unsigned shift = 0;

    while (offset > 1) {
        offset >>= 1;
        ++shift;
    }

    control |= shift << 10;

    if (map) {
        // Retain the original byte rotation, including contributions outside
        // the two resolution bits for out-of-range public selector values.
        UBYTE resolution = (BYTE)map->SpriteResolution < 0 ? map->SpriteResDefault : map->SpriteResolution;
        control |= (UBYTE)((resolution >> 2) | (resolution << 6));

        if (map->Flags & BORDERSPRITES) {
            control |= 2;
        }

        if (map->Flags & BORDER_BLANKING) {
            control |= 0x20;
        }

        if (map->Flags & BORDER_NOTRANSPARENCY) {
            control |= 0x10;
        }
    }

    *display_control = control;
    *sprite_control = ((UWORD)palette.index_xor << 8) | (sprites.even_bank << 4) | sprites.odd_bank;
}

static BOOL validViewPortSpriteLayout(const struct ViewPortSpriteLayout* layout, UBYTE horizontal_shift)
{
    return layout && horizontal_shift <= 2 && layout->resolution <= SPRITERESN_35NS &&
        layout->even_bank <= 15 && layout->odd_bank <= 15;
}

BOOL pcApplyViewPortSpriteLayout(struct GraphicsFramebufferSprites* sprites,
    const struct ViewPortSpriteLayout* layout, UWORD modes, UBYTE horizontal_shift)
{
    if (!sprites || !validViewPortSpriteLayout(layout, horizontal_shift)) {
        return FALSE;
    }

    unsigned resolution = layout->resolution == SPRITERESN_ECS ?
        ((modes & SUPERHIRES) ? 1 : 0) : layout->resolution - 1;
    BYTE shift = (int)horizontal_shift - (int)resolution;
    sprites->even_bank = layout->even_bank;
    sprites->odd_bank = layout->odd_bank;

    for (unsigned channel = 0; channel < 8; ++channel) {
        sprites->images[channel].x_shift = shift;
    }

    return TRUE;
}

BOOL pcRenderViewPortSprites(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const struct ViewPortSpriteLayout* layout,
    UWORD modes, UBYTE horizontal_shift, const struct GraphicsFramebufferPalette* palette,
    const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window)
{
    if (!sprites || !layout || !palette || !region || !window ||
        region->MinX > region->MaxX || region->MinY > region->MaxY ||
        window->MinX > window->MaxX || window->MinY > window->MaxY) {
        return FALSE;
    }

    struct GraphicsFramebufferSprites selected = *sprites;

    if (!pcApplyViewPortSpriteLayout(&selected, layout, modes, horizontal_shift)) {
        return FALSE;
    }

    LONG left = region->MinX, top = region->MinY;
    LONG right = region->MaxX, bottom = region->MaxY;

    if (!layout->border_sprites || layout->border_blank) {
        if (left < window->MinX) {
            left = window->MinX;
        }

        if (top < window->MinY) {
            top = window->MinY;
        }

        if (right > window->MaxX) {
            right = window->MaxX;
        }

        if (bottom > window->MaxY) {
            bottom = window->MaxY;
        }
    }

    return pcRenderSpriteFramebufferArea(framebuffer, &selected, palette->colors, occlusion,
        left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
}

BOOL pcInitViewFieldSpriteRuns(struct ViewFieldSpriteRuns* state, struct NativeSpriteField* field,
    WORD origin_x, WORD origin_y)
{
    if (!state || !field || field->line > field->limit || field->limit > 1024) {
        return FALSE;
    }

    *state = (struct ViewFieldSpriteRuns){0};
    state->field = field;
    state->position.y = field->line;
    state->origin_x = origin_x;
    state->origin_y = origin_y;
    state->line = field->line;

    return TRUE;
}

static BOOL fieldPositionBefore(const Point* first, const Point* second)
{
    return first->y < second->y || (first->y == second->y && first->x < second->x);
}

static WORD clipSpriteRunCoordinate(LONG coordinate, WORD first, WORD end)
{
    if (coordinate < first) {
        return first;
    }

    return coordinate > end ? end : coordinate;
}

static BOOL validViewFieldSpriteRun(const struct ViewFieldSpriteRuns* state, const struct ViewFieldRun* run)
{
    if (!state || !state->field || !run ||
        run->start.x < 0 || run->start.x > 254 || run->end.x < 0 || run->end.x > 254 ||
        ((run->start.x | run->end.x) & 1) || run->start.y < 0 ||
        fieldPositionBefore(&run->start, &state->position) || fieldPositionBefore(&run->end, &run->start)) {
        return FALSE;
    }

    const struct NativeSpriteField* field = state->field;

    if (field->line > field->limit || field->limit > 1024 || run->end.y > field->limit ||
        (run->end.y == field->limit && run->end.x) ||
        field->line != (state->row_ready ? state->line + 1 : state->position.y)) {
        return FALSE;
    }

    return TRUE;
}

BOOL pcRenderViewFieldSpriteRun(struct GraphicsFramebuffer* framebuffer, struct ViewFieldSpriteRuns* state,
    const struct ViewFieldRun* run, const struct ViewPortSpriteLayout* layout, UWORD modes,
    const struct GraphicsFramebufferPalette* palette, const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window)
{
    if (!framebuffer || !palette || !region || !window ||
        region->MinX > region->MaxX || region->MinY > region->MaxY ||
        window->MinX > window->MaxX || window->MinY > window->MaxY ||
        !validViewFieldSpriteRun(state, run) || !validViewPortSpriteLayout(layout, 2)) {
        return FALSE;
    }

    struct NativeSpriteField* field = state->field;

    if (!fieldPositionBefore(&run->start, &run->end)) {
        return TRUE;
    }

    for (UWORD line = run->start.y; line < run->end.y || (line == run->end.y && run->end.x); ++line) {
        // Consume skipped lines too, but never fetch twice when a later
        // palette/priority interval still uses this line's retained data.
        while (field->line <= line) {
            state->line = field->line;

            if (pcNextNativeSpriteLine(field, &state->row, state->origin_x, state->origin_y, 2) != 1) {
                return FALSE;
            }

            state->row_ready = TRUE;
        }

        struct Rectangle selected = *region;

        if (line == run->start.y) {
            selected.MinX = clipSpriteRunCoordinate((LONG)run->start.x * 8 - state->origin_x,
                region->MinX, region->MaxX);
        }

        if (line == run->end.y) {
            selected.MaxX = clipSpriteRunCoordinate((LONG)run->end.x * 8 - state->origin_x,
                region->MinX, region->MaxX);
        }

        LONG y = (LONG)line - state->origin_y;
        selected.MinY = clipSpriteRunCoordinate(y, region->MinY, region->MaxY);
        selected.MaxY = clipSpriteRunCoordinate(y + 1, region->MinY, region->MaxY);

        if (!pcRenderViewPortSprites(framebuffer, &state->row, layout, modes, 2,
            palette, occlusion, &selected, window)) {
            return FALSE;
        }
    }

    state->position = run->end;

    return TRUE;
}

BOOL pcRenderViewPortSpriteBand(struct GraphicsFramebuffer* framebuffer, struct NativeSpriteField* field,
    UWORD limit, WORD origin_x, WORD origin_y, const struct ViewPortSpriteLayout* layout, UWORD modes,
    const struct GraphicsFramebufferPalette* palette, const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window)
{
    if (!framebuffer || !field || field->line > limit || limit > field->limit || field->limit > 1024 ||
        !layout || !palette || !region || !window ||
        region->MinX > region->MaxX || region->MinY > region->MaxY ||
        window->MinX > window->MaxX || window->MinY > window->MaxY) {
        return FALSE;
    }

    struct GraphicsFramebufferSprites sprites = {0};

    if (!pcApplyViewPortSpriteLayout(&sprites, layout, modes, 2)) {
        return FALSE;
    }

    while (field->line < limit) {
        // Advance every channel before cropping. In particular a repeated
        // row may have been fetched outside this viewport or the visible crop.
        if (pcNextNativeSpriteLine(field, &sprites, origin_x, origin_y, 2) != 1 ||
            !pcRenderViewPortSprites(framebuffer, &sprites, layout, modes, 2,
                palette, occlusion, region, window)) {
            return FALSE;
        }
    }

    return TRUE;
}

static UBYTE spritePriorityMask(unsigned priority)
{
    return priority < 4 ? (15U << priority) & 15 : 0;
}

struct ViewPortDisplaySnapshot* pcAllocViewPortDisplay(const struct ViewPort* viewport,
    const struct ViewPortBuildState* state, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ViewPortDisplaySnapshot* snapshot = AllocMem(sizeof(*snapshot), MEMF_PUBLIC | MEMF_CLEAR);

    if (!snapshot) {
        return NULL;
    }

    snapshot->viewport_identity = (ULONG)viewport;
    snapshot->display.MinX = state->start_x;
    snapshot->display.MinY = state->start_y;
    snapshot->display.MaxX = state->stop_x;
    snapshot->display.MaxY = state->stop_y;
    snapshot->modes = state->modes;
    snapshot->rasters.interlaced = state->interlaced != 0;
    const struct ColorMap* map = viewport->ColorMap;
    snapshot->dual_disabled = map && map->Type && (map->AuxFlags & CMAF_DUALPF_DISABLE);
    pcReadPlayfieldPaletteLayout(&snapshot->palette_layout, map);
    pcReadViewPortSpriteLayout(&snapshot->sprites, map, graphics);

    // VideoControl's priority query uses the map's attached viewport. An
    // unattached map selects priority four, even if this viewport was edited.
    UBYTE priorities = map && map->cm_vp ? map->cm_vp->SpritePriorities : 0x24;

    for (unsigned field = 0; field < 2; ++field) {
        unsigned priority = (priorities >> (3 * field)) & 7;
        snapshot->sprite_mask[field] = spritePriorityMask(priority);
    }

    for (unsigned bank = 0; bank < 8; ++bank) {
        snapshot->color_mask[bank] = state->color_mask[bank];
    }

    pcReadPreparedPalette(&snapshot->palette, map, snapshot->color_mask);
    const struct RasInfo* raster = viewport->RasInfo;

    for (unsigned field = 0; field < 2; ++field) {
        snapshot->rasters.source_x[field] = state->source_x[field];
        snapshot->rasters.source_y[field] = state->source_y[field];

        if (raster && raster->BitMap) {
            snapshot->rasters.bitmap[field] = *raster->BitMap;
            snapshot->rasters.present |= 1UL << field;
        }

        raster = raster && (state->modes & DUALPF) ? raster->Next : NULL;
    }

    const struct ViewPortBuildMode* mode = state->mode_data;
    snapshot->horizontal_shift = mode ? mode->horizontal_shift : 255;
    snapshot->vertical_shift = state->interlaced ? 1 : 0;
    snapshot->ehb_precision = -1;
    snapshot->ham_format = -1;

    if (mode) {
        snapshot->ehb_precision = mode->ehb_precision;
        snapshot->ham_format = snapshot->rasters.bitmap[0].Depth < 7 ? GRAPHICS_HAM6 :
            mode->ham8_planar ? GRAPHICS_HAM8_PLANAR : GRAPHICS_HAM8;
    }

    return snapshot;
}

BOOL pcGetViewPortFieldArea(const struct ViewPortDisplaySnapshot* display,
    struct GraphicsFramebufferScaleArea* area)
{
    if (!display || !area || display->horizontal_shift > 2 || display->vertical_shift > 1 ||
        display->display.MinX > display->display.MaxX || display->display.MinY > display->display.MaxY) {
        return FALSE;
    }

    LONG top = display->display.MinY;
    LONG bottom = display->display.MaxY;

    if (display->vertical_shift) {
        // Round both endpoints down, retaining the field-line phase even
        // for signed coordinates. Do not halve an already clipped height.
        top = (top - (top & 1)) / 2;
        bottom = (bottom - (bottom & 1)) / 2;
    }

    struct GraphicsFramebufferScaleArea placement = {0};
    placement.width = (LONG)display->display.MaxX - display->display.MinX;
    placement.height = bottom - top;
    placement.x_shift = 2 - display->horizontal_shift;
    placement.dest_x = (LONG)display->display.MinX * (1L << placement.x_shift);
    placement.dest_y = top;
    *area = placement;

    return TRUE;
}

struct ViewFieldBuffers* pcAllocViewFieldBuffers(const struct ViewDisplaySnapshot* display,
    unsigned field, const struct FramebufferMode* format, ULONG width, ULONG height, struct GfxBase* graphics)
{
    if (!display || field >= 2 || !format || !graphics ||
        !width || !height || width > INT16_MAX || height > INT16_MAX) {
        return NULL;
    }

    unsigned bytes;

    switch (format->format) {
        case FRAMEBUFFER_RGB565:
        case FRAMEBUFFER_RGB555:
            bytes = 2;
            break;
        case FRAMEBUFFER_RGB888:
            bytes = 3;
            break;
        case FRAMEBUFFER_XRGB8888:
        case FRAMEBUFFER_XBGR8888:
            bytes = 4;
            break;
        default:
            return NULL;
    }

    if (format->pixel_bytes != bytes) {
        return NULL;
    }

    ULONG scratch_width = 1, scratch_height = 1;
    ULONG cursor = display->first_port[field];
    ULONG remaining = display->count;

    while (cursor != ~0UL) {
        struct ViewFieldPortEvent event;
        struct GraphicsFramebufferScaleArea area;

        if (!remaining-- || pcNextViewFieldSetupEvent(display, field, &cursor, &event) != 1 ||
            !event.port->prepared || !pcGetViewPortFieldArea(event.port->prepared->display, &area)) {
            return NULL;
        }

        if (area.width > scratch_width) {
            scratch_width = area.width;
        }

        if (area.height > scratch_height) {
            scratch_height = area.height;
        }
    }

    // WORD endpoint differences fit in UWORD, so each pixel count fits ULONG.
    // Bound their sum before multiplying by color bytes plus one mask byte.
    ULONG field_pixels = width * height;
    ULONG scratch_pixels = scratch_width * scratch_height;
    ULONG available = (UINT32_MAX - sizeof(struct ViewFieldBuffers)) / (bytes + 1);

    if (field_pixels > available || scratch_pixels > available - field_pixels) {
        return NULL;
    }

    ULONG size = sizeof(struct ViewFieldBuffers) + (field_pixels + scratch_pixels) * (bytes + 1);
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct ViewFieldBuffers* buffers = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!buffers) {
        return NULL;
    }

    buffers->allocation_size = size;
    buffers->field.mode.width = width;
    buffers->field.mode.height = height;
    buffers->field.mode.format = format->format;
    buffers->field.mode.pixel_bytes = bytes;
    buffers->field.mode.little_endian = format->little_endian;
    buffers->field.pixels = buffers->pixels;
    buffers->field.stride = width * bytes;
    buffers->field.size = field_pixels * bytes;

    buffers->scratch.mode = buffers->field.mode;
    buffers->scratch.mode.width = scratch_width;
    buffers->scratch.mode.height = scratch_height;
    buffers->scratch.pixels = buffers->field.pixels + buffers->field.size;
    buffers->scratch.stride = scratch_width * bytes;
    buffers->scratch.size = scratch_pixels * bytes;

    buffers->occlusion.pixels = buffers->scratch.pixels + buffers->scratch.size;
    buffers->occlusion.stride = width;
    buffers->occlusion.size = field_pixels;
    buffers->occlusion.scratch = buffers->occlusion.pixels + field_pixels;
    buffers->occlusion.scratch_stride = scratch_width;
    buffers->occlusion.scratch_size = scratch_pixels;

    return buffers;
}

void pcFreeViewFieldBuffers(struct ViewFieldBuffers* buffers, struct GfxBase* graphics)
{
    if (buffers) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        FreeMem(buffers, buffers->allocation_size);
    }
}

void pcFreeViewPortDisplay(struct ViewPortDisplaySnapshot* snapshot, struct GfxBase* graphics)
{
    if (snapshot) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        FreeMem(snapshot, sizeof(*snapshot));
    }
}

static BOOL renderFetchedViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, enum GraphicsEhbPrecision precision,
    enum GraphicsHamFormat format, LONG dest_x, LONG dest_y, ULONG width, ULONG height);

BOOL pcRenderViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!display || !rasters) {
        return FALSE;
    }

    if (display->modes & DUALPF) {
        return pcRenderDualViewPort(framebuffer, display, rasters, palette, dest_x, dest_y, width, height);
    }

    if (display->modes & HAM) {
        return pcRenderHamViewPort(framebuffer, display, rasters, palette, display->ham_format,
            dest_x, dest_y, width, height);
    }

    if (display->modes & EXTRA_HALFBRITE) {
        return pcRenderEhbViewPort(framebuffer, display, rasters, palette, display->ehb_precision,
            dest_x, dest_y, width, height);
    }

    return pcRenderIndexedViewPort(framebuffer, display, rasters, palette, dest_x, dest_y, width, height);
}

static BOOL fieldOcclusionFits(const struct ViewFieldOcclusion* occlusion,
    const struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebuffer* decoded)
{
    if (!occlusion->pixels || !occlusion->scratch || !occlusion->stride || !occlusion->scratch_stride ||
        framebuffer->mode.width > occlusion->stride ||
        framebuffer->mode.height > occlusion->size / occlusion->stride ||
        decoded->mode.width > occlusion->scratch_stride ||
        decoded->mode.height > occlusion->scratch_size / occlusion->scratch_stride) {
        return FALSE;
    }

    const UBYTE* pixels[] = {framebuffer->pixels, decoded->pixels, occlusion->pixels, occlusion->scratch};
    ULONG sizes[] = {framebuffer->size, decoded->size, occlusion->size, occlusion->scratch_size};

    for (unsigned first = 0; first < 4; ++first) {
        for (unsigned second = first + 1; second < 4; ++second) {
            uintptr_t a = (uintptr_t)pixels[first];
            uintptr_t b = (uintptr_t)pixels[second];

            if (a <= b ? b - a < sizes[first] : a - b < sizes[second]) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

static BOOL readViewPortFieldPlacement(const struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebuffer* scratch, const struct ViewPortDisplaySnapshot* display,
    LONG origin_x, LONG origin_y, const struct ViewFieldOcclusion* occlusion,
    struct GraphicsFramebuffer* decoded, struct GraphicsFramebufferScaleArea* area)
{
    if (!framebuffer || !scratch || !pcGetViewPortFieldArea(display, area) ||
        area->width > scratch->mode.width || area->height > scratch->mode.height ||
        framebuffer->mode.format != scratch->mode.format ||
        !!framebuffer->mode.little_endian != !!scratch->mode.little_endian) {
        return FALSE;
    }

    uintptr_t input = (uintptr_t)scratch->pixels;
    uintptr_t output = (uintptr_t)framebuffer->pixels;

    if (input <= output ? output - input < scratch->size : input - output < framebuffer->size) {
        return FALSE;
    }

    int64_t left = (int64_t)area->dest_x - origin_x;
    int64_t top = (int64_t)area->dest_y - origin_y;

    if (left < INT32_MIN || left > INT32_MAX || top < INT32_MIN || top > INT32_MAX) {
        return FALSE;
    }

    area->dest_x = left;
    area->dest_y = top;
    *decoded = *scratch;
    decoded->mode.width = area->width;
    decoded->mode.height = area->height;

    if (occlusion && !fieldOcclusionFits(occlusion, framebuffer, decoded)) {
        return FALSE;
    }

    return TRUE;
}

BOOL pcProjectViewPortSpan(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebuffer* scratch,
    const struct ViewPortDisplaySnapshot* display, LONG origin_x, LONG origin_y,
    ULONG row, ULONG first, ULONG end, const struct ViewFieldOcclusion* occlusion)
{
    struct GraphicsFramebuffer decoded;
    struct GraphicsFramebufferScaleArea area;

    if (!readViewPortFieldPlacement(framebuffer, scratch, display, origin_x, origin_y,
        occlusion, &decoded, &area) || row >= area.height || first > end || end > area.width) {
        return FALSE;
    }

    // first is bounded by the WORD-endpoint window width; at most two
    // expansion bits fit comfortably in ULONG before the signed origin sum.
    int64_t left = (int64_t)area.dest_x + (first << area.x_shift);
    int64_t top = (int64_t)area.dest_y + row;

    if (left < INT32_MIN || left > INT32_MAX || top < INT32_MIN || top > INT32_MAX) {
        return FALSE;
    }

    area.source_x = first;
    area.source_y = row;
    area.width = end - first;
    area.height = 1;
    area.dest_x = left;
    area.dest_y = top;

    if (occlusion) {
        struct GraphicsSpriteOcclusion mask = {
            occlusion->scratch, occlusion->scratch_stride, occlusion->scratch_size
        };

        // This validates both framebuffer descriptors and the selected area
        // before changing the mask. Matching formats and disjoint color storage
        // were checked above, so the following color copy cannot then reject.
        if (!pcScaleSpriteOcclusion(framebuffer, occlusion->pixels, occlusion->stride, occlusion->size,
            &decoded, &mask, &area)) {
            return FALSE;
        }
    }

    return pcScaleFramebufferArea(framebuffer, &decoded, &area);
}

BOOL pcRenderViewPortField(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, LONG origin_x, LONG origin_y, ULONG background,
    const struct ViewFieldOcclusion* occlusion)
{
    struct GraphicsFramebuffer decoded;
    struct GraphicsFramebufferScaleArea area;

    if (!rasters || !palette || !readViewPortFieldPlacement(framebuffer, scratch, display,
        origin_x, origin_y, occlusion, &decoded, &area)) {
        return FALSE;
    }

    if (!pcFillGraphicsFramebuffer(&decoded, background) ||
        !pcRenderViewPort(&decoded, display, rasters, palette, 0, 0, area.width, area.height)) {
        return FALSE;
    }

    if (occlusion) {
        // Missing source pixels keep the supplied background color but do
        // not hide sprites. Initialize only the viewport, preserving padding.
        for (ULONG y = 0; y < area.height; ++y) {
            UBYTE* row = occlusion->scratch + y * occlusion->scratch_stride;

            for (ULONG x = 0; x < area.width; ++x) {
                row[x] = 0;
            }
        }

        struct GraphicsSpriteOcclusion mask = {
            occlusion->scratch, occlusion->scratch_stride, occlusion->scratch_size
        };

        if (!pcWriteViewPortSpriteOcclusion(&decoded, occlusion->scratch, occlusion->scratch_stride,
            occlusion->scratch_size, display, rasters, 0, 0, area.width, area.height) ||
            !pcScaleSpriteOcclusion(framebuffer, occlusion->pixels, occlusion->stride, occlusion->size,
                &decoded, &mask, &area)) {
            return FALSE;
        }
    }

    return pcScaleFramebufferArea(framebuffer, &decoded, &area);
}

BOOL pcRenderIndexedViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!display || !rasters || !palette || !(rasters->present & 1) ||
        (display->modes & (HAM | EXTRA_HALFBRITE | DUALPF))) {
        return FALSE;
    }

    if (rasters->plane_fetches || rasters->interlaced) {
        return renderFetchedViewPort(framebuffer, display, rasters, palette, display->ehb_precision,
            display->ham_format, dest_x, dest_y, width, height);
    }

    struct GraphicsFramebufferPlacement placement = {
        rasters->source_x[0], rasters->source_y[0], dest_x, dest_y, width, height
    };
    struct GraphicsFramebufferArea area;

    if (!pcClipFramebufferArea(framebuffer, &rasters->bitmap[0], &placement, &area)) {
        return FALSE;
    }

    return pcRenderIndexedFramebuffer(framebuffer, &rasters->bitmap[0], palette->colors,
        &area, display->palette_layout.index_xor);
}

BOOL pcRenderEhbViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    enum GraphicsEhbPrecision precision, LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!display || !rasters || !palette || !(rasters->present & 1) ||
        !(display->modes & EXTRA_HALFBRITE) || (display->modes & (HAM | DUALPF))) {
        return FALSE;
    }

    if (rasters->plane_fetches || rasters->interlaced) {
        return renderFetchedViewPort(framebuffer, display, rasters, palette, precision,
            display->ham_format, dest_x, dest_y, width, height);
    }

    struct GraphicsFramebufferPlacement placement = {
        rasters->source_x[0], rasters->source_y[0], dest_x, dest_y, width, height
    };
    struct GraphicsFramebufferArea area;

    if (!pcClipFramebufferArea(framebuffer, &rasters->bitmap[0], &placement, &area)) {
        return FALSE;
    }

    return pcRenderEhbFramebuffer(framebuffer, &rasters->bitmap[0], palette->colors,
        &area, precision, display->palette_layout.index_xor);
}

BOOL pcRenderHamViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    enum GraphicsHamFormat format, LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!display || !rasters || !palette || !(rasters->present & 1) ||
        !(display->modes & HAM) || (display->modes & DUALPF)) {
        return FALSE;
    }

    if (rasters->plane_fetches || rasters->interlaced) {
        return renderFetchedViewPort(framebuffer, display, rasters, palette, display->ehb_precision,
            format, dest_x, dest_y, width, height);
    }

    struct GraphicsFramebufferPlacement placement = {
        rasters->source_x[0], rasters->source_y[0], dest_x, dest_y, width, height
    };
    struct GraphicsFramebufferArea area;

    if (!pcClipFramebufferArea(framebuffer, &rasters->bitmap[0], &placement, &area)) {
        return FALSE;
    }

    // Destination clipping must not discard the HAM history. Start at the
    // captured raster origin, or the first readable pixel for a negative scroll.
    ULONG origin = area.width && area.height && rasters->source_x[0] > 0 ? rasters->source_x[0] : 0;

    return pcRenderHamFramebuffer(framebuffer, &rasters->bitmap[0], palette->colors,
        &area, origin, format, display->palette_layout.index_xor);
}

static void readDualPalette(ULONG colors[2][16], const struct ViewPortDisplaySnapshot* display,
    const struct GraphicsFramebufferPalette* palette)
{
    UBYTE index_xor = display->palette_layout.index_xor;
    UBYTE second_offset = display->palette_layout.second_offset;

    for (unsigned index = 0; index < 16; ++index) {
        // The second field selects a palette-address bit before the common
        // XOR mask. Keep pixel indices unchanged so the renderer determines
        // transparency before either field's palette addressing.
        colors[0][index] = palette->colors[index ^ index_xor];
        colors[1][index] = palette->colors[(index | second_offset) ^ index_xor];
    }

}

BOOL pcRenderDualViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!display || !rasters || !palette || !(rasters->present & 1) ||
        !(display->modes & DUALPF) || (display->modes & HAM)) {
        return FALSE;
    }

    if (rasters->plane_fetches || rasters->interlaced) {
        return renderFetchedViewPort(framebuffer, display, rasters, palette, display->ehb_precision,
            display->ham_format, dest_x, dest_y, width, height);
    }

    struct GraphicsFramebufferPlacement placements[2];
    struct GraphicsFramebufferArea areas[2];

    for (unsigned field = 0; field < 2; ++field) {
        placements[field].source_x = rasters->source_x[field];
        placements[field].source_y = rasters->source_y[field];
        placements[field].dest_x = dest_x;
        placements[field].dest_y = dest_y;
        placements[field].width = width;
        placements[field].height = height;
    }

    BOOL second = (rasters->present & 2) != 0;

    if (second) {
        if (!pcClipFramebufferPair(framebuffer, &rasters->bitmap[0], &rasters->bitmap[1],
            &placements[0], &placements[1], &areas[0], &areas[1])) {
            return FALSE;
        }
    } else if (!pcClipFramebufferArea(framebuffer, &rasters->bitmap[0], &placements[0], &areas[0])) {
        return FALSE;
    }

    UBYTE index_xor = display->palette_layout.index_xor;

    if (display->dual_disabled) {
        if (second) {
            return pcRenderCombinedFramebuffer(framebuffer, &rasters->bitmap[0], &rasters->bitmap[1],
                palette->colors, &areas[0], &areas[1], index_xor);
        }

        return pcRenderIndexedFramebuffer(framebuffer, &rasters->bitmap[0], palette->colors,
            &areas[0], index_xor);
    }

    ULONG colors[2][16];
    readDualPalette(colors, display, palette);

    BOOL second_in_front = (display->modes & PFBA) != 0;

    if (second) {
        return pcRenderDualFramebuffer(framebuffer, &rasters->bitmap[0], &rasters->bitmap[1],
            colors[0], colors[1], &areas[0], &areas[1], second_in_front);
    }

    return pcRenderPlanarDualFramebuffer(framebuffer, &rasters->bitmap[0], colors[0], colors[1],
        &areas[0], second_in_front);
}

// Intersect a viewport-relative interval with one source's readable bounds.
// Keep unsigned magnitudes so LONG_MIN and an entirely clipped source do not
// overflow before the empty interval is recognized.
static void clipFetchAxis(LONG origin, ULONG limit, ULONG* first, ULONG* end)
{
    ULONG available;

    if (origin < 0) {
        ULONG skip = 0UL - (ULONG)origin;

        if (skip > *first) {
            *first = skip;
        }

        available = limit > 0xffffffffUL - skip ? 0xffffffffUL : limit + skip;
    } else {
        available = (ULONG)origin < limit ? limit - (ULONG)origin : 0;
    }

    if (available < *end) {
        *end = available;
    }
}

static unsigned displayPlaneSlot(unsigned plane, unsigned field, BOOL second, BOOL ham_planar)
{
    if (second) {
        return 2 * plane + field;
    }

    // Application-order HAM8's command planes 6/7 occupy display slots 0/1.
    // This is the prepared layout, independent of the current scroll mode.
    return ham_planar ? (plane + 2) & 7 : plane;
}

// Clip in field-line coordinates while retaining the source-row phase. For
// a negative odd origin an interlaced field first reaches row one, not zero.
static void clipFieldRows(LONG origin, ULONG limit, BOOL interlaced, ULONG* first, ULONG* end)
{
    ULONG source_first = 0, source_end = ~0UL;
    clipFetchAxis(origin, limit, &source_first, &source_end);

    if (interlaced) {
        source_first = (source_first >> 1) + (source_first & 1);
        source_end = (source_end >> 1) + (source_end & 1);
    }

    if (source_first > *first) {
        *first = source_first;
    }

    if (source_end < *end) {
        *end = source_end;
    }
}

static const struct ViewPortPlaneFetch* readViewPortFetch(struct ViewPortPlaneFetch* source,
    const struct ViewPortRasterState* rasters, unsigned field, unsigned plane, unsigned slot)
{
    if (rasters->plane_fetches) {
        return &rasters->fetches[slot];
    }

    source->plane = rasters->bitmap[field].Planes[plane];
    source->bytes_per_row = rasters->bitmap[field].BytesPerRow;
    source->source_x = rasters->source_x[field];
    source->source_y = rasters->source_y[field];
    source->row_offset = 0;

    return source;
}

static BOOL readFetchedViewPort(const struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    enum GraphicsHamFormat format, LONG dest_x, LONG dest_y, ULONG width, ULONG height,
    struct GraphicsFramebufferRaster inputs[2], struct GraphicsFramebufferArea* output_area)
{
    if (!framebuffer) {
        return FALSE;
    }

    BOOL dual = (display->modes & DUALPF) != 0;
    BOOL second = dual && (rasters->present & 2);
    BOOL ham = (display->modes & HAM) != 0;
    BOOL ham_planar = ham && format == GRAPHICS_HAM8_PLANAR;
    unsigned fields = second ? 2 : 1;
    unsigned depths[2];

    for (unsigned field = 0; field < fields; ++field) {
        if (!pcReadFramebufferRaster(&inputs[field], &rasters->bitmap[field])) {
            return FALSE;
        }

        depths[field] = inputs[field].depth;
    }

    if (second) {
        if (display->dual_disabled) {
            unsigned total = depths[0] + depths[1];

            if (total > 8) {
                return FALSE;
            }

            depths[0] = (total + 1) / 2;
            depths[1] = total / 2;
        } else if (depths[0] > 4 || depths[1] > 4) {
            return FALSE;
        }
    } else if (ham_planar && depths[0] == 7) {
        depths[0] = 8;
    }

    ULONG left = 0, top = 0, right = width, bottom = height;

    for (unsigned field = 0; field < fields; ++field) {
        for (unsigned plane = 0; plane < (depths[field] ? depths[field] : 1); ++plane) {
            if (ham_planar && inputs[field].depth == 7 && plane == 5) {
                continue;
            }

            unsigned slot = displayPlaneSlot(plane, field, second, ham_planar);
            struct ViewPortPlaneFetch initial;
            const struct ViewPortPlaneFetch* fetch = readViewPortFetch(&initial, rasters, field, plane, slot);
            clipFetchAxis(fetch->source_x, inputs[field].width, &left, &right);
            clipFieldRows(fetch->source_y, inputs[field].height, rasters->interlaced, &top, &bottom);
        }
    }

    // Normalize all planes to the common readable origin before destination
    // clipping. HAM must still decode any prefix hidden by that later clipping.
    BOOL readable = left < right && top < bottom;

    for (unsigned field = 0; field < fields; ++field) {
        inputs[field].width = readable ? right - left : 0;
        inputs[field].height = readable ? bottom - top : 0;

        for (unsigned plane = 0; plane < 8; ++plane) {
            struct GraphicsFramebufferPlane* source = &inputs[field].planes[plane];
            source->data = NULL;
            source->stride = source->source_x = source->source_y = 0;
            source->row_offset = 0;

            if (!readable || plane >= depths[field] ||
                (ham_planar && inputs[field].depth == 7 && plane == 5)) {
                continue;
            }

            unsigned slot = displayPlaneSlot(plane, field, second, ham_planar);
            struct ViewPortPlaneFetch initial;
            const struct ViewPortPlaneFetch* fetch = readViewPortFetch(&initial, rasters, field, plane, slot);
            source->data = fetch->plane;
            source->stride = (ULONG)fetch->bytes_per_row << (rasters->interlaced ? 1 : 0);
            source->source_x = (ULONG)fetch->source_x + left;
            source->source_y = top;
            // The first-row displacement uses the original bitmap pitch;
            // subsequent field lines use the sampled pitch. Keep scroll's
            // independent byte correction, including a changed source stride.
            source->row_offset = (LONG)((ULONG)fetch->row_offset +
                (ULONG)fetch->source_y * fetch->bytes_per_row);
        }
    }

    ULONG output_left = left, output_top = top;
    clipFetchAxis(dest_x, framebuffer->mode.width, &output_left, &right);
    clipFetchAxis(dest_y, framebuffer->mode.height, &output_top, &bottom);
    struct GraphicsFramebufferArea area = {0};

    if (readable && output_left < right && output_top < bottom) {
        area.source_x = output_left - left;
        area.source_y = output_top - top;
        area.dest_x = (ULONG)dest_x + output_left;
        area.dest_y = (ULONG)dest_y + output_top;
        area.width = right - output_left;
        area.height = bottom - output_top;
    }

    *output_area = area;

    return TRUE;
}

LONG pcViewPortBitMapOverlap(const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, const struct BitMap* bitmap)
{
    struct GraphicsFramebufferScaleArea placement;

    if (!pcGetViewPortFieldArea(display, &placement)) {
        return -1;
    }

    struct GraphicsFramebuffer frame = {0};
    frame.mode.width = placement.width;
    frame.mode.height = placement.height;
    struct GraphicsFramebufferRaster inputs[2] = {{0}};
    struct GraphicsFramebufferArea area;

    if (!readFetchedViewPort(&frame, display, rasters, display->ham_format, 0, 0,
        placement.width, placement.height, inputs, &area)) {
        return -1;
    }

    ULONG bitmap_size = (ULONG)bitmap->BytesPerRow * bitmap->Rows;
    unsigned bitmap_planes = bitmap->pad == 0x805c ? 1 : bitmap->Depth;

    if (!bitmap_size || !area.width || !area.height) {
        return 0;
    }

    for (unsigned field = 0; field < 2; ++field) {
        const struct GraphicsFramebufferRaster* input = &inputs[field];

        for (unsigned plane = 0; plane < 8; ++plane) {
            const struct GraphicsFramebufferPlane* source = &input->planes[plane];

            if (!source->data || (ULONG)source->data == ~0UL) {
                continue;
            }

            ULONG first_byte = source->source_x >> 3;
            ULONG start = (ULONG)source->data + (ULONG)source->row_offset +
                source->source_y * source->stride + first_byte;
            ULONG row_bytes = ((source->source_x + input->width + 7) >> 3) - first_byte;
            ULONG size = (input->height - 1) * source->stride + row_bytes;

            // Interleaved bitmaps own one allocation at plane zero, exactly
            // as FreeBitMap does. Later planes do not extend that allocation.
            for (unsigned old_plane = 0; old_plane < bitmap_planes && old_plane < 8; ++old_plane) {
                ULONG old = (ULONG)bitmap->Planes[old_plane];

                // Null/all-one planes are constants, not borrowed storage.
                // Unsigned distances also handle a wrapped 32-bit range.
                if (!old || old == ~0UL || (start - old >= bitmap_size && old - start >= size)) {
                    continue;
                }

                // A pitch can include unread padding or skipped interlaced
                // rows. Bounding-range overlap alone does not establish a
                // reader of a bitmap stored in one of those gaps.
                ULONG row_start = start;

                for (ULONG row = 0; row < input->height; ++row) {
                    if (row_start - old < bitmap_size || old - row_start < row_bytes) {
                        return 1;
                    }

                    row_start += source->stride;
                }
            }
        }
    }

    return 0;
}

static BOOL renderViewPortRasterArea(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, const struct GraphicsFramebufferRaster inputs[2],
    const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision,
    enum GraphicsHamFormat format, ULONG* held_color)
{
    BOOL dual = (display->modes & DUALPF) != 0;
    BOOL second = dual && (rasters->present & 2);
    BOOL ham = (display->modes & HAM) != 0;

    UBYTE index_xor = display->palette_layout.index_xor;

    if (dual) {
        if (display->dual_disabled) {
            if (second) {
                return pcRenderCombinedRaster(framebuffer, &inputs[0], &inputs[1], palette->colors,
                    area, area, index_xor);
            }

            return pcRenderIndexedRaster(framebuffer, &inputs[0], palette->colors, area, index_xor);
        }

        ULONG colors[2][16];
        readDualPalette(colors, display, palette);
        BOOL second_in_front = (display->modes & PFBA) != 0;

        if (second) {
            return pcRenderDualRaster(framebuffer, &inputs[0], &inputs[1], colors[0], colors[1],
                area, area, second_in_front);
        }

        return pcRenderPlanarDualRaster(framebuffer, &inputs[0], colors[0], colors[1], area, second_in_front);
    }

    if (ham) {
        if (held_color) {
            return pcRenderHamRasterSpan(framebuffer, &inputs[0], palette->colors,
                area, format, index_xor, held_color);
        }

        return pcRenderHamRaster(framebuffer, &inputs[0], palette->colors, area, 0, format, index_xor);
    }

    if (display->modes & EXTRA_HALFBRITE) {
        return pcRenderEhbRaster(framebuffer, &inputs[0], palette->colors, area, precision, index_xor);
    }

    return pcRenderIndexedRaster(framebuffer, &inputs[0], palette->colors, area, index_xor);
}

static BOOL renderFetchedViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, enum GraphicsEhbPrecision precision,
    enum GraphicsHamFormat format, LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    struct GraphicsFramebufferRaster inputs[2];
    struct GraphicsFramebufferArea area;

    if (!readFetchedViewPort(framebuffer, display, rasters, format, dest_x, dest_y, width, height, inputs, &area)) {
        return FALSE;
    }

    return renderViewPortRasterArea(framebuffer, display, rasters, palette, inputs, &area,
        precision, format, NULL);
}

static void clipViewPortSpan(struct GraphicsFramebufferArea* area, ULONG row, ULONG first, ULONG end)
{
    if (first < area->dest_x) {
        first = area->dest_x;
    }

    if (end > area->dest_x + area->width) {
        end = area->dest_x + area->width;
    }

    if (first >= end || row < area->dest_y || row - area->dest_y >= area->height) {
        *area = (struct GraphicsFramebufferArea){0};
    } else {
        area->source_x += first - area->dest_x;
        area->source_y += row - area->dest_y;
        area->dest_x = first;
        area->dest_y = row;
        area->width = end - first;
        area->height = 1;
    }
}

BOOL pcRenderViewPortSpan(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, ULONG row, ULONG first, ULONG end, ULONG* held_color)
{
    if (!framebuffer || !display || !rasters || !palette || !(rasters->present & 1) ||
        row >= framebuffer->mode.height || first > end || end > framebuffer->mode.width ||
        ((display->modes & HAM) && (!held_color || (display->modes & DUALPF)))) {
        return FALSE;
    }

    struct GraphicsFramebufferRaster inputs[2];
    struct GraphicsFramebufferArea area;

    if (!readFetchedViewPort(framebuffer, display, rasters, display->ham_format, 0, 0,
        framebuffer->mode.width, framebuffer->mode.height, inputs, &area)) {
        return FALSE;
    }

    clipViewPortSpan(&area, row, first, end);

    return renderViewPortRasterArea(framebuffer, display, rasters, palette, inputs, &area,
        display->ehb_precision, display->ham_format, held_color);
}

BOOL pcRenderViewFieldRasterRun(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    struct ViewFieldRasterRuns* state, const struct ViewFieldRun* run,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, LONG origin_x, LONG origin_y,
    const struct ViewFieldOcclusion* occlusion)
{
    struct GraphicsFramebuffer decoded;
    struct GraphicsFramebufferScaleArea placement;

    if (!state || !run || !rasters || !palette || !(rasters->present & 1) ||
        run->start.x != state->position.x || run->start.y != state->position.y ||
        !readViewPortFieldPlacement(framebuffer, scratch, display, origin_x, origin_y,
            occlusion, &decoded, &placement) || (display->modes & (HAM | DUALPF)) == (HAM | DUALPF)) {
        return FALSE;
    }

    ULONG cursor = 0;
    struct ViewPortRunSpan span;
    LONG result = pcNextViewPortRunSpan(display, run, &cursor, &span);

    if (result < 0) {
        return FALSE;
    }

    struct GraphicsFramebufferRaster inputs[2];
    struct GraphicsFramebufferArea readable;

    if (!readFetchedViewPort(&decoded, display, rasters, display->ham_format, 0, 0,
        placement.width, placement.height, inputs, &readable)) {
        return FALSE;
    }

    while (result > 0) {
        struct GraphicsFramebufferArea area = readable;
        clipViewPortSpan(&area, span.row, span.first, span.end);

        if (area.width && area.height) {
            if (!state->row_ready || state->row != span.row) {
                state->row = span.row;
                state->held_color = palette->colors[0];
                state->row_ready = TRUE;
            }

            if (!renderViewPortRasterArea(&decoded, display, rasters, palette, inputs, &area,
                display->ehb_precision, display->ham_format, &state->held_color)) {
                return FALSE;
            }

            ULONG first = area.dest_x;
            ULONG end = first + area.width;

            if (occlusion && !pcWriteViewPortSpriteOcclusionSpan(&decoded, occlusion->scratch,
                occlusion->scratch_stride, occlusion->scratch_size, display, rasters, span.row, first, end)) {
                return FALSE;
            }

            if (!pcProjectViewPortSpan(framebuffer, &decoded, display, origin_x, origin_y,
                span.row, first, end, occlusion)) {
                return FALSE;
            }
        }

        result = pcNextViewPortRunSpan(display, run, &cursor, &span);
    }

    if (result < 0) {
        return FALSE;
    }

    state->position = run->end;

    return TRUE;
}

static BOOL composeViewFieldBackground(struct GraphicsFramebuffer* framebuffer,
    const struct ViewFieldSpriteRuns* sprites, const struct ViewFieldRun* run,
    const struct ViewPortSpriteLayout* layout, ULONG background,
    const struct ViewFieldOcclusion* occlusion, const struct Rectangle* region, const struct Rectangle* window)
{
    WORD origin_x = sprites->origin_x;
    WORD origin_y = sprites->origin_y;

    for (LONG line = run->start.y; line < run->end.y || (line == run->end.y && run->end.x); ++line) {
        LONG y = line - origin_y;

        if (y < 0 || y >= region->MaxY) {
            continue;
        }

        WORD first = line == run->start.y ?
            clipSpriteRunCoordinate((LONG)run->start.x * 8 - origin_x, 0, region->MaxX) : 0;
        WORD end = line == run->end.y ?
            clipSpriteRunCoordinate((LONG)run->end.x * 8 - origin_x, 0, region->MaxX) : region->MaxX;
        BOOL blank = layout->border_blank;

        if (!pcFillGraphicsFramebufferArea(framebuffer, blank ? 0 : background, first, y, end - first, 1)) {
            return FALSE;
        }

        if (blank && y >= window->MinY && y < window->MaxY) {
            WORD inside_first = first > window->MinX ? first : window->MinX;
            WORD inside_end = end < window->MaxX ? end : window->MaxX;

            if (inside_first < inside_end && !pcFillGraphicsFramebufferArea(framebuffer, background,
                inside_first, y, inside_end - inside_first, 1)) {
                return FALSE;
            }
        }

        UBYTE* mask = occlusion->pixels + (ULONG)y * occlusion->stride;

        for (WORD x = first; x < end; ++x) {
            mask[x] = 0;
        }
    }

    return TRUE;
}

BOOL pcComposeViewFieldRun(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    struct ViewFieldRasterRuns* raster_state, struct ViewFieldSpriteRuns* sprite_state,
    const struct ViewFieldRun* run, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, const struct GraphicsFramebufferPalette* palette,
    ULONG background, const struct ViewFieldOcclusion* occlusion)
{
    if (!framebuffer || !raster_state || !display || !rasters || !palette || !occlusion ||
        framebuffer->mode.width > INT16_MAX || framebuffer->mode.height > INT16_MAX ||
        !validViewFieldSpriteRun(sprite_state, run) || !validViewPortSpriteLayout(&display->sprites, 2) ||
        run->start.x != raster_state->position.x || run->start.y != raster_state->position.y ||
        run->start.x != sprite_state->position.x || run->start.y != sprite_state->position.y) {
        return FALSE;
    }

    struct GraphicsFramebuffer decoded;
    struct GraphicsFramebufferScaleArea placement;
    WORD origin_x = sprite_state->origin_x;
    WORD origin_y = sprite_state->origin_y;

    if (!readViewPortFieldPlacement(framebuffer, scratch, display, origin_x, origin_y,
        occlusion, &decoded, &placement)) {
        return FALSE;
    }

    struct Rectangle region = {0};
    region.MaxX = framebuffer->mode.width;
    region.MaxY = framebuffer->mode.height;
    struct Rectangle window;
    window.MinX = clipSpriteRunCoordinate(placement.dest_x, 0, region.MaxX);
    window.MinY = clipSpriteRunCoordinate(placement.dest_y, 0, region.MaxY);
    window.MaxX = clipSpriteRunCoordinate(placement.dest_x + (placement.width << placement.x_shift),
        0, region.MaxX);
    window.MaxY = clipSpriteRunCoordinate(placement.dest_y + placement.height, 0, region.MaxY);

    if (!composeViewFieldBackground(framebuffer, sprite_state, run, &display->sprites,
        background, occlusion, &region, &window)) {
        return FALSE;
    }

    if (!pcRenderViewFieldRasterRun(framebuffer, scratch, raster_state, run,
        display, rasters, palette, origin_x, origin_y, occlusion)) {
        return FALSE;
    }

    struct GraphicsSpriteOcclusion mask = {occlusion->pixels, occlusion->stride, occlusion->size};

    return pcRenderViewFieldSpriteRun(framebuffer, sprite_state, run, &display->sprites,
        display->modes, palette, &mask, &region, &window);
}

static BOOL writeViewPortOcclusion(UBYTE* output, ULONG stride, ULONG size,
    const struct ViewPortDisplaySnapshot* display, struct GraphicsFramebufferRaster inputs[2],
    struct GraphicsFramebufferArea areas[2], BOOL second)
{
    BOOL dual = (display->modes & DUALPF) != 0;

    if (!dual || display->dual_disabled) {
        if (!dual && (display->modes & HAM)) {
            enum GraphicsHamFormat format = display->ham_format;
            unsigned depth = inputs[0].depth;

            if (format == GRAPHICS_HAM6) {
                if (depth != 5 && depth != 6) {
                    return FALSE;
                }
            } else if (format == GRAPHICS_HAM8 || format == GRAPHICS_HAM8_PLANAR) {
                if (depth != 7 && depth != 8) {
                    return FALSE;
                }

                if (format == GRAPHICS_HAM8_PLANAR && depth == 7) {
                    // The prepared application layout omits data plane five,
                    // but still displays both command planes six and seven.
                    inputs[0].planes[5] = inputs[0].planes[6];
                    inputs[0].planes[6] = inputs[0].planes[7];
                }
            } else {
                return FALSE;
            }
        } else if (!dual && (display->modes & EXTRA_HALFBRITE)) {
            if (inputs[0].depth != 6 || (display->ehb_precision != GRAPHICS_EHB_RGB4 &&
                display->ehb_precision != GRAPHICS_EHB_RGB8)) {
                return FALSE;
            }

            // EHB's intensity selector alone does not hide a sprite. Its
            // five color planes determine transparency before palette XOR.
            inputs[0].depth = 5;
        }

        if (second) {
            struct GraphicsFramebufferRaster combined = inputs[0];
            unsigned depth = inputs[0].depth + inputs[1].depth;

            if (depth > 8) {
                return FALSE;
            }

            // Both rasters supply alternate bits of one index. Normalize
            // their independently clipped origins to the output rectangle.
            combined.width = areas[0].width;
            combined.height = areas[0].height;
            combined.depth = depth;

            for (unsigned plane = 0; plane < depth; ++plane) {
                unsigned field = plane & 1;
                combined.planes[plane] = inputs[field].planes[plane / 2];
                combined.planes[plane].source_x += areas[field].source_x;
                combined.planes[plane].source_y += areas[field].source_y;
            }

            inputs[0] = combined;
            areas[0].source_x = areas[0].source_y = 0;
        }

        return pcWriteSpriteOcclusion(output, stride, size, &inputs[0], NULL,
            &areas[0], NULL, display->sprite_mask[1], 0, FALSE);
    }

    if (!second) {
        struct GraphicsFramebufferRaster source = inputs[0];
        inputs[0].depth = (source.depth + 1) / 2;
        inputs[1] = source;
        inputs[1].depth = source.depth / 2;
        areas[1] = areas[0];

        for (unsigned plane = 0; plane < source.depth; ++plane) {
            inputs[plane & 1].planes[plane / 2] = source.planes[plane];
        }
    } else if (inputs[0].depth > 4 || inputs[1].depth > 4) {
        return FALSE;
    }

    return pcWriteSpriteOcclusion(output, stride, size, &inputs[0], &inputs[1], &areas[0], &areas[1],
        display->sprite_mask[0], display->sprite_mask[1], (display->modes & PFBA) != 0);
}

BOOL pcWriteViewPortSpriteOcclusionSpan(const struct GraphicsFramebuffer* framebuffer,
    UBYTE* output, ULONG stride, ULONG size, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, ULONG row, ULONG first, ULONG end)
{
    if (!framebuffer || !display || !rasters || !(rasters->present & 1) ||
        !stride || framebuffer->mode.width > stride || framebuffer->mode.height > size / stride ||
        row >= framebuffer->mode.height || first > end || end > framebuffer->mode.width ||
        ((display->modes & DUALPF) && (display->modes & HAM))) {
        return FALSE;
    }

    struct GraphicsFramebufferRaster inputs[2];
    struct GraphicsFramebufferArea areas[2];

    if (!readFetchedViewPort(framebuffer, display, rasters, display->ham_format, 0, 0,
        framebuffer->mode.width, framebuffer->mode.height, inputs, &areas[0])) {
        return FALSE;
    }

    clipViewPortSpan(&areas[0], row, first, end);
    areas[1] = areas[0];
    BOOL second = (display->modes & DUALPF) && (rasters->present & 2);

    return writeViewPortOcclusion(output, stride, size, display, inputs, areas, second);
}

BOOL pcWriteViewPortSpriteOcclusion(const struct GraphicsFramebuffer* framebuffer,
    UBYTE* output, ULONG stride, ULONG size, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, LONG dest_x, LONG dest_y, ULONG width, ULONG height)
{
    if (!framebuffer || !display || !rasters || !(rasters->present & 1) ||
        !stride || framebuffer->mode.width > stride || framebuffer->mode.height > size / stride ||
        ((display->modes & DUALPF) && (display->modes & HAM))) {
        return FALSE;
    }

    BOOL dual = (display->modes & DUALPF) != 0;
    BOOL second = dual && (rasters->present & 2);
    struct GraphicsFramebufferRaster inputs[2];
    struct GraphicsFramebufferArea areas[2];

    if (rasters->plane_fetches || rasters->interlaced) {
        if (!readFetchedViewPort(framebuffer, display, rasters, display->ham_format,
            dest_x, dest_y, width, height, inputs, &areas[0])) {
            return FALSE;
        }

        areas[1] = areas[0];
    } else {
        struct GraphicsFramebufferPlacement placements[2];

        for (unsigned field = 0; field < (second ? 2U : 1U); ++field) {
            if (!pcReadFramebufferRaster(&inputs[field], &rasters->bitmap[field])) {
                return FALSE;
            }

            placements[field].source_x = rasters->source_x[field];
            placements[field].source_y = rasters->source_y[field];
            placements[field].dest_x = dest_x;
            placements[field].dest_y = dest_y;
            placements[field].width = width;
            placements[field].height = height;
        }

        if (second) {
            if (!pcClipFramebufferPair(framebuffer, &rasters->bitmap[0], &rasters->bitmap[1],
                &placements[0], &placements[1], &areas[0], &areas[1])) {
                return FALSE;
            }
        } else if (!pcClipFramebufferArea(framebuffer, &rasters->bitmap[0], &placements[0], &areas[0])) {
            return FALSE;
        }
    }

    return writeViewPortOcclusion(output, stride, size, display, inputs, areas, second);
}

void pcReleasePreparedViewPort(struct PreparedViewPort* entry, struct GfxBase* graphics)
{
    if (entry && !--entry->references) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        pcFreeViewPortDisplay(entry->display, graphics);
        FreeMem(entry, sizeof(*entry));
    }
}

BOOL pcStorePreparedViewPort(struct PreparedViewPorts* prepared, const struct CopList* list,
    struct ViewPortDisplaySnapshot* display, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct PreparedViewPort* entry = AllocMem(sizeof(*entry), MEMF_PUBLIC | MEMF_CLEAR);

    if (!entry) {
        return FALSE;
    }

    entry->list = list;
    entry->display = display;
    entry->references = 1;
    struct PreparedViewPort** position = &prepared->first;

    while (*position && (*position)->list != list) {
        position = &(*position)->next;
    }

    struct PreparedViewPort* previous = *position;
    entry->next = previous ? previous->next : NULL;
    *position = entry;

    if (previous) {
        previous->next = NULL;
        pcReleasePreparedViewPort(previous, graphics);
    }

    return TRUE;
}

struct PreparedViewPort* pcRetainPreparedViewPort(const struct PreparedViewPorts* prepared,
    const struct CopList* list)
{
    struct PreparedViewPort* entry = prepared->first;

    while (entry && entry->list != list) {
        entry = entry->next;
    }

    if (entry) {
        ++entry->references;
    }

    return entry;
}

void pcRemovePreparedViewPort(struct PreparedViewPorts* prepared, const struct CopList* list,
    struct GfxBase* graphics)
{
    struct PreparedViewPort** position = &prepared->first;

    while (*position && (*position)->list != list) {
        position = &(*position)->next;
    }

    struct PreparedViewPort* entry = *position;

    if (entry) {
        *position = entry->next;
        entry->next = NULL;
        pcReleasePreparedViewPort(entry, graphics);
    }
}

void pcClearPreparedViewPorts(struct PreparedViewPorts* prepared, struct GfxBase* graphics)
{
    struct PreparedViewPort* entry = prepared->first;
    prepared->first = NULL;

    while (entry) {
        struct PreparedViewPort* next = entry->next;
        entry->next = NULL;
        pcReleasePreparedViewPort(entry, graphics);
        entry = next;
    }
}

ULONG pcCommitViewPortBuild(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    (void)view;
    const struct ViewPortBuildState* state = extra->DriverData[0];
    const struct ViewPortBuildMode* mode = state->mode_data;
    struct ViewPortDisplaySnapshot* display = pcAllocViewPortDisplay(viewport, state, graphics);

    if (!display) {
        return MVP_NO_MEM;
    }

    if (!pcStorePreparedViewPort(mode->prepared, viewport->DspIns, display, graphics)) {
        pcFreeViewPortDisplay(display, graphics);
        return MVP_NO_MEM;
    }

    return MVP_OK;
}

static BOOL visibleViewPort(const struct ViewPort* viewport)
{
    return !(viewport->Modes & VP_HIDE) && viewport->DspIns;
}

static void advanceViewPortField(struct ViewPortRasterState* rasters)
{
    for (unsigned playfield = 0; playfield < 2; ++playfield) {
        rasters->source_y[playfield] = (LONG)((ULONG)rasters->source_y[playfield] + 1);
    }

    if (rasters->plane_fetches) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            rasters->fetches[slot].source_y = (LONG)((ULONG)rasters->fetches[slot].source_y + 1);
        }
    }
}

struct ViewDisplaySnapshot* pcAllocViewDisplay(const struct View* view,
    const struct PreparedViewPorts* prepared, struct GfxBase* graphics)
{
    ULONG count = 0;
    const ULONG maximum = (~0UL - sizeof(struct ViewDisplaySnapshot)) / sizeof(struct ViewDisplayPort);

    for (const struct ViewPort* viewport = view->ViewPort; viewport; viewport = viewport->Next) {
        if (visibleViewPort(viewport)) {
            if (count == maximum) {
                return NULL;
            }

            ++count;
        }
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ULONG size = sizeof(struct ViewDisplaySnapshot) + count * sizeof(struct ViewDisplayPort);
    struct ViewDisplaySnapshot* display = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!display) {
        return NULL;
    }

    display->references = 1;
    display->modes = view->Modes;
    display->first_port[0] = display->first_port[1] = ~0UL;

    for (const struct ViewPort* viewport = view->ViewPort; viewport; viewport = viewport->Next) {
        if (!visibleViewPort(viewport)) {
            continue;
        }

        struct PreparedViewPort* entry = pcRetainPreparedViewPort(prepared, viewport->DspIns);

        if (!entry) {
            // Count records acquired references during capture. The allocation
            // still has room for the entire selection, including this failure.
            for (ULONG index = 0; index < display->count; ++index) {
                pcReleasePreparedViewPort(display->viewports[index].prepared, graphics);
            }

            FreeMem(display, size);
            return NULL;
        }

        struct ViewDisplayPort* port = &display->viewports[display->count++];
        port->prepared = entry;
        port->palette[0].next_port = port->palette[1].next_port = ~0UL;
        port->rasters[0] = port->rasters[1] = entry->display->rasters;
        port->sprites[0] = port->sprites[1] = entry->display->sprites;

        // Native preparation emits the short-field pointers one bitmap row
        // later for LACE. This depends on the prepared viewport's modes, not
        // the current View or the geometry stage's interlace flag.
        if (entry->display->modes & LACE) {
            advanceViewPortField(&port->rasters[1]);

            // Plane-fetch sources retain ScrollVPort's intermediate writes.
            // Those records keep their prepared field flags, but add a
            // sign-extended WORD stride for the short field. Already merged
            // output uses the unsigned stride instead. Keep this byte
            // adjustment separate from the source row and allocation bounds.
            if (port->rasters[1].plane_fetches) {
                for (unsigned slot = 0; slot < 8; ++slot) {
                    struct ViewPortPlaneFetch* fetch = &port->rasters[1].fetches[slot];
                    LONG difference = (LONG)(WORD)fetch->bytes_per_row - fetch->bytes_per_row;
                    fetch->row_offset = (LONG)((ULONG)fetch->row_offset + (ULONG)difference);
                }
            }
        }
    }

    return display;
}

void pcRetainViewDisplay(struct ViewDisplaySnapshot* display)
{
    if (display) {
        ++display->references;
    }
}

void pcReleaseViewDisplay(struct ViewDisplaySnapshot* display, struct GfxBase* graphics)
{
    if (display && !--display->references) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;

        for (ULONG index = 0; index < display->count; ++index) {
            pcReleasePreparedViewPort(display->viewports[index].prepared, graphics);
        }

        for (unsigned field = 0; field < 2; ++field) {
            struct ViewFieldTiming* timing = &display->timing[field];

            if (timing->waits) {
                FreeMem(timing->waits, pcViewFieldStorageSize(timing->capacity));
            }
        }

        ULONG size = sizeof(*display) + display->count * sizeof(struct ViewDisplayPort);
        FreeMem(display, size);
    }
}

static void readViewFieldPalette(struct ViewFieldPalette* palette)
{
    for (unsigned bank = 0; bank < 8; ++bank) {
        palette->high_mask[bank] = palette->low_mask[bank] = 0;
    }

    for (unsigned index = 0; index < 256; ++index) {
        palette->colors.colors[index] = 0;
    }

    if (!palette->start || !palette->end) {
        return;
    }

    UWORD control = 0;
    BOOL found = FALSE;

    // Consume the palette prefix in the emitted public RAM-list format. The
    // surrounding WAIT records locate it; they are not executed or emulated.
    // Stop at the first non-palette record after the first color, just as the
    // palette updater does. The next viewport prefix also bounds the scan:
    // an empty palette must not acquire a later viewport's colors.
    for (const UWORD* record = palette->start; record + 1 < palette->end; record += 2) {
        UWORD destination = record[0];
        UWORD data = record[1];

        if (destination == 0x106) {
            control = data;
        } else if (!(destination & 1) && destination >= 0x180 && destination < 0x1c0) {
            unsigned bank = control >> 13;
            unsigned index = (destination - 0x180) >> 1;
            ULONG selected = 0x80000000UL >> index;
            ULONG* color = &palette->colors.colors[bank * 32 + index];
            *color = pcApplyRGB4Color(*color, data, (control & 0x200) != 0);

            if (control & 0x200) {
                palette->low_mask[bank] |= selected;
            } else {
                palette->high_mask[bank] |= selected;
                palette->low_mask[bank] |= selected;
            }

            found = TRUE;
        } else if (found) {
            return;
        }
    }
}

static void copyViewFieldRecords(struct ViewFieldTiming* timing)
{
    for (ULONG word = 0; word < timing->words; ++word) {
        timing->records[word] = timing->live[word];
    }
}

static BOOL viewFieldWaitMatches(const struct ViewFieldTiming* timing, const UWORD* records, ULONG index)
{
    const struct ViewFieldWait* wait = &timing->waits[index];

    if ((wait->offset & 1) || wait->offset >= timing->words || timing->words - wait->offset < 2) {
        return FALSE;
    }

    UWORD instruction = ((UWORD)wait->position.y << 8) | (wait->position.x & 0xfe) | 1;

    return records[wait->offset] == instruction && records[wait->offset + 1] == 0xfffe;
}

LONG pcNextViewFieldRun(const struct ViewFieldTiming* timing, ULONG* cursor, struct ViewFieldRun* run)
{
    if (!timing || !cursor || !run || timing->count > timing->capacity ||
        timing->words / 2 > timing->capacity || (timing->words & 1) || *cursor > timing->count) {
        return -1;
    }

    if (!timing->count) {
        return timing->words ? -1 : 0;
    }

    if (!timing->waits || timing->words < 2 ||
        timing->waits[timing->count - 1].offset != timing->words - 2) {
        return -1;
    }

    if (*cursor == timing->count) {
        return 0;
    }

    const UWORD* records = timing->live ? timing->live : timing->records;
    ULONG index = *cursor;

    if (!timing->waits || !records || !viewFieldWaitMatches(timing, records, index)) {
        return -1;
    }

    struct ViewFieldRun selected = {0};
    selected.end = timing->waits[index].position;

    if (index) {
        if (!viewFieldWaitMatches(timing, records, index - 1)) {
            return -1;
        }

        selected.first = timing->waits[index - 1].offset + 2;
        selected.start = timing->waits[index - 1].position;
    }

    if (selected.first > timing->waits[index].offset) {
        return -1;
    }

    selected.words = timing->waits[index].offset - selected.first;
    selected.records = records + selected.first;

    // Reject an edited/uncaptured WAIT before the consumer changes its state.
    for (ULONG word = 0; word < selected.words; word += 2) {
        if (selected.records[word] & 1) {
            return -1;
        }
    }

    *run = selected;
    *cursor = index + 1;

    return 1;
}

LONG pcNextViewPortRunSpan(const struct ViewPortDisplaySnapshot* display,
    const struct ViewFieldRun* run, ULONG* cursor, struct ViewPortRunSpan* span)
{
    struct GraphicsFramebufferScaleArea window;

    if (!run || !cursor || !span || !pcGetViewPortFieldArea(display, &window) ||
        *cursor > window.height || run->start.x < 0 || run->start.x > 254 ||
        run->end.x < 0 || run->end.x > 254 || ((run->start.x | run->end.x) & 1) ||
        run->start.y > run->end.y || (run->start.y == run->end.y && run->start.x > run->end.x)) {
        return -1;
    }

    if (!window.width || !window.height) {
        return 0;
    }

    LONG first_row = (LONG)run->start.y - window.dest_y;
    LONG last_row = (LONG)run->end.y - window.dest_y;
    LONG row = *cursor;

    if (row < first_row) {
        row = first_row;
    }

    // HRM chapter 2: the even horizontal comparison steps cover four LORES
    // pixels. Convert the numeric counter units before subtracting the window
    // origin, which is already in this mode's native pixel coordinates.
    LONG pixels_per_clock = 2L << display->horizontal_shift;
    LONG first_pixel = run->start.x * pixels_per_clock - display->display.MinX;
    LONG last_pixel = run->end.x * pixels_per_clock - display->display.MinX;

    for (; row < (LONG)window.height && row <= last_row; ++row) {
        LONG first = row == first_row ? first_pixel : 0;
        LONG end = row == last_row ? last_pixel : (LONG)window.width;

        if (first < 0) {
            first = 0;
        }

        if (end > (LONG)window.width) {
            end = window.width;
        }

        if (first < end) {
            struct ViewPortRunSpan selected;
            selected.row = row;
            selected.first = first;
            selected.end = end;
            *span = selected;
            *cursor = row + 1;

            return 1;
        }
    }

    return 0;
}

static void updateSpriteControl(struct ViewPortSpriteLayout* sprites, UWORD control);

static BOOL applyViewFieldColors(struct GraphicsFramebufferPalette* colors, UWORD* control,
    struct PlayfieldPaletteLayout* palette, struct ViewPortSpriteLayout* sprites,
    struct ViewPortDisplaySnapshot* display, const struct ViewFieldRun* run)
{
    if (!colors || !control || !run || (run->words & 1) || (!run->records && run->words)) {
        return FALSE;
    }

    for (ULONG word = 0; word < run->words; word += 2) {
        if (run->records[word] & 1) {
            return FALSE;
        }
    }

    for (ULONG word = 0; word < run->words; word += 2) {
        UWORD destination = run->records[word];
        UWORD data = run->records[word + 1];

        if (destination == 0x104 && display) {
            // The list's low seven bits select the two playfield/sprite
            // priorities and the playfield ordering. Change working state
            // only; geometry, fetches and all other mode bits are retained.
            display->sprite_mask[0] = spritePriorityMask(data & 7);
            display->sprite_mask[1] = spritePriorityMask((data >> 3) & 7);
            display->modes = (display->modes & ~((ULONG)PFBA)) | (data & PFBA);
        } else if (destination == 0x106) {
            *control = data;

            if (palette) {
                unsigned offset = (data >> 10) & 7;
                palette->second_offset = offset ? 1U << offset : 0;
                updateSpriteControl(sprites, data);
            }
        } else if (destination == 0x10c) {
            if (palette) {
                palette->index_xor = data >> 8;
                sprites->even_bank = (data >> 4) & 15;
                sprites->odd_bank = data & 15;
            }
        } else if (destination >= 0x180 && destination < 0x1c0) {
            unsigned index = (*control >> 13) * 32 + (destination - 0x180) / 2;
            colors->colors[index] = pcApplyRGB4Color(colors->colors[index], data, (*control & 0x200) != 0);
        }
    }

    return TRUE;
}

BOOL pcApplyViewFieldPaletteRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    const struct ViewFieldRun* run)
{
    return applyViewFieldColors(colors, control, NULL, NULL, NULL, run);
}

BOOL pcApplyViewFieldColorRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    struct PlayfieldPaletteLayout* palette, struct ViewPortSpriteLayout* sprites,
    const struct ViewFieldRun* run)
{
    if (!palette || !sprites) {
        return FALSE;
    }

    return applyViewFieldColors(colors, control, palette, sprites, NULL, run);
}

BOOL pcApplyViewFieldDisplayRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    struct ViewPortDisplaySnapshot* display, const struct ViewFieldRun* run)
{
    if (!display) {
        return FALSE;
    }

    return applyViewFieldColors(colors, control, &display->palette_layout,
        &display->sprites, display, run);
}

void pcCaptureViewDisplayPalettes(struct ViewDisplaySnapshot* display,
    const struct cprlist* long_list, const UWORD* long_end,
    const struct cprlist* short_list, const UWORD* short_end)
{
    for (ULONG index = 0; index < display->count; ++index) {
        struct ViewDisplayPort* viewport = &display->viewports[index];
        const struct CopList* list = viewport->prepared->list;
        viewport->palette[0].owner = long_list;
        viewport->palette[1].owner = short_list;
        viewport->palette[0].start = long_end ? list->CopLStart : NULL;
        viewport->palette[0].end = long_end;
        viewport->palette[1].start = short_end ? list->CopSStart : NULL;
        viewport->palette[1].end = short_end;
    }

    for (unsigned field = 0; field < 2; ++field) {
        struct ViewFieldTiming* timing = &display->timing[field];

        if (timing->records) {
            timing->owner = field ? short_list : long_list;
            timing->live = timing->owner->start;
            timing->identity = (ULONG)timing->live;
            copyViewFieldRecords(timing);
        }

        display->first_port[field] = ~0UL;

        for (ULONG index = 0; index < display->count; ++index) {
            struct ViewFieldPalette* palette = &display->viewports[index].palette[field];
            palette->next_port = ~0UL;

            if (!palette->start || !palette->end || palette->start >= palette->end) {
                continue;
            }

            // Compare positions only while the merger owns the live output.
            // Keep indices thereafter, including when a retained field outlives
            // its list. Equal positions preserve the captured chain order.
            ULONG* link = &display->first_port[field];

            while (*link != ~0UL) {
                struct ViewFieldPalette* preceding = &display->viewports[*link].palette[field];

                if (preceding->start > palette->start) {
                    break;
                }

                link = &preceding->next_port;
            }

            palette->next_port = *link;
            *link = index;
        }

        for (ULONG index = 0; index < display->count; ++index) {
            struct ViewFieldPalette* palette = &display->viewports[index].palette[field];
            ULONG next = palette->next_port;

            // Multiple references to one prefix share its complete range.
            // Only a later position ends it, never an alias at the same word.
            while (next != ~0UL) {
                const struct ViewFieldPalette* following = &display->viewports[next].palette[field];

                if (following->start > palette->start) {
                    palette->end = following->start;
                    break;
                }

                next = following->next_port;
            }

            readViewFieldPalette(palette);
        }
    }
}

void pcResolveViewFieldPalette(struct GraphicsFramebufferPalette* output,
    const struct GraphicsFramebufferPalette* preceding, const struct ViewFieldPalette* palette)
{
    for (unsigned index = 0; index < 256; ++index) {
        ULONG selected = 0x80000000UL >> (index & 31);
        ULONG mask = palette->high_mask[index >> 5] & selected ? 0xf0f0f0UL : 0;

        if (palette->low_mask[index >> 5] & selected) {
            mask |= 0x0f0f0fUL;
        }

        output->colors[index] = (palette->colors.colors[index] & mask) | (preceding->colors[index] & ~mask);
    }
}

const struct ViewDisplayPort* pcNextViewFieldPort(const struct ViewDisplaySnapshot* display,
    unsigned field, ULONG* cursor, struct GraphicsFramebufferPalette* colors)
{
    if (!display || field >= 2 || !cursor || !colors || *cursor >= display->count) {
        return NULL;
    }

    const struct ViewDisplayPort* port = &display->viewports[*cursor];
    pcResolveViewFieldPalette(colors, colors, &port->palette[field]);
    *cursor = port->palette[field].next_port;

    return port;
}

static LONG nextViewFieldPortEvent(const struct ViewDisplaySnapshot* display, unsigned field,
    ULONG* cursor, struct ViewFieldPortEvent* event, BOOL setup)
{
    if (!display || field >= 2 || !cursor || !event) {
        return -1;
    }

    if (*cursor == ~0UL) {
        return 0;
    }

    if (*cursor >= display->count) {
        return -1;
    }

    const struct ViewDisplayPort* port = &display->viewports[*cursor];
    ULONG word = setup ? port->setup_word[field] : port->prefix_word[field];
    ULONG next = port->palette[field].next_port;

    if (!(port->prefix_wait_mask & (1U << field)) || (word & 1) ||
        word > display->timing[field].words || next == *cursor ||
        (setup && ((port->prefix_word[field] & 1) || word < port->prefix_word[field]))) {
        return -1;
    }

    if (next != ~0UL) {
        if (next >= display->count || !(display->viewports[next].prefix_wait_mask & (1U << field)) ||
            (setup ? display->viewports[next].setup_word[field] :
                display->viewports[next].prefix_word[field]) < word) {
            return -1;
        }
    }

    struct ViewFieldPortEvent selected;
    selected.word = word;
    selected.port = port;
    *event = selected;
    *cursor = next;

    return 1;
}

LONG pcNextViewFieldPortEvent(const struct ViewDisplaySnapshot* display, unsigned field,
    ULONG* cursor, struct ViewFieldPortEvent* event)
{
    return nextViewFieldPortEvent(display, field, cursor, event, FALSE);
}

LONG pcNextViewFieldSetupEvent(const struct ViewDisplaySnapshot* display, unsigned field,
    ULONG* cursor, struct ViewFieldPortEvent* event)
{
    return nextViewFieldPortEvent(display, field, cursor, event, TRUE);
}

static BOOL visitViewFieldMoves(const struct ViewFieldVisitor* visitor, APTR context,
    const struct ViewFieldRun* run, ULONG first, ULONG end)
{
    if (first == end) {
        return TRUE;
    }

    struct ViewFieldRun selected = *run;
    selected.first = first;
    selected.words = end - first;
    selected.records += first - run->first;

    return visitor->moves(context, &selected);
}

static BOOL visitViewField(const struct ViewDisplaySnapshot* display, unsigned field,
    const struct ViewFieldVisitor* visitor, APTR context, BOOL setup)
{
    if (!display || field >= 2 || !visitor || !visitor->prefix || !visitor->moves || !visitor->wait) {
        return FALSE;
    }

    const struct ViewFieldTiming* timing = &display->timing[field];
    ULONG run_cursor = 0;
    ULONG port_cursor = display->first_port[field];
    ULONG remaining = display->count;
    struct ViewFieldPortEvent event;
    LONG has_port = nextViewFieldPortEvent(display, field, &port_cursor, &event, setup);

    for (;;) {
        struct ViewFieldRun run = {0};
        LONG has_run = pcNextViewFieldRun(timing, &run_cursor, &run);

        if (has_run < 0 || has_port < 0) {
            return FALSE;
        }

        if (!has_run) {
            // A prefix may have no records after it. Drain that final position
            // through the same ordering path without inventing another WAIT.
            run.first = timing->words;
        }

        ULONG first = run.first;
        ULONG end = first + run.words;

        while (has_port && event.word <= end) {
            if (!remaining || event.word < first ||
                !visitViewFieldMoves(visitor, context, &run, first, event.word)) {
                return FALSE;
            }

            --remaining;
            first = event.word;

            if (!visitor->prefix(context, &event)) {
                return FALSE;
            }

            has_port = nextViewFieldPortEvent(display, field, &port_cursor, &event, setup);

            if (has_port < 0) {
                return FALSE;
            }
        }

        if (!visitViewFieldMoves(visitor, context, &run, first, end)) {
            return FALSE;
        }

        if (!has_run) {
            return has_port == 0;
        }

        if (!visitor->wait(context, end, &run.end, run_cursor == timing->count)) {
            return FALSE;
        }
    }
}

BOOL pcVisitViewField(const struct ViewDisplaySnapshot* display, unsigned field,
    const struct ViewFieldVisitor* visitor, APTR context)
{
    return visitViewField(display, field, visitor, context, FALSE);
}

BOOL pcVisitViewFieldSetup(const struct ViewDisplaySnapshot* display, unsigned field,
    const struct ViewFieldVisitor* visitor, APTR context)
{
    return visitViewField(display, field, visitor, context, TRUE);
}

struct ViewFieldSchedule {
    const struct ViewFieldScheduleVisitor* visitor;
    APTR context;
    Point position;
    UWORD rows, last_horizontal;
};

static BOOL scheduleViewSetup(APTR context, const struct ViewFieldPortEvent* event)
{
    struct ViewFieldSchedule* schedule = context;

    if (schedule->position.y == schedule->rows) {
        return TRUE;
    }

    return schedule->visitor->setup(schedule->context, event, &schedule->position);
}

static BOOL scheduleViewMoves(APTR context, const struct ViewFieldRun* run)
{
    struct ViewFieldSchedule* schedule = context;

    if (schedule->position.y == schedule->rows) {
        return TRUE;
    }

    struct ViewFieldRun selected = *run;
    selected.start = selected.end = schedule->position;

    return schedule->visitor->moves(schedule->context, &selected);
}

static BOOL scheduleViewInterval(struct ViewFieldSchedule* schedule, Point end)
{
    if (!fieldPositionBefore(&schedule->position, &end)) {
        return TRUE;
    }

    struct ViewFieldRun run = {0};
    run.start = schedule->position;
    run.end = end;

    if (!schedule->visitor->interval(schedule->context, &run)) {
        return FALSE;
    }

    schedule->position = end;

    return TRUE;
}

static BOOL scheduleViewWait(APTR context, ULONG word, const Point* position, BOOL terminal)
{
    (void)word;
    (void)terminal;
    struct ViewFieldSchedule* schedule = context;

    if (position->y < 0 || position->x < 0 || position->x > 254 || (position->x & 1)) {
        return FALSE;
    }

    Point end = *position;

    // The final sentinel may retain its first-page encoding after a list
    // has advanced through later pages. Compare it in the current page;
    // a WAIT cannot rewind the field. Explicit later-page captures still win.
    if ((end.y & ~255) < (schedule->position.y & ~255)) {
        end.y = (schedule->position.y & ~255) | (end.y & 255);
    }

    if (end.x > schedule->last_horizontal) {
        // The comparison can pass on a later vertical line even if this
        // horizontal target is absent. At vertical 255 that comparison wraps
        // instead, so the usual FFFF/FFFE sentinel remains pending.
        if ((end.y & 255) == 255) {
            end.y = schedule->rows;
        } else if (end.y < schedule->rows) {
            ++end.y;
        }

        end.x = 0;
    }

    if (end.y >= schedule->rows) {
        end.x = 0;
        end.y = schedule->rows;
    }

    return scheduleViewInterval(schedule, end);
}

BOOL pcScheduleViewField(const struct ViewDisplaySnapshot* display, unsigned field,
    UWORD rows, UWORD last_horizontal, const struct ViewFieldScheduleVisitor* visitor, APTR context)
{
    if (!rows || rows > 1024 || last_horizontal > 254 || (last_horizontal & 1) ||
        !visitor || !visitor->setup || !visitor->moves || !visitor->interval) {
        return FALSE;
    }

    static const struct ViewFieldVisitor callbacks = {
        scheduleViewSetup, scheduleViewMoves, scheduleViewWait
    };
    struct ViewFieldSchedule schedule = {0};
    schedule.visitor = visitor;
    schedule.context = context;
    schedule.rows = rows;
    schedule.last_horizontal = last_horizontal;

    if (!pcVisitViewFieldSetup(display, field, &callbacks, &schedule)) {
        return FALSE;
    }

    Point end = {0};
    end.y = rows;

    return scheduleViewInterval(&schedule, end);
}

struct ViewGelImages {
    struct NativeSpriteSource sources[8];
    UBYTE written[8];
};

static BOOL readNativeViewControls(ULONG* link, struct ViewGelImages* images, const struct ViewFieldRun* run)
{
    if (!link || !run || (run->words & 1) || (run->words && !run->records)) {
        return FALSE;
    }

    // Validate the entire batch before applying either link half. Native
    // geometry/fetches live in prepared snapshots, so foreign controls must
    // fail identically during reader discovery and during composition.
    for (ULONG word = 0; word < run->words; word += 2) {
        UWORD destination = run->records[word];
        UWORD data = run->records[word + 1];
        BOOL pointer = destination >= 0x120 && destination < 0x140;
        BOOL position = destination >= 0x140 && destination < 0x180 && !(destination & 4);

        if ((destination & 1) || (destination == 0x104 && (run->records[word + 1] & ~0x7f)) ||
            (destination != 0x84 && destination != 0x86 && destination != 0x104 &&
                destination != 0x106 && destination != 0x10c &&
                !pointer && !position &&
                (destination < 0x180 || destination >= 0x1c0))) {
            return FALSE;
        }

        if (pointer) {
            unsigned channel = (destination - 0x120) / 4;
            ULONG address = (ULONG)images->sources[channel].image;
            address = destination & 2 ? (address & 0xffff0000UL) | data :
                (address & 0xffff) | ((ULONG)data << 16);
            images->sources[channel].image = (APTR)address;
            images->written[channel] |= destination & 2 ? 2 : 1;
        } else if (position) {
            unsigned channel = (destination - 0x140) / 8;

            if (destination & 2) {
                images->sources[channel].control = data;
                images->written[channel] |= 8;
            } else {
                images->sources[channel].position = data;
                images->written[channel] |= 4;
            }
        }
    }

    // DrawGList supplies all four words together between waits. Partial
    // application-written sprite operations have different source semantics;
    // do not accept those as a complete GEL image or ignore the missing words.
    for (unsigned channel = 0; channel < 8; ++channel) {
        if (images->written[channel] && images->written[channel] != 15) {
            return FALSE;
        }
    }

    ULONG selected = *link;

    for (ULONG word = 0; word < run->words; word += 2) {
        UWORD destination = run->records[word];
        UWORD data = run->records[word + 1];

        if (destination == 0x84) {
            selected = (selected & 0xffff) | ((ULONG)data << 16);
        } else if (destination == 0x86) {
            selected = (selected & 0xffff0000UL) | data;
        }
    }

    *link = selected;

    return TRUE;
}

BOOL pcReadNativeViewFieldLink(ULONG* link, const struct ViewFieldRun* run)
{
    struct ViewGelImages images = {0};

    return readNativeViewControls(link, &images, run);
}

BOOL pcApplyNativeViewFieldControls(APTR context, struct ViewFieldComposition* composition,
    const struct ViewFieldRun* run)
{
    struct ViewFieldControls* controls = context;

    if (!controls || !composition || !composition->colors) {
        return FALSE;
    }

    ULONG link = controls->field_link;
    struct ViewGelImages images = {0};

    if (!readNativeViewControls(&link, &images, run)) {
        return FALSE;
    }

    struct NativeSpriteField* field = composition->sprites.field;

    for (unsigned channel = 0; channel < 8; ++channel) {
        if (!images.written[channel]) {
            continue;
        }

        if (!validViewFieldSpriteRun(&composition->sprites, run)) {
            return FALSE;
        }

        UWORD width = field->cursors[channel].word_width;

        if (width != 1 && width != 2 && width != 4) {
            return FALSE;
        }
    }

    if (!pcApplyViewFieldDisplayRun(composition->colors, &controls->display, &composition->working, run)) {
        return FALSE;
    }

    for (unsigned channel = 0; channel < 8; ++channel) {
        if (images.written[channel]) {
            const struct NativeSpriteSource* image = &images.sources[channel];
            pcSelectNativeGelImage(field, channel, image->image, image->position, image->control);
        }
    }

    controls->field_link = link;

    return TRUE;
}

static BOOL composeScheduledSetup(APTR context, const struct ViewFieldPortEvent* event, const Point* position)
{
    struct ViewFieldComposition* composition = context;
    const struct ViewDisplayPort* port = event->port;

    if (!port->prepared || !port->prepared->display) {
        return FALSE;
    }

    composition->working = *port->prepared->display;
    composition->working.rasters = port->rasters[composition->field];
    composition->working.sprites = port->sprites[composition->field];
    composition->raster = (struct ViewFieldRasterRuns){0};
    composition->raster.position = *position;
    composition->active = TRUE;

    return TRUE;
}

static BOOL composeScheduledMoves(APTR context, const struct ViewFieldRun* run)
{
    struct ViewFieldComposition* composition = context;

    return composition->controls(composition->context, composition, run);
}

static BOOL composeScheduledInterval(APTR context, const struct ViewFieldRun* run)
{
    struct ViewFieldComposition* composition = context;

    if (composition->active) {
        return pcComposeViewFieldRun(composition->framebuffer, composition->scratch,
            &composition->raster, &composition->sprites, run, &composition->working,
            &composition->working.rasters, composition->colors, composition->colors->colors[0],
            composition->occlusion);
    }

    struct GraphicsFramebuffer* framebuffer = composition->framebuffer;
    const struct ViewFieldOcclusion* occlusion = composition->occlusion;
    struct Rectangle region = {0}, window = {0};
    region.MaxX = framebuffer->mode.width;
    region.MaxY = framebuffer->mode.height;

    if (!validViewFieldSpriteRun(&composition->sprites, run) ||
        !validViewPortSpriteLayout(&composition->working.sprites, 2) ||
        !composeViewFieldBackground(framebuffer, &composition->sprites, run, &composition->working.sprites,
            composition->colors->colors[0], occlusion, &region, &window)) {
        return FALSE;
    }

    struct GraphicsSpriteOcclusion mask = {occlusion->pixels, occlusion->stride, occlusion->size};

    return pcRenderViewFieldSpriteRun(framebuffer, &composition->sprites, run,
        &composition->working.sprites, composition->working.modes, composition->colors,
        &mask, &region, &window);
}

BOOL pcComposeScheduledViewField(struct ViewFieldComposition* composition,
    const struct ViewDisplaySnapshot* display, unsigned field, UWORD last_horizontal)
{
    if (!composition || !display || field >= 2 || !composition->framebuffer || !composition->scratch ||
        !composition->occlusion || !composition->colors || !composition->controls ||
        !composition->sprites.field || composition->sprites.field->line ||
        composition->sprites.position.x || composition->sprites.position.y || composition->sprites.row_ready ||
        composition->framebuffer->mode.width > INT16_MAX || composition->framebuffer->mode.height > INT16_MAX ||
        !fieldOcclusionFits(composition->occlusion, composition->framebuffer, composition->scratch)) {
        return FALSE;
    }

    static const struct ViewFieldScheduleVisitor visitor = {
        composeScheduledSetup, composeScheduledMoves, composeScheduledInterval
    };
    composition->field = field;
    composition->active = FALSE;

    return pcScheduleViewField(display, field, composition->sprites.field->limit, last_horizontal,
        &visitor, composition);
}

BOOL pcResolveViewFieldLink(const struct ViewDisplaySnapshot* display, ULONG identity, unsigned* field)
{
    if (!display || !identity || !field) {
        return FALSE;
    }

    unsigned selected = 2;

    for (unsigned source = 0; source < 2; ++source) {
        if (display->timing[source].identity == identity) {
            if (selected != 2) {
                return FALSE;
            }

            selected = source;
        }
    }

    if (selected == 2) {
        return FALSE;
    }

    *field = selected;

    return TRUE;
}

BOOL pcComposeNativeViewField(struct ViewFieldComposition* composition,
    const struct ViewDisplaySnapshot* display, unsigned field, UWORD last_horizontal,
    struct ViewFieldControls* controls, unsigned* next_field)
{
    unsigned selected;

    if (!composition || !display || field >= 2 || !controls || !next_field ||
        !pcResolveViewFieldLink(display, display->timing[field].identity, &selected)) {
        return FALSE;
    }

    controls->field_link = display->timing[field].identity;
    composition->controls = pcApplyNativeViewFieldControls;
    composition->context = controls;

    if (!pcComposeScheduledViewField(composition, display, field, last_horizontal) ||
        !pcResolveViewFieldLink(display, controls->field_link, &selected)) {
        return FALSE;
    }

    *next_field = selected;

    return TRUE;
}

BOOL pcRenderViewFieldRasters(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    const struct ViewDisplaySnapshot* display, unsigned field, struct GraphicsFramebufferPalette* colors,
    LONG origin_x, LONG origin_y, ULONG background, const struct ViewFieldOcclusion* occlusion)
{
    if (!framebuffer || !scratch || !display || field >= 2 || !colors) {
        return FALSE;
    }

    ULONG cursor = display->first_port[field];
    ULONG remaining = display->count;

    while (cursor != ~0UL) {
        if (!remaining--) {
            return FALSE;
        }

        const struct ViewDisplayPort* port = pcNextViewFieldPort(display, field, &cursor, colors);

        if (!port || !port->prepared || !port->prepared->display ||
            !pcRenderViewPortField(framebuffer, scratch, port->prepared->display, &port->rasters[field],
                colors, origin_x, origin_y, background, occlusion)) {
            return FALSE;
        }
    }

    return TRUE;
}

void pcRefreshMergedViewPalettes(struct MergedViews* merged, const struct CopList* list)
{
    for (struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (unsigned field = 0; field < 2; ++field) {
            if (!entry->fields[field]) {
                continue;
            }

            BOOL changed = FALSE;

            for (ULONG index = 0; index < entry->display->count; ++index) {
                const struct ViewFieldPalette* palette = &entry->display->viewports[index].palette[field];

                if (palette->start &&
                    (palette->start == list->CopLStart || palette->start == list->CopSStart)) {
                    changed = TRUE;
                    break;
                }
            }

            if (changed) {
                // The original updater can search beyond an empty prefix for
                // color zero. Refresh the affected field, so the actual later
                // owner observes that write without attributing it to this one.
                for (ULONG index = 0; index < entry->display->count; ++index) {
                    struct ViewFieldPalette* palette = &entry->display->viewports[index].palette[field];

                    if (palette->start) {
                        readViewFieldPalette(palette);
                    }
                }
            }
        }
    }
}

static void updateSpriteControl(struct ViewPortSpriteLayout* sprites, UWORD control)
{
    sprites->resolution = (control >> 6) & 3;
    sprites->border_sprites = (control & 2) != 0;
    sprites->border_blank = (control & 0x20) != 0;
}

void pcUpdateViewPortSpriteControl(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, UWORD control, UWORD written)
{
    if (written & 4) {
        for (struct PreparedViewPort* entry = prepared->first; entry; entry = entry->next) {
            if (entry->list == list) {
                updateSpriteControl(&entry->display->sprites, control);
                break;
            }
        }
    }

    for (struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (ULONG index = 0; index < entry->display->count; ++index) {
            struct ViewDisplayPort* port = &entry->display->viewports[index];

            for (unsigned field = 0; field < 2; ++field) {
                const UWORD* start = port->palette[field].start;

                if (entry->fields[field] && start &&
                    (((written & 1) && start == list->CopLStart) ||
                     ((written & 2) && start == list->CopSStart))) {
                    updateSpriteControl(&port->sprites[field], control);
                }
            }
        }
    }
}

void pcReadViewPortScrollFetches(struct ViewPortPlaneFetch* fetches, const struct ViewPort* viewport,
    UWORD modes, const LONG* source_x, const LONG* source_y, const UWORD* source_stride)
{
    const struct RasInfo* first = viewport->RasInfo;
    BOOL dual = first->Next && (modes & DUALPF);
    BOOL rotated = (modes & HAM) && (dual || first->BitMap->Depth == 8);

    // A missing second raster selects the single-raster path even with
    // DUALPF set. Only that path clears HAM selection for a non-eight-plane
    // source. Current depth never limits the eight fetched source slots.
    for (unsigned slot = 0; slot < 8; ++slot) {
        unsigned source = rotated ? (slot + 6) & 7 : slot;
        unsigned playfield = dual ? source & 1 : 0;
        unsigned plane = dual ? source >> 1 : source;
        const struct BitMap* bitmap = playfield ? first->Next->BitMap : first->BitMap;
        fetches[slot].plane = bitmap->Planes[plane];
        fetches[slot].bytes_per_row = bitmap->BytesPerRow;
        fetches[slot].source_x = source_x[playfield];
        fetches[slot].source_y = source_y[playfield];

        // Preparation fixes the initial row displacement. Cleanup callbacks
        // can change the stride before source selection; retain both values
        // without dividing by the new stride (which may legitimately be zero).
        LONG difference = (LONG)source_stride[playfield] - bitmap->BytesPerRow;
        fetches[slot].row_offset = (LONG)((ULONG)source_y[playfield] * (ULONG)difference);
    }
}

static void updateRasterFetches(struct ViewPortRasterState* target, const struct ViewPortRasterState* source)
{
    target->plane_fetches = source->plane_fetches;
    target->interlaced = source->interlaced;

    if (source->plane_fetches) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            target->fetches[slot] = source->fetches[slot];
        }

        return;
    }

    // Scrolling changes fetch addresses and row strides. Plane enables and
    // allocation bounds belong to the prepared display, not the new BitMap.
    for (unsigned playfield = 0; playfield < 2; ++playfield) {
        if (!(target->present & (1UL << playfield))) {
            continue;
        }

        struct BitMap* bitmap = &target->bitmap[playfield];
        bitmap->BytesPerRow = source->bitmap[playfield].BytesPerRow;
        target->source_x[playfield] = source->source_x[playfield];
        target->source_y[playfield] = source->source_y[playfield];

        for (unsigned plane = 0; plane < bitmap->Depth && plane < 8; ++plane) {
            bitmap->Planes[plane] = source->bitmap[playfield].Planes[plane];
        }
    }
}

BOOL pcCacheNativeDBufInfo(const struct ViewPort* viewport, struct DBufInfo* info,
    struct GfxBase* graphics)
{
    if (info->dbi_MatchLong == graphics->SpecialCounter) {
        return TRUE;
    }

    const struct CopList* list = viewport->DspIns;

    if (!list) {
        return FALSE;
    }

    BOOL hidden = viewport->Modes & VP_HIDE;
    const struct ColorMap* map = viewport->ColorMap;
    BOOL intermediate = !hidden && list->CopIns && (!map || !(map->AuxFlags & CMAF_NO_INTERMED_UPDATE));
    info->dbi_CopPtr1 = intermediate ? (APTR)list : NULL;
    info->dbi_CopPtr2 = hidden ? NULL : list->CopSStart;
    info->dbi_CopPtr3 = hidden ? NULL : list->CopLStart;

    ULONG modes = pcViewportModes(viewport);
    WORD top = 30000;

    if (!hidden && graphics->ActiView) {
        top = viewport->DyOffset;

        if (modes & LACE) {
            top >>= 1;
        }

        if (modes & DOUBLESCAN) {
            top = (WORD)(top + top);
        }

        top = (WORD)(top + graphics->ActiView->DyOffset);
    }

    WORD height = viewport->DHeight;

    if (modes & DOUBLESCAN) {
        height = (WORD)(height + height);
    }

    WORD bottom = (WORD)(top + height);
    WORD clipped = (WORD)(top - graphics->current_monitor->min_row);

    if (clipped < 0) {
        bottom = (WORD)(bottom + clipped);
        top = (WORD)(top - clipped);
    }

    info->dbi_MatchLong = graphics->SpecialCounter;
    info->dbi_BeamPos1 = (UWORD)top;
    info->dbi_BeamPos2 = (UWORD)bottom;

    return TRUE;
}

void pcRelocateViewPortRaster(struct ViewPortRasterState* rasters,
    const struct ViewPortDisplaySnapshot* display, const struct BitMap* previous,
    const struct BitMap* next, UWORD modes, UBYTE slots)
{
    ULONG differences[8];

    for (unsigned slot = 0; slot < 8; ++slot) {
        if (slots & (1U << slot)) {
            unsigned plane = (modes & HAM) && next->Depth == 8 ? (slot + 6) & 7 : slot;
            differences[slot] = (ULONG)next->Planes[plane] - (ULONG)previous->Planes[plane];
        }
    }

    if (rasters->plane_fetches) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            if (slots & (1U << slot)) {
                rasters->fetches[slot].plane = (PLANEPTR)((ULONG)rasters->fetches[slot].plane + differences[slot]);
            }
        }

        return;
    }

    BOOL dual = (display->modes & DUALPF) && (rasters->present & 2);
    BOOL ham_planar = (display->modes & HAM) && display->ham_format == GRAPHICS_HAM8_PLANAR;

    for (unsigned field = 0; field < (dual ? 2U : 1U); ++field) {
        if (!(rasters->present & (1UL << field))) {
            continue;
        }

        for (unsigned plane = 0; plane < 8; ++plane) {
            unsigned slot = displayPlaneSlot(plane, field, dual, ham_planar);

            if (slot < 8 && (slots & (1U << slot))) {
                PLANEPTR* address = &rasters->bitmap[field].Planes[plane];
                *address = (PLANEPTR)((ULONG)*address + differences[slot]);
            }
        }
    }
}

UWORD pcRelocateViewPortRasters(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* intermediate, const UWORD* long_start, const UWORD* short_start,
    const struct BitMap* previous, const struct BitMap* next, UWORD modes,
    UBYTE intermediate_slots, UBYTE field_slots)
{
    UWORD updated = 0;

    if (intermediate && intermediate_slots) {
        for (struct PreparedViewPort* entry = prepared->first; entry; entry = entry->next) {
            if (entry->list == intermediate) {
                pcRelocateViewPortRaster(&entry->display->rasters, entry->display,
                    previous, next, modes, intermediate_slots);
                updated |= 4;
                break;
            }
        }
    }

    if (!field_slots) {
        return updated;
    }

    for (struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (unsigned field = 0; field < 2; ++field) {
            if (!entry->fields[field]) {
                continue;
            }

            // More than one output key can retain the same snapshot. Apply
            // address differences once per field, not once per key: unlike
            // a scroll assignment, repeating a relocation moves it twice.
            const struct MergedView* preceding = merged->first;

            while (preceding != entry &&
                (preceding->display != entry->display || !preceding->fields[field])) {
                preceding = preceding->next;
            }

            if (preceding != entry) {
                continue;
            }

            for (ULONG index = 0; index < entry->display->count; ++index) {
                struct ViewDisplayPort* port = &entry->display->viewports[index];
                const UWORD* start = port->palette[field].start;

                if (!start) {
                    continue;
                }

                if (start == long_start) {
                    pcRelocateViewPortRaster(&port->rasters[field], port->prepared->display,
                        previous, next, modes, field_slots);
                    updated |= 1;
                }

                // The original writes both cached targets even when they
                // alias. That deliberate second write is separate from the
                // duplicate ownership keys excluded above.
                if (start == short_start) {
                    pcRelocateViewPortRaster(&port->rasters[field], port->prepared->display,
                        previous, next, modes, field_slots);
                    updated |= 2;
                }
            }
        }
    }

    return updated;
}

BOOL pcSwitchNativeViewPortBitMap(struct ViewPort* viewport, struct BitMap* bitmap,
    struct DBufInfo* info, struct GfxBase* graphics)
{
    if (!pcCacheNativeDBufInfo(viewport, info, graphics)) {
        return FALSE;
    }

    if (viewport->Modes & VP_HIDE) {
        return TRUE;
    }

    struct PreparedViewPorts* prepared = pcPreparedViewPorts(graphics);
    UBYTE intermediate_slots = 0;

    for (const struct PreparedViewPort* entry = prepared->first; entry; entry = entry->next) {
        if (entry->list == info->dbi_CopPtr1) {
            const struct ViewPortRasterState* rasters = &entry->display->rasters;
            unsigned depth = 0;

            for (unsigned field = 0; field < 2; ++field) {
                if (rasters->present & (1U << field)) {
                    depth += rasters->bitmap[field].Depth;
                }
            }

            intermediate_slots = depth >= 8 ? 255 : (1U << depth) - 1;
            break;
        }
    }

    struct BitMap* previous = viewport->RasInfo->BitMap;
    viewport->RasInfo->BitMap = bitmap;
    UBYTE field_slots = bitmap->Depth >= 8 ? 255 : (1U << bitmap->Depth) - 1;
    pcRelocateViewPortRasters(prepared, pcMergedViews(graphics), info->dbi_CopPtr1,
        info->dbi_CopPtr3, info->dbi_CopPtr2, previous, bitmap, viewport->Modes,
        intermediate_slots, field_slots);

    return TRUE;
}

static UWORD updateViewPortRasters(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, const struct ViewPortRasterState* intermediate,
    const UWORD* long_start, const struct ViewPortRasterState* long_field,
    const UWORD* short_start, const struct ViewPortRasterState* short_field)
{
    UWORD updated = 0;

    if (intermediate) {
        for (struct PreparedViewPort* entry = prepared->first; entry; entry = entry->next) {
            if (entry->list == list) {
                updateRasterFetches(&entry->display->rasters, intermediate);
                break;
            }
        }
    }

    for (struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (ULONG index = 0; index < entry->display->count; ++index) {
            struct ViewDisplayPort* port = &entry->display->viewports[index];

            for (unsigned field = 0; field < 2; ++field) {
                const struct ViewFieldPalette* binding = &port->palette[field];

                if (!entry->fields[field] || !binding->start) {
                    continue;
                }

                if (long_field && binding->start == long_start) {
                    updateRasterFetches(&port->rasters[field], long_field);
                    updated |= 1;
                }

                // If both selected field pointers alias, the short-field write
                // follows the long-field write, as in the native scroll path.
                if (short_field && binding->start == short_start) {
                    updateRasterFetches(&port->rasters[field], short_field);
                    updated |= 2;
                }
            }
        }
    }

    return updated;
}

void pcUpdateViewPortRasters(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, const struct ViewPortRasterState* intermediate,
    const struct ViewPortRasterState* long_field, const struct ViewPortRasterState* short_field)
{
    updateViewPortRasters(prepared, merged, list, intermediate,
        list->CopLStart, long_field, list->CopSStart, short_field);
}

static BOOL viewFieldHasPlanes(const struct MergedViews* merged, const UWORD* start)
{
    if (!start) {
        return FALSE;
    }

    for (const struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (ULONG index = 0; index < entry->display->count; ++index) {
            const struct ViewDisplayPort* port = &entry->display->viewports[index];

            for (unsigned field = 0; field < 2; ++field) {
                if (!entry->fields[field] || port->palette[field].start != start) {
                    continue;
                }

                const struct ViewPortRasterState* rasters = &port->rasters[field];

                for (unsigned playfield = 0; playfield < 2; ++playfield) {
                    if ((rasters->present & (1UL << playfield)) && rasters->bitmap[playfield].Depth) {
                        return TRUE;
                    }
                }
            }
        }
    }

    return FALSE;
}

static void offsetViewPortField(struct ViewPortRasterState* rasters, ULONG offset)
{
    if (rasters->plane_fetches) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            rasters->fetches[slot].row_offset = (LONG)((ULONG)rasters->fetches[slot].row_offset + offset);
        }
    } else {
        for (unsigned playfield = 0; playfield < 2; ++playfield) {
            for (unsigned plane = 0; plane < 8; ++plane) {
                PLANEPTR* pointer = &rasters->bitmap[playfield].Planes[plane];
                *pointer = (PLANEPTR)((ULONG)*pointer + offset);
            }
        }
    }
}

void pcApplyViewPortScroll(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, struct ViewPortExtra* field_extra, const struct ViewPortRasterState* rasters,
    UWORD view_modes, UWORD viewport_modes, BOOL update_intermediate, ULONG caller_d3)
{
    struct ViewPortRasterState short_field;
    const struct ViewPortRasterState* short_rasters = NULL;

    if (view_modes & LACE) {
        short_field = *rasters;

        // Field selection uses the saved mode bits. DOUBLESCAN affects the
        // preparation geometry, but does not suppress this LACE row offset.
        if (viewport_modes & LACE) {
            advanceViewPortField(&short_field);
        }

        short_rasters = &short_field;
    }

    const UWORD* long_start = list->CopLStart;
    const UWORD* short_start = list->CopSStart;

    // Output ownership is captured before mode lookup. A cached native field
    // binding may differ from the current list header, but cannot enable an
    // absent public field. Intermediate ownership still uses the list itself.
    if (field_extra) {
        if (long_start && field_extra->cop1ptr) {
            long_start = (const UWORD*)field_extra->cop1ptr;
        }

        if (short_start && field_extra->cop2ptr) {
            short_start = (const UWORD*)field_extra->cop2ptr;
        }
    }

    // The original short-field pointer calculation retains the caller's upper
    // D3 word until a long-field plane write clears it. Its preparation methods
    // preserve D3. Retain that observable address addition for a short-only
    // interlaced field; it affects neither intermediate sources nor long fields.
    if ((caller_d3 & 0xffff0000UL) && (view_modes & viewport_modes & LACE) &&
        !viewFieldHasPlanes(merged, long_start)) {
        offsetViewPortField(&short_field, caller_d3 & 0xffff0000UL);
    }

    UWORD updated = updateViewPortRasters(prepared, merged, list, update_intermediate ? rasters : NULL,
        long_start, rasters, short_start, short_rasters);

    // Cache only resolved native output bindings. Missing or retired fields
    // cannot publish a new target; an existing cache remains owned by its
    // captured extra even when mode lookup selected another preparation owner.
    if (field_extra) {
        if (updated & 1) {
            field_extra->cop1ptr = (ULONG)long_start;
        }

        if (updated & 2) {
            field_extra->cop2ptr = (ULONG)short_start;
        }
    }
}

void pcApplyViewPortPlaneScroll(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, struct ViewPortExtra* field_extra, const struct ViewPortPlaneFetch* fetches,
    BOOL interlaced, UWORD view_modes, UWORD viewport_modes, BOOL update_intermediate, ULONG caller_d3)
{
    struct ViewPortRasterState sources = {0};
    sources.plane_fetches = TRUE;
    sources.interlaced = interlaced;

    for (unsigned slot = 0; slot < 8; ++slot) {
        sources.fetches[slot] = fetches[slot];
    }

    pcApplyViewPortScroll(prepared, merged, list, field_extra, &sources,
        view_modes, viewport_modes, update_intermediate, caller_d3);
}

static void retireViewFieldRecords(struct ViewFieldTiming* timing)
{
    if (timing->live) {
        copyViewFieldRecords(timing);
    }

    timing->live = NULL;
    timing->owner = NULL;
}

static void retireViewPalette(struct ViewFieldPalette* palette)
{
    if (palette->start) {
        readViewFieldPalette(palette);
    }

    palette->owner = NULL;
    palette->start = NULL;
    palette->end = NULL;
}

static void retireViewFieldPalette(struct ViewDisplaySnapshot* display, unsigned field)
{
    retireViewFieldRecords(&display->timing[field]);

    for (ULONG index = 0; index < display->count; ++index) {
        retireViewPalette(&display->viewports[index].palette[field]);
    }
}

static void retireViewOutput(struct ViewDisplaySnapshot* display, const struct cprlist* list)
{
    for (unsigned field = 0; field < 2; ++field) {
        if (display->timing[field].owner == list) {
            retireViewFieldRecords(&display->timing[field]);
        }

        for (ULONG index = 0; index < display->count; ++index) {
            struct ViewFieldPalette* palette = &display->viewports[index].palette[field];

            if (palette->owner == list) {
                retireViewPalette(palette);
            }
        }
    }
}

void pcRetireMergedViewOutput(struct MergedViews* merged, const struct cprlist* list)
{
    if (list) {
        for (struct MergedView* entry = merged->first; entry; entry = entry->next) {
            retireViewOutput(entry->display, list);
        }
    }
}

BOOL pcStoreMergedView(struct MergedViews* merged, const struct cprlist* long_list,
    const struct cprlist* short_list, struct ViewDisplaySnapshot* display, struct GfxBase* graphics)
{
    if ((!long_list && !short_list) || !display) {
        return FALSE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct MergedView* entry = AllocMem(sizeof(*entry), MEMF_PUBLIC | MEMF_CLEAR);

    if (!entry) {
        return FALSE;
    }

    // Reserve before detaching either field. A failed store must not retire
    // one half of the preceding display or consume the caller's snapshot.
    entry->display = display;
    pcCommitMergedView(merged, entry, long_list, short_list, graphics);

    return TRUE;
}

static void removeMergedView(struct MergedViews* merged, const struct cprlist* list,
    struct ViewDisplaySnapshot* preserved, struct GfxBase* graphics);

void pcCommitMergedView(struct MergedViews* merged, struct MergedView* entry,
    const struct cprlist* long_list, const struct cprlist* short_list, struct GfxBase* graphics)
{
    entry->fields[0] = long_list;
    entry->fields[1] = short_list;
    removeMergedView(merged, long_list, entry->display, graphics);
    removeMergedView(merged, short_list, entry->display, graphics);
    entry->next = merged->first;
    merged->first = entry;
}

struct ViewDisplaySnapshot* pcRetainMergedViewField(const struct MergedViews* merged,
    const struct cprlist* list, unsigned* field)
{
    if (!list) {
        return NULL;
    }

    for (const struct MergedView* entry = merged->first; entry; entry = entry->next) {
        for (unsigned source = 0; source < 2; ++source) {
            if (entry->fields[source] == list) {
                if (field) {
                    *field = source;
                }

                pcRetainViewDisplay(entry->display);
                return entry->display;
            }
        }
    }

    return NULL;
}

struct ViewDisplaySnapshot* pcRetainMergedView(const struct MergedViews* merged,
    const struct cprlist* list)
{
    return pcRetainMergedViewField(merged, list, NULL);
}

static void removeMergedView(struct MergedViews* merged, const struct cprlist* list,
    struct ViewDisplaySnapshot* preserved, struct GfxBase* graphics)
{
    if (!list) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct MergedView** link = &merged->first;

    while (*link) {
        struct MergedView* entry = *link;

        // A snapshot can have extra binding aliases. Retire only the actual
        // output storage owner, not an unrelated alias of the same snapshot.
        if (entry->display != preserved) {
            retireViewOutput(entry->display, list);
        }

        for (unsigned field = 0; field < 2; ++field) {
            if (entry->fields[field] == list) {
                entry->fields[field] = NULL;
            }
        }

        if (!entry->fields[0] && !entry->fields[1]) {
            *link = entry->next;
            pcReleaseViewDisplay(entry->display, graphics);
            FreeMem(entry, sizeof(*entry));
        } else {
            link = &entry->next;
        }
    }
}

void pcRemoveMergedView(struct MergedViews* merged, const struct cprlist* list,
    struct GfxBase* graphics)
{
    removeMergedView(merged, list, NULL, graphics);
}

void pcClearMergedViews(struct MergedViews* merged, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    while (merged->first) {
        struct MergedView* entry = merged->first;
        merged->first = entry->next;
        retireViewFieldPalette(entry->display, 0);
        retireViewFieldPalette(entry->display, 1);
        pcReleaseViewDisplay(entry->display, graphics);
        FreeMem(entry, sizeof(*entry));
    }
}

UWORD pcAcquireViewPortExtra(struct ViewPort* viewport, struct ViewPortExtra** extra,
    struct GfxBase* graphics)
{
    struct ColorMap* map = viewport->ColorMap;

    if (!*extra) {
        struct ViewPortExtra* allocated = GfxNew(VIEWPORT_EXTRA_TYPE);

        if (!allocated) {
            return MVP_NO_VPE;
        }

        allocated->Flags = VPXF_FREE_ME;
        *extra = allocated;

        if (map && map->Type) {
            map->cm_vpe = allocated;
        }
    }

    return MVP_OK;
}

ULONG pcReleaseViewPortExtra(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"))
{
    (void)view;

    if (extra && (extra->Flags & VPXF_FREE_ME)) {
        struct ColorMap* map = viewport->ColorMap;

        // Use the current map, even if a driver changed it or its association.
        // This ownership flag also applies to extras supplied by the caller.
        if (map && map->Type) {
            map->cm_vpe = NULL;
        }

        GfxFree(extra);
    }

    return MVP_OK;
}

struct ViewFieldSelection {
    struct ViewDisplaySnapshot* display[2];
    UWORD* streams[2];
    unsigned source_field[2];
    BOOL populated;
};

static BOOL selectViewFields(struct ViewFieldSelection* selection, struct View* view, struct GfxBase* graphics)
{
    selection->populated = view && view->LOFCprList;

    if (selection->populated) {
        selection->display[0] =
            pcRetainMergedViewField(pcMergedViews(graphics), view->LOFCprList, &selection->source_field[0]);

        if (!selection->display[0]) {
            // A monitor can supply its own public lists without a native
            // render snapshot. Keep LoadView's state and callback lifecycle
            // independent of whether the software renderer owns those lists.
            selection->source_field[0] = 0;
        }

        selection->streams[0] = view->LOFCprList->start;

        if (view->SHFCprList) {
            selection->display[1] =
                pcRetainMergedViewField(pcMergedViews(graphics), view->SHFCprList, &selection->source_field[1]);

            if (!selection->display[1]) {
                selection->source_field[1] = 0;
            }

            selection->streams[1] = view->SHFCprList->start;

            if ((graphics->system_bplcon0 & 8) && (graphics->DisplayFlags & LPEN_SWAP_FRAMES)) {
                struct ViewDisplaySnapshot* field = selection->display[0];
                selection->display[0] = selection->display[1];
                selection->display[1] = field;
                unsigned source = selection->source_field[0];
                selection->source_field[0] = selection->source_field[1];
                selection->source_field[1] = source;
                UWORD* stream = selection->streams[0];
                selection->streams[0] = selection->streams[1];
                selection->streams[1] = stream;
            }

            return TRUE;
        }
    } else {
        selection->display[0] = NULL;
        selection->source_field[0] = 0;
        selection->streams[0] = graphics->copinit->wait_forever;
    }

    selection->display[1] = pcActiveViewFields(graphics)->display[1];
    selection->source_field[1] = pcActiveViewFields(graphics)->source_field[1];
    pcRetainViewDisplay(selection->display[1]);
    selection->streams[1] = graphics->SHFlist;

    return TRUE;
}

static void installViewFields(const struct ViewFieldSelection* selection, struct View* view, struct GfxBase* graphics)
{
    graphics->Modes = selection->populated ? view->Modes : 0;
    graphics->LOFlist = selection->streams[0];
    graphics->SHFlist = selection->streams[1];
}

static void commitViewFields(const struct ViewFieldSelection* selection, struct GfxBase* graphics)
{
    struct ActiveViewFields* active = pcActiveViewFields(graphics);

    // Both new references are reserved before any old field is released or
    // monitor callback is made. KillView may retire the outgoing native owner
    // after public fields change, without consuming this pending selection.
    for (unsigned field = 0; field < 2; ++field) {
        struct ViewDisplaySnapshot* previous = active->display[field];
        active->display[field] = selection->display[field];
        active->source_field[field] = selection->source_field[field];
        pcReleaseViewDisplay(previous, graphics);
    }

    active->enabled = selection->populated;
    active->owned = TRUE;
    ++active->generation;
}

BOOL pcSelectViewFields(struct View* view, struct GfxBase* graphics)
{
    struct ViewFieldSelection selection;

    if (!selectViewFields(&selection, view, graphics)) {
        return FALSE;
    }

    graphics->ActiView = view;
    installViewFields(&selection, view, graphics);
    commitViewFields(&selection, graphics);

    return TRUE;
}

static void killMonitorView(struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    if (!monitor || !monitor->ms_KillView) {
        return;
    }

    register struct MonitorSpec* argument asm("a0") = monitor;
    register APTR entry asm("a1") = (APTR)monitor->ms_KillView;
    register struct GfxBase* base asm("a6") = graphics;

    __asm__ volatile(
        "jsr (%1)"
        : "+r"(argument), "+r"(entry)
        : "r"(base)
        : "d0", "d1", "cc", "memory");
}

static void setupMonitorView(struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    // SpecialMonitor.do_monitor is a stack-argument callback. Keep the
    // original A0 method, A1 SpecialMonitor and A6 graphics context as well.
    register APTR entry asm("a0") = (APTR)monitor->ms_Special->do_monitor;
    register struct SpecialMonitor* special asm("a1") = monitor->ms_Special;
    register struct GfxBase* base asm("a6") = graphics;

    __asm__ volatile(
        "move.l %3,-(sp)\n\t"
        "jsr (%0)\n\t"
        "addq.l #4,sp"
        : "+r"(entry), "+r"(special)
        : "r"(base), "r"(monitor)
        : "d0", "d1", "cc", "memory");
}

BOOL pcLoadViewState(struct View* view, struct GfxBase* graphics)
{
    struct MonitorSpec* previous = graphics->current_monitor;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    struct ViewFieldSelection selection;

    if (!selectViewFields(&selection, view, graphics)) {
        ReleaseSemaphore(graphics->ActiViewCprSemaphore);
        return FALSE;
    }

    BOOL reset_sprites = view && (graphics->Modes & LACE) && !(view->Modes & LACE);
    ++graphics->SpecialCounter;
    graphics->ActiView = view;

    if (selection.populated) {
        graphics->Modes = view->Modes;
        graphics->LOFlist = view->LOFCprList->start;
        struct MonitorSpec* monitor = graphics->natural_monitor;
        UWORD top = 0xc00;

        if (view->Modes & EXTEND_VSTRUCT) {
            struct ViewExtra* extra = GfxLookUp(view);

            if (extra) {
                if (extra->Monitor) {
                    monitor = extra->Monitor;
                }

                if (graphics->ChipRevBits0 & GFXF_AA_LISA) {
                    top = extra->TopLine;
                }
            }
        }

        graphics->TopLine = top;
        installViewFields(&selection, view, graphics);
        graphics->current_monitor = monitor;
        pcUpdateNativeViewBackground(monitor, graphics);
        UWORD rows = monitor->ms_Flags ? monitor->total_rows : 0;
        UWORD clocks = monitor->ms_Flags ? monitor->total_colorclocks : 0;
        BOOL changed = monitor != previous || rows != graphics->current_tot_rows ||
            clocks != graphics->current_tot_cclks;

        if (changed) {
            if (rows || clocks) {
                if (monitor->ms_Flags & MSF_REQUEST_SPECIAL) {
                    if (monitor->ms_Special && monitor->ms_Special->do_monitor) {
                        setupMonitorView(monitor, graphics);
                        reset_sprites = TRUE;
                    }
#if GRAPHICS_CHIPSET
                    else if (pcUsesChipset(graphics)) {
                        // A special monitor without a method does not retain
                        // the outgoing monitor's beam control on AmigaOS.
                        pcSetChipsetBeamControl(0);
                    }
#endif
                }
#if GRAPHICS_CHIPSET
                else if (pcUsesChipset(graphics)) {
                    // Reloads of the same monitor with unchanged totals do
                    // not reapply beam control in the original library.
                    pcSetChipsetBeamControl(monitor->BeamCon0);
                }
#endif

                // The callback can update monitor geometry. The original
                // copies those live values, but saves pre-callback totals.
                graphics->MaxDisplayRow = monitor->total_rows - 1;
                graphics->MaxDisplayColumn = monitor->DeniseMaxDisplayColumn;
                graphics->MinDisplayColumn = monitor->DeniseMinDisplayColumn;
            } else {
                // These are the original logical display defaults, not PC
                // firmware timing. This branch leaves MaxDisplayColumn at
                // the standard minimum and does not change MinDisplayColumn.
                graphics->MaxDisplayRow = (graphics->DisplayFlags & PAL ? STANDARD_PAL_ROWS : STANDARD_NTSC_ROWS) - 1;
                graphics->MaxDisplayColumn = STANDARD_DENISE_MIN;
            }

            killMonitorView(previous, graphics);
        }

        graphics->current_tot_rows = rows;
        graphics->current_tot_cclks = clocks;
        graphics->MonitorFlags = monitor->ms_Flags;
    } else {
        // Blank LoadView calls the outgoing owner before clearing monitor
        // and field metadata, even when ActiView is a non-null empty View.
        killMonitorView(graphics->current_monitor, graphics);
        graphics->MonitorFlags = 0;
        graphics->current_monitor = NULL;
        graphics->hedley_flags = 0;
        installViewFields(&selection, view, graphics);
    }

    if (reset_sprites) {
        graphics->Bugs |= 0x10;
#if GRAPHICS_CHIPSET
        if (pcUsesChipset(graphics)) {
            // Restart blanking before the frame handler applies the newly
            // selected monitor or field mode, as the original LoadView does.
            pcResetChipsetBlanking();
        }
#endif
    }

    commitViewFields(&selection, graphics);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return TRUE;
}

struct ViewDisplaySnapshot* pcRetainActiveViewField(BOOL short_field, struct GfxBase* graphics)
{
    struct ActiveViewFields* active = pcActiveViewFields(graphics);
    struct ViewDisplaySnapshot* display = active->enabled ? active->display[!!short_field] : NULL;
    pcRetainViewDisplay(display);

    return display;
}

void pcClearActiveViewFields(struct GfxBase* graphics)
{
    struct ActiveViewFields* active = pcActiveViewFields(graphics);
    active->enabled = FALSE;
    active->owned = FALSE;
    ++active->generation;

    for (unsigned field = 0; field < 2; ++field) {
        struct ViewDisplaySnapshot* previous = active->display[field];
        active->display[field] = NULL;
        pcReleaseViewDisplay(previous, graphics);
    }
}

BOOL pcPresentActiveViewField(struct ViewFieldComposition* composition,
    struct GraphicsFramebuffer* output, const struct GraphicsFramebufferScaleArea* area,
    BOOL short_field, UWORD last_horizontal, struct ViewFieldControls* controls,
    unsigned* next_field, struct GfxBase* graphics)
{
    if (!composition || !output || !area || !controls || !next_field || !graphics) {
        return FALSE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    const struct ActiveViewFields* active = pcActiveViewFields(graphics);
    unsigned slot = !!short_field;
    const struct ViewDisplaySnapshot* display = active->enabled ? active->display[slot] : NULL;
    unsigned next;
    BOOL success = display && pcComposeNativeViewField(composition, display,
        active->source_field[slot], last_horizontal, controls, &next);

    if (success) {
        if (graphics->Modes & LACE) {
            success = pcScaleFramebufferField(output, composition->framebuffer, area, short_field);
        } else {
            success = pcScaleFramebufferArea(output, composition->framebuffer, area);
        }
    }

    if (success) {
        // Keep ownership until the last visible byte is written. In particular,
        // KillView cannot return to a new monitor while this copy is pending.
        success = pcPresentGraphicsFramebuffer(output);
    }

    if (success) {
        *next_field = next;
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return success;
}

LONG pcKillNativeView(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"))
{
    (void)monitor;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    pcClearActiveViewFields(graphics);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return 0;
}
