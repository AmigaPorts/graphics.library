/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/view.h>

struct GfxBase;
struct MonitorSpec;
struct ViewFieldTiming;

// Shared RAM-list allocation and continuation used by user lists and GELs.
// Bump consumes the record immediately before following, and may move it to
// a continuation buffer. The caller publishes the final cursor in CopPtr.
struct CopList* pcInitCopList(struct CopList* list, LONG count);
struct CopIns* pcBumpCopList(struct CopList** list, struct CopIns* following);

// Merge the View's intermediate lists into owned long/short output lists,
// preserving source cursors, output starts and monitor metadata. Inputs have
// valid terminating WAITs and continuation links; the caller serializes list
// access. This is the RAM-list owner; public monitor dispatch and native display
// activation remain separate.
LONG pcMergeViewLists(struct View* view, struct GfxBase* graphics);

// Apply the original mixed-mode admission and saved A2024 state before list
// collection. The caller has resolved the View's monitor. Hidden ViewPorts and
// absent display lists do not participate. Return the merge's vertical origin;
// the View's own origin remains unchanged. This updates RAM state only.
WORD pcPrepareViewModes(struct View* view, const struct MonitorSpec* monitor, struct GfxBase* graphics);

// Collect visible ViewPort streams in original display/sprite/color/user order,
// prepending each nonempty stream to *first. Initialize all continuation buffers
// and mark clipped user instructions; hidden/absent display lists skip the whole
// ViewPort. Return the original wrapping word count, before output overhead.
// The owner resolves monitor/mode compatibility and serializes list access.
// This only prepares RAM lists, with no allocation or display activation.
UWORD pcCollectViewLists(struct View* view, struct CopList** first);

// Prepare output storage after collecting at least one input stream. count is
// the wrapping input count before the original ten-record allowance. Preserve
// existing list identity, allocation flags and failure ownership; detach/free
// trailing output chains only after both fields are ready. This neither emits
// records nor changes the active display. Return MCOP_OK or MCOP_NO_MEM.
LONG pcAllocViewLists(struct View* view, UWORD count);

// Select the next wait from the merger's _CopList chain. Each participating
// list has a current WAIT in CopPtr and its owner in _ViewPort; MOVE runs and
// CPRNXTBUF transitions must already have been consumed. Compare effective
// ViewPort coordinates, retaining field-specific precedence on equal waits.
// This does not advance cursors, emit instructions or access display hardware.
struct CopList* pcSelectViewWait(struct CopList* first);

typedef void (*ViewMergeMoveFunction)(struct CopList* list, UWORD destination,
    UWORD data, UWORD flags, APTR context);

// Consume each active list's leading MOVE run and follow its continuation
// buffers, leaving current waits ready for pcSelectViewWait. The owner first
// initializes every buffer's CopPtr, _ViewPort and _CopList. Buffers and links
// must remain valid throughout the merge; a continuation has a non-NULL target.
// The required sink receives normalized destinations and original flags in
// merge order. It owns field filtering and output storage. User-list clipping
// after a selected wait belongs to the caller, before resuming this traversal.
// All writes affect RAM cursors/links; this never programs display hardware.
void pcAdvanceViewWaits(struct CopList** first, ViewMergeMoveFunction move, APTR context);

// After emitting the selected WAIT, advance its cursor and skip the marked
// user run if that wait lies below the ViewPort. Stop at an unmarked command,
// terminator or unknown opcode; follow continuation buffers without relinking
// the active stream chain. Return the last buffer reached (NULL for a null
// continuation), which the merger retains as its previous selected buffer.
// The next pcAdvanceViewWaits call updates active links. No commands are emitted
// or changed here, and caller-owned list storage must remain valid throughout.
struct CopList* pcAdvanceViewWait(struct CopList* list);

// Resolve the current WAIT's output line with the original WORD arithmetic.
// This differs from sorting: interlaced emission rounds the line upward before
// halving it. Apply the View origin and monitor limit after mode conversion.
// First-wait minimum, field selection and encoded output belong to the emitter.
WORD pcViewWaitLine(const struct CopList* list, WORD view_y,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics);

struct ViewWaitOutput {
    Point previous[2];
    // Absolute coordinates of the last record actually written to each field.
    // Unlike previous[], excluded/suppressed writes leave these unchanged.
    Point emitted[2];
    UWORD* cursor[2];
    BOOL interlaced;
    // Optional native scheduling capture, reserved before output allocation.
    // Non-native merging leaves these NULL and emits identical public lists.
    // For capture, set start to each output's first word and initialize count
    // to zero with capacity reserved in waits. The owner sets words after finish.
    struct ViewFieldTiming* timing[2];
    const UWORD* start[2];
    BOOL timing_overflow;
};

struct ViewWaitSequence {
    struct ViewWaitOutput output;
    const UWORD* long_start;
    LONG first_wait;
    BOOL wrapped[2];
};

// Emit the current source WAIT, including vertical wraps and field history.
// Initialize first_wait to -1, both histories/wrapped flags to zero, and the
// output cursors/start to allocated RAM lists. The owner reserves capacity for
// every crossed 256-line boundary in both fields and supplies effective
// ViewPortExtra straddle flags and the previous source's exact-line condition.
// No source instruction, hardware register or public display owner is changed.
void pcEmitViewWait(struct ViewWaitSequence* sequence, const struct CopList* list,
    WORD view_y, UWORD viewport_flags, BOOL exact_line,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics);

