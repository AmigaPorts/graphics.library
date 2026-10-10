/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/framebuffer_tree.h"

#include <stdint.h>

#define __NOLIBBASE__
#include <proto/devicetree.h>

static uint32_t cell(const UBYTE* value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
        ((uint32_t)value[2] << 8) | value[3];
}

static uint64_t number(const UBYTE* value, unsigned cells)
{
    return cells == 2 ? ((uint64_t)cell(value) << 32) | cell(value + 4) : cell(value);
}

static unsigned cells(APTR DeviceTreeBase, APTR key, const char* name, unsigned fallback)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)name);

    if (!property) {
        return fallback;
    }

    const UBYTE* value = DT_GetPropValue(property);

    if (!value || DT_GetPropLen(property) != 4) {
        return 0;
    }

    unsigned count = cell(value);

    return count == 1 || count == 2 ? count : 0;
}

static BOOL matches(const UBYTE* value, ULONG length, const char* expected)
{
    if (!value || !expected) {
        return FALSE;
    }

    ULONG offset = 0;

    BOOL found = FALSE;

    while (offset < length) {
        ULONG start = offset;

        while (offset < length && value[offset]) {
            ++offset;
        }

        if (offset == length) {
            return FALSE;
        }

        ULONG i = 0;

        while (i < offset - start && expected[i] && expected[i] == value[start + i]) {
            ++i;
        }

        if (i == offset - start && !expected[i]) {
            found = TRUE;
        }

        ++offset;
    }

    return found;
}

static BOOL enabled(APTR DeviceTreeBase, APTR key)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)"status");

    if (!property) {
        return TRUE;
    }

    ULONG length = DT_GetPropLen(property);

    const UBYTE* value = DT_GetPropValue(property);

    return (length == 3 && matches(value, length, "ok")) ||
        (length == 5 && matches(value, length, "okay"));
}

APTR pcGraphicsFindCompatible(APTR DeviceTreeBase, APTR root, APTR previous, const char* compatible)
{
    if (!DeviceTreeBase || !root || !compatible || !*compatible) {
        return NULL;
    }

    APTR key = previous ? previous : root;

    if (previous) {
        APTR parent = previous;

        while (parent && parent != root) {
            parent = DT_GetParent(parent);

            if (parent && !enabled(DeviceTreeBase, parent)) {
                return NULL;
            }
        }

        if (!parent) {
            return NULL;
        }
    }

    BOOL first = !previous;

    for (;;) {
        BOOL available = enabled(DeviceTreeBase, key);

        if (!first) {
            APTR child = available ? DT_GetChild(key, NULL) : NULL;

            while (!child && key != root) {
                APTR parent = DT_GetParent(key);

                if (!parent) {
                    return NULL;
                }

                child = DT_GetChild(parent, key);
                key = parent;
            }

            if (!child) {
                return NULL;
            }

            key = child;
            available = enabled(DeviceTreeBase, key);
        }

        first = FALSE;

        if (!available) {
            continue;
        }

        APTR property = DT_FindProperty(key, (CONST_STRPTR)"compatible");

        if (property && matches(DT_GetPropValue(property), DT_GetPropLen(property), compatible)) {
            return key;
        }
    }
}

static BOOL translateAddress(APTR DeviceTreeBase, APTR bus, uint64_t base, uint64_t span, ULONG* address)
{
    if (!DeviceTreeBase || !bus || !address) {
        return FALSE;
    }

    unsigned ac = cells(DeviceTreeBase, bus, "#address-cells", 2);

    unsigned sc = cells(DeviceTreeBase, bus, "#size-cells", 1);

    if (!ac || !sc) {
        return FALSE;
    }

    if (!span || span - 1 > UINT64_MAX - base ||
        (ac == 1 && (base > UINT32_MAX || span - 1 > UINT32_MAX - base))) {
        return FALSE;
    }

    for (;;) {
        APTR parent = DT_GetParent(bus);

        if (!parent) {
            break;
        }

        unsigned pac = cells(DeviceTreeBase, parent, "#address-cells", 2);

        APTR property = DT_FindProperty(bus, (CONST_STRPTR)"ranges");

        if (!pac || !property) {
            return FALSE;
        }

        const UBYTE* value = DT_GetPropValue(property);

        ULONG length = DT_GetPropLen(property);

        if (length) {
            ULONG stride = 4 * (ac + pac + sc);

            if (!value || length % stride) {
                return FALSE;
            }

            BOOL found = FALSE;

            uint64_t translated = 0;

            for (ULONG offset = 0; offset < length; offset += stride) {
                uint64_t child = number(value + offset, ac);

                uint64_t host = number(value + offset + 4 * ac, pac);

                uint64_t window = number(value + offset + 4 * (ac + pac), sc);

                if (!window || window - 1 > UINT64_MAX - child || window - 1 > UINT64_MAX - host ||
                    (ac == 1 && window - 1 > UINT32_MAX - child) ||
                    (pac == 1 && window - 1 > UINT32_MAX - host)) {
                    return FALSE;
                }

                if (base >= child && base - child < window && span <= window - (base - child)) {
                    if (found) {
                        return FALSE;
                    }

                    translated = host + (base - child);
                    found = TRUE;
                }
            }

            if (!found) {
                return FALSE;
            }

            base = translated;
        }
        // An empty ranges property is an identity mapping; an absent one is
        // deliberately rejected above. Cell counts are local, not inherited.
        if (pac == 1 && (base > UINT32_MAX || span - 1 > UINT32_MAX - base)) {
            return FALSE;
        }

        bus = parent;
        ac = pac;
        sc = cells(DeviceTreeBase, bus, "#size-cells", 1);

        if (!sc) {
            return FALSE;
        }
    }

    if (base > UINT32_MAX || span > UINT32_MAX || span - 1 > UINT32_MAX - base) {
        return FALSE;
    }

    *address = base;

    return TRUE;
}

