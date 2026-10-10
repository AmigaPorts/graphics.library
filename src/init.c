/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/library.h"
#include "internal/region.h"
#include "internal/state.h"

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
	
    base = (APTR)MakeLibrary((APTR)graphicsFunctions, NULL, NULL, sizeof(*base), 0);
	
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

    // Font installation and native display/database startup are still absent.
    // Completing the vector table alone must not publish an unusable library.
    // Their owners must initialize these before AddLibrary becomes reachable.
    if (!base->DefaultFont || !base->default_monitor || !base->DisplayInfoDataBase) {
	    goto failed;
    }
	
    AddLibrary(&base->LibNode);
    
	goto done;
failed:
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