// Emit a resolved WAIT record with field history and duplicate suppression.
// The owner initializes both histories to zero and reserves two output words
// per field for each call. First-wait minimum and line-wrap sequencing belong
// to the surrounding emitter; this primitive handles lines within 0..255.
// Keep unmasked horizontal positions in history, even though encoding uses
// only even low-byte positions. A horizontal value of -1 emits nothing.
// The output is caller-owned RAM, never display hardware.
void pcEmitViewWaitRecord(struct ViewWaitOutput* output, WORD line, WORD horizontal,
    UWORD flags, const struct GfxBase* graphics);

// Finish an interlaced pair with the original opposite-field address records.
// Each cursor must follow at least one emitted record and have four free words.
// Preserve the final long-field record at the end of both lists, replacing each
// old final record with the link. Advance both cursors past the finished output.
// The starts are encoded addresses only; noninterlaced output is unchanged.
void pcFinishViewFields(struct ViewWaitOutput* output, const UWORD* long_start, const UWORD* short_start);

// Resolve the horizontal position of a vertical-wrap WAIT from the long-field
// records already emitted. line is the absolute output line; relative_line is
// the remaining line within the current wrap sequence. previous_line is the
// saved long-field position on entry to that sequence. exact_line preserves
// the previous source list's EXACT_LINE boundary rule. The range contains a
// preceding WAIT when previous_line ends in 255. Return -1 when the existing
// records leave no room for another wrap WAIT. This reads RAM only.
WORD pcViewWrapPosition(const UWORD* start, const UWORD* end, WORD line,
    WORD relative_line, WORD previous_line, BOOL exact_line,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics);

// Original allocation extent, including the four legacy copper pointers omitted
// by the modern SDK unless V1_3 is defined. This is not sizeof(struct CopList).
#define COPLIST_STORAGE_SIZE 54

// Build the banked RGB8 palette prefix of DspIns in the public intermediate
// list format. The caller supplies the selected colors and display/sprite
// control words. Count identifies the append position; CopPtr stays at the
// beginning, as in the original preparation method. Final display instructions
// and termination belong to the following build stage, not this prefix.
// The selected entries must have readable ColorMap storage. With no map,
// selection is limited to the first 32 defaults. No hardware writes occur.
UWORD pcBuildColorList(struct ViewPort* viewport, const ULONG* mask,
    UWORD display_control, UWORD sprite_control);

// Update the prepared palette records, including both merged field streams.
// These are RAM lists in the public Amiga format, never hardware addresses.
// Each non-NULL stream must contain its mandatory color-zero prefix and an
// end record. high/low are already selected by the native display owner;
// color_mask preserves or removes transparency as requested by the ColorMap.
// Call under ActiViewCprSemaphore. The owner handles hidden/unprepared Views,
// palette precision and presentation separately. CMAB_NO_INTERMED_UPDATE
// suppresses only the intermediate list, not either merged field.
void pcUpdateColorLists(struct CopList* list, const UWORD* high, const UWORD* low,
    UWORD color_mask, BOOL update_intermediate);

// Native RAM-list update plus synchronization of saved merged palettes under
// the display semaphore. Table selection, hidden-View rules and global colors
// still belong to the palette driver; this is not its complete public method.
void pcUpdateNativeColorLists(struct CopList* list, const UWORD* high, const UWORD* low,
    UWORD color_mask, BOOL update_intermediate, struct GfxBase* graphics);

// Legacy RGB4 updates are not RGB32 palette-prefix updates: visit long then
// short, replace every matching MOVE before the next merged WAIT, and follow
// intermediate continuation targets until a WAIT with horizontal position 255.
// Preserve the upper four data bits. Each stream must have its normal readable
// terminator. The owner serializes this operation; no lock or background update
// is performed here. destination is the wrapped WORD in the public RAM format.
void pcUpdateColor4Lists(struct CopList* list, UWORD destination, UWORD color);

// Finish a prepared display list with room for its terminator. first_line is
// the resolved unsigned display line, after interlace coordinate conversion;
// modes retains the effective ViewPort LACE/DOUBLESCAN bits. The first entry
// is the palette wait installed by pcBuildColorList. CalcIVG remains a public
// call so the installed driver owns gap calculation and list timing flags.
UWORD pcFinishViewPortList(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, UWORD first_line, ULONG modes, struct GfxBase* graphics);

struct CopList* pcUCopperListInit(struct UCopList* list asm("a0"), WORD count asm("d0"));
LONG pcCMove(struct UCopList* list asm("a1"), APTR destination asm("d0"), WORD data asm("d1"));
LONG pcCWait(struct UCopList* list asm("a1"), WORD vertical asm("d0"), WORD horizontal asm("d1"));
void pcCBump(struct UCopList* list asm("a1"));

void pcFreeCopList(struct CopList* list asm("a0"), struct GfxBase* graphics asm("a6"));
void pcFreeCprListStorage(struct cprlist* list asm("a0"));
// Public cleanup also detaches native display associations before freeing any
// output-list address. Retained active readers keep their captured generations.
void pcFreeCprList(struct cprlist* list asm("a0"), struct GfxBase* graphics asm("a6"));
void pcFreeVPortCopLists(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"));

// The public UWORD result is zero-extended in D0 by the original vector.
ULONG pcCalcIVG(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"));
