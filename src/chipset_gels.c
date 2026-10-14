/* SPDX-License-Identifier: MPL-2.0 */
// Native GEL transfers. Queue callbacks program real Amiga DMA; allocation,
// clipping and request retirement remain in the calling task.
#define ECS_SPECIFIC
#include "internal/chipset_gels.h"
#include "internal/monitor.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#include <hardware/blit.h>
#include <hardware/custom.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

struct GelTransfer {
    struct bltnode node;
    struct GelTransfer* next;
    struct Task* task;
    volatile BOOL complete;
    UBYTE plane;
    UBYTE depth;
    UBYTE mask;
    UWORD first_mask;
    UWORD last_mask;
    UWORD control[8];
    UWORD control1;
    WORD source_modulo;
    WORD dest_modulo;
    UWORD words;
    UWORD height;
    const UBYTE* source[8];
    UBYTE* dest[8];
    const UBYTE* shadow;
    UBYTE* allocated_shadow;
    ULONG shadow_size;
};

static ULONG __attribute__((used)) transferPlane(volatile struct Custom* custom asm("a0"),
    struct GelTransfer* transfer asm("a1"))
{
    unsigned plane = transfer->plane++;

    if (transfer->mask & (1U << plane)) {
        custom->bltcon0 = transfer->control[plane];
        custom->bltcon1 = transfer->control1;
        custom->bltafwm = transfer->first_mask;
        custom->bltalwm = transfer->last_mask;
        custom->bltamod = transfer->source_modulo;
        custom->bltbmod = transfer->source_modulo;
        custom->bltcmod = transfer->dest_modulo;
        custom->bltdmod = transfer->dest_modulo;
        custom->bltapt = (APTR)transfer->shadow;
        custom->bltbpt = (APTR)transfer->source[plane];
        custom->bltcpt = (APTR)transfer->dest[plane];
        custom->bltdpt = (APTR)transfer->dest[plane];
        custom->bltadat = 0xffff;

        if (custom->vposr & 0x2000) {
            custom->bltsizv = transfer->height & 0x7fff;
            custom->bltsizh = transfer->words & 0x7ff;
        } else {
            custom->bltsize = (transfer->height << 6) | transfer->words;
        }
    }

    return transfer->depth - transfer->plane;
}

// The queue continuation result is Z, independently of the compiler's return
// sequence. Keep the callback convention at this small assembly boundary.
void gelTransfer(void);
__asm__(
    ".text\n"
    "_gelTransfer:\n"
    "jsr _transferPlane\n"
    "tst.l d0\n"
    "rts\n"
);

