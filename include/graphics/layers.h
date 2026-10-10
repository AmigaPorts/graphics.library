/* SPDX-License-Identifier: APL-1.1
 * Adapted for the classic m68k AmigaOS ABI; see ../../HEADERS.md.
 */
#ifndef GRAPHICS_LAYERS_H
#define GRAPHICS_LAYERS_H

/*
    Copyright © 1995-2011, The AROS Development Team. All rights reserved.
    $Id$

    Desc: Layer description
    Lang: english
*/

#include <exec/semaphores.h>
#include <graphics/gfx.h>


struct Layer_Info
{
    struct Layer    * top_layer;
    VOID            * resPtr1;
    VOID            * resPtr2;
    struct ClipRect * FreeClipRects;

    struct Rectangle bounds;

    struct SignalSemaphore Lock;
    struct MinList         gs_Head;

    WORD   PrivateReserve3;
    VOID * PrivateReserve4;

    UWORD  Flags;
    BYTE   res_count;
    BYTE   LockLayersCount;
    BYTE   PrivateReserve5;
    BYTE   UserClipRectsCount;
    struct Hook * BlankHook;
    VOID * resPtr5;
};

#define NEWLAYERINFO_CALLED 1

#define LAYERSIMPLE          (1<<0)
#define LAYERSMART           (1<<1)
#define LAYERSUPER           (1<<2)
#define LAYERUPDATING        (1<<4)
#define LAYERBACKDROP        (1<<6)
#define LAYERREFRESH         (1<<7)
#define LAYER_CLIPRECTS_LOST (1<<8)
#define LAYERIREFRESH        (1<<9)
#define LAYERIREFRESH2       (1<<10)

#define LAYERS_BACKFILL   ((struct Hook *)0)
#define LAYERS_NOBACKFILL ((struct Hook *)1)

#define LAYERSAVEBACK 0x800
#define LAYERHIDDEN 0x1000
#define LAYER_BACKMOST ((struct Layer *)0)
#define LAYER_FRONTMOST ((struct Layer *)1)

#endif /* GRAPHICS_LAYERS_H */
