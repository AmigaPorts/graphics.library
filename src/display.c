/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/display.h"

#include <stddef.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

static struct GraphicsDisplayRecord* findRecord(struct GraphicsDisplayRecord* record,
    UWORD key, UWORD mask, struct ExecBase* SysBase)
{
    key &= mask;
    Forbid();

    while (record && ((UWORD)record->id & mask) != key) {
        record = record->next;
    }

    Permit();

    return record;
}

ULONG pcNextDisplayInfo(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct GraphicsDisplayRecord* root = graphics->DisplayInfoDataBase;

    if (!root) {
        return INVALID_ID;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    UWORD mask = display_id == (ULONG)INVALID_ID ? 0 : 0xffff;
    struct GraphicsDisplayRecord* monitor = findRecord(root->children, display_id >> 16, mask, SysBase);
    struct GraphicsDisplayRecord* mode = NULL;

    if (monitor) {
        mode = monitor->children;

        if (mask) {
            struct GraphicsDisplayRecord* previous = findRecord(mode, display_id, mask, SysBase);

            // A missing mode within an existing monitor restarts that monitor.
            // An unknown monitor instead ends enumeration. Preserve list order,
            // including mode IDs which are not numerically sorted.
            mode = findRecord(previous ? previous->next : monitor->children, 0, 0, SysBase);
        }
    }

    // The original tries only the immediately following monitor. Two adjacent
    // empty monitor groups end this call even if a later group has modes.
    if (!mode && monitor) {
        monitor = findRecord(monitor->next, 0, 0, SysBase);

        if (monitor) {
            mode = findRecord(monitor->children, 0, 0, SysBase);
        }
    }

    if (!mode) {
        return INVALID_ID;
    }

    // The original composes the ID from the parent and the child's key, rather
    // than returning the child's cached full ID or normalizing a default mode.
    return ((ULONG)(UWORD)mode->parent->id << 16) | (UWORD)mode->id;
}

void pcAddDisplayInfo(APTR display_record asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct GraphicsDisplayRecord* root = graphics->DisplayInfoDataBase;
    struct GraphicsDisplayRecord* record = display_record;

    if (!root || !record) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();
    struct GraphicsDisplayRecord* previous = root->children;

    if (previous) {
        while (previous->next) {
            previous = previous->next;
        }
    }

    record->previous = previous;
    record->parent = root;
    record->id = ((ULONG)(UWORD)root->id << 16) | (UWORD)record->id;

    // Append without sorting or taking ownership of the record's allocation.
    // Its successor and query data are supplied by the record initializer.
    if (previous) {
        previous->next = record;
    } else {
        root->children = record;
    }

    Permit();
}

static void appendQuery(struct TagItem* list, struct TagItem* query)
{
    ULONG tag = query->ti_Tag;

    while (list && list->ti_Tag != tag) {
        switch (list->ti_Tag) {
            case TAG_DONE:
                list->ti_Data = (ULONG)query;
                list->ti_Tag = TAG_MORE;
                list = query;
                break;
            case TAG_MORE:
                list = (APTR)list->ti_Data;
                break;
            case TAG_SKIP:
                list += list->ti_Data + 1;
                break;
            default:
                ++list;
                break;
        }
    }
}

void pcAddDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"))
{
    if (!buffer) {
        return;
    }

    struct GraphicsDisplayRecord* record = handle;

    if (!record) {
        record = FindDisplayInfo(display_id);

        if (!record) {
            return;
        }
    }

    struct GraphicsDisplayRecord* owner = tag == DTAG_MNTR ? record->parent : record;

    if (!owner) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    struct TagItem* list = (APTR)(owner + 1);
    struct TagItem* destination = FindTagItem(tag, list);

    if (!destination) {
        destination = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

        if (!destination) {
            return;
        }
    }

    const struct QueryHeader* header = (APTR)buffer;
    BOOL query_header = header->SkipID == TAG_SKIP;
    ULONG available = query_header ? 24 + 8 * header->Length : 8;
    ULONG count = size < available ? size : available;
    UBYTE* output = (APTR)destination;

    // Retain the original WORD-sized transfer loop, including its wrap for
    // oversized requests. The source header bounds the transfer, not the old
    // destination header. Callers own the size of an existing record's storage.
    for (ULONG byte = 0; byte < (UWORD)count; ++byte) {
        output[byte] = buffer[byte];
    }

    if (query_header) {
        ((struct QueryHeader*)destination)->DisplayID = record->id;
    }

    appendQuery(list, destination);
}

struct QueryTransfer {
    UBYTE* source;
    UBYTE* destination;
    ULONG source_remaining;
    ULONG destination_remaining;
};

static ULONG copyQuery(struct QueryTransfer* transfer, ULONG count)
{
    if (count > transfer->source_remaining) {
        count = transfer->source_remaining;
    }

    if (count > transfer->destination_remaining) {
        count = transfer->destination_remaining;
    }

    // The original copy loop advances pointers by a WORD-sized byte count,
    // while accounting for the full ULONG count in the remaining lengths.
    for (ULONG byte = 0; byte < (UWORD)count; ++byte) {
        *transfer->destination++ = *transfer->source++;
    }

    transfer->source_remaining -= count;
    transfer->destination_remaining -= count;

    return count;
}

static WORD pixelCoordinate(LONG coordinate, WORD resolution)
{
    // DIVS.W also preserves the original low WORD on quotient overflow.
    __asm__ volatile("divs.w %1,%0" : "+d"(coordinate) : "d"(resolution) : "cc");

    return coordinate;
}

void pcUpdateDisplayAvailability(struct GraphicsDisplayDataRecord* record, struct GfxBase* graphics)
{
    struct Library* UtilityBase = graphics->UtilBase;
    struct DisplayInfo* display = (APTR)FindTagItem(DTAG_DISP, (APTR)(&record->record + 1));

    if (!display) {
        return;
    }

    // The stored display query retains the original mode-control LONG in its
    // public padding. Copy it to the record cache before deriving availability.
    // This is software metadata, not a read of any chipset register.
    ULONG modes = (ULONG)display->pad[0] << 24 | (ULONG)display->pad[1] << 16 |
        (ULONG)display->pad[2] << 8 | display->pad[3];
    record->reserved = modes;

    if (display->PropertyFlags & (DIPF_IS_ECS | DIPF_IS_AA)) {
        display->NotAvailable |= DI_AVAIL_NOCHIPS;
    }

    if ((graphics->ChipRevBits0 & GFXF_HR_DENISE) && (display->PropertyFlags & DIPF_IS_ECS)) {
        display->NotAvailable &= ~DI_AVAIL_NOCHIPS;
    }

    if ((graphics->ChipRevBits0 & GFXF_AA_LISA) && (display->PropertyFlags & DIPF_IS_AA)) {
        display->NotAvailable &= ~DI_AVAIL_NOCHIPS;

        // Retain the mode/memory-class restrictions of the shared database.
        // A native provider supplies its own properties; this does not claim
        // that PC hardware has an Amiga chipset or select a hardware backend.
        if ((modes & (HAM | EXTRA_HALFBRITE)) &&
            !((modes & SUPERHIRES) && graphics->MemType == BANDWIDTH_4X) &&
            !((modes & HIRES) && graphics->MemType != BANDWIDTH_1X) && (modes & (HIRES | SUPERHIRES))) {
            display->NotAvailable |= DI_AVAIL_NOCHIPS;
        }
    }

    const UBYTE* dimensions = (APTR)FindTagItem(DTAG_DIMS, (APTR)(&record->record + 1));

    if (dimensions) {
        // The cached pixel bounds come from the third native LONG rectangle,
        // VideoOScan. Keep its signed division and quotient-overflow behavior.
        const LONG* rectangle = (APTR)(dimensions + offsetof(struct DimensionInfo, Nominal) + 8 * sizeof(LONG));
        record->bounds.MinX = pixelCoordinate(rectangle[0], display->Resolution.x);
        record->bounds.MaxX = pixelCoordinate(rectangle[2], display->Resolution.x);
        record->bounds.MinY = pixelCoordinate(rectangle[1], display->Resolution.y);
        record->bounds.MaxY = pixelCoordinate(rectangle[3], display->Resolution.y);
    }

    if ((graphics->system_bplcon0 & GENLOCK_VIDEO) && !(display->PropertyFlags & DIPF_IS_GENLOCK)) {
        display->NotAvailable |= DI_AVAIL_NOTWITHGENLOCK;
    }

    if (record->record.parent) {
        const struct MonitorInfo* monitor = (APTR)FindTagItem(DTAG_MNTR, (APTR)(record->record.parent + 1));

        if (monitor && monitor->Mspc) {
            display->NotAvailable &= ~DI_AVAIL_NOMONITOR;
        }
    }
}

static BOOL readRectangle(struct QueryTransfer* transfer, const volatile struct DisplayInfo* display)
{
    if (transfer->destination_remaining < sizeof(struct Rectangle) ||
        transfer->source_remaining < 4 * sizeof(LONG)) {
        return FALSE;
    }

    const volatile LONG* source = (APTR)transfer->source;
    volatile struct Rectangle* destination = (APTR)transfer->destination;
    destination->MinX = pixelCoordinate(source[0], display->Resolution.x);
    destination->MaxX = pixelCoordinate(source[2], display->Resolution.x);
    destination->MinY = pixelCoordinate(source[1], display->Resolution.y);
    destination->MaxY = pixelCoordinate(source[3], display->Resolution.y);
    transfer->source += 4 * sizeof(LONG);
    transfer->source_remaining -= 4 * sizeof(LONG);
    transfer->destination += sizeof(struct Rectangle);
    transfer->destination_remaining -= sizeof(struct Rectangle);

    return TRUE;
}

static ULONG readDimensions(struct GraphicsDisplayRecord* record, struct QueryTransfer* transfer,
    struct GfxBase* graphics)
{
    struct Library* UtilityBase = graphics->UtilBase;
    struct DimensionInfo* output = (APTR)transfer->destination;
    ULONG count = copyQuery(transfer, offsetof(struct DimensionInfo, Nominal));
    const struct DisplayInfo* display = (APTR)FindTagItem(DTAG_DISP, (APTR)(record + 1));

    if (!display || !display->Resolution.x || !display->Resolution.y) {
        return count;
    }

    struct GraphicsDisplayRecord* monitor_record = record->parent;

    // Native records hold LONG coordinates in monitor units. Return the three
    // mode rectangles in pixels, then the monitor's text and standard bounds.
    for (unsigned rectangle = 0; rectangle < 3; ++rectangle) {
        if (!readRectangle(transfer, display)) {
            return count;
        }

        count += sizeof(struct Rectangle);
    }

    if (!monitor_record) {
        return count;
    }

    const struct QueryHeader* monitor = (APTR)FindTagItem(DTAG_MNTR, (APTR)(monitor_record + 1));

    if (!monitor) {
        return count;
    }

    // The stored monitor query uses the public padding for these two LONG
    // rectangles; they are not the ViewPositionRange in MonitorInfo.
    transfer->source = (UBYTE*)monitor + offsetof(struct MonitorInfo, pad);
    transfer->source_remaining = 24 + 8 * monitor->Length - offsetof(struct MonitorInfo, pad);

    if (!readRectangle(transfer, display)) {
        return count;
    }

    count += sizeof(struct Rectangle);

    if (readRectangle(transfer, display)) {
        count += sizeof(struct Rectangle);
    }

    // Keep the original write order and short-buffer behavior: after the text
    // rectangle succeeds, both horizontal limits are clamped, even when the
    // caller's remaining length prevented copying the standard rectangle.
    if (output->TxtOScan.MaxX > output->MaxOScan.MaxX) {
        output->TxtOScan.MaxX = output->MaxOScan.MaxX;
    }

    if (output->StdOScan.MaxX > output->MaxOScan.MaxX) {
        output->StdOScan.MaxX = output->MaxOScan.MaxX;
    }

    return count;
}

ULONG pcGetDisplayInfoData(DisplayInfoHandle handle asm("a0"), APTR buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"))
{
    if (!buffer) {
        return 0;
    }

    struct GraphicsDisplayRecord* record = handle;

    if (!record) {
        record = FindDisplayInfo(display_id);

        if (!record) {
            return 0;
        }
    }

    struct GraphicsDisplayRecord* owner = tag == DTAG_MNTR ? record->parent : record;

    if (!owner) {
        return 0;
    }

    struct Library* UtilityBase = graphics->UtilBase;
    GraphicsDisplayQueryFunction callback = ((struct GraphicsDisplayDataRecord*)owner)->get_data;
    const struct QueryHeader* query = (APTR)FindTagItem(tag, (APTR)(owner + 1));

    if (!query) {
        return 0;
    }

    struct QueryTransfer original = {
        .source = (UBYTE*)query,
        .destination = buffer,
        .source_remaining = query->SkipID == TAG_SKIP ? 24 + 8 * query->Length : 8,
        .destination_remaining = size
    };
    struct QueryTransfer transfer = original;
    ULONG result;

    if (tag == DTAG_DIMS) {
        result = readDimensions(record, &transfer, graphics);
    } else {
        result = copyQuery(&transfer, 16 + 8 * query->Length);
    }

    if (callback) {
        callback(&record, &original.source, &original.destination_remaining, &tag, &display_id,
            &original.destination, &original.source_remaining, &result, graphics);
    }

    return result;
}

static BOOL writeRectangle(struct QueryTransfer* transfer, const volatile struct DisplayInfo* display)
{
    // SetDisplayInfoData accounts for the caller's pixel buffer in the size
    // counter and the expanded native record in the header-derived counter.
    if (transfer->destination_remaining < sizeof(struct Rectangle) ||
        transfer->source_remaining < 4 * sizeof(LONG)) {
        return FALSE;
    }

    const volatile struct Rectangle* source = (APTR)transfer->source;
    volatile LONG* destination = (APTR)transfer->destination;
    destination[0] = (LONG)source->MinX * display->Resolution.x;
    destination[2] = (LONG)(WORD)(source->MaxX + 1) * display->Resolution.x - 1;
    destination[1] = (LONG)source->MinY * display->Resolution.y;
    destination[3] = (LONG)(WORD)(source->MaxY + 1) * display->Resolution.y - 1;
    transfer->source += sizeof(struct Rectangle);
    transfer->destination += 4 * sizeof(LONG);
    transfer->destination_remaining -= sizeof(struct Rectangle);
    transfer->source_remaining -= 4 * sizeof(LONG);

    return TRUE;
}

static ULONG writeDimensions(struct GraphicsDisplayRecord* record, struct QueryTransfer* transfer,
    struct GfxBase* graphics)
{
    struct Library* UtilityBase = graphics->UtilBase;
    ULONG count = copyQuery(transfer, offsetof(struct DimensionInfo, Nominal));
    const struct DisplayInfo* display = (APTR)FindTagItem(DTAG_DISP, (APTR)(record + 1));

    if (!display || !display->Resolution.x || !display->Resolution.y) {
        return count;
    }

    struct GraphicsDisplayRecord* monitor_record = record->parent;

    for (unsigned rectangle = 0; rectangle < 3; ++rectangle) {
        if (!writeRectangle(transfer, display)) {
            return count;
        }

        count += sizeof(struct Rectangle);
    }

    if (!monitor_record) {
        return count;
    }

    struct QueryHeader* monitor = (APTR)FindTagItem(DTAG_MNTR, (APTR)(monitor_record + 1));

    if (!monitor) {
        return count;
    }

    transfer->destination = (UBYTE*)monitor + offsetof(struct MonitorInfo, pad);
    transfer->source_remaining = 24 + 8 * monitor->Length - offsetof(struct MonitorInfo, pad);

    for (unsigned rectangle = 0; rectangle < 2; ++rectangle) {
        if (!writeRectangle(transfer, display)) {
            break;
        }

        count += sizeof(struct Rectangle);
    }

    return count;
}

static ULONG writeMonitor(struct GraphicsDisplayRecord* record, struct QueryTransfer* transfer,
    struct GfxBase* graphics)
{
    const struct MonitorInfo* input = (APTR)transfer->source;
    const struct MonitorInfo* output = (APTR)transfer->destination;
    copyQuery(transfer, offsetof(struct MonitorInfo, ViewPosition));

    // The original compares the positions even when the caller's advertised
    // length did not include them. Retain WORD subtraction before scaling by
    // the old monitor resolution, and update children before copying the rest.
    if (input->ViewPosition.x != output->ViewPosition.x || input->ViewPosition.y != output->ViewPosition.y) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        struct Library* UtilityBase = graphics->UtilBase;
        LONG dx = (LONG)(WORD)(output->ViewPosition.x - input->ViewPosition.x) * output->ViewResolution.x;
        LONG dy = (LONG)(WORD)(output->ViewPosition.y - input->ViewPosition.y) * output->ViewResolution.y;
        struct GraphicsDisplayRecord* monitor = record->parent;
        Forbid();
        struct GraphicsDisplayRecord* child = monitor->children;

        while (child) {
            UBYTE* dimensions = (APTR)FindTagItem(DTAG_DIMS, (APTR)(child + 1));

            if (dimensions) {
                // Skip the native Nominal rectangle; shift MaxOScan and
                // VideoOScan in monitor units, retaining ULONG wrap.
                ULONG* rectangles = (APTR)(dimensions + offsetof(struct DimensionInfo, Nominal) + 4 * sizeof(LONG));

                for (unsigned point = 0; point < 4; ++point) {
                    rectangles[2 * point] += (ULONG)dx;
                    rectangles[2 * point + 1] += (ULONG)dy;
                }
            }

            const struct DisplayInfo* display = (APTR)FindTagItem(DTAG_DISP, (APTR)(child + 1));

            if (display) {
                WORD x = pixelCoordinate(dx, display->Resolution.x);
                WORD y = pixelCoordinate(dy, display->Resolution.y);
                struct Rectangle* bounds = &((struct GraphicsDisplayDataRecord*)child)->bounds;
                bounds->MinX = (UWORD)bounds->MinX + (UWORD)x;
                bounds->MinY = (UWORD)bounds->MinY + (UWORD)y;
                bounds->MaxX = (UWORD)bounds->MaxX + (UWORD)x;
                bounds->MaxY = (UWORD)bounds->MaxY + (UWORD)y;
            }

            child = child->next;
        }

        Permit();
    }

    // The callback receives this second copy's count, excluding the prefix.
    return copyQuery(transfer, offsetof(struct MonitorInfo, reserved) - offsetof(struct MonitorInfo, ViewPosition));
}

void pcSetDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"))
{
    if (!buffer) {
        return;
    }

    struct GraphicsDisplayRecord* record = handle;

    if (!record) {
        record = FindDisplayInfo(display_id);

        if (!record) {
            return;
        }
    }

    struct GraphicsDisplayRecord* owner = tag == DTAG_MNTR ? record->parent : record;

    if (!owner) {
        return;
    }

    struct Library* UtilityBase = graphics->UtilBase;
    GraphicsDisplayQueryFunction callback = ((struct GraphicsDisplayDataRecord*)owner)->set_data;
    struct QueryHeader* query = (APTR)FindTagItem(tag, (APTR)(owner + 1));

    if (!query) {
        return;
    }

    const struct QueryHeader* header = (APTR)buffer;
    struct QueryTransfer original = {
        .source = buffer,
        .destination = (UBYTE*)query,
        .source_remaining = header->SkipID == TAG_SKIP ? 24 + 8 * header->Length : 8,
        .destination_remaining = size
    };
    struct QueryTransfer transfer = original;
    ULONG result;

    if (tag == DTAG_DIMS) {
        result = writeDimensions(record, &transfer, graphics);
    } else if (tag == DTAG_MNTR) {
        result = writeMonitor(record, &transfer, graphics);
    } else {
        result = copyQuery(&transfer, 16 + 8 * header->Length);
    }

    if (callback) {
        callback(&record, &original.source, &original.destination_remaining, &tag, &display_id,
            &original.destination, &original.source_remaining, &result, graphics);
    }
}
