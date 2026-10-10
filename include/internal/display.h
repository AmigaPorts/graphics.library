/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <graphics/displayinfo.h>
#include "internal/view.h"

struct GfxBase;

// Revision-dependent query updates shared by initial mode construction and
// later capability changes. These operate only on software descriptions;
// they neither discover hardware nor select a display owner.
void pcApplyDisplayRevision(struct DisplayInfo* display, const struct GfxBase* graphics);
UWORD pcLimitDisplayDepth(UWORD depth, UBYTE maximum, const struct GfxBase* graphics);
// Complete the database phase after a changed revision selection. Query
// transfers go through public vectors so monitor/P96 hooks retain ownership.
void pcRefreshDisplayRevision(UBYTE selected, struct GfxBase* graphics);

// Common prefix of the display database's root, monitor and mode records.
// Monitor keys occupy the low WORD of id; modes combine their parent's key
// and their own low WORD. Query data follows this prefix in complete records.
// The public DisplayInfoHandle remains opaque to callers.
struct GraphicsDisplayRecord {
    struct GraphicsDisplayRecord* next;
    struct GraphicsDisplayRecord* previous;
    struct GraphicsDisplayRecord* children;
    struct GraphicsDisplayRecord* parent;
    ULONG id;
};

// Query callbacks use stack arguments and retain the library base in A6.
// The pointers describe the original transfer, before its built-in processing;
// the callback may change the returned byte count through result.
typedef void (*GraphicsDisplayQueryFunction)(struct GraphicsDisplayRecord** record,
    UBYTE** source, ULONG* destination_remaining, ULONG* tag, ULONG* display_id,
    UBYTE** destination, ULONG* source_remaining, ULONG* result, struct GfxBase* graphics asm("a6"));

struct GraphicsDisplayDataRecord {
    struct GraphicsDisplayRecord record;
    struct TagItem query;
    ULONG reserved;
    GraphicsDisplayQueryFunction get_data;
    GraphicsDisplayQueryFunction set_data;
    struct Rectangle bounds;
};

struct GraphicsDisplayLookup {
    ULONG id;
    struct GraphicsDisplayRecord* monitor;
    struct GraphicsDisplayDataRecord* mode;
};

// Resolve a request against the current database. Normalize default/double-scan
// IDs under MonitorListSemaphore, retain exact list order, and refresh an
// existing mode only when its display query reports NOMONITOR. Return the
// normalized ID and parent separately so the mode provider can distinguish a
// missing family from a missing child. This never creates modes or publishes
// FindDisplayInfo; the public lookup must also supply lazy native families.
void pcLookupDisplayInfo(struct GraphicsDisplayLookup* lookup, ULONG display_id, struct GfxBase* graphics);

// Stored dimensions use monitor coordinates, not the pixel rectangles returned
// by GetDisplayInfoData. Text/standard overscan belong to the parent monitor's
// query. Keep the trailing tag slot on an eight-byte query boundary.
struct GraphicsDisplayDimensions {
    struct QueryHeader Header;
    UWORD MaxDepth;
    UWORD MinRasterWidth, MinRasterHeight;
    UWORD MaxRasterWidth, MaxRasterHeight;
    struct {
        LONG MinX, MinY, MaxX, MaxY;
    } bounds[3]; // Nominal, maximum overscan, video overscan.
    UBYTE pad[6];
    ULONG reserved[2];
};

struct NativeDisplayModeData {
    struct DisplayInfo display;
    struct GraphicsDisplayDimensions dimensions;
    struct NameInfo name;
    struct VecInfo vectors;
    struct ViewPortBuildMode build;
    BOOL named; // DTAG_NAME is absent until a provider supplies a name.
};

struct NativeDisplayMode {
    struct GraphicsDisplayDataRecord record;
    struct NativeDisplayModeData data;
};

// Construct a complete, unpublished native mode. Query headers, continuations,
// ID and VecInfo.Data are owned here; all other description fields are copied.
// The driver vector table, monitor and prepared-display owner remain borrowed.
// DTAG_NAME is linked only when named is set. No caller description is
// retained, and no existing database is modified.
// The owner serializes construction against changes to the parent monitor.
// The owner publishes the returned record only after its monitor/driver is
// ready. Free only unpublished or detached records, after all readers finish.
struct NativeDisplayMode* pcAllocNativeDisplayMode(const struct NativeDisplayModeData* description,
    struct GraphicsDisplayRecord* monitor, UWORD key, struct GfxBase* graphics);
void pcFreeNativeDisplayMode(struct NativeDisplayMode* mode, struct GfxBase* graphics);

struct NativeDisplayModeEntry {
    UWORD key;
    struct NativeDisplayModeData data;
};

#define STANDARD_DISPLAY_MODE_COUNT 34

// Describe the legacy/default, NTSC or PAL software mode family. The caller
// provides room for STANDARD_DISPLAY_MODE_COUNT entries. Return the populated
// count in original enumeration order, or zero for an unsupported family.
// These are Amiga display coordinates and capabilities, not PC video timings.
// Driver vectors, preparation ownership and MonitorSpec binding are supplied
// by the native display owner before allocation/publication.
ULONG pcDescribeStandardDisplayFamily(UWORD key, struct MonitorInfo* monitor,
    struct NativeDisplayModeEntry* entries, struct GfxBase* graphics);

struct NativeDisplayFamily {
    struct GraphicsDisplayDataRecord record;
    struct MonitorInfo monitor;
    ULONG count;
    struct NativeDisplayMode* modes[];
};

// Library-lifetime ownership is independent of the public database links.
// Monitor drivers may append their own records without transferring ownership.
struct NativeDisplayDatabase {
    struct GraphicsDisplayRecord root;
    struct NativeDisplayFamily* families[3];
};

BOOL pcInitDisplayDatabase(struct GfxBase* graphics);
// Startup failure cleanup only, before publication or display readers exist.
void pcFreeDisplayDatabase(struct GfxBase* graphics);
DisplayInfoHandle pcFindDisplayInfo(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"));

// Construct a detached monitor family in provider order. Its query and every
// mode are owned copies; MonitorSpec and driver/renderer owners remain borrowed.
// The monitor query retains its supplied low ID word, with the family key
// replacing its high word. No MonitorSpec initialization, database publication or default-mode selection
// is performed here. The provider must supply unique keys within the family.
// Once ready, publish record through AddDisplayInfo under the display owner's
// serialization. Free only after detachment and after all readers finish.
// Allocation ownership is separate from the public database links, which may
// subsequently also contain records supplied by other monitor drivers.
struct NativeDisplayFamily* pcAllocNativeDisplayFamily(const struct MonitorInfo* monitor, UWORD key,
    const struct NativeDisplayModeEntry* entries, ULONG count, struct GfxBase* graphics);
void pcFreeNativeDisplayFamily(struct NativeDisplayFamily* family, struct GfxBase* graphics);

ULONG pcNextDisplayInfo(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"));
void pcAddDisplayInfo(APTR display_record asm("a0"), struct GfxBase* graphics asm("a6"));
void pcAddDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
ULONG pcGetDisplayInfoData(DisplayInfoHandle handle asm("a0"), APTR buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
void pcSetDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
// Internal transfer count, including the query callback's adjustment. The
// public SetDisplayInfoData vector remains void as specified by its SFD.
ULONG pcWriteDisplayInfoData(DisplayInfoHandle handle, UBYTE* buffer, ULONG size,
    ULONG tag, ULONG display_id, struct GfxBase* graphics);
void pcUpdateDisplayAvailability(struct GraphicsDisplayDataRecord* record, struct GfxBase* graphics);
