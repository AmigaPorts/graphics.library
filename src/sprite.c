/* SPDX-License-Identifier: MPL-2.0 */
// Sprite storage, bitmap conversion and display-driver dispatch. Native sprite
// rendering and display ownership are separate from these operations.
#include "internal/sprite.h"
#include "internal/monitor.h"
#include "internal/framebuffer.h"
#include "internal/videocontrol.h"

#include <exec/execbase.h>
#include <exec/memory.h>
#include <graphics/view.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

// Width changes affect all native channels. Public sprite ownership is task
// serialized with Forbid, while the RAM lists share the renderer's semaphore.
// The installed notification runs after releasing that semaphore, as in the
// original library; it may rebuild the application's soft sprites.
static BOOL setSpriteWidth(UWORD width, WORD exempt, struct GfxBase* graphics)
{
    if (width == graphics->SpriteWidth) {
        return width != 0;
    }

    UBYTE fixed = graphics->SpriteReserved - (UBYTE)graphics->SoftSprites;

    if (exempt >= 0) {
        fixed &= ~(1U << (exempt & 7));
    }

    if (fixed) {
        return FALSE;
    }

    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    graphics->SpriteWidth = width;

    for (unsigned channel = 0; channel < 8; ++channel) {
        if (graphics->SoftSprites & (1U << channel)) {
            graphics->SprMoveDisable |= 1U << channel;
            UWORD* entry = &graphics->copinit->sprstrtup[channel * 4];
            entry[1] = graphics->copinit->sprstop[0];
            entry[3] = graphics->copinit->sprstop[1];
        }
    }

    UWORD mode = width == 1 ? 0 : width == 2 ?
        (graphics->MemType == 3 ? 4 : graphics->MemType) : 12;
    graphics->SpriteFMode = mode;
    graphics->copinit->fm0[1] = (graphics->copinit->fm0[1] & ~12U) | mode;

    if (graphics->ActiView) {
        for (struct ViewPort* viewport = graphics->ActiView->ViewPort; viewport; viewport = viewport->Next) {
            pcUpdateViewPortControlLists(viewport, 0x1fc, mode, 12);
        }
    }

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);

    if (((BYTE)exempt < 0 || !(graphics->SoftSprites & (1U << (exempt & 7)))) && graphics->IVector) {
        typedef void (*Notify)(APTR data asm("a1"), struct GfxBase* graphics asm("a6"));
        ((Notify)graphics->IVector)(graphics->IData, graphics);
    }

    return TRUE;
}

static LONG availableSprite(LONG number, const struct GfxBase* graphics)
{
    if ((WORD)number < 0) {
        for (number = 0; number < 8; ++number) {
            if (!(graphics->SpriteReserved & (1U << number))) {
                return number;
            }
        }

        return -1;
    }

    number &= 7;
    return (graphics->SpriteReserved & (1U << number)) ? -1 : number;
}

