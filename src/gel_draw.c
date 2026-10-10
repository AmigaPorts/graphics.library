/* SPDX-License-Identifier: MPL-2.0 */
// Independent CPU drawing and background restoration for public graphics elements.
#include "internal/gels.h"
#include "internal/bitmap.h"

#include <graphics/clip.h>
#include <graphics/gfxbase.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/graphics.h>

static void restoreBitmap(const struct VSprite* sprite, const WORD* buffer, struct BitMap* bitmap,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height, UBYTE mask)
{
    struct BitMap source = {0};
    source.BytesPerRow = (UWORD)sprite->Width * 2;
    source.Rows = sprite->Height;
    source.Depth = bitmap->Depth;
    ULONG plane_size = (ULONG)(UWORD)sprite->Width * (UWORD)sprite->Height;
    // Restoration consumes consecutive saved planes for the selected output
    // planes. PlanePick/PlaneOnOff describe drawing, not background restoration.
    for (unsigned plane = 0; plane < source.Depth; ++plane) {
        if (mask & (1U << plane)) {
            source.Planes[plane] = (PLANEPTR)buffer;
            buffer += plane_size;
        }
    }
    // GEL blits are internal operations in the original library as well.
    pcBltBitMap(&source, source_x, source_y, bitmap, dest_x, dest_y, width, height, 0xc0, mask, NULL);
}

static void restoreClips(const struct VSprite* sprite, const WORD* buffer, const struct RastPort* port,
    struct ClipRect* clips, struct BitMap* bitmap, BOOL super_bitmap,
    WORD source_x, WORD source_y, WORD dest_x, WORD dest_y, WORD width, WORD height)
{
    WORD right = dest_x + width - 1, bottom = dest_y + height - 1;
    for (const struct ClipRect* clip = clips; clip; clip = clip->Next) {
        WORD left = dest_x > clip->bounds.MinX ? dest_x : clip->bounds.MinX;
        WORD top = dest_y > clip->bounds.MinY ? dest_y : clip->bounds.MinY;
        WORD last_x = right < clip->bounds.MaxX ? right : clip->bounds.MaxX;
        WORD last_y = bottom < clip->bounds.MaxY ? bottom : clip->bounds.MaxY;
        if (left > last_x || top > last_y) { continue; }
        WORD x = left, y = top;
        struct BitMap* target = bitmap;
        if (!super_bitmap && clip->obscured) {
            target = clip->BitMap;
            if (!target) { continue; }
            x -= clip->bounds.MinX & ~15;
            y -= clip->bounds.MinY;
        }
        if (!target) { continue; }
        restoreBitmap(sprite, buffer, target, source_x + left - dest_x, source_y + top - dest_y,
            x, y, last_x - left + 1, last_y - top + 1, port->Mask);
    }
}

