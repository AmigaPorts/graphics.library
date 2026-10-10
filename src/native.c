/* SPDX-License-Identifier: MPL-2.0 */
// Native display ownership. Exec supplies frame delivery and task scheduling;
// devicetree.resource supplies the existing firmware framebuffer. No PCI mode
// change, custom-chip access or emulator service is involved.
#include "internal/native.h"
#include "internal/library.h"
#include "internal/node.h"
#include "internal/monitor.h"
#include "internal/frame.h"
#include "internal/color.h"
#include "internal/state.h"
#include <exec/memory.h>
#include <hardware/intbits.h>
#define __NOLIBBASE__
#include <proto/exec.h>

#define DISPLAY_STACK_SIZE 16384

void pcWaitBlit(void)
{
    // Native bitmap writes are synchronous: there is no submitted transfer
    // left to poll after a drawing call returns. Keep the compiler boundary
    // for callers that reuse or release those buffers immediately afterward.
    // Do not wait for BlitLock, drain queued callbacks, or wait for display
    // presentation: none of those is the active-transfer contract of WaitBlit.
    // Queued chipset callbacks remain unavailable on this backend. A future
    // asynchronous backend must supply its own completion implementation.
    __asm__ volatile("" : : : "memory");
}

ULONG pcSetChipRev(ULONG requested asm("d0"), struct GfxBase* graphics asm("a6"))
{
    // This backend presents a firmware framebuffer, not an Amiga chipset.
    // Renderer/P96 capabilities do not describe Agnus or Denise revisions.
    // Keep the software selection and database rules shared with a future
    // chipset backend, which will supply its own discovery results.
    struct GraphicsChipRevision revision = pcApplyChipRevision(requested, 0, 0, graphics);

    if (revision.changed) {
        pcRefreshDisplayRevision(revision.reported, graphics);
    }

    return revision.reported;
}

static BOOL presentBlank(struct NativeGraphicsDisplay* native, ULONG color)
{
    if (native->blank && native->blank_color == color) {
        return TRUE;
    }

    if (!pcFillGraphicsFramebuffer(&native->output, color) || !pcPresentGraphicsFramebuffer(&native->output)) {
        return FALSE;
    }

    native->blank = TRUE;
    native->blank_color = color;
    return TRUE;
}

static void releaseDisplay(struct NativeGraphicsDisplay* native, struct GfxBase* graphics)
{
    for (unsigned field = 0; field < 2; ++field) {
        pcFreeViewFieldBuffers(native->buffers[field], graphics);
        native->buffers[field] = NULL;
    }

    pcReleaseViewDisplay(native->display, graphics);
    native->display = NULL;
}

static BOOL displayCrop(struct Rectangle* crop, struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    if (!monitor || !monitor->ms_videoscan || !monitor->total_rows || monitor->total_rows > 1024 ||
        monitor->total_colorclocks < 2) {
        return FALSE;
    }

    typedef LONG (*ScanFunction)(struct MonitorSpec* monitor asm("a0"), struct Rectangle* rectangle asm("a1"),
        ULONG modes asm("d0"), struct GfxBase* graphics asm("a6"));
    struct Rectangle bounds;

    if (((ScanFunction)monitor->ms_videoscan)(monitor, &bounds, SUPERHIRES, graphics) ||
        bounds.MinX < 0 || bounds.MaxX > 8191 || bounds.MinX >= bounds.MaxX ||
        bounds.MinY < 0 || bounds.MinY >= bounds.MaxY || bounds.MaxY > monitor->total_rows) {
        return FALSE;
    }

    // Monitor columns are LORES coordinates; the compositor keeps quarter
    // pixels so mixed-resolution viewports and sprite positions share a grid.
    bounds.MinX *= 4;
    bounds.MaxX *= 4;
    *crop = bounds;

    return TRUE;
}

static ULONG scaledSize(ULONG size, int shift)
{
    return shift < 0 ? (size + (1UL << -shift) - 1) >> -shift : size << shift;
}

