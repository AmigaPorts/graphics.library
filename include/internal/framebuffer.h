/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>

enum FramebufferFormat {
    FRAMEBUFFER_RGB565,
    FRAMEBUFFER_RGB555,
    FRAMEBUFFER_RGB888,
    FRAMEBUFFER_XRGB8888,
    FRAMEBUFFER_XBGR8888
};

struct FramebufferMode {
    ULONG address, size;
    ULONG width, height, stride;
    enum FramebufferFormat format;
    UBYTE pixel_bytes;
    BOOL little_endian;
};

struct ExecBase;

struct GraphicsFramebuffer {
    struct FramebufferMode mode;
    UBYTE* pixels;
    ULONG stride;
    ULONG size;
};

// The display owner serializes these operations. Start with zeroed state;
// retain it until close succeeds. Opening discovers Emu68's already-active
// framebuffer and allocates staging storage without changing visible pixels.
BOOL pcOpenGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase);
BOOL pcCloseGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, struct ExecBase* SysBase);

// Transfer staging pixels in the discovered framebuffer format, like the
// standalone applications. Call only while the native boot display is active;
// P96 mode activation must stop native presentation. This backend neither
// claims PCI devices nor changes hardware modes or installs a refresh task.
BOOL pcPresentGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer);

// Fill staging pixels from a 0x00RRGGBB color, retaining row padding. Validate
// format and storage before writes. This neither presents nor chooses the
// native display's background/border policy.
BOOL pcFillGraphicsFramebuffer(struct GraphicsFramebuffer* framebuffer, ULONG rgb);

// Fill a clipped staging rectangle, leaving other pixels and padding intact.
// Signed origins and unsigned extents follow the scaling/placement helpers.
// Invalid storage rejects before writes; empty/offscreen rectangles succeed.
BOOL pcFillGraphicsFramebufferArea(struct GraphicsFramebuffer* framebuffer, ULONG rgb,
    LONG x, LONG y, ULONG width, ULONG height);

struct GraphicsFramebufferScaleArea {
    ULONG source_x, source_y;
    ULONG width, height;
    LONG dest_x, dest_y;
    BYTE x_shift, y_shift;
};

// Place decoded pixels into another staging image of the same format. Shifts
// -2 through 2 select independent power-of-two pixel scales; shrinking selects
// the first pixel in each group, including an incomplete final group. Clipping
// retains the original sampling phase, even within an enlarged source pixel.
// The entire source rectangle must be readable. Storage must not overlap.
// Invalid layouts leave destination unchanged; valid empty/offscreen areas
// succeed without writes. The native owner chooses the scale and placement
// from its display geometry; this does not choose a mode, infer monitor timing,
// present pixels, or change the caller's descriptors.
BOOL pcScaleFramebufferArea(struct GraphicsFramebuffer* destination,
    const struct GraphicsFramebuffer* source, const struct GraphicsFramebufferScaleArea* area);

// Place a decoded field into alternating staging rows. Long fields write even
// rows, short fields write odd rows; the other field and row padding remain
// untouched. dest_y and y_shift are in field lines, before interleaving. This
// retains vertical clipping/sampling phase and handles odd output heights.
// Source bounds, format, scaling and storage rules match the area operation.
// The display owner supplies the selected field; this does not infer it from
// mutable View state, advance source rows, allocate storage or present pixels.
BOOL pcScaleFramebufferField(struct GraphicsFramebuffer* destination,
    const struct GraphicsFramebuffer* source, const struct GraphicsFramebufferScaleArea* area, BOOL short_field);

// One native sprite image, after the display owner has resolved its control
// words, visibility and coordinate system. Each row contains two big-endian
// bitplanes of width/8 bytes each. Control/terminator words are not pixel data.
// Empty images disable that channel. Storage is borrowed and must not overlap
// the destination. Widths are 16, 32 or 64; independent scales use -2 through 2
// as in pcScaleFramebufferArea, preserving phase when clipped.
struct GraphicsFramebufferSprite {
    const UBYTE* data;
    ULONG size, stride;
    UWORD width, height;
    LONG dest_x, dest_y;
    BYTE x_shift, y_shift;
};

struct GraphicsFramebufferSprites {
    struct GraphicsFramebufferSprite images[8];
    UBYTE attached_pairs; // Bit n joins channels 2*n and 2*n+1.
    UBYTE even_bank, odd_bank; // Resolved 16-color banks, 0 through 15.
};

