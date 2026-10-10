/* SPDX-License-Identifier: MPL-2.0 */
// Fixed firmware mode output through public 68k resources. No mode setting or
// GPU register programming belongs to this simple framebuffer backend.
#include "internal/framebuffer.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include "internal/framebuffer_tree.h"
#define __NOLIBBASE__
#include <proto/devicetree.h>
#include <proto/exec.h>

BOOL pcOpenGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase)
{
    if (!framebuffer || framebuffer->pixels) {
        return FALSE;
    }

    APTR DeviceTreeBase = OpenResource("devicetree.resource");

    if (!DeviceTreeBase) {
        return FALSE;
    }

    APTR chosen = DT_OpenKey((CONST_STRPTR)"/chosen");

    if (!chosen) {
        return FALSE;
    }

    struct FramebufferMode mode;
    APTR key = pcGraphicsFindCompatible(DeviceTreeBase, chosen, NULL, "simple-framebuffer");
    BOOL valid = key && pcReadGraphicsFramebuffer(DeviceTreeBase, key, &mode) &&
        !pcGraphicsFindCompatible(DeviceTreeBase, chosen, key, "simple-framebuffer");

    DT_CloseKey(chosen);

    if (!valid) {
        return FALSE;
    }

    // pcReadGraphicsFramebuffer checked the firmware pitch and complete interval. The
    // staging image needs only visible pixels, without firmware row padding.
    ULONG stride = mode.width * mode.pixel_bytes;
    ULONG size = stride * mode.height;
    UBYTE* pixels = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!pixels) {
        return FALSE;
    }

    framebuffer->mode = mode;
    framebuffer->pixels = pixels;
    framebuffer->stride = stride;
    framebuffer->size = size;

    return TRUE;
}

BOOL pcCloseGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase)
{
    if (!framebuffer) {
        return FALSE;
    }

    if (framebuffer->pixels) {
        FreeMem(framebuffer->pixels, framebuffer->size);
    }

    *framebuffer = (struct GraphicsFramebuffer){0};

    return TRUE;
}

BOOL pcPresentGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer)
{
    if (!framebuffer || !framebuffer->pixels) {
        return FALSE;
    }

    const UBYTE* source = framebuffer->pixels;
    volatile UBYTE* destination = (APTR)framebuffer->mode.address;

    for (ULONG y = 0; y < framebuffer->mode.height; ++y) {
        for (ULONG x = 0; x < framebuffer->stride; ++x) {
            destination[x] = source[x];
        }

        if (y + 1 < framebuffer->mode.height) {
            source += framebuffer->stride;
            destination += framebuffer->mode.stride;
        }
    }

    return TRUE;
}
