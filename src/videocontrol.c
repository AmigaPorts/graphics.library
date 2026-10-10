/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/videocontrol.h"
#include "internal/library.h"

#include <graphics/displayinfo.h>
#define __NOLIBBASE__
#define GRAPHICS_BASE_NAME graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/utility.h>

// These are destinations in the public Copper list representation, not MMIO
// addresses. Updating that representation does not execute a Copper program.
#define DISPLAY_CONTROL 0x0100
#define BORDER_CONTROL 0x0106
#define BORDER_CONTROL_MASK 0x1cfe

ULONG pcVideoControl(struct ColorMap* map asm("a0"), struct TagItem* tags asm("a1"),
    struct GfxBase* graphics asm("a6"))
{
    struct VideoControlChanges changes;
    ULONG result = pcVideoControlTags(map, tags, &changes, (APTR)pcNativeViewPortVectors, graphics);

    if (changes.immediate && !changes.remake && (changes.palette || changes.border)) {
        struct ExecBase* SysBase = (APTR)graphics->ExecBase;
        ObtainSemaphore(graphics->ActiViewCprSemaphore);

        pcUpdateNativeVideoControlLists(map, &changes, pcNativeViewportPalette, graphics);
        pcUpdateNativeViewBackground(graphics->current_monitor, graphics);

        ReleaseSemaphore(graphics->ActiViewCprSemaphore);
    }

    return result;
}

static BOOL updateMergedList(UWORD* list, UWORD destination, UWORD value, UWORD mask)
{
    if (!list) {
        return FALSE;
    }

    BOOL written = FALSE;

    // The original skips the leading waits, then updates only the first run of
    // moves. A later wait ends that run, even if more matching moves follow it.
    while (list[0] & 1) {
        list += 2;
    }

    while (!(list[0] & 1)) {
        if (list[0] == destination) {
            list[1] = (list[1] & ~mask) | (value & mask);
            written = TRUE;
        }

        list += 2;
    }

    return written;
}

UWORD pcUpdateViewPortControlLists(struct ViewPort* viewport, UWORD destination, UWORD value, UWORD mask)
{
    if ((viewport->Modes & VP_HIDE) || !viewport->DspIns) {
        return 0;
    }

    struct CopList* list = viewport->DspIns;
    UWORD written = 0;

    if (updateMergedList(list->CopLStart, destination, value, mask)) {
        written |= 1;
    }

    if (updateMergedList(list->CopSStart, destination, value, mask)) {
        written |= 2;
    }

    struct CopIns* instruction = list->CopIns;

    while (instruction) {
        switch (instruction->OpCode & 3) {
            case COPPER_MOVE:
                if ((UWORD)instruction->DESTADDR == destination) {
                    instruction->DESTDATA = (instruction->DESTDATA & ~mask) | (value & mask);
                    written |= 4;
                }
                break;

            case COPPER_WAIT:
                if (instruction->HWAITPOS == 255) {
                    return written;
                }
                break;

            default:
                // Preserve the original updater's direct instruction pointer
                // traversal, including opcode 3 and high opcode flag bits.
                instruction = (APTR)instruction->NXTLIST;
                continue;
        }

        ++instruction;
    }

    return written;
}

static UWORD borderControl(struct ColorMap* map, struct GfxBase* graphics)
{
    UBYTE difference = map->Bp_1_base - map->Bp_0_base;
    UWORD offset = 0;

    while (difference > 1) {
        difference >>= 1;
        ++offset;
    }

    UWORD value = offset << 10;

    if (map->Flags & BORDER_BLANKING) {
        value |= 0x20;
    }

    if (map->Flags & BORDER_NOTRANSPARENCY) {
        value |= 0x10;
    }

    if (map->Flags & BORDERSPRITES) {
        value |= 2;
    }

    UBYTE resolution = (BYTE)map->SpriteResolution < 0 ? map->SpriteResDefault : map->SpriteResolution;

    value |= (UBYTE)((resolution >> 2) | (resolution << 6));

    return value | graphics->BP3Bits;
}

UWORD pcVideoControlLists(struct ColorMap* map, const struct VideoControlChanges* changes,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics)
{
    if (!changes->immediate || changes->remake || (!changes->palette && !changes->border)) {
        return 0;
    }

    UWORD written = 0;

    if (changes->palette) {
        pcUpdateViewportPalette(map->cm_vp, default_palette, graphics);
    }

    if (changes->border) {
        written = pcUpdateViewPortControlLists(map->cm_vp, BORDER_CONTROL, borderControl(map, graphics), BORDER_CONTROL_MASK);
    }

    if (changes->blanking) {
        pcUpdateViewPortControlLists(map->cm_vp, DISPLAY_CONTROL, 1, 1);
    }

    return written;
}

