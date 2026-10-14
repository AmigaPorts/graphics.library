/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "internal/framebuffer.h"

#include <graphics/gfxbase.h>
#include <graphics/view.h>

typedef LONG (*MonitorViewFunction)(struct View* view);

LONG pcLoadView(struct View* view asm("a1"), struct GfxBase* graphics asm("a6"));
LONG pcMrgCop(struct View* view asm("a1"), struct GfxBase* graphics asm("a6"));
ULONG pcMakeVPort(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"), ULONG caller_d6 asm("d6"));
void pcScrollVPort(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"),
    ULONG caller_d3 asm("d3"), ULONG caller_d5 asm("d5"));
void pcChangeVPBitMap(struct ViewPort* viewport asm("a0"), struct BitMap* bitmap asm("a1"),
    struct DBufInfo* info asm("a2"), struct GfxBase* graphics asm("a6"));

// Shared monitor dispatch for the display owner. The required default method
// implements native merge/load behavior; it is not a no-op fallback. These
// dispatchers preserve monitor ownership for the public display vectors.
LONG pcDispatchLoadView(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics);
LONG pcDispatchMrgCop(struct View* view, MonitorViewFunction default_method, struct GfxBase* graphics);

// Driver slot 2 owns scrolling. Preserve A0/A6 and the method address in A1
// at entry, and forward D3 for the original short-field address rule and D5
// for the chipset fetch phase. These are retained caller registers, not new
// public parameters.
// The caller supplies the native method for absent/legacy maps or
// an absent extra/table; a selected driver's slot is never replaced here.
typedef void (*ViewPortScrollFunction)(struct ViewPort* viewport asm("a0"),
    struct GfxBase* graphics asm("a6"));
void pcDispatchScrollVPort(struct ViewPort* viewport, ViewPortScrollFunction default_method,
    struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5);

// Driver table slot 3 points to a NULL-terminated preparation sequence. The
// first method takes these five stack arguments; subsequent methods take the
// first two plus the resolved ViewPortExtra. A6 remains the graphics base.
typedef ULONG (*ViewPortBuildInitFunction)(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra** extra, APTR data, APTR work, struct GfxBase* graphics asm("a6"));
typedef ULONG (*ViewPortBuildFunction)(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));

// Complete native preparation sequence for driver slot 3. The display owner
// supplies ViewPortBuildMode through DTAG_VEC.Data and holds its semaphore.
// The five-entry native table below selects this sequence for both playfields.
extern const APTR pcNativeViewPortBuildMethods[];
extern const APTR pcNativeViewPortVectors[5];

void pcNativeScrollVPort(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"),
    ULONG caller_d3 asm("d3"), ULONG caller_d5 asm("d5"));

// Preparation methods receive this much WORD-aligned temporary storage. Each
// driver owns its contents; native framebuffer state does not encode chipset
// register values. DriverData[0] is valid only during the preparation sequence.
#define VIEWPORT_BUILD_WORK_SIZE 82

struct PreparedViewPorts;

// Native DTAG_VEC data describes coordinate scaling and its display owner,
// not chipset registers.
// horizontal_shift is the mode's pixel scale (0, 1 or 2).
// ehb_precision describes the display's color precision, independently of
// ColorMap.Type. ham8_planar selects application-plane order for seven/eight
// plane HAM; it has no effect on HAM6 or non-HAM modes.
struct ViewPortBuildMode {
    UBYTE horizontal_shift;
    BOOL scan_doubled;
    BOOL extend_to_monitor_bottom;
    struct PreparedViewPorts* prepared;
    enum GraphicsEhbPrecision ehb_precision;
    BOOL ham8_planar;
};

struct ViewPortBuildState {
    APTR mode_data;
    struct MonitorSpec* monitor;
    ULONG modes;
    // Fetch mode is the validated byte returned by pcViewportFetchMode.
    UBYTE fetch_mode;
    UBYTE interlaced;
    struct Rectangle clip;
    WORD start_x, start_y;
    WORD stop_x, stop_y; // Exclusive display ends, unlike Rectangle maxima.
    ULONG color_mask[8];
    LONG source_y[2];
    LONG source_x[2];
    UWORD source_stride[2];
};

ULONG pcInitViewPortBuild(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra** extra, APTR data, APTR work, struct GfxBase* graphics asm("a6"));
ULONG pcBuildViewPortGeometry(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));
ULONG pcBuildViewPortColors(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));

// Resolve each raster's source origin after ViewPort clipping. The
// display owner clips the resolved signed coordinates to the bitmap separately.
// The original rounds the viewport origin before clipping, then narrows the
// scroll difference to WORD before negating it. View/monitor offsets are
// already accounted for by the prepared clip, not applied to the raster again.
// Horizontal wrapping depends on the mode's pixel resolution. Store the
// results in source_x/source_y and capture source_stride after geometry. Missing
// bitmap slots remain zero; a missing second RasInfo reuses the first raster.
ULONG pcBuildViewPortRasters(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));

struct ViewPortPlaneFetch {
    PLANEPTR plane;
    UWORD bytes_per_row;
    LONG source_x, source_y;
    LONG row_offset; // Byte adjustment for a stride changed after preparation.
};

struct ViewPortRasterState {
    struct BitMap bitmap[2];
    ULONG present;
    LONG source_x[2], source_y[2];
    BOOL interlaced; // Field lines advance two source rows when set.
    // Native scroll selection is in display-slot order. Retain the prepared
    // bitmap metadata for bounds and decoding, even when current sources mix
    // rasters with different strides and origins into those slots.
    BOOL plane_fetches;
    struct ViewPortPlaneFetch fetches[8];
};

// Resolve the eight current plane sources after scroll preparation. This is
// fetch selection, not a new bitmap depth, decoder or display allocation.
// modes is the effective viewport mode captured before callbacks. Origins
// come from preparation; current RasInfo/BitMap descriptors supply pointers
// and strides. source_stride contains the pitches used during preparation;
// retain that initial row displacement even if a callback changed the live
// pitches afterward. Later rows and the short-field advance use live strides.
// The caller holds the display semaphore and supplies valid
// rasters, as required after successful native preparation. No pointer to a
// caller-owned descriptor is retained and no plane memory is read.
void pcReadViewPortScrollFetches(struct ViewPortPlaneFetch* fetches, const struct ViewPort* viewport,
    UWORD modes, const LONG* source_x, const LONG* source_y, const UWORD* source_stride);

// Resolved native sprite addressing at preparation time. Resolution retains
// the public SPRITERESN_* selector: ECS still needs the selected display mode
// to determine its pixel scale. Banks contain 16-color bank numbers, not pen
// bases. border_sprites and border_blank retain separate settings; blanking
// takes precedence outside the display window. No application ColorMap pointer is
// retained. The caller holds the native display owner's semaphore.
struct ViewPortSpriteLayout {
    UBYTE even_bank, odd_bank;
    UBYTE resolution;
    BOOL border_sprites;
    BOOL border_blank;
};

void pcReadViewPortSpriteLayout(struct ViewPortSpriteLayout* layout, const struct ColorMap* map,
    const struct GfxBase* graphics);

// Encode the same palette addressing and sprite policy in public intermediate
// list records. Geometry and plane sources stay in the native snapshot. This
// preserves selectors through palette-bank/precision writes and permits live
// list updates without replacing non-default software settings with zeroes.
// No bank/precision selection is added here; pcBuildColorList supplies those.
void pcReadViewPortListControls(UWORD* display_control, UWORD* sprite_control,
    const struct ColorMap* map, const struct GfxBase* graphics);