static void restoreBackground(struct VSprite* sprite, struct RastPort* port, struct GfxBase* graphics)
{
    WORD bitmap_width = GetBitMapAttr(port->BitMap, BMA_WIDTH);
    struct Bob* bob = sprite->VSBob;
    struct VSprite* tail = port->GelsInfo->gelTail;
    WORD x = sprite->OldX, y = sprite->OldY;
    WORD height = sprite->Height, width = (UWORD)sprite->Width * 16;
    WORD right = x + width - 1, bottom = y + height - 1;
    for (struct VSprite* next = sprite->ClearPath; next != tail; next = next->ClearPath) {
        if (!(next->Flags & BACKSAVED) || x > next->OldX + (LONG)next->Width * 16 - 1 ||
            next->OldX > right || y > next->OldY + (LONG)next->Height - 1 || next->OldY > bottom) { continue; }
        restoreBackground(next, port, graphics);
    }
    if (!(sprite->Flags & BACKSAVED)) { return; }
    sprite->Flags &= ~BACKSAVED;
    if (bob->Flags & SAVEBOB) { return; }
    WORD source_x = x < 0 ? (UWORD)-x : 0;
    WORD source_y = y < 0 ? (UWORD)-y : 0;
    WORD dest_x = x < 0 ? 0 : x, dest_y = y < 0 ? 0 : y;
    width = (UWORD)width - source_x;
    height = (UWORD)height - source_y;
    if ((LONG)dest_x + width > bitmap_width) { width = bitmap_width - dest_x; }
    if ((LONG)dest_y + height > port->BitMap->Rows) { height = port->BitMap->Rows - dest_y; }
    if (width <= 0 || height <= 0) { return; }
    struct Layer* layer = port->Layer;
    if (!layer) {
        restoreBitmap(sprite, bob->SaveBuffer, port->BitMap, source_x, source_y, dest_x, dest_y,
            width, height, port->Mask);
        return;
    }
    port->GelsInfo->Flags = port->Flags & 4;
    restoreClips(sprite, bob->SaveBuffer, port, layer->ClipRect, port->BitMap, FALSE,
        source_x, source_y, dest_x + layer->bounds.MinX - layer->Scroll_X,
        dest_y + layer->bounds.MinY - layer->Scroll_Y, width, height);
    if (layer->SuperBitMap) {
        restoreClips(sprite, bob->SaveBuffer, port, layer->SuperClipRect, layer->SuperBitMap, TRUE,
            source_x, source_y, dest_x, dest_y, width, height);
    }
}

void pcRemIBob(struct Bob* bob asm("a0"), struct RastPort* port asm("a1"), struct ViewPort* viewport asm("a2"),
    struct GfxBase* graphics asm("a6"))
{
    // CPU transfers finish synchronously; there is no chipset blitter queue or
    // beam-position dependency. The viewport is needed only by that scheduling.
    (void)viewport;
    port->GelsInfo->firstBlissObj = NULL;
    port->GelsInfo->lastBlissObj = NULL;
    struct VSprite* sprite = bob->BobVSprite;
    if (sprite->Flags & BACKSAVED) { restoreBackground(sprite, port, graphics); }
    pcRemVSprite(sprite);
    bob->Flags |= BOBNIX;
}

void pcGelSaveBitmap(const struct VSprite* sprite, const struct BitMap* bitmap,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height, UBYTE mask)
{
    struct BitMap source = *bitmap, saved = {0};
    saved.BytesPerRow = (UWORD)sprite->Width * 2;
    saved.Rows = sprite->Height;
    saved.Depth = bitmap->Depth;
    ULONG plane_bytes = (ULONG)(UWORD)sprite->Width * (UWORD)sprite->Height * 2;
    unsigned selected = 0;
    for (unsigned plane = 0; plane < bitmap->Depth; ++plane) {
        if (mask & (1U << plane)) {
            // The original save transfer advances its screen-source index only
            // for selected output planes; saved-plane slots retain their index.
            source.Planes[plane] = bitmap->Planes[selected++];
            saved.Planes[plane] = (PLANEPTR)sprite->VSBob->SaveBuffer + plane * plane_bytes;
        }
    }
    pcBltBitMap(&source, bitmap_x, bitmap_y, &saved, image_x, image_y, width, height, 0xc0, mask, NULL);
}

static UWORD imageBits(const UWORD* row, unsigned x, unsigned count)
{
    unsigned shift = x & 15;
    row += x >> 4;
    UWORD bits = *row << shift;
    if (shift + count > 16) { bits |= row[1] >> (16 - shift); }
    return bits;
}

