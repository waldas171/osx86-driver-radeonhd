/*
 * Copyright 2007, 2008  Luc Verhaegen <libv@exsuse.de>
 * Copyright 2007, 2008  Matthias Hopf <mhopf@novell.com>
 * Copyright 2007, 2008  Egbert Eich   <eich@novell.com>
 * Copyright 2007, 2008  Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "xf86.h"
#include "xf86DDC.h"

#include "rhd.h"
#include "rhd_connector.h"
#include "rhd_modes.h"
#include "rhd_monitor.h"
#include "rhd_output.h"
#ifdef ATOM_BIOS
# include "rhd_atombios.h"
#endif

/* From rhd_edid.c */
void RHDMonitorEDIDSet(struct rhdMonitor *Monitor, xf86MonPtr EDID);

/*
 *
 */
void
RHDMonitorPrint(struct rhdMonitor *Monitor)
{
    int i;

    LOG("    Bandwidth: %dMHz\n", Monitor->Bandwidth / 1000);
    LOG("    Horizontal timing:\n");
    for (i = 0; i < Monitor->numHSync; i++)
	LOG("        %d - %dkHz\n",  (int)Monitor->HSync[i].lo,
		(int)Monitor->HSync[i].hi);
    LOG("    Vertical timing:\n");
    for (i = 0; i < Monitor->numVRefresh; i++)
	LOG("        %d - %dHz\n",  (int)Monitor->VRefresh[i].lo,
		(int)Monitor->VRefresh[i].hi);
    LOG("    DPI: %dx%d\n", Monitor->xDpi, Monitor->yDpi);
    if (Monitor->ReducedAllowed)
	LOG("    Allows reduced blanking.\n");
    if (Monitor->UseFixedModes)
	LOG("    Uses Fixed Modes.\n");

    if (!Monitor->Modes)
	LOG("    No modes are provided.\n");
    else {
	DisplayModePtr Mode;

	LOG("    Attached modes:\n");
	for (Mode = Monitor->Modes; Mode; Mode = Mode->next) {
	    LOG("        ");
	    RHDPrintModeline(Mode);
	}
    }
}

/*
 * Make sure that we keep only a single mode in our list. This mode should
 * hopefully match our panel at native resolution correctly.
 */
static void
rhdPanelEDIDModesFilter(struct rhdMonitor *Monitor)
{
    DisplayModeRec *Best = Monitor->Modes, *Mode;

    RHDFUNC(Monitor);

    if (!Best || !Best->next)
	return; /* don't bother */

    /* don't go for preferred, just take the biggest */
    for (Mode = Best->next; Mode; Mode = Mode->next) {
		/*
		if (((Best->HDisplay <= Mode->HDisplay) &&
			 (Best->VDisplay < Mode->VDisplay)) ||
			((Best->HDisplay < Mode->HDisplay) &&
			 (Best->VDisplay <= Mode->VDisplay)))
			Best = Mode; */
 		if (((Best->HDisplay == Mode->HDisplay) &&
			 (Best->VDisplay < Mode->VDisplay)) ||
			(Best->HDisplay < Mode->HDisplay))
			Best = Mode;
	}
	
    /* kill all other modes */
	/*
	 DisplayModePtr Temp;
    Mode = Monitor->Modes;
    while (Mode) {
	Temp = Mode->next;

	if (Mode != Best) {
	    LOG("Monitor \"%s\": Discarding Mode \"%s\"\n",
		     Monitor->Name, Mode->name);

	    IODelete(Mode, DisplayModeRec, 1);
	}

	Mode = Temp;
    }

    Best->next = NULL;
    Best->prev = NULL;
 	if (!Monitor->NativeMode) {
		Best->type |= M_T_PREFERRED;
		Monitor->NativeMode = Best;
		LOG("Monitor \"%s\": Using Mode \"%s\""
			" for native resolution.\n", Monitor->Name, Best->name);
	}
	
    Monitor->Modes = Monitor->NativeMode;
	 */
 	if (!Monitor->NativeMode) {
		Best->type |= M_T_PREFERRED;
		Monitor->NativeMode = Best;
		LOG("Monitor \"%s\": Using Mode \"%s\""
			" for native resolution.\n", Monitor->Name, Best->name);
	}
	
    Monitor->numHSync = 1;
    Monitor->HSync[0].lo = Best->HSync;
    Monitor->HSync[0].hi = Best->HSync;
    Monitor->numVRefresh = 1;
    Monitor->VRefresh[0].lo = Best->VRefresh;
    Monitor->VRefresh[0].hi = Best->VRefresh;
    Monitor->Bandwidth = Best->Clock;
}