// Apply captured color banks and sprite pixel resolution to a prepared group.
// horizontal_shift describes the destination grid: 0 LORES, 1 HIRES, 2 SHRES.
// ECS uses LORES sprites except on SHRES playfields, where it uses HIRES.
// modes is the captured effective playfield mode, not the destination grid.
// Image positions, row scaling, storage and attachment stay with their owner.
// The owner resolves coordinate origins/sampling phase before composition; use
// the SHRES grid to retain every horizontal position bit. No pixels are read.
// Invalid layout/grid/output rejects without changing the prepared group.
BOOL pcApplyViewPortSpriteLayout(struct GraphicsFramebufferSprites* sprites,
    const struct ViewPortSpriteLayout* layout, UWORD modes, UBYTE horizontal_shift);

// Compose a prepared sprite group with this viewport's captured addressing and
// the selected field's resolved palette. region and window use framebuffer
// coordinates with exclusive ends. The owner resolves their placement/grid;
// neither is a source-memory clipping rectangle. Border blanking suppresses
// border sprites even when enabled. It does not suppress sprites inside the
// display window. The caller fills the background and supplies playfield
// occlusion; this operation only overlays sprite pixels, preserving their
// sampling phase and the input group. Serialize through the display owner.
// Invalid inputs reject without writes. No allocation or presentation occurs.
BOOL pcRenderViewPortSprites(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const struct ViewPortSpriteLayout* layout,
    UWORD modes, UBYTE horizontal_shift, const struct GraphicsFramebufferPalette* palette,
    const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window);

struct NativeSpriteField;

// Sequential interval composition retains the fetched row across horizontal
// palette/priority boundaries. The field owns pixel rows and must outlive this
// state; never copy/move either object or advance the field independently.
struct ViewFieldSpriteRuns {
    struct NativeSpriteField* field;
    struct GraphicsFramebufferSprites row;
    Point position;
    WORD origin_x, origin_y;
    UWORD line;
    BOOL row_ready;
};

// Attach to an initialized field at its current line. Origins select the
// shared SHRES/field-line staging grid. No source pixels are fetched here.
// State must not overlap the field or its source storage. Failure preserves it.
BOOL pcInitViewFieldSpriteRuns(struct ViewFieldSpriteRuns* state, struct NativeSpriteField* field,
    WORD origin_x, WORD origin_y);

struct ViewFieldRun;

// Overlay [start,end) in absolute WAIT coordinates, retaining a fetched row
// across successive intervals. The caller resolves actual execution boundaries
// and terminal-WAIT reachability; MOVE records are not interpreted here. Apply
// the interval's palette/layout/occlusion before calling. Both horizontal
// endpoints must be even 0..254; lines must be within the attached field, with
// x=0 at its exclusive limit. Intervals may skip forward, never overlap/reverse.
// Skipped/offscreen lines still advance all channels and retain repeated data.
// An empty interval fetches/writes nothing. Region/window use staging pixels.
// Hold the display semaphore. No allocation, activation or presentation.
// Bad coordinates/layout reject before advancement; a later rendering failure
// can consume field state, so the owner must discard the incomplete pass.
BOOL pcRenderViewFieldSpriteRun(struct GraphicsFramebuffer* framebuffer, struct ViewFieldSpriteRuns* state,
    const struct ViewFieldRun* run, const struct ViewPortSpriteLayout* layout, UWORD modes,
    const struct GraphicsFramebufferPalette* palette, const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window);

// Advance the initialized eight-channel field through [field->line, limit)
// and compose each row with one viewport's palette/border policy. Coordinates
// use the SHRES/native-line grid; origins match the raster staging crop.
// The display owner splits bands at policy changes and retains field between
// calls. Offscreen rows still fetch and update retained sprite data. An empty
// band does no work. This does not schedule transitions or handle a policy
// change within one line; the owner uses the row compositor for those spans.
// Hold source ownership and the display semaphore throughout. Invalid ranges
// fail before advancement; a rendering failure invalidates the working field
// and staging image, which the owner must discard rather than resume.
BOOL pcRenderViewPortSpriteBand(struct GraphicsFramebuffer* framebuffer, struct NativeSpriteField* field,
    UWORD limit, WORD origin_x, WORD origin_y, const struct ViewPortSpriteLayout* layout, UWORD modes,
    const struct GraphicsFramebufferPalette* palette, const struct GraphicsSpriteOcclusion* occlusion,
    const struct Rectangle* region, const struct Rectangle* window);

struct ViewPortDisplaySnapshot {
    // Identity only: compare generations belonging to the same ViewPort.
    // Never dereference the caller's header through this captured value.
    ULONG viewport_identity;
    struct ViewPortRasterState rasters;
    struct Rectangle display; // Prepared coordinates, with exclusive ends.
    // Coordinate units belong to preparation, not to later scroll sampling.
    // horizontal_shift is 0/1/2 for LORES/HIRES/SHRES; 255 means absent mode data.
    UBYTE horizontal_shift, vertical_shift;
    ULONG modes;
    BOOL dual_disabled;
    UBYTE sprite_mask[2]; // Resolved pair blocking for each playfield.
    struct ViewPortSpriteLayout sprites;
    enum GraphicsEhbPrecision ehb_precision;
    enum GraphicsHamFormat ham_format;
    struct PlayfieldPaletteLayout palette_layout;
    ULONG color_mask[8];
    struct GraphicsFramebufferPalette palette;
};

// Resolve a decoded field's placement on a shared SHRES/field-line grid.
// The source starts at (0, 0), in this viewport's native pixel units. Both
// fields use the same window; their source-row phase is already in rasters.
// Monitor cropping, field weaving and firmware-framebuffer scaling belong to
// the output owner. Never derive these coordinates from mutable scroll state.
// Missing mode data or inverted windows reject without changing area.
BOOL pcGetViewPortFieldArea(const struct ViewPortDisplaySnapshot* display,
    struct GraphicsFramebufferScaleArea* area);

// The native display owner serializes capture against raster changes and owns
// the returned storage. Copy bitmap descriptors, origins, geometry and selected
// colors before the temporary build workspace/extra is released. Plane storage is borrowed,
// as with the original prepared display: later pixel writes remain visible.
// No caller header is borrowed. The ViewPort identity is compared across merge
// generations but never dereferenced. A missing
// bitmap clears its present bit; a second raster is captured only for DUALPF.
// Failure leaves the caller's current snapshot untouched. Replacing or freeing
// an active snapshot requires the display owner to finish its readers first.
// This palette describes initial preparation only. Live merged field palettes
// are captured separately from their emitted records, including independent
// high/low-nibble masks; do not present this initial capture as live colors.
struct ViewPortDisplaySnapshot* pcAllocViewPortDisplay(const struct ViewPort* viewport,
    const struct ViewPortBuildState* state, struct GfxBase* graphics);
void pcFreeViewPortDisplay(struct ViewPortDisplaySnapshot* snapshot, struct GfxBase* graphics);

// Select the decoder from captured modes and native driver format settings.
// Depth alone never enables HAM or EHB. The driver supplies EHB precision and
// HAM8 plane order through ViewPortBuildMode; these are copied at preparation,
// so no mode descriptor is borrowed during rendering. Within a HAM mode,
// depths 5/6 use HAM6 and depths 7/8 use the selected HAM8 order. Missing mode
// settings reject HAM/EHB, rather than guessing precision or plane order.
// Raster state is supplied separately: prepared state for a new merge, or
// the selected merged field after scrolling. No full snapshot/palette copy is
// needed for rendering. Width is in source pixels; height is in field lines.
// Captured interlace samples every other source row, starting at the supplied
// field origin. Placement, live palette and ownership follow below.
BOOL pcRenderViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height);

// Whether the captured raster's readable plane ranges overlap this bitmap.
// Uses the renderer's fetch normalization (including scroll strides, HAM and
// dual playfields), without reading any pixels or retaining the bitmap header.
// Returns -1 for an invalid raster, 0 for disjoint storage, 1 for overlap.
LONG pcViewPortBitMapOverlap(const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, const struct BitMap* bitmap);

