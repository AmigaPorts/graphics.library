/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <graphics/displayinfo.h>

struct GfxBase;

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

ULONG pcNextDisplayInfo(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"));
void pcAddDisplayInfo(APTR display_record asm("a0"), struct GfxBase* graphics asm("a6"));
void pcAddDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
ULONG pcGetDisplayInfoData(DisplayInfoHandle handle asm("a0"), APTR buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
void pcSetDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"));
void pcUpdateDisplayAvailability(struct GraphicsDisplayDataRecord* record, struct GfxBase* graphics);
