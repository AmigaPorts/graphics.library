/* SPDX-License-Identifier: MPL-2.0 */
// Independent implementation of public graphics element state and mask operations.
#include "internal/gels.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#define __NOLIBBASE__
#include <proto/exec.h>

static LONG position(const struct VSprite* sprite)
{
    // The original ordering treats the adjacent Y/X words as one signed key:
    // Y is signed, but X orders as an unsigned word within each scanline.
    return (LONG)(((ULONG)(UWORD)sprite->Y << 16) | (UWORD)sprite->X);
}

void pcInitGels(struct VSprite* head asm("a0"), struct VSprite* tail asm("a1"), struct GelsInfo* info asm("a2"))
{
    info->gelHead = head;
    info->gelTail = tail;
    head->NextVSprite = tail;
    head->PrevVSprite = NULL;
    head->ClearPath = tail;
    tail->NextVSprite = NULL;
    tail->PrevVSprite = head;
    head->Y = head->OldY = head->X = head->OldX = -32767;
    tail->Y = tail->OldY = tail->X = tail->OldX = 32767;
    if (info->collHandler) { info->collHandler->collPtrs[0] = NULL; }
}

static void insertAfter(struct VSprite* sprite, struct VSprite* previous)
{
    sprite->NextVSprite = previous->NextVSprite;
    previous->NextVSprite->PrevVSprite = sprite;
    previous->NextVSprite = sprite;
    sprite->PrevVSprite = previous;
}

void pcAddVSprite(struct VSprite* sprite asm("a0"), struct RastPort* port asm("a1"))
{
    sprite->Flags &= SUSERFLAGS;
    LONG key = position(sprite);
    struct VSprite* previous = port->GelsInfo->gelHead;
    while (position(previous->NextVSprite) < key) { previous = previous->NextVSprite; }
    insertAfter(sprite, previous);
    sprite->OldY = sprite->Y;
    sprite->OldX = sprite->X;
}

void pcAddBob(struct Bob* bob asm("a0"), struct RastPort* port asm("a1"))
{
    bob->Flags &= BUSERFLAGS;
    // AddBob uses the same implementation internally, not an installed public
    // AddVSprite owner. Preserve that distinction at the library boundary.
    pcAddVSprite(bob->BobVSprite, port);
}

void pcRemVSprite(struct VSprite* sprite asm("a0"))
{
    sprite->PrevVSprite->NextVSprite = sprite->NextVSprite;
    sprite->NextVSprite->PrevVSprite = sprite->PrevVSprite;
}

void pcSetCollision(ULONG number asm("d0"), VOID (*routine)(struct VSprite*, struct VSprite*) asm("a0"),
    struct GelsInfo* info asm("a1"))
{
    info->collHandler->collPtrs[number] = (LONG (*)(struct VSprite*, struct VSprite*))routine;
}

void pcSortGList(struct RastPort* port asm("a1"))
{
    struct GelsInfo* info = port->GelsInfo;
    struct VSprite* sprite = info->gelHead->NextVSprite;
    while (sprite != info->gelTail) {
        // Save the next visit before moving this node. Equal keys retain their
        // existing neighbour order; neither flags nor old positions change.
        struct VSprite* next = sprite->NextVSprite;
        LONG key = position(sprite);
        struct VSprite* previous = sprite->PrevVSprite;
        if (key < position(previous)) {
            do { previous = previous->PrevVSprite; } while (key < position(previous));
            pcRemVSprite(sprite);
            insertAfter(sprite, previous);
        } else if (key > position(next)) {
            struct VSprite* following = next;
            do { following = following->NextVSprite; } while (key > position(following));
            pcRemVSprite(sprite);
            insertAfter(sprite, following->PrevVSprite);
        }
        sprite = next;
    }
}

