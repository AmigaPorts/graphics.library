/* SPDX-License-Identifier: MPL-2.0 */
#define ECS_SPECIFIC // Public BEAMCON metadata constants; no register access.
#include "internal/monitor.h"
#include "internal/display.h"
#include "internal/library.h"

#include <stddef.h>
#include <exec/execbase.h>
#include <hardware/custom.h>
#include <graphics/displayinfo.h>
#include <graphics/coerce.h>
#include <graphics/videocontrol.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

BOOL pcInitDefaultMonitor(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    UWORD id = graphics->DisplayFlags & PAL ? 2 : 1;
    UBYTE frequency = SysBase->VBlankFrequency;

    // This existing private vector has no SDK inline. Preserve its owner and
    // the original register ABI just like the public calls below.
    register struct GfxBase* base asm("a6") = graphics;
    register ULONG selected asm("d0") = id;
    __asm__ volatile("jsr -912(a6)" : "+r"(selected) : "r"(base)
        : "d1", "a0", "a1", "fp0", "fp1", "cc", "memory");

    if (!(UWORD)selected) {
        return FALSE;
    }

    library->initial_monitor = graphics->default_monitor;
    library->initial_frequency = frequency;

    // SetDefaultMonitor can succeed after a display-record allocation fails.
    // Startup needs both the default-ID alias and the named standard family.
    struct MonitorInfo info;
    ULONG size = GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_MNTR,
        ((ULONG)id << 16) | EXTENDED_MODE);

    if (size < offsetof(struct MonitorInfo, Mspc) + sizeof(info.Mspc) ||
        info.Mspc != library->initial_monitor) {
        return FALSE;
    }

    return FindDisplayInfo(0) != NULL;
}

void pcFreeDefaultMonitor(struct GfxBase* graphics)
{
    struct GraphicsLibrary* library = (APTR)graphics;
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct MonitorSpec* monitor = library->initial_monitor;

    if (!monitor) {
        return;
    }

    Remove((struct Node*)monitor);
    graphics->default_monitor = NULL;
    graphics->natural_monitor = NULL;
    library->initial_monitor = NULL;
    SysBase->VBlankFrequency = library->initial_frequency;
    GfxFree(monitor);
}

UWORD pcInitMonitorSpec(struct MonitorSpec* monitor, UWORD flags, MonitorProgramFunction program,
    struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    monitor->ms_Flags = flags & ~0x000f;
    monitor->ms_OpenCount = -1;
    monitor->ratioh = RATIO_UNITY;
    monitor->ratiov = RATIO_UNITY;
    monitor->total_rows = STANDARD_NTSC_ROWS;
    monitor->total_colorclocks = STANDARD_COLORCLOCKS;
    monitor->DeniseMinDisplayColumn = STANDARD_DENISE_MIN;
    monitor->DeniseMaxDisplayColumn = STANDARD_DENISE_MAX;
    monitor->BeamCon0 = STANDARD_NTSC_BEAMCON;
    monitor->min_row = MIN_NTSC_ROW;
    monitor->ms_LegalView.MinX = monitor->ms_LegalView.MaxX = STANDARD_VIEW_X;
    monitor->ms_LegalView.MinY = monitor->ms_LegalView.MaxY = STANDARD_VIEW_Y;

    if (flags & MSF_REQUEST_SPECIAL) {
        struct SpecialMonitor* special = monitor->ms_Special;

        if (!special) {
            special = GfxNew(SPECIAL_MONITOR_TYPE);
        }

        if (special) {
            special->hblank.asi_Start = STANDARD_HBSTRT;
            special->hblank.asi_Stop = STANDARD_HBSTOP;
            special->hsync.asi_Start = STANDARD_HSSTRT;
            special->hsync.asi_Stop = STANDARD_HSSTOP;
            special->vblank.asi_Start = STANDARD_VBSTRT;
            special->vblank.asi_Stop = STANDARD_VBSTOP;
            special->vsync.asi_Start = STANDARD_VSSTRT;
            special->vsync.asi_Stop = STANDARD_VSSTOP;
            special->do_monitor = program;
            monitor->ms_Special = special;
            monitor->ms_Flags |= MSF_REQUEST_SPECIAL;
            monitor->BeamCon0 |= SPECIAL_BEAMCON;
        }
    } else if (monitor->ms_Special) {
        GfxFree(monitor->ms_Special);
        monitor->ms_Special = NULL;
    }

    // NTSC takes precedence when both standard requests are present. Unknown
    // upper flag bits survive, including when special allocation fails.
    if (flags & MSF_REQUEST_NTSC) {
        monitor->ms_Flags |= MSF_REQUEST_NTSC;
    } else if (flags & MSF_REQUEST_PAL) {
        monitor->BeamCon0 |= STANDARD_PAL_BEAMCON;
        monitor->total_rows = STANDARD_PAL_ROWS;
        monitor->min_row = MIN_PAL_ROW;
        monitor->ms_Flags |= MSF_REQUEST_PAL;
    }

    monitor->ms_Flags |= flags & MSF_REQUEST_A2024;
    struct List* list = &monitor->DisplayInfoDataBase;
    list->lh_Head = (APTR)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (APTR)&list->lh_Head;
    InitSemaphore(&monitor->DisplayInfoDataBaseSemaphore);

    return monitor->ms_Flags;
}

