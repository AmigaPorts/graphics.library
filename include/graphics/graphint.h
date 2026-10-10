/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/nodes.h>
#include <exec/types.h>

// Record used by the amiga.lib AddTOFTask/RemTOFTask interface.
struct Isrvstr
{
    struct Node is_Node;
    struct Isrvstr* Iptr;
    LONG (*code)();
    LONG __STDARGS__ (*ccode) __CLIB_PROTOTYPE((APTR));
    APTR Carg;
};
