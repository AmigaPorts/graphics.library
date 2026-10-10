/* SPDX-License-Identifier: APL-1.1
 * Adapted for the classic m68k AmigaOS ABI; see ../../HEADERS.md.
 */
#ifndef GRAPHICS_GFXNODES_H
#define GRAPHICS_GFXNODES_H

/*
    Copyright © 1995-2010, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Extended node for graphics.library.
    Lang: english
*/

#ifndef EXEC_NODES_H
#   include <exec/nodes.h>
#endif

struct ExtendedNode
{
    struct Node * xln_Succ;
    struct Node * xln_Pred;

    UBYTE   xln_Type;	      /* NT_GRAPHICS */
    BYTE    xln_Pri;
    char  * xln_Name;
    UBYTE   xln_Subsystem;    /* see below */
    UBYTE   xln_Subtype;      /* see below */
    struct GfxBase *xln_Library;
    LONG (* __ASM__ xln_Init) __CLIB_PROTOTYPE((__REG__(a0, struct ExtendedNode *), __REG__(d0, UWORD)));
};

/* xln_Type */
#define VIEW_EXTRA_TYPE      1
#define VIEWPORT_EXTRA_TYPE  2
#define SPECIAL_MONITOR_TYPE 3
#define MONITOR_SPEC_TYPE    4

/* xln_Subsystem */
#define SS_GRAPHICS 0x02

#endif /* GRAPHICS_GFXNODES_H */
