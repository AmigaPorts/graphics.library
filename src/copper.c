/* SPDX-License-Identifier: MPL-2.0 */
// The original allocator retains the legacy SDK fields, including SLRepeat and
// Flags after the four old frame pointers. Keep that allocation layout locally.
#define V1_3
#include "internal/copper.h"
#include "internal/color.h"
#include "internal/display.h"
#include "internal/monitor.h"
#include "internal/library.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/modeid.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>

_Static_assert(sizeof(struct CopList) == COPLIST_STORAGE_SIZE, "legacy CopList allocation");

static void mergeViewMove(struct CopList* list, UWORD destination, UWORD data, UWORD flags, APTR context)
{
    (void)list;
    struct ViewWaitOutput* output = context;

    // The legacy MOVE field bits use the opposite mapping to WAIT admission.
    if (!(flags & CPR_NT_SHT)) {
        *output->cursor[0]++ = destination;
        *output->cursor[0]++ = data;
    }

    if (output->interlaced && !(flags & CPR_NT_LOF)) {
        *output->cursor[1]++ = destination;
        *output->cursor[1]++ = data;
    }
}

static UWORD viewWaitFlags(const struct CopList* selected, const struct CopList* previous,
    const struct ViewPort* first_viewport)
{
    // Straddle and exact-line state comes from the preceding stream owner.
    // On its own first/same-owner wait, only auxiliary list heads admit it;
    // continuation buffers do not acquire their head's identity.
    const struct ViewPort* viewport = previous ? previous->_ViewPort : first_viewport;

    if (!previous || previous == selected || selected->_ViewPort == first_viewport) {
        BOOL auxiliary = viewport->SprIns == selected || viewport->ClrIns == selected;

        for (const struct UCopList* user = viewport->UCopIns; !auxiliary && user; user = user->Next) {
            auxiliary = user->FirstCopList == selected;
        }

        if (!auxiliary) {
            return 0;
        }
    }

    const struct ColorMap* map = viewport->ColorMap;
    UWORD flags = 0;

    if (map && map->Type && map->cm_vpe) {
        flags = map->cm_vpe->Flags & (VPXF_STRADDLES_256 | VPXF_STRADDLES_512);
    }

    if (previous && (previous->Flags & EXACT_LINE)) {
        flags |= 0x40;
    }

    return flags;
}

static LONG allocViewLists(struct View* view, UWORD count, struct GfxBase* owner);

