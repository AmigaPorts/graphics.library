/* SPDX-License-Identifier: MPL-2.0 */
// GEL display-list construction. Destinations below are words in the public
// RAM-list format consumed by the display backend, never MMIO dereferences.
#include "internal/gels.h"
#include "internal/copper.h"
#include "internal/monitor.h"
#include "internal/backend.h"
#if GRAPHICS_CHIPSET
#include "internal/chipset_gels.h"
#endif

#include <exec/execbase.h>
#include <graphics/clip.h>
#include <graphics/gfxbase.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

static struct CopIns* emitInstruction(struct CopList** list, struct CopIns* instruction,
    UWORD opcode, UWORD first, UWORD second)
{
    instruction->OpCode = opcode;
    instruction->DESTADDR = first;
    instruction->DESTDATA = second;

    return pcBumpCopList(list, instruction + 1);
}

static void drawSprite(struct VSprite* sprite, struct RastPort* port, struct ViewPort* viewport,
    struct CopList** sprite_list, struct CopList** color_list, struct GfxBase* graphics)
{
    WORD x = sprite->X < 0 ? 0 : sprite->X;
    WORD y = sprite->Y < 0 ? 0 : sprite->Y;
    WORD source_x = (UWORD)x - (UWORD)sprite->X;
    WORD source_y = (UWORD)y - (UWORD)sprite->Y;
    WORD width = (UWORD)sprite->Width * 16 - (UWORD)source_x;
    WORD height = (UWORD)sprite->Height - (UWORD)source_y;
    LONG bitmap_height = port->BitMap->Rows;
    LONG bitmap_width = GetBitMapAttr(port->BitMap, BMA_WIDTH);

    if ((LONG)x + width > bitmap_width) {
        width = bitmap_width - x;
    }

    if ((LONG)y + height > bitmap_height) {
        height = bitmap_height - y;
    }

    if (width <= 0 || height <= 0) {
        sprite->Flags |= GELGONE;
        return;
    }

    sprite->Flags &= ~GELGONE;
    struct GelsInfo* info = port->GelsInfo;
    WORD previous_lines[8];

    for (unsigned i = 0; i < 8; ++i) {
        previous_lines[i] = info->nextLine[i];
    }

    WORD selected = pcGelAssignSprite(sprite, info, y, height, graphics->SpriteReserved);

    if (selected < 0) {
        return;
    }

    ULONG image = (ULONG)sprite->ImageData + (ULONG)(UWORD)sprite->Depth * (UWORD)source_y * 2;
    UWORD pointer = 0xf120 + selected * 4;
    struct CopIns* cursor = (*sprite_list)->CopPtr;
    cursor = emitInstruction(sprite_list, cursor, COPPER_WAIT, previous_lines[selected], 0);
    cursor = emitInstruction(sprite_list, cursor, COPPER_MOVE, pointer, image >> 16);
    cursor = emitInstruction(sprite_list, cursor, COPPER_MOVE, pointer + 2, image);

    ULONG modes = pcViewportModes(viewport);
    WORD horizontal = sprite->X;

    if (modes & SUPERHIRES) {
        horizontal >>= 2;
    } else if (modes & HIRES) {
        horizontal >>= 1;
    }

    horizontal = (UWORD)horizontal + (UWORD)graphics->ActiView->DxOffset;
    WORD top = (UWORD)y + (UWORD)graphics->ActiView->DyOffset;
    WORD bottom = (UWORD)top + (UWORD)height;
    UWORD position = ((UWORD)top << 8) | (((WORD)(horizontal >> 1)) & 0xff);
    UWORD control = ((UWORD)bottom << 8) | ((top >> 6) & 4) |
        ((bottom >> 7) & 2) | (horizontal & 1);
    UWORD destination = 0xf140 + selected * 8;
    cursor = emitInstruction(sprite_list, cursor, COPPER_MOVE, destination, position);
    cursor = emitInstruction(sprite_list, cursor, COPPER_MOVE, destination + 2, control);
    (*sprite_list)->CopPtr = cursor;

    if (sprite->SprColors) {
        cursor = (*color_list)->CopPtr;
        cursor = emitInstruction(color_list, cursor, COPPER_WAIT, (UWORD)y - 1, 0);
        UWORD color = 0xf180 + ((selected & 6) * 2 + 17) * 2;

        for (unsigned i = 0; i < 3; ++i) {
            cursor = emitInstruction(color_list, cursor, COPPER_MOVE, color + i * 2, sprite->SprColors[i]);
        }

        (*color_list)->CopPtr = cursor;
    }
}

void pcDrawGList(struct RastPort* port asm("a1"), struct ViewPort* viewport asm("a0"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Layer* layer = port->Layer;

    if (layer) {
        ObtainSemaphore(&layer->Lock);
    }

    struct GelsInfo* info = port->GelsInfo;
    info->firstBlissObj = NULL;
    info->lastBlissObj = NULL;
    struct CopList* sprites = pcInitCopList(viewport->SprIns, 32);

    if (!sprites || !sprites->MaxCount) {
        goto done;
    }

    struct CopList* colors = pcInitCopList(viewport->ClrIns, 32);

    if (!colors || !colors->MaxCount) {
        goto done;
    }

    for (unsigned i = 0; i < 8; ++i) {
        if (info->nextLine) {
            info->nextLine[i] = -1;
        }

        if (info->lastColor) {
            info->lastColor[i] = NULL;
        }
    }

    graphics->VBlank = 1;
    struct CopList* current_sprite = sprites;
    struct CopList* current_color = colors;
    struct VSprite* last = info->gelHead;

    for (struct VSprite* sprite = last->NextVSprite; sprite != info->gelTail; sprite = sprite->NextVSprite) {
        if (sprite->Flags & VSPRITE) {
            drawSprite(sprite, port, viewport, &current_sprite, &current_color, graphics);
        } else {
            pcGelProcessBob(sprite, port, &last, viewport, graphics);
        }
    }

    pcGelFinishFrame(port, last, viewport, graphics);
    current_sprite->CopPtr->OpCode = COPPER_WAIT;
    current_sprite->CopPtr->VWAITPOS = 10000;
    current_sprite->CopPtr->HWAITPOS = 255;
    current_color->CopPtr->OpCode = COPPER_WAIT;
    current_color->CopPtr->VWAITPOS = 10000;
    current_color->CopPtr->HWAITPOS = 255;
    viewport->SprIns = sprites;
    viewport->ClrIns = colors;

done:
#if GRAPHICS_CHIPSET
    if (pcUsesChipset(graphics)) {
        pcFinishGelQueue(info, graphics);
    }
#endif

    if (layer) {
        ReleaseSemaphore(&layer->Lock);
    }
}