// Optional, already resolved playfield/border occlusion: bit n blocks sprite
// pair n at that destination pixel. The visible playfield determines blocking;
// this cannot be inferred from their final RGB values. NULL pixels mean no
// occlusion. Otherwise describe one byte per framebuffer pixel, with padding.
// The composition owner supplies this state; it is neither a hardware register
// image nor a public API. This operation does not allocate an occlusion image.
struct GraphicsSpriteOcclusion {
    const UBYTE* pixels;
    ULONG stride, size;
};

// Project an existing occlusion image with the same coordinates and sampling
// phase as pcScaleFramebufferArea. Dimensions come from the associated color
// staging buffers, but mask rows have independent strides. Both masks must be
// present and disjoint from each other and from the color buffers. No color
// pixels are modified. Clipped pixels and padding remain untouched; invalid
// layouts leave the output unchanged. The display owner supplies storage.
BOOL pcScaleSpriteOcclusion(const struct GraphicsFramebuffer* destination,
    UBYTE* output, ULONG stride, ULONG size, const struct GraphicsFramebuffer* source,
    const struct GraphicsSpriteOcclusion* input, const struct GraphicsFramebufferScaleArea* area);

// Compose all eight sprite channels over existing staging pixels. Lower
// channel numbers win; attached pairs combine their independent two-bit pixels
// using the odd color bank. Zero pixels are transparent before palette lookup.
// Occlusion blocks the selected pair without exposing a lower-priority sprite.
// All layouts are checked before writes; invalid inputs leave staging intact.
// No image ownership, restore/background capture, presentation or driver-table
// activation is performed. The native display owner serializes the operation.
BOOL pcRenderSpriteFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const ULONG* palette,
    const struct GraphicsSpriteOcclusion* occlusion);

// Restrict composition to this destination rectangle, clipped to framebuffer
// bounds. Sprite positions, scaling phase and occlusion coordinates remain
// relative to the complete framebuffer. The display owner uses separate areas
// for viewport/border regions with different palettes or visibility policies.
// An empty/offscreen area performs no writes; source validation and ownership
// requirements are the same as for whole-frame composition.
BOOL pcRenderSpriteFramebufferArea(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferSprites* sprites, const ULONG* palette,
    const struct GraphicsSpriteOcclusion* occlusion,
    LONG dest_x, LONG dest_y, ULONG width, ULONG height);

struct BitMap;
struct ColorMap;
struct GfxBase;

// Palette state for a prepared native display, separate from the caller's
// editable ColorMap. The display owner initializes it from the preceding
// display/global palette and selects which entries an update replaces.
struct GraphicsFramebufferPalette {
    ULONG colors[256];
};

struct PlayfieldPaletteLayout {
    UBYTE index_xor;
    UBYTE second_offset;
};

// Capture the effective native palette addressing selected by ColorMap's
// playfield bases. Keep normalization separate from rendering and from the
// original tables: legacy/absent maps select different second-field offsets.
void pcReadPlayfieldPaletteLayout(struct PlayfieldPaletteLayout* layout, const struct ColorMap* map);

// Resolve selected prepared colors over the preceding palette. Unselected
// entries come from preceding, not from cleared capture storage. Output may
// alias either input. This does not choose display/list ordering or activation.
void pcResolvePreparedPalette(struct GraphicsFramebufferPalette* output,
    const struct GraphicsFramebufferPalette* preceding, const struct GraphicsFramebufferPalette* prepared,
    const ULONG* mask);

// Import an explicit range through public GetRGB32 into effective RGB8 colors.
// Unselected entries retain their previous values. A zero range makes no call;
// an invalid destination range leaves all state unchanged. As with GetRGB32,
// the caller supplies readable ColorMap storage for the selected source range.
// This does not activate a View, render pixels or claim ownership of scanout.
BOOL pcReadFramebufferPalette(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    ULONG first, ULONG count, struct GfxBase* graphics);

// Read only selected entries, retaining the preceding display's other colors.
// mask uses the eight MSB-first groups produced by pcViewPortColorMask.
BOOL pcReadFramebufferPaletteMask(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    const ULONG* mask, struct GfxBase* graphics);

// Capture colors emitted by native ViewPort preparation. Unlike the public
// GetRGB32 query above, intermediate display lists use the low-nibble table
// only for a V39 ColorMap. A missing map supplies the original 32 defaults.
// The mask selects readable entries; unselected colors retain their values.
// This is preparation, not presentation or a replacement for GetRGB32.
void pcReadPreparedPalette(struct GraphicsFramebufferPalette* palette, const struct ColorMap* map,
    const ULONG* mask);

struct GraphicsFramebufferArea {
    ULONG source_x, source_y;
    ULONG dest_x, dest_y;
    ULONG width, height;
};

struct GraphicsFramebufferPlacement {
    LONG source_x, source_y;
    LONG dest_x, dest_y;
    ULONG width, height;
};

