/* SPDX-License-Identifier: MPL-2.0 */
// SDK vector order, including private slots. NULL denotes unfinished work,
// never a callable fallback: Init refuses publication until every slot exists.
#include "internal/library.h"
#include "internal/area.h"
#include "internal/bitmap.h"
#include "internal/blitter.h"
#include "internal/scroll.h"
#include "internal/flood.h"
#include "internal/sprite.h"
#include "internal/color.h"
#include "internal/copper.h"
#include "internal/ellipse.h"
#include "internal/font.h"
#include "internal/frame.h"
#include "internal/gels.h"
#include "internal/initialize.h"
#include "internal/layer.h"
#include "internal/line.h"
#include "internal/memory.h"
#include "internal/monitor.h"
#include "internal/display.h"
#include "internal/node.h"
#include "internal/native.h"
#include "internal/pixel.h"
#include "internal/rastport.h"
#include "internal/region.h"
#include "internal/text.h"
#include "internal/videocontrol.h"

const APTR graphicsFunctions[GRAPHICS_VECTOR_COUNT + 1] = {
    (APTR)pcGraphicsOpen,     // -6
    (APTR)pcGraphicsClose,    // -12
    (APTR)pcGraphicsExpunge,  // -18
    (APTR)pcGraphicsReserved, // -24
    (APTR)pcBltBitMap, // -30 BltBitMap
    (APTR)pcBltTemplate, // -36 BltTemplate
    (APTR)pcClearEOL, // -42 ClearEOL
    (APTR)pcClearScreen, // -48 ClearScreen
    (APTR)pcTextLength, // -54 TextLength
    (APTR)pcText, // -60 Text
    (APTR)pcSetFont, // -66 SetFont
    (APTR)pcOpenFont, // -72 OpenFont
    (APTR)pcCloseFont, // -78 CloseFont
    (APTR)pcAskSoftStyle, // -84 AskSoftStyle
    (APTR)pcSetSoftStyle, // -90 SetSoftStyle
    (APTR)pcAddBob, // -96 AddBob
    (APTR)pcAddVSprite, // -102 AddVSprite
    (APTR)pcDoCollision, // -108 DoCollision
    (APTR)pcDrawGList, // -114 DrawGList
    (APTR)pcInitGels, // -120 InitGels
    (APTR)pcInitMasks, // -126 InitMasks
    (APTR)pcRemIBob, // -132 RemIBob
    (APTR)pcRemVSprite, // -138 RemVSprite
    (APTR)pcSetCollision, // -144 SetCollision
    (APTR)pcSortGList, // -150 SortGList
    (APTR)pcAddAnimOb, // -156 AddAnimOb
    (APTR)pcAnimate, // -162 Animate
    (APTR)pcGetGBuffers, // -168 GetGBuffers
    (APTR)pcInitGMasks, // -174 InitGMasks
    (APTR)pcDrawEllipse, // -180 DrawEllipse
    (APTR)pcAreaEllipse, // -186 AreaEllipse
    (APTR)pcLoadRGB4, // -192 LoadRGB4
    (APTR)pcInitRastPort, // -198 InitRastPort
    (APTR)pcInitVPort, // -204 InitVPort
    (APTR)pcMrgCop, // -210 MrgCop
    (APTR)pcMakeVPort, // -216 MakeVPort
    (APTR)pcLoadView, // -222 LoadView
    (APTR)pcWaitBlit, // -228 WaitBlit
    (APTR)pcSetRast, // -234 SetRast
    (APTR)pcMove, // -240 Move
    (APTR)pcDraw, // -246 Draw
    (APTR)pcAreaMove, // -252 AreaMove
    (APTR)pcAreaDraw, // -258 AreaDraw
    (APTR)pcAreaEnd, // -264 AreaEnd
    (APTR)pcWaitTOF, // -270 WaitTOF
    NULL, // -276 QBlit (not implemented)
    (APTR)pcInitArea, // -282 InitArea
    (APTR)pcSetRGB4, // -288 SetRGB4
    NULL, // -294 QBSBlit (not implemented)
    (APTR)pcBltClear, // -300 BltClear
    (APTR)pcRectFill, // -306 RectFill
    (APTR)pcBltPattern, // -312 BltPattern
    (APTR)pcReadPixel, // -318 ReadPixel
    (APTR)pcWritePixel, // -324 WritePixel
    (APTR)pcFlood, // -330 Flood
    (APTR)pcPolyDraw, // -336 PolyDraw
    (APTR)pcSetAPen, // -342 SetAPen
    (APTR)pcSetBPen, // -348 SetBPen
    (APTR)pcSetDrMd, // -354 SetDrMd
    (APTR)pcInitView, // -360 InitView
    (APTR)pcCBump, // -366 CBump
    (APTR)pcCMove, // -372 CMove
    (APTR)pcCWait, // -378 CWait
    NULL, // -384 VBeamPos (not implemented)
    (APTR)pcInitBitMap, // -390 InitBitMap
    (APTR)pcScrollRaster, // -396 ScrollRaster
    (APTR)pcWaitBOVP, // -402 WaitBOVP
    (APTR)pcGetSprite, // -408 GetSprite
    (APTR)pcFreeSprite, // -414 FreeSprite
    (APTR)pcChangeSprite, // -420 ChangeSprite
    (APTR)pcMoveSprite, // -426 MoveSprite
    (APTR)pcLockLayerRom, // -432 LockLayerRom
    (APTR)pcUnlockLayerRom, // -438 UnlockLayerRom
    (APTR)pcSyncSBitMap, // -444 SyncSBitMap
    (APTR)pcCopySBitMap, // -450 CopySBitMap
    (APTR)pcOwnBlitter, // -456 OwnBlitter
    (APTR)pcDisownBlitter, // -462 DisownBlitter
    (APTR)pcInitTmpRas, // -468 InitTmpRas
    (APTR)pcAskFont, // -474 AskFont
    (APTR)pcAddFont, // -480 AddFont
    (APTR)pcRemFont, // -486 RemFont
    (APTR)pcAllocRaster, // -492 AllocRaster
    (APTR)pcFreeRaster, // -498 FreeRaster
    (APTR)pcAndRectRegion, // -504 AndRectRegion
    (APTR)pcOrRectRegion, // -510 OrRectRegion
    (APTR)pcNewRegion, // -516 NewRegion
    (APTR)pcClearRectRegion, // -522 ClearRectRegion
    (APTR)pcClearRegion, // -528 ClearRegion
    (APTR)pcDisposeRegion, // -534 DisposeRegion
    (APTR)pcFreeVPortCopLists, // -540 FreeVPortCopLists
    (APTR)pcFreeCopList, // -546 FreeCopList
    (APTR)pcClipBlit, // -552 ClipBlit
    (APTR)pcXorRectRegion, // -558 XorRectRegion
    (APTR)pcFreeCprList, // -564 FreeCprList
    (APTR)pcGetColorMap, // -570 GetColorMap
    (APTR)pcFreeColorMap, // -576 FreeColorMap
    (APTR)pcGetRGB4, // -582 GetRGB4
    (APTR)pcScrollVPort, // -588 ScrollVPort
    (APTR)pcUCopperListInit, // -594 UCopperListInit
    (APTR)pcFreeGBuffers, // -600 FreeGBuffers
    (APTR)pcBltBitMapRastPort, // -606 BltBitMapRastPort
    (APTR)pcOrRegionRegion, // -612 OrRegionRegion
    (APTR)pcXorRegionRegion, // -618 XorRegionRegion
    (APTR)pcAndRegionRegion, // -624 AndRegionRegion
    (APTR)pcSetRGB4CM, // -630 SetRGB4CM
    (APTR)pcBltMaskBitMapRastPort, // -636 BltMaskBitMapRastPort
    // Original private aliases share the pen implementations and register ABI.
    (APTR)pcReleasePen, // -642 GfxInternal1
    (APTR)pcObtainPen, // -648 GfxInternal2
    (APTR)pcAttemptLockLayerRom, // -654 AttemptLockLayerRom
    (APTR)pcGfxNew, // -660 GfxNew
    (APTR)pcGfxFree, // -666 GfxFree
    (APTR)pcGfxAssociate, // -672 GfxAssociate
    (APTR)pcBitMapScale, // -678 BitMapScale
    (APTR)pcScalerDiv, // -684 ScalerDiv
    (APTR)pcTextExtent, // -690 TextExtent
    (APTR)pcTextFit, // -696 TextFit
    (APTR)pcGfxLookUp, // -702 GfxLookUp
    (APTR)pcVideoControl, // -708 VideoControl
    (APTR)pcOpenMonitor, // -714 OpenMonitor
    (APTR)pcCloseMonitor, // -720 CloseMonitor
    (APTR)pcFindDisplayInfo, // -726 FindDisplayInfo
    (APTR)pcNextDisplayInfo, // -732 NextDisplayInfo
    (APTR)pcAddDisplayInfo, // -738 AddDisplayInfo
    (APTR)pcAddDisplayInfoData, // -744 AddDisplayInfoData
    (APTR)pcSetDisplayInfoData, // -750 SetDisplayInfoData
    (APTR)pcGetDisplayInfoData, // -756 GetDisplayInfoData
    (APTR)pcFontExtent, // -762 FontExtent
    (APTR)pcReadPixelLine8, // -768 ReadPixelLine8
    (APTR)pcWritePixelLine8, // -774 WritePixelLine8
    (APTR)pcReadPixelArray8, // -780 ReadPixelArray8
    (APTR)pcWritePixelArray8, // -786 WritePixelArray8
    (APTR)pcGetVPModeID, // -792 GetVPModeID
    (APTR)pcModeNotAvailable, // -798 ModeNotAvailable
    (APTR)pcWeighTAMatch, // -804 WeighTAMatch
    (APTR)pcEraseRect, // -810 EraseRect
    (APTR)pcExtendFont, // -816 ExtendFont
    (APTR)pcStripFont, // -822 StripFont
    (APTR)pcCalcIVG, // -828 CalcIVG
    (APTR)pcAttachPalExtra, // -834 AttachPalExtra
    (APTR)pcObtainBestPenA, // -840 ObtainBestPenA
    (APTR)pcGfxInternal3, // -846 GfxInternal3
    (APTR)pcSetRGB32, // -852 SetRGB32
    (APTR)pcGetAPen, // -858 GetAPen
    (APTR)pcGetBPen, // -864 GetBPen
    (APTR)pcGetDrMd, // -870 GetDrMd
    (APTR)pcGetOutlinePen, // -876 GetOutlinePen
    (APTR)pcLoadRGB32, // -882 LoadRGB32
    (APTR)pcSetChipRev, // -888 SetChipRev
    (APTR)pcSetABPenDrMd, // -894 SetABPenDrMd
    (APTR)pcGetRGB32, // -900 GetRGB32
    (APTR)pcGfxSpare1, // -906 GfxSpare1
    (APTR)pcSetDefaultMonitor, // -912 SetDefaultMonitor
    (APTR)pcAllocBitMap, // -918 AllocBitMap
    (APTR)pcFreeBitMap, // -924 FreeBitMap
    (APTR)pcGetExtSpriteA, // -930 GetExtSpriteA
    (APTR)pcCoerceMode, // -936 CoerceMode
    (APTR)pcChangeVPBitMap, // -942 ChangeVPBitMap
    (APTR)pcReleasePen, // -948 ReleasePen
    (APTR)pcObtainPen, // -954 ObtainPen
    (APTR)pcGetBitMapAttr, // -960 GetBitMapAttr
    (APTR)pcAllocDBufInfo, // -966 AllocDBufInfo
    (APTR)pcFreeDBufInfo, // -972 FreeDBufInfo
    (APTR)pcSetOutlinePen, // -978 SetOutlinePen
    (APTR)pcSetWriteMask, // -984 SetWriteMask
    (APTR)pcSetMaxPen, // -990 SetMaxPen
    (APTR)pcSetRGB32CM, // -996 SetRGB32CM
    (APTR)pcScrollRasterBF, // -1002 ScrollRasterBF
    (APTR)pcFindColor, // -1008 FindColor
    (APTR)pcGfxSpare2, // -1014 GfxSpare2
    (APTR)pcAllocSpriteDataA, // -1020 AllocSpriteDataA
    (APTR)pcChangeExtSpriteA, // -1026 ChangeExtSpriteA
    (APTR)pcFreeSpriteData, // -1032 FreeSpriteData
    (APTR)pcSetRPAttrsA, // -1038 SetRPAttrsA
    (APTR)pcGetRPAttrsA, // -1044 GetRPAttrsA
    (APTR)pcBestModeIDA, // -1050 BestModeIDA
    (APTR)pcWriteChunkyPixels, // -1056 WriteChunkyPixels
    (APTR)-1
};

BOOL pcGraphicsVectorsComplete(void)
{
    for (unsigned slot = 0; slot < GRAPHICS_VECTOR_COUNT; ++slot) {
        if (!graphicsFunctions[slot] || graphicsFunctions[slot] == (APTR)-1) {
	        return FALSE;
        }
    }
	
    return graphicsFunctions[GRAPHICS_VECTOR_COUNT] == (APTR)-1;
}