static void displayPlacement(struct GraphicsFramebufferScaleArea* area,
    const struct NativeGraphicsDisplay* native, BOOL lace)
{
    *area = (struct GraphicsFramebufferScaleArea){0};
    area->width = native->crop.MaxX - native->crop.MinX;
    area->height = native->crop.MaxY - native->crop.MinY;
    area->x_shift = -2;
    area->y_shift = lace ? -1 : 0;
    ULONG rows = native->output.mode.height;

    if (lace) {
        // Use the complete row pairs for both fields. An odd final row must
        // not make one field choose a different scale or vertical origin.
        rows /= 2;
    }

    // Preserve the logical LORES pixel aspect with integral power-of-two
    // scaling. Recompute rounded extents at each scale: doubling an already
    // rounded extent would misplace odd-sized crops.
    while (area->x_shift < 2 && area->y_shift < 2 &&
        scaledSize(area->width, area->x_shift + 1) <= native->output.mode.width &&
        scaledSize(area->height, area->y_shift + 1) <= rows) {
        ++area->x_shift;
        ++area->y_shift;
    }

    ULONG width = scaledSize(area->width, area->x_shift);
    ULONG height = scaledSize(area->height, area->y_shift);
    area->dest_x = ((LONG)native->output.mode.width - (LONG)width) / 2;
    // Interlaced output is addressed in rows of the selected physical field.
    area->dest_y = ((LONG)rows - (LONG)height) / 2;
}

static BOOL nextDisplayField(const struct ActiveViewFields* active, struct ViewDisplaySnapshot* current,
    ULONG identity, struct ViewDisplaySnapshot** display, unsigned* source)
{
    if (!identity) {
        return FALSE;
    }

    // LoadView can select the two fields from different merged generations.
    // Resolve explicitly selected output before following the retained partner
    // in the current snapshot; the latter is still needed for a missing SHF.
    for (unsigned slot = 0; slot < 2; ++slot) {
        struct ViewDisplaySnapshot* selected = active->display[slot];
        unsigned field = active->source_field[slot];

        if (selected && field < 2 && selected->timing[field].identity == identity) {
            *display = selected;
            *source = field;
            return TRUE;
        }
    }

    *display = current;
    return pcResolveViewFieldLink(current, identity, source);
}

struct NativeFieldReader {
    const struct DBufInfo* info;
    ULONG viewport_identity;
    const struct BitMap* previous;
    const struct ViewDisplaySnapshot* display;
    unsigned source;
    ULONG link;
    BOOL updated, retained;
};

static BOOL nativeReaderSetup(APTR context, const struct ViewFieldPortEvent* event, const Point* position)
{
    (void)position;
    struct NativeFieldReader* reader = context;
    const struct ViewDisplayPort* port = event->port;

    if (!port->prepared || !port->prepared->display) {
        return FALSE;
    }

    BOOL updated = FALSE;

    if (reader->info) {
        const struct ViewFieldPalette* palette = &port->palette[reader->source];
        ULONG identity = palette->start ? (ULONG)palette->start :
            reader->display->timing[reader->source].identity +
                sizeof(UWORD) * port->prefix_word[reader->source];

        if (identity && (identity == (ULONG)reader->info->dbi_CopPtr2 ||
            identity == (ULONG)reader->info->dbi_CopPtr3)) {
            if (palette->start) {
                reader->updated = TRUE;
                updated = TRUE;
            } else if (!reader->viewport_identity) {
                reader->retained = TRUE;
            }
        }
    }

    if (reader->viewport_identity && !updated &&
        port->prepared->display->viewport_identity == reader->viewport_identity) {
        LONG overlap = pcViewPortBitMapOverlap(port->prepared->display,
            &port->rasters[reader->source], reader->previous);

        if (overlap < 0) {
            return FALSE;
        }

        reader->retained |= overlap != 0;
    }

    return TRUE;
}

static BOOL nativeReaderMoves(APTR context, const struct ViewFieldRun* run)
{
    struct NativeFieldReader* reader = context;

    return pcReadNativeViewFieldLink(&reader->link, run);
}

static BOOL nativeReaderInterval(APTR context, const struct ViewFieldRun* run)
{
    (void)context;
    (void)run;

    // Source discovery follows field restarts without reading bitmap pixels.
    return TRUE;
}

static BOOL readNativeFields(struct NativeViewFields* fields, struct NativeBitmapFields* targets,
    const struct DBufInfo* info, ULONG viewport_identity, const struct BitMap* previous,
    struct GfxBase* graphics)
{
    const struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;
    const struct ActiveViewFields* active = pcActiveViewFields(graphics);
    struct NativeViewFields selected = {0};
    struct NativeBitmapFields bitmap = {0};

    if (!native || !active->owned || !active->enabled) {
        *fields = selected;

        if (targets) {
            *targets = bitmap;
        }

        return TRUE;
    }

    const struct MonitorSpec* monitor = graphics->current_monitor;

    if (!monitor || !monitor->total_rows || monitor->total_rows > 1024 || monitor->total_colorclocks < 2) {
        return FALSE;
    }