static void finishTransfer(struct GelTransfer* transfer asm("a1"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    transfer->complete = TRUE;
    Signal(transfer->task, SIGF_BLIT);
}

WORD pcGelBeamLine(const struct VSprite* sprite, const struct RastPort* port,
    struct ViewPort* viewport, BOOL restore, struct GfxBase* graphics)
{
    WORD line = (UWORD)(restore ? sprite->OldY : sprite->Y) + (UWORD)sprite->Height;

    if (port->Layer) {
        line = (UWORD)line + (UWORD)port->Layer->bounds.MinY - (UWORD)port->Layer->Scroll_Y;
    }

    if (graphics->ActiView) {
        ULONG modes = pcViewportModes(viewport);
        WORD offset = graphics->ActiView->DyOffset;

        if (modes & DOUBLESCAN) {
            line = (UWORD)line * 2;
        }

        line = (UWORD)line + (UWORD)offset + (UWORD)viewport->DyOffset - (UWORD)viewport->RasInfo->RyOffset;

        if (modes & LACE) {
            line = (WORD)((UWORD)line + (UWORD)offset) >> 1;
        }
    } else {
        line = 0;
    }

    return line < 12 ? 12 : line;
}

static struct GelTransfer* prepareTransfer(const struct BitMap* source, struct BitMap* dest,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height,
    UBYTE mask, BOOL overlay, const UBYTE* shadow, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    if (width <= 0 || height <= 0 || !dest->Depth) {
        return NULL;
    }

    struct GelTransfer* transfer = AllocMem(sizeof(*transfer), MEMF_PUBLIC | MEMF_CLEAR);

    if (!transfer) {
        return NULL;
    }

    WORD source_phase = source_x & 15;
    WORD dest_phase = dest_x & 15;
    WORD shift = dest_phase - source_phase;
    UWORD source_words = (source_phase + (UWORD)width + 15) >> 4;
    UWORD dest_words = (dest_phase + (UWORD)width + 15) >> 4;
    UWORD end = (source_phase + (UWORD)width) & 15;
    UWORD source_first = 0xffffU >> source_phase;
    UWORD source_last = end ? (0xffffUL << (16 - end)) : 0xffff;
    LONG source_offset = (ULONG)(UWORD)source_y * source->BytesPerRow + ((UWORD)source_x >> 4) * 2;
    LONG dest_offset = (ULONG)(UWORD)dest_y * dest->BytesPerRow + ((UWORD)dest_x >> 4) * 2;
    UWORD a_shift;

    // Walk upward through rows for a right shift, downward for a left
    // shift. Negative modulos retain each bitmap's independent row pitch.
    if (shift < 0) {
        transfer->words = source_words;
        transfer->first_mask = source_last;
        transfer->last_mask = source_first;
        transfer->control1 = ((UWORD)-shift << 12) | 2;
        a_shift = (UWORD)-shift << 12;
        source_offset += (source_words - 1) * 2;
        dest_offset += (source_words - 1) * 2;
    } else {
        transfer->words = dest_words;
        transfer->control1 = (UWORD)shift << 12;
        source_offset += (ULONG)(height - 1) * source->BytesPerRow;
        dest_offset += (ULONG)(height - 1) * dest->BytesPerRow;
        a_shift = overlay ? (UWORD)shift << 12 : 0;

        if (overlay) {
            transfer->first_mask = source_first;
            transfer->last_mask = dest_words > source_words ? 0 : source_last;

            // When shifting adds a destination word, BLTALWM masks that extra
            // word. Clip the preceding shadow word separately before DMA.
            if (dest_words > source_words && end) {
                transfer->shadow_size = (ULONG)source->Rows * source->BytesPerRow;
                transfer->allocated_shadow = AllocMem(transfer->shadow_size, MEMF_CHIP);

                if (!transfer->allocated_shadow) {
                    FreeMem(transfer, sizeof(*transfer));
                    return NULL;
                }

                CopyMem((APTR)shadow, transfer->allocated_shadow, transfer->shadow_size);
                UWORD* row = (APTR)(transfer->allocated_shadow +
                    (ULONG)(UWORD)source_y * source->BytesPerRow +
                    (((UWORD)source_x >> 4) + source_words - 1) * 2);

                for (WORD y = 0; y < height; ++y) {
                    *row &= source_last;
                    row = (APTR)((UBYTE*)row + source->BytesPerRow);
                }

                shadow = transfer->allocated_shadow;
            }
        } else {
            end = (dest_phase + (UWORD)width) & 15;
            transfer->first_mask = 0xffffU >> dest_phase;
            transfer->last_mask = end ? (0xffffUL << (16 - end)) : 0xffff;
        }
    }

    transfer->source_modulo = -(source->BytesPerRow + transfer->words * 2);
    transfer->dest_modulo = -(dest->BytesPerRow + transfer->words * 2);
    transfer->height = height;
    transfer->depth = dest->Depth;
    transfer->mask = mask;
    transfer->task = SysBase->ThisTask;
    transfer->shadow = shadow ? shadow + source_offset : NULL;
    transfer->node.function = (APTR)gelTransfer;
    transfer->node.cleanup = (APTR)finishTransfer;
    transfer->node.stat = CLEANUP;

    for (unsigned plane = 0; plane < dest->Depth; ++plane) {
        if (mask & (1U << plane)) {
            transfer->source[plane] = source->Planes[plane] + source_offset;
            transfer->dest[plane] = dest->Planes[plane] + dest_offset;
        }

        transfer->control[plane] = a_shift | (overlay ? 0xfca : 0x7ca);
    }

    return transfer;
}

static void submitTransfer(struct GelsInfo* info, struct GelTransfer* transfer, WORD beam,
    struct GfxBase* graphics)
{
    if (!transfer) {
        return;
    }

    if (info->lastBlissObj) {
        ((struct GelTransfer*)info->lastBlissObj)->next = transfer;
    } else {
        info->firstBlissObj = (APTR)transfer;
    }

    info->lastBlissObj = (APTR)transfer;
    transfer->node.beamsync = beam;

    if (info->Flags & DBUFFER) {
        QBlit(&transfer->node);
    } else {
        QBSBlit(&transfer->node);
    }
}

void pcQueueGelCopy(struct GelsInfo* info, const struct BitMap* source, struct BitMap* dest,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height,
    UBYTE mask, WORD beam, struct GfxBase* graphics)
{
    struct GelTransfer* transfer = prepareTransfer(source, dest, source_x, source_y,
        dest_x, dest_y, width, height, mask, FALSE, NULL, graphics);
    submitTransfer(info, transfer, beam, graphics);
}

void pcQueueGelDraw(struct GelsInfo* info, const struct VSprite* sprite, struct BitMap* dest,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height,
    UBYTE mask, WORD beam, struct GfxBase* graphics)
{
    struct BitMap source = {0};
    source.BytesPerRow = (UWORD)sprite->Width * 2;
    source.Rows = sprite->Height;
    source.Depth = dest->Depth;
    const UBYTE* image = (APTR)sprite->ImageData;
    ULONG plane_size = (ULONG)source.BytesPerRow * source.Rows;

    for (unsigned plane = 0; plane < dest->Depth; ++plane) {
        source.Planes[plane] = (PLANEPTR)image;

        if ((mask & (1U << plane)) && ((UBYTE)sprite->PlanePick & (1U << plane))) {
            image += plane_size;
        }
    }

    struct GelTransfer* transfer = prepareTransfer(&source, dest, source_x, source_y,
        dest_x, dest_y, width, height, mask, sprite->Flags & OVERLAY,
        (APTR)sprite->VSBob->ImageShadow, graphics);

    if (transfer) {
        for (unsigned plane = 0; plane < dest->Depth; ++plane) {
            if (!((UBYTE)sprite->PlanePick & (1U << plane))) {
                transfer->control[plane] = (transfer->control[plane] & 0xf000) |
                    ((UBYTE)sprite->PlaneOnOff & (1U << plane) ? 0xbfa : 0xb0a);
            }
        }
    }

    submitTransfer(info, transfer, beam, graphics);
}

void pcFinishGelQueue(struct GelsInfo* info, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct GelTransfer* transfer = (APTR)info->firstBlissObj;

    while (transfer) {
        while (!transfer->complete) {
            Wait(SIGF_BLIT);
        }

        struct GelTransfer* next = transfer->next;

        if (transfer->allocated_shadow) {
            FreeMem(transfer->allocated_shadow, transfer->shadow_size);
        }

        FreeMem(transfer, sizeof(*transfer));
        transfer = next;
    }

    info->firstBlissObj = NULL;
    // Like the original, retirement consumes the allocation list. The last
    // submitted-node field is reset at the next DrawGList/RemIBob entry.
}