static LONG mergeViewLists(struct View* view, struct GfxBase* graphics, BOOL native)
{
    struct MonitorSpec* monitor = graphics->natural_monitor;
    struct ViewExtra* extra = NULL;

    if (view->Modes & EXTEND_VSTRUCT) {
        extra = GfxLookUp(view);

        if (extra && extra->Monitor) {
            monitor = extra->Monitor;
        }
    }

    WORD origin = pcPrepareViewModes(view, monitor, graphics);
    struct CopList* first;
    UWORD count = pcCollectViewLists(view, &first);

    if (!first) {
        return MCOP_NOP;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct MergedView* pending = NULL;

    if (native) {
        pending = AllocMem(sizeof(*pending), MEMF_PUBLIC | MEMF_CLEAR);

        if (!pending) {
            return MCOP_NO_MEM;
        }

        pending->display = pcAllocViewDisplay(view, pcPreparedViewPorts(graphics), graphics);

        if (!pending->display) {
            FreeMem(pending, sizeof(*pending));
            return MCOP_NO_MEM;
        }

        for (unsigned field = 0; field <= (unsigned)!!(view->Modes & LACE); ++field) {
            struct ViewFieldTiming* timing = &pending->display->timing[field];
            timing->capacity = (ULONG)count + 10;
            timing->waits = AllocMem(pcViewFieldStorageSize(timing->capacity), MEMF_PUBLIC);

            if (!timing->waits) {
                pcReleaseViewDisplay(pending->display, graphics);
                FreeMem(pending, sizeof(*pending));
                return MCOP_NO_MEM;
            }

            timing->records = (APTR)(timing->waits + timing->capacity);
        }
    }

    LONG result = allocViewLists(view, count, native ? graphics : NULL);

    if (result != MCOP_OK) {
        if (pending) {
            pcReleaseViewDisplay(pending->display, graphics);
            FreeMem(pending, sizeof(*pending));
        }

        return result;
    }

    struct ViewWaitSequence sequence = {0};

    if (pending) {
        pcRetireMergedViewOutput(pcMergedViews(graphics), view->LOFCprList);
        pcRetireMergedViewOutput(pcMergedViews(graphics), view->SHFCprList);
    }

    sequence.first_wait = -1;
    sequence.long_start = view->LOFCprList->start;
    sequence.output.cursor[0] = view->LOFCprList->start;
    sequence.output.interlaced = !!(view->Modes & LACE);
    sequence.output.cursor[1] = sequence.output.interlaced ? view->SHFCprList->start : NULL;

    if (pending) {
        for (unsigned field = 0; field <= (unsigned)sequence.output.interlaced; ++field) {
            sequence.output.timing[field] = &pending->display->timing[field];
            sequence.output.start[field] = sequence.output.cursor[field];
        }
    }

    const struct ViewPort* first_viewport = NULL;
    struct CopList* previous = NULL;

    while (sequence.output.previous[0].x < 255) {
        pcAdvanceViewWaits(&first, mergeViewMove, &sequence.output);
        struct CopList* selected = pcSelectViewWait(first);
        BOOL first_wait = !selected->CopLStart;

        if (first_wait) {
            selected->CopLStart = sequence.output.cursor[0];
            selected->CopSStart = sequence.output.cursor[1];
        }

        if (!previous) {
            first_viewport = selected->_ViewPort;
        }

        UWORD flags = viewWaitFlags(selected, previous, first_viewport);
        pcEmitViewWait(&sequence, selected, origin, flags, !!(flags & 0x40), monitor, graphics);

        if (pending && first_wait) {
            for (ULONG index = 0; index < pending->display->count; ++index) {
                struct ViewDisplayPort* port = &pending->display->viewports[index];

                if (port->prepared->list != selected) {
                    continue;
                }

                for (unsigned field = 0; field <= (unsigned)sequence.output.interlaced; ++field) {
                    const UWORD* start = field ? selected->CopSStart : selected->CopLStart;

                    port->prefix_wait[field] = sequence.output.emitted[field];
                    port->prefix_word[field] = start - sequence.output.start[field];
                    port->setup_word[field] = sequence.output.cursor[field] - sequence.output.start[field];
                    port->prefix_wait_mask |= 1U << field;
                }
            }
        }

        previous = pcAdvanceViewWait(selected);
    }

    if (extra && (graphics->ChipRevBits0 & GFXF_AA_LISA)) {
        ULONG top = sequence.first_wait;
        extra->TopLine = top < 0xc00 ? top : 0xc00;
    }

    pcFinishViewFields(&sequence.output, view->LOFCprList->start,
        sequence.output.interlaced ? view->SHFCprList->start : NULL);

    if (pending) {
        if (sequence.output.timing_overflow) {
            pcReleaseViewDisplay(pending->display, graphics);
            FreeMem(pending, sizeof(*pending));
            return MCOP_NO_MEM;
        }

        for (unsigned field = 0; field <= (unsigned)sequence.output.interlaced; ++field) {
            pending->display->timing[field].words =
                sequence.output.cursor[field] - sequence.output.start[field];
        }

        pcCaptureViewDisplayPalettes(pending->display,
            view->LOFCprList, sequence.output.cursor[0],
            view->SHFCprList, sequence.output.cursor[1]);
        pcCommitMergedView(pcMergedViews(graphics), pending,
            view->LOFCprList, view->SHFCprList, graphics);
    }

    return MCOP_OK;
}

LONG pcMergeViewLists(struct View* view, struct GfxBase* graphics)
{
    return mergeViewLists(view, graphics, FALSE);
}

LONG pcMergeNativeView(struct View* view, struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    LONG result = mergeViewLists(view, graphics, TRUE);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    return result;
}

WORD pcPrepareViewModes(struct View* view, const struct MonitorSpec* monitor, struct GfxBase* graphics)
{
    enum { UNSELECTED, NORMAL, A2024_PENDING, A2024_SELECTED };
    unsigned mode = monitor->ms_Flags & MSF_REQUEST_A2024 ? A2024_PENDING : UNSELECTED;
    WORD origin = view->DyOffset;
    graphics->hedley_flags = 0;

    for (struct ViewPort* viewport = view->ViewPort; viewport; viewport = viewport->Next) {
        if ((viewport->Modes & VP_HIDE) || !viewport->DspIns) {
            continue;
        }

        BOOL extended = viewport->ExtendedModes & 0x40;

        switch (mode) {
            case UNSELECTED:
                mode = extended ? A2024_SELECTED : NORMAL;
                break;

            case NORMAL:
                if (extended) {
                    viewport->Modes = 0;
                }
                break;

            case A2024_PENDING: {
                ULONG id = pcViewportModes(viewport);
                origin = STANDARD_VIEW_Y;

                if (!extended && id != A2024TENHERTZ_KEY && id != A2024FIFTEENHERTZ_KEY) {
                    mode = NORMAL;
                    break;
                }

                mode = A2024_SELECTED;
                graphics->hedley_flags = 0x40;

                if (graphics->hedley[5] == (ULONG)viewport) {
                    graphics->hedley_flags |= 0x80;
                } else {
                    graphics->hedley[5] = (ULONG)viewport;
                }

                if (id == A2024TENHERTZ_KEY) {
                    graphics->hedley_flags |= 0x20;
                }

                graphics->hedley_tmp = 0;

                if (viewport->RasInfo) {
                    const struct BitMap* bitmap = viewport->RasInfo->BitMap;
                    graphics->hedley[6] = (ULONG)bitmap;

                    if (bitmap) {
                        graphics->hedley_tmp = bitmap->Depth;
                    }
                }
                break;
            }

            case A2024_SELECTED:
                viewport->Modes = 0;
                break;
        }
    }

    return origin;
}

static void freeViewLists(struct cprlist* list, struct GfxBase* owner)
{
    if (owner) {
        pcFreeCprList(list, owner);
    } else {
        pcFreeCprListStorage(list);
    }
}

static LONG allocViewLists(struct View* view, UWORD count, struct GfxBase* owner)
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    WORD capacity = (UWORD)(count + 10);
    ULONG size = (ULONG)(LONG)capacity * sizeof(ULONG);
    struct cprlist* long_list = view->LOFCprList;
    struct cprlist* short_list = NULL;
    BOOL new_long = FALSE;
    BOOL new_data = FALSE;
    BOOL new_short = FALSE;

    if (!long_list) {
        long_list = AllocMem(sizeof(*long_list), MEMF_PUBLIC);

        if (!long_list) {
            return MCOP_NO_MEM;
        }

        new_long = TRUE;
        view->LOFCprList = long_list;
        long_list->Next = NULL;
    } else if (long_list->MaxCount < capacity) {
        if (owner) {
            pcRetireMergedViewOutput(pcMergedViews(owner), long_list);
        }

        FreeMem(long_list->start, (ULONG)(LONG)long_list->MaxCount * sizeof(ULONG));
    } else {
        goto short_field;
    }

    long_list->start = AllocMem(size, MEMF_PUBLIC | MEMF_CHIP);

    if (!long_list->start) {
        goto failure;
    }

    new_data = TRUE;
    long_list->MaxCount = capacity;

short_field:
    if (view->Modes & LACE) {
        short_list = view->SHFCprList;

        // The original reuses an existing short-field list without changing
        // its capacity, even when it has just grown the long-field storage.
        if (!short_list) {
            short_list = AllocMem(sizeof(*short_list), MEMF_PUBLIC);

            if (!short_list) {
                goto failure;
            }

            new_short = TRUE;
            view->SHFCprList = short_list;
            short_list->Next = NULL;
            short_list->start = AllocMem(size, MEMF_PUBLIC | MEMF_CHIP);

            if (!short_list->start) {
                goto failure;
            }

            short_list->MaxCount = capacity;
        }
    } else {
        freeViewLists(view->SHFCprList, owner);
        view->SHFCprList = NULL;
    }

    freeViewLists(long_list->Next, owner);
    long_list->Next = NULL;

    if (short_list) {
        freeViewLists(short_list->Next, owner);
        short_list->Next = NULL;
    }

    return MCOP_OK;

failure:
    if (new_short) {
        FreeMem(short_list, sizeof(*short_list));
        view->SHFCprList = NULL;
    }

    if (new_data) {
        FreeMem(long_list->start, size);
        long_list->start = NULL;
    }

    if (new_long) {
        FreeMem(long_list, sizeof(*long_list));
        view->LOFCprList = NULL;
    }

    return MCOP_NO_MEM;
}