struct MonitorSpec* pcOpenMonitor(CONST_STRPTR name asm("a1"), ULONG display_id asm("d0"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    BOOL use_default = (!name && !display_id) || (name && !Stricmp(name, DEFAULT_MONITOR_NAME));
    struct MonitorSpec* monitor = NULL;
    ObtainSemaphore(graphics->MonitorListSemaphore);
    if (use_default) { monitor = graphics->default_monitor; }
    else if (name) { monitor = (APTR)FindName(&graphics->MonitorList, name); }
    else {
        struct MonitorInfo info;
        ULONG size = GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_MNTR, display_id);
        if (size >= offsetof(struct MonitorInfo, Mspc) + sizeof(info.Mspc)) { monitor = info.Mspc; }
    }
    if (monitor) { monitor->ms_OpenCount = (UWORD)monitor->ms_OpenCount + 1; }
    ReleaseSemaphore(graphics->MonitorListSemaphore);
    return monitor;
}

BOOL pcCloseMonitor(struct MonitorSpec* monitor asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->MonitorListSemaphore);
    if (monitor) { monitor->ms_OpenCount = (UWORD)monitor->ms_OpenCount - 1; }
    ReleaseSemaphore(graphics->MonitorListSemaphore);
    return monitor ? FALSE : TRUE;
}