// Decode [first,end) of one viewport-relative row into its unscaled staging
// window. The framebuffer describes the full window, including portions hidden
// by later output cropping. Clip against the selected field's readable planes;
// missing pixels retain the owner's background. Use successive spans in row
// order. HAM requires held_color initialized by the owner before the first
// readable pixel, after applying its initial palette records. Carry it through
// later spans/palette changes without replay. Resolve WAIT positions and other
// control records before selecting spans; Copper horizontal units are not pixels.
// Other formats ignore held_color. No allocation, scaling or presentation.
BOOL pcRenderViewPortSpan(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, ULONG row, ULONG first, ULONG end, ULONG* held_color);

// Optional mask workspace accompanying the color staging buffers. Each mask
// stores one byte per pixel with its own pitch. Scratch dimensions are those
// of the decoded viewport; output dimensions match the field framebuffer.
// All four buffers must be disjoint, including their padding. Plane storage
// and descriptors must not overlap either writable mask. Storage stays with
// the display owner; no pointer is retained by the renderer.
struct ViewFieldOcclusion {
    UBYTE* pixels;
    ULONG stride, size;
    UBYTE* scratch;
    ULONG scratch_stride, scratch_size;
};

// Project only [first,end) of a decoded viewport row onto the cropped field
// grid. Scratch contains the full unscaled viewport window; optional scratch
// occlusion contains matching source pixels. Use after decoding each interval,
// before later palette/priority changes. Color and mask use identical clipping
// and expansion. Pixels outside this span, padding and source storage stay
// untouched. Invalid arguments leave both outputs unchanged. No raster decode,
// allocation, sprite composition, field weaving or physical presentation.
// Storage and serialization follow pcRenderViewPortField's contract below.
BOOL pcProjectViewPortSpan(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebuffer* scratch,
    const struct ViewPortDisplaySnapshot* display, LONG origin_x, LONG origin_y,
    ULONG row, ULONG first, ULONG end, const struct ViewFieldOcclusion* occlusion);

// Ordered raster consumption for one retained viewport/field. Initialize to
// zero and set position to its activation boundary before the first run. Keep
// geometry, mode and raster selection stable; palette, addressing selectors
// and sprite masks may change between runs. The owner resets this state when
// selecting another viewport/field. No borrowed pointers are retained here.
struct ViewFieldRasterRuns {
    Point position;
    ULONG row, held_color;
    BOOL row_ready;
};

// Decode and project the readable raster portion of an absolute WAIT interval.
// Runs must be contiguous, including offscreen intervals: skipping pixels can
// lose HAM history. Reset that history at the first readable pixel of each
// row, using that interval's color zero, then retain it across palette changes.
// Decode before output cropping so hidden prefixes still affect visible HAM.
// The owner fills field background and clears its occlusion separately; missing
// raster pixels leave them untouched. Scratch spans outside readable sources
// are neither initialized nor projected. Optional masks follow color pixels.
// Apply control records and resolve terminal WAIT reachability before calling.
// Hold the display semaphore. No allocation, scheduling or presentation. On a
// rendering failure discard the working pass; earlier spans may have changed.
BOOL pcRenderViewFieldRasterRun(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    struct ViewFieldRasterRuns* state, const struct ViewFieldRun* run,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, LONG origin_x, LONG origin_y,
    const struct ViewFieldOcclusion* occlusion);

// Compose one resolved interval in background -> raster -> sprite order.
// Both states must start at run.start; initialize/retain them as documented
// above. Sprite origins select the common field grid. The owner supplies the
// current background color and working display selectors after processing
// control records. Border blanking uses black outside the prepared window.
// Clear only this interval's output mask before playfields write it; sprites
// must use that newly rendered mask, never the preceding interval's priority.
// Occlusion storage is required and follows the disjoint-buffer contract.
// Field staging dimensions must fit positive WORD rectangle coordinates.
// This neither selects/activates viewports nor assigns MOVE/WAIT times. Keep
// geometry/modes/raster selection stable for the attached raster state; reset
// it at actual viewport activation, preserving the ongoing sprite field.
// Hold the display semaphore. No allocation or physical presentation. Discard
// the working pass on failure, since earlier layers may have been rendered.
BOOL pcComposeViewFieldRun(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    struct ViewFieldRasterRuns* raster_state, struct ViewFieldSpriteRuns* sprite_state,
    const struct ViewFieldRun* run, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, const struct GraphicsFramebufferPalette* palette,
    ULONG background, const struct ViewFieldOcclusion* occlusion);

// Decode a retained field and place it on a cropped SHRES/field-line staging
// grid. origin selects that grid's top-left in prepared display coordinates;
// it is independent of scroll origins. Keep fractional expanded-pixel clipping
// and HAM prefix history. Palette and unreadable-source background are supplied
// by the display owner, rather than rereading caller-owned ColorMaps.
// A non-NULL occlusion workspace produces the matching projected mask. Missing
// source pixels clear its mask independently of background RGB; NULL leaves
// mask handling with the caller and retains the color-only path.
// Scratch must hold the complete unscaled field window and have the same pixel
// format as output. Its contents are workspace; output changes only after a
// successful decode. Buffers must be disjoint and must not alias borrowed planes
// or descriptors. Pixels outside the window retain their previous output.
// No allocation, field selection, palette inheritance, sprite composition,
// weaving or physical presentation occurs here. Hold the display semaphore.
BOOL pcRenderViewPortField(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette, LONG origin_x, LONG origin_y, ULONG background,
    const struct ViewFieldOcclusion* occlusion);

// Derive sprite visibility from captured playfield pixels, using
// the same clipping and live scroll fetches as their color rendering. Single
// playfields use the second priority; dual selects the visible nonzero field
// before its mask. Palette banks/XOR never change opacity. Output is one byte
// per unscaled staging pixel and must not overlap planes or color storage.
// Border pixels remain untouched. EHB excludes its intensity selector from
// opacity; HAM counts the displayed data and command planes, including the
// seven-plane HAM8 application's omitted-data/extra-command-slot layout.
// The display owner serializes this with color decoding and sprite composition.
BOOL pcWriteViewPortSpriteOcclusion(const struct GraphicsFramebuffer* framebuffer,
    UBYTE* output, ULONG stride, ULONG size, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, LONG dest_x, LONG dest_y, ULONG width, ULONG height);

// Match pcRenderViewPortSpan's source fetches and viewport-relative interval.
// Only readable pixels in that interval are written. The owner initializes
// missing-source background to zero and retains earlier spans across priority
// changes. Bounds and disjoint-storage requirements are the same as above.
BOOL pcWriteViewPortSpriteOcclusionSpan(const struct GraphicsFramebuffer* framebuffer,
    UBYTE* output, ULONG stride, ULONG size, const struct ViewPortDisplaySnapshot* display,
    const struct ViewPortRasterState* rasters, ULONG row, ULONG first, ULONG end);

// Decode an ordinary indexed capture using the resolved live field palette.
// Placement uses unscaled pixels and field lines; captured scrolling is clipped against
// both the bitmap and staging image. Pixels outside that area remain unchanged.
// Only the captured first raster participates, even if RasInfo had a Next node.
// Reject HAM, EHB and dual-playfield modes instead of treating their control
// planes as palette bits. This neither presents pixels nor reads caller-owned
// descriptors. Serialize with the native display owner, as for other readers.
BOOL pcRenderIndexedViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height);

// Six-plane EHB capture, with the same scrolling/lifetime contract as indexed
// rendering. Precision comes from the display owner, not ColorMap.Type: a
// legacy palette displayed at RGB8 precision still halves at RGB8 precision.
// Resolve the live palette before calling; the captured XOR precedes EHB
// decoding. Reject non-EHB, HAM and dual input without modifying pixels.
BOOL pcRenderEhbViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    enum GraphicsEhbPrecision precision, LONG dest_x, LONG dest_y, ULONG width, ULONG height);