LONG pcAllocViewLists(struct View* view, UWORD count)
{
    return allocViewLists(view, count, NULL);
}

static void markUserList(struct CopList* list)
{
    struct CopIns* instruction = list->CopPtr;

    for (;;) {
        UWORD opcode = instruction->OpCode & 3;

        if (opcode == CPRNXTBUF) {
            list = instruction->NXTLIST;

            if (!list) {
                return;
            }

            instruction = list->CopPtr;
            continue;
        }

        if (opcode == COPPER_WAIT) {
            if (instruction->VWAITPOS == 10000 && instruction->HWAITPOS == 255) {
                return;
            }
        } else if (opcode != COPPER_MOVE) {
            return;
        }

        instruction->OpCode |= CPR_NT_SYS;
        ++instruction;
    }
}

static UWORD collectViewList(struct CopList** first, struct CopList* list,
    struct ViewPort* viewport, BOOL user)
{
    if (!list || !list->Count) {
        return 0;
    }

    struct CopList* previous = *first;
    *first = list;
    UWORD count = 0;

    for (struct CopList* buffer = list; buffer; buffer = buffer->Next) {
        buffer->CopSStart = NULL;
        buffer->CopLStart = NULL;
        buffer->_CopList = previous;
        buffer->_ViewPort = viewport;
        buffer->CopPtr = buffer->CopIns;
        count += buffer->Count;
    }

    if (user && viewport->ColorMap && (viewport->ColorMap->Flags & USER_COPPER_CLIP)) {
        markUserList(list);
    }

    return count;
}

UWORD pcCollectViewLists(struct View* view, struct CopList** first)
{
    UWORD count = 0;
    *first = NULL;

    for (struct ViewPort* viewport = view->ViewPort; viewport; viewport = viewport->Next) {
        if ((viewport->Modes & VP_HIDE) || !viewport->DspIns) {
            if (viewport->DspIns) {
                viewport->DspIns->CopLStart = NULL;
                viewport->DspIns->CopSStart = NULL;
            }

            continue;
        }

        struct ColorMap* map = viewport->ColorMap;

        if (map && map->Type && map->cm_vpe) {
            map->cm_vpe->cop1ptr = 0;
            map->cm_vpe->cop2ptr = 0;
        }

        count += collectViewList(first, viewport->DspIns, viewport, FALSE);
        count += collectViewList(first, viewport->SprIns, viewport, FALSE);
        count += collectViewList(first, viewport->ClrIns, viewport, FALSE);

        for (struct UCopList* user = viewport->UCopIns; user; user = user->Next) {
            count += collectViewList(first, user->FirstCopList, viewport, TRUE);
        }
    }

    return count;
}

void pcAdvanceViewWaits(struct CopList** first, ViewMergeMoveFunction move, APTR context)
{
    struct CopList* previous = NULL;
    struct CopList* list = *first;

    while (list) {
        struct CopIns* instruction = list->CopPtr;

        while (!(instruction->OpCode & 3)) {
            move(list, instruction->DESTADDR & 0x01fe, instruction->DESTDATA,
                instruction->OpCode, context);
            ++instruction;
        }

        if (instruction->OpCode == CPRNXTBUF) {
            list = instruction->NXTLIST;

            if (!previous) {
                *first = list;
            } else {
                // Every remaining buffer of the preceding stream must reach
                // the new current buffer, including after its own transition.
                for (struct CopList* buffer = previous; buffer; buffer = buffer->Next) {
                    buffer->_CopList = list;
                }
            }

            continue;
        }

        list->CopPtr = instruction;
        previous = list;
        list = list->_CopList;
    }
}

struct CopList* pcAdvanceViewWait(struct CopList* list)
{
    const struct ViewPort* viewport = list->_ViewPort;
    const struct ColorMap* map = viewport->ColorMap;
    const struct CopIns* wait = list->CopPtr;

    // Clipping uses the signed ViewPort-relative line, before interlace or
    // doublescan conversion. DyOffset cancels from the original comparison.
    if (!map || !(map->Flags & USER_COPPER_CLIP) || wait->VWAITPOS < viewport->DHeight) {
        ++list->CopPtr;
        return list;
    }

    if (wait->VWAITPOS == 10000 && wait->HWAITPOS == 255) {
        return list;
    }

    ++list->CopPtr;

    if (!(wait->OpCode & CPR_NT_SYS)) {
        return list;
    }

    for (;;) {
        const struct CopIns* instruction = list->CopPtr;
        UWORD opcode = instruction->OpCode & 3;

        if (opcode == CPRNXTBUF) {
            list = instruction->NXTLIST;

            if (!list) {
                return NULL;
            }

            continue;
        }

        if (opcode == COPPER_WAIT) {
            if (instruction->VWAITPOS == 10000 && instruction->HWAITPOS == 255) {
                return list;
            }
        } else if (opcode != COPPER_MOVE) {
            return list;
        }

        if (!(instruction->OpCode & CPR_NT_SYS)) {
            return list;
        }

        ++list->CopPtr;
    }
}

struct CopList* pcSelectViewWait(struct CopList* first)
{
    struct CopList* selected = NULL;
    LONG earliest = 0;

    for (struct CopList* list = first; list; list = list->_CopList) {
        const struct ViewPort* viewport = list->_ViewPort;
        const struct CopIns* wait = list->CopPtr;
        ULONG modes = pcViewportModes(viewport);
        LONG line = (LONG)viewport->DyOffset + wait->VWAITPOS;

        if (modes & LACE) {
            line >>= 1;
        }

        if (modes & DOUBLESCAN) {
            line = line * 2 + !!(list->Flags & HALF_LINE);
        }

        // Keep the original signed horizontal position and full intermediate
        // line range. View/monitor offsets apply later, when emitting waits.
        LONG position = line * 1024 + wait->HWAITPOS;

        if (!selected || position < earliest ||
            (position == earliest && !(selected->CopPtr->OpCode & (CPR_NT_SHT | CPR_NT_LOF)))) {
            selected = list;
            earliest = position;
        }
    }