    UWORD horizontal = monitor->total_colorclocks - 2;

    if (horizontal > 254) {
        horizontal = 254;
    }

    horizontal &= ~1;
    struct ViewDisplaySnapshot* display = active->display[0];
    unsigned source = active->source_field[0];

    if (native->generation == active->generation && native->display) {
        display = native->display;
        source = native->source;
    }

    static const struct ViewFieldScheduleVisitor visitor = {
        nativeReaderSetup, nativeReaderMoves, nativeReaderInterval
    };

    for (;;) {
        if (!display || source >= 2 || !display->timing[source].identity) {
            return FALSE;
        }

        unsigned slot = 0;

        while (slot < 2 && selected.display[slot] && selected.display[slot] != display) {
            ++slot;
        }

        // The renderer starts at LOF and can select only an active field or
        // a retained partner in its current snapshot. At most the two active
        // merge generations (four distinct source fields) can be reachable.
        if (slot == 2) {
            return FALSE;
        }

        if (selected.fields[slot] & (1U << source)) {
            *fields = selected;

            if (targets) {
                *targets = bitmap;
            }

            return TRUE;
        }

        selected.display[slot] = display;
        selected.fields[slot] |= 1U << source;
        struct NativeFieldReader reader = {info, viewport_identity, previous, display, source,
            display->timing[source].identity, FALSE, FALSE};

        if (!pcScheduleViewField(display, source, monitor->total_rows, horizontal, &visitor, &reader)) {
            return FALSE;
        }

        if (reader.updated) {
            bitmap.updated.display[slot] = display;
            bitmap.updated.fields[slot] |= 1U << source;
        }

        if (reader.retained) {
            bitmap.retained.display[slot] = display;
            bitmap.retained.fields[slot] |= 1U << source;
        }

        if (!nextDisplayField(active, display, reader.link, &display, &source)) {
            return FALSE;
        }
    }
}

BOOL pcReadNativeViewFields(struct NativeViewFields* fields, struct GfxBase* graphics)
{
    return readNativeFields(fields, NULL, NULL, 0, NULL, graphics);
}

BOOL pcReadNativeBitmapFields(struct NativeBitmapFields* fields, const struct DBufInfo* info,
    struct GfxBase* graphics)
{
    struct NativeViewFields readers;

    return readNativeFields(&readers, fields, info, 0, NULL, graphics);
}

BOOL pcReadNativeBitmapSwitch(struct NativeBitmapFields* fields, const struct ViewPort* viewport,
    const struct BitMap* previous, const struct DBufInfo* info, struct GfxBase* graphics)
{
    struct NativeViewFields readers;

    return readNativeFields(&readers, fields, info, (ULONG)viewport, previous, graphics);
}

static UBYTE nativeReaderMask(const struct NativeViewFields* readers, const struct ViewDisplaySnapshot* display)
{
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (readers->display[slot] == display) {
            return readers->fields[slot];
        }
    }

    return 0;
}

static BOOL queueBitmapReply(struct DBufInfo* info,
    struct ViewDisplaySnapshot* first, UBYTE first_fields,
    struct ViewDisplaySnapshot* second, UBYTE second_fields, BOOL safe, BOOL unresolved,
    struct GfxBase* graphics)
{
    struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;
    const struct ActiveViewFields* active = pcActiveViewFields(graphics);
    first_fields = first ? first_fields & 3 : 0;
    second_fields = second ? second_fields & 3 : 0;

    struct Message* message = safe ? &info->dbi_SafeMessage : &info->dbi_DispMessage;

    if (!native || !active->owned || !message->mn_ReplyPort ||
        (!unresolved && !first_fields && !second_fields)) {
        return FALSE;
    }

    if (!unresolved) {
        struct NativeViewFields readers;

        if (!pcReadNativeViewFields(&readers, graphics)) {
            return FALSE;
        }

        first_fields &= nativeReaderMask(&readers, first);
        second_fields &= nativeReaderMask(&readers, second);

        if (!first_fields && !second_fields) {
            return FALSE;
        }
    }

    struct GraphicsDBufInfo* allocation = (APTR)info;
    struct NativeBitmapReply* reply = safe ? &allocation->safe_reply : &allocation->reply;
    reply->info = info;
    reply->display[0] = first_fields ? first : NULL;
    reply->display[1] = second_fields ? second : NULL;
    reply->fields[0] = first_fields;
    reply->fields[1] = second_fields;
    reply->generation = active->generation;
    reply->frame = graphics->VBCounter;
    reply->minimum_frames = safe ? info->dbi_Count1 : info->dbi_Count2;
    reply->safe = safe;
    reply->unresolved = unresolved;
    reply->targets[0] = unresolved ? info->dbi_CopPtr2 : NULL;
    reply->targets[1] = unresolved ? info->dbi_CopPtr3 : NULL;

