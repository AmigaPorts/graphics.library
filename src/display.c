/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/display.h"
#include "internal/library.h"

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

BOOL pcInitDisplayDatabase(struct GfxBase* graphics)
{
    struct NativeDisplayDatabase* database = &((struct GraphicsLibrary*)graphics)->display;

    if (graphics->DisplayInfoDataBase && graphics->DisplayInfoDataBase != &database->root) {
        return FALSE;
    }

    graphics->DisplayInfoDataBase = &database->root;

    return TRUE;
}

void pcFreeDisplayDatabase(struct GfxBase* graphics)
{
    struct NativeDisplayDatabase* database = &((struct GraphicsLibrary*)graphics)->display;

    if (graphics->DisplayInfoDataBase == &database->root) {
        graphics->DisplayInfoDataBase = NULL;
    }

    database->root = (struct GraphicsDisplayRecord){0};

    for (unsigned key = 0; key < 3; ++key) {
        pcFreeNativeDisplayFamily(database->families[key], graphics);
        database->families[key] = NULL;
    }
}

static void refreshNativeVectors(DisplayInfoHandle handle, struct VecInfo* vectors, struct GfxBase* graphics)
{
    if (vectors->Vec != (APTR)pcNativeViewPortVectors) {
        return;
    }

    struct NativeDisplayDatabase* database = &((struct GraphicsLibrary*)graphics)->display;

    for (unsigned key = 0; key < 3; ++key) {
        struct NativeDisplayFamily* family = database->families[key];

        if (!family) {
            continue;
        }

        for (ULONG index = 0; index < family->count; ++index) {
            struct NativeDisplayMode* mode = family->modes[index];

            if (handle == &mode->record && vectors->Data == &mode->data.build) {
                // Native software methods serve every revision. Only their
                // color interpretation changes; another driver's opaque data
                // must never be treated as our ViewPortBuildMode.
                mode->data.build.ehb_precision = graphics->ChipRevBits0 & GFXF_AA_LISA ?
                    GRAPHICS_EHB_RGB8 : GRAPHICS_EHB_RGB4;

                return;
            }
        }
    }
}

void pcRefreshDisplayRevision(UBYTE selected, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    union {
        struct VecInfo vectors;
        struct DisplayInfo display;
        struct DimensionInfo dimensions;
    }* query = AllocVec(sizeof(*query), MEMF_CLEAR);

    if (!query) {
        return;
    }

    // SetDisplayInfoData is a private SDK slot, omitted from public inlines.
    // Call the vector itself rather than bypassing installed query hooks.
    typedef void (*SetDataFunction)(DisplayInfoHandle handle asm("a0"), UBYTE* data asm("a1"),
        ULONG size asm("d0"), ULONG tag asm("d1"), ULONG id asm("d2"), struct GfxBase* graphics asm("a6"));
    SetDataFunction set_data = (APTR)((UBYTE*)graphics - 750);
    ULONG id = INVALID_ID;
    BOOL ehb = FALSE;

    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        DisplayInfoHandle handle = FindDisplayInfo(id);

        if (!handle || !(id & 0xffff1000UL)) {
            continue;
        }

        if (GetDisplayInfoData(handle, query, sizeof(query->vectors), DTAG_VEC, id)) {
            refreshNativeVectors(handle, &query->vectors, graphics);
            set_data(handle, (UBYTE*)query, sizeof(query->vectors), DTAG_VEC, id, graphics);
        }

        if (GetDisplayInfoData(handle, query, sizeof(query->display), DTAG_DISP, id)) {
            ehb = (query->display.PropertyFlags & DIPF_IS_EXTRAHALFBRITE) != 0;

            if (selected & GFXF_AA_LISA) {
                pcApplyDisplayRevision(&query->display, graphics);
                set_data(handle, (UBYTE*)query, sizeof(query->display), DTAG_DISP, id, graphics);
            }
        }

        // The original retains the preceding EHB decision when a display
        // query fails. Its cleared allocation is also reused for short reads.
        if (GetDisplayInfoData(handle, query, sizeof(query->dimensions), DTAG_DIMS, id)) {
            struct DimensionInfo* dimensions = &query->dimensions;

            if (!(graphics->ChipRevBits0 & GFXF_HR_AGNUS)) {
                dimensions->MaxRasterWidth = 1008;
                dimensions->MaxRasterHeight = 1024;
            }

            if (!ehb) {
                dimensions->MaxDepth = pcLimitDisplayDepth(dimensions->MaxDepth, dimensions->pad[8], graphics);
            }

            ULONG mode = id & 0xffff9020UL;

            if ((selected & GFXF_AA_LISA) && (mode == 0x19020 || mode == 0x29020)) {
                dimensions->MaxOScan.MaxX = (UWORD)dimensions->MaxOScan.MinX + 1447;
                dimensions->VideoOScan.MaxX = (UWORD)dimensions->VideoOScan.MinX + 1471;
            }

            set_data(handle, (UBYTE*)query, sizeof(*dimensions), DTAG_DIMS, id, graphics);
        }
    }

    // Availability is a direct database maintenance pass, independent of
    // public enumeration hooks, just as it is after lazy family creation.
    id = INVALID_ID;

    while ((id = pcNextDisplayInfo(id, graphics)) != (ULONG)INVALID_ID) {
        struct GraphicsDisplayDataRecord* record = pcFindDisplayInfo(id, graphics);

        if (record) {
            pcUpdateDisplayAvailability(record, graphics);
        }
    }

    FreeVec(query);
}