// Clip a one-to-one pixel placement to both bitmap and staging bounds, keeping
// the source/destination relationship. The owner first converts View/monitor
// coordinates into framebuffer pixels. Invalid inputs leave area unchanged;
// a valid fully clipped placement returns TRUE with a zeroed area. No pixel,
// palette or hardware access occurs, and neither input structure is modified.
BOOL pcClipFramebufferArea(const struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const struct GraphicsFramebufferPlacement* placement, struct GraphicsFramebufferArea* area);

// Clip two independently scrolled placements to their common destination and
// both bitmap bounds. The resulting areas cover exactly the same pixels for
// dual/combined decoding, with each source origin advanced independently.
// This selects only the region backed by both rasters; it neither fills pixels
// outside that intersection nor assigns them a transparency/border policy.
// Both outputs must be distinct. Invalid inputs leave both unchanged; a valid
// empty intersection zeros both. Coordinates are already in framebuffer pixels.
BOOL pcClipFramebufferPair(const struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const struct GraphicsFramebufferPlacement* first_placement,
    const struct GraphicsFramebufferPlacement* second_placement,
    struct GraphicsFramebufferArea* first_area, struct GraphicsFramebufferArea* second_area);

// Convert indexed planar pixels into staging memory only. The display owner
// supplies a nonoverlapping bitmap, a 0x00RRGGBB palette, and a rectangle
// already clipped to both images. index_xor selects the prepared playfield's
// palette addressing, after decoding the planar index. With a nonzero mask,
// provide all 256 palette entries; with zero, 2^Depth entries suffice. Resolve
// inherited colors before calling. Reject invalid rectangles/layouts without
// modifying staging storage. No hardware access,
// scaling, HAM/dual-playfield decoding, sprite composition or mode activation.
BOOL pcRenderIndexedFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, UBYTE index_xor);

enum GraphicsEhbPrecision {
    GRAPHICS_EHB_RGB4,
    GRAPHICS_EHB_RGB8
};

// Decode a six-plane EHB bitmap using effective RGB8 palette entries.
// RGB4 palettes must already have their nibbles replicated to RGB8; dimming
// takes place at the selected precision, before output-format quantization.
// Apply index_xor before decoding: addresses 32..63 select half-intensity
// colors 0..31; other addresses select ordinary palette entries. Zero needs
// only 32 readable colors, while a nonzero mask requires all 256 entries.
// As with indexed conversion, this writes only the clipped staging rectangle.
BOOL pcRenderEhbFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision,
    UBYTE index_xor);

// Compose the two RasInfo bitmaps into staging memory. Each has up to four
// planes, its own source rectangle and resolved RGB8 palette. Both rectangles
// must cover the same destination. Index zero is transparent in either field;
// when both are zero, use first_palette[0]. second_in_front corresponds to
// ViewPort PFBA. The owner resolves palette banks before calling (including
// the classic second-playfield bank starting at color 8).
BOOL pcRenderDualFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front);

// A dual-playfield ViewPort without a second bitmap uses alternating planes
// of its first bitmap: slots 0,2,4,6 belong to the first field, 1,3,5,7 to the
// second. Both share its source rectangle and row stride. Validate clipping
// against the original bitmap's logical width, including interleaved storage.
// Palette and priority semantics are the same as the two-raster path above.
BOOL pcRenderPlanarDualFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* first_palette, const ULONG* second_palette, const struct GraphicsFramebufferArea* area,
    BOOL second_in_front);

// With CMAF_DUALPF_DISABLE, the two RasInfo bitmaps supply alternating bits
// of one index, rather than transparent playfields. The sum of their depths
// selects the number of planes (at most eight): even bits come from first,
// odd bits from second. Each selected plane slot must contain readable storage
// or a constant plane, including slots beyond either bitmap's own Depth when
// the depths differ. Both clipped rectangles cover the same destination.
// Apply index_xor to the combined index, not to either individual field.
// Palette size follows the indexed renderer, using the sum of both depths.
BOOL pcRenderCombinedFramebuffer(struct GraphicsFramebuffer* framebuffer,
    const struct BitMap* first, const struct BitMap* second, const ULONG* palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area, UBYTE index_xor);


enum GraphicsHamFormat {
    GRAPHICS_HAM6,
    GRAPHICS_HAM8,
    GRAPHICS_HAM8_PLANAR
};