/*
 *
 */
void
rhdMonitorPrintEDID(struct rhdMonitor *Monitor, xf86MonPtr EDID)
{
    LOGV("EDID data for %s\n", Monitor->Name);
	Uchar * block = (Uchar *) EDID->rawData;
	if (!block) return;
	
	LOGV("%02x: 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n", 0);
	int index = 0;
	int i;
	for (i = 0; i < 8; i++)
	{
		LOGV("%02x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			 i, block[index], block[index+1], block[index+2], block[index+3]
			 , block[index+4], block[index+5], block[index+6], block[index+7]
			 , block[index+8], block[index+9], block[index+10], block[index+11]
			 , block[index+12], block[index+13], block[index+14], block[index+15]);
		index += 16;
	}
}

/*
 * Panels are the most complicated case we need to handle here.
 * Information can come from several places, and we need to make sure
 * that we end up with only the native resolution in our table.
 */
static struct rhdMonitor *
rhdMonitorPanel(struct rhdConnector *Connector)
{
    struct rhdMonitor *Monitor;
    DisplayModeRec *Mode = NULL;
    xf86MonPtr EDID = NULL;

    RHDFUNC(Connector);

    /* has priority over AtomBIOS EDID */
    if (Connector->DDC)
	EDID = xf86DoEDID_DDC2(Connector->scrnIndex, Connector->DDC);

#ifdef ATOM_BIOS
    {
	RHDPtr rhdPtr = RHDPTR(xf86Screens[Connector->scrnIndex]);
	AtomBiosArgRec data;
	AtomBiosResult Result;

	Result = RHDAtomBiosFunc(Connector->scrnIndex, rhdPtr->atomBIOS,
				 ATOM_GET_PANEL_MODE, &data);
	if (Result == ATOM_SUCCESS) {
	    Mode = data.mode;
	    Mode->type |= M_T_PREFERRED;
	}
	if (!EDID) {
	    Result = RHDAtomBiosFunc(Connector->scrnIndex,
				     rhdPtr->atomBIOS,
				     ATOM_GET_PANEL_EDID, &data);
	    if (Result == ATOM_SUCCESS)
		EDID = xf86InterpretEDID(Connector->scrnIndex,
					 data.EDIDBlock);
	}
    }
#endif

    Monitor = IONew(struct rhdMonitor, 1);
	if (!Monitor) {
		if (EDID) IODelete(EDID, xf86MonPtr, 1);
		return NULL;
	}
	bzero(Monitor, sizeof(struct rhdMonitor));
	
    Monitor->scrnIndex = Connector->scrnIndex;
    Monitor->EDID      = EDID;
	
    if (Mode) {
		snprintf(Monitor->Name, MONITOR_NAME_SIZE, "LVDS Panel");
		Monitor->Modes = RHDModesAdd(Monitor->Modes, Mode);
		Monitor->NativeMode = Mode;
		Monitor->numHSync = 1;
		Monitor->HSync[0].lo = Mode->HSync;
		Monitor->HSync[0].hi = Mode->HSync;
		Monitor->numVRefresh = 1;
		Monitor->VRefresh[0].lo = Mode->VRefresh;
		Monitor->VRefresh[0].hi = Mode->VRefresh;
		Monitor->Bandwidth = Mode->SynthClock;
		if (EDID) {
			/* Clueless atombios does give us a mode, but doesn't give us a
			 * DPI or a size. It is just perfect, right? */
			if (EDID->features.hsize)
				Monitor->xDpi = (Mode->HDisplay * 2.54) / ((float) EDID->features.hsize) + 0.5;
			if (EDID->features.vsize)
				Monitor->yDpi = (Mode->VDisplay * 2.54) / ((float) EDID->features.vsize) + 0.5;
		}
    } else if (EDID) {
		RHDMonitorEDIDSet(Monitor, EDID);
		rhdPanelEDIDModesFilter(Monitor);
	} else {
		LOG("%s: No panel mode information found.\n", __func__);
		IODelete(Monitor, struct rhdMonitor, 1);
		return NULL;
	}
	
    /* Fixup some broken modes - if we can do so, otherwise we might have no
     * chance of driving the panel at all */
    if (Monitor->NativeMode) {
		
		/* Some Panels have H or VSyncEnd values greater than H or VTotal. */
		if (Monitor->NativeMode->HTotal <= Monitor->NativeMode->HSyncEnd)
			Monitor->NativeMode->HTotal =  Monitor->NativeMode->CrtcHTotal = Monitor->NativeMode->HSyncEnd + 1;
		if (Monitor->NativeMode->VTotal <= Monitor->NativeMode->VSyncEnd)
			Monitor->NativeMode->VTotal =  Monitor->NativeMode->CrtcVTotal = Monitor->NativeMode->VSyncEnd + 1;
		
		rhdModeFillOutCrtcValues(Monitor->NativeMode);
		if (Monitor->NativeMode->CrtcHBlankEnd <= Monitor->NativeMode->CrtcHSyncEnd)
			Monitor->NativeMode->CrtcHBlankEnd  = Monitor->NativeMode->CrtcHSyncEnd + 1;
		if (Monitor->NativeMode->CrtcVBlankEnd <= Monitor->NativeMode->CrtcVSyncEnd)
			Monitor->NativeMode->CrtcVBlankEnd =  Monitor->NativeMode->CrtcVSyncEnd + 1;
		
		//Let's add some established modes, thus provide user some choices (Dong)
		RHDSynthModes(Monitor->Modes, Monitor->NativeMode);
    }
	
    /* panel should be driven at native resolution only. */
    Monitor->UseFixedModes = TRUE;
    Monitor->ReducedAllowed = TRUE;

    if (EDID)
	rhdMonitorPrintEDID(Monitor, EDID);

    return Monitor;
}

