/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/backend.h"
#include "internal/library.h"
#if GRAPHICS_CHIPSET
#include "internal/chipset.h"
#endif

#define __NOLIBBASE__
#include <proto/exec.h>

#if GRAPHICS_CHIPSET
BOOL pcSelectGraphicsBackend(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;

#if GRAPHICS_FRAMEBUFFER
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    library->chipset_selected = OpenResource("devicetree.resource") == NULL;
#else
    library->chipset_selected = TRUE;
#endif

    if (library->chipset_selected) {
        pcInitChipsetRevision(graphics);
    }

    return TRUE;
}

#endif