void pcGelDrawBitmap(const struct VSprite* sprite, struct BitMap* bitmap,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height, UBYTE mask)
{
    ULONG plane_words = (ULONG)(UWORD)sprite->Width * (UWORD)sprite->Height;
    const UWORD* image = (const UWORD*)sprite->ImageData;
    for (unsigned plane = 0; plane < bitmap->Depth; ++plane) {
        if (!(mask & (1U << plane))) { continue; }
        BOOL picked = (UBYTE)sprite->PlanePick & (1U << plane);
        BOOL overlay = !picked || (sprite->Flags & OVERLAY);
        unsigned shadow_x = (UWORD)image_x;
        // Without OVERLAY, the original prepares an unshifted mask for a
        // forward transfer. PlaneOnOff still enables that mask on unpicked
        // planes, so its phase follows the destination word boundary there.
        if (!picked && !(sprite->Flags & OVERLAY) && (bitmap_x & 15) >= (image_x & 15)) {
            shadow_x += (bitmap_x & 15) - (image_x & 15);
        }
        const UWORD* pixels = image;
        if (picked) { image += plane_words; }
        const UWORD* shadow = (const UWORD*)sprite->VSBob->ImageShadow;
        if (picked) { pixels += (ULONG)(UWORD)image_y * (UWORD)sprite->Width; }
        if (overlay) { shadow += (ULONG)(UWORD)image_y * (UWORD)sprite->Width; }
        UBYTE* dest = bitmap->Planes[plane] + (ULONG)(UWORD)bitmap_y * bitmap->BytesPerRow;
        for (WORD y = 0; y < height; ++y) {
            unsigned remaining = (UWORD)width, offset = 0;
            while (remaining) {
                unsigned shift = ((UWORD)bitmap_x + offset) & 15;
                unsigned count = 16 - shift;
                if (count > remaining) { count = remaining; }
                UWORD active = (0xffffUL >> (16 - count)) << (16 - shift - count);
                if (overlay) { active &= imageBits(shadow, shadow_x + offset, count) >> shift; }
                UWORD bits = picked ? imageBits(pixels, (UWORD)image_x + offset, count) >> shift :
                    ((UBYTE)sprite->PlaneOnOff & (1U << plane) ? 0xffff : 0);
                UWORD* word = (UWORD*)dest + (((UWORD)bitmap_x + offset) >> 4);
                *word = (*word & ~active) | (bits & active);
                offset += count; remaining -= count;
            }
            if (picked) { pixels += sprite->Width; }
            if (overlay) { shadow += sprite->Width; }
            dest += bitmap->BytesPerRow;
        }
    }
}

static void transferClips(const struct VSprite* sprite, const struct RastPort* port,
    const struct ClipRect* clips, struct BitMap* bitmap, BOOL super_bitmap, BOOL save,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height)
{
    WORD right = bitmap_x + width - 1, bottom = bitmap_y + height - 1;
    for (const struct ClipRect* clip = clips; clip; clip = clip->Next) {
        WORD left = bitmap_x > clip->bounds.MinX ? bitmap_x : clip->bounds.MinX;
        WORD top = bitmap_y > clip->bounds.MinY ? bitmap_y : clip->bounds.MinY;
        WORD last_x = right < clip->bounds.MaxX ? right : clip->bounds.MaxX;
        WORD last_y = bottom < clip->bounds.MaxY ? bottom : clip->bounds.MaxY;
        if (left > last_x || top > last_y) { continue; }
        WORD x = left, y = top;
        struct BitMap* target = bitmap;
        if (!super_bitmap && clip->obscured) {
            target = clip->BitMap;
            x -= clip->bounds.MinX & ~15;
            y -= clip->bounds.MinY;
        }
        if (!target) { continue; }
        WORD sx = image_x + left - bitmap_x, sy = image_y + top - bitmap_y;
        if (save) {
            pcGelSaveBitmap(sprite, target, sx, sy, x, y, last_x - left + 1, last_y - top + 1, port->Mask);
        } else {
            pcGelDrawBitmap(sprite, target, sx, sy, x, y, last_x - left + 1, last_y - top + 1, port->Mask);
        }
    }
}