static LONG defaultMonitorScale(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"))
{
    (void)monitor;
    (void)direction;

    // The default monitor uses one LONG transfer, including overlapping
    // points. This differs from the node's general WORD-by-WORD transform.
    typedef ULONG MonitorPoint __attribute__((may_alias));
    *(volatile MonitorPoint*)destination = *(const volatile MonitorPoint*)source;

    return 0;
}

static struct MonitorSpec* createStandardMonitor(UWORD monitor_id, struct GfxBase* graphics)
{
    struct MonitorSpec* monitor = GfxNew(MONITOR_SPEC_TYPE);

    if (monitor) {
        typedef LONG (*InitFunction)(struct ExtendedNode* node asm("a0"), UWORD flags asm("d0"),
            struct GfxBase* base asm("a6"));
        monitor->ms_Node.xln_Library = graphics;
        ((InitFunction)monitor->ms_Node.xln_Init)(&monitor->ms_Node, monitor_id, graphics);
        monitor->ms_Node.xln_Name = (char*)(monitor_id == 2 ? PAL_MONITOR_NAME : NTSC_MONITOR_NAME);
    }

    return monitor;
}

static void initMonitorLegalView(struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    if (!monitor->ms_maxoscan) {
        return;
    }

    typedef LONG (*OverscanFunction)(struct MonitorSpec* monitor asm("a0"), struct Rectangle* bounds asm("a1"),
        ULONG modes asm("d0"), struct GfxBase* base asm("a6"));
    struct Rectangle* bounds = &monitor->ms_LegalView;
    Point normal = {graphics->NormalDisplayColumns >> 1, graphics->NormalDisplayRows};
    ((OverscanFunction)monitor->ms_maxoscan)(monitor, bounds, 0, graphics);
    bounds->MaxX = (UWORD)bounds->MaxX - (UWORD)bounds->MinX + 1;
    bounds->MaxY = (UWORD)bounds->MaxY - (UWORD)bounds->MinY + 1;
    monitor->ms_scale(monitor, &normal, TO_MONITOR, &normal);
    bounds->MaxX = (UWORD)bounds->MaxX - (UWORD)normal.x;
    bounds->MaxY = (UWORD)bounds->MaxY - (UWORD)normal.y;
    bounds->MaxX = (UWORD)bounds->MaxX + (UWORD)bounds->MinX - 1;
    bounds->MaxY = (UWORD)bounds->MaxY + (UWORD)bounds->MinY - 1;
}

static void attachMonitorDisplayInfo(struct MonitorSpec* monitor, ULONG monitor_id, struct GfxBase* graphics)
{
    ULONG id = INVALID_ID;

    while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
        if ((id & 0xffff0000UL) != monitor_id) {
            continue;
        }

        DisplayInfoHandle handle = FindDisplayInfo(id);
        struct MonitorInfo info;

        if (handle && GetDisplayInfoData(handle, (APTR)&info, sizeof(info), DTAG_MNTR, id)) {
            info.Mspc = monitor;

            if (pcWriteDisplayInfoData(handle, (APTR)&info, sizeof(info), DTAG_MNTR, id, graphics)) {
                break;
            }
        }
    }
}

BOOL pcSetDefaultMonitor(UWORD monitor_id asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    BOOL was_pal = (graphics->DisplayFlags & PAL) != 0;
    BOOL pal = monitor_id == 2 || monitor_id == 10;
    BOOL doubled = monitor_id == 9 || monitor_id == 10;
    BOOL created = FALSE;
    BOOL success = FALSE;
    struct MonitorSpec* monitor = NULL;
    ObtainSemaphore(graphics->MonitorListSemaphore);

    if (monitor_id == 1 || monitor_id == 2 || doubled) {
        monitor = OpenMonitor(NULL, ((ULONG)monitor_id << 16) | EXTENDED_MODE);

        if (!doubled) {
            if (!monitor) {
                monitor = createStandardMonitor(monitor_id, graphics);
                created = monitor != NULL;
            }

            // Even a failed standard-monitor allocation replaces this pointer.
            // Double-scan selection leaves the natural monitor unchanged.
            graphics->natural_monitor = monitor;
        }
    }

    if (monitor && !(doubled && (graphics->system_bplcon0 & GENLOCK_VIDEO))) {
        monitor->ms_scale = defaultMonitorScale;
        graphics->default_monitor = monitor;
        graphics->DisplayFlags = (graphics->DisplayFlags & ~(NTSC | PAL)) | (pal ? PAL : NTSC);
        graphics->NormalDisplayRows = pal ? 256 : 200;
        graphics->monitor_id = monitor_id;
        graphics->NormalDPMX = pal ? 1226 : 1280;
        graphics->NormalDPMY = pal ? 1299 : 1098;
        SysBase->VBlankFrequency = pal ? 50 : 60;

        if (created) {
            initMonitorLegalView(monitor, graphics);
            AddTail(&graphics->MonitorList, (struct Node*)monitor);
            attachMonitorDisplayInfo(monitor, (ULONG)monitor_id << 16, graphics);
        }

        if (pal != was_pal && !doubled) {
            // Populate the newly selected standard family before enumerating.
            // Clear NOMONITOR for the newly selected family as the original
            // does; do not replace other availability bits or monitor owners.
            FindDisplayInfo(((ULONG)monitor_id << 16) | EXTENDED_MODE);
            ULONG id = INVALID_ID;

            while ((id = NextDisplayInfo(id)) != (ULONG)INVALID_ID) {
                ULONG family = id & 0xffff1000UL;

                if (family == (was_pal ? NTSC_MONITOR_ID : PAL_MONITOR_ID)) {
                    struct DisplayInfo info;

                    if (GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_DISP, id)) {
                        info.NotAvailable &= ~DI_AVAIL_NOMONITOR;
                        // This existing private SFD vector has no public SDK
                        // inline. Preserve its current owner and register ABI.
                        typedef void (*SetDataFunction)(DisplayInfoHandle handle asm("a0"),
                            UBYTE* buffer asm("a1"), ULONG size asm("d0"), ULONG tag asm("d1"),
                            ULONG id asm("d2"), struct GfxBase* base asm("a6"));
                        ((SetDataFunction)((UBYTE*)graphics - 750))(NULL, (APTR)&info,
                            sizeof(info), DTAG_DISP, id, graphics);
                    }
                }
            }

            graphics->monitor_id = monitor_id;
        }

        success = TRUE;
    }

    ReleaseSemaphore(graphics->MonitorListSemaphore);

    return success;
}