static void addStandardDisplayFamily(UWORD key, struct GfxBase* graphics)
{
    if (key > 2) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ULONG size = STANDARD_DISPLAY_MODE_COUNT * sizeof(struct NativeDisplayModeEntry);
    struct NativeDisplayModeEntry* entries = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!entries) {
        return;
    }

    struct MonitorInfo monitor;
    ULONG count = pcDescribeStandardDisplayFamily(key, &monitor, entries, graphics);

    for (ULONG i = 0; i < count; ++i) {
        struct NativeDisplayModeData* data = &entries[i].data;
        UWORD modes = entries[i].key;
        data->vectors.Vec = (APTR)pcNativeViewPortVectors;
        data->build.horizontal_shift = modes & SUPERHIRES ? 2 : modes & HIRES ? 1 : 0;
        data->build.scan_doubled = (modes & DOUBLESCAN) != 0;
        data->build.prepared = pcPreparedViewPorts(graphics);
        data->build.ehb_precision = graphics->ChipRevBits0 & GFXF_AA_LISA ?
            GRAPHICS_EHB_RGB8 : GRAPHICS_EHB_RGB4;
        data->build.ham8_planar = TRUE;
    }

    struct NativeDisplayFamily* family = pcAllocNativeDisplayFamily(&monitor, key, entries, count, graphics);
    FreeMem(entries, size);

    if (family) {
        struct NativeDisplayDatabase* database = &((struct GraphicsLibrary*)graphics)->display;
        database->families[key] = family;
        pcAddDisplayInfo(&family->record, graphics);
    }
}

DisplayInfoHandle pcFindDisplayInfo(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct GraphicsDisplayLookup lookup;
    pcLookupDisplayInfo(&lookup, display_id, graphics);

    if (lookup.mode || lookup.monitor || !graphics->DisplayInfoDataBase ||
        lookup.id == (ULONG)INVALID_ID) {
        return lookup.mode;
    }

    // Only legacy IDs or extended IDs with a monitor key trigger the original
    // lazy provider. An existing family is never repopulated for a missing key.
    if ((lookup.id & 0xffff1000UL) && (!(lookup.id & EXTENDED_MODE) || !(lookup.id >> 16))) {
        return NULL;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->MonitorListSemaphore);
    struct GraphicsDisplayRecord* root = graphics->DisplayInfoDataBase;
    UWORD key = lookup.id >> 16;

    // Another caller may have provided the family while this task waited.
    if (!findRecord(root->children, key, 0xffff, SysBase)) {
        addStandardDisplayFamily(key, graphics);
    }

    // The original refreshes all enumerated modes after trying its provider,
    // including unsupported families. Keep enumeration order and its handling
    // of empty monitor groups; do not overwrite other drivers' records.
    ULONG id = INVALID_ID;

    while ((id = pcNextDisplayInfo(id, graphics)) != (ULONG)INVALID_ID) {
        struct GraphicsDisplayLookup existing;
        pcLookupDisplayInfo(&existing, id, graphics);

        if (existing.mode) {
            pcUpdateDisplayAvailability(existing.mode, graphics);
        }
    }

    lookup.monitor = findRecord(root->children, key, 0xffff, SysBase);
    lookup.mode = lookup.monitor ?
        (APTR)findRecord(lookup.monitor->children, lookup.id, 0xffff, SysBase) : NULL;
    ReleaseSemaphore(graphics->MonitorListSemaphore);

    return lookup.mode;
}