static BOOL word(APTR DeviceTreeBase, APTR key, const char* name, ULONG* result)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)name);

    if (!property || DT_GetPropLen(property) != 4) {
        return FALSE;
    }

    const UBYTE* data = DT_GetPropValue(property);

    if (!data) {
        return FALSE;
    }

    *result = cell(data);

    return TRUE;
}

static BOOL text(APTR DeviceTreeBase, APTR key, const char* name, const char* expected)
{
    APTR property = DT_FindProperty(key, (CONST_STRPTR)name);

    if (!property) {
        return FALSE;
    }

    ULONG size = DT_GetPropLen(property);

    const UBYTE* data = DT_GetPropValue(property);

    if (!data || !size) {
        return FALSE;
    }

    for (ULONG i = 0; i < size; ++i) {
        if (data[i] != (UBYTE)expected[i]) {
            return FALSE;
        }

        if (!data[i]) {
            return i + 1 == size;
        }
    }

    return FALSE;
}

static BOOL countCells(APTR DeviceTreeBase, APTR key, const char* name, ULONG fallback, ULONG* result)
{
    *result = fallback;

    if (DT_FindProperty(key, (CONST_STRPTR)name) && !word(DeviceTreeBase, key, name, result)) {
        return FALSE;
    }

    return *result == 1 || *result == 2;
}

BOOL pcReadGraphicsFramebuffer(APTR DeviceTreeBase, APTR key, struct FramebufferMode* mode)
{
    if (!DeviceTreeBase || !key || !mode) {
        return FALSE;
    }

    APTR chosen = DT_OpenKey((CONST_STRPTR)"/chosen");

    if (!chosen) {
        return FALSE;
    }

    BOOL success = FALSE;

    struct FramebufferMode pending = {0};

    if (DT_GetParent(key) != chosen ||
        pcGraphicsFindCompatible(DeviceTreeBase, key, NULL, "simple-framebuffer") != key) {
        goto done;
    }

    APTR root = DT_GetParent(chosen);

    if (!root || !word(DeviceTreeBase, key, "width", &pending.width) ||
        !word(DeviceTreeBase, key, "height", &pending.height) ||
        !word(DeviceTreeBase, key, "stride", &pending.stride)) {
        goto done;
    }

    static const struct {
        const char* name;
        enum FramebufferFormat format;
        UBYTE bytes;
    } formats[] = {
        {"r5g6b5", FRAMEBUFFER_RGB565, 2},
        {"x1r5g5b5", FRAMEBUFFER_RGB555, 2},
        {"a1r5g5b5", FRAMEBUFFER_RGB555, 2},
        {"r8g8b8", FRAMEBUFFER_RGB888, 3},
        {"x8r8g8b8", FRAMEBUFFER_XRGB8888, 4},
        {"a8r8g8b8", FRAMEBUFFER_XRGB8888, 4},
        {"a8b8g8r8", FRAMEBUFFER_XBGR8888, 4}
    };

    for (unsigned i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
        if (text(DeviceTreeBase, key, "format", formats[i].name)) {
            pending.format = formats[i].format;
            pending.pixel_bytes = formats[i].bytes;
            break;
        }
    }
    // The cross-endian guest must not substitute its own byte order for the
    // firmware's pixel layout. The PC exporter marks it explicitly.

    APTR little = DT_FindProperty(key, (CONST_STRPTR)"little-endian");

    APTR big = DT_FindProperty(key, (CONST_STRPTR)"big-endian");

    if (!!little == !!big || DT_GetPropLen(little ? little : big) != 0) {
        goto done;
    }

    pending.little_endian = little != NULL;

    ULONG ac, sc;

    if (!countCells(DeviceTreeBase, chosen, "#address-cells", 2, &ac) ||
        !countCells(DeviceTreeBase, chosen, "#size-cells", 1, &sc)) {
        goto done;
    }

    APTR reg = DT_FindProperty(key, (CONST_STRPTR)"reg");

    if (!reg || DT_GetPropLen(reg) != (ac + sc) * 4) {
        goto done;
    }

    const UBYTE* data = DT_GetPropValue(reg);

    if (!data) {
        goto done;
    }

    uint64_t address = cell(data), size = cell(data + ac * 4);

    if (ac == 2) {
        address = (address << 32) | cell(data + 4);
    }

    if (sc == 2) {
        size = (size << 32) | cell(data + ac * 4 + 4);
    }

    if (!pending.width || !pending.height || !pending.pixel_bytes ||
        (uint64_t)pending.width * pending.pixel_bytes > pending.stride ||
        (uint64_t)pending.stride * pending.height > size || size > UINT32_MAX ||
        !translateAddress(DeviceTreeBase, root, address, size, &pending.address)) {
        goto done;
    }

    pending.size = size;
    *mode = pending;
    success = TRUE;
done:
    DT_CloseKey(chosen);

    return success;
}