    return selected;
}

WORD pcViewWaitLine(const struct CopList* list, WORD view_y,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics)
{
    const struct ViewPort* viewport = list->_ViewPort;
    ULONG modes = pcViewportModes(viewport);
    WORD line = (WORD)(list->CopPtr->VWAITPOS + viewport->DyOffset);

    if (modes & LACE) {
        line = (WORD)(line + 1);
        line >>= 1;
    }

    if (modes & DOUBLESCAN) {
        line = (WORD)(line * 2 + !!(list->Flags & HALF_LINE));
    }

    line = (WORD)(line + view_y);

    if (line < 0) {
        line = 0;
    }

    WORD limit = monitor->total_rows;

    // Retain the public graphics base's external-sync timing adjustment.
    // This reads the saved mode word; it does not access chipset hardware.
    if (graphics->system_bplcon0 & 0x0102) {
        limit = (WORD)(limit - 1);
    }

    if (line >= limit) {
        line = (WORD)(limit - 1);

        if (line < 255) {
            line = 255;
        }
    }

    return line;
}

static void captureViewWait(struct ViewWaitOutput* output, unsigned field, const UWORD* record,
    Point position)
{
    struct ViewFieldTiming* timing = output->timing[field];

    if (!timing) {
        return;
    }

    if (timing->count == timing->capacity) {
        output->timing_overflow = TRUE;
        return;
    }

    struct ViewFieldWait* wait = &timing->waits[timing->count++];
    wait->offset = record - output->start[field];
    wait->position = position;
}

static unsigned emitViewWaitRecord(struct ViewWaitOutput* output, WORD line, WORD absolute_line,
    WORD emitted_line, WORD horizontal, UWORD flags, BOOL repeat_short, unsigned step)
{
    unsigned selected = flags & CPR_NT_LOF ? 1 : 0;
    Point* previous = &output->previous[selected];

    if (horizontal == -1 || (previous->x == horizontal && previous->y == line)) {
        return step;
    }

    UWORD instruction = ((UWORD)line << 8) | (horizontal & 0xfe) | 1;

    // At an existing end-of-page wait, a wrap cannot move backwards in
    // horizontal time. Preserve its absolute history even when no record fits.
    if (!step || previous->y != 255 || horizontal > previous->x) {
        if (!(flags & CPR_NT_LOF)) {
            *output->cursor[0]++ = instruction;
            *output->cursor[0]++ = 0xfffe;
            output->emitted[0].x = horizontal & 0xfe;
            output->emitted[0].y = emitted_line;
            captureViewWait(output, 0, output->cursor[0] - 2, output->emitted[0]);
            previous->x = horizontal;
            previous->y = absolute_line;

            if (!(flags & CPR_NT_SHT)) {
                output->previous[1] = *previous;
            }
        }

        if (!output->interlaced) {
            return step;
        }

        if (!(flags & CPR_NT_SHT)) {
            if (!repeat_short) {
                *output->cursor[1]++ = instruction;
                *output->cursor[1]++ = 0xfffe;
                output->emitted[1].x = horizontal & 0xfe;
                output->emitted[1].y = emitted_line;
                captureViewWait(output, 1, output->cursor[1] - 2, output->emitted[1]);
            }

            previous->x = horizontal;
            previous->y = absolute_line;
        }
    }

    if (step == 1 && previous->y == 255) {
        step = 2;
    }

    previous->y = absolute_line;
    return step;
}

void pcEmitViewWaitRecord(struct ViewWaitOutput* output, WORD line, WORD horizontal,
    UWORD flags, const struct GfxBase* graphics)
{
    BOOL repeat_short = !(graphics->ChipRevBits0 & GFXF_HR_DENISE) &&
        !(flags & (CPR_NT_LOF | CPR_NT_SHT)) &&
        output->previous[1].x == horizontal && output->previous[1].y == line;
    emitViewWaitRecord(output, line, line, line, horizontal, flags, repeat_short, 0);
}

void pcFinishViewFields(struct ViewWaitOutput* output, const UWORD* long_start, const UWORD* short_start)
{
    if (!output->interlaced) {
        return;
    }

    UWORD instruction = output->cursor[0][-2];
    UWORD data = output->cursor[0][-1];

    for (unsigned field = 0; field < 2; ++field) {
        UWORD* end = output->cursor[field];
        ULONG address = (ULONG)(field ? long_start : short_start);

        // These are address records in the public output-list format. The
        // native display owner consumes the lists; no registers are accessed.
        end[2] = instruction;
        end[3] = data;
        end[-2] = 0x84;
        end[-1] = address >> 16;
        end[0] = 0x86;
        end[1] = address;
        output->cursor[field] = end + 4;

        struct ViewFieldTiming* timing = output->timing[field];

        if (timing) {
            ULONG replaced = end - 2 - output->start[field];

            if (timing->count && timing->waits[timing->count - 1].offset == replaced) {
                --timing->count;
            }

            if (instruction & 1) {
                captureViewWait(output, field, end + 2, output->emitted[0]);
            }
        }
    }
}

static BOOL previousViewMove(const UWORD** end, UWORD* count, UWORD destination, UWORD* data)
{
    while (*count) {
        --*count;
        *end -= 2;

        if ((*end)[0] == destination) {
            *data = (*end)[1];
            return TRUE;
        }
    }

    return FALSE;
}