LONG pcGetSprite(struct SimpleSprite* sprite asm("a0"), LONG number asm("d0"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    Forbid();

    LONG selected = setSpriteWidth(1, -1, graphics) ? availableSprite((WORD)number, graphics) : -1;

    if (selected >= 0) {
        UBYTE bit = 1U << selected;
        graphics->ScanDoubledSprites |= bit;
        graphics->SpriteReserved |= bit;
        graphics->ExtSprites &= ~bit;
    }

    Permit();
    sprite->num = selected;
    return selected;
}

void pcFreeSprite(LONG number asm("d0"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    UBYTE bit = 1U << (number & 7);
    Forbid();
    ObtainSemaphore(graphics->ActiViewCprSemaphore);

    graphics->SpriteReserved &= ~bit;
    graphics->ExtSprites &= ~bit;
    graphics->SoftSprites &= ~bit;
    graphics->SimpleSprites[(WORD)number] = NULL;
    UWORD* entry = &graphics->copinit->sprstrtup[(WORD)number * 4];
    ULONG blank = (ULONG)graphics->copinit->sprstop;
    entry[1] = blank >> 16;
    entry[3] = blank;

    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
    setSpriteWidth(graphics->DefaultSpriteWidth, -1, graphics);
    Permit();
}

LONG pcGetExtSpriteA(struct ExtSprite* sprite asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    struct TagItem* cursor = (APTR)tags;
    struct TagItem* tag;
    WORD number = -1;
    ULONG attached = 0;
    BOOL soft = FALSE;
    BOOL doubled = FALSE;

    while ((tag = NextTagItem(&cursor))) {
        switch (tag->ti_Tag) {
            case GSTAG_SPRITE_NUM:
                number = tag->ti_Data;
                break;
            case GSTAG_ATTACHED:
                attached = tag->ti_Data;
                break;
            case GSTAG_SOFTSPRITE:
                soft |= tag->ti_Data != 0;
                break;
            case GSTAG_SCANDOUBLED:
                doubled |= tag->ti_Data != 0;
                break;
        }
    }

    Forbid();
    UWORD width = sprite->es_wordwidth;
    BOOL supported = width == 1 || (width == 2 && graphics->MemType) ||
        (width == 4 && graphics->MemType == 3);
    LONG selected = -1;

    if (supported && (width == graphics->SpriteWidth ||
        graphics->SpriteReserved == (UBYTE)graphics->SoftSprites) && !attached) {
        selected = availableSprite(number, graphics);
    }

    if (selected >= 0) {
        UBYTE bit = 1U << selected;
        sprite->es_SimpleSprite.num = selected;
        graphics->SpriteReserved |= bit;
        graphics->ExtSprites |= bit;

        if (soft) {
            graphics->SoftSprites |= bit;
        }

        if (doubled) {
            graphics->ScanDoubledSprites |= bit;
        }

        setSpriteWidth(width, selected, graphics);
    }

    Permit();
    // The notification can replace the descriptor's channel assignment.
    // The original reports its current unsigned WORD after Permit.
    return selected < 0 ? -1 : (LONG)(ULONG)sprite->es_SimpleSprite.num;
}

LONG pcChangeExtSpriteA(struct ViewPort* viewport asm("a0"), struct ExtSprite* old asm("a1"),
    struct ExtSprite* sprite asm("a2"), const struct TagItem* tags asm("a3"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    struct TagItem* cursor = (APTR)tags;
    struct TagItem* tag;
    ULONG doubled = 0;

    while ((tag = NextTagItem(&cursor))) {
        if (tag->ti_Tag == GSTAG_SCANDOUBLED) {
            doubled = tag->ti_Data;
        }
    }

    Forbid();
    BOOL changed = setSpriteWidth(sprite->es_wordwidth, old->es_SimpleSprite.num, graphics);
    UWORD number = old->es_SimpleSprite.num;
    sprite->es_SimpleSprite.num = number;

    if (changed) {
        UBYTE bit = 1U << (number & 7);
        graphics->SprMoveDisable &= ~bit;
        graphics->ScanDoubledSprites &= ~bit;

        if (doubled) {
            graphics->ScanDoubledSprites |= bit;
        }
    }

    Permit();

    if (!changed) {
        return 0;
    }

    MoveSprite(viewport, &sprite->es_SimpleSprite, (WORD)old->es_SimpleSprite.x, (WORD)old->es_SimpleSprite.y);
    return -1;
}

void pcDispatchMoveSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, LONG x, LONG y,
    ViewPortSpriteFunction default_method, struct GfxBase* graphics)
{
    const struct ViewPort* selected = viewport;

    if (!selected && graphics->ActiView) {
        selected = graphics->ActiView->ViewPort;
    }

    ViewPortSpriteFunction method = default_method;
    const struct ColorMap* map = selected ? selected->ColorMap : NULL;

    if (map && map->Type && map->cm_vpe && map->cm_vpe->VecTable) {
        method = ((const ViewPortSpriteFunction*)map->cm_vpe->VecTable)[0];
    }

    register struct ViewPort* argument asm("a0") = viewport;
    register struct SimpleSprite* object asm("a1") = sprite;
    register LONG horizontal asm("d0") = x;
    register LONG vertical asm("d1") = y;
    register APTR entry asm("a2") = (APTR)method;
    register struct GfxBase* base asm("a6") = graphics;

    __asm__ volatile(
        "jsr (%4)"
        : "+r"(argument), "+r"(object), "+r"(horizontal), "+r"(vertical), "+r"(entry)
        : "r"(base)
        : "cc", "memory");
}

void pcDispatchChangeSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, UWORD* data,
    ViewPortSpriteFunction default_method, struct GfxBase* graphics)
{
    sprite->posctldata = data;

    // The original enters the movement dispatcher directly, not the public
    // MoveSprite vector. Its WORD loads sign-extend the stored coordinates.
    pcDispatchMoveSprite(viewport, sprite, (WORD)sprite->x, (WORD)sprite->y, default_method, graphics);
}

void pcNativeMoveSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    LONG x asm("d0"), LONG y asm("d1"), struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;

    // Wait for an active field presentation before changing its selected
    // image. Monitor retirement uses the same exclusion. Foreign drivers
    // enter through their own method before this lock.
    ObtainSemaphore(graphics->ActiViewCprSemaphore);
    pcUpdateNativeSprite(viewport, sprite, x, y, graphics);
    ReleaseSemaphore(graphics->ActiViewCprSemaphore);
}

void pcMoveSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    LONG x asm("d0"), LONG y asm("d1"), struct GfxBase* graphics asm("a6"))
{
    pcDispatchMoveSprite(viewport, sprite, x, y, pcNativeMoveSprite, graphics);
}

void pcChangeSprite(struct ViewPort* viewport asm("a0"), struct SimpleSprite* sprite asm("a1"),
    UWORD* data asm("a2"), struct GfxBase* graphics asm("a6"))
{
    pcDispatchChangeSprite(viewport, sprite, data, pcNativeMoveSprite, graphics);
}

static struct SimpleSprite* attachedSprite(const struct SimpleSprite* sprite, const struct GfxBase* graphics)
{
    if (sprite->num & 1) {
        return NULL;
    }

    struct SimpleSprite* next = graphics->SimpleSprites[sprite->num + 1];

    return next && (next->posctldata[1] & SPRITE_ATTACHED) ? next : NULL;
}

static void spriteControlWords(const struct ViewPort* viewport, struct SimpleSprite* sprite,
    UWORD width, const struct GfxBase* graphics)
{
    UWORD x = sprite->x;
    WORD y = sprite->y;

    if (!viewport) {
        x *= 4;
    } else {
        ULONG modes = pcViewportModes(viewport);
        y += viewport->DyOffset;

        if (modes & DOUBLESCAN) {
            y *= 2;
        }

        if (modes & LACE) {
            y >>= 1;
        }

        x += viewport->DxOffset;

        if (!(modes & SUPERHIRES)) {
            x *= (modes & HIRES) ? 2 : 4;
        }

        if (!(graphics->ChipRevBits0 & GFXF_AA_LISA)) {
            x &= (modes & SUPERHIRES) ? ~1U : ~3U;
        }

        if (graphics->ExtSprites & (1U << (sprite->num & 7))) {
            x -= 4;
        }
    }

    y += graphics->ActiView->DyOffset;
    WORD minimum = 21;
    const struct MonitorSpec* monitor = graphics->current_monitor;

    if (monitor) {
        minimum = monitor->min_row;

        if (!(graphics->ChipRevBits0 & GFXF_AA_MLISA) && (monitor->ms_Flags & MSF_REQUEST_SPECIAL)) {
            ++minimum;
        }
    }

    if (y < minimum) {
        y = minimum;
    }

    x += (UWORD)(graphics->ActiView->DxOffset * 4);
    UWORD end = y + sprite->height;
    UWORD position = ((UWORD)y << 8) | ((x >> 3) & 0xff);
    UWORD control = ((x & 1) << 3) | ((x & 2) << 3) | ((x & 4) >> 2);

    if ((graphics->MonitorFlags & 0x10) && (graphics->ScanDoubledSprites & (1U << (sprite->num & 7)))) {
        end += sprite->height;
        position |= 0x80;
    }

    control |= sprite->posctldata[1] & SPRITE_ATTACHED;
    control |= (((UWORD)y & 0x100) >> 6) | (((UWORD)y & 0x200) >> 3);
    control |= (end << 8) | ((end & 0x100) >> 7) | ((end & 0x200) >> 4);

    if (!(graphics->ChipRevBits0 & GFXF_AA_LISA)) {
        control &= ~0x18;
    }

    // These words are part of the application's sprite image format. Native
    // presentation consumes that state without executing its hardware commands.
    *(ULONG*)sprite->posctldata = ((ULONG)position << 16) | control;

    if (!(width & 1)) {
        sprite->posctldata[(width & 4) ? 4 : 2] = control;
    }
}