// Decode HAM into staging memory, with the same bounds/ownership contract as
// indexed conversion. HAM6 accepts five or six planes and 16 palette entries;
// HAM8 accepts seven/eight planes and 64 entries. Palettes contain effective RGB8
// colors (expand legacy RGB4 before calling). HAM6 modifications expand RGB4;
// HAM8 modifications retain the low two component bits of the held color.
// GRAPHICS_HAM8 takes command bits from planes 0/1 in display order.
// GRAPHICS_HAM8_PLANAR takes them from bitmap planes 6/7, matching the native
// graphics driver's HAM8 permutation. At depth seven it still reads slot 7
// and omits slot 5: supply readable or constant planes in slots 0..4,6,7.
// Display-order HAM8 instead omits its final data plane. Validate interleaved
// bounds using the original descriptor. This selects input layout only; it does
// not select a ViewPort mode or modify the caller's plane descriptors.
// index_xor is in display-plane order, including for HAM8_PLANAR. It selects
// both the operation and palette index. HAM6 component modifications use the
// unmodified nibble; HAM8 modifications use the XORed six bits. High XOR bits
// outside the six HAM6 planes are ignored. Initial held color is palette[0].
// origin_x is the source pixel at the displayed scanline's left edge before
// output clipping. Decode from there, resetting to palette[0] on each row.
BOOL pcRenderHamFramebuffer(struct GraphicsFramebuffer* framebuffer, const struct BitMap* bitmap,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format,
    UBYTE index_xor);

// Internal prepared-display input. Each plane has an independent storage
// origin and pitch; width/height describe the common readable rectangle in
// raster coordinates, not a new BitMap allocation. The owner establishes that
// every selected plane backs that rectangle (or is a NULL/all-one constant).
// Decoders retain their prepared depth and plane order. No caller descriptors
// or pixel storage are owned, and none of this changes the public BitMap ABI.
struct GraphicsFramebufferPlane {
    const UBYTE* data;
    ULONG stride;
    ULONG source_x, source_y;
    LONG row_offset; // Byte adjustment, independent of logical source coordinates.
};

struct GraphicsFramebufferRaster {
    ULONG width, height;
    UBYTE depth;
    struct GraphicsFramebufferPlane planes[8];
};

// Convert a software BitMap without reading pixels. Capture all eight source
// slots, reject unsupported layouts, and preserve interleaved logical bounds.
BOOL pcReadFramebufferRaster(struct GraphicsFramebufferRaster* raster, const struct BitMap* bitmap);

// The same decoders and rectangle contracts as the BitMap entry points above,
// with source coordinates relative to the raster's common readable rectangle.
// A plane's own origin is added when reading it. HAM origin_x is in this same
// coordinate space, so prefix decoding keeps each plane's independent phase.
BOOL pcRenderIndexedRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, UBYTE index_xor);
BOOL pcRenderEhbRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsEhbPrecision precision,
    UBYTE index_xor);
BOOL pcRenderHamRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, ULONG origin_x, enum GraphicsHamFormat format,
    UBYTE index_xor);
// Decode one successive row span using the color held before its first source
// pixel. The owner initializes that color at row start, then carries it across
// palette changes. No prefix is replayed. Decode offscreen spans into staging
// as well, so later visible pixels inherit their state. Empty spans preserve
// the held color; invalid input changes neither pixels nor state.
BOOL pcRenderHamRasterSpan(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* palette, const struct GraphicsFramebufferArea* area, enum GraphicsHamFormat format,
    UBYTE index_xor, ULONG* held_color);
BOOL pcRenderDualRaster(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second,
    const ULONG* first_palette, const ULONG* second_palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    BOOL second_in_front);
BOOL pcRenderPlanarDualRaster(struct GraphicsFramebuffer* framebuffer, const struct GraphicsFramebufferRaster* raster,
    const ULONG* first_palette, const ULONG* second_palette, const struct GraphicsFramebufferArea* area,
    BOOL second_in_front);
BOOL pcRenderCombinedRaster(struct GraphicsFramebuffer* framebuffer,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second, const ULONG* palette,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area, UBYTE index_xor);

// Write one byte per destination pixel for sprite composition, without RGB
// palette lookup. Raw index zero is transparent, including when palette XOR
// selects a nonzero displayed color. With two fields, select the visible field
// before applying its four-bit pair mask, just as dual pixel composition does.
// A NULL second raster selects one field. Rectangles must cover the same output
// area and be clipped to readable raster bounds and the output allocation.
// Untouched pixels (including borders/padding) retain their previous masks.
// The owner supplies nonoverlapping raster/output storage and serializes reads.
BOOL pcWriteSpriteOcclusion(UBYTE* output, ULONG stride, ULONG size,
    const struct GraphicsFramebufferRaster* first, const struct GraphicsFramebufferRaster* second,
    const struct GraphicsFramebufferArea* first_area, const struct GraphicsFramebufferArea* second_area,
    UBYTE first_mask, UBYTE second_mask, BOOL second_in_front);