/*
 * rhdMonitorTV(): get TV modes. Currently we can only get this from AtomBIOS.
 */
static struct rhdMonitor *
rhdMonitorTV(struct rhdConnector *Connector)
{
    struct rhdMonitor *Monitor = NULL;
#ifdef ATOM_BIOS
    ScrnInfoPtr pScrn = xf86Screens[Connector->scrnIndex];
    RHDPtr rhdPtr = RHDPTR(pScrn);
    DisplayModeRec *Mode = NULL;
    AtomBiosArgRec arg;

    RHDFUNC(Connector);

    arg.tvMode = rhdPtr->tvMode;
    if (RHDAtomBiosFunc(Connector->scrnIndex, rhdPtr->atomBIOS,
			ATOM_ANALOG_TV_MODE, &arg)
	!= ATOM_SUCCESS)
	return NULL;

    Mode = arg.mode;
    Mode->type |= M_T_PREFERRED;

    Monitor = IONew(struct rhdMonitor, 1);
	if (!Monitor) return NULL;
	bzero(Monitor, sizeof(struct rhdMonitor));

    Monitor->scrnIndex = Connector->scrnIndex;
    Monitor->EDID      = NULL;

	snprintf(Monitor->Name, MONITOR_NAME_SIZE, "TV");
	Monitor->Modes     = RHDModesAdd(Monitor->Modes, Mode);
    Monitor->NativeMode= Mode;
    Monitor->numHSync  = 1;
    Monitor->HSync[0].lo = Mode->HSync;
    Monitor->HSync[0].hi = Mode->HSync;
    Monitor->numVRefresh = 1;
    Monitor->VRefresh[0].lo = Mode->VRefresh;
    Monitor->VRefresh[0].hi = Mode->VRefresh;
    Monitor->Bandwidth = Mode->SynthClock;

    /* TV should be driven at native resolution only. */
    Monitor->UseFixedModes = TRUE;
    Monitor->ReducedAllowed = FALSE;
    /*
     *  hack: the TV encoder takes care of that.
     *  The mode that goes in isn't what comes out.
     */
    Mode->Flags &= ~(V_INTERLACE);
#endif
    return Monitor;
}