void pcGelTransferRastPort(const struct VSprite* sprite, struct RastPort* port, BOOL save,
    WORD image_x, WORD image_y, WORD bitmap_x, WORD bitmap_y, WORD width, WORD height)
{
    struct Layer* layer = port->Layer;
    if (!layer) {
        if (save) {
            pcGelSaveBitmap(sprite, port->BitMap, image_x, image_y, bitmap_x, bitmap_y, width, height, port->Mask);
        } else {
            pcGelDrawBitmap(sprite, port->BitMap, image_x, image_y, bitmap_x, bitmap_y, width, height, port->Mask);
        }
        return;
    }
    // The caller holds the layer lock for the complete DrawGList traversal.
    // Unsaved obscured rectangles have no backing pixels to transfer.
    port->GelsInfo->Flags = port->Flags & 4;
    transferClips(sprite, port, layer->ClipRect, port->BitMap, FALSE, save,
        image_x, image_y, bitmap_x + layer->bounds.MinX - layer->Scroll_X,
        bitmap_y + layer->bounds.MinY - layer->Scroll_Y, width, height);
    if (layer->SuperBitMap) {
        transferClips(sprite, port, layer->SuperClipRect, layer->SuperBitMap, TRUE, save,
            image_x, image_y, bitmap_x, bitmap_y, width, height);
    }
}

static void drawBob(struct VSprite* sprite, struct RastPort* port, struct VSprite** last,
    struct GfxBase* graphics)
{
    WORD bitmap_height = port->BitMap->Rows;
    WORD bitmap_width = GetBitMapAttr(port->BitMap, BMA_WIDTH);
    struct Bob* bob = sprite->VSBob;
    if (bob->After && !(bob->After->Flags & BDRAWN)) {
        bob->Flags |= BWAITING;
        return;
    }
    WORD x = sprite->X, y = sprite->Y;
    WORD full_width = (UWORD)sprite->Width * 16, full_height = sprite->Height;
    WORD dest_x = x < 0 ? 0 : x, dest_y = y < 0 ? 0 : y;
    WORD source_x = dest_x - x, source_y = dest_y - y;
    WORD width = (UWORD)full_width - source_x, height = (UWORD)full_height - source_y;
    if ((LONG)dest_x + width > bitmap_width) { width = bitmap_width - dest_x; }
    if ((LONG)dest_y + height > bitmap_height) { height = bitmap_height - dest_y; }
    BOOL visible = width > 0 && height > 0;
    if (visible) { sprite->Flags &= ~GELGONE; }
    else { sprite->Flags |= GELGONE; }
    if (sprite->Flags & SAVEBACK) {
        if (sprite->Flags & BACKSAVED) { restoreBackground(sprite, port, graphics); }
        WORD right = x + full_width - 1, bottom = y + full_height - 1;
        struct VSprite* tail = port->GelsInfo->gelTail;
        for (struct VSprite* next = port->GelsInfo->gelHead->NextVSprite; next != tail; next = next->NextVSprite) {
            if (!(next->Flags & BACKSAVED) || x > next->OldX + (LONG)next->Width * 16 - 1 ||
                next->OldX > right || y > next->OldY + (LONG)next->Height - 1 || next->OldY > bottom) { continue; }
            restoreBackground(next, port, graphics);
        }
        if (visible) {
            pcGelTransferRastPort(sprite, port, TRUE, source_x, source_y, dest_x, dest_y, width, height);
            // Backgrounds saved this pass must not be restored again until the
            // following pass. BACKSAVED is exchanged at frame completion.
            sprite->Flags |= BOBUPDATE;
        }
    }
    if (visible) {
        (*last)->DrawPath = sprite;
        *last = sprite;
        pcGelTransferRastPort(sprite, port, FALSE, source_x, source_y, dest_x, dest_y, width, height);
    }
    bob->Flags = (bob->Flags & ~BWAITING) | BDRAWN;
    if (bob->Before && (bob->Before->Flags & BWAITING)) {
        drawBob(bob->Before->BobVSprite, port, last, graphics);
    }
}

