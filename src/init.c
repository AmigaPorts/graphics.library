/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/library.h"
#include "internal/region.h"
#include "internal/state.h"
#include "internal/native.h"
#include "internal/monitor.h"
#include "internal/font.h"
#include "internal/backend.h"

#define __NOLIBBASE__
#include <proto/exec.h>

struct GfxBase* pcGraphicsOpen(struct GfxBase* base asm("a6"))
{
    // The original Open lazily initializes layers through Exec's Resident API.
    // Its lifecycle vectors do not modify lib_OpenCnt themselves.
    struct ExecBase* SysBase = (APTR)base->ExecBase;
	
    if (!base->gb_LayersBase) {
        const struct Resident* layers = FindResident("layers.library");
        InitResident(layers, 0);
    }
	
    return base;
}

ULONG pcGraphicsClose(void)
{
    return 0;
}

ULONG pcGraphicsExpunge(struct GfxBase* base asm("a6"))
{
    // graphics.library has system lifetime. Expunge releases cached region
    // rectangles, not the library, its fonts or active display.
    pcFlushRegionPool(base);
	
    return 0;
}

ULONG pcGraphicsReserved(void)
{
    return 0;
}

// These three reserved API entries return zero in the original library.
// GfxInternal1 and GfxInternal2 have active behavior and are separate.
ULONG pcGfxInternal3(void)
{
    return 0;
}

ULONG pcGfxSpare1(void)
{
    return 0;
}

ULONG pcGfxSpare2(void)
{
    return 0;
}

struct GfxBase* Init(ULONG segment asm("a0"), struct ExecBase* SysBase asm("a6"))
{
    (void)segment;
    struct GfxBase* base = NULL;
	
    Forbid();
	
    struct Library* existing = (APTR)FindName(&SysBase->LibList, deviceName);
	
    if (existing) {
        // Repeated board initialization may reuse this instance, but must not
        // remove or overwrite another graphics.library, including P96's owner.
        if (existing->lib_IdString == deviceIdString) {
	        base = (APTR)existing;
        }
    	
        goto done;
    }
	
    if (!pcGraphicsVectorsComplete()) {
	    goto done;
    }
	
    base = (APTR)MakeLibrary((APTR)graphicsFunctions, NULL, NULL, sizeof(struct GraphicsLibrary), 0);
	
    if (!base) {
	    goto done;
    }
    
	base->ExecBase = (APTR)SysBase;
    base->LibNode.lib_Node.ln_Type = NT_LIBRARY;
    base->LibNode.lib_Node.ln_Name = (char*)deviceName;
    base->LibNode.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->LibNode.lib_Version = LIBRARY_VERSION;
    base->LibNode.lib_Revision = LIBRARY_REVISION;
    base->LibNode.lib_IdString = (STRPTR)deviceIdString;
    base->UtilBase = OpenLibrary("utility.library", 0);
	
    if (!base->UtilBase || !pcInitGraphicsState(base)) {
	    goto failed;
    }

    // Retain the OS's standard display family. This is logical Amiga display
    // geometry; the boot framebuffer's physical mode is discovered separately.
    base->DisplayFlags = SysBase->VBlankFrequency == 50 ? PAL : NTSC;

    if (!pcSelectGraphicsBackend(base)) {
        goto failed;
    }

    const struct TextFont fonts[2] = {pcTopaz9, pcTopaz8};
    const struct TagItem* const font_tags[2] = {pcTopaz9Tags, NULL};

    if (!pcInitROMFonts(fonts, font_tags, base)) {
        goto failed;
    }

    if (!pcInitStartupDisplayLists(base)) {
        goto failed;
    }

    if (!pcInitDisplayDatabase(base)) {
        goto failed;
    }

    if (!pcInitDefaultMonitor(base)) {
        goto failed;
    }

    // The framebuffer backend does not submit chipset blits and needs no
    // blitter server. Its public QBlit/QBSBlit entries decline those requests.
    if (!base->DefaultFont || !base->default_monitor || !base->DisplayInfoDataBase) {
	    goto failed;
    }

    pcInitRegionPool(base);
	
    if (!pcOpenNativeDisplay(base)) {
        goto failed;
    }

    AddLibrary(&base->LibNode);
    
	goto done;
failed:
    pcClearActiveViewFields(base);
    pcClearMergedViews(pcMergedViews(base), base);
    pcClearPreparedViewPorts(pcPreparedViewPorts(base), base);
    pcFreeDisplayDatabase(base);
    pcFreeDefaultMonitor(base);
    pcFreeROMFonts(base);
    pcFreeStartupDisplayLists(base);
    pcFreeGraphicsState(base);
	
    if (base->UtilBase) {
	    CloseLibrary(base->UtilBase);
    }
	
    ULONG negative = base->LibNode.lib_NegSize;
    FreeMem((UBYTE*)base - negative, negative + base->LibNode.lib_PosSize);
    base = NULL;
done:
    Permit();
	
    return base;
}