static Bool isSameOutputType(char *t1, enum rhdOutputType t2) {
	const struct {
		char type1[outputTypeLength];
		enum rhdOutputType type2;
	} types[] = {
		{"DACA", RHD_OUTPUT_DACA}, 
		{"DACB", RHD_OUTPUT_DACB},
		{"TMDSA", RHD_OUTPUT_TMDSA},
		{"TMDSB", RHD_OUTPUT_LVTMA},
		{"LVDS", RHD_OUTPUT_LVTMA},
		{"LVTMA", RHD_OUTPUT_LVTMA},
		{"DDIA", RHD_OUTPUT_DVO},
		{"UNIPHY_KLDSKP_LVTMA", RHD_OUTPUT_KLDSKP_LVTMA},
		{"UNIPHYA", RHD_OUTPUT_UNIPHYA},
		{"UNIPHYB", RHD_OUTPUT_UNIPHYB},
		{"UNIPHYC", RHD_OUTPUT_UNIPHYC},
		{"UNIPHYD", RHD_OUTPUT_UNIPHYD},
		{"UNIPHYE", RHD_OUTPUT_UNIPHYE},
		{"UNIPHYF", RHD_OUTPUT_UNIPHYF},
		
		{"AtomOutput DACA", RHD_OUTPUT_DACA},
	    {"AtomOutput DACB", RHD_OUTPUT_DACB},
	    {"AtomOutput TMDSA", RHD_OUTPUT_TMDSA},
	    {"AtomOutput LVTMA", RHD_OUTPUT_LVTMA},
	    {"AtomOutput DVO", RHD_OUTPUT_DVO},
	    {"AtomOutput KldskpLvtma", RHD_OUTPUT_KLDSKP_LVTMA},
	    {"AtomOutput UniphyA", RHD_OUTPUT_UNIPHYA},
	    {"AtomOutput UniphyB", RHD_OUTPUT_UNIPHYB},
	    {"AtomOutput UniphyC", RHD_OUTPUT_UNIPHYC},
	    {"AtomOutput UniphyD", RHD_OUTPUT_UNIPHYD},
	    {"AtomOutput UniphyE", RHD_OUTPUT_UNIPHYE},
	    {"AtomOutput UniphyF", RHD_OUTPUT_UNIPHYF}
	};
	
	if (!t1) return FALSE;
	int i;
	for (i = 0;i < 5;i++)
		if ((t2 == types[i].type2) && !strncmp(t1, types[i].type1, strlen(types[i].type1))) break;
	if (i < 5) return TRUE;
	else return FALSE;
}

