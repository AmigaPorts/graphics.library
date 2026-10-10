/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/view.h"
#include "internal/copper.h"
#include "internal/display.h"
#include "internal/monitor.h"

#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

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

static ULONG normalizeDisplayID(ULONG id)
{
    if (!(id & EXTENDED_MODE)) {
        id &= 0xffff9efdUL;
    } else if ((id & MONITOR_ID_MASK) == EXTENDED_MODE) {
        id &= ~EXTENDED_MODE;
    }

    return id;
}

BOOL pcGetViewPortBuildInfo(struct View* view, struct ViewPort* viewport, struct VecInfo* info,
    struct GfxBase* graphics)
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
                // MakeVPort reads the original mode flags, not DisplayFlags.
                id |= graphics->Flags & PAL ? PAL_MONITOR_ID : NTSC_MONITOR_ID;
            }
        }
    }

    // A public query owns resolution and can be supplied by another driver.
    // Keep the original nonzero-count rule, including a partial VecInfo reply.
    return GetDisplayInfoData(NULL, (APTR)info, sizeof(*info), DTAG_VEC, id) != 0;
}

UWORD pcBuildViewPort(struct View* view, struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics)
{
    struct ColorMap* map = viewport->ColorMap;
    struct ViewPortExtra* extra = map && map->Type ? map->cm_vpe : NULL;
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

    const APTR* methods = ((const APTR*)vectors)[3];
    ViewPortBuildInitFunction initialize = (ViewPortBuildInitFunction)*methods++;
    UWORD work[41];
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
        pcFreeCopList(viewport->DspIns);
        viewport->DspIns = NULL;
        result = MVP_OK;
    }

    return result;
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