LONG pcGetVPModeID(const struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct TagItem tags[3];
    tags[0].ti_Tag = VTAG_NORMAL_DISP_GET;
    tags[0].ti_Data = 0;
    tags[1].ti_Tag = VTAG_VPMODEID_GET;
    tags[1].ti_Data = 0;
    tags[2].ti_Tag = VTAG_END_CM;
    tags[2].ti_Data = 0;
    if (VideoControl(viewport->ColorMap, tags)) { return INVALID_ID; }
    ULONG id = tags[1].ti_Data;
    if (id == (ULONG)INVALID_ID) {
        // The explicit ViewPort mode takes precedence. Otherwise query the
        // normal display handle; legacy ViewPort.Modes is not a display ID.
        struct QueryHeader header;
        if (GetDisplayInfoData((DisplayInfoHandle)tags[0].ti_Data, (APTR)&header,
            sizeof(header), DTAG_DISP, INVALID_ID)) {
            id = header.DisplayID;
        }
    }
    return id;
}

LONG pcModeNotAvailable(ULONG display_id asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct DisplayInfo info;
    ULONG bytes = GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_DISP, display_id);
    // Preserve the original strict threshold, including an exactly 18-byte
    // result, and zero-extend the public availability flags.
    if (bytes > offsetof(struct DisplayInfo, NotAvailable) + sizeof(info.NotAvailable)) {
        return info.NotAvailable;
    }
    return INVALID_ID;
}