struct rhdMonitor *
RHDMonitorInit(struct rhdOutput *Output)
{
	ScrnInfoPtr pScrn = xf86Screens[0];
	struct rhdConnector *Connector = Output->Connector;
    struct rhdMonitor *Monitor = NULL;
	int i;
	
    RHDFUNC(Connector);
	
    if (Connector->Type == RHD_CONNECTOR_PANEL)
		Monitor = rhdMonitorPanel(Connector);
    else if (Connector->Type == RHD_CONNECTOR_TV)
		Monitor = rhdMonitorTV(Connector);
    else if (Connector->DDC) {
		xf86MonPtr EDID = xf86DoEDID_DDC2(Connector->scrnIndex, Connector->DDC);
		if (EDID) {
			Monitor = IONew(struct rhdMonitor, 1);
			if (Monitor) {
				bzero(Monitor, sizeof(struct rhdMonitor));
				Monitor->scrnIndex = Connector->scrnIndex;
				Monitor->EDID      = EDID;
				Monitor->NativeMode = NULL;
				
				RHDMonitorEDIDSet(Monitor, EDID);
				rhdMonitorPrintEDID(Monitor, EDID);
				/*
				for (i = 0;i < 2;i++)
					if (isSameOutputType(pScrn->options->outputTypes[i], Output->Id) && pScrn->options->UseFixedModes[i] || TRUE) {
						rhdPanelEDIDModesFilter(Monitor);
						RHDSynthModes(Monitor->Modes, Monitor->NativeMode);
						Monitor->UseFixedModes = TRUE;
						break;
					}
				 */
			}
		}
    }
	
	// user provided EDID as last resort
	if (!Monitor) {
		for (i = 0;i < 2;i++) {
			if (pScrn->options->outputChecked[i]) continue;
			if (!isSameOutputType(pScrn->options->outputTypes[i], Output->Id)) continue;
			if (!pScrn->options->EDID_Block[i] || !pScrn->options->EDID_Length[i]) continue;
			Monitor = IONew(struct rhdMonitor, 1);
			if (!Monitor) continue;
			UInt8 *rawData = (UInt8 *)IOMalloc(pScrn->options->EDID_Length[i]);
			if (!rawData) {
				IODelete(Monitor, struct rhdMonitor, 1);
				Monitor = NULL;
				continue;
			}
			
			bcopy(pScrn->options->EDID_Block[i], rawData, pScrn->options->EDID_Length[i]);
			bzero(Monitor, sizeof(struct rhdMonitor));
			Monitor->scrnIndex = Connector->scrnIndex;
			Monitor->EDID = xf86InterpretEDID(Connector->scrnIndex, rawData);
			Monitor->NativeMode = NULL;
			
			RHDMonitorEDIDSet(Monitor, Monitor->EDID);
			rhdMonitorPrintEDID(Monitor, Monitor->EDID);
			if ((Connector->Type == RHD_CONNECTOR_PANEL) || pScrn->options->UseFixedModes[i]) {
				rhdPanelEDIDModesFilter(Monitor);
				if (Monitor->NativeMode) {
					
					/* Some Panels have H or VSyncEnd values greater than H or VTotal. */
					if (Monitor->NativeMode->HTotal <= Monitor->NativeMode->HSyncEnd)
						Monitor->NativeMode->HTotal =  Monitor->NativeMode->CrtcHTotal = Monitor->NativeMode->HSyncEnd + 1;
					if (Monitor->NativeMode->VTotal <= Monitor->NativeMode->VSyncEnd)
						Monitor->NativeMode->VTotal =  Monitor->NativeMode->CrtcVTotal = Monitor->NativeMode->VSyncEnd + 1;
					
					rhdModeFillOutCrtcValues(Monitor->NativeMode);
					if (Monitor->NativeMode->CrtcHBlankEnd <= Monitor->NativeMode->CrtcHSyncEnd)
						Monitor->NativeMode->CrtcHBlankEnd  = Monitor->NativeMode->CrtcHSyncEnd + 1;
					if (Monitor->NativeMode->CrtcVBlankEnd <= Monitor->NativeMode->CrtcVSyncEnd)
						Monitor->NativeMode->CrtcVBlankEnd =  Monitor->NativeMode->CrtcVSyncEnd + 1;
					
					//Let's add some established modes, thus provide user some choices (Dong)
					RHDSynthModes(Monitor->Modes, Monitor->NativeMode);
				}
				
				/* panel should be driven at native resolution only. */
				Monitor->UseFixedModes = TRUE;
				if (Connector->Type == RHD_CONNECTOR_PANEL) Monitor->ReducedAllowed = TRUE;
			}
			pScrn->options->outputChecked[i] = true;	// already used
		}
	}
	
	// fill in Crtc values
	if (Monitor) {
		DisplayModePtr Mode = Monitor->Modes;
		while (Mode) {
			rhdModeFillOutCrtcValues(Mode);
			Mode = Mode->next;
		}
		// put NativeMode as the first, 2nd condition is important, otherwise will create a infinite loop by the code
		if (Monitor->NativeMode && (Monitor->NativeMode != Monitor->Modes)) {
			Mode = Monitor->NativeMode->prev;
			if (Mode != NULL)
				Mode->next = Monitor->NativeMode->next;
			Mode = Monitor->NativeMode->next;
			if (Mode != NULL)
				Mode->prev = Monitor->NativeMode->prev;
			Monitor->NativeMode->prev = NULL;
			Monitor->NativeMode->next = Monitor->Modes;
			
			Monitor->Modes = Monitor->NativeMode;
		}
	}
	
    return Monitor;
}


/*
 *
 */
void
RHDMonitorDestroy(struct rhdMonitor *Monitor)
{
    DisplayModePtr Mode, Next;

    for (Mode = Monitor->Modes; Mode;) {
	Next = Mode->next;

	IODelete(Mode, DisplayModeRec, 1);

	Mode = Next;
    }

    if (Monitor->EDID)
	IOFree(Monitor->EDID->rawData, EDID1_LEN);
    IODelete(Monitor->EDID, xf86Monitor, 1);
    IODelete(Monitor, struct rhdMonitor, 1);
}
