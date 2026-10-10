/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/sprite.h>
#include <utility/tagitem.h>

struct ViewPort;
struct GraphicsFramebufferSprite;
struct GraphicsFramebufferSprites;

// Driver slot 0 owns sprite movement. A NULL viewport selects the first
// active viewport's driver but remains NULL in the callback's A0. The native
// method is required when that route has no table. Dispatch itself neither
// locks native state nor interprets a foreign driver's private storage.
typedef void (*ViewPortSpriteFunction)(struct ViewPort* viewport asm("a0"),
    struct SimpleSprite* sprite asm("a1"), LONG x asm("d0"), LONG y asm("d1"),
    struct GfxBase* graphics asm("a6"));
void pcDispatchMoveSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, LONG x, LONG y,
    ViewPortSpriteFunction default_method, struct GfxBase* graphics);
void pcDispatchChangeSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, UWORD* data,
    ViewPortSpriteFunction default_method, struct GfxBase* graphics);

void pcMoveSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    LONG x asm("d0"), LONG y asm("d1"), struct GfxBase* graphics asm("a6"));
void pcChangeSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    UWORD* data asm("a2"), struct GfxBase* graphics asm("a6"));

// Native slot 0 serializes image changes with field presentation. Attached
// pairs are updated inside the same exclusion, without redispatching.
void pcNativeMoveSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    LONG x asm("d0"), LONG y asm("d1"), struct GfxBase* graphics asm("a6"));

// Update the native sprite's public image and pointer-list state. The native
// display owner must serialize this with presentation; this operation performs
// no chipset access and does not activate or render a sprite by itself.
void pcUpdateNativeSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, LONG x, LONG y,
    struct GfxBase* graphics);

struct NativeSpriteSource {
    const UWORD* image;
    UWORD position, control;
};

struct NativeSpriteSources {
    struct NativeSpriteSource channels[8];
    UWORD word_width;
    BOOL scan_double;
};

// Capture the images selected by the native pointer records, independently of
// SimpleSprites registration or later edits to application descriptors. Copy
// their position/control words, global width and monitor scan-doubling mode;
// pixel storage remains borrowed. A NULL image
// has zero controls; the library's blank image retains its actual identity.
// The native display owner holds its semaphore through capture and consumption
// and supplies valid selected image storage. This reads ordinary library RAM,
// not hardware, and does not execute a display list or activate presentation.
// The displayed control word follows one word-width group; for wide images it
// is separate from the compatibility copy in word 1 used by MoveSprite.
// Missing output/base/copinit or invalid width rejects without modifying output.
BOOL pcReadNativeSpriteSources(struct NativeSpriteSources* sources, const struct GfxBase* graphics);

struct NativeSpriteGeometry {
    const UWORD* data;
    UWORD width, row_bytes;
    UWORD x, start, stop;
    BOOL scan_doubled, attached;
};

// Decode the captured public image format without consulting live descriptors.
// x is in quarter-LORES units; start/stop are absolute native line coordinates.
// Preserve both vertical endpoints: monitor/frame clipping must precede deriving
// a readable row count, especially when stop precedes start. Scan doubling uses
// the selected image's enable bit, not the current SimpleSprite registration.
// data skips the complete width-dependent header; no pixel storage is read.
// NULL images produce empty geometry. Invalid inputs leave output untouched.
BOOL pcReadNativeSpriteGeometry(struct NativeSpriteGeometry* geometry,
    const struct NativeSpriteSources* sources, unsigned channel);

struct NativeSpriteInterval {
    UWORD first, limit; // Half-open native-line interval; equal means no pixels.
    UWORD next; // Earliest line for the following image descriptor.
    BOOL reload;
};

// Resolve one captured descriptor within the remaining field, before viewport
// clipping. first_line is the next eligible image line, not a display-window
// edge; limit_line is the exclusive field end. Neither endpoint wraps here.
// A stop encountered before/equal to start yields no pixels but still selects
// the following descriptor. A stop before first_line cannot end this image.
// After a doubled image starts, a stop on its repeated-row parity is skipped.
// The owner must follow reload even for an empty interval, retaining source
// ownership; this operation neither reads subsequent descriptors nor pixels.
// Row duplication, readable byte spans and display clipping are separate.
// Missing inputs or coordinates outside the ten-bit field reject untouched.
BOOL pcResolveNativeSpriteInterval(struct NativeSpriteInterval* interval,
    const struct NativeSpriteGeometry* geometry, UWORD first_line, UWORD limit_line);

struct NativeSpriteImage {
    struct NativeSpriteGeometry geometry;
    struct NativeSpriteInterval interval;
    UWORD data_first; // First native line using a new row from geometry.data.
    UWORD rows; // Stored two-plane rows, before any display-window clipping.
};

struct NativeSpriteCursor {
    struct NativeSpriteSource source;
    UWORD word_width, channel;
    UWORD first_line, limit_line;
    BOOL scan_double, pending;
};