static void spritePointer(UWORD* entry, const UWORD* data)
{
    entry[1] = (ULONG)data >> 16;
    entry[3] = (ULONG)data;
}

void pcUpdateNativeSprite(struct ViewPort* viewport, struct SimpleSprite* sprite, LONG x, LONG y,
    struct GfxBase* graphics)
{
    sprite->x = x;
    sprite->y = y;

    if (graphics->SprMoveDisable & (1U << (sprite->num & 7))) {
        return;
    }

    UWORD width = graphics->SpriteWidth;

    if (graphics->ActiView) {
        graphics->SimpleSprites[sprite->num] = sprite;
        struct SimpleSprite* next = attachedSprite(sprite, graphics);

        if (next) {
            pcUpdateNativeSprite(viewport, next, (WORD)sprite->x, (WORD)sprite->y, graphics);
        }

        if (sprite->posctldata) {
            spriteControlWords(viewport, sprite, width, graphics);
        }
    }

    BOOL hidden = FALSE;

    if (viewport) {
        WORD bottom = sprite->y;
        hidden = (viewport->Modes & VP_HIDE) || bottom > viewport->DHeight;

        if (viewport->Modes & LACE) {
            bottom >>= 1;
        }

        bottom += sprite->height;
        hidden |= bottom < 0;
    }

    UWORD* entry = &graphics->copinit->sprstrtup[sprite->num * 4];
    const UWORD* blank = graphics->copinit->sprstop;
    spritePointer(entry, hidden ? blank : sprite->posctldata);
    struct SimpleSprite* next = attachedSprite(sprite, graphics);

    if (next) {
        spritePointer(entry + 4, hidden ? blank : next->posctldata);
    }
}

BOOL pcReadNativeSpriteSources(struct NativeSpriteSources* sources, const struct GfxBase* graphics)
{
    if (!sources || !graphics || !graphics->copinit ||
        (graphics->SpriteWidth != 1 && graphics->SpriteWidth != 2 && graphics->SpriteWidth != 4)) {
        return FALSE;
    }

    sources->word_width = graphics->SpriteWidth;
    sources->scan_double = (graphics->MonitorFlags & 0x10) != 0;

    for (unsigned channel = 0; channel < 8; ++channel) {
        const UWORD* entry = &graphics->copinit->sprstrtup[channel * 4];
        struct NativeSpriteSource* source = &sources->channels[channel];
        const UWORD* image = (APTR)(((ULONG)entry[1] << 16) | entry[3]);
        source->image = image;
        source->position = image ? image[0] : 0;
        source->control = image ? image[sources->word_width] : 0;
    }

    return TRUE;
}

static void spriteDataGeometry(struct NativeSpriteGeometry* geometry, const UWORD* data,
    UWORD position, UWORD control, UWORD word_width, BOOL scan_double, unsigned channel)
{
    struct NativeSpriteGeometry result = {0};

    if (data) {
        result.data = data;
        result.width = 16 * word_width;
        result.row_bytes = 4 * word_width;
        result.x = ((position & (scan_double ? 0x7f : 0xff)) << 3) |
            ((control & 1) << 2) | ((control >> 3) & 3);
        result.start = (position >> 8) | ((control & 4) << 6) | ((control & 0x40) << 3);
        result.stop = (control >> 8) | ((control & 2) << 7) | ((control & 0x20) << 4);
        result.scan_doubled = scan_double && (position & 0x80);
        result.attached = (channel & 1) && (control & SPRITE_ATTACHED);
    }

    *geometry = result;
}

static void spriteGeometry(struct NativeSpriteGeometry* geometry,
    const struct NativeSpriteSource* source, UWORD word_width, BOOL scan_double, unsigned channel)
{
    spriteDataGeometry(geometry, source->image ? source->image + 2 * word_width : NULL,
        source->position, source->control, word_width, scan_double, channel);
}