void pcLookupDisplayInfo(struct GraphicsDisplayLookup* lookup, ULONG display_id, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->MonitorListSemaphore);

    // The default test uses only the high WORD. MONITOR_ID_MASK also includes
    // EXTENDED_MODE, which must not prevent a default-monitor substitution.
    if (!(display_id >> 16) && graphics->monitor_id) {
        display_id |= ((ULONG)graphics->monitor_id << 16) | EXTENDED_MODE;
    }

    UWORD monitor_id = display_id >> 16;

    if (monitor_id == (DBLNTSC_MONITOR_ID >> 16) || monitor_id == (DBLPAL_MONITOR_ID >> 16)) {
        display_id &= ~(SUPERHIRES | DOUBLESCAN);
    }

    ReleaseSemaphore(graphics->MonitorListSemaphore);
    lookup->id = display_id;
    lookup->monitor = NULL;
    lookup->mode = NULL;
    struct GraphicsDisplayRecord* root = graphics->DisplayInfoDataBase;

    if (!root || display_id == (ULONG)INVALID_ID) {
        return;
    }

    lookup->monitor = findRecord(root->children, display_id >> 16, 0xffff, SysBase);

    if (!lookup->monitor) {
        return;
    }

    lookup->mode = (APTR)findRecord(lookup->monitor->children, display_id, 0xffff, SysBase);

    if (lookup->mode) {
        struct Library* UtilityBase = graphics->UtilBase;
        const struct DisplayInfo* display = (APTR)FindTagItem(DTAG_DISP,
            (APTR)(&lookup->mode->record + 1));

        if (display && (display->NotAvailable & DI_AVAIL_NOMONITOR)) {
            pcUpdateDisplayAvailability(lookup->mode, graphics);
        }
    }
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

static void initQuery(struct QueryHeader* header, ULONG size, ULONG tag, ULONG id, APTR next)
{
    header->StructID = tag;
    header->DisplayID = id;
    header->SkipID = TAG_SKIP;
    header->Length = (size - 24) / sizeof(struct TagItem);
    struct TagItem* continuation = (APTR)((UBYTE*)header + size - sizeof(struct TagItem));
    continuation->ti_Tag = next ? TAG_MORE : TAG_DONE;
    continuation->ti_Data = (ULONG)next;
}

void pcApplyDisplayRevision(struct DisplayInfo* display, const struct GfxBase* graphics)
{
    if (!(graphics->ChipRevBits0 & GFXF_AA_LISA) || (display->PropertyFlags & DIPF_IS_PANELLED)) {
        return;
    }

    if (display->PropertyFlags & DIPF_IS_SPRITES) {
        display->PropertyFlags |= DIPF_IS_SPRITES_ATT | DIPF_IS_SPRITES_CHNG_RES |
            DIPF_IS_SPRITES_BORDER | DIPF_IS_SPRITES_CHNG_BASE | DIPF_IS_SPRITES_CHNG_PRI;
    }

    display->PaletteRange = 65535;
    display->RedBits = 8;
    display->GreenBits = 8;
    display->BlueBits = 8;
}

UWORD pcLimitDisplayDepth(UWORD depth, UBYTE maximum, const struct GfxBase* graphics)
{
    unsigned shift = graphics->bwshifts[graphics->MemType] & 63;

    // The original shifts a LONG, then uses its low WORD for the comparison.
    // Preserve that truncation even for nonstandard bandwidth table entries.
    UWORD scaled = shift < 16 ? (ULONG)depth << shift : 0;
    UWORD limit = maximum ? maximum : 8;

    return scaled < limit ? scaled : limit;
}

ULONG pcDescribeStandardDisplayFamily(UWORD key, struct MonitorInfo* monitor,
    struct NativeDisplayModeEntry* entries, struct GfxBase* graphics)
{
    if (key > 2 || !monitor || !entries) {
        return 0;
    }