// Decode a HAM capture with its live resolved palette and captured XOR. The
// mode provider selects the format/plane order; do not infer it from depth
// alone. Preserve held-color history across destination clipping, starting at
// the captured raster origin (clamped to its first readable pixel). Each row
// starts with palette[0]. Reject non-HAM/dual input without writes. Ownership,
// descriptor lifetime and unscaled placement follow the indexed path above.
BOOL pcRenderHamViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    enum GraphicsHamFormat format, LONG dest_x, LONG dest_y, ULONG width, ULONG height);

// Decode a captured dual-playfield viewport into staging pixels. The caller
// resolves this field's live palette and supplies the destination and extent
// in unscaled raster pixels. Source origins come from the capture, including
// independent scrolling. Clip to the common readable area of both rasters;
// pixels outside it remain the composition owner's responsibility. No caller
// ViewPort/ColorMap is read, and plane storage remains borrowed.
// Supports both the single-bitmap alternating-plane representation and two
// RasInfo bitmaps, including CMAF_DUALPF_DISABLE's combined indexed output.
// HAM decoding, monitor placement/scaling, sprites and presentation belong to
// their separate stages. Reject non-dual/HAM input without writes. Serialize
// with the native display owner, as for all snapshot readers.
BOOL pcRenderDualViewPort(struct GraphicsFramebuffer* framebuffer,
    const struct ViewPortDisplaySnapshot* display, const struct ViewPortRasterState* rasters,
    const struct GraphicsFramebufferPalette* palette,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height);

struct PreparedViewPort {
    struct PreparedViewPort* next;
    const struct CopList* list;
    struct ViewPortDisplaySnapshot* display;
    ULONG references;
};

struct PreparedViewPorts {
    struct PreparedViewPort* first;
};

// Native driver ownership, keyed by the intermediate list rather than its
// caller-owned ViewPort. Start with an empty collection. The display owner
// serializes all calls, including readers, using its display semaphore.
// Store takes a newly captured, caller-owned display and a valid list key.
// It consumes display only on success; failure leaves both the preceding
// entry and the caller's allocation intact. Each retained entry survives list
// replacement/removal until its final reader releases it. This storage does
// not own the CopList or pixel planes, publish a View, or activate scanout.
BOOL pcStorePreparedViewPort(struct PreparedViewPorts* prepared, const struct CopList* list,
    struct ViewPortDisplaySnapshot* display, struct GfxBase* graphics);
struct PreparedViewPort* pcRetainPreparedViewPort(const struct PreparedViewPorts* prepared,
    const struct CopList* list);
void pcReleasePreparedViewPort(struct PreparedViewPort* entry, struct GfxBase* graphics);
void pcRemovePreparedViewPort(struct PreparedViewPorts* prepared, const struct CopList* list,
    struct GfxBase* graphics);
void pcClearPreparedViewPorts(struct PreparedViewPorts* prepared, struct GfxBase* graphics);

// Each merged field owns its palette writes independently of preparation and
// of the other field. Missing high/low nibbles inherit the preceding palette.
// The bounded stream is borrowed only while its output owner is bound. Retiring
// that owner clears the pointers, leaving retained readers' colors intact.
// Additional binding aliases never acquire ownership of that stream storage.
// A prefix ends no later than the next distinct viewport prefix. An empty
// prefix therefore retains no writes; it cannot read ahead into the next one.
struct ViewFieldPalette {
    const struct cprlist* owner;
    const UWORD* start;
    const UWORD* end;
    struct GraphicsFramebufferPalette colors;
    ULONG high_mask[8], low_mask[8];
    ULONG next_port;
};

struct ViewDisplayPort {
    struct PreparedViewPort* prepared;
    struct ViewFieldPalette palette[2];
    // Last emitted wait at this display-list head's first selected WAIT.
    // Coordinates are absolute native lines and even horizontal wait units,
    // before MOVE execution. An excluded field keeps its preceding emitted position.
    // These survive output retirement; they are not inferred from geometry
    // or a later scroll. Bit n marks field n as captured by the native merger.
    Point prefix_wait[2];
    // Public CopLStart/CopSStart position in this field, including the initial
    // WAIT when one is emitted. Retain it with the resolved prefix wait:
    // equal wait coordinates can have intervening records or distinct owners.
    ULONG prefix_word[2];
    // Output cursor immediately after this prefix's first WAIT is processed.
    // It includes emitted page-boundary waits and excludes suppressed waits.
    // Native setup must not take ownership at the earlier public list pointer.
    ULONG setup_word[2];
    UBYTE prefix_wait_mask;
    // Scrolls update the two emitted fields independently of preparation.
    // Plane storage is borrowed; bitmap descriptors and origins are owned.
    struct ViewPortRasterState rasters[2];
    // Immediate border/resolution updates follow each live output field.
    struct ViewPortSpriteLayout sprites[2];
};

// Positions of actual WAIT records in the merged field, in output order.
// Offsets count UWORDs from the field's start. Capture absolute lines during
// emission: an encoded eight-bit line alone cannot identify its page. These
// owned records survive output retirement; they borrow no list storage.
// Positions precede subsequent MOVE execution, not per-instruction bus timing.
struct ViewFieldWait {
    ULONG offset;
    Point position;
};

struct ViewFieldTiming {
    struct ViewFieldWait* waits;
    ULONG capacity;
    ULONG count;
    ULONG words;
    // Native capture reserves records immediately after waits in one block.
    // live borrows the actual output until its owner is overwritten/freed;
    // retirement copies it into records before dropping that borrowed pointer.
    UWORD* records;
    const UWORD* live;
    const struct cprlist* owner;
    // Original list identity survives retirement. Compare only; never read
    // through this address after live has been detached.
    ULONG identity;
};

static inline ULONG pcViewFieldStorageSize(ULONG capacity)
{
    return capacity * (sizeof(struct ViewFieldWait) + 2 * sizeof(UWORD));
}

struct ViewFieldRun {
    Point start;
    Point end;
    ULONG first;
    ULONG words;
    const UWORD* records;
};

// Walk MOVE runs between captured WAITs, including the initial run at (0,0).
// Initialize cursor to zero. Return 1 for a run, 0 at the final boundary, -1
// for inconsistent metadata/output without modifying cursor or run. Empty
// runs are significant. Read the live output while bound, the owned copy after
// retirement. A run's pointer is valid only while holding the display semaphore.
// Changing WAITs requires re-merging; reject stale boundaries rather than guess
// their absolute page. This does not execute display control or present pixels.
// The final WAIT is a stream boundary, not a derived monitor end-of-field time;
// horizontal positions remain in the merger's units, not framebuffer pixels.
LONG pcNextViewFieldRun(const struct ViewFieldTiming* timing, ULONG* cursor, struct ViewFieldRun* run);

struct ViewPortRunSpan {
    ULONG row;
    ULONG first, end;
};

// Intersect one ordered run's wait-coordinate interval with a prepared viewport.
// Initialize cursor to zero for each run. Return 1 for the next nonempty row
// span, 0 when exhausted, or -1 for invalid geometry/range. Only success changes
// cursor and span. The span uses unscaled viewport pixels and field rows, as
// pcRenderViewPortSpan requires; scroll offsets do not move this display window.
// Horizontal wait units are two LORES pixels (four/eight HIRES/SHRES pixels).
// Absolute field lines are already resolved by the merger; do not wrap at 256
// or halve the wait coordinates for LACE. Equal boundaries produce no pixels.
// This maps boundaries, not MOVE execution/bus timing or the terminal WAIT's
// reachability. The display owner resolves those before selecting an interval,
// processes all palette/control records even for invisible runs, and preserves
// HAM state between successive spans of a row. No plane/output memory is read.
LONG pcNextViewPortRunSpan(const struct ViewPortDisplaySnapshot* display,
    const struct ViewFieldRun* run, ULONG* cursor, struct ViewPortRunSpan* span);

// Apply native RGB8 palette records in run order, carrying bank/precision
// control and untouched color nibbles across runs. Other MOVE destinations
// remain the display owner's responsibility. Validate before changing either
// output. No viewport-prefix reset, allocation or public library state change.
BOOL pcApplyViewFieldPaletteRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    const struct ViewFieldRun* run);