void pcGelProcessBob(struct VSprite* sprite, struct RastPort* port, struct VSprite** last,
    struct GfxBase* graphics)
{
    struct Bob* bob = sprite->VSBob;
    bob->Flags &= ~(BWAITING | BDRAWN);
    if (bob->Flags & BOBSAWAY) {
        if (!bob->DBuffer || (bob->Flags & OUTSTEP)) {
            if (sprite->Flags & BACKSAVED) { restoreBackground(sprite, port, graphics); }
            pcRemVSprite(sprite);
            bob->Flags |= BOBNIX;
        } else {
            if (sprite->Flags & BACKSAVED) { restoreBackground(sprite, port, graphics); }
            bob->Flags |= OUTSTEP;
        }
    } else {
        drawBob(sprite, port, last, graphics);
    }
}

void pcGelFinishFrame(struct RastPort* port, struct VSprite* last, struct GfxBase* graphics)
{
    struct VSprite* tail = port->GelsInfo->gelTail;
    last->DrawPath = tail;
    for (struct VSprite* sprite = port->GelsInfo->gelHead->NextVSprite; sprite != tail; sprite = sprite->NextVSprite) {
        if (sprite->Flags & BACKSAVED) { restoreBackground(sprite, port, graphics); }
        if (sprite->Flags & BOBUPDATE) { sprite->Flags = (sprite->Flags & ~BOBUPDATE) | BACKSAVED; }
        struct Bob* bob = sprite->VSBob;
        if (!bob) { continue; }
        bob->Flags &= ~(BWAITING | BDRAWN);
        struct DBufPacket* packet = bob->DBuffer;
        if (packet) {
            sprite->OldX = packet->BufX; packet->BufX = sprite->X;
            sprite->OldY = packet->BufY; packet->BufY = sprite->Y;
            sprite->ClearPath = packet->BufPath; packet->BufPath = sprite->DrawPath;
            WORD* buffer = bob->SaveBuffer;
            bob->SaveBuffer = packet->BufBuffer; packet->BufBuffer = buffer;
            BOOL saved = sprite->Flags & BACKSAVED;
            if (bob->Flags & SAVEPRESERVE) { sprite->Flags |= BACKSAVED; }
            else { sprite->Flags &= ~BACKSAVED; }
            if (saved) { bob->Flags |= SAVEPRESERVE; }
            else { bob->Flags &= ~SAVEPRESERVE; }
        } else {
            sprite->OldX = sprite->X;
            sprite->OldY = sprite->Y;
            sprite->ClearPath = sprite->DrawPath;
        }
    }
}

WORD pcGelAssignSprite(struct VSprite* sprite, struct GelsInfo* info, WORD top, WORD height, UBYTE reserved)
{
    UBYTE available = info->sprRsrvd & ~reserved;
    WORD selected = -1;
    // Prefer the free half of an occupied pair when its shared colours agree.
    // A NULL colour table imposes no colour requirement, as in DrawGList.
    for (unsigned pair = 0; pair < 8; pair += 2) {
        BOOL first = (available & (1U << pair)) && info->nextLine[pair] < top;
        BOOL second = (available & (2U << pair)) && info->nextLine[pair + 1] < top;
        if (!!first == !!second) { continue; }
        unsigned slot = first ? pair : pair + 1, partner = slot ^ 1;
        if (sprite->SprColors && info->lastColor[partner] && info->lastColor[partner] != sprite->SprColors) { continue; }
        selected = slot;
        break;
    }
    if (selected < 0) {
        for (unsigned pair = 0; pair < 8; pair += 2) {
            if ((available & (3U << pair)) == (3U << pair) &&
                info->nextLine[pair] < top && info->nextLine[pair + 1] < top) {
                selected = pair;
                break;
            }
        }
    }
    if (selected < 0) {
        sprite->Flags |= VSOVERFLOW;
    } else {
        info->lastColor[selected] = sprite->SprColors;
        info->nextLine[selected] = top + height + 2;
    }
    return selected;
}