LONG pcMonitorTransform(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"))
{
    (void)monitor; (void)direction;
    // The original default transform and translation are identity callbacks.
    // Keep the two writes separate, including for overlapping Point storage.
    const volatile Point* input = source;
    volatile Point* output = destination;
    output->x = input->x;
    output->y = input->y;
    return 0;
}

LONG pcMonitorTranslate(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"))
{
    return pcMonitorTransform(monitor, source, direction, destination);
}

LONG pcMonitorScale(struct MonitorSpec* monitor asm("a0"), Point* source asm("a1"),
    UWORD direction asm("d0"), Point* destination asm("a2"))
{
    WORD x = source->x, y = source->y;
    if (monitor && (direction == TO_MONITOR || direction == FROM_MONITOR)) {
        // Ratios use four fractional bits. FROM_MONITOR exchanges the two
        // ratios; it does not calculate reciprocals. Both use signed WORDs.
        WORD horizontal = direction == TO_MONITOR ? monitor->ratioh : monitor->ratiov;
        WORD vertical = direction == TO_MONITOR ? monitor->ratiov : monitor->ratioh;
        x = ((LONG)x * horizontal + 8) >> 4;
        y = ((LONG)y * vertical + 8) >> 4;
    }
    destination->x = x;
    destination->y = y;
    return 0;
}

static LONG monitorOverscan(struct GfxBase* graphics, struct MonitorSpec* monitor,
    struct Rectangle* rectangle, UWORD modes, UWORD extra_columns)
{
    if (!monitor || !rectangle) { return 1; }
    UWORD alignment = 0xfff8;
    if (!(graphics->ChipRevBits0 & GFXF_HR_DENISE)) {
        if (modes & (HIRES | SUPERHIRES)) { alignment = 0x7ffc; }
        if (modes & SUPERHIRES) { alignment >>= 1; }
    }
    WORD offset = modes & SUPERHIRES ? -2 : modes & HIRES ? 0 : 4;
    UWORD minimum = monitor->DeniseMinDisplayColumn;
    UWORD maximum = monitor->DeniseMaxDisplayColumn + extra_columns;
    UWORD start = (((UWORD)(minimum - 9) >> 1) - offset) & alignment;
    if (start < 24) { start = 24; }

    // Keep the original fetch-window alignment and WORD wrap. These are
    // coordinate calculations only; the callback does not program hardware.
    UWORD end = monitor->total_colorclocks - 8;
    end -= (end - start - 1) & 7;
    end = (end & 0xfffe) + 8;
    UWORD left = (start + offset) * 2 + 9;
    UWORD right = (end + offset) * 2 + 9;
    rectangle->MinX = left > minimum ? left : minimum;
    rectangle->MinY = monitor->min_row;
    rectangle->MaxX = right < maximum ? right : maximum;
    rectangle->MaxY = monitor->total_rows;
    return 0;
}

LONG pcMonitorMaxOverscan(struct MonitorSpec* monitor asm("a0"), struct Rectangle* rectangle asm("a1"),
    ULONG modes asm("d0"), struct GfxBase* graphics asm("a6"))
{
    return monitorOverscan(graphics, monitor, rectangle, modes, 0);
}

LONG pcMonitorVideoScan(struct MonitorSpec* monitor asm("a0"), struct Rectangle* rectangle asm("a1"),
    ULONG modes asm("d0"), struct GfxBase* graphics asm("a6"))
{
    return monitorOverscan(graphics, monitor, rectangle, modes, 9);
}

ULONG pcViewportModes(const struct ViewPort* viewport)
{
    const struct ColorMap* map = viewport->ColorMap;
    ULONG modes = viewport->Modes;

    if (map && map->Type) {
        const struct GraphicsDisplayDataRecord* record = map->CoerceDisplayInfo;

        if (!record) {
            record = map->NormalDisplayInfo;
        }

        if (record) {
            return record->reserved;
        }
    }

    if (modes & EXTENDED_MODE) {
        modes &= (modes & DUALPF) ? 0x8c4c : 0x8c0c;
    }

    return modes;
}

LONG pcViewportFetchMode(const struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!viewport || !viewport->RasInfo || !viewport->RasInfo->BitMap) {
        return -1;
    }

    ULONG modes = pcViewportModes(viewport);
    const struct BitMap* bitmap = viewport->RasInfo->BitMap;
    const struct BitMap* second = NULL;
    UBYTE second_depth = 0;

    if ((modes & DUALPF) && viewport->RasInfo->Next) {
        second = viewport->RasInfo->Next->BitMap;

        if (second) {
            second_depth = second->Depth;
        }
    }

    UBYTE depth = bitmap->Depth + second_depth;
    UWORD fetch = 0;

    if (modes & HIRES) {
        fetch = depth > 4;
    } else if (modes & SUPERHIRES) {
        fetch = (depth > 2) + (depth > 4);
    }

    // Preserve the original WORD comparison: its mode register receives only
    // the memory-class byte before this test, retaining the upper mode byte.
    WORD memory = (modes & 0xff00) | graphics->MemType;

    if ((fetch == 1 && !graphics->MemType) || (fetch == 2 && memory < BANDWIDTH_4X)) {
        return -1;
    }

    ++fetch;
    UWORD mask = (1U << fetch) - 1;
    UWORD alignment = bitmap->BytesPerRow;

    for (unsigned plane = 0; plane < bitmap->Depth; ++plane) {
        alignment |= (ULONG)bitmap->Planes[plane];
    }

    if (second_depth) {
        alignment |= second->BytesPerRow;

        for (unsigned plane = 0; plane < second_depth; ++plane) {
            alignment |= (ULONG)second->Planes[plane];
        }
    }

    if (alignment & mask) {
        return -1;
    }

    while (fetch != 3) {
        mask = (mask << 1) | 1;

        if (alignment & mask) {
            break;
        }

        ++fetch;
    }

    --fetch;

    if (fetch) {
        fetch = (fetch == 1 && graphics->MemType == BANDWIDTH_4X) ? BANDWIDTH_2XNML : graphics->MemType;
    }

    return (UBYTE)fetch;
}

ULONG pcCoerceMode(struct ViewPort* viewport asm("a0"), ULONG monitor_id asm("d0"),
    ULONG flags asm("d1"), struct GfxBase* graphics asm("a6"))
{
    ULONG source_id = GetVPModeID(viewport);
    ULONG monitor = monitor_id & MONITOR_ID_MASK;
    ULONG default_monitor = (graphics->DisplayFlags & PAL) ? PAL_MONITOR_ID : NTSC_MONITOR_ID;

    if (!(source_id >> 16) && monitor == default_monitor) {
        return monitor | source_id;
    }

    if ((monitor_id >> 16) == (source_id >> 16) && (flags & IGNORE_MCOMPAT) &&
        (WORD)pcViewportFetchMode(viewport, graphics) != -1) {
        return source_id;
    }

    struct TagItem tags[10];
    struct TagItem* next = tags;
    next->ti_Tag = BIDTAG_ViewPort;
    next++->ti_Data = (ULONG)viewport;
    next->ti_Tag = BIDTAG_MonitorID;
    next++->ti_Data = monitor_id;

    const ULONG preserve = DIPF_IS_DUALPF | DIPF_IS_PF2PRI | DIPF_IS_HAM | DIPF_IS_EXTRAHALFBRITE;
    ULONG must_have = 0;
    ULONG must_not_have = preserve;
    struct DisplayInfo info;

    if (GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_DISP, source_id)) {
        must_have = info.PropertyFlags & preserve;
        must_not_have = ~must_have & preserve;

        if ((flags & AVOID_FLICKER) && !(info.PropertyFlags & DIPF_IS_LACE)) {
            must_not_have |= DIPF_IS_LACE;
        }

        next->ti_Tag = BIDTAG_RedBits;
        next++->ti_Data = info.RedBits;
        next->ti_Tag = BIDTAG_GreenBits;
        next++->ti_Data = info.GreenBits;
        next->ti_Tag = BIDTAG_BlueBits;
        next++->ti_Data = info.BlueBits;
    }

    next->ti_Tag = BIDTAG_DIPFMustNotHave;
    next++->ti_Data = must_not_have;
    next->ti_Tag = BIDTAG_DIPFMustHave;
    next++->ti_Data = must_have;

    if (!(flags & IGNORE_MCOMPAT)) {
        next->ti_Tag = BIDTAG_GfxPrivate;
        next++->ti_Data = BIDTAG_COERCE;
    }

    next->ti_Tag = TAG_DONE;

    return BestModeIDA(tags);
}