BOOL pcReadNativeSpriteGeometry(struct NativeSpriteGeometry* geometry,
    const struct NativeSpriteSources* sources, unsigned channel)
{
    if (!geometry || !sources || channel >= 8 ||
        (sources->word_width != 1 && sources->word_width != 2 && sources->word_width != 4)) {
        return FALSE;
    }

    spriteGeometry(geometry, &sources->channels[channel], sources->word_width, sources->scan_double, channel);

    return TRUE;
}

BOOL pcResolveNativeSpriteInterval(struct NativeSpriteInterval* interval,
    const struct NativeSpriteGeometry* geometry, UWORD first_line, UWORD limit_line)
{
    if (!interval || !geometry || first_line > limit_line || limit_line > 1024 ||
        geometry->start > 1023 || geometry->stop > 1023) {
        return FALSE;
    }

    struct NativeSpriteInterval result = {0};

    if (geometry->data) {
        result.reload = geometry->stop >= first_line && geometry->stop < limit_line;

        // Once an image starts, a doubled row is reused on the opposite
        // parity. A stop on that line cannot fetch the following header.
        if (geometry->scan_doubled && geometry->start >= first_line &&
            geometry->start < geometry->stop && ((geometry->start ^ geometry->stop) & 1)) {
            result.reload = FALSE;
        }

        result.next = result.reload ? geometry->stop + 1 : limit_line;
        UWORD end = result.reload ? geometry->stop : limit_line;

        // Start must still be ahead in this field. Clipping an already
        // started image is the compositor's job, not a new start event.
        if (geometry->start >= first_line && geometry->start < end) {
            result.first = geometry->start;
            result.limit = end;
        }
    }

    *interval = result;

    return TRUE;
}

BOOL pcInitNativeSpriteCursor(struct NativeSpriteCursor* cursor,
    const struct NativeSpriteSources* sources, unsigned channel, UWORD first_line, UWORD limit_line)
{
    if (!cursor || !sources || channel >= 8 || first_line > limit_line || limit_line > 1024 ||
        (sources->word_width != 1 && sources->word_width != 2 && sources->word_width != 4)) {
        return FALSE;
    }

    *cursor = (struct NativeSpriteCursor){
        sources->channels[channel], sources->word_width, channel, first_line, limit_line,
        sources->scan_double, FALSE
    };

    return TRUE;
}

LONG pcNextNativeSpriteImage(struct NativeSpriteCursor* cursor, struct NativeSpriteImage* image)
{
    if (!cursor || !image || cursor->channel >= 8 || cursor->first_line > cursor->limit_line ||
        cursor->limit_line > 1024 ||
        (cursor->word_width != 1 && cursor->word_width != 2 && cursor->word_width != 4)) {
        return -1;
    }

    if (!cursor->source.image || cursor->first_line == cursor->limit_line) {
        return 0;
    }

    struct NativeSpriteSource source = cursor->source;

    if (cursor->pending) {
        source.position = source.image[0];
        source.control = source.image[cursor->word_width];
    }

    struct NativeSpriteImage result = {0};
    spriteGeometry(&result.geometry, &source, cursor->word_width, cursor->scan_double, cursor->channel);
    UWORD first_line = cursor->first_line;

    // A header loaded on its own start line enables the image, but that
    // line was used by the header, not a pixel row. Retain the captured
    // coordinate while resolving the first line that can consume pixels.
    if (cursor->pending && result.geometry.start + 1 == first_line &&
        result.geometry.start != result.geometry.stop) {
        --first_line;
    }

    pcResolveNativeSpriteInterval(&result.interval, &result.geometry, first_line, cursor->limit_line);

    if (result.interval.first < result.interval.limit) {
        if (result.interval.first < cursor->first_line) {
            result.interval.first = cursor->first_line;
        }

        result.data_first = result.interval.first;

        if (result.geometry.scan_doubled && ((result.data_first ^ result.geometry.start) & 1)) {
            ++result.data_first;
        }

        unsigned lines = result.interval.limit - result.data_first;
        result.rows = result.geometry.scan_doubled ? (lines + 1) / 2 : lines;
    }

    // Only a descriptor reload consumes the following header. In particular,
    // a terminator or an image continuing beyond this field needs no read
    // from the address after its rows. Delay that read until the next call.
    if (result.interval.reload && result.interval.next < cursor->limit_line) {
        cursor->source.image = result.geometry.data + result.rows * 2 * cursor->word_width;
        cursor->first_line = result.interval.next;
        cursor->pending = TRUE;
    } else {
        cursor->first_line = cursor->limit_line;
    }

    *image = result;

    return 1;
}