// Walk one channel's sequential image descriptors within an explicit field.
// The first header comes from the capture; following headers remain borrowed
// and are read only on the next call. Keep source ownership/serialization for
// the entire walk. The caller supplies readable storage for the selected field,
// including any following headers; SimpleSprite.height is not a storage bound.
BOOL pcInitNativeSpriteCursor(struct NativeSpriteCursor* cursor,
    const struct NativeSpriteSources* sources, unsigned channel, UWORD first_line, UWORD limit_line);

// Return 1 for a descriptor (including one with no displayed rows), 0 at field
// end, or -1 for invalid arguments. End/error leaves both objects untouched.
// A scan-doubled image loaded on its start line can first repeat the channel's
// preceding pixel row. Lines before data_first use that retained row, not
// geometry.data; the display owner must preserve it across image/field changes.
// Progress is bounded by the field: a reload consumes at least one line.
// No pixel reads, rendering, clipping, allocation or display activation occur.
LONG pcNextNativeSpriteImage(struct NativeSpriteCursor* cursor, struct NativeSpriteImage* image);

// Resolve one channel for composition on a SHRES horizontal/native-field-line
// grid. Origins are in that same grid, not application ViewPort coordinates;
// subtract them before clipping so fractional LORES positions survive.
// resolution is the resolved sprite pixel scale: 0 LORES, 1 HIRES, 2 SHRES.
// A visible repeated row requires preceding_row, retained by the display owner
// at the selected width. Fresh rows remain borrowed from the image. The owner
// must retain channel data even outside the clipped display region.
// Hidden lines return an empty sprite. Invalid/missing visible storage leaves
// output untouched. No pixel reads, ownership changes or presentation occur.
BOOL pcPlaceNativeSpriteRow(struct GraphicsFramebufferSprite* sprite,
    const struct NativeSpriteImage* image, const UWORD* preceding_row, UWORD line,
    WORD origin_x, WORD origin_y, UBYTE resolution);

// Retained pixel words independent of the selected image width. Each channel
// stores two four-word planes. A narrow fetch fills each plane's unused words
// with its first word; changing width alone does not alter the retained data.
struct NativeSpriteRows {
    UWORD words[8][8];
};

struct NativeSpriteField {
    struct NativeSpriteCursor cursors[8];
    struct NativeSpriteImage images[8];
    struct NativeSpriteRows retained;
    UWORD rows[8][8]; // Compact two-plane rows at the current field's width.
    UWORD line, limit;
};

// Begin a sequential field from captured sources and known preceding rows.
// retained may be &field->retained to carry rows between fields of any width.
// Other inputs must not alias field. Hold the display semaphore and keep the
// complete selected image chains readable throughout traversal. No pixel rows
// are read until their field line; the initial controls were already captured.
BOOL pcInitNativeSpriteField(struct NativeSpriteField* field, const struct NativeSpriteSources* sources,
    const struct NativeSpriteRows* retained, UWORD first_line, UWORD limit_line);

// Select a complete DrawGList descriptor at the field's next unfetched line.
// data addresses pixels directly; position/control come from the RAM list.
// Unlike a SimpleSprite image, no following header is read at the stop line.
// Keep retained rows and other channels intact. The scheduler holds source
// ownership and calls this before rendering the interval following the entry.
BOOL pcSelectNativeGelImage(struct NativeSpriteField* field, unsigned channel,
    const UWORD* data, UWORD position, UWORD control);

// Assemble all eight channels on the next native line, including offscreen
// fetches. Copy freshly consumed pixels; repeats use owned retained words.
// Following controls take effect on their load line, including attachment
// while the odd channel is invisible. Preserve the caller's color banks.
// Output row pointers belong to field and last only until its next advance.
// Return 1 for a line, 0 at end, -1 for invalid arguments; end/error preserves
// state/output. The field must have been initialized and not edited by callers.
// This supplies the existing compositor; it performs no clipping, palette
// selection, monitor activation or presentation. Origins/resolution use the
// same staging grid as pcPlaceNativeSpriteRow.
LONG pcNextNativeSpriteLine(struct NativeSpriteField* field, struct GraphicsFramebufferSprites* sprites,
    WORD origin_x, WORD origin_y, UBYTE resolution);

struct ExtSprite* pcAllocSpriteDataA(const struct BitMap* bitmap asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
void pcFreeSpriteData(struct ExtSprite* sprite asm("a2"), struct GfxBase* graphics asm("a6"));

LONG pcGetSprite(struct SimpleSprite* sprite asm("a0"), LONG number asm("d0"),
    struct GfxBase* graphics asm("a6"));
void pcFreeSprite(LONG number asm("d0"), struct GfxBase* graphics asm("a6"));
LONG pcGetExtSpriteA(struct ExtSprite* sprite asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
LONG pcChangeExtSpriteA(struct ViewPort* viewport asm("a0"), struct ExtSprite* old asm("a1"),
    struct ExtSprite* sprite asm("a2"), const struct TagItem* tags asm("a3"), struct GfxBase* graphics asm("a6"));