WORD pcViewWrapPosition(const UWORD* start, const UWORD* end, WORD line,
    WORD relative_line, WORD previous_line, BOOL exact_line,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics)
{
    UWORD clocks = monitor->total_colorclocks;
    WORD fallback = (clocks - 4) & 0xfffe;

    if ((WORD)((line & 0xff00) - (previous_line & 0xff00)) >= 512) {
        return fallback;
    }

    // Search backwards in the required order. A later write to a register
    // does not supply a value for an earlier stage of this display setup.
    // These identifiers describe the public list format, not MMIO addresses.
    static const UWORD registers[] = {0x1fc, 0x1e4, 0x094, 0x092, 0x090, 0x100, 0x08e};
    UWORD values[7] = {0};
    UWORD count = (end - start) / 2;
    const UWORD* cursor = end;
    UBYTE revision = graphics->ChipRevBits0;
    unsigned first = revision & GFXF_AA_LISA ? 0 : 1;
    UWORD original_control = 0;

    if (!(revision & (GFXF_AA_LISA | GFXF_HR_DENISE))) {
        if (!previousViewMove(&cursor, &count, 0x100, &original_control)) {
            return fallback;
        }

        first = 2;
    }

    for (unsigned index = first; index < 7; ++index) {
        if (!previousViewMove(&cursor, &count, registers[index], &values[index])) {
            return fallback;
        }
    }

    UWORD fetch_start = values[3];
    UWORD fetch_stop = values[2];
    UWORD control = revision & GFXF_HR_DENISE ? values[5] : original_control;
    WORD window_stop = (values[4] >> 8) + (values[1] & 0x700);
    WORD window_start = values[6] >> 8;

    if (revision & GFXF_HR_DENISE) {
        window_start += (values[1] & 7) << 8;
    } else {
        window_stop = line;
    }

    WORD boundary = (line & 0xff00) - (relative_line & 0xff00) + 255;
    WORD horizontal = (clocks - 1) & 0xfffe;

    if ((previous_line & 255) != 255) {
        if (!(window_start & 255)) {
            if (exact_line) {
                return 0;
            }

            if (window_start > boundary) {
                return horizontal;
            }
        }

        if (window_stop <= boundary) {
            return horizontal;
        }
    }

    if ((horizontal & 7) >= 2 || (WORD)(fetch_stop + 8) < horizontal) {
        return (WORD)(horizontal - 2);
    }

    WORD period = control & 0x40 ? 2 : control & 0x8000 ? 4 : 8;
    UWORD fetch_mode = values[0] & 3;

    if (fetch_mode) {
        period *= fetch_mode == 3 ? 4 : 2;
    }

    WORD planes = control & 0x10 ? 8 : (control >> 12) & 7;

    if ((previous_line & 255) == 255) {
        WORD remaining = (clocks & 0xfff8) - fetch_stop - 8;
        WORD slots = (WORD)((WORD)(fetch_start - 2) - remaining) >> 1;
        WORD available = period;
        WORD extra = 0;

        if (available > 8) {
            available = 8;
            extra = period == 16 ? 4 : 12;
        }

        WORD free_slots = available >> 1;

        if (free_slots <= planes) {
            free_slots = available - planes;
        }

        extra += free_slots;
        ULONG product = (ULONG)(UWORD)(fetch_stop - fetch_start) * (UWORD)extra;
        ULONG quotient = product / (UWORD)period;

        // DIVU leaves the dividend unchanged on quotient overflow. Retain
        // the consumed low word for unusual caller-supplied list values.
        slots = (WORD)(slots + (WORD)(quotient > 0xffff ? product : quotient));
        slots >>= 1;
        cursor = end;
        UWORD following = 0xffff;

        do {
            ++following;
            cursor -= 2;
        } while (!(cursor[0] & 1) || (cursor[1] & 1));

        if (slots <= (WORD)following) {
            return -1;
        }
    }

    if (period == planes) {
        return fetch_start;
    }

    if (period > 8) {
        period = 8;
    }

    if (planes <= (period >> 1) + 1) {
        return (WORD)(horizontal - 2);
    }

    if (period == 4) {
        horizontal -= 4;
    }

    if (planes != 6) {
        horizontal -= 4;
    }

    return (WORD)(horizontal - 4);
}

static void emitViewWaitSequence(struct ViewWaitSequence* sequence, WORD absolute_line,
    WORD target_horizontal, UWORD flags, UWORD viewport_flags, BOOL exact_line,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics)
{
    struct ViewWaitOutput* output = &sequence->output;
    WORD saved_line = output->previous[0].y;
    BOOL repeat_short = FALSE;

    if (!(graphics->ChipRevBits0 & GFXF_HR_DENISE) && !(flags & (CPR_NT_LOF | CPR_NT_SHT))) {
        repeat_short = output->previous[1].x == target_horizontal &&
            output->previous[1].y == absolute_line;

        if (!repeat_short && sequence->wrapped[0] && output->previous[1].y <= 255 && absolute_line > 255) {
            // The fields have reached different pages. Resolve the short
            // field first, then the long field, retaining each call's history.
            emitViewWaitSequence(sequence, absolute_line, target_horizontal, flags | CPR_NT_LOF,
                viewport_flags, exact_line, monitor, graphics);
            emitViewWaitSequence(sequence, absolute_line, target_horizontal, flags | CPR_NT_SHT,
                viewport_flags, exact_line, monitor, graphics);
            return;
        }
    }

    unsigned selected = flags & CPR_NT_LOF ? 1 : 0;
    Point* previous = &output->previous[selected];
    WORD line = absolute_line;
    WORD remaining = absolute_line;
    WORD horizontal = target_horizontal;
    unsigned step = 0;
    BOOL emit_record = TRUE;

    if (line > 255) {
        if ((viewport_flags & VPXF_STRADDLES_256) && (graphics->ChipRevBits0 & GFXF_AA_LISA)) {
            if ((WORD)(line - 256) == previous->y) {
                previous->y = (WORD)(previous->y + 256);
            }

            step = 1;

            if (line >= 512 && (viewport_flags & VPXF_STRADDLES_512)) {
                step = 2;
                remaining = (WORD)(remaining - 256);
            }

            remaining = (WORD)(remaining - 256);
            emit_record = FALSE;
        } else if ((graphics->ChipRevBits0 & GFXF_HR_DENISE) && previous->y == 255) {
            if (horizontal == 255) {
                line = 255;
            } else {
                step = 1;
                emit_record = FALSE;
            }
        } else {
            line = 255;

            if (horizontal != 255) {
                step = 1;

                if (sequence->wrapped[selected]) {
                    remaining = (WORD)(remaining - (previous->y & 0xff00));
                    emit_record = FALSE;
                } else {
                    horizontal = pcViewWrapPosition(sequence->long_start, output->cursor[0],
                        absolute_line, remaining, saved_line, exact_line, monitor, graphics);
                }
            }
        }
    }

    for (;;) {
        if (emit_record) {
            // A page-boundary record precedes the eventual absolute target.
            // Retain its own position, not the logical history used for merging.
            WORD emitted_line = (WORD)(absolute_line - remaining + line);
            step = emitViewWaitRecord(output, line, absolute_line, emitted_line,
                horizontal, flags, repeat_short, step);

            if (!step) {
                return;
            }

            remaining = (WORD)(remaining - 256);
        }

        if (!(flags & CPR_NT_LOF)) {
            sequence->wrapped[0] = TRUE;

            if (!(flags & CPR_NT_SHT)) {
                sequence->wrapped[1] = TRUE;
            }
        } else {
            sequence->wrapped[1] = TRUE;
        }

        line = remaining;
        emit_record = TRUE;

        if (line >= 256) {
            horizontal = pcViewWrapPosition(sequence->long_start, output->cursor[0],
                absolute_line, remaining, saved_line, exact_line, monitor, graphics);
            line = 255;
        } else {
            if (step == 2 && previous->y == absolute_line &&
                absolute_line != (WORD)(monitor->total_rows - 1)) {
                horizontal = pcViewWrapPosition(sequence->long_start, output->cursor[0],
                    absolute_line, remaining, saved_line, exact_line, monitor, graphics);

                if (horizontal >= target_horizontal) {
                    remaining = (WORD)(remaining + 256);
                    line = 255;
                    step = 3;
                    continue;
                }
            }

            horizontal = target_horizontal;
            step = 0;
        }
    }
}