// Also apply playfield palette addressing and sprite bank/resolution/border
// selectors in the same record order. Supply writable working layouts, not
// fields of a retained preparation. Unwritten settings and palette nibbles
// carry across runs/prefixes. Validate the whole run before any output changes.
// This consumes color/selector records only; geometry, modes, plane sources,
// activation and timing remain with the owner. No rendering or allocation.
BOOL pcApplyViewFieldColorRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    struct PlayfieldPaletteLayout* palette, struct ViewPortSpriteLayout* sprites,
    const struct ViewFieldRun* run);

// Consume those colors/selectors together with playfield ordering and both
// playfield/sprite priorities (the low seven bits of destination 0x104).
// display is the owner's writable working copy, never a retained preparation.
// Preserve other mode bits, geometry and source selection. Unwritten state
// carries across batches; a late invalid record rejects all state changes.
// Other destinations and the remaining 0x104 bits still require the owner's
// separate control handling. This does not activate, render or schedule a field.
BOOL pcApplyViewFieldDisplayRun(struct GraphicsFramebufferPalette* colors, UWORD* control,
    struct ViewPortDisplaySnapshot* display, const struct ViewFieldRun* run);

struct ViewDisplaySnapshot {
    ULONG references;
    ULONG count;
    UWORD modes;
    ULONG first_port[2];
    struct ViewFieldTiming timing[2];
    struct ViewDisplayPort viewports[];
};

// One owned allocation for field color, viewport scratch and both occlusion
// images. These descriptors are staging memory only, never scanout addresses.
// Scratch covers the largest full decoded viewport in the selected retained
// field, even when the output crop hides its HAM prefix or whole rows. Keep
// this object stationary while a composition refers to its descriptors.
struct ViewFieldBuffers {
    struct GraphicsFramebuffer field, scratch;
    struct ViewFieldOcclusion occlusion;
    ULONG allocation_size;
    UBYTE pixels[];
};

// Allocate a detached buffer set in the opened output's pixel format. width
// and height describe the owner's cropped SHRES/field-line grid, not firmware
// resolution. Retain display and hold the display semaphore while inspecting
// its geometry. No snapshot/plane pointers are retained. Validate geometry and
// all size arithmetic before allocating; failure leaves an existing set and
// active selection untouched. Empty fields still need a minimal scratch image
// for the common background/sprite compositor. Neither operation presents,
// changes modes, chooses a crop or activates a View. Free only after readers
// finish, using the same Exec allocator; NULL is accepted by the release.
struct ViewFieldBuffers* pcAllocViewFieldBuffers(const struct ViewDisplaySnapshot* display,
    unsigned field, const struct FramebufferMode* format, ULONG width, ULONG height, struct GfxBase* graphics);
void pcFreeViewFieldBuffers(struct ViewFieldBuffers* buffers, struct GfxBase* graphics);

struct ViewFieldPortEvent {
    ULONG word;
    const struct ViewDisplayPort* port;
};

// Enumerate native viewport-prefix entries in emitted field order. Start
// cursor at first_port[field] and bound traversal by display->count, as for the
// palette-prefix iterator. Return 1 for an event, 0 for the sentinel, or -1 for
// inconsistent metadata; only success changes cursor/event. Equal offsets keep
// the captured ViewPort-chain order. An event can precede an empty MOVE run or
// sit at the field end; it does not itself imply visible pixels or activation.
// The offset is the public prefix entry, before any initial WAIT is consumed.
// Match word offsets against the field records, not against wait coordinates.
// Both position and port survive output-list retirement. The caller holds the
// display semaphore and retains the snapshot while using the returned pointer.
// No palette, raster, sprite, public OS state or pixel data is changed here.
LONG pcNextViewFieldPortEvent(const struct ViewDisplaySnapshot* display, unsigned field,
    ULONG* cursor, struct ViewFieldPortEvent* event);

// The same retained order at the post-WAIT setup boundary. event.word selects
// setup_word rather than the public prefix pointer. No timing is inferred from
// coordinates or mutable output records. A boundary can be at field end; the
// caller still determines whether the preceding WAIT is actually reachable.
LONG pcNextViewFieldSetupEvent(const struct ViewDisplaySnapshot* display, unsigned field,
    ULONG* cursor, struct ViewFieldPortEvent* event);

struct ViewFieldVisitor {
    BOOL (*prefix)(APTR context, const struct ViewFieldPortEvent* event);
    BOOL (*moves)(APTR context, const struct ViewFieldRun* run);
    BOOL (*wait)(APTR context, ULONG word, const Point* position, BOOL terminal);
};

// Visit prefixes, MOVE batches and WAITs in record order. Split batches at
// every prefix, including tied entries, before delivering the record there.
// Each MOVE batch retains its enclosing wait coordinates; these are not an
// instruction execution time. Empty batches are omitted, never empty WAITs
// or prefixes. The final WAIT is marked terminal, not assigned a monitor time.
// Prefixes identify captured state; the owner resolves activation after the
// corresponding control records, and handles all non-palette destinations.
// All callbacks are required. FALSE means invalid metadata or a rejected
// callback; earlier callbacks are not undone, and the owner must discard any
// incomplete presentation. Hold the display semaphore and retain the snapshot
// throughout; callbacks must not mutate its lists, metadata or ownership.
// No allocation, MMIO, public OS state change or pixel output occurs here.
BOOL pcVisitViewField(const struct ViewDisplaySnapshot* display, unsigned field,
    const struct ViewFieldVisitor* visitor, APTR context);

// Traverse the same records but call visitor.prefix at each post-WAIT setup
// boundary instead of its public prefix entry. This is the native display
// owner's handover point for the captured viewport state; palette/control MOVEs
// still follow in their original order. Earlier intervals keep the previous
// owner. Callback rejection, terminal-WAIT handling and serialization follow
// pcVisitViewField. No MOVE execution latency or visible activation is invented.
BOOL pcVisitViewFieldSetup(const struct ViewDisplaySnapshot* display, unsigned field,
    const struct ViewFieldVisitor* visitor, APTR context);

struct ViewFieldScheduleVisitor {
    BOOL (*setup)(APTR context, const struct ViewFieldPortEvent* event, const Point* position);
    BOOL (*moves)(APTR context, const struct ViewFieldRun* run);
    BOOL (*interval)(APTR context, const struct ViewFieldRun* run);
};

// Schedule captured setup/MOVE batches between resolved WAIT boundaries.
// rows is the exclusive native field-line limit (1..1024); last_horizontal
// is the last reachable even WAIT coordinate (0..254), selected by the native
// monitor owner. Neither describes the firmware framebuffer's physical timing.
// MOVE batches have start == end at their current boundary. Nonempty interval
// callbacks have no records and cover the field continuously from (0,0) to
// (0,rows), retaining the preceding owner while a WAIT is pending. A horizontal
// target beyond the line's extent resolves on the next line, except at the
// eight-bit vertical maximum, where it cannot complete before field restart.
// Suppress setup/MOVE callbacks after an unreachable WAIT or the field limit;
// still validate the remaining stream. A reachable terminal WAIT does not end
// scanout: the last interval extends to rows. Empty fields get that interval too.
// All callbacks are required; rejection discards the incomplete pass. Hold the
// same ownership/semaphore as pcVisitViewFieldSetup. This schedules OS list
// boundaries, not instruction bus latency, hardware registers or presentation.
BOOL pcScheduleViewField(const struct ViewDisplaySnapshot* display, unsigned field,
    UWORD rows, UWORD last_horizontal, const struct ViewFieldScheduleVisitor* visitor, APTR context);