static ULONG unsignedDistance(ULONG first, ULONG second)
{
    LONG difference = first - second;

    return difference < 0 ? 0UL - (ULONG)difference : (ULONG)difference;
}

ULONG pcBestModeIDA(const struct TagItem* tags asm("a0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    ULONG result = INVALID_ID;
    ObtainSemaphore(graphics->MonitorListSemaphore);

    ULONG must_have = GetTagData(BIDTAG_DIPFMustHave, 0, tags);
    ULONG must_not_have = GetTagData(BIDTAG_DIPFMustNotHave, ~must_have & SPECIAL_FLAGS, tags);
    struct ViewPort* viewport = (APTR)GetTagData(BIDTAG_ViewPort, 0, tags);
    UWORD depth = 1;

    if (viewport && viewport->RasInfo && viewport->RasInfo->BitMap) {
        depth = viewport->RasInfo->BitMap->Depth;
    }

    depth = GetTagData(BIDTAG_Depth, depth, tags);
    ULONG monitor_id = GetTagData(BIDTAG_MonitorID, PROTO_MONITOR_ID, tags);
    BOOL default_monitor = !(monitor_id >> 16);

    if (default_monitor) {
        monitor_id = ((ULONG)graphics->monitor_id << 16) | EXTENDED_MODE;
    }

    ULONG source_id = viewport ? GetVPModeID(viewport) : INVALID_ID;
    source_id = GetTagData(BIDTAG_SourceID, source_id, tags);
    UWORD nominal_width = 640;
    UWORD nominal_height = 200;
    UWORD minimum_speed = 35;

    if (viewport) {
        nominal_width = viewport->DWidth;
        nominal_height = viewport->DHeight;
        WORD fetch = pcViewportFetchMode(viewport, graphics);

        if (fetch == -1) {
            fetch = 0;
        }

        if (fetch != BANDWIDTH_4X) {
            if (!fetch) {
                if (depth > 2) {
                    minimum_speed = 70;
                }

                if (depth > 4) {
                    minimum_speed = 140;
                }
            } else if (depth > 4) {
                minimum_speed = 70;
            }
        }
    }

    struct DimensionInfo dimensions;

    if (GetDisplayInfoData(NULL, (APTR)&dimensions, sizeof(dimensions), DTAG_DIMS, source_id)) {
        nominal_width = (UWORD)dimensions.Nominal.MaxX - (UWORD)dimensions.Nominal.MinX + 1;
        nominal_height = (UWORD)dimensions.Nominal.MaxY - (UWORD)dimensions.Nominal.MinY + 1;
    }

    nominal_width = GetTagData(BIDTAG_NominalWidth, nominal_width, tags);
    nominal_height = GetTagData(BIDTAG_NominalHeight, nominal_height, tags);
    UWORD desired_width = GetTagData(BIDTAG_DesiredWidth, nominal_width, tags);
    UWORD desired_height = GetTagData(BIDTAG_DesiredHeight, nominal_height, tags);
    UBYTE red = GetTagData(BIDTAG_RedBits, 4, tags);
    UBYTE green = GetTagData(BIDTAG_GreenBits, 4, tags);
    UBYTE blue = GetTagData(BIDTAG_BlueBits, 4, tags);
    BOOL coerce = FindTagItem(BIDTAG_GfxPrivate, tags) != NULL;
    DisplayInfoHandle source = FindDisplayInfo(source_id);

    // Start at the requested monitor's first record when present. The original
    // falls back to enumeration from INVALID_ID when that monitor is absent.
    struct GraphicsDisplayRecord* root = graphics->DisplayInfoDataBase;
    struct GraphicsDisplayRecord* monitor = root->children;
    Forbid();

    while (monitor && (UWORD)monitor->id != (UWORD)(monitor_id >> 16)) {
        monitor = monitor->next;
    }

    Permit();
    struct GraphicsDisplayRecord* first = monitor ? monitor->children : NULL;
    ULONG current = first ? first->id : (ULONG)INVALID_ID;
    BOOL compatible = TRUE;

    if (coerce) {
        struct MonitorInfo from, to;

        if (GetDisplayInfoData(source, (APTR)&from, sizeof(from), DTAG_MNTR, source_id) &&
            GetDisplayInfoData(first, (APTR)&to, sizeof(to), DTAG_MNTR, monitor_id)) {
            if (from.Compatibility == MCOMPAT_NOBODY || to.Compatibility == MCOMPAT_NOBODY ||
                ((from.Compatibility == MCOMPAT_SELF || to.Compatibility == MCOMPAT_SELF) &&
                 (source_id & MONITOR_ID_MASK) != monitor_id)) {
                compatible = FALSE;
            }
        }
    }

    if (monitor_id != PROTO_MONITOR_ID && !compatible) {
        goto done;
    }

    struct DisplayInfo display;

    if (source) {
        if (!GetDisplayInfoData(source, (APTR)&display, sizeof(display), DTAG_DISP, source_id)) {
            goto done;
        }

        must_have |= display.PropertyFlags & SPECIAL_FLAGS;
    }

    must_not_have &= ~must_have;
    ULONG nominal_ratio = UDivMod32((ULONG)nominal_width << 16, nominal_height);
    ULONG best_distance = nominal_ratio;
    UWORD best_width = 0;
    UWORD best_height = 0;
    BOOL enough_width = FALSE;
    BOOL enough_height = FALSE;
    BOOL best_lace = TRUE;
    BOOL same_monitor;

    do {
        ULONG candidate_monitor = current & MONITOR_ID_MASK;
        same_monitor = monitor_id == PROTO_MONITOR_ID || candidate_monitor == monitor_id;

        if (candidate_monitor && same_monitor) {
            DisplayInfoHandle handle = FindDisplayInfo(current);

            if (!GetDisplayInfoData(handle, (APTR)&display, sizeof(display), DTAG_DISP, current) ||
                (display.PropertyFlags & must_have) != must_have ||
                (display.PropertyFlags & must_not_have) || display.NotAvailable ||
                display.PixelSpeed < minimum_speed) {
                goto next;
            }

            if (!GetDisplayInfoData(handle, (APTR)&dimensions, sizeof(dimensions), DTAG_DIMS, current)) {
                goto next;
            }

            // Candidate sizes use the inclusive maximum endpoints. Source
            // nominal sizes above also subtract the origin; retain that split.
            UWORD width = (UWORD)dimensions.Nominal.MaxX + 1;
            UWORD height = (UWORD)dimensions.Nominal.MaxY + 1;
            ULONG ratio = UDivMod32((ULONG)width << 16, height);

            if (dimensions.MaxDepth < depth || display.RedBits < red ||
                display.GreenBits < green || display.BlueBits < blue) {
                goto next;
            }

            ULONG distance = unsignedDistance(ratio, nominal_ratio);
            BOOL accept = FALSE;

            if (distance <= best_distance) {
                accept = (!enough_height && unsignedDistance((ULONG)height + best_height, desired_height) <= desired_height) ||
                    (!enough_width && unsignedDistance((ULONG)width + best_width, desired_width) <= desired_width) ||
                    (enough_height && height == desired_height) || (enough_width && width == desired_width);
            }

            if (!accept && best_height) {
                goto next;
            }

            // Equal geometry keeps an existing noninterlaced mode. While the
            // current winner is interlaced, enumeration order may replace it.
            if (width == best_width && height == best_height && !best_lace) {
                goto next;
            }

            result = current;
            depth = dimensions.MaxDepth < depth ? dimensions.MaxDepth : depth;
            best_distance = distance;
            best_width = width;
            best_height = height;
            enough_width = width >= desired_width;
            enough_height = height >= desired_height;
            best_lace = (display.PropertyFlags & DIPF_IS_LACE) != 0;
        }

next:
        current = NextDisplayInfo(current);
    } while (current != (ULONG)INVALID_ID && same_monitor);

    // This conversion also applies to an unsuccessful enumeration, matching
    // the original default-monitor result rather than normalizing INVALID_ID.
    if (default_monitor) {
        result &= ~MONITOR_ID_MASK;
    }

done:
    ReleaseSemaphore(graphics->MonitorListSemaphore);

    return result;
}