    for (unsigned slot = 0; slot < 2; ++slot) {
        pcRetainViewDisplay(reply->display[slot]);
    }

    reply->next = native->replies;
    native->replies = reply;

    return TRUE;
}

BOOL pcQueueNativeBitmapReply(struct DBufInfo* info,
    struct ViewDisplaySnapshot* first, UBYTE first_fields,
    struct ViewDisplaySnapshot* second, UBYTE second_fields, struct GfxBase* graphics)
{
    return queueBitmapReply(info, first, first_fields, second, second_fields, FALSE, FALSE, graphics);
}

static void capturePreviousBitmap(struct DBufInfo* info, const struct ViewPort* viewport,
    const struct BitMap* previous)
{
    struct GraphicsDBufInfo* allocation = (APTR)info;
    allocation->viewport_identity = (ULONG)viewport;

    if (previous) {
        struct BitMap* saved = &allocation->previous;
        saved->BytesPerRow = previous->BytesPerRow;
        saved->Rows = previous->Rows;
        saved->Flags = previous->Flags;
        saved->Depth = previous->Depth;
        saved->pad = previous->pad;
        unsigned planes = previous->pad == 0x805c ? 1 : previous->Depth;

        // MINPLANES headers need not contain all eight entries. Interleaved
        // storage is one allocation at plane zero, as in FreeBitMap.
        for (unsigned plane = 0; plane < 8; ++plane) {
            saved->Planes[plane] = plane < planes ? previous->Planes[plane] : NULL;
        }
    }
}

BOOL pcQueueNativeBitmapSafeReply(struct DBufInfo* info,
    const struct ViewPort* viewport, const struct BitMap* previous,
    struct ViewDisplaySnapshot* first, UBYTE first_fields,
    struct ViewDisplaySnapshot* second, UBYTE second_fields, struct GfxBase* graphics)
{
    if ((viewport && !previous) || (previous && !viewport) ||
        !queueBitmapReply(info, first, first_fields, second, second_fields, TRUE, FALSE, graphics)) {
        return FALSE;
    }

    capturePreviousBitmap(info, viewport, previous);

    return TRUE;
}

