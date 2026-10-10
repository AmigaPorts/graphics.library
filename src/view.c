/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/view.h"
#include "internal/copper.h"

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

UWORD pcBuildViewPort(struct View* view, struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics)
{
    struct ColorMap* map = viewport->ColorMap;
    struct ViewPortExtra* extra = map && map->Type ? map->cm_vpe : NULL;
    APTR vectors = NULL;

    // Dual playfields use the queried driver even when cm_vpe has a table.
    // A single playfield gives an already installed viewport owner precedence.
    if (!(viewport->Modes & DUALPF) && extra) {
        vectors = extra->VecTable;
    }

    if (!vectors) {
        vectors = info->Vec;
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
