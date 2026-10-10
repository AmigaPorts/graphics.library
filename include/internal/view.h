/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/view.h>

typedef LONG (*MonitorViewFunction)(struct View* view);

// Shared monitor dispatch for the display owner. The required default method
// implements native merge/load behavior; it is not a no-op fallback. These
// dispatchers alone do not implement or publish the public display vectors.
LONG pcDispatchLoadView(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics);
LONG pcDispatchMrgCop(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics);

// Driver table slot 3 points to a NULL-terminated preparation sequence. The
// first method takes these five stack arguments; subsequent methods take the
// first two plus the resolved ViewPortExtra. A6 remains the graphics base.
typedef ULONG (*ViewPortBuildInitFunction)(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra** extra, APTR data, APTR work, struct GfxBase* graphics asm("a6"));
typedef ULONG (*ViewPortBuildFunction)(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));

// Execute preparation after the caller has resolved a valid DTAG_VEC query.
// Default single/dual tables must contain actual backend methods. This does
// not resolve display IDs, publish MakeVPort or activate the resulting View.
UWORD pcBuildViewPort(struct View* view, struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics);