void pcInitMasks(struct VSprite* sprite asm("a0"))
{
    // BorderLine is reduced from the initial image rows, independently of the
    // collision mask. Do not replace it with a reduction of the combined planes.
    WORD* border = sprite->BorderLine;
    for (WORD column = 1; column <= sprite->Width; column = (UWORD)column + 1) {
        *border = 0;
        const WORD* source = sprite->ImageData + column - 1;
        for (WORD row = 1; row <= sprite->Height; row = (UWORD)row + 1) {
            *border |= *source;
            source += sprite->Width;
        }
        ++border;
    }

    // The original collision-mask loops consume the low word of Width*Height.
    UWORD words = (ULONG)(UWORD)sprite->Width * (UWORD)sprite->Height;
    const WORD* source = sprite->ImageData;
    WORD* mask = sprite->CollMask;
    if (sprite->Flags & VSPRITE) {
        // Hardware-sprite images contain alternating words from two planes;
        // Depth does not select this layout or the number of planes combined.
        for (ULONG i = 0; i < words; ++i) {
            mask[i] = source[0] | source[1];
            source += 2;
        }
    } else {
        for (ULONG i = 0; i < words; ++i) { mask[i] = *source++; }
        UWORD remaining = (UWORD)sprite->Depth - 1;
        while (remaining--) {
            for (ULONG i = 0; i < words; ++i) { mask[i] |= *source++; }
        }
    }
}

void pcInitGMasks(struct AnimOb* object asm("a0"))
{
    for (struct AnimComp* component = object->HeadComp; component; component = component->NextComp) {
        struct AnimComp* frame = component;
        do {
            // InitGMasks uses the mask implementation internally, not a public
            // InitMasks override. Sequence lists may be circular or terminated.
            pcInitMasks(frame->AnimBob->BobVSprite);
            frame = frame->NextSeq;
        } while (frame && frame != component);
    }
}

void pcAddAnimOb(struct AnimOb* object asm("a0"), struct AnimOb** key asm("a1"), struct RastPort* port asm("a2"))
{
    object->PrevOb = NULL;
    object->NextOb = *key;
    if (*key) { (*key)->PrevOb = object; }
    *key = object;
    for (struct AnimComp* component = object->HeadComp; component; component = component->NextComp) {
        component->Timer = component->TimeSet;
        pcAddBob(component->AnimBob, port);
    }
}

void pcAnimate(struct AnimOb** key asm("a0"), struct RastPort* port asm("a1"))
{
    for (struct AnimOb* object = *key; object; object = object->NextOb) {
        object->Clock = (ULONG)object->Clock + 1;
        object->AnOldX = object->AnX;
        object->AnOldY = object->AnY;
        object->AnX = (UWORD)object->AnX + object->XVel;
        object->AnY = (UWORD)object->AnY + object->YVel;
        object->XVel = (UWORD)object->XVel + object->XAccel;
        object->YVel = (UWORD)object->YVel + object->YAccel;
        if (object->AnimORoutine) { object->AnimORoutine(object); }

        struct AnimComp* component = object->HeadComp;
        while (component) {
            if (component->Timer && !(component->Timer = (UWORD)component->Timer - 1)) {
                struct AnimComp* next = component->NextSeq;
                struct Bob* old_bob = component->AnimBob;
                struct Bob* new_bob = next->AnimBob;
                struct VSprite* old_sprite = old_bob->BobVSprite;
                struct VSprite* new_sprite = new_bob->BobVSprite;
                if (old_bob != new_bob) {
                    new_bob->Before = old_bob->Before;
                    if (old_bob->Before) { old_bob->Before->After = new_bob; }
                    new_bob->After = old_bob->After;
                    if (old_bob->After) { old_bob->After->Before = new_bob; }
                    new_bob->Flags = old_bob->Flags;
                }
                new_sprite->X = old_sprite->X;
                new_sprite->Y = old_sprite->Y;
                old_bob->Flags |= BOBSAWAY;
                pcAddBob(new_bob, port);
                // Drawing retires the previous Bob later. Animate retains its
                // list entry and transfers the saved drawing paths to the new one.
                old_sprite->Y = old_sprite->X = -32767;
                new_sprite->ClearPath = old_sprite->ClearPath;
                new_sprite->DrawPath = old_sprite->DrawPath;
                if (component->PrevComp) { component->PrevComp->NextComp = next; }
                if (component->NextComp) { component->NextComp->PrevComp = next; }
                next->NextComp = component->NextComp;
                next->PrevComp = component->PrevComp;
                next->Timer = next->TimeSet;
                if (object->HeadComp == component) { object->HeadComp = next; }
                if (next->Flags & RINGTRIGGER) {
                    object->AnX = (UWORD)object->AnX + object->RingXTrans;
                    object->AnY = (UWORD)object->AnY + object->RingYTrans;
                }
                component = next;
            }
            if (component->AnimCRoutine) { component->AnimCRoutine(component); }
            struct VSprite* sprite = component->AnimBob->BobVSprite;
            // Translate in the animation's 1/64-pixel units, then round the
            // signed sum. Neither input is truncated before the addition.
            sprite->X = ((LONG)component->XTrans + object->AnX + ANIMHALF) >> ANFRACSIZE;
            sprite->Y = ((LONG)component->YTrans + object->AnY + ANIMHALF) >> ANFRACSIZE;
            component = component->NextComp;
        }
    }
}