    // Keep the database's order: basic modes, their interlaced variants,
    // both dual-playfield priorities, then the additional AA modes.
    static const UWORD modes[STANDARD_DISPLAY_MODE_COUNT] = {
        LORES_KEY, HIRES_KEY, SUPER_KEY, HAM_KEY, EXTRAHALFBRITE_KEY,
        LORESLACE_KEY, HIRESLACE_KEY, SUPERLACE_KEY, HAMLACE_KEY, EXTRAHALFBRITELACE_KEY,
        LORESDPF_KEY, HIRESDPF_KEY, SUPERDPF_KEY,
        LORESLACEDPF_KEY, HIRESLACEDPF_KEY, SUPERLACEDPF_KEY,
        LORESDPF2_KEY, HIRESDPF2_KEY, SUPERDPF2_KEY,
        LORESLACEDPF2_KEY, HIRESLACEDPF2_KEY, SUPERLACEDPF2_KEY,
        HIRESHAM_KEY, SUPERHAM_KEY, HIRESHAMLACE_KEY, SUPERHAMLACE_KEY,
        HIRESEHB_KEY, SUPEREHB_KEY, HIRESEHBLACE_KEY, SUPEREHBLACE_KEY,
        LORESSDBL_KEY, LORESHAMSDBL_KEY, LORESEHBSDBL_KEY, HIRESHAMSDBL_KEY
    };
    BOOL pal = key == 2 || (!key && (graphics->DisplayFlags & PAL));
    ULONG count = graphics->ChipRevBits0 & GFXF_AA_MLISA ? STANDARD_DISPLAY_MODE_COUNT : 22;
    WORD vertical = pal ? 44 : 52;
    WORD rows = pal ? 256 : 200;
    ULONG family_id = (ULONG)key << 16;
    ULONG monitor_id = family_id | (key ? EXTENDED_MODE : 0);
    *monitor = (struct MonitorInfo){0};
    initQuery(&monitor->Header, sizeof(*monitor), DTAG_MNTR, monitor_id, NULL);
    monitor->ViewPosition = (Point){129, 44};
    monitor->ViewResolution = (Point){44, vertical};
    monitor->ViewPositionRange = (struct Rectangle){93, pal ? 29 : 21, 136, pal ? 57 : 63};
    monitor->TotalRows = pal ? 312 : 262;
    monitor->TotalColorClocks = 226;
    monitor->MinRow = pal ? 29 : 21;
    monitor->MouseTicks = (Point){22, vertical / 2};
    monitor->DefaultViewPosition = monitor->ViewPosition;
    monitor->PreferredModeID = (pal ? PAL_MONITOR_ID : NTSC_MONITOR_ID) | HIRES_KEY;
    LONG* overscan = (APTR)monitor->pad;

    for (unsigned rectangle = 0; rectangle < 2; ++rectangle) {
        overscan[rectangle * 4 + 2] = 320 * 44 - 1;
        overscan[rectangle * 4 + 3] = rows * vertical - 1;
    }

