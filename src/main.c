/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/library.h"

extern const UBYTE rom_end;

// A library HUNK is not a Shell application. The linker keeps this entry first.
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
    return -1;
}

const char deviceName[] = LIBRARY_NAME;
const char deviceIdString[] = VERSION_STRING;

const struct Resident RomTag __attribute__((used, aligned(2))) = {
    RTC_MATCHWORD,
    (struct Resident*)&RomTag,
    (APTR)&rom_end,
    RTF_COLDSTART,
    LIBRARY_VERSION,
    NT_LIBRARY,
    // Original graphics priority: after utility/CIA, before layers (64).
    // The board's own initialization phase is separate from this sort order.
    65,
    (char*)deviceName,
    (char*)deviceIdString,
    (APTR)Init,
};
