/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "color.h"

#include <graphics/gfxbase.h>
#include <graphics/videocontrol.h>

// Results for the display owner, after the ordered ColorMap operations finish.
// An immediate update requires applying these changes before returning through
// the public vector. This state processor alone is not that public entry.
struct VideoControlChanges {
    ULONG* immediate;
    BOOL remake;
    BOOL palette;
    BOOL border;
    BOOL blanking;
};

// Mode binding queries the public display database under ActiViewCprSemaphore.
// The display owner supplies its default renderer table for records without
// their own VecInfo.Vec. This does not install or construct that renderer.
ULONG pcVideoControlTags(struct ColorMap* map, struct TagItem* tags, struct VideoControlChanges* changes,
    APTR default_vectors, struct GfxBase* graphics);

// Update the existing display-list data and call the viewport's palette method.
// The caller holds ActiViewCprSemaphore and must still refresh the active native
// display before releasing it. default_palette is the native renderer's method,
// required when the viewport has no driver table; there is no no-op fallback.
void pcVideoControlLists(struct ColorMap* map, const struct VideoControlChanges* changes,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics);
