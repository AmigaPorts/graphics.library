/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/monitor.h"
#include "internal/display.h"

#include <stddef.h>
#include <exec/execbase.h>
#include <graphics/displayinfo.h>
#include <graphics/coerce.h>
#include <graphics/videocontrol.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

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
