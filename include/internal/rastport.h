/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>

void pcInitRastPort(struct RastPort* port asm("a1"), struct GfxBase* graphics asm("a6"));
void pcMove(struct RastPort* port asm("a1"), WORD x asm("d0"), WORD y asm("d1"));
void pcSetAPen(struct RastPort* port asm("a1"), UBYTE pen asm("d0"));
void pcSetBPen(struct RastPort* port asm("a1"), UBYTE pen asm("d0"));
void pcSetDrMd(struct RastPort* port asm("a1"), UBYTE mode asm("d0"));
void pcSetABPenDrMd(struct RastPort* port asm("a1"), ULONG foreground asm("d0"),
    ULONG background asm("d1"), ULONG mode asm("d2"));
ULONG pcGetAPen(struct RastPort* port asm("a0"));
ULONG pcGetBPen(struct RastPort* port asm("a0"));
ULONG pcGetDrMd(struct RastPort* port asm("a0"));
ULONG pcGetOutlinePen(struct RastPort* port asm("a0"));
ULONG pcSetOutlinePen(struct RastPort* port asm("a0"), ULONG pen asm("d0"));
ULONG pcSetWriteMask(struct RastPort* port asm("a0"), ULONG mask asm("d0"));
void pcSetMaxPen(struct RastPort* port asm("a0"), ULONG pen asm("d0"));
void pcSetRPAttrsA(struct RastPort* port asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));
void pcGetRPAttrsA(struct RastPort* port asm("a0"), const struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"));

// Internal cached drawing-state update, without restarting a line pattern.
void pcUpdateMinterms(struct RastPort* port);
