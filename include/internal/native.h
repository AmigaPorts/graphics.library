/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "internal/backend.h"
#if GRAPHICS_CHIPSET
#include "internal/chipset.h"
#endif
#include "internal/view.h"
#include "internal/sprite.h"
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <devices/timer.h>

struct NativeBitmapReply {
    struct NativeBitmapReply* next;
    struct DBufInfo* info;
    struct ViewDisplaySnapshot* display[2];
    UBYTE fields[2];
    ULONG generation;
    ULONG frame;
    ULONG minimum_frames;
    BOOL safe;
    // Malformed field streams postpone discovery, never imply inactivity.
    // Saved output identities are compared only; no caller storage is read.
    BOOL unresolved;
    APTR targets[2];
};

// AllocDBufInfo owns this storage. Keep the public prefix unchanged; its
// private tail follows the same lifetime, without allocating during a switch.
// Both messages must have returned before the caller frees the DBufInfo.
struct GraphicsDBufInfo {
    struct DBufInfo info;
    struct NativeBitmapReply reply;
    struct NativeBitmapReply safe_reply;
    struct BitMap previous;
    ULONG viewport_identity;
};

ULONG pcSetChipRev(ULONG requested asm("d0"), struct GfxBase* graphics asm("a6"));

// CPU bitmap operations complete before returning. The chipset backend waits
// for the real blitter, independently of the shared ownership queue.
void pcWaitBlit(struct GfxBase* graphics asm("a6"));

// Internal drawing keeps the public wait behavior without a dispatch call.
static inline void pcWaitDrawing(struct GfxBase* graphics)
{
#if GRAPHICS_CHIPSET

    if (pcUsesChipset(graphics)) {
        pcWaitChipsetBlit();
        return;
    }
#endif
    (void)graphics;
    // Native bitmap writes are synchronous: there is no submitted transfer
    // left to poll after a drawing call returns. Keep the compiler boundary
    // for callers that reuse or release those buffers immediately afterward.
    // Do not wait for BlitLock, drain queued callbacks, or wait for display
    // presentation: none of those is the active-transfer contract of WaitBlit.
    // Queued chipset callbacks remain unavailable on this backend. A future
    // asynchronous backend must supply its own completion implementation.
    __asm__ volatile("" : : : "memory");
}

// Logical scan position within the native software frame. The firmware
// framebuffer does not expose the monitor's physical scanout position.
ULONG pcVBeamPos(struct GfxBase* graphics asm("a6"));

// Persistent native presentation state. The display semaphore protects every
// field below except the interrupt/task notification, frame clock and shutdown
// handshake. Frame-clock snapshots use interrupt exclusion instead.
struct NativeGraphicsDisplay {
    struct GraphicsFramebuffer output;
    struct ViewFieldBuffers* buffers[2];
    struct ViewDisplaySnapshot* display;
    struct GraphicsFramebufferPalette colors;
    struct NativeSpriteRows sprite_rows;
    struct ViewFieldControls controls;
    struct Rectangle crop;
    ULONG generation;
    unsigned source;
    BOOL short_field;
    BOOL clear_output;
    BOOL blank;
    ULONG blank_color;
    struct Task task;
    struct Interrupt frame;
    struct timerequest timer;
    struct EClockVal frame_start;
    ULONG frame_ticks;
    struct GfxBase* graphics;
    struct Task* starting;
    ULONG started_signal;
    ULONG frame_signal;
    struct Task* stopping;
    ULONG stopped_signal;
    UBYTE* stack;
    struct NativeBitmapReply* replies;
};

// Open during serialized library initialization. Close from a different task,
// without holding the display semaphore, and before freeing graphics state.
// Normal graphics.library Expunge keeps this system-lifetime owner installed.
BOOL pcOpenNativeDisplay(struct GfxBase* graphics);
BOOL pcCloseNativeDisplay(struct GfxBase* graphics);
BOOL pcRefreshNativeDisplay(struct GfxBase* graphics);
void pcWakeNativeDisplay(struct GfxBase* graphics);