void pcEmitViewWait(struct ViewWaitSequence* sequence, const struct CopList* list,
    WORD view_y, UWORD viewport_flags, BOOL exact_line,
    const struct MonitorSpec* monitor, const struct GfxBase* graphics)
{
    WORD line = pcViewWaitLine(list, view_y, monitor, graphics);

    if (sequence->first_wait == -1) {
        if (line <= 5) {
            line = 5;
        }

        sequence->first_wait = (ULONG)(UWORD)line << 8;
    }

    emitViewWaitSequence(sequence, line, list->CopPtr->HWAITPOS, list->CopPtr->OpCode,
        viewport_flags, exact_line, monitor, graphics);
}

struct CopList* pcInitCopList(struct CopList* list, LONG count)
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    BOOL allocated = FALSE;
    if (!list) {
        list = AllocMem(sizeof(*list), MEMF_PUBLIC | MEMF_CLEAR);
        if (!list) { return NULL; }
        allocated = TRUE;
    }
    if (!list->CopIns || list->MaxCount < count) {
        if (list->CopIns) {
            FreeMem(list->CopIns, (ULONG)(LONG)list->MaxCount * sizeof(struct CopIns));
            list->CopIns = NULL;
        }
        list->Count = 0;
        struct CopIns* instructions = AllocMem((ULONG)count * sizeof(*instructions), MEMF_PUBLIC);
        if (!instructions) {
            if (allocated) {
                FreeMem(list, sizeof(*list));
                return NULL;
            }
            list->CopPtr = list->CopIns;
            list->MaxCount = 0;
            list->CopIns = NULL;
            list->CopLStart = NULL;
            list->CopSStart = NULL;
            return list;
        }
        list->CopIns = instructions;
        list->MaxCount = count;
    }
    list->CopPtr = list->CopIns;
    list->Count = 0;
    list->SLRepeat = 1;
    list->Flags = 0;
    return list;
}

static struct CopIns* appendInstruction(struct CopIns* instruction,
    UWORD opcode, UWORD destination, UWORD data)
{
    instruction->OpCode = opcode;
    instruction->DESTADDR = destination;
    instruction->DESTDATA = data;

    return instruction + 1;
}

UWORD pcBuildColorList(struct ViewPort* viewport, const ULONG* mask,
    UWORD display_control, UWORD sprite_control)
{
    const struct ColorMap* map = viewport->ColorMap;
    ULONG colors = map ? map->Count : 32;
    ULONG capacity = 112 + 2 * colors + 2 * ((colors >> 5) + 1);
    struct CopList* list = pcInitCopList(viewport->DspIns, capacity);
    viewport->DspIns = list;

    if (!list || !list->MaxCount) {
        return MVP_NO_DSPINS;
    }

    const UWORD* high = map ? map->ColorTable : NULL;
    const UWORD* low = map && map->Type == COLORMAP_TYPE_V39 ? map->LowColorBits : high;
    UWORD color_mask = map && (map->Flags & COLORMAP_TRANSPARENCY) ? 0xffff : 0x7fff;
    struct CopIns* instruction = list->CopIns;
    instruction = appendInstruction(instruction, COPPER_WAIT, 0, 0);

    // These are data records in AmigaOS's public intermediate-list format.
    // Keeping the bank/precision selectors preserves palette updates by list
    // consumers; constructing the list does not access chipset registers.
    for (unsigned bank = 0; bank < 8; ++bank) {
        ULONG selected = mask[bank];

        if (!selected) {
            continue;
        }

        for (unsigned pass = 0; pass < 2; ++pass) {
            UWORD control = display_control | (bank << 13) | (pass << 9);
            instruction = appendInstruction(instruction, COPPER_MOVE, 0x106, control);
            const UWORD* table = pass ? low : high;

            for (unsigned index = 0; index < 32; ++index) {
                if (!(selected & (0x80000000UL >> index))) {
                    continue;
                }

                unsigned color_index = bank * 32 + index;
                UWORD color = map ? table[color_index] : pcDefaultColor(color_index);
                instruction = appendInstruction(instruction, COPPER_MOVE,
                    0x180 + 2 * index, color & color_mask);
            }
        }
    }

    instruction = appendInstruction(instruction, COPPER_MOVE, 0x10c, sprite_control);
    list->Count = instruction - list->CopIns;

    return MVP_OK;
}