// Caller-owned workspace for one scheduled native composition pass. Initialize
// sprites with a fresh field starting at line zero. working supplies initial
// border/sprite policy and mode; colors supplies the inherited live palette.
// Buffers follow pcComposeViewFieldRun's storage contract. The pass owns these
// mutable objects until it finishes; never share them with another reader.
struct ViewFieldComposition {
    struct GraphicsFramebuffer* framebuffer;
    struct GraphicsFramebuffer* scratch;
    const struct ViewFieldOcclusion* occlusion;
    struct GraphicsFramebufferPalette* colors;
    struct ViewPortDisplaySnapshot working;
    struct ViewFieldRasterRuns raster;
    struct ViewFieldSpriteRuns sprites;
    unsigned field;
    BOOL active;
    // Required owner operation for every scheduled MOVE batch, including
    // records before the first viewport. Reject unsupported controls. It may
    // update working selectors/colors and, when changing fetch interpretation,
    // must reset raster history at run.start. It must not advance sprites,
    // change buffers/field ownership, or retain the borrowed run records.
    BOOL (*controls)(APTR context, struct ViewFieldComposition* composition, const struct ViewFieldRun* run);
    APTR context;
};

// Native list control state, independent of viewport setup. The palette bank
// and nibble selector survive a setup or field boundary until another record
// writes them. Field-link halves likewise retain the unwritten half. The link
// is an output-list identity, not an address to dereference here; the display
// owner resolves it against its retained field generations at field completion.
struct ViewFieldControls {
    UWORD display;
    ULONG field_link;
};

// Validate the native control batch and resolve only its field-link halves.
// Shares composition's accepted controls without touching palette/raster state
// or pixel memory. Failure leaves link untouched. The caller supplies the
// inherited identity and invokes this only for scheduled, reachable MOVE runs.
BOOL pcReadNativeViewFieldLink(ULONG* link, const struct ViewFieldRun* run);

// Composition callback for native palette/GEL lists and their interlaced links.
// context points to ViewFieldControls. Consume palette/sprite selectors,
// playfield priorities and the two field-link halves in actual record order.
// Reject unknown destinations and unimplemented priority-control bits before
// modifying any state; never silently accept a foreign hardware operation.
// Geometry/rasters come from captured native setup, not register writes. This
// consumes complete DrawGList image descriptors, selecting their pixel storage
// for the next unfetched line without touching an already fetched row. Partial
// sprite descriptors and direct pixel-word writes are not supported here.
// It does not jump to a field link, render or select an owner.
BOOL pcApplyNativeViewFieldControls(APTR context, struct ViewFieldComposition* composition,
    const struct ViewFieldRun* run);

// Join the scheduler, retained viewport setup, background, rasters and sprites.
// No raster is active before the first setup; color zero and initial border/
// sprite policy still apply. Each setup copies its prepared geometry/modes and
// the selected field's current raster/sprite state, resets raster history and
// preserves the ongoing sprite field and live palette. The controls callback
// then applies the actual emitted records before the next interval. Palette
// color zero supplies background; border blanking remains independent.
// Retain display and hold the display semaphore throughout. No allocation,
// monitor selection, public-state activation, field weaving or presentation.
// Success leaves final working state available to the owner; any failure
// invalidates that state and the partially rendered staging image together.
BOOL pcComposeScheduledViewField(struct ViewFieldComposition* composition,
    const struct ViewDisplaySnapshot* display, unsigned field, UWORD last_horizontal);

// Resolve a complete native field-link identity within its retained merge
// generation. Exact, unique matches only; no dereference or global address
// search that could select newly allocated storage at a retired address.
// Missing/ambiguous identities leave field unchanged. The caller retains the
// snapshot and holds the display semaphore; this acquires no new reference.
BOOL pcResolveViewFieldLink(const struct ViewDisplaySnapshot* display, ULONG identity, unsigned* field);

// Compose a native list using the production control consumer and resolve its
// final link for the next restart. An unwritten link repeats this source field;
// interlaced tails emitted by the merger normally select its partner. Seed
// link identity from this selected field, retaining the supplied palette bank/
// nibble selector. Install the callback/context in the caller's workspace.
// next_field is written only after successful composition and resolution.
// Unknown links reject the pass; never assume a long/short toggle. The same
// ownership and invalid-pass rules as pcComposeScheduledViewField apply.
// This does not select physical field parity, activate or present a display.
BOOL pcComposeNativeViewField(struct ViewFieldComposition* composition,
    const struct ViewDisplaySnapshot* display, unsigned field, UWORD last_horizontal,
    struct ViewFieldControls* controls, unsigned* next_field);

// Capture the native prepared generations in ViewPort-chain order, omitting
// VP_HIDE and absent DspIns as the original merger does. Every other list must
// have a native preparation association. Failure returns NULL and releases any
// acquired references; an empty selection is a valid snapshot with count zero.
// LACE preparation advances the short field by one source row, independently
// of the current View's LACE bit and the preparation geometry's line scale.
// After a plane-source scroll updates intermediate state, a new capture keeps
// that prepared field selection and applies the original signed WORD stride
// adjustment. Existing merged fields retain their separately updated addresses.
// No caller-owned View/ViewPort/list header is dereferenced by readers. Pixel
// planes remain borrowed through the prepared generations. The owner serializes capture,
// retain and release with its display semaphore, including source traversal.
// This owns merger input; it does not merge auxiliary/user Copper lists, alter
// public lists, assign a public MrgCop status or activate a View.
struct ViewDisplaySnapshot* pcAllocViewDisplay(const struct View* view,
    const struct PreparedViewPorts* prepared, struct GfxBase* graphics);
void pcRetainViewDisplay(struct ViewDisplaySnapshot* display);
void pcReleaseViewDisplay(struct ViewDisplaySnapshot* display, struct GfxBase* graphics);

// Called by the native merger after finalizing output. Bind complete field
// records and capture viewport palette prefixes while list identities are
// live. Later readers use the retained fields, not caller metadata. Ends are
// the actual emitted one-past-last field cursors.
void pcCaptureViewDisplayPalettes(struct ViewDisplaySnapshot* display,
    const struct cprlist* long_list, const UWORD* long_end,
    const struct cprlist* short_list, const UWORD* short_end);
void pcResolveViewFieldPalette(struct GraphicsFramebufferPalette* output,
    const struct GraphicsFramebufferPalette* preceding, const struct ViewFieldPalette* palette);

// Iterate the captured viewport prefixes in emitted field order. Initialize
// cursor from first_port[field] and colors from the owner's preceding field
// palette. Each call applies the selected prefix and advances cursor; ~0UL
// terminates the chain. Ordering survives retirement of the public RAM lists.
// No mutable ViewPort/list metadata is read, and no palette reset is implied.
// This does not interpret auxiliary lists or schedule mid-viewport changes.
const struct ViewDisplayPort* pcNextViewFieldPort(const struct ViewDisplaySnapshot* display,
    unsigned field, ULONG* cursor, struct GraphicsFramebufferPalette* colors);

// Compose retained viewport rasters in emitted prefix order on the cropped
// SHRES/field-line grid. colors carries the preceding palette into this field
// and receives its final palette, including prefixes outside the visible crop.
// Use each port's selected field rasters, not its original prepared sources.
// When supplied, occlusion follows the same field sources, crop and overwrite
// order as colors. Unreadable source pixels have no playfield occlusion,
// independently of their background RGB; pixels outside windows stay intact.
// Only viewport windows are written; the owner supplies background/borders,
// sprite composition, auxiliary-list transitions, weaving and presentation.
// This is a raster pass, not a scanout scheduler: later windows overwrite
// earlier pixels. It does not resolve overlapping display-list intervals.
// Scratch and output obey pcRenderViewPortField's disjoint-storage contract.
// Hold the display semaphore throughout. On failure, discard the staging image
// and working palette: earlier ports may already have been composed. No public
// View/ColorMap/list pointers are read and no ownership or display is changed.
BOOL pcRenderViewFieldRasters(struct GraphicsFramebuffer* framebuffer, struct GraphicsFramebuffer* scratch,
    const struct ViewDisplaySnapshot* display, unsigned field, struct GraphicsFramebufferPalette* colors,
    LONG origin_x, LONG origin_y, ULONG background, const struct ViewFieldOcclusion* occlusion);