BOOL pcPlaceNativeSpriteRow(struct GraphicsFramebufferSprite* sprite,
    const struct NativeSpriteImage* image, const UWORD* preceding_row, UWORD line,
    WORD origin_x, WORD origin_y, UBYTE resolution)
{
    if (!sprite || !image || resolution > 2 || line > 1023 ||
        image->interval.first > image->interval.limit || image->interval.limit > 1024) {
        return FALSE;
    }

    struct GraphicsFramebufferSprite result = {0};

    if (line >= image->interval.first && line < image->interval.limit) {
        const struct NativeSpriteGeometry* geometry = &image->geometry;

        if ((geometry->width != 16 && geometry->width != 32 && geometry->width != 64) ||
            geometry->row_bytes != geometry->width / 4 ||
            image->data_first < image->interval.first || image->data_first > image->interval.limit ||
            image->data_first - image->interval.first > (geometry->scan_doubled ? 1 : 0)) {
            return FALSE;
        }

        const UWORD* row = preceding_row;

        if (line >= image->data_first) {
            unsigned index = line - image->data_first;

            if (geometry->scan_doubled) {
                index >>= 1;
            }

            if (!geometry->data || index >= image->rows) {
                return FALSE;
            }

            row = geometry->data + index * (geometry->row_bytes / 2);
        }

        if (!row) {
            return FALSE;
        }

        result.data = (const UBYTE*)row;
        result.size = result.stride = geometry->row_bytes;
        result.width = geometry->width;
        result.height = 1;
        result.dest_x = (LONG)geometry->x - origin_x;
        result.dest_y = (LONG)line - origin_y;
        result.x_shift = 2 - resolution;
    }

    *sprite = result;

    return TRUE;
}

BOOL pcInitNativeSpriteField(struct NativeSpriteField* field, const struct NativeSpriteSources* sources,
    const struct NativeSpriteRows* retained, UWORD first_line, UWORD limit_line)
{
    if (!field || !sources || !retained || first_line > limit_line || limit_line > 1024 ||
        (sources->word_width != 1 && sources->word_width != 2 && sources->word_width != 4)) {
        return FALSE;
    }

    field->retained = *retained;
    field->line = first_line;
    field->limit = limit_line;

    for (unsigned channel = 0; channel < 8; ++channel) {
        field->images[channel] = (struct NativeSpriteImage){0};
        pcInitNativeSpriteCursor(&field->cursors[channel], sources, channel, first_line, limit_line);
        pcNextNativeSpriteImage(&field->cursors[channel], &field->images[channel]);
    }

    return TRUE;
}

BOOL pcSelectNativeGelImage(struct NativeSpriteField* field, unsigned channel,
    const UWORD* data, UWORD position, UWORD control)
{
    if (!field || channel >= 8 || field->line > field->limit || field->limit > 1024) {
        return FALSE;
    }

    struct NativeSpriteCursor* cursor = &field->cursors[channel];
    unsigned width = cursor->word_width;

    if (width != 1 && width != 2 && width != 4) {
        return FALSE;
    }

    struct NativeSpriteImage image = {0};
    spriteDataGeometry(&image.geometry, data, position, control, width, cursor->scan_double, channel);
    pcResolveNativeSpriteInterval(&image.interval, &image.geometry, field->line, field->limit);
    image.data_first = image.interval.first;

    if (image.interval.first < image.interval.limit) {
        if (image.geometry.scan_doubled && ((image.data_first ^ image.geometry.start) & 1)) {
            ++image.data_first;
        }

        unsigned lines = image.interval.limit - image.data_first;
        image.rows = image.geometry.scan_doubled ? (lines + 1) / 2 : lines;
    }

    // GEL image storage consists of pixels, not a SimpleSprite header chain.
    // The next object using this channel is selected by another list entry.
    // Preserve the decoded stop, without reading beyond this object's rows.
    image.interval.reload = FALSE;
    cursor->first_line = cursor->limit_line;
    cursor->pending = FALSE;
    field->images[channel] = image;

    return TRUE;
}