static void updateColorStream(UWORD* stream, unsigned stride, const UWORD* high,
    const UWORD* low, UWORD color_mask)
{
    if (!stream) {
        return;
    }

    // Intermediate records have an opcode before their destination/data;
    // merged records have just the destination/data pair. The original starts
    // at color zero, ignoring earlier control selectors in the list prefix.
    UWORD* record = stream;

    while ((stride == 3 && (record[0] & 3)) || record[stride - 2] != 0x180) {
        record += stride;
    }

    const UWORD* table = high;
    record[stride - 1] = table[0] & color_mask;
    record += stride;

    for (;;) {
        if (stride == 3 && (record[0] & 3)) {
            return;
        }

        UWORD destination = record[stride - 2];

        if (destination >= 0x180 && destination < 0x1c0) {
            record[stride - 1] = table[(destination - 0x180) >> 1] & color_mask;
        } else if (destination == 0x106) {
            UWORD control = record[stride - 1];
            table = (control & 0x200 ? low : high) + ((control >> 13) * 32);
        } else {
            return;
        }

        record += stride;
    }
}

void pcUpdateColorLists(struct CopList* list, const UWORD* high, const UWORD* low,
    UWORD color_mask, BOOL update_intermediate)
{
    // Preserve the original short-before-long order, including aliased lists.
    updateColorStream(list->CopSStart, 2, high, low, color_mask);
    updateColorStream(list->CopLStart, 2, high, low, color_mask);

    if (update_intermediate) {
        updateColorStream((UWORD*)list->CopIns, 3, high, low, color_mask);
    }
}