struct MergedView {
    struct MergedView* next;
    const struct cprlist* fields[2];
    struct ViewDisplaySnapshot* display;
};

struct MergedViews {
    struct MergedView* first;
};

// Freeze borrowed output before overwriting or freeing it. Keep associations
// and retained readers alive; aliases of another output owner are unaffected.
// Call under the display semaphore, before any output-buffer reallocation.
void pcRetireMergedViewOutput(struct MergedViews* merged, const struct cprlist* list);

// Bind a captured native display to the merged output lists, not the editable
// View or its intermediate lists. A successful store consumes display; failure
// preserves all bindings and leaves display with the caller. Both field keys
// are optional individually, but at least one must be present. Reusing a key
// replaces only that association; the other field and retained readers survive.
// Serialize all operations with ActiViewCprSemaphore. These bindings neither
// own the cprlist allocations nor activate/present a display.
BOOL pcStoreMergedView(struct MergedViews* merged, const struct cprlist* long_list,
    const struct cprlist* short_list, struct ViewDisplaySnapshot* display, struct GfxBase* graphics);

// Commit an already allocated entry with an owned display reference. At least
// one output key is required. This cannot fail or allocate, allowing the native
// merger to reserve ownership before changing public output storage.
void pcCommitMergedView(struct MergedViews* merged, struct MergedView* entry,
    const struct cprlist* long_list, const struct cprlist* short_list, struct GfxBase* graphics);
struct ViewDisplaySnapshot* pcRetainMergedView(const struct MergedViews* merged,
    const struct cprlist* list);
struct ViewDisplaySnapshot* pcRetainMergedViewField(const struct MergedViews* merged,
    const struct cprlist* list, unsigned* field);
// Synchronize matching live output fields after a palette-list update. The
// original updater may search past an empty prefix for color zero, so refresh
// every viewport in that affected field. Prepared/intermediate captures,
// unrelated Views and retired fields retain their state.
void pcRefreshMergedViewPalettes(struct MergedViews* merged, const struct CopList* list);

// Synchronize sprite settings after immediate border-control list writes.
// Bits 0/1 select the written long/short streams; bit 2 selects preparation.
// Keep palette banks unchanged. Retired fields and other merged generations
// retain their settings, even when they share the same preparation object.
// The caller holds ActiViewCprSemaphore.
void pcUpdateViewPortSpriteControl(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, UWORD control, UWORD written);

// Apply a resolved native scroll under the display semaphore. The optional
// intermediate state updates only the currently prepared list generation;
// NULL preserves it (CMAF_NO_INTERMED_UPDATE). Long/short states update only
// still-bound field streams selected by the list's CopLStart/CopSStart. Retired
// readers and unrelated generations retain their raster state. Inputs must
// not alias destination storage. Copy values rather than retaining caller
// BitMap/RasInfo descriptors. Only active plane pointers, strides and source
// origins change; presence, depth, row bounds, flags and inactive planes stay
// prepared. Ordinary inputs describe the prepared playfield interpretation.
// plane_fetches instead selects independent display-slot sources and leaves
// every prepared BitMap descriptor intact; rendering resolves that layout.
// No allocation, palette/geometry change, mode lookup or rendering occurs here.
void pcUpdateViewPortRasters(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, const struct ViewPortRasterState* intermediate,
    const struct ViewPortRasterState* long_field, const struct ViewPortRasterState* short_field);

// Resolve the native ChangeVPBitMap cache only when SpecialCounter changes.
// CopPtr1 identifies the prepared list; CopPtr2/3 identify short/long output
// bindings. These private cache fields identify native sources, not chipset
// plane-address instructions. A cache hit must not inspect the current list.
// FALSE means an uncached viewport has no DspIns; leave all info untouched.
// Hidden viewports cache no sources. Preserve the original WORD geometry and
// mode lookup used by the public synchronization bookkeeping. Caller holds
// ActiViewCprSemaphore. No relocation, bitmap update or message delivery here.
BOOL pcCacheNativeDBufInfo(const struct ViewPort* viewport, struct DBufInfo* info,
    struct GfxBase* graphics);

// ChangeVPBitMap relocates existing fetch addresses by the difference between
// old and new bitmap planes. It does not rebuild geometry, stride or scrolling.
// Resolve application planes from each display slot, including HAM8 rotation.
// The caller supplies same-layout bitmaps and holds the display semaphore.
// slots selects the plane-address records present in the updated output.
// This changes only one retained raster state; ownership, public RasInfo and
// DBufInfo completion are the switching operation's separate responsibilities.
void pcRelocateViewPortRaster(struct ViewPortRasterState* rasters,
    const struct ViewPortDisplaySnapshot* display, const struct BitMap* previous,
    const struct BitMap* next, UWORD modes, UBYTE slots);

// Relocate the selected prepared list and still-bound output fields. The
// caller resolves DBufInfo's cached identities; do not substitute the current
// public list header when a cache selects an older generation. NULL suppresses
// that target. Intermediate and output masks are separate because the original
// intermediate walk follows its records while raw output uses the new depth.
// Duplicate ownership keys do not duplicate a relocation. Aliased long/short
// targets do, matching the two original writes. Retired readers stay unchanged.
// Returns bits 0/1 for resolved long/short targets and bit 2 for preparation.
// The caller holds the display semaphore; no allocation, public bitmap update,
// descriptor retention, presentation or DBufInfo message delivery occurs here.
UWORD pcRelocateViewPortRasters(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* intermediate, const UWORD* long_start, const UWORD* short_start,
    const struct BitMap* previous, const struct BitMap* next, UWORD modes,
    UBYTE intermediate_slots, UBYTE field_slots);

// Native source-switch transaction, serialized by ActiViewCprSemaphore.
// Resolve the cache, replace RasInfo.BitMap and relocate its selected native
// sources. Prepared depth controls intermediate writes; new depth controls
// output fields. Hidden viewports retain their bitmap and source state.
// FALSE leaves everything untouched when an uncached viewport lacks DspIns.
// Same-layout display bitmaps (up to eight planes) are required. This does not
// acknowledge either DBufInfo message: safe/display completion is owned by
// the caller after resolving active readers and presentation requirements.
BOOL pcSwitchNativeViewPortBitMap(struct ViewPort* viewport, struct BitMap* bitmap,
    struct DBufInfo* info, struct GfxBase* graphics);

// Apply resolved source fetches using the control state captured before scroll
// preparation. The active View's LACE bit selects the short output; the saved
// viewport LACE bit adds one source row, even with DOUBLESCAN. The caller owns
// intermediate-update eligibility. Sources use the prepared playfield layout
// unless plane_fetches selects independent display-slot sources.
// field_extra is the output owner captured before mode lookup, not the later
// preparation extra. Its nonzero cached pointers select native field bindings
// only when that public field exists. Successful native field updates publish
// their resolved binding back to that owner. Absent, unmatched or retired
// bindings leave their previous cache values intact. No extra is retained.
// caller_d3 preserves the original short-field address addition when no long
// field plane write clears its upper word. It is captured at the public entry;
// preparation callbacks preserve D3 under the existing 68k calling convention.
void pcApplyViewPortScroll(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, struct ViewPortExtra* field_extra, const struct ViewPortRasterState* rasters,
    UWORD view_modes, UWORD viewport_modes, BOOL update_intermediate, ULONG caller_d3);

// Apply source selection directly to each matching prepared display layout.
// Sources are in display-slot order, as returned by pcReadViewPortScrollFetches;
// interlaced is the row sampling resolved by scroll preparation, independently
// of the saved mode bits that select field offsets. Depth, decoder, plane
// enables and allocation bounds remain prepared. Field
// ownership/cache publication and intermediate policy follow pcApplyViewPortScroll.
void pcApplyViewPortPlaneScroll(struct PreparedViewPorts* prepared, struct MergedViews* merged,
    const struct CopList* list, struct ViewPortExtra* field_extra, const struct ViewPortPlaneFetch* fetches,
    BOOL interlaced, UWORD view_modes, UWORD viewport_modes, BOOL update_intermediate, ULONG caller_d3);