void pcUpdateNativeVideoControlLists(struct ColorMap* map, const struct VideoControlChanges* changes,
    ViewPortPaletteFunction default_palette, struct GfxBase* graphics)
{
    UWORD written = pcVideoControlLists(map, changes, default_palette, graphics);

    if (written) {
        pcUpdateViewPortSpriteControl(pcPreparedViewPorts(graphics), pcMergedViews(graphics),
            map->cm_vp->DspIns, borderControl(map, graphics), written);
    }
}

static void bindVideoMode(struct ColorMap* map, APTR default_vectors, struct GfxBase* graphics)
{
    struct VecInfo info;

    // Preserve the public vector owner, including P96's display database.
    // A failed query leaves the previous table intact. As in the original,
    // any nonzero result accepts the returned VecInfo; it is not a size test.
    if (GetDisplayInfoData(NULL, (APTR)&info, sizeof(info), DTAG_VEC, map->VPModeID)) {
        map->cm_vpe->VecTable = info.Vec ? info.Vec : default_vectors;
    }
}

static void flagQuery(struct TagItem* tag, UBYTE flags, UBYTE mask, ULONG clear_tag, ULONG set_tag)
{
    tag->ti_Tag = (flags & mask) ? set_tag : clear_tag;
}

static void chromaQuery(struct ColorMap* map, struct TagItem* tag, struct ExecBase* SysBase)
{
    flagQuery(tag, map->Flags, COLORMAP_TRANSPARENCY, VTAG_CHROMAKEY_CLR, VTAG_CHROMAKEY_SET);
    ULONG count = 0;
    const UWORD* colors = map->ColorTable;

    Forbid();

    ULONG entries = map->Count;

    for (ULONG i = 0; i < entries; ++i) {
        if (colors[i] & 0x8000) {
            ++count;
        }
    }

    Permit();
    tag->ti_Data = count;
}

static void addBatch(struct ColorMap* map, struct TagItem* incoming, struct ExecBase* SysBase)
{
    Forbid();

    struct TagItem* previous = map->cm_batch_items;
    struct TagItem* end = incoming;

    while (end && end->ti_Tag) {
        if (end->ti_Tag == VTAG_NEXTBUF_CM) {
            end = (APTR)end->ti_Data;
        } else {
            ++end;
        }
    }

    if (end != incoming) {
        end->ti_Tag = VTAG_NEXTBUF_CM;
        end->ti_Data = (ULONG)previous;
        map->cm_batch_items = incoming;
    }

    Permit();
}

