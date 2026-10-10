/* SPDX-License-Identifier: APL-1.1
 * Adapted for the classic m68k AmigaOS ABI; see ../../HEADERS.md.
 */
#ifndef GRAPHICS_CLIP_H
#define GRAPHICS_CLIP_H

/*
    Copyright © 1995-2017, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Clip descriptions.
    Lang: english
*/

#ifndef EXEC_SEMAPHORES_H
#   include <exec/semaphores.h>
#endif
#ifndef EXEC_TYPES_H
#   include <exec/types.h>
#endif
#ifndef GRAPHICS_GFX_H
#   include <graphics/gfx.h>
#endif

#ifndef UTILITY_HOOKS_H
#   include <utility/hooks.h>
#endif

#define NEWLOCKS


struct Layer
{
    struct Layer     	    * front;
    struct Layer     	    * back;
    struct ClipRect  	    * ClipRect;
    struct RastPort  	    * rp;
    struct Rectangle          bounds;

    struct Layer             * nlink;

    UWORD   	    	      priority;
    UWORD   	    	      Flags;

    struct BitMap   	    * SuperBitMap;
    struct ClipRect 	    * SuperClipRect;

    APTR    	    	      Window;
    WORD    	    	      Scroll_X;
    WORD    	    	      Scroll_Y;

    struct ClipRect 	    * OnScreen;
    struct ClipRect 	    * OffScreen;
    struct ClipRect 	    * Backup;
    struct ClipRect 	    * SuperSaveClipRects;
    struct ClipRect 	    * Undamaged;

    struct Layer_Info       * LayerInfo;
    struct SignalSemaphore    Lock;
    struct Hook             * BackFill;

    ULONG   	    	      reserved1;


    struct Region   	    * ClipRegion;
    struct ClipRect          * clipped;

    WORD    	    	      Width;
    WORD    	    	      Height;

    UBYTE   	    	      reserved2[18];


    struct Region   	    * DamageList;
};


struct ClipRect
{
    struct ClipRect  	    * Next;
    struct ClipRect  	    * reservedlink;
    LONG                      obscured;
    struct BitMap    	    * BitMap;
    struct Rectangle          bounds;

    struct ClipRect          * vlink;
    struct Layer_Info        * home;
    APTR                      reserved;
#ifdef NEWCLIPRECTS_1_1
    LONG                      Flags;
#endif
};

#define CR_USERCLIPPED 16
#define CR_DAMAGECLIPPED 32

#define ISLESSX (1 << 0)
#define ISLESSY (1 << 1)
#define ISGRTRX (1 << 2)
#define ISGRTRY (1 << 3)

#endif /* GRAPHICS_CLIP_H */