// The display semaphore excludes composition/source changes. Interrupt
// exclusion additionally protects the original public VBlank reply list.
void pcChangeVPBitMap(struct ViewPort* viewport asm("a0"), struct BitMap* bitmap asm("a1"),
    struct DBufInfo* info asm("a2"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    if (!pcCacheNativeDBufInfo(viewport, info, graphics)) {
        ReleaseSemaphore(graphics->ActiViewCprSemaphore);
        return;
    }

    BOOL hidden = (viewport->Modes & VP_HIDE) != 0;
    BOOL safe_message = info->dbi_SafeMessage.mn_ReplyPort != NULL;
    BOOL display_message = info->dbi_DispMessage.mn_ReplyPort != NULL;
    struct BitMap* previous = hidden ? NULL : viewport->RasInfo->BitMap;
    struct NativeBitmapFields targets = {0};
    BOOL targets_valid = TRUE;

    if (!hidden && (display_message || safe_message)) {
        targets_valid = pcReadNativeBitmapSwitch(&targets, viewport, previous, info, graphics);
    }

    // The cached live sources become the new bitmap, even when its planes
    // alias the previous bitmap. Only older, unswitched sources hold it in use.
    pcSwitchNativeViewPortBitMap(viewport, bitmap, info, graphics);

    Disable();

    if (safe_message) {
        if (hidden && (WORD)info->dbi_BeamPos2 > 0) {
            info->dbi_Count1 = 1;
            info->dbi_Link1 = graphics->DBList;
            graphics->DBList = &info->dbi_Link1;
        } else if (!targets_valid) {
            info->dbi_Count1 = 1;
            capturePreviousBitmap(info, viewport, previous);
            queueBitmapReply(info, NULL, 0, NULL, 0, TRUE, TRUE, graphics);
        } else if (targets.retained.fields[0] || targets.retained.fields[1]) {
            info->dbi_Count1 = 1;
            pcQueueNativeBitmapSafeReply(info, viewport, previous,
                targets.retained.display[0], targets.retained.fields[0],
                targets.retained.display[1], targets.retained.fields[1], graphics);
        } else {
            ReplyMsg(&info->dbi_SafeMessage);
        }
    }

    if (display_message) {
        // Native composition runs atomically with respect to this call.
        // Its next frame can see the switch, but VBlank alone cannot prove
        // presentation. Hidden viewports retain the original two-frame delay.
        info->dbi_Count2 = hidden ? 2 : 1;

        if (!targets_valid) {
            queueBitmapReply(info, NULL, 0, NULL, 0, FALSE, TRUE, graphics);
        } else if (targets.updated.fields[0] || targets.updated.fields[1]) {
            pcQueueNativeBitmapReply(info, targets.updated.display[0], targets.updated.fields[0],
                targets.updated.display[1], targets.updated.fields[1], graphics);
        } else {
            info->dbi_Link2 = graphics->DBList;
            graphics->DBList = &info->dbi_Link2;
        }
    }

    Enable();
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

static BOOL resolveBitmapReply(struct NativeBitmapReply* reply, struct GfxBase* graphics)
{
    const struct GraphicsDBufInfo* allocation = (APTR)reply->info;
    struct NativeViewFields reached;
    struct NativeBitmapFields bitmap;
    struct DBufInfo targets = {0};
    targets.dbi_CopPtr2 = reply->targets[0];
    targets.dbi_CopPtr3 = reply->targets[1];

    if (!readNativeFields(&reached, &bitmap, &targets,
        reply->safe ? allocation->viewport_identity : 0,
        reply->safe ? &allocation->previous : NULL, graphics)) {
        return FALSE;
    }

    for (unsigned slot = 0; slot < 2; ++slot) {
        UBYTE fields = bitmap.retained.fields[slot];

        if (!reply->safe) {
            fields |= bitmap.updated.fields[slot];
        }

        reply->fields[slot] = fields;
        reply->display[slot] = fields ? reached.display[slot] : NULL;
        pcRetainViewDisplay(reply->display[slot]);
    }

    reply->unresolved = FALSE;

    return TRUE;
}

static void completeBitmapReplies(struct NativeGraphicsDisplay* native,
    struct ViewDisplaySnapshot* displayed, unsigned field, BOOL closing)
{
    struct GfxBase* graphics = native->graphics;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    const struct ActiveViewFields* active = pcActiveViewFields(graphics);
    struct NativeBitmapReply** link = &native->replies;
    struct NativeViewFields readers;
    BOOL have_readers = *link && !closing && pcReadNativeViewFields(&readers, graphics);

    while (*link) {
        struct NativeBitmapReply* reply = *link;
        BOOL retired = closing || !active->owned ||
            (!reply->safe && reply->generation != active->generation);

        if (reply->unresolved) {
            if (retired) {
                reply->unresolved = FALSE;
            } else if (!resolveBitmapReply(reply, graphics)) {
                link = &reply->next;
                continue;
            }
        }
        struct NativeBitmapFields bitmap;
        BOOL have_bitmap = FALSE;

        if (reply->safe && !retired && (reply->fields[0] || reply->fields[1])) {
            const struct GraphicsDBufInfo* allocation = (APTR)reply->info;

            if (allocation->viewport_identity) {
                struct NativeViewFields reached;
                have_bitmap = readNativeFields(&reached, &bitmap, NULL, allocation->viewport_identity,
                    &allocation->previous, graphics);
            }
        }

        for (unsigned slot = 0; slot < 2; ++slot) {
            if (retired) {
                reply->fields[slot] = 0;
            } else {
                // Live field-link edits can retire a pending source without
                // replacing the View. Only a valid traversal establishes that;
                // invalid descriptions must not masquerade as completion.
                if (have_readers) {
                    reply->fields[slot] &= nativeReaderMask(&readers, reply->display[slot]);
                }

                // A later scroll/switch can replace an old source while its
                // field remains reachable. Ignore the now-mutable public cache
                // and only remove pending readers; never add them back.
                if (have_bitmap) {
                    reply->fields[slot] &= nativeReaderMask(&bitmap.retained, reply->display[slot]);
                }

                if (!reply->safe && displayed && reply->display[slot] == displayed) {
                    reply->fields[slot] &= ~(1U << field);
                }
            }
        }

        ULONG elapsed = graphics->VBCounter - reply->frame;
        ULONG remaining = elapsed < reply->minimum_frames ? reply->minimum_frames - elapsed : 0;
        ULONG* count = reply->safe ? &reply->info->dbi_Count1 : &reply->info->dbi_Count2;
        *count = remaining;

        if (reply->fields[0] || reply->fields[1] || (!closing && remaining)) {
            link = &reply->next;
            continue;
        }

        // Remove all references before ReplyMsg: a higher-priority receiver
        // can immediately reuse or free its request after taking the message.
        struct DBufInfo* info = reply->info;
        struct Message* message = reply->safe ? &info->dbi_SafeMessage : &info->dbi_DispMessage;
        *link = reply->next;
        reply->next = NULL;
        reply->info = NULL;

        if (reply->safe) {
            struct GraphicsDBufInfo* allocation = (APTR)info;
            allocation->viewport_identity = 0;
        }

        for (unsigned slot = 0; slot < 2; ++slot) {
            struct ViewDisplaySnapshot* display = reply->display[slot];
            reply->display[slot] = NULL;
            pcReleaseViewDisplay(display, graphics);
        }

        *count = 0;
        ReplyMsg(message);
    }
}

BOOL pcRefreshNativeDisplay(struct GfxBase* graphics)
{
    struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;

    if (!native) {
        return FALSE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    struct ActiveViewFields* active = pcActiveViewFields(graphics);
    BOOL success = FALSE;

    if (!active->owned) {
        releaseDisplay(native, graphics);
        goto done;
    }

    if (!active->enabled) {
        releaseDisplay(native, graphics);
        native->colors.colors[0] = active->background.color;
        // The empty field ends in bank zero with high-nibble writes selected.
        native->controls.display &= ~0xe200;
        success = presentBlank(native, active->background.flags & BORDER_BLANKING ? 0 : active->background.color);
        goto done;
    }

    if (native->generation != active->generation || !native->display) {
        struct ViewDisplaySnapshot* display = active->display[0];

        if (!display) {
            goto done;
        }

        pcRetainViewDisplay(display);
        releaseDisplay(native, graphics);
        native->display = display;
        native->source = active->source_field[0];
        native->short_field = FALSE;
        native->generation = active->generation;
        native->clear_output = TRUE;
    }

    struct Rectangle crop;
    struct MonitorSpec* monitor = graphics->current_monitor;

    if (!displayCrop(&crop, monitor, graphics) || native->source >= 2) {
        goto done;
    }

    if (crop.MinX != native->crop.MinX || crop.MinY != native->crop.MinY ||
        crop.MaxX != native->crop.MaxX || crop.MaxY != native->crop.MaxY) {
        for (unsigned field = 0; field < 2; ++field) {
            pcFreeViewFieldBuffers(native->buffers[field], graphics);
            native->buffers[field] = NULL;
        }

        native->crop = crop;
        native->clear_output = TRUE;
    }

    unsigned source = native->source;

    if (!native->buffers[source]) {
        native->buffers[source] = pcAllocViewFieldBuffers(native->display, source, &native->output.mode,
            crop.MaxX - crop.MinX, crop.MaxY - crop.MinY, graphics);

        if (!native->buffers[source]) {
            goto done;
        }
    }

    struct ViewFieldBuffers* buffers = native->buffers[source];
    struct NativeSpriteSources sprites;
    struct NativeSpriteField field;

    if (!pcReadNativeSpriteSources(&sprites, graphics) ||
        !pcInitNativeSpriteField(&field, &sprites, &native->sprite_rows, 0, monitor->total_rows)) {
        goto done;
    }

    struct ViewFieldComposition composition = {0};
    composition.framebuffer = &buffers->field;
    composition.scratch = &buffers->scratch;
    composition.occlusion = &buffers->occlusion;
    struct GraphicsFramebufferPalette colors = native->colors;
    colors.colors[0] = active->background.color;
    composition.colors = &colors;
    pcReadViewPortSpriteLayout(&composition.working.sprites, NULL, graphics);
    composition.working.sprites.border_blank |= (active->background.flags & BORDER_BLANKING) != 0;
    composition.working.sprites.border_sprites |= (active->background.flags & BORDERSPRITES) != 0;
    composition.working.modes = graphics->Modes;

    if (!pcInitViewFieldSpriteRuns(&composition.sprites, &field, crop.MinX, crop.MinY)) {
        goto done;
    }

    struct ViewFieldControls controls = native->controls;
    unsigned next;
    struct ViewDisplaySnapshot* next_display;
    UWORD horizontal = monitor->total_colorclocks - 2;

    if (horizontal > 254) {
        horizontal = 254;
    }

    horizontal &= ~1;

    controls.field_link = native->display->timing[source].identity;
    composition.controls = pcApplyNativeViewFieldControls;
    composition.context = &controls;

    if (!controls.field_link ||
        !pcComposeScheduledViewField(&composition, native->display, source, horizontal) ||
        !nextDisplayField(active, native->display, controls.field_link, &next_display, &next)) {
        goto done;
    }

    struct GraphicsFramebufferScaleArea area;
    BOOL lace = (graphics->Modes & LACE) != 0;
    displayPlacement(&area, native, lace);
    ULONG background = composition.working.sprites.border_blank ? 0 : colors.colors[0];
    native->blank = FALSE;

    if (native->clear_output || !lace) {
        if (!pcFillGraphicsFramebuffer(&native->output, background)) {
            goto done;
        }
    } else {
        for (ULONG row = native->short_field; row < native->output.mode.height; row += 2) {
            if (!pcFillGraphicsFramebufferArea(&native->output, background,
                0, row, native->output.mode.width, 1)) {
                goto done;
            }
        }
    }

    if (lace) {
        success = pcScaleFramebufferField(&native->output, &buffers->field, &area, native->short_field);
    } else {
        success = pcScaleFramebufferArea(&native->output, &buffers->field, &area);
    }

    if (success) {
        success = pcPresentGraphicsFramebuffer(&native->output);
    }

    if (success) {
        completeBitmapReplies(native, native->display, source, FALSE);
        native->colors = colors;
        native->sprite_rows = field.retained;
        native->controls = controls;

        if (next_display != native->display) {
            pcRetainViewDisplay(next_display);
            releaseDisplay(native, graphics);
            native->display = next_display;
        }

        native->source = next;
        native->short_field = lace && !native->short_field;
        native->clear_output = FALSE;
    }

done:
    // Failed rendering cannot complete a selected field. Still service a
    // preceding completed copy's frame delay, or requests whose View was
    // replaced/hidden. Those no longer have a frame that can be presented.
    completeBitmapReplies(native, NULL, 0, FALSE);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return success;
}

void pcWakeNativeDisplay(struct GfxBase* graphics)
{
    struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;

    if (native) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        Signal(&native->task, SIGF_SINGLE);
    }
}

void pcNativeViewportColor4(struct ViewPort* viewport, UWORD index, UWORD color, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    if (viewport) {
        pcUpdateNativeViewportColor4(viewport, index, color, graphics);
    } else {
        struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;

        // The legacy global path addresses 32 colors in the current bank.
        // It changes current output state, never a ColorMap or a RAM list.
        // Values beyond that range address non-color chipset registers on
        // Amiga hardware and are not palette operations on this backend.
        if (native && index < 32) {
            UWORD control = native->controls.display;
            unsigned slot = (control >> 13) * 32 + index;
            ULONG value = pcApplyRGB4Color(native->colors.colors[slot], color, (control & 0x200) != 0);
            native->colors.colors[slot] = value;
            const struct ActiveViewFields* active = pcActiveViewFields(graphics);

            if (!slot && active->owned && !active->enabled) {
                presentBlank(native, active->background.flags & BORDER_BLANKING ? 0 : value);
            }
        }
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

static ULONG nativeFrame(struct NativeGraphicsDisplay* native asm("a1"))
{
    pcGraphicsFrame(native->graphics);
    pcWakeNativeDisplay(native->graphics);

    return 0;
}

static void displayTask(void)
{
    struct ExecBase* SysBase = *(struct ExecBase**)4UL;
    struct Task* task = FindTask(NULL);
    struct NativeGraphicsDisplay* native = task->tc_UserData;

    for (;;) {
        Wait(SIGF_SINGLE);
        Forbid();
        struct Task* stopping = native->stopping;

        if (stopping) {
            // Park after acknowledging. The caller removes the task before
            // releasing its stack; no rendering or semaphore is held here.
            Signal(stopping, native->stopped_signal);

            for (;;) {
                Wait(0);
            }
        }

        Permit();
        pcRefreshNativeDisplay(native->graphics);
    }
}

BOOL pcOpenNativeDisplay(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;

    if (library->native) {
        return TRUE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct NativeGraphicsDisplay* native = AllocMem(sizeof(*native), MEMF_PUBLIC | MEMF_CLEAR);

    if (!native) {
        return FALSE;
    }

    if (!pcOpenGraphicsFramebuffer(&native->output, SysBase)) {
        FreeMem(native, sizeof(*native));
        return FALSE;
    }

    native->stack = AllocMem(DISPLAY_STACK_SIZE, MEMF_PUBLIC);

    if (!native->stack) {
        pcCloseGraphicsFramebuffer(&native->output, SysBase);
        FreeMem(native, sizeof(*native));
        return FALSE;
    }

    native->graphics = graphics;
    struct Task* task = &native->task;
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Name = "graphics.library display";
    task->tc_UserData = native;
    task->tc_SPLower = native->stack;
    task->tc_SPUpper = native->stack + DISPLAY_STACK_SIZE;
    task->tc_SPReg = task->tc_SPUpper;
    task->tc_MemEntry.lh_Head = (APTR)&task->tc_MemEntry.lh_Tail;
    task->tc_MemEntry.lh_TailPred = (APTR)&task->tc_MemEntry.lh_Head;
    native->frame.is_Node.ln_Type = NT_INTERRUPT;
    native->frame.is_Node.ln_Name = "graphics.library frame";
    native->frame.is_Data = native;
    native->frame.is_Code = (APTR)nativeFrame;
    Forbid();
    if (!AddTask(task, displayTask, NULL)) {
        Permit();
        FreeMem(native->stack, DISPLAY_STACK_SIZE);
        pcCloseGraphicsFramebuffer(&native->output, SysBase);
        FreeMem(native, sizeof(*native));
        return FALSE;
    }

    library->native = native;
    AddIntServer(INTB_VERTB, &native->frame);
    Permit();

    return TRUE;
}

BOOL pcCloseNativeDisplay(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;
    struct NativeGraphicsDisplay* native = library->native;

    if (!native) {
        return TRUE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (FindTask(NULL) == &native->task) {
        return FALSE;
    }

    BYTE signal = AllocSignal(-1);

    if (signal < 0) {
        return FALSE;
    }

    Forbid();
    native->stopped_signal = 1UL << signal;
    SetSignal(0, native->stopped_signal);
    native->stopping = FindTask(NULL);
    RemIntServer(INTB_VERTB, &native->frame);
    Signal(&native->task, SIGF_SINGLE);
    Wait(native->stopped_signal);
    RemTask(&native->task);
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    library->native = NULL;
    releaseDisplay(native, graphics);
    completeBitmapReplies(native, NULL, 0, TRUE);
    pcCloseGraphicsFramebuffer(&native->output, SysBase);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
    FreeMem(native->stack, DISPLAY_STACK_SIZE);
    FreeMem(native, sizeof(*native));
    FreeSignal(signal);
    Permit();

    return TRUE;
}

static LONG programMonitor(struct MonitorSpec* monitor)
{
    // This backend retains the firmware mode. Monitor geometry is consumed by
    // the next serialized refresh, not translated into chipset register writes.
    pcWakeNativeDisplay(monitor->ms_Node.xln_Library);

    return 0;
}

static LONG initMonitor(struct ExtendedNode* node asm("a0"), UWORD flags asm("d0"),
    struct GfxBase* graphics asm("a6"))
{
    struct MonitorSpec* monitor = (APTR)node;
    UWORD result = pcInitMonitorSpec(monitor, flags, programMonitor, graphics);
    monitor->ms_KillView = (APTR)pcKillNativeView;

    return result;
}

static LONG loadView(struct View* view, struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    if (!pcLoadViewState(view, graphics)) {
        ReleaseSemaphore(graphics->ActiViewCprSemaphore);
        return 0;
    }

    struct NativeGraphicsDisplay* native = ((struct GraphicsLibrary*)graphics)->native;

    if (native && !pcActiveViewFields(graphics)->enabled) {
        // The empty display retains its initial background, including border
        // blanking. Present it synchronously: a delayed blank frame must never
        // overwrite a subsequent owner after LoadView returns.
        releaseDisplay(native, graphics);
        const struct ViewBackground* background = &pcActiveViewFields(graphics)->background;
        ULONG color = background->flags & BORDER_BLANKING ? 0 : background->color;
        native->colors.colors[0] = background->color;
        native->controls.display &= ~0xe200;
        native->blank = FALSE;
        presentBlank(native, color);
    } else {
        pcWakeNativeDisplay(graphics);
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return 1;
}

const struct GraphicsNodeMethods pcNativeNodeMethods = {
    initMonitor, programMonitor, pcMergeNativeView, loadView
};

LONG pcLoadView(struct View* view asm("a1"), struct GfxBase* graphics asm("a6"))
{
    return pcDispatchLoadView(view, (MonitorViewFunction)loadView, graphics);
}

APTR pcGfxNew(ULONG type asm("d0"), struct GfxBase* graphics asm("a6"))
{
    return pcAllocGraphicsNode(type, &pcNativeNodeMethods, graphics);
}