ULONG pcVideoControlTags(struct ColorMap* map, struct TagItem* tags, struct VideoControlChanges* changes,
    APTR default_vectors, struct GfxBase* graphics)
{
    struct ExecBase* SysBase = (APTR)graphics->ExecBase;
    struct Library* UtilityBase = graphics->UtilBase;

    changes->immediate = NULL;
    changes->remake = FALSE;
    changes->palette = FALSE;
    changes->border = FALSE;
    changes->blanking = FALSE;

    if (!map || !map->Type) {
        return ~0UL;
    }

    struct TagItem* cursor = tags;
    struct TagItem* tag;

    while ((tag = NextTagItem(&cursor))) {
        ULONG data = tag->ti_Data;
        BOOL advanced = (graphics->ChipRevBits0 & GFXF_AA_LISA) != 0;

        switch (tag->ti_Tag) {
            case VTAG_CHROMAKEY_CLR:
            case VTAG_CHROMAKEY_SET:
                map->Flags = (map->Flags & ~COLORMAP_TRANSPARENCY) |
                    (tag->ti_Tag == VTAG_CHROMAKEY_SET ? COLORMAP_TRANSPARENCY : 0);
                changes->palette = TRUE;
                changes->remake = TRUE;
                break;

            case VTAG_BITPLANEKEY_CLR:
            case VTAG_BITPLANEKEY_SET:
                map->Flags = (map->Flags & ~COLORPLANE_TRANSPARENCY) |
                    (tag->ti_Tag == VTAG_BITPLANEKEY_SET ? COLORPLANE_TRANSPARENCY : 0);
                changes->remake = TRUE;
                break;

            case VTAG_BORDERBLANK_CLR:
            case VTAG_BORDERBLANK_SET:
                map->Flags = (map->Flags & ~BORDER_BLANKING) |
                    (tag->ti_Tag == VTAG_BORDERBLANK_SET ? BORDER_BLANKING : 0);
                changes->border = TRUE;
                changes->blanking = TRUE;
                break;

            case VTAG_BORDERNOTRANS_CLR:
            case VTAG_BORDERNOTRANS_SET:
                map->Flags = (map->Flags & ~BORDER_NOTRANSPARENCY) |
                    (tag->ti_Tag == VTAG_BORDERNOTRANS_SET ? BORDER_NOTRANSPARENCY : 0);
                changes->border = TRUE;
                changes->blanking = TRUE;
                break;

            case VTAG_CHROMA_PEN_CLR:
            case VTAG_CHROMA_PEN_SET:
                // The original compares the full signed index against Count.
                // Negative indices are not silently converted to unsigned.
                if ((LONG)data < (LONG)map->Count) {
                    UWORD* color = (UWORD*)map->ColorTable + (LONG)data;

                    *color = (*color & 0x7fff) | (tag->ti_Tag == VTAG_CHROMA_PEN_SET ? 0x8000 : 0);
                    changes->palette = TRUE;
                }
                break;

            case VTAG_CHROMA_PEN_GET:
                tag->ti_Tag = (((UWORD*)map->ColorTable)[(LONG)data] & 0x8000) ?
                    VTAG_CHROMA_PEN_SET : VTAG_CHROMA_PEN_CLR;
                break;

            case VTAG_CHROMA_PLANE_SET:
                if ((LONG)data < 8) {
                    map->TransparencyPlane = data;
                    changes->remake = TRUE;
                }
                break;

            case VTAG_CHROMA_PLANE_GET:
                tag->ti_Tag = VTAG_CHROMA_PLANE_SET;
                tag->ti_Data = map->TransparencyPlane;
                break;

            case VTAG_ATTACH_CM_SET: {
                struct ViewPort* viewport = (APTR)data;

                Forbid();

                if (viewport) {
                    if (viewport->ColorMap) {
                        viewport->ColorMap->cm_vp = NULL;
                    }

                    viewport->ColorMap = map;
                    map->cm_vp = viewport;
                }

                Permit();
                changes->remake = TRUE;
                break;
            }

            case VTAG_ATTACH_CM_GET:
                tag->ti_Tag = VTAG_ATTACH_CM_SET;
                tag->ti_Data = (ULONG)map->cm_vp;
                break;

            case VTAG_NEXTBUF_CM:
                cursor = (APTR)data;
                break;

            case VTAG_BATCH_CM_CLR:
                map->Flags &= ~VIDEOCONTROL_BATCH;
                break;

            case VTAG_BATCH_CM_SET:
                map->Flags |= VIDEOCONTROL_BATCH;
                break;

            case VTAG_BATCH_CM_GET:
                flagQuery(tag, map->Flags, VIDEOCONTROL_BATCH, VTAG_BATCH_CM_CLR, VTAG_BATCH_CM_SET);
                break;

            case VTAG_NORMAL_DISP_GET:
                tag->ti_Tag = VTAG_NORMAL_DISP_SET;
                tag->ti_Data = (ULONG)map->NormalDisplayInfo;
                break;

            case VTAG_NORMAL_DISP_SET:
                map->NormalDisplayInfo = (APTR)data;
                changes->remake = TRUE;
                break;

            case VTAG_COERCE_DISP_GET:
                tag->ti_Tag = VTAG_COERCE_DISP_SET;
                tag->ti_Data = (ULONG)map->CoerceDisplayInfo;
                break;

            case VTAG_COERCE_DISP_SET:
                map->CoerceDisplayInfo = (APTR)data;
                changes->remake = TRUE;
                break;

            case VTAG_VIEWPORTEXTRA_GET:
                tag->ti_Tag = VTAG_VIEWPORTEXTRA_SET;
                tag->ti_Data = (ULONG)map->cm_vpe;
                break;

            case VTAG_VIEWPORTEXTRA_SET:
                map->cm_vpe = (APTR)data;
                changes->remake = TRUE;
                chromaQuery(map, tag, SysBase);
                break;

            case VTAG_CHROMAKEY_GET:
                chromaQuery(map, tag, SysBase);
                break;

            case VTAG_BITPLANEKEY_GET:
                flagQuery(tag, map->Flags, COLORPLANE_TRANSPARENCY, VTAG_BITPLANEKEY_CLR, VTAG_BITPLANEKEY_SET);
                break;

            case VTAG_BORDERBLANK_GET:
                flagQuery(tag, map->Flags, BORDER_BLANKING, VTAG_BORDERBLANK_CLR, VTAG_BORDERBLANK_SET);
                break;

            case VTAG_BORDERNOTRANS_GET:
                flagQuery(tag, map->Flags, BORDER_NOTRANSPARENCY, VTAG_BORDERNOTRANS_CLR, VTAG_BORDERNOTRANS_SET);
                break;

            case VTAG_BATCH_ITEMS_GET:
                tag->ti_Tag = VTAG_BATCH_ITEMS_SET;
                tag->ti_Data = (ULONG)map->cm_batch_items;
                break;

            case VTAG_BATCH_ITEMS_SET:
                map->cm_batch_items = (APTR)data;
                break;

            case VTAG_BATCH_ITEMS_ADD:
                addBatch(map, (APTR)data, SysBase);
                break;

            case VTAG_VPMODEID_GET:
                tag->ti_Tag = VTAG_VPMODEID_SET;
                tag->ti_Data = map->VPModeID;
                break;

            case VTAG_VPMODEID_SET: {
                BOOL attached = map->cm_vp != NULL;

                ObtainSemaphore(graphics->ActiViewCprSemaphore);
                map->VPModeID = data;

                if (attached && data != ~0UL) {
                    bindVideoMode(map, default_vectors, graphics);
                }

                ReleaseSemaphore(graphics->ActiViewCprSemaphore);
                changes->remake = TRUE;
                break;
            }

            case VTAG_VPMODEID_CLR:
                ObtainSemaphore(graphics->ActiViewCprSemaphore);
                map->VPModeID = ~0UL;

                if (map->cm_vpe) {
                    map->cm_vpe->VecTable = NULL;
                }

                ReleaseSemaphore(graphics->ActiViewCprSemaphore);
                changes->remake = TRUE;
                break;

            case VTAG_USERCLIP_GET:
                flagQuery(tag, map->Flags, USER_COPPER_CLIP, VTAG_USERCLIP_CLR, VTAG_USERCLIP_SET);
                break;

            case VTAG_USERCLIP_SET:
            case VTAG_USERCLIP_CLR:
                map->Flags = (map->Flags & ~USER_COPPER_CLIP) |
                    (tag->ti_Tag == VTAG_USERCLIP_SET ? USER_COPPER_CLIP : 0);
                changes->remake = TRUE;
                break;

            // Retain the original association of PF1 with Bp_1_base, and PF2
            // with Bp_0_base, despite the apparent reversal of the names.
            case VTAG_PF1_BASE_GET:
                tag->ti_Tag = VTAG_PF1_BASE_SET;
                tag->ti_Data = map->Bp_1_base;
                break;

            case VTAG_PF2_BASE_GET:
                tag->ti_Tag = VTAG_PF2_BASE_SET;
                tag->ti_Data = map->Bp_0_base;
                break;

            case VTAG_SPEVEN_BASE_GET:
                tag->ti_Tag = VTAG_SPEVEN_BASE_SET;
                tag->ti_Data = map->SpriteBase_Even;
                break;

            case VTAG_SPODD_BASE_GET:
                tag->ti_Tag = VTAG_SPODD_BASE_SET;
                tag->ti_Data = map->SpriteBase_Odd;
                break;

            case VTAG_PF1_BASE_SET:
            case VTAG_PF2_BASE_SET:
            case VTAG_SPEVEN_BASE_SET:
            case VTAG_SPODD_BASE_SET:
                if (advanced) {
                    switch (tag->ti_Tag) {
                        case VTAG_PF1_BASE_SET:
                            map->Bp_1_base = data;
                            break;

                        case VTAG_PF2_BASE_SET:
                            map->Bp_0_base = data;
                            break;

                        case VTAG_SPEVEN_BASE_SET:
                            map->SpriteBase_Even = data;
                            break;

                        case VTAG_SPODD_BASE_SET:
                            map->SpriteBase_Odd = data;
                            break;
                    }

                    changes->remake = TRUE;
                }
                break;

            case VTAG_BORDERSPRITE_GET:
                flagQuery(tag, map->Flags, BORDERSPRITES, VTAG_BORDERSPRITE_CLR, VTAG_BORDERSPRITE_SET);
                break;

            case VTAG_BORDERSPRITE_SET:
            case VTAG_BORDERSPRITE_CLR:
                if (advanced) {
                    map->Flags = (map->Flags & ~BORDERSPRITES) |
                        (tag->ti_Tag == VTAG_BORDERSPRITE_SET ? BORDERSPRITES : 0);
                    changes->border = TRUE;
                }
                break;

            case VTAG_SPRITERESN_SET:
            case VTAG_DEFSPRITERESN_SET: {
                UBYTE* resolution = tag->ti_Tag == VTAG_SPRITERESN_SET ?
                    &map->SpriteResolution : &map->SpriteResDefault;

                if (advanced && (ULONG)(LONG)(BYTE)*resolution != data) {
                    *resolution = data;
                    changes->border = TRUE;
                }
                break;
            }

            case VTAG_SPRITERESN_GET:
                tag->ti_Tag = VTAG_SPRITERESN_SET;
                tag->ti_Data = (LONG)(BYTE)map->SpriteResolution;
                break;

            case VTAG_DEFSPRITERESN_GET:
                tag->ti_Tag = VTAG_DEFSPRITERESN_SET;
                tag->ti_Data = (LONG)(BYTE)map->SpriteResDefault;
                break;

            case VTAG_PF1_TO_SPRITEPRI_SET:
            case VTAG_PF2_TO_SPRITEPRI_SET:
                if (map->cm_vp) {
                    ULONG priority = (LONG)data < 7 ? data : 7;
                    UBYTE old = map->cm_vp->SpritePriorities;

                    map->cm_vp->SpritePriorities = tag->ti_Tag == VTAG_PF1_TO_SPRITEPRI_SET ?
                        (old & 0x38) | priority : (old & 7) | (priority << 3);
                    changes->remake = TRUE;
                }
                break;

            case VTAG_PF1_TO_SPRITEPRI_GET:
                tag->ti_Tag = VTAG_PF1_TO_SPRITEPRI_SET;
                tag->ti_Data = map->cm_vp ? map->cm_vp->SpritePriorities & 7 : 4;
                break;

            case VTAG_PF2_TO_SPRITEPRI_GET:
                tag->ti_Tag = VTAG_PF2_TO_SPRITEPRI_SET;
                tag->ti_Data = map->cm_vp ? (map->cm_vp->SpritePriorities >> 3) & 7 : 4;
                break;

            case VTAG_IMMEDIATE:
                changes->immediate = (APTR)data;
                break;

            case VTAG_FULLPALETTE_SET:
                map->AuxFlags |= CMAF_FULLPALETTE;
                changes->remake = TRUE;
                break;

            case VTAG_FULLPALETTE_CLR:
                map->AuxFlags &= ~CMAF_FULLPALETTE;
                break;

            case VTAG_FULLPALETTE_GET:
                tag->ti_Tag = VTAG_FULLPALETTE_SET;
                tag->ti_Data = (map->AuxFlags & CMAF_FULLPALETTE) ? ~0UL : 0;
                break;

            case VC_IntermediateCLUpdate:
                map->AuxFlags = (map->AuxFlags & ~CMAF_NO_INTERMED_UPDATE) |
                    (data ? 0 : CMAF_NO_INTERMED_UPDATE);
                break;

            case VC_NoColorPaletteLoad:
                map->AuxFlags = (map->AuxFlags & ~CMAF_NO_COLOR_LOAD) | (data ? CMAF_NO_COLOR_LOAD : 0);
                changes->remake = TRUE;
                break;

            case VC_DUALPF_Disable:
                map->AuxFlags = (map->AuxFlags & ~CMAF_DUALPF_DISABLE) | (data ? CMAF_DUALPF_DISABLE : 0);
                changes->remake = TRUE;
                break;

            case VC_IntermediateCLUpdate_Query:
                *(ULONG*)data = (map->AuxFlags & CMAF_NO_INTERMED_UPDATE) ? 0 : ~0UL;
                break;

            case VC_NoColorPaletteLoad_Query:
                *(ULONG*)data = (map->AuxFlags & CMAF_NO_COLOR_LOAD) ? ~0UL : 0;
                break;

            case VC_DUALPF_Disable_Query:
                *(ULONG*)data = (map->AuxFlags & CMAF_DUALPF_DISABLE) ? ~0UL : 0;
                break;
        }
    }

    changes->remake |= !map->cm_vp;

    if (changes->immediate) {
        *changes->immediate = changes->remake ? 1 : 0;
    }

    return 0;
}