// Borrow the fields reachable from the next native refresh. Follow the same
// scheduled links and restart rules as rendering, including retained partners
// and two independently selected merge generations. Hold ActiViewCprSemaphore
// until the borrowed snapshots are no longer needed. No allocation, rendering,
// reference changes or public-state changes. Empty/foreign ownership succeeds
// with empty masks; invalid field/control descriptions leave result untouched.
struct NativeViewFields {
    struct ViewDisplaySnapshot* display[2];
    UBYTE fields[2];
};

BOOL pcReadNativeViewFields(struct NativeViewFields* fields, struct GfxBase* graphics);

// Classify reachable viewport setups selected by DBufInfo's cached output
// identities. The updated set selects live bindings eligible for relocation;
// retained sources have detached and will not receive that relocation. This
// query performs no writes and does not select the switch's plane masks.
// Only compare captured identities after retirement; never dereference them.
// This selects cache targets, not every alias of an application's plane memory.
// The same locking, borrowing and failure rules as pcReadNativeViewFields apply.
struct NativeBitmapFields {
    struct NativeViewFields updated;
    struct NativeViewFields retained;
};

BOOL pcReadNativeBitmapFields(struct NativeBitmapFields* fields, const struct DBufInfo* info,
    struct GfxBase* graphics);

// Also find older generations of this viewport still reading the previous
// bitmap outside the cached live targets. The retained set includes those
// readers even when neither cached output address selects them. Caller holds
// the display semaphore before relocating the live targets. Descriptor/plane
// storage is inspected only during this query and is never retained.
BOOL pcReadNativeBitmapSwitch(struct NativeBitmapFields* fields, const struct ViewPort* viewport,
    const struct BitMap* previous, const struct DBufInfo* info, struct GfxBase* graphics);

// Gate an AllocDBufInfo display reply on actual output completion. The caller
// keeps info until its messages return, selects up to two retained generations and
// supplies field masks (bits 0/1), after serializing source replacement with
// ActiViewCprSemaphore. dbi_Count2 supplies the original minimum frame delay.
// An idle request and an unqueued DBufInfo display message are required. This
// allocates nothing and retains only the display snapshots, not bitmap headers.
// FALSE leaves the allocation untouched: no native owner, no requested message
// or no selected reachable fields. Invalid reader descriptions also fail
// without enqueueing; they do not establish that the output is inactive.
// The caller handles non-native/hidden frame replies.
// A View replacement, handoff or valid restart which no longer reaches a
// selected source removes that outstanding field requirement, but
// keeps the frame delay. Final owner shutdown drains replies after stopping
// the rendering task, when no further native frame can occur.
BOOL pcQueueNativeBitmapReply(struct DBufInfo* info,
    struct ViewDisplaySnapshot* first, UBYTE first_fields,
    struct ViewDisplaySnapshot* second, UBYTE second_fields, struct GfxBase* graphics);

// Keep SafeMessage pending while any selected old source remains reachable.
// A successful copy does not retire an old source that can be read again.
// Unlike a display reply, changing the View generation alone is insufficient:
// the newly loaded View may still select that same captured raster. The caller
// identifies the old sources before switching, under ActiViewCprSemaphore.
// All queue/lifetime rules above apply; dbi_Count1 supplies the frame delay.
// Capture the old bitmap's allocation description so subsequent source changes
// can retire readers without removing the field. Neither caller header is
// retained. NULL for both descriptors requests conservative field retirement
// only, for a source whose storage description is unavailable.
BOOL pcQueueNativeBitmapSafeReply(struct DBufInfo* info,
    const struct ViewPort* viewport, const struct BitMap* previous,
    struct ViewDisplaySnapshot* first, UBYTE first_fields,
    struct ViewDisplaySnapshot* second, UBYTE second_fields, struct GfxBase* graphics);
void pcNativeViewportColor4(struct ViewPort* viewport, UWORD index, UWORD color, struct GfxBase* graphics);
extern const struct GraphicsNodeMethods pcNativeNodeMethods;