LONG pcNextNativeSpriteLine(struct NativeSpriteField* field, struct GraphicsFramebufferSprites* sprites,
    WORD origin_x, WORD origin_y, UBYTE resolution)
{
    if (!field || !sprites || resolution > 2 || field->line > field->limit || field->limit > 1024) {
        return -1;
    }

    if (field->line == field->limit) {
        return 0;
    }

    UWORD line = field->line;
    sprites->attached_pairs = 0;

    for (unsigned channel = 0; channel < 8; ++channel) {
        struct NativeSpriteImage* image = &field->images[channel];
        struct NativeSpriteCursor* cursor = &field->cursors[channel];

        if (image->interval.reload && line + 1 == image->interval.next) {
            if (cursor->first_line < field->limit) {
                pcNextNativeSpriteImage(cursor, image);
            } else {
                // A final-line header has no following pixel interval, but
                // its attachment still affects the other member of the pair.
                const UWORD* header = image->geometry.data + image->rows * 2 * cursor->word_width;
                struct NativeSpriteSource source = {header, header[0], header[cursor->word_width]};
                *image = (struct NativeSpriteImage){0};
                spriteGeometry(&image->geometry, &source, cursor->word_width, cursor->scan_double, channel);
            }
        }

        UWORD* retained = field->retained.words[channel];
        UWORD* row = field->rows[channel];
        unsigned width = cursor->word_width;

        struct GraphicsFramebufferSprite* placed = &sprites->images[channel];
        pcPlaceNativeSpriteRow(placed, image, row, line, origin_x, origin_y, resolution);

        if (placed->height) {
            if (line >= image->data_first &&
                (!image->geometry.scan_doubled || !((line - image->data_first) & 1))) {
                const UWORD* source = (const UWORD*)placed->data;

                for (unsigned plane = 0; plane < 2; ++plane) {
                    for (unsigned word = 0; word < 4; ++word) {
                        retained[plane * 4 + word] = source[plane * width + (word < width ? word : 0)];
                    }
                }
            }

            for (unsigned plane = 0; plane < 2; ++plane) {
                for (unsigned word = 0; word < width; ++word) {
                    row[plane * width + word] = retained[plane * 4 + word];
                }
            }

            placed->data = (const UBYTE*)row;
        }

        if (image->geometry.attached) {
            sprites->attached_pairs |= 1U << (channel / 2);
        }
    }

    ++field->line;

    return 1;
}

static void expandRow(UWORD* row)
{
    // Double each source pixel. Work backwards so the expanded words cannot
    // overwrite source pixels still needed for this row.
    for (int word = 7; word >= 0; --word) {
        UWORD source = row[word];
        ULONG expanded = 0;
        for (unsigned bit = 0; bit < 16; ++bit) {
            expanded = (expanded << 2) | ((source & 0x8000) ? 3 : 0);
            source <<= 1;
        }
        row[2 * word] = expanded >> 16;
        row[2 * word + 1] = expanded;
    }
}

static void contractRow(UWORD* row)
{
    // Retain every other pixel, starting with the most significant bit.
    UBYTE* output = (APTR)row;
    for (unsigned word = 0; word < 16; ++word) {
        UWORD source = row[word];
        UBYTE contracted = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            contracted = (contracted << 1) | (source >> 15);
            source <<= 2;
        }
        output[word] = contracted;
    }
    for (unsigned word = 8; word < 16; ++word) { row[word] = 0; }
}