BOOL pcGetGBuffers(struct AnimOb* object asm("a0"), struct RastPort* port asm("a1"), BOOL double_buffer asm("d0"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    for (struct AnimComp* component = object->HeadComp; component; component = component->NextComp) {
        struct AnimComp* frame = component;
        do {
            struct Bob* bob = frame->AnimBob;
            struct VSprite* sprite = bob->BobVSprite;
            WORD row_bytes = (UWORD)sprite->Width * 2;
            WORD plane_bytes = (ULONG)(UWORD)row_bytes * (UWORD)sprite->Height;
            WORD save_bytes = (ULONG)(UWORD)plane_bytes * port->BitMap->Depth;
            bob->SaveBuffer = AllocMem((LONG)save_bytes, MEMF_PUBLIC | MEMF_CHIP);
            if (!bob->SaveBuffer) { return FALSE; }
            if ((WORD)double_buffer) {
                bob->DBuffer = AllocMem(sizeof(*bob->DBuffer), MEMF_PUBLIC);
                if (!bob->DBuffer) { return FALSE; }
                bob->DBuffer->BufBuffer = AllocMem((LONG)save_bytes, MEMF_PUBLIC | MEMF_CHIP);
                if (!bob->DBuffer->BufBuffer) { return FALSE; }
            }
            sprite->BorderLine = AllocMem((LONG)row_bytes, MEMF_PUBLIC | MEMF_CHIP);
            if (!sprite->BorderLine) { return FALSE; }
            // VSBob need not be the component's Bob. The original publishes the
            // same mask in that owner's ImageShadow and this sprite's CollMask.
            sprite->VSBob->ImageShadow = AllocMem((LONG)plane_bytes, MEMF_PUBLIC | MEMF_CHIP);
            sprite->CollMask = sprite->VSBob->ImageShadow;
            if (!sprite->CollMask) { return FALSE; }
            frame = frame->NextSeq;
        } while (frame && frame != component);
    }
    return TRUE;
}

void pcFreeGBuffers(struct AnimOb* object asm("a0"), struct RastPort* port asm("a1"), BOOL double_buffer asm("d0"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    for (struct AnimComp* component = object->HeadComp; component; component = component->NextComp) {
        struct AnimComp* frame = component;
        do {
            struct Bob* bob = frame->AnimBob;
            struct VSprite* sprite = bob->BobVSprite;
            WORD row_bytes = (UWORD)sprite->Width * 2;
            WORD plane_bytes = (ULONG)(UWORD)row_bytes * (UWORD)sprite->Height;
            WORD save_bytes = (ULONG)(UWORD)plane_bytes * port->BitMap->Depth;
            FreeMem(bob->SaveBuffer, (LONG)save_bytes);
            if ((WORD)double_buffer) {
                FreeMem(bob->DBuffer->BufBuffer, (LONG)save_bytes);
                FreeMem(bob->DBuffer, sizeof(*bob->DBuffer));
            }
            FreeMem(sprite->BorderLine, (LONG)row_bytes);
            FreeMem(sprite->CollMask, (LONG)plane_bytes);
            frame = frame->NextSeq;
        } while (frame && frame != component);
    }
}

WORD pcGelBorderHits(const struct VSprite* sprite, const struct GelsInfo* info)
{
    WORD hits = 0;
    if (sprite->Y < info->topmost) {
        // Preserve the original scan extent, including its two extra rows.
        WORD rows = (UWORD)info->topmost - sprite->Y + 2;
        LONG words = (LONG)sprite->Width * rows;
        for (LONG i = 0; i < words; ++i) {
            if (sprite->CollMask[i]) { hits |= 1; break; }
        }
    }
    if ((LONG)sprite->Y + sprite->Height - 1 > info->bottommost) {
        WORD first = (UWORD)info->bottommost - sprite->Y + 1;
        WORD rows = (UWORD)sprite->Height - first;
        const WORD* mask = sprite->CollMask + (LONG)sprite->Width * first;
        LONG words = (LONG)sprite->Width * rows;
        for (LONG i = 0; i < words; ++i) {
            if (mask[i]) { hits |= 2; break; }
        }
    }

    WORD left = (UWORD)info->leftmost - sprite->X;
    if (left > 0) {
        if (left > (LONG)sprite->Width * 16) { left = (UWORD)sprite->Width * 16; }
        const UWORD* border = (const UWORD*)sprite->BorderLine;
        // The left edge consumes pairs of words; the right edge consumes one.
        // Read the same padded pair even when fewer than 32 pixels are tested.
        do {
            ULONG bits = (ULONG)border[0] << 16 | border[1];
            border += 2;
            if (left < 32) {
                unsigned shift = (32 - left) & 63;
                bits = shift < 32 ? bits >> shift : 0;
            }
            if (bits) { hits |= 4; break; }
            left = (UWORD)left - 32;
        } while (left > 0);
    }
    WORD right = (LONG)sprite->X + (LONG)sprite->Width * 16 - info->rightmost - 1;
    if (right > 0) {
        if (right > (LONG)sprite->Width * 16) { right = (UWORD)sprite->Width * 16; }
        const UWORD* border = (const UWORD*)sprite->BorderLine + sprite->Width;
        do {
            UWORD bits = *--border;
            if (right < 16) {
                unsigned shift = (16 - right) & 63;
                bits = shift < 16 ? (ULONG)bits << shift : 0;
            }
            if (bits) { hits |= 8; break; }
            right = (UWORD)right - 16;
        } while (right > 0);
    }
    return hits;
}

static BOOL gelMasksOverlap(const struct VSprite* first, const struct VSprite* second)
{
    WORD first_right = (UWORD)first->X + (UWORD)first->Width * 16 - 1;
    WORD second_right = (UWORD)second->X + (UWORD)second->Width * 16 - 1;
    WORD first_bottom = (UWORD)first->Y + first->Height - 1;
    WORD second_bottom = (UWORD)second->Y + second->Height - 1;
    WORD left = first->X > second->X ? first->X : second->X;
    WORD right = first_right < second_right ? first_right : second_right;
    WORD top = first->Y > second->Y ? first->Y : second->Y;
    WORD bottom = first_bottom < second_bottom ? first_bottom : second_bottom;
    WORD width = (UWORD)right - left + 1;
    WORD height = (UWORD)bottom - top + 1;
    WORD first_word = ((LONG)left - first->X) >> 4;
    WORD words = (((LONG)left + width - first->X - 1) >> 4) - first_word + 1;
    unsigned shift = ((UWORD)second->X - first->X) & 15;
    WORD right_difference = (UWORD)first_right - second_right;
    unsigned last_shift = right_difference < 0 ? 0 : right_difference & 15;
    UWORD first_mask = 0xffffU >> shift;
    UWORD last_mask = 0xffffU << last_shift;
    WORD first_offset = ((ULONG)(UWORD)(top - first->Y) * (UWORD)first->Width + first_word) * 2;
    WORD second_offset = ((ULONG)(UWORD)(top - second->Y) * (UWORD)second->Width +
        (((LONG)left - second->X) >> 4)) * 2;
    const UWORD* first_row = (const void*)((const UBYTE*)first->CollMask + first_offset);
    const UWORD* second_row = (const void*)((const UBYTE*)second->CollMask + second_offset);
    UWORD previous = 0;
    for (LONG y = 0; y < height; ++y) {
        for (LONG x = 0; x < words; ++x) {
            UWORD a = first_row[x];
            UWORD b = second_row[x];
            if (!x) { a &= first_mask; }
            if (x + 1 == words) { a &= last_mask; }
            // Preserve the original word alignment and edge masks, including
            // staggered rows with the second sprite to the left of the first.
            // A geometrical per-pixel intersection gives different results.
            UWORD aligned = ((ULONG)previous << 16 | b) >> shift;
            if (a & aligned) { return TRUE; }
            previous = b;
        }
        first_row += first->Width;
        second_row += second->Width;
    }
    return FALSE;
}

static void collideFollowing(struct VSprite* sprite, struct GelsInfo* info, struct VSprite* original)
{
    // Keep this scan's bounds across callbacks, but follow the live next link.
    WORD bottom = (UWORD)sprite->Y + sprite->Height;
    WORD left = sprite->X;
    WORD width = (UWORD)sprite->Width * 16;
    for (struct VSprite* next = sprite->NextVSprite; next->Y < bottom; next = next->NextVSprite) {
        WORD distance = (UWORD)next->X - left;
        if (distance < 0) {
            if (-(LONG)distance >= (LONG)next->Width * 16) { continue; }
        } else if (distance >= width) { continue; }
        UWORD types = sprite->MeMask & next->HitMask & 0xfffe;
        if (!types || !gelMasksOverlap(sprite, next)) { continue; }
        if ((sprite->VSBob && (sprite->VSBob->Flags & BOBSAWAY)) ||
            (next->VSBob && (next->VSBob->Flags & BOBSAWAY))) { continue; }
        unsigned number = 1;
        while (!(types & (1U << number))) { ++number; }
        if (info->collHandler->collPtrs[number]) { info->collHandler->collPtrs[number](original, next); }
    }
}

void pcDoCollision(struct RastPort* port asm("a1"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    struct Layer* layer = port->Layer;
    if (layer) { ObtainSemaphore(&layer->Lock); }
    struct GelsInfo* info = port->GelsInfo;
    struct VSprite* tail = info->gelTail;
    BOOL borders = info->collHandler->collPtrs[0] != NULL;
    for (struct VSprite* original = info->gelHead->NextVSprite; original != tail; original = original->NextVSprite) {
        struct VSprite adjusted;
        struct VSprite* sprite = original;
        if (layer && (original->Flags & VSPRITE)) {
            adjusted = *original;
            adjusted.X = (UWORD)adjusted.X - layer->bounds.MinX + layer->Scroll_X;
            adjusted.Y = (UWORD)adjusted.Y - layer->bounds.MinY + layer->Scroll_Y;
            sprite = &adjusted;
        }
        if (borders && sprite->HitMask) {
            WORD hits = pcGelBorderHits(sprite, info);
            if (hits && !(original->VSBob && (original->VSBob->Flags & BOBSAWAY))) {
                typedef LONG (*BorderRoutine)(struct VSprite*, LONG);
                BorderRoutine routine = (BorderRoutine)info->collHandler->collPtrs[0];
                if (routine) { routine(original, (LONG)hits); }
            }
        }
        collideFollowing(sprite, info, original);
    }
    if (layer) { ReleaseSemaphore(&layer->Lock); }
}