void pcUpdateNativeColorLists(struct CopList* list, const UWORD* high, const UWORD* low,
    UWORD color_mask, BOOL update_intermediate, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    pcUpdateColorLists(list, high, low, color_mask, update_intermediate);
    pcRefreshMergedViewPalettes(pcMergedViews(graphics), list);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

static void updateColor4Stream(UWORD* stream, UWORD destination, UWORD color)
{
    if (!stream) {
        return;
    }

    // Initial waits precede the first MOVE run. A later WAIT ends this update,
    // even if the same destination occurs again farther down the merged field.
    while (stream[0] & 1) {
        stream += 2;
    }

    while (!(stream[0] & 1)) {
        if (stream[0] == destination) {
            stream[1] = (stream[1] & 0xf000) | color;
        }

        stream += 2;
    }
}

void pcUpdateColor4Lists(struct CopList* list, UWORD destination, UWORD color)
{
    color &= 0x0fff;
    updateColor4Stream(list->CopLStart, destination, color);
    updateColor4Stream(list->CopSStart, destination, color);
    struct CopIns* instruction = list->CopIns;

    if (!instruction) {
        return;
    }

    for (;;) {
        unsigned opcode = instruction->OpCode & 3;

        if (opcode == COPPER_MOVE) {
            if ((UWORD)instruction->DESTADDR == destination) {
                instruction->DESTDATA = ((UWORD)instruction->DESTDATA & 0xf000) | color;
            }
        } else if (opcode == COPPER_WAIT) {
            if (instruction->HWAITPOS == 255) {
                return;
            }
        } else {
            // This legacy updater follows the raw instruction target. Unlike
            // the merger, it does not dereference a CopList's CopIns member.
            instruction = (struct CopIns*)instruction->NXTLIST;
            continue;
        }

        ++instruction;
    }
}

UWORD pcFinishViewPortList(struct View* view, struct ViewPort* viewport,
    struct ViewPortExtra* extra, UWORD first_line, ULONG modes, struct GfxBase* graphics)
{
    struct CopList* list = viewport->DspIns;

    if (!list) {
        return MVP_NO_DSPINS;
    }

    struct CopIns* first = list->CopIns;
    appendInstruction(first + list->Count, COPPER_WAIT, 10000, 255);
    ++list->Count;

    UWORD gap = CalcIVG(view, viewport);
    first->VWAITPOS = -gap;

    if (modes & DOUBLESCAN) {
        gap *= 2;
    } else if (modes & LACE) {
        gap >>= 1;
    }

    // Preserve the original WORD subtraction and signed gap comparison. Its
    // straddle path adds flags without clearing a preceding STRADDLES512 bit;
    // the other path clears both boundary flags, preserving unrelated bits.
    BOOL straddles = !(first_line & 255) && (list->Flags & EXACT_LINE);

    if ((first_line & 255) && (LONG)first_line > (LONG)(WORD)gap) {
        UWORD before = first_line - gap;

        if ((before & 0xff00) != (first_line & 0xff00)) {
            straddles = TRUE;
        }
    }

    if (straddles) {
        extra->Flags |= VPXF_STRADDLES256;

        if (first_line >= 512) {
            extra->Flags |= VPXF_STRADDLES512;
        }
    } else {
        extra->Flags &= ~(VPXF_STRADDLES256 | VPXF_STRADDLES512);
    }

    return MVP_OK;
}

static struct CopList* __attribute__((used, noinline)) initUserList(struct UCopList* list asm("a0"),
    LONG count asm("d0"))
{
    if (!list) { return NULL; }
    struct CopList* first = pcInitCopList(list->FirstCopList, count);
    if (first) { list->FirstCopList = first; list->CopList = first; }
    return first;
}

// The SDK declares a WORD count, but the original allocation calculation uses
// all of D0. Keep the public declaration and preserve the incoming register.
__asm__(".globl _pcUCopperListInit\n.set _pcUCopperListInit,_initUserList\n");

static LONG writeInstruction(struct UCopList* list, UWORD opcode, UWORD first, UWORD second)
{
    if (!list) { return 0; }
    struct CopList* current = list->CopList;
    if (!current) { current = initUserList(list, 16); }
    if (!current || !current->MaxCount || current->Count > current->MaxCount) { return 0; }
    struct CopIns* instruction = current->CopPtr;
    instruction->OpCode = opcode;
    instruction->DESTADDR = first;
    instruction->DESTDATA = second;
    return 1;
}

LONG pcCMove(struct UCopList* list asm("a1"), APTR destination asm("d0"), WORD data asm("d1"))
{
    return writeInstruction(list, COPPER_MOVE, (ULONG)destination, data);
}

LONG pcCWait(struct UCopList* list asm("a1"), WORD vertical asm("d0"), WORD horizontal asm("d1"))
{
    return writeInstruction(list, COPPER_WAIT, vertical, horizontal);
}

struct CopIns* pcBumpCopList(struct CopList** list, struct CopIns* following)
{
    struct CopList* current = *list;
    struct CopIns* written = following - 1;
    current->Count = (UWORD)current->Count + 1;

    if (current->Count >= current->MaxCount) {
        struct ExecBase* SysBase = *(struct ExecBase**)4;
        struct CopList* next = current->Next;

        if (!next) {
            next = AllocMem(sizeof(*next), MEMF_PUBLIC);
            current->Next = next;

            if (!next) {
                current->Count = (UWORD)current->Count - 1;
                return following;
            }

            struct CopIns* instructions = AllocMem(16 * sizeof(*instructions), MEMF_PUBLIC);

            if (!instructions) {
                FreeMem(next, sizeof(*next));
                current->Next = NULL;
                current->Count = (UWORD)current->Count - 1;
                return following;
            }

            next->CopIns = instructions;
            next->MaxCount = 16;
            next->Next = NULL;
        }

        *list = next;
        next->CopIns->OpCode = written->OpCode;
        next->CopIns->NXTLIST = written->NXTLIST;
        written->OpCode = CPRNXTBUF;
        written->NXTLIST = next;
        next->Count = 1;
        following = next->CopIns + 1;
    }

    return following;
}

void pcCBump(struct UCopList* list asm("a1"))
{
    if (!list) {
        return;
    }

    struct CopIns* following = pcBumpCopList(&list->CopList, list->CopList->CopPtr + 1);
    list->CopList->CopPtr = following;
}

void pcFreeCopList(struct CopList* list asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!list) {
        return;
    }

    struct ExecBase* SysBase = *(struct ExecBase**)4;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    while (list) {
        // Remove the preparation association before the list's address can
        // be reused. A merged display's retained generation remains alive.
        pcRemovePreparedViewPort(pcPreparedViewPorts(graphics), list, graphics);
        FreeMem(list->CopIns, (ULONG)(LONG)list->MaxCount * sizeof(struct CopIns));
        struct CopList* next = list->Next;
        FreeMem(list, COPLIST_STORAGE_SIZE);
        list = next;
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

void pcFreeCprListStorage(struct cprlist* list asm("a0"))
{
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    while (list) {
        FreeMem(list->start, (ULONG)(LONG)list->MaxCount * sizeof(ULONG));
        struct cprlist* next = list->Next;
        FreeMem(list, sizeof(*list));
        list = next;
    }
}

void pcFreeCprList(struct cprlist* list asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!list) {
        return;
    }

    struct ExecBase* SysBase = *(struct ExecBase**)4;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    for (const struct cprlist* current = list; current; current = current->Next) {
        pcRemoveMergedView(pcMergedViews(graphics), current, graphics);
    }

    pcFreeCprListStorage(list);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

void pcFreeVPortCopLists(struct ViewPort* viewport asm("a0"), struct GfxBase* graphics asm("a6"))
{
    if (!viewport) { return; }
    struct ExecBase* SysBase = *(struct ExecBase**)4;
    pcFreeCopList(viewport->DspIns, graphics);
    pcFreeCopList(viewport->ClrIns, graphics);
    pcFreeCopList(viewport->SprIns, graphics);
    struct UCopList* list = viewport->UCopIns;
    while (list) {
        pcFreeCopList(list->FirstCopList, graphics);
        struct UCopList* next = list->Next;
        FreeMem(list, sizeof(*list));
        list = next;
    }
    viewport->DspIns = NULL;
    viewport->ClrIns = NULL;
    viewport->SprIns = NULL;
    viewport->UCopIns = NULL;
}

ULONG pcCalcIVG(struct View* view asm("a0"), struct ViewPort* viewport asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    (void)view;
    ULONG display_id = GetVPModeID(viewport);

    if (display_id == (ULONG)INVALID_ID) {
        display_id = pcViewportModes(viewport);

        if (!(display_id & EXTENDED_MODE)) {
            display_id &= 0xffff9efdUL;
        } else if ((display_id & MONITOR_ID_MASK) == EXTENDED_MODE) {
            display_id &= ~EXTENDED_MODE;
        }
    }

    const struct ColorMap* map = viewport->ColorMap;

    if (map && map->Type) {
        const struct GraphicsDisplayDataRecord* record = map->CoerceDisplayInfo;

        if (!record) {
            record = map->NormalDisplayInfo;
        }

        if (record) {
            display_id = record->record.id;
        }

        if (map->cm_vpe && (WORD)pcViewportFetchMode(viewport, graphics) < 0) {
            return 0;
        }
    }

    // The original reuses one query buffer, asking for only the display prefix
    // after reading the monitor timing. Query return counts do not gate this
    // calculation; keep the public vector calls and their lengths unchanged.
    union {
        struct MonitorInfo monitor;
        struct DisplayInfo display;
    } info;
    GetDisplayInfoData(NULL, (APTR)&info, sizeof(info.monitor), DTAG_MNTR, display_id);
    UWORD clocks = (WORD)info.monitor.TotalColorClocks >> 1;
    GetDisplayInfoData(NULL, (APTR)&info, 28, DTAG_DISP, display_id);

    struct CopList* list = viewport->DspIns;

    if (!list) {
        return 0;
    }

    UWORD count = 0;

    for (;;) {
        count += list->Count;

        if (!list->Next) {
            break;
        }

        list = list->Next;
    }

    count -= list->SLRepeat;
    ULONG lines = (ULONG)count * 2;

    // DIVU.W leaves quotient/remainder packed together, and leaves the dividend
    // unchanged on quotient overflow. The original signed comparison below
    // determines rounding and EXACT_LINE from that complete result.
    __asm__ volatile("divu.w %1,%0" : "+d"(lines) : "d"(clocks) : "cc");

    if ((LONG)lines > 0xffff) {
        lines = (lines & 0xffff0000UL) | (UWORD)(lines + 1);
    } else {
        list->Flags |= EXACT_LINE;
    }

    if (!(graphics->ChipRevBits0 & GFXF_AA_ALICE) && (WORD)lines < 2) {
        lines = 2;
    }

    if (info.display.PropertyFlags & DIPF_IS_LACE) {
        return (UWORD)(lines * 2);
    }

    if (info.display.PropertyFlags & DIPF_IS_SCANDBL) {
        list->Flags |= ((UWORD)lines & 1) * HALF_LINE;
        lines = (WORD)(lines + 1) >> 1;
    }

    return (UWORD)lines;
}