struct ExtSprite* pcAllocSpriteDataA(const struct BitMap* bitmap asm("a2"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;
    UWORD height = bitmap->Rows, width = 1, old_format = 0;
    WORD x_replication = 0, y_replication = 0;
    BOOL attached = FALSE;
    ULONG source_bytes = bitmap->BytesPerRow;
    if (bitmap->pad == 0x805c) { source_bytes = (ULONG)bitmap->Planes[1] - (ULONG)bitmap->Planes[0]; }
    UWORD source_words = source_bytes >> 1;
    struct TagItem* cursor = (APTR)tags;
    struct TagItem* tag;
    while ((tag = NextTagItem(&cursor))) {
        switch (tag->ti_Tag) {
            case SPRITEA_Width:
                width = (UBYTE)tag->ti_Data ? (UWORD)tag->ti_Data : 16;
                width = (UWORD)(width + 15) >> 4;
                if (width == 3) { width = 4; }
                break;
            case SPRITEA_XReplication: x_replication = tag->ti_Data; break;
            case SPRITEA_YReplication: y_replication = tag->ti_Data; break;
            case SPRITEA_OutputHeight: height = tag->ti_Data; break;
            case SPRITEA_Attached: if (tag->ti_Data) { attached = TRUE; } break;
            case SPRITEA_OldDataFormat: old_format = tag->ti_Data; break;
        }
    }

    ULONG stride = old_format ? 4 : bitmap->BytesPerRow;
    if (old_format) { source_words = 1; }
    // OutputHeight is the requested row count. Negative Y replication skips
    // input rows; a positive value does not synthesize additional rows here.
    UWORD skip = (WORD)-y_replication > 0 ? -y_replication : 0;
    skip &= 63;
    stride = skip >= 32 ? 0 : stride << skip;
    UWORD required = width, shift = (x_replication < 0 ? -x_replication : x_replication) & 63;
    if (x_replication < 0) { required = shift >= 16 ? 0 : (UWORD)(required << shift); }
    else {
        required = shift >= 16 ? 0 : required >> shift;
        if (!required) { required = 1; }
    }
    if (source_words > required) { source_words = required; }

    struct ExtSprite* sprite = AllocMem(sizeof(*sprite), 0);
    if (!sprite) { return NULL; }
    sprite->es_wordwidth = width;
    sprite->es_SimpleSprite.height = height;
    if (old_format && !x_replication && !y_replication && width == 1) {
        sprite->es_SimpleSprite.posctldata = (APTR)bitmap;
        // Only the high byte is set by the original borrowed-data path. The
        // remaining SimpleSprite fields and low flag byte retain their allocation contents.
        ((UBYTE*)&sprite->es_flags)[0] = 0xff;
        return sprite;
    }
    sprite->es_flags = 0;
    ULONG padding = (width & 63) >= 32 ? 0 : 2UL << (width & 63);
    ULONG bytes = ((ULONG)height * width * 2 + padding) * 2 + 4;
    UBYTE* storage = AllocVec(bytes, MEMF_CHIP);
    if (!storage) { FreeMem(sprite, sizeof(*sprite)); return NULL; }
    UWORD* output = (APTR)(storage + 4);
    sprite->es_SimpleSprite.posctldata = output;
    UWORD control = attached ? SPRITE_ATTACHED : 0;
    *output++ = 0; *output++ = control;
    if (width >= 2) { *output++ = control; *output++ = control; }
    if (width >= 4) { *output++ = control; *output++ = 0; *output++ = 0; *output++ = 0; }
    const UBYTE* planes[2];
    if (old_format) { planes[0] = (const UBYTE*)bitmap + 4; planes[1] = (const UBYTE*)bitmap + 6; }
    else {
        planes[0] = bitmap->Planes[attached ? 2 : 0];
        planes[1] = bitmap->Planes[attached ? 3 : 1];
    }
    for (ULONG line = 0; line < (UWORD)(height * 2); ++line) {
        UWORD row[16] = {0};
        unsigned plane = line & 1;
        const UWORD* source = (APTR)planes[plane];
        CopyMem((APTR)source, row, (ULONG)source_words * sizeof(*row));
        if (x_replication < 0) {
            contractRow(row);
            if (!(x_replication & 1)) { contractRow(row); }
        } else if (x_replication) {
            expandRow(row);
            if (x_replication & 2) { expandRow(row); }
        }
        CopyMem(row, output, (ULONG)width * sizeof(*row));
        output += width;
        planes[plane] += stride;
    }
    *output++ = 0; *output++ = 0;
    if (width >= 2) { *output++ = 0; *output++ = 0; }
    if (width >= 4) { *output++ = 0; *output++ = 0; *output++ = 0; *output++ = 0; }
    return sprite;
}

void pcFreeSpriteData(struct ExtSprite* sprite asm("a2"), struct GfxBase* graphics asm("a6"))
{
    if (!sprite) { return; }
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    if (!sprite->es_flags) { FreeVec((UBYTE*)sprite->es_SimpleSprite.posctldata - 4); }
    FreeMem(sprite, sizeof(*sprite));
}