    for (ULONG index = 0; index < count; ++index) {
        UWORD mode = modes[index];
        struct NativeDisplayModeData* data = &entries[index].data;
        *data = (struct NativeDisplayModeData){0};
        entries[index].key = mode | (key ? EXTENDED_MODE : 0);
        unsigned shift = mode & SUPERHIRES ? 2 : mode & HIRES ? 1 : 0;
        BOOL ham = (mode & HAM) != 0;
        BOOL ehb = (mode & EXTRA_HALFBRITE) != 0;
        BOOL dual = (mode & DUALPF) != 0;
        BOOL priority = dual && (mode & PFBA);
        BOOL doubled = (mode & DOUBLESCAN) != 0;
        UWORD control = mode | EXTENDED_MODE;

        if (shift == 2) {
            control &= ~HIRES;
        }

        ULONG flags = DIPF_IS_SPRITES | DIPF_IS_GENLOCK | DIPF_IS_DRAGGABLE |
            DIPF_IS_BEAMSYNC | DIPF_IS_DBUFFER;

        if (!ham && !ehb && !dual && !doubled) {
            flags |= DIPF_IS_WB;
        }

        if (pal) {
            flags |= DIPF_IS_PAL;
        }

        if (mode & LACE) {
            flags |= DIPF_IS_LACE;
        }

        if (ham) {
            flags |= DIPF_IS_HAM;
        }

        if (ehb) {
            flags |= DIPF_IS_EXTRAHALFBRITE;
        }

        if (dual) {
            flags |= DIPF_IS_DUALPF;
        }

        if (priority) {
            flags |= DIPF_IS_PF2PRI;
        }

        if (shift == 2 && !ham && !ehb) {
            flags |= DIPF_IS_ECS;
        }

        if ((shift && (ham || ehb)) || doubled) {
            flags |= DIPF_IS_AA;
        }

        if (doubled) {
            flags |= DIPF_IS_SCANDBL;
        }

        struct DisplayInfo* display = &data->display;
        display->NotAvailable = DI_AVAIL_NOMONITOR;
        display->PropertyFlags = flags;
        display->Resolution = (Point){44 >> shift, doubled ? vertical * 2 :
            mode & LACE ? vertical / 2 : vertical};
        display->PixelSpeed = 140 >> shift;
        display->NumStdSprites = 8;
        // Palette range and component precision are separate query fields.
        // AA-only superhires modes retain the legacy range without AA_LISA.
        display->PaletteRange = flags & DIPF_IS_ECS ? 64 : 4096;
        display->SpriteResolution = (Point){shift == 2 ? 22 : 44, vertical};
        display->pad[2] = control >> 8;
        display->pad[3] = control;
        display->RedBits = display->GreenBits = display->BlueBits = shift == 2 ? 2 : 4;
        pcApplyDisplayRevision(display, graphics);

        struct GraphicsDisplayDimensions* dimensions = &data->dimensions;
        UWORD depth = ehb ? 6 : shift ? (8 >> shift) : ham || dual ? 6 : 5;
        dimensions->MaxDepth = pcLimitDisplayDepth(depth, ehb ? 6 : 8, graphics);
        dimensions->MinRasterWidth = 16 << shift;
        dimensions->MinRasterHeight = 1;
        dimensions->MaxRasterWidth = graphics->ChipRevBits0 & GFXF_HR_AGNUS ? 16368 : 1008;
        dimensions->MaxRasterHeight = graphics->ChipRevBits0 & GFXF_HR_AGNUS ? 16384 : 1024;
        dimensions->bounds[0].MaxX = 320 * 44 - 1;
        dimensions->bounds[0].MaxY = rows * vertical - 1;

        for (unsigned rectangle = 1; rectangle < 3; ++rectangle) {
            dimensions->bounds[rectangle].MinX = -36 * 44;
            dimensions->bounds[rectangle].MinY = (monitor->MinRow - 44) * vertical;
            dimensions->bounds[rectangle].MaxX = shift == 2 ? 324 * 44 - 1 :
                (rectangle == 1 ? 326 : 332) * 44 - 1;
            dimensions->bounds[rectangle].MaxY = (monitor->TotalRows - 44) * vertical - 1;
        }

        dimensions->pad[0] = ehb ? 6 : 8;
        data->vectors.Type = doubled ? (ham ? (shift ? 33 : 31) : ehb ? 32 : 30) :
            ham ? 3 + shift : ehb ? (shift ? 25 + shift : 6) :
            dual ? (priority ? 10 : 7) + shift : shift;
    }

    return count;
}