void pcRemoveMergedView(struct MergedViews* merged, const struct cprlist* list,
    struct GfxBase* graphics);
void pcClearMergedViews(struct MergedViews* merged, struct GfxBase* graphics);

// Native output retains each field independently: callers can select fields
// from different merged Views. A missing short list preserves the old short
// field, as LoadView does, even when the new View is not interlaced.
// Native RGB8 background captured independently of the merged View's palette
// writes. No caller-owned ViewPort or ColorMap survives the update. Keep the
// raw nibble words and border flags for the display owner's border policy.
struct ViewBackground {
    ULONG color;
    UWORD high, low;
    UBYTE flags;
};

struct ActiveViewFields {
    ULONG generation;
    struct ViewDisplaySnapshot* display[2];
    unsigned source_field[2];
    BOOL enabled;
    // Empty LoadView still owns native output; KillView relinquishes it.
    BOOL owned;
    struct ViewBackground background;
};

// Field-selection stage of native LoadView. Serialize with
// ActiViewCprSemaphore, including all readers and presentation. Select the
// public ActiView/Modes/LOFlist/SHFlist and retain their native generations.
// Blank Views use copinit->wait_forever; they require initialized copinit.
// Caller-owned lists retain public field selection without a native snapshot.
// This allocates nothing and does not change monitors, invoke callbacks,
// increment SpecialCounter, present pixels or implement all of LoadView.
BOOL pcSelectViewFields(struct View* view, struct GfxBase* graphics);
// Native LoadView software state and monitor lifecycle. Reserve native fields,
// serialize with ActiViewCprSemaphore, and preserve counter/lookup/callback
// ordering. do_monitor and outgoing ms_KillView use their existing ABIs. A
// missing native field binding does not suppress public state or callbacks.
// The native display owner supplies palette/presentation and startup
// integration around this shared state transition. No chipset registers are used.
BOOL pcLoadViewState(struct View* view, struct GfxBase* graphics);
struct ViewDisplaySnapshot* pcRetainActiveViewField(BOOL short_field, struct GfxBase* graphics);
void pcClearActiveViewFields(struct GfxBase* graphics);

// Refresh one selected native field under ActiViewCprSemaphore, including the
// final framebuffer transfer. Resolve the source from the active output slot;
// it need not have the same index as physical short_field. LACE weaves into
// even/odd output rows; other modes place a complete image. The caller supplies
// fresh composition workspace, persistent firmware-format output staging and
// placement resolved from display geometry. output must be an opened firmware
// framebuffer, disjoint from composition storage. No allocation or mode change.
// Disabled/missing selections and failed composition/placement never present.
// next_field changes only on success; it is the list's resolved restart source,
// not a change to active selection or physical parity. Failed composition state
// is discarded as for pcComposeNativeViewField. This is task-context work, not
// an interrupt handler, and does not install a refresh task or activate a View.
BOOL pcPresentActiveViewField(struct ViewFieldComposition* composition,
    struct GraphicsFramebuffer* output, const struct GraphicsFramebufferScaleArea* area,
    BOOL short_field, UWORD last_horizontal, struct ViewFieldControls* controls,
    unsigned* next_field, struct GfxBase* graphics);

// Native monitor's ms_KillView callback. Stop presentation and release field
// references under the display semaphore. Leave public View/monitor pointers
// to the new owner: it may already have installed its own selection. Future
// refresh must hold the semaphore through presentation, not just capture.
LONG pcKillNativeView(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"));

// Native monitor merge method: capture prepared raster generations and merge
// public lists under ActiViewCprSemaphore, then bind the completed outputs.
// Eligible display lists must have native preparation associations. This does
// not activate the View; custom monitor dispatch remains with MrgCop's wrapper.
LONG pcMergeNativeView(struct View* view, struct GfxBase* graphics asm("a6"));

// Final native preparation stage, after successful list construction and
// before releasing the temporary extra/workspace. The native mode supplies
// the owning collection. Run under that owner's display semaphore, like the
// collection calls above. Failure retains the preceding prepared generation.
ULONG pcCommitViewPortBuild(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));

// Resolve the preparation query for a valid View and ViewPort. A selected
// ColorMap display record precedes public GetVPModeID; legacy fallback uses
// ViewPort mode bits and the View's monitor-selection convention. A false
// result reports an absent query, without choosing a MakeVPort error code or
// executing any backend methods. The caller owns the SpecialCounter update.
BOOL pcGetViewPortBuildInfo(struct View* view, struct ViewPort* viewport, struct VecInfo* info,
    struct GfxBase* graphics);

struct ViewPortScrollInfo {
    struct View* view;
    struct ViewPortExtra* extra;
    struct ViewPortExtra* field_extra;
    struct VecInfo driver;
    UWORD modes, view_modes;
    BOOL update_intermediate;
};

// Resolve native scroll eligibility and capture control state before driver
// preparation. Caller holds ActiViewCprSemaphore. Hidden viewports and absent
// active Views do not query a mode; fetch validation follows ID resolution and
// precedes the internal display-data read. A false result must not be consumed.
// field_extra retains the output owner from before ID lookup; extra is the
// preparation owner captured afterward. Callbacks may change the current map.
// This does not execute methods, change SpecialCounter or update raster state.
BOOL pcGetViewPortScrollInfo(struct ViewPort* viewport, struct ViewPortScrollInfo* info,
    struct GfxBase* graphics);

// Execute preparation after the caller has resolved a valid DTAG_VEC query.
// Default single/dual tables must contain actual backend methods. This does
// not resolve display IDs, publish MakeVPort or activate the resulting View.
UWORD pcBuildViewPort(struct View* view, struct ViewPort* viewport, const struct VecInfo* info,
    APTR default_vectors, APTR dual_vectors, struct GfxBase* graphics);

// Prepare a scroll using the same driver selection as MakeVPort, after mode
// resolution/fetch validation. The driver supplies its preparation-prefix
// length and WORD-aligned workspace; the final method must follow that prefix.
// Pass the extra captured before the display query. Driver selection uses the
// current viewport, even if a query callback replaced that extra in its map.
// Read the length after initialization, allowing that callback to update it.
// Only the initializer's low WORD gates the prefix. Stage errors are ignored,
// the final cleanup always runs, and VecTable is never rebound. Caller owns
// locking, workspace interpretation and subsequent field/intermediate updates.
void pcPrepareViewPortScroll(struct View* view, struct ViewPort* viewport, struct ViewPortExtra* extra,
    const struct VecInfo* info, APTR default_vectors, APTR dual_vectors, const UWORD* steps,
    APTR work, struct GfxBase* graphics);

// Complete scroll transaction for the native framebuffer owner. The supplied
// tables and any selected preparation methods use ViewPortBuildState; foreign
// workspace formats belong to their own slot-2 implementation. The caller
// supplies its preparation-prefix length, read after the initializer runs.
// Hold ActiViewCprSemaphore from eligibility checks through cleanup, live
// source selection and updates to this library's prepared/merged collections.
// No display generation is constructed or activated. The public dispatcher
// selects the native slot-2 method, which supplies the native table and prefix.
void pcScrollNativeViewPort(struct ViewPort* viewport, APTR default_vectors, APTR dual_vectors,
    const UWORD* steps, struct GfxBase* graphics, ULONG caller_d3, ULONG caller_d5);

// Native preparation ownership, after validating the display. A borrowed
// extra is retained; a newly allocated one belongs to this build sequence.
UWORD pcAcquireViewPortExtra(struct ViewPort* viewport, struct ViewPortExtra** extra,
    struct GfxBase* graphics);
ULONG pcReleaseViewPortExtra(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, struct GfxBase* graphics asm("a6"));