struct NativeDisplayMode* pcAllocNativeDisplayMode(const struct NativeDisplayModeData* description,
    struct GraphicsDisplayRecord* monitor, UWORD key, struct GfxBase* graphics)
{
    if (!description || !monitor || !description->vectors.Vec ||
        description->display.Resolution.x <= 0 || description->display.Resolution.y <= 0) {
        return NULL;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct NativeDisplayMode* mode = AllocMem(sizeof(*mode), MEMF_PUBLIC | MEMF_CLEAR);

    if (!mode) {
        return NULL;
    }

    mode->data = *description;
    ULONG id = ((ULONG)(UWORD)monitor->id << 16) | key;
    mode->record.record.id = id;
    mode->record.record.parent = monitor;
    mode->record.query.ti_Tag = TAG_MORE;
    mode->record.query.ti_Data = (ULONG)&mode->data.display;
    mode->data.vectors.Data = &mode->data.build;
    initQuery(&mode->data.display.Header, sizeof(mode->data.display), DTAG_DISP, id, &mode->data.dimensions);
    initQuery(&mode->data.dimensions.Header, sizeof(mode->data.dimensions), DTAG_DIMS, id,
        description->named ? (APTR)&mode->data.name : (APTR)&mode->data.vectors);
    initQuery(&mode->data.name.Header, sizeof(mode->data.name), DTAG_NAME, id, &mode->data.vectors);
    initQuery(&mode->data.vectors.Header, sizeof(mode->data.vectors), DTAG_VEC, id, NULL);
    pcUpdateDisplayAvailability(&mode->record, graphics);

    return mode;
}

void pcFreeNativeDisplayMode(struct NativeDisplayMode* mode, struct GfxBase* graphics)
{
    if (mode) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        FreeMem(mode, sizeof(*mode));
    }
}

struct NativeDisplayFamily* pcAllocNativeDisplayFamily(const struct MonitorInfo* monitor, UWORD key,
    const struct NativeDisplayModeEntry* entries, ULONG count, struct GfxBase* graphics)
{
    if (!monitor || (count && !entries) ||
        count > (0xffffffffUL - sizeof(struct NativeDisplayFamily)) / sizeof(struct NativeDisplayMode*)) {
        return NULL;
    }

    // Duplicate keys would hide a record from exact lookup. Check the provider
    // before allocating; retain its order rather than sorting the database.
    for (ULONG i = 0; i < count; ++i) {
        for (ULONG j = 0; j < i; ++j) {
            if (entries[i].key == entries[j].key) {
                return NULL;
            }
        }
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ULONG size = sizeof(struct NativeDisplayFamily) + count * sizeof(struct NativeDisplayMode*);
    struct NativeDisplayFamily* family = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!family) {
        return NULL;
    }

    family->count = count;
    family->monitor = *monitor;
    family->record.record.id = key;
    family->record.query.ti_Tag = TAG_MORE;
    family->record.query.ti_Data = (ULONG)&family->monitor;
    initQuery(&family->monitor.Header, sizeof(family->monitor), DTAG_MNTR,
        ((ULONG)key << 16) | (UWORD)monitor->Header.DisplayID, NULL);
    struct GraphicsDisplayRecord* previous = NULL;

    for (ULONG i = 0; i < count; ++i) {
        struct NativeDisplayMode* mode = pcAllocNativeDisplayMode(&entries[i].data,
            &family->record.record, entries[i].key, graphics);

        if (!mode) {
            pcFreeNativeDisplayFamily(family, graphics);

            return NULL;
        }

        family->modes[i] = mode;
        mode->record.record.previous = previous;

        if (previous) {
            previous->next = &mode->record.record;
        } else {
            family->record.record.children = &mode->record.record;
        }

        previous = &mode->record.record;
    }

    return family;
}

void pcFreeNativeDisplayFamily(struct NativeDisplayFamily* family, struct GfxBase* graphics)
{
    if (!family) {
        return;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    for (ULONG i = 0; i < family->count; ++i) {
        pcFreeNativeDisplayMode(family->modes[i], graphics);
    }

    ULONG size = sizeof(*family) + family->count * sizeof(*family->modes);
    FreeMem(family, size);
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

ULONG pcWriteDisplayInfoData(DisplayInfoHandle handle, UBYTE* buffer, ULONG size,
    ULONG tag, ULONG display_id, struct GfxBase* graphics)
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
    GraphicsDisplayQueryFunction callback = ((struct GraphicsDisplayDataRecord*)owner)->set_data;
    struct QueryHeader* query = (APTR)FindTagItem(tag, (APTR)(owner + 1));

    if (!query) {
        return 0;
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

    return result;
}

void pcSetDisplayInfoData(DisplayInfoHandle handle asm("a0"), UBYTE* buffer asm("a1"),
    ULONG size asm("d0"), ULONG tag asm("d1"), ULONG display_id asm("d2"), struct GfxBase* graphics asm("a6"))
{
    pcWriteDisplayInfoData(handle, buffer, size, tag, display_id, graphics);
}
