/*
 * Copyright 2007-2009  Luc Verhaegen <libv@exsuse.de>
 * Copyright 2007-2009  Matthias Hopf <mhopf@novell.com>
 * Copyright 2007-2009  Egbert Eich   <eich@novell.com>
 * Copyright 2007-2009  Advanced Micro Devices, Inc.
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

#include "rhd.h"
#include "rhd_crtc.h"
#include "rhd_connector.h"
#include "rhd_output.h"
#include "rhd_regs.h"
#include "rhd_hdmi.h"
#ifdef ATOM_BIOS
#include "rhd_atombios.h"
#include "rhd_atomout.h"
#endif

#define FMT2_OFFSET 0x800
#define DIG1_OFFSET 0x000
#define DIG2_OFFSET 0x400

/*
 * Transmitter
 */
struct transmitter {
    enum rhdSensedOutput (*Sense) (struct rhdOutput *Output,
				   enum rhdConnectorType Type);
    ModeStatus (*ModeValid) (struct rhdOutput *Output, DisplayModePtr Mode);
    void (*Mode) (struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode);
    void (*Power) (struct rhdOutput *Output, int Power);
    void (*Save) (struct rhdOutput *Output);
    void (*Restore) (struct rhdOutput *Output);
    void (*Destroy) (struct rhdOutput *Output);
    Bool (*Property) (struct rhdOutput *Output,
		      enum rhdPropertyAction Action, enum rhdOutputProperty Property, union rhdPropertyData *val);
    Bool (*WrappedPropertyCallback) (struct rhdOutput *Output,
		      enum rhdPropertyAction Action, enum rhdOutputProperty Property, union rhdPropertyData *val);
    void *PropertyPrivate;
    void *Private;
};

/*
 * Encoder
 */
struct encoder {
    ModeStatus (*ModeValid) (struct rhdOutput *Output, DisplayModePtr Mode);
    void (*Mode) (struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode);
    void (*Power) (struct rhdOutput *Output, int Power);
    void (*Save) (struct rhdOutput *Output);
    void (*Restore) (struct rhdOutput *Output);
    void (*Destroy) (struct rhdOutput *Output);
    void *Private;
};

/*
 *
 */
enum encoderMode {
    DISPLAYPORT = 0,
    LVDS = 1,
    TMDS_DVI = 2,
    TMDS_HDMI = 3,
    SDVO = 4
};

enum encoderID {
    ENCODER_NONE,
    ENCODER_DIG1,
    ENCODER_DIG2
};

struct DIGPrivate
{
    struct encoder Encoder;
    struct transmitter Transmitter;
    enum encoderID EncoderID;
    enum encoderMode EncoderMode;
    Bool Coherent;
    Bool RunDualLink;
    DisplayModePtr Mode;
    struct rhdHdmi *Hdmi;

    /* LVDS */
    Bool FPDI;
    CARD32 PowerSequenceDe2Bl;
    CARD32 PowerSequenceDig2De;
    CARD32 OffDelay;
    struct rhdFMTDither FMTDither;
    int BlLevel;
};

/*
 * LVTMA Transmitter
 */

struct LVTMATransmitterPrivate
{
    Bool Stored;

    CARD32 StoredTransmitterControl;
    CARD32 StoredTransmitterAdjust;
    CARD32 StoredPreemphasisControl;
    CARD32 StoredMacroControl;
    CARD32 StoredLVTMADataSynchronization;
    CARD32 StoredTransmiterEnable;
    CARD32 StoredPwrSeqCntl;
    CARD32 StoredPwrSeqRevDiv;
    CARD32 StoredPwrSeqDelay1;
    CARD32 StoredPwrSeqDelay2;
};

/*
 *
 */
static ModeStatus
LVTMATransmitterModeValid(struct rhdOutput *Output, DisplayModePtr Mode)
{
    RHDFUNC(Output);

    if (Output->Connector->Type == RHD_CONNECTOR_DVI_SINGLE
	&& Mode->SynthClock > 165000)
	return MODE_CLOCK_HIGH;

    return MODE_OK;
}

static void
LVDSSetBacklight(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *) Output->Private;
    int level = Private->BlLevel;

    RHDFUNC(Output);

	if (level < 1) return;
	LOG("%s: trying to set BL_MOD_LEVEL to: %d\n", __func__, level);
	RHDRegMask(Output, RV620_LVTMA_PWRSEQ_REF_DIV,
			   0x144 << LVTMA_BL_MOD_REF_DI_SHIFT,
			   0x7ff << LVTMA_BL_MOD_REF_DI_SHIFT);
	RHDRegWrite(Output, RV620_LVTMA_BL_MOD_CNTL,
				0xff << LVTMA_BL_MOD_RES_SHIFT
				| level << LVTMA_BL_MOD_LEVEL_SHIFT
				| LVTMA_BL_MOD_EN);
}

/*
 *
 */
static Bool
LVDSTransmitterPropertyControl(struct rhdOutput *Output,
	     enum rhdPropertyAction Action, enum rhdOutputProperty Property, union rhdPropertyData *val)
{
    struct DIGPrivate *Private = (struct DIGPrivate *) Output->Private;

    RHDFUNC(Output);
    switch (Action) {
	case rhdPropertyCheck:
	switch (Property) {
	    case RHD_OUTPUT_BACKLIGHT:
		if (Private->BlLevel < 0)
		    return FALSE;
		return TRUE;
	    default:
		return FALSE;
	}
	case rhdPropertyGet:
	    switch (Property) {
		case RHD_OUTPUT_BACKLIGHT:
		    if (Private->BlLevel < 0)
			return FALSE;
		    val->integer = Private->BlLevel;
		    return TRUE;
		default:
		    return FALSE;
	    }
	    break;
	case rhdPropertySet:
	    switch (Property) {
		case RHD_OUTPUT_BACKLIGHT:
		    if (Private->BlLevel < 0)
			return FALSE;
		    Private->BlLevel = val->integer;
		    return TRUE;
		default:
		    return FALSE;
	    }
	    break;
	case rhdPropertyCommit:
	    switch (Property) {
		case RHD_OUTPUT_BACKLIGHT:
		    LVDSSetBacklight(Output);
		    return TRUE;
		default:
		    return FALSE;
	    }
	    break;
    }
    return TRUE;
}

/*
 *
 */
static Bool
TMDSTransmitterPropertyControl(struct rhdOutput *Output,
	     enum rhdPropertyAction Action, enum rhdOutputProperty Property, union rhdPropertyData *val)
{
    struct DIGPrivate *Private = (struct DIGPrivate *) Output->Private;

    RHDFUNC(Output);
    switch (Action) {
	case rhdPropertyCheck:
	    switch (Property) {
		case RHD_OUTPUT_COHERENT:
		case RHD_OUTPUT_HDMI:
		    return TRUE;
	        default:
		    return FALSE;
	    }
	case rhdPropertyGet:
	    switch (Property) {
		case RHD_OUTPUT_COHERENT:
		    val->aBool =  Private->Coherent;
		    return TRUE;
		case RHD_OUTPUT_HDMI:
		    val->aBool = Private->EncoderMode == TMDS_HDMI;
		    return TRUE;
		default:
		    return FALSE;
	    }
	    break;
	case rhdPropertySet:
	    switch (Property) {
		case RHD_OUTPUT_COHERENT:
		    Private->Coherent = val->aBool;
		    break;
		case RHD_OUTPUT_HDMI:
		    Private->EncoderMode = val->aBool ? TMDS_HDMI : TMDS_DVI;
		    break;
		default:
		    return FALSE;
	    }
	    break;
	case rhdPropertyCommit:
	    switch (Property) {
		case RHD_OUTPUT_COHERENT:
		case RHD_OUTPUT_HDMI:
		    Output->Mode(Output, Private->Mode);
		    Output->Power(Output, RHD_POWER_ON);
		    break;
		default:
		    return FALSE;
	    }
	    break;
    }
    return TRUE;
}

/*
 *
 */
static void
LVTMATransmitterSet(struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
#ifdef ATOM_BIOS
    CARD32 value = 0;
    AtomBiosArgRec data;
    RHDPtr rhdPtr = RHDPTRI(Output);
#endif
    Bool doCoherent = Private->Coherent;
    RHDFUNC(Output);

    /* set coherent / not coherent mode; whatever that is */
    if (Output->Connector->Type != RHD_CONNECTOR_PANEL)
	RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		   doCoherent ? 0 : RV62_LVTMA_BYPASS_PLL, RV62_LVTMA_BYPASS_PLL);

#ifdef ATOM_BIOS
    LOG("%s: SynthClock: %d Hex: %x EncoderMode: %x\n",__func__,
	     (Mode->SynthClock),(Mode->SynthClock / 10), Private->EncoderMode);

    /* Set up magic value that's used for list lookup */
    value = ((Mode->SynthClock / 10 / ((Private->RunDualLink) ? 2 : 1)) & 0xffff)
	| (Private->EncoderMode << 16)
	| ((doCoherent ? 0x2 : 0) << 24);

    LOG("%s: GetConditionalGoldenSettings for: %x\n", __func__, value);

    /* Get data from DIG2TransmitterControl table */
    data.val = 0x4d;
    if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS, ATOM_GET_CODE_DATA_TABLE,
			&data) == ATOM_SUCCESS) {
	AtomBiosArgRec data1;
	CARD32 *d_p = NULL;

	data1.GoldenSettings.BIOSPtr = data.CommandDataTable.loc;
	data1.GoldenSettings.End = data1.GoldenSettings.BIOSPtr + data.CommandDataTable.size;
	data1.GoldenSettings.value = value;

	/* now find pointer */
	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_GET_CONDITIONAL_GOLDEN_SETTINGS, &data1) == ATOM_SUCCESS) {
	    d_p = (CARD32*)data1.GoldenSettings.BIOSPtr;
	} else {
	    /* nothing found, now try toggling the coherent setting */
	    doCoherent = !doCoherent;
	    value = (value & ~(0x2 << 24)) | ((doCoherent ? 0x2 : 0) << 24);
	    data1.GoldenSettings.value = value;

	    if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_GET_CONDITIONAL_GOLDEN_SETTINGS, &data1) == ATOM_SUCCESS) {
		d_p = (CARD32*)data1.GoldenSettings.BIOSPtr;
		/* set coherent / not coherent mode; whatever that is */
		LOG("%s: %soherent Mode not supported, switching to %soherent.\n",
			   __func__, doCoherent ? "Inc" : "C", doCoherent ? "C" : "Inc");
		if (Output->Connector->Type != RHD_CONNECTOR_PANEL)
		    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
			       doCoherent ? 0 : RV62_LVTMA_BYPASS_PLL, RV62_LVTMA_BYPASS_PLL);
	    } else
		doCoherent = Private->Coherent; /* reset old value if nothing found either */
	}
	if (d_p) {
	    LOG("TransmitterAdjust: 0x%8.8x\n",d_p[0]);
	    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_ADJUST, d_p[0]);

	    LOG("PreemphasisControl: 0x%8.8x\n",d_p[1]);
	    RHDRegWrite(Output, RV620_LVTMA_PREEMPHASIS_CONTROL, d_p[1]);

	    LOG("MacroControl: 0x%8.8x\n",d_p[2]);
	    RHDRegWrite(Output, RV620_LVTMA_MACRO_CONTROL, d_p[2]);
	} else
	    LOG("%s: cannot get golden settings\n",__func__);
    } else
#endif
    {
	LOG("%s: No AtomBIOS supplied "
		   "electrical parameters available\n", __func__);
    }
}

/*
 *
 */
static void
LVTMATransmitterSave(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    Private->StoredTransmitterControl       = RHDRegRead(Output, RV620_LVTMA_TRANSMITTER_CONTROL);
    Private->StoredTransmitterAdjust        = RHDRegRead(Output, RV620_LVTMA_TRANSMITTER_ADJUST);
    Private->StoredPreemphasisControl       = RHDRegRead(Output, RV620_LVTMA_PREEMPHASIS_CONTROL);
    Private->StoredMacroControl             = RHDRegRead(Output, RV620_LVTMA_MACRO_CONTROL);
    Private->StoredLVTMADataSynchronization = RHDRegRead(Output, RV620_LVTMA_DATA_SYNCHRONIZATION);
    Private->StoredTransmiterEnable         = RHDRegRead(Output, RV620_LVTMA_TRANSMITTER_ENABLE);
}

/*
 *
 */
static void
LVTMATransmitterRestore(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    RHDFUNC(Output);

    /* write control values back */
    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_CONTROL,Private->StoredTransmitterControl);
    IODelay (14);
    /* reset PLL */
    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_CONTROL,Private->StoredTransmitterControl
		| RV62_LVTMA_PLL_RESET);
    IODelay (10);
    /* unreset PLL */
    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_CONTROL,Private->StoredTransmitterControl);
    IODelay(1000);
    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_ADJUST, Private->StoredTransmitterAdjust);
    RHDRegWrite(Output, RV620_LVTMA_PREEMPHASIS_CONTROL, Private->StoredPreemphasisControl);
    RHDRegWrite(Output, RV620_LVTMA_MACRO_CONTROL, Private->StoredMacroControl);
    /* start data synchronization */
    RHDRegWrite(Output, RV620_LVTMA_DATA_SYNCHRONIZATION, (Private->StoredLVTMADataSynchronization
							   & ~(CARD32)RV62_LVTMA_DSYNSEL)
		| RV62_LVTMA_PFREQCHG);
    IODelay (1);
    RHDRegWrite(Output, RV620_LVTMA_DATA_SYNCHRONIZATION, Private->StoredLVTMADataSynchronization);
    IODelay(10);
    RHDRegWrite(Output, RV620_LVTMA_DATA_SYNCHRONIZATION, Private->StoredLVTMADataSynchronization);
    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_ENABLE, Private->StoredTransmiterEnable);
}

/*
 *
 */
static void
LVTMA_TMDSTransmitterSet(struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode)
{
    RHDFUNC(Output);

    /* TMDS Mode */
    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
	       RV62_LVTMA_USE_CLK_DATA, RV62_LVTMA_USE_CLK_DATA);

    LVTMATransmitterSet(Output, Crtc, Mode);

    /* use differential post divider input */
    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
	       RV62_LVTMA_IDSCKSEL, RV62_LVTMA_IDSCKSEL);
}

/*
 *
 */
static void
LVTMA_TMDSTransmitterPower(struct rhdOutput *Output, int Power)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;

    RHDFUNC(Output);

    switch (Power) {
	case RHD_POWER_ON:
	    /* enable PLL */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_PLL_ENABLE, RV62_LVTMA_PLL_ENABLE);
	    IODelay(14);
	    /* PLL reset on */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_PLL_RESET, RV62_LVTMA_PLL_RESET);
	    IODelay(10);
	    /* PLL reset off */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       0, RV62_LVTMA_PLL_RESET);
	    IODelay(1000);
	    /* start data synchronization */
	    RHDRegMask(Output, RV620_LVTMA_DATA_SYNCHRONIZATION,
		       RV62_LVTMA_PFREQCHG, RV62_LVTMA_PFREQCHG);
	    IODelay(1);
	    /* restart write address logic */
	    RHDRegMask(Output, RV620_LVTMA_DATA_SYNCHRONIZATION,
		       RV62_LVTMA_DSYNSEL, RV62_LVTMA_DSYNSEL);
#if 1
	    /* TMDS Mode ?? */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_MODE, RV62_LVTMA_MODE);
#endif
	    /* enable lower link */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE,
		       RV62_LVTMA_LNKL,
		       RV62_LVTMA_LNK_ALL);
	    if (Private->RunDualLink) {
		IODelay (28);
		/* enable upper link */
		RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE,
			   RV62_LVTMA_LNKU,
			   RV62_LVTMA_LNKU);
	    }
	    return;
	case RHD_POWER_RESET:
	    /* disable all links */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE,
		       0, RV62_LVTMA_LNK_ALL);
	    return;
	case RHD_POWER_SHUTDOWN:
	default:
	    /* disable transmitter */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE,
		       0, RV62_LVTMA_LNK_ALL);
	    /* PLL reset */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_PLL_RESET, RV62_LVTMA_PLL_RESET);
	    IODelay(10);
	    /* end PLL reset */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       0, RV62_LVTMA_PLL_RESET);
	    /* disable data synchronization */
	    RHDRegMask(Output, RV620_LVTMA_DATA_SYNCHRONIZATION,
		       0, RV62_LVTMA_DSYNSEL);
	    /* reset macro control */
	    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_ADJUST, 0);

	    return;
    }
}

/*
 *
 */
static void
LVTMA_TMDSTransmitterSave(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    RHDFUNC(Output);

    LVTMATransmitterSave(Output);

    Private->Stored = TRUE;
}

/*
 *
 */
static void
LVTMA_TMDSTransmitterRestore(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    RHDFUNC(Output);

    if (!Private->Stored) {
	LOG("%s: No registers stored.\n", __func__);
	return;
    }

    LVTMATransmitterRestore(Output);
}

/*
 *
 */
static void
LVTMA_LVDSTransmitterSet(struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode)
{
    RHDFUNC(Output);

    /* LVDS Mode */
    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
	       0, RV62_LVTMA_USE_CLK_DATA);

    LVTMATransmitterSet(Output, Crtc, Mode);

    /* use IDCLK */
    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL, RV62_LVTMA_IDSCKSEL, RV62_LVTMA_IDSCKSEL);
    /* enable pwrseq, pwrseq overwrite PPL enable, reset */
    RHDRegMask(Output,  RV620_LVTMA_PWRSEQ_CNTL,
	       RV62_LVTMA_PWRSEQ_EN
	       | RV62_LVTMA_PLL_ENABLE_PWRSEQ_MASK
	       | RV62_LVTMA_PLL_RESET_PWRSEQ_MASK,
	       RV62_LVTMA_PWRSEQ_EN
	       | RV62_LVTMA_PLL_ENABLE_PWRSEQ_MASK
	       | RV62_LVTMA_PLL_RESET_PWRSEQ_MASK
	);

}

/*
 *
 */
static void
LVTMA_LVDSTransmitterPower(struct rhdOutput *Output, int Power)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    CARD32 tmp, tmp1;
    int i;

    RHDFUNC(Output);

    switch (Power) {
	case RHD_POWER_ON:
	    /* enable PLL */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_PLL_ENABLE, RV62_LVTMA_PLL_ENABLE);
	    IODelay(14);
	    /* PLL reset on */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       RV62_LVTMA_PLL_RESET, RV62_LVTMA_PLL_RESET);
	    IODelay(10);
	    /* PLL reset off */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       0, RV62_LVTMA_PLL_RESET);
	    IODelay(1000);
	    /* start data synchronization */
	    RHDRegMask(Output, RV620_LVTMA_DATA_SYNCHRONIZATION,
		       RV62_LVTMA_PFREQCHG, RV62_LVTMA_PFREQCHG);
	    IODelay(1);
	    /* restart write address logic */
	    RHDRegMask(Output, RV620_LVTMA_DATA_SYNCHRONIZATION,
		       RV62_LVTMA_DSYNSEL, RV62_LVTMA_DSYNSEL);
	    /* SYNCEN disables pwrseq ?? */
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_CNTL,
		       RV62_LVTMA_PWRSEQ_DISABLE_SYNCEN_CONTROL_OF_TX_EN,
		       RV62_LVTMA_PWRSEQ_DISABLE_SYNCEN_CONTROL_OF_TX_EN);
	    /* LVDS Mode ?? */
	    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_CONTROL,
		       0, RV62_LVTMA_MODE);
	    /* enable links */
	    if (Private->RunDualLink) {
		if (Private->FMTDither.LVDS24Bit)
		    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE, 0x3ff, 0x3ff);
		else
		    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE, 0x1ef, 0x3ff);
		    } else {
		if (Private->FMTDither.LVDS24Bit)
		    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE, 0x1f, 0x3ff);
		else
		    RHDRegMask(Output, RV620_LVTMA_TRANSMITTER_ENABLE, 0x0f, 0x3ff);
	    }
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_CNTL, 0,
		       RV62_LVTMA_DIGON_OVRD | RV62_LVTMA_BLON_OVRD);
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_REF_DIV, 3999, 0xffff); /* 4000 - 1 */
	    tmp = Private->PowerSequenceDe2Bl * 10 / 4;
	    tmp1 = Private->PowerSequenceDig2De * 10 / 4;
	    /* power sequencing delay for on / off between DIGON and SYNCEN, and SYNCEN and BLON */
	    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_DELAY1, (tmp1 << 24) | tmp1 | (tmp << 8) | (tmp << 16));
	    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_DELAY2, Private->OffDelay / 4);
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_CNTL, 0, RV62_LVTMA_PWRSEQ_DISABLE_SYNCEN_CONTROL_OF_TX_EN);
	    for (i = 0; i < 500; i++) {
		CARD32 tmp;

		IODelay(1000);
		tmp = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_STATE);
		tmp >>= RV62_LVTMA_PWRSEQ_STATE_SHIFT;
		tmp &= 0xff;
		if (tmp <= RV62_POWERUP_DONE)
		    break;
		if (tmp >= RV62_POWERDOWN_DONE)
		    break;
	    }
	    /* LCD on */
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_CNTL, RV62_LVTMA_PWRSEQ_TARGET_STATE,
		       RV62_LVTMA_PWRSEQ_TARGET_STATE);
	    return;

	case RHD_POWER_RESET:
	    /* Disable LCD and BL */
	    RHDRegMask(Output, RV620_LVTMA_PWRSEQ_CNTL, 0,
		       RV62_LVTMA_PWRSEQ_TARGET_STATE
		       | RV62_LVTMA_DIGON_OVRD
		       | RV62_LVTMA_BLON_OVRD);
	    for (i = 0; i < 500; i++) {
		CARD32 tmp;

		IODelay(1000);
		tmp = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_STATE);
		tmp >>= RV62_LVTMA_PWRSEQ_STATE_SHIFT;
		tmp &= 0xff;
		if (tmp >= RV62_POWERDOWN_DONE)
		    break;
	    }
	    return;
	case RHD_POWER_SHUTDOWN:
	    LVTMA_LVDSTransmitterPower(Output, RHD_POWER_RESET);
	    /* op-amp down, bias current for output driver down, shunt resistor down */
	    RHDRegWrite(Output, RV620_LVTMA_TRANSMITTER_ADJUST, 0x00e00000);
	    /* set macro control */
	    RHDRegWrite(Output, RV620_LVTMA_MACRO_CONTROL, 0x07430408);
	default:
	    return;
    }
}

/*
 *
 */
static void
LVTMA_LVDSTransmitterSave(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    RHDFUNC(Output);

    LVTMATransmitterSave(Output);

    Private->StoredPwrSeqCntl               = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_CNTL);
    Private->StoredPwrSeqRevDiv             = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_REF_DIV);
    Private->StoredPwrSeqDelay1             = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_DELAY1);
    Private->StoredPwrSeqDelay2             = RHDRegRead(Output, RV620_LVTMA_PWRSEQ_DELAY2);

    Private->Stored = TRUE;
}

/*
 *
 */
static void
LVTMA_LVDSTransmitterRestore(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct LVTMATransmitterPrivate *Private = (struct LVTMATransmitterPrivate*)digPrivate->Transmitter.Private;

    RHDFUNC(Output);

    if (!Private->Stored) {
	LOG("%s: No registers stored.\n", __func__);
	return;
    }

    LVTMATransmitterRestore(Output);

    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_REF_DIV, Private->StoredPwrSeqRevDiv);
    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_DELAY1, Private->StoredPwrSeqDelay1);
    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_DELAY2, Private->StoredPwrSeqDelay2);
    RHDRegWrite(Output, RV620_LVTMA_PWRSEQ_CNTL, Private->StoredPwrSeqCntl);
}

/*
 *
 */
static void
LVTMATransmitterDestroy(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;

    RHDFUNC(Output);

    if (!digPrivate)
	return;

    IODelete(digPrivate->Transmitter.Private, struct LVTMATransmitterPrivate, 1);
}

/*
 *
 */
void
rhdPrintDigDebug(RHDPtr rhdPtr, const char *name)
{
    LOG("%s: DIGn_CNTL: n=1: 0x%x n=2: 0x%x\n",
	   name, (unsigned int) RHDRegRead(rhdPtr, RV620_DIG1_CNTL),
	   (unsigned int) RHDRegRead(rhdPtr, DIG2_OFFSET + RV620_DIG1_CNTL));
}

/*
 *
 */
static CARD32
digProbeEncoder(struct rhdOutput *Output)
{
    if (Output->Id == RHD_OUTPUT_KLDSKP_LVTMA) {
	return ENCODER_DIG2;
    } else {
	Bool swap = (RHDRegRead(Output, RV620_DCIO_LINK_STEER_CNTL)
		     & RV62_LINK_STEER_SWAP) == RV62_LINK_STEER_SWAP;

	switch (Output->Id) {
	    case RHD_OUTPUT_UNIPHYA:
		if (swap) {
		    LOG("%s: detected ENCODER_DIG2 for UNIPHYA\n",__func__);
		    return ENCODER_DIG2;
		} else {
		    LOG("%s: detected ENCODER_DIG1 for UNIPHYA\n",__func__);
		    return ENCODER_DIG1;
		}
		break;
	    case RHD_OUTPUT_UNIPHYB:
		if (swap) {
		    LOG("%s: detected ENCODER_DIG1 for UNIPHYB\n",__func__);
		    return ENCODER_DIG1;
		} else {
		    LOG("%s: detected ENCODER_DIG2 for UNIPHYB\n",__func__);
		    return ENCODER_DIG2;
		}
		break;
	    default:
		return ENCODER_NONE; /* should not get here */
	}
    }
    return ENCODER_NONE;
}

#if defined(ATOM_BIOS) && defined(ATOM_BIOS_PARSER)

struct ATOMTransmitterPrivate
{
    struct atomTransmitterConfig atomTransmitterConfig;
    enum atomTransmitter atomTransmitterID;
};

/*
 *
 */
static ModeStatus
ATOMTransmitterModeValid(struct rhdOutput *Output, DisplayModePtr Mode)
{

    RHDFUNC(Output);

    if (Output->Connector->Type == RHD_CONNECTOR_DVI_SINGLE
	&& Mode->SynthClock > 165000)
	return MODE_CLOCK_HIGH;

    return MODE_OK;
}

/*
 *
 */
static void
ATOMTransmitterSet(struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode)
{
    RHDPtr rhdPtr = RHDPTRI(Output);
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct ATOMTransmitterPrivate *transPrivate
	= (struct ATOMTransmitterPrivate*) Private->Transmitter.Private;
    struct atomTransmitterConfig *atc = &transPrivate->atomTransmitterConfig;

    RHDFUNC(Output);

    atc->Coherent = Private->Coherent;
    atc->PixelClock = Mode->SynthClock;

    rhdPrintDigDebug(rhdPtr,__func__);

    if (Private->RunDualLink) {
	atc->Mode = atomDualLink;

	if (atc->Link == atomTransLinkA)
	    atc->Link = atomTransLinkAB;
	else if (atc->Link == atomTransLinkB)
	    atc->Link = atomTransLinkBA;

    } else {
	atc->Mode = atomSingleLink;

	if (atc->Link == atomTransLinkAB)
	    atc->Link = atomTransLinkA;
	else if (atc->Link == atomTransLinkBA)
	    atc->Link = atomTransLinkB;

    }

    atc->PixelClock = Mode->SynthClock;

    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
				 atomTransSetup, atc);
    rhdPrintDigDebug(rhdPtr,__func__);
}

/*
 *
 */
static void
ATOMTransmitterPower(struct rhdOutput *Output, int Power)
{
    RHDPtr rhdPtr = RHDPTRI(Output);
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct ATOMTransmitterPrivate *transPrivate
	= (struct ATOMTransmitterPrivate*) Private->Transmitter.Private;
    struct atomTransmitterConfig *atc = &transPrivate->atomTransmitterConfig;

    RHDFUNC(Output);

    rhdPrintDigDebug(rhdPtr,__func__);

    if (Private->RunDualLink)
	atc->LinkCnt = atomDualLink;
    else
	atc->LinkCnt = atomSingleLink;

    atc->Coherent = Private->Coherent;

    if (atc->Encoder == atomEncoderNone) {
	switch (digProbeEncoder(Output)) {
	    case ENCODER_DIG1:
		if (rhdPtr->DigEncoderOutput[0]) {
		    LOG("%s: DIG1 for %s already taken\n",__func__,Output->Name);
		    return;
		}
		atc->Encoder = atomEncoderDIG1;
		break;
	    case ENCODER_DIG2:
		if (rhdPtr->DigEncoderOutput[1]) {
		    LOG("%s: DIG2 for %s already taken\n",__func__,Output->Name);
		    return;
		}
		atc->Encoder = atomEncoderDIG2;
		break;
	    default:
		return;
	}
    }

    switch (Power) {
	case RHD_POWER_ON:
	    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
					 atomTransEnable, atc);
	    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
					 atomTransEnableOutput, atc);
	    break;
	case RHD_POWER_RESET:
	    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
					 atomTransDisableOutput, atc);
	    break;
	case RHD_POWER_SHUTDOWN:
	    if (!Output->Connector || Output->Connector->Type == RHD_CONNECTOR_DVI)
		atc->Mode = atomDVI;

	    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
					 atomTransDisableOutput, atc);
	    rhdAtomDigTransmitterControl(rhdPtr->atomBIOS, transPrivate->atomTransmitterID,
					 atomTransDisable, atc);
	    break;
    }
    rhdPrintDigDebug(rhdPtr,__func__);
}

/*
 *
 */
static void
ATOMTransmitterSave(struct rhdOutput *Output)
{
    RHDFUNC(Output);
}

/*
 *
 */
static void
ATOMTransmitterRestore(struct rhdOutput *Output)
{
    RHDFUNC(Output);
}

/*
 *
 */
static void
ATOMTransmitterDestroy(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;

    RHDFUNC(Output);

    if (!digPrivate)
	return;

    IODelete(digPrivate->Transmitter.Private, struct ATOMTransmitterPrivate, 1);
}

#endif /* ATOM_BIOS && ATOM_BIOS_PASER */

/*
 *  Encoder
 */

struct DIGEncoder
{
    Bool Stored;

    CARD32 StoredOff;

    CARD32 StoredRegExt1DiffPostDivCntl;
    CARD32 StoredRegExt2DiffPostDivCntl;
    CARD32 StoredDIGClockPattern;
    CARD32 StoredLVDSDataCntl;
    CARD32 StoredTMDSPixelEncoding;
    CARD32 StoredTMDSCntl;
    CARD32 StoredDIGCntl;
    CARD32 StoredDIGMisc1;
    CARD32 StoredDIGMisc2;
    CARD32 StoredDIGMisc3;
    CARD32 StoredDCCGPclkDigCntl;
    CARD32 StoredDCCGSymclkCntl;
    CARD32 StoredDCIOLinkSteerCntl;
    CARD32 StoredBlModCntl;
};

/*
 *
 */
static ModeStatus
EncoderModeValid(struct rhdOutput *Output, DisplayModePtr Mode)
{
    RHDFUNC(Output);

    return MODE_OK;
}

/*
 *
 */
static void
LVDSEncoder(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    CARD32 off;

    RHDFUNC(Output);

    //ASSERT(Private->EncoderID != ENCODER_NONE);

    off = (Private->EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;
    /* Clock pattern ? */
    RHDRegMask(Output, off + RV620_DIG1_CLOCK_PATTERN, 0x0063, 0xFFFF);
    /* set panel type: 18/24 bit mode */
    RHDRegMask(Output, off + RV620_LVDS1_DATA_CNTL,
	       (Private->FMTDither.LVDS24Bit ? RV62_LVDS_24BIT_ENABLE : 0)
	       | (Private->FPDI ? RV62_LVDS_24BIT_FORMAT : 0),
	       RV62_LVDS_24BIT_ENABLE | RV62_LVDS_24BIT_FORMAT);

    Output->Crtc->FMTModeSet(Output->Crtc, &Private->FMTDither);
}

/*
 *
 */
static void
TMDSEncoder(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    CARD32 off;

    RHDFUNC(Output);

    //ASSERT(Private->EncoderID != ENCODER_NONE);
    off = (Private->EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;
    /* clock pattern ? */
    RHDRegMask(Output, off + RV620_DIG1_CLOCK_PATTERN, 0x001F, 0xFFFF);
    /* color format RGB - normal color format 24bpp, Twin-Single 30bpp or Dual 48bpp*/
    RHDRegMask(Output, off + RV620_TMDS1_CNTL, 0x0,
	       RV62_TMDS_PIXEL_ENCODING | RV62_TMDS_COLOR_FORMAT);
    /* no dithering */
    Output->Crtc->FMTModeSet(Output->Crtc, NULL);
}

/*
 *
 */
static void
EncoderSet(struct rhdOutput *Output, struct rhdCrtc *Crtc, DisplayModePtr Mode)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    RHDPtr rhdPtr = RHDPTRI(Output);
    CARD32 off;

    RHDFUNC(Output);

    //ASSERT(Private->EncoderID != ENCODER_NONE);
    off = (Private->EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;

    rhdPrintDigDebug(rhdPtr,__func__);

    RHDRegMask(Output, off + RV620_DIG1_CNTL, Output->Crtc->Id,
	       RV62_DIG_SOURCE_SELECT);

    if (Output->Id == RHD_OUTPUT_UNIPHYA) {
	/* select LinkA ?? */
	RHDRegMask(Output, RV620_DCIO_LINK_STEER_CNTL,
		   ((Private->EncoderID == ENCODER_DIG2)
		    ? RV62_LINK_STEER_SWAP
		    : 0), RV62_LINK_STEER_SWAP); /* swap if DIG2 */
	if (!Private->RunDualLink) {
	    RHDRegMask(Output, off + RV620_DIG1_CNTL,
		       0,
		       RV62_DIG_SWAP |RV62_DIG_DUAL_LINK_ENABLE);
	} else {
	    RHDRegMask(Output, off + RV620_DIG1_CNTL,
		       RV62_DIG_DUAL_LINK_ENABLE,
		       RV62_DIG_SWAP | RV62_DIG_DUAL_LINK_ENABLE);
	}
    } else if (Output->Id == RHD_OUTPUT_UNIPHYB) {
	/* select LinkB ?? */
	RHDRegMask(Output, RV620_DCIO_LINK_STEER_CNTL,
		   ((Private->EncoderID == ENCODER_DIG2)
		    ? 0
		    : RV62_LINK_STEER_SWAP), RV62_LINK_STEER_SWAP);
	if (!Private->RunDualLink)
	    RHDRegMask(Output, off + RV620_DIG1_CNTL,
		       0,
		       RV62_DIG_SWAP |  RV62_DIG_DUAL_LINK_ENABLE);
	 else
	    RHDRegMask(Output, off + RV620_DIG1_CNTL,
		       RV62_DIG_SWAP | RV62_DIG_DUAL_LINK_ENABLE,
		       RV62_DIG_SWAP | RV62_DIG_DUAL_LINK_ENABLE);
    } else { /* LVTMA */
	RHDRegMask(Output, RV620_EXT2_DIFF_POST_DIV_CNTL, 0, RV62_EXT2_DIFF_DRIVER_ENABLE);
    }

    if (Private->EncoderMode == LVDS)
	LVDSEncoder(Output);
    else if (Private->EncoderMode == DISPLAYPORT)
	LOG("%s: No displayport support yet!\n", __func__);  /* bugger ! */
    else
	TMDSEncoder(Output);

    /* Start DIG, set links, disable stereo sync, select FMT source */
    RHDRegMask(Output, off + RV620_DIG1_CNTL,
	       (Private->EncoderMode & 0x7) << 8
	       | RV62_DIG_START
	       | (Private->RunDualLink ? RV62_DIG_DUAL_LINK_ENABLE : 0)
	       | Output->Crtc->Id,
	       RV62_DIG_MODE
	       | RV62_DIG_START
	       | RV62_DIG_DUAL_LINK_ENABLE
	       | RV62_DIG_STEREOSYNC_SELECT
	       | RV62_DIG_SOURCE_SELECT);
    rhdPrintDigDebug(rhdPtr,__func__);
}

/*
 *
 */
static void
EncoderPower(struct rhdOutput *Output, int Power)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    CARD32 off;
    enum encoderID EncoderID = Private->EncoderID;
    RHDPtr rhdPtr = RHDPTRI(Output);

    RHDFUNC(Output);

    if (EncoderID == ENCODER_NONE) {
	EncoderID = digProbeEncoder(Output);
	switch (EncoderID) {
	    case ENCODER_DIG1:
		if (rhdPtr->DigEncoderOutput[0]) {
		    LOG("%s: DIG1 for %s already taken\n",__func__,Output->Name);
		    return;
		}
		break;
	    case ENCODER_DIG2:
		if (rhdPtr->DigEncoderOutput[1]) {
		    LOG("%s: DIG2 for %s already taken\n",__func__,Output->Name);
		    return;
		}
		break;
	    default:
		return;
	}
    }

    off = (EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;

    /* clock src is pixel PLL */
    RHDRegMask(Output, RV620_DCCG_SYMCLK_CNTL, 0x0,
	       0x3 << ((EncoderID == ENCODER_DIG2)
		       ? RV62_SYMCLKB_SRC_SHIFT
		       : RV62_SYMCLKA_SRC_SHIFT));

    rhdPrintDigDebug(rhdPtr,__func__);
    switch (Power) {
	case RHD_POWER_ON:
	    LOG("%s(RHD_POWER_ON, %d)\n",__func__,
		     EncoderID);
	    /* enable DIG */
	    RHDRegMask(Output, off + RV620_DIG1_CNTL, 0x10, 0x10);
	    RHDRegMask(Output, (EncoderID == ENCODER_DIG2)
		       ? RV620_DCCG_PCLK_DIGB_CNTL
		       : RV620_DCCG_PCLK_DIGA_CNTL,
		       RV62_PCLK_DIGA_ON, RV62_PCLK_DIGA_ON); /* @@@ */
	    rhdPrintDigDebug(rhdPtr,__func__);
	    return;
	case RHD_POWER_RESET:
	case RHD_POWER_SHUTDOWN:
	default:
	    LOG("%s(RHD_POWER_SHUTDOWN, %d)\n",__func__,
		     EncoderID);
	    /* disable differential clock driver */
	    if (EncoderID == ENCODER_DIG1)
		RHDRegMask(Output, RV620_EXT1_DIFF_POST_DIV_CNTL,
			   0,
			   RV62_EXT1_DIFF_DRIVER_ENABLE);
	    else
		RHDRegMask(Output, RV620_EXT2_DIFF_POST_DIV_CNTL,
			   0,
			   RV62_EXT2_DIFF_DRIVER_ENABLE);
	    /* disable DIG */
	    RHDRegMask(Output, off + RV620_DIG1_CNTL, 0x0, 0x10);
	    RHDRegMask(Output, (EncoderID == ENCODER_DIG2)
		       ? RV620_DCCG_PCLK_DIGB_CNTL
		       : RV620_DCCG_PCLK_DIGA_CNTL,
		       0, RV62_PCLK_DIGA_ON); /* @@@ */
	    rhdPrintDigDebug(rhdPtr,__func__);
	    return;
    }
}

/*
 *
 */
static void
EncoderSave(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct DIGEncoder *Private = (struct DIGEncoder *)(digPrivate->Encoder.Private);
    CARD32 off;
    enum encoderID EncoderID;

    RHDFUNC(Output);

    EncoderID = digProbeEncoder(Output);
    off = (EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;
    Private->StoredOff = off;

    Private->StoredRegExt1DiffPostDivCntl          = RHDRegRead(Output, off + RV620_EXT1_DIFF_POST_DIV_CNTL);
    Private->StoredRegExt2DiffPostDivCntl          = RHDRegRead(Output, off + RV620_EXT2_DIFF_POST_DIV_CNTL);
    Private->StoredDIGClockPattern = RHDRegRead(Output, off + RV620_DIG1_CLOCK_PATTERN);
    Private->StoredLVDSDataCntl    = RHDRegRead(Output, off + RV620_LVDS1_DATA_CNTL);
    Private->StoredDIGCntl         = RHDRegRead(Output, off + RV620_DIG1_CNTL);
    Private->StoredTMDSCntl        = RHDRegRead(Output, off + RV620_TMDS1_CNTL);
    Private->StoredDCIOLinkSteerCntl = RHDRegRead(Output, RV620_DCIO_LINK_STEER_CNTL);
    Private->StoredDCCGPclkDigCntl    = RHDRegRead(Output,
						   (off == DIG2_OFFSET)
						   ? RV620_DCCG_PCLK_DIGB_CNTL
						   : RV620_DCCG_PCLK_DIGA_CNTL);
    Private->StoredDCCGSymclkCntl     = RHDRegRead(Output, RV620_DCCG_SYMCLK_CNTL);
    Private->StoredBlModCntl          = RHDRegRead(Output, RV620_LVTMA_BL_MOD_CNTL);
    Private->Stored = TRUE;
}

/*
 *
 */
static void
EncoderRestore(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;
    struct DIGEncoder *Private = (struct DIGEncoder *)(digPrivate->Encoder.Private);
    CARD32 off;

    RHDFUNC(Output);

    if (!Private->Stored) {
	LOG("%s: No registers stored.\n", __func__);
	return;
    }
    off = Private->StoredOff;

    RHDRegWrite(Output, off + RV620_EXT1_DIFF_POST_DIV_CNTL, Private->StoredRegExt1DiffPostDivCntl);
    RHDRegWrite(Output, off + RV620_EXT2_DIFF_POST_DIV_CNTL, Private->StoredRegExt2DiffPostDivCntl);
    /* reprogram all values but don't start the encoder, yet */
    RHDRegWrite(Output, off + RV620_DIG1_CNTL, Private->StoredDIGCntl & ~(CARD32)RV62_DIG_START);
    RHDRegWrite(Output, RV620_DCIO_LINK_STEER_CNTL, Private->StoredDCIOLinkSteerCntl);
    RHDRegWrite(Output, off + RV620_DIG1_CLOCK_PATTERN, Private->StoredDIGClockPattern);
    RHDRegWrite(Output, off + RV620_LVDS1_DATA_CNTL, Private->StoredLVDSDataCntl);
    RHDRegWrite(Output, off + RV620_TMDS1_CNTL, Private->StoredTMDSCntl);
    RHDRegWrite(Output, (off == DIG2_OFFSET)
		? RV620_DCCG_PCLK_DIGB_CNTL
		: RV620_DCCG_PCLK_DIGA_CNTL,
		Private->StoredDCCGPclkDigCntl);
    /* now enable the encoder */
    RHDRegWrite(Output, off + RV620_DIG1_CNTL, Private->StoredDIGCntl);
    RHDRegWrite(Output, RV620_DCCG_SYMCLK_CNTL, Private->StoredDCCGSymclkCntl);
    RHDRegWrite(Output, RV620_LVTMA_BL_MOD_CNTL, Private->StoredBlModCntl);
}

/*
 *
 */
static void
EncoderDestroy(struct rhdOutput *Output)
{
    struct DIGPrivate *digPrivate = (struct DIGPrivate *)Output->Private;

    RHDFUNC(Output);

    if (!digPrivate || !digPrivate->Encoder.Private)
	return;

    IODelete(digPrivate->Encoder.Private, struct DIGEncoder,1);
}

/*
 * Housekeeping
 */
void
GetLVDSInfo(RHDPtr rhdPtr, struct DIGPrivate *Private)
{
    CARD32 off = (Private->EncoderID == ENCODER_DIG2) ? DIG2_OFFSET : DIG1_OFFSET;
    CARD32 tmp;

    RHDFUNC(rhdPtr);

    Private->FPDI = ((RHDRegRead(rhdPtr, off + RV620_LVDS1_DATA_CNTL)
				 & RV62_LVDS_24BIT_FORMAT) != 0);
    Private->RunDualLink = ((RHDRegRead(rhdPtr, off + RV620_DIG1_CNTL)
				 & RV62_DIG_DUAL_LINK_ENABLE) != 0);
    Private->FMTDither.LVDS24Bit = ((RHDRegRead(rhdPtr, off  + RV620_LVDS1_DATA_CNTL)
			   & RV62_LVDS_24BIT_ENABLE) != 0);

    tmp = RHDRegRead(rhdPtr, RV620_LVTMA_BL_MOD_CNTL);
    if (tmp & LVTMA_BL_MOD_EN)
	Private->BlLevel = ( tmp >> LVTMA_BL_MOD_LEVEL_SHIFT )  & 0xff;
    else
	Private->BlLevel = -1;

    tmp = RHDRegRead(rhdPtr, RV620_LVTMA_PWRSEQ_REF_DIV);
    tmp &= 0xffff;
    tmp += 1;
    tmp /= 1000;
    Private->PowerSequenceDig2De = Private->PowerSequenceDe2Bl =
	RHDRegRead(rhdPtr, RV620_LVTMA_PWRSEQ_REF_DIV);
    Private->PowerSequenceDig2De = ((Private->PowerSequenceDig2De & 0xff) * tmp) / 10;
    Private->PowerSequenceDe2Bl = (((Private->PowerSequenceDe2Bl >> 8) & 0xff) * tmp) / 10;
    Private->OffDelay = RHDRegRead(rhdPtr, RV620_LVTMA_PWRSEQ_DELAY2);
    Private->OffDelay *= tmp;

    /* This is really ugly! */
    {
	CARD32 fmt_offset;

	tmp = RHDRegRead(rhdPtr, off + RV620_DIG1_CNTL);
	fmt_offset = (tmp & RV62_DIG_SOURCE_SELECT) ? FMT2_OFFSET :0;
	tmp = RHDRegRead(rhdPtr, fmt_offset + RV620_FMT1_BIT_DEPTH_CONTROL);
	Private->FMTDither.LVDSSpatialDither = ((tmp & 0x100) != 0);
	Private->FMTDither.LVDSGreyLevel = ((tmp & 0x10000) != 0);
	Private->FMTDither.LVDSTemporalDither
	    = (Private->FMTDither.LVDSGreyLevel > 0) || ((tmp & 0x1000000) != 0);
    }

#ifdef ATOM_BIOS
    {
	AtomBiosArgRec data;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
				 ATOM_LVDS_FPDI, &data) == ATOM_SUCCESS)
	    Private->FPDI = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_DUALLINK, &data) == ATOM_SUCCESS)
	    Private->RunDualLink = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_GREYLVL, &data) == ATOM_SUCCESS)
	    Private->FMTDither.LVDSGreyLevel = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_SEQ_DIG_ONTO_DE, &data) == ATOM_SUCCESS)
	    Private->PowerSequenceDig2De = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_SEQ_DE_TO_BL, &data) == ATOM_SUCCESS)
	    Private->PowerSequenceDe2Bl = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_OFF_DELAY, &data) == ATOM_SUCCESS)
	    Private->OffDelay = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_24BIT, &data) == ATOM_SUCCESS)
	    Private->FMTDither.LVDS24Bit = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_SPATIAL_DITHER, &data) == ATOM_SUCCESS)
	    Private->FMTDither.LVDSSpatialDither = data.val;

	if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS,
			    ATOM_LVDS_TEMPORAL_DITHER, &data) == ATOM_SUCCESS)
	    Private->FMTDither.LVDSTemporalDither = data.val;

	Private->PowerSequenceDe2Bl = data.val;

    }
#endif

}

/*
 * Infrastructure
 */

static ModeStatus
DigModeValid(struct rhdOutput *Output, DisplayModePtr Mode)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;
    ModeStatus Status;

    RHDFUNC(Output);

    if ((Status = Transmitter->ModeValid(Output, Mode)) == MODE_OK)
	return ((Encoder->ModeValid(Output, Mode)));
    else
	return Status;
}

/*
 *
 */
static void
DigPower(struct rhdOutput *Output, int Power)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;

    LOG("%s(%s,%s)\n",__func__,Output->Name,
	     rhdPowerString[Power]);

    switch (Power) {
	case RHD_POWER_ON:
	    Encoder->Power(Output, Power);
	    Transmitter->Power(Output, Power);
	    RHDHdmiEnable(Private->Hdmi, Private->EncoderMode == TMDS_HDMI);
	    return;
	case RHD_POWER_RESET:
	    Transmitter->Power(Output, Power);
	    Encoder->Power(Output, Power);
	    return;
	case RHD_POWER_SHUTDOWN:
	default:
	    Transmitter->Power(Output, Power);
	    Encoder->Power(Output, Power);
	    RHDHdmiEnable(Private->Hdmi, FALSE);
	    return;
    }
}

/*
 *
 */
static Bool
DigPropertyControl(struct rhdOutput *Output,
			      enum rhdPropertyAction Action, enum rhdOutputProperty Property, union rhdPropertyData *val)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;

    RHDFUNC(Output);

    switch(Property) {
	case RHD_OUTPUT_COHERENT:
	case RHD_OUTPUT_BACKLIGHT:
	case RHD_OUTPUT_HDMI:
	{
	    if (!Private->Transmitter.Property)
		return FALSE;
	    return Private->Transmitter.Property(Output, Action, Property, val);
	}
	default:
	    return FALSE;
    }
    return TRUE;
}


/*
 *
 */
static void
DigMode(struct rhdOutput *Output, DisplayModePtr Mode)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;
    struct rhdCrtc *Crtc = Output->Crtc;

    RHDFUNC(Output);

    Private->Mode = Mode;

    /*
     * For dual link capable DVI we need to decide from the pix clock if we are dual link.
     * Do it here as it is convenient.
     */
    if (Output->Connector->Type == RHD_CONNECTOR_DVI)
	Private->RunDualLink = (Mode->SynthClock > 165000) ? TRUE : FALSE;

    Encoder->Mode(Output, Crtc, Mode);
    Transmitter->Mode(Output, Crtc, Mode);
    RHDHdmiSetMode(Private->Hdmi, Mode);
}

/*
 *
 */
static void
DigSave(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;

    RHDFUNC(Output);

    Encoder->Save(Output);
    Transmitter->Save(Output);
    RHDHdmiSave(Private->Hdmi);
}

/*
 *
 */
static void
DigRestore(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;

    RHDFUNC(Output);

    Encoder->Restore(Output);
    Transmitter->Restore(Output);
    RHDHdmiRestore(Private->Hdmi);
}

/*
 *
 */
static void
DigDestroy(struct rhdOutput *Output)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    struct transmitter *Transmitter = &Private->Transmitter;
    struct encoder *Encoder = &Private->Encoder;

    RHDFUNC(Output);

    Encoder->Destroy(Output);
    Transmitter->Destroy(Output);
    RHDHdmiDestroy(Private->Hdmi);
#ifdef ATOM_BIOS
    if (Transmitter->PropertyPrivate)
	RhdAtomDestroyBacklightControlProperty(Output, Transmitter->PropertyPrivate);
#endif
    IODelete(Private, struct DIGPrivate, 1);
    Output->Private = NULL;
}

/*
 *
 */
static Bool
DigAllocFree(struct rhdOutput *Output, enum rhdOutputAllocation Alloc)
{
	static char names[3][15] = {
		"KLDSKP_LVTMA",
	    "UNIPHYA",
		"UNIPHYB"
	};
	
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    RHDPtr rhdPtr = RHDPTRI(Output);
    char *TransmitterName;

    RHDFUNC(rhdPtr);

    switch (Output->Id) {
	case RHD_OUTPUT_KLDSKP_LVTMA:
	    TransmitterName = names[0];
	    break;
	case RHD_OUTPUT_UNIPHYA:
	    TransmitterName = names[1];
	    break;
	case RHD_OUTPUT_UNIPHYB:
	    TransmitterName = names[2];
	    break;
	default:
	    return FALSE;
    }
    switch (Alloc) {
	case RHD_OUTPUT_ALLOC:

	    if (Private->EncoderID != ENCODER_NONE)
		return TRUE;

	    /*
	     * LVTMA can only use DIG2. Thus exclude
	     * DIG1 for LVTMA and prefer it for the
	     * UNIPHYs.
	     */
	    if (Output->Id == RHD_OUTPUT_KLDSKP_LVTMA) {
		if (!rhdPtr->DigEncoderOutput[1]) {
		    rhdPtr->DigEncoderOutput[1] = Output;
		    Private->EncoderID = ENCODER_DIG2;
		    LOG("Mapping DIG2 encoder to %s\n",TransmitterName);
		return TRUE;
		} else
		    return FALSE;
	    } else
#if defined(ATOM_BIOS) && defined(ATOM_BIOS_PARSER)
	    {
		struct ATOMTransmitterPrivate *transPrivate =
		    (struct ATOMTransmitterPrivate *)Private->Transmitter.Private;
		struct atomTransmitterConfig *atc = &transPrivate->atomTransmitterConfig;
		if (!rhdPtr->DigEncoderOutput[0]) {
		    rhdPtr->DigEncoderOutput[0] = Output;
		    Private->EncoderID = ENCODER_DIG1;
		    atc->Encoder = atomEncoderDIG1;
		    LOG("Mapping DIG1 encoder to %s\n",TransmitterName);
		    return TRUE;
		} else if (!rhdPtr->DigEncoderOutput[1]) {
		    rhdPtr->DigEncoderOutput[1] = Output;
		    Private->EncoderID = ENCODER_DIG2;
		    atc->Encoder = atomEncoderDIG2;
		    LOG("Mapping DIG2 encoder to %s\n",TransmitterName);
		    return TRUE;
		} else
		    return FALSE;
	    }
#else
	    return FALSE;
#endif
	case RHD_OUTPUT_FREE:
		Private->EncoderID = ENCODER_NONE;
	    if (rhdPtr->DigEncoderOutput[0] == Output) {
		rhdPtr->DigEncoderOutput[0] = NULL;
		return TRUE;
	    } else if (rhdPtr->DigEncoderOutput[1] == Output) {
		rhdPtr->DigEncoderOutput[1] = NULL;
		return TRUE;
	    } else
		return FALSE;
	    break;
	default:
	    return FALSE;
    }
}

/*
 *
 */
static Bool
digTransmitterPropertyWrapper(struct rhdOutput *Output,
			      enum rhdPropertyAction Action,
			      enum rhdOutputProperty Property,
			      union rhdPropertyData *val)
{
    struct DIGPrivate *Private = (struct DIGPrivate *)Output->Private;
    void *storePrivate = Output->Private;
    Bool (*func)(struct rhdOutput *,enum rhdPropertyAction, enum rhdOutputProperty,
		  union rhdPropertyData *) = Private->Transmitter.WrappedPropertyCallback;
    Bool ret;

    Output->Private = Private->Transmitter.PropertyPrivate;
    ret = func(Output, Action, Property, val);
    Output->Private = storePrivate;

    return ret;
}

/*
 *
 */
struct rhdOutput *
RHDDIGInit(RHDPtr rhdPtr,  enum rhdOutputType outputType, CARD8 ConnectorType)
{
#ifdef ATOM_BIOS
	static char UNIPHYA[9] = "UNIPHY_A";
	static char UNIPHYB[9] = "UNIPHY_B";
#endif
	static char UNIPHY_KLDSKP_LVTMA[20] = "UNIPHY_KLDSKP_LVTMA";

    struct rhdOutput *Output;
    struct DIGPrivate *Private;
    struct DIGEncoder *Encoder;

    RHDFUNC(rhdPtr);

    Output = IONew(struct rhdOutput, 1);
	if (!Output) return NULL;
	bzero(Output, sizeof(struct rhdOutput));

    Output->scrnIndex = rhdPtr->scrnIndex;
    Output->Id = outputType;

    Output->Sense = NULL;
    Output->ModeValid = DigModeValid;
    Output->Mode = DigMode;
    Output->Power = DigPower;
    Output->Save = DigSave;
    Output->Restore = DigRestore;
    Output->Destroy = DigDestroy;
    Output->Property = DigPropertyControl;
    Output->AllocFree = DigAllocFree;

    Private = IONew(struct DIGPrivate, 1);
	if (!Private) {
		IODelete(Output, struct rhdOutput, 1);
		return NULL;
	}
	bzero(Private, sizeof(struct DIGPrivate));
    Output->Private = Private;

    Private->EncoderID = ENCODER_NONE;
	
    switch (outputType) {
	case RHD_OUTPUT_UNIPHYA:
#if defined (ATOM_BIOS) && defined (ATOM_BIOS_PARSER)
			Output->Name = UNIPHYA;
			Private->Transmitter.Private = IONew(struct ATOMTransmitterPrivate, 1);
			if (!Private->Transmitter.Private) {
				IODelete(Private, struct DIGPrivate, 1);
				IODelete(Output, struct rhdOutput, 1);
				return NULL;
			}
			bzero(Private->Transmitter.Private, sizeof(struct ATOMTransmitterPrivate));
			Private->Transmitter.Sense = NULL;
			Private->Transmitter.ModeValid = ATOMTransmitterModeValid;
			Private->Transmitter.Mode = ATOMTransmitterSet;
			Private->Transmitter.Power = ATOMTransmitterPower;
			Private->Transmitter.Save = ATOMTransmitterSave;
			Private->Transmitter.Restore = ATOMTransmitterRestore;
			Private->Transmitter.Destroy = ATOMTransmitterDestroy;
			Private->Transmitter.Property = TMDSTransmitterPropertyControl;
	    {
			struct ATOMTransmitterPrivate *transPrivate =
		    (struct ATOMTransmitterPrivate *)Private->Transmitter.Private;
			struct atomTransmitterConfig *atc = &transPrivate->atomTransmitterConfig;
			atc->Coherent = Private->Coherent = FALSE;
			atc->Link = atomTransLinkA;
			atc->Encoder = atomEncoderNone;
			if (RHDIsIGP(rhdPtr->ChipSet)) {
				AtomBiosArgRec data;
				data.val = 1;
				if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS, ATOM_GET_PCIE_LANES,
									&data) == ATOM_SUCCESS)
					atc->Lanes = data.pcieLanes.Chassis; /* only do 'chassis' for now */
				else {
					IODelete(Private, struct DIGPrivate, 1);
					IODelete(Output, struct rhdOutput, 1);
					return NULL;
				}
			}
			if (RHDIsIGP(rhdPtr->ChipSet))
				transPrivate->atomTransmitterID = atomTransmitterPCIEPHY;
			else
				transPrivate->atomTransmitterID = atomTransmitterUNIPHY;
	    }
			break;
#else
		IODelete(Private, struct DIGPrivate, 1);
	    IODelete(Output, struct rhdOutput, 1);
	    return NULL;
#endif /* ATOM_BIOS && ATOM_BIOS_PARSER */

	case RHD_OUTPUT_UNIPHYB:
#if defined (ATOM_BIOS) && defined (ATOM_BIOS_PARSER)
	    Output->Name = UNIPHYB;
	    Private->Transmitter.Private = IONew(struct ATOMTransmitterPrivate, 1);
			if (!Private->Transmitter.Private) {
				IODelete(Private, struct DIGPrivate, 1);
				IODelete(Output, struct rhdOutput, 1);
				return NULL;
			}
			bzero(Private->Transmitter.Private, sizeof(struct ATOMTransmitterPrivate));
	    Private->Transmitter.Sense = NULL;
	    Private->Transmitter.ModeValid = ATOMTransmitterModeValid;
	    Private->Transmitter.Mode = ATOMTransmitterSet;
	    Private->Transmitter.Power = ATOMTransmitterPower;
	    Private->Transmitter.Save = ATOMTransmitterSave;
	    Private->Transmitter.Restore = ATOMTransmitterRestore;
	    Private->Transmitter.Destroy = ATOMTransmitterDestroy;
	    Private->Transmitter.Property = TMDSTransmitterPropertyControl;
	    {
		struct ATOMTransmitterPrivate *transPrivate =
		    (struct ATOMTransmitterPrivate *)Private->Transmitter.Private;
		struct atomTransmitterConfig *atc = &transPrivate->atomTransmitterConfig;
		atc->Coherent = Private->Coherent = FALSE;
		atc->Link = atomTransLinkB;
		atc->Encoder = atomEncoderNone;
		if (RHDIsIGP(rhdPtr->ChipSet)) {
		    AtomBiosArgRec data;
		    data.val = 2;
		    if (RHDAtomBiosFunc(rhdPtr->scrnIndex, rhdPtr->atomBIOS, ATOM_GET_PCIE_LANES,
					&data) == ATOM_SUCCESS)
			atc->Lanes = data.pcieLanes.Chassis; /* only do 'chassis' for now */
		    else {
			IODelete(Private, struct DIGPrivate, 1);
			IODelete(Output, struct rhdOutput, 1);
			return NULL;
		    }
		}
		if (RHDIsIGP(rhdPtr->ChipSet))
		    transPrivate->atomTransmitterID = atomTransmitterPCIEPHY;
		else
		    transPrivate->atomTransmitterID = atomTransmitterUNIPHY;
	    }
	    break;
#else
			IODelete(Private, struct DIGPrivate, 1);
			IODelete(Output, struct rhdOutput, 1);
	    return NULL;
#endif /* ATOM_BIOS && ATOM_BIOS_PARSER */

	case RHD_OUTPUT_KLDSKP_LVTMA:
	    Output->Name = UNIPHY_KLDSKP_LVTMA;
	    Private->Coherent = FALSE;
	    Private->Transmitter.Private = IONew(struct LVTMATransmitterPrivate, 1);
			if (!Private->Transmitter.Private) {
				IODelete(Private, struct DIGPrivate, 1);
				IODelete(Output, struct rhdOutput, 1);
				return NULL;
			}
			bzero(Private->Transmitter.Private, sizeof(struct LVTMATransmitterPrivate));
	    Private->Transmitter.Sense = NULL;
	    Private->Transmitter.ModeValid = LVTMATransmitterModeValid;
	    if (ConnectorType != RHD_CONNECTOR_PANEL) {
		Private->Transmitter.Mode = LVTMA_TMDSTransmitterSet;
		Private->Transmitter.Power = LVTMA_TMDSTransmitterPower;
		Private->Transmitter.Save = LVTMA_TMDSTransmitterSave;
		Private->Transmitter.Restore = LVTMA_TMDSTransmitterRestore;
	    } else {
		Private->Transmitter.Mode = LVTMA_LVDSTransmitterSet;
		Private->Transmitter.Power = LVTMA_LVDSTransmitterPower;
		Private->Transmitter.Save = LVTMA_LVDSTransmitterSave;
		Private->Transmitter.Restore = LVTMA_LVDSTransmitterRestore;
	    }
	    Private->Transmitter.Destroy = LVTMATransmitterDestroy;
	    if (ConnectorType == RHD_CONNECTOR_PANEL)
		Private->Transmitter.Property = LVDSTransmitterPropertyControl;
	    else
		Private->Transmitter.Property = TMDSTransmitterPropertyControl;
	    break;

	default:
			IODelete(Private, struct DIGPrivate, 1);
			IODelete(Output, struct rhdOutput, 1);
	    return NULL;
    }


    Encoder = IONew(struct DIGEncoder,1);
	if (!Encoder) {
		if (Private->Transmitter.Private) {
			if ((outputType == RHD_OUTPUT_UNIPHYA) || (outputType == RHD_OUTPUT_UNIPHYB))
				IODelete(Private->Transmitter.Private, struct ATOMTransmitterPrivate, 1);
			else
				IODelete(Private->Transmitter.Private, struct LVTMATransmitterPrivate, 1);
		}
		IODelete(Private, struct DIGPrivate, 1);
		IODelete(Output, struct rhdOutput, 1);
		return NULL;
	}
	bzero(Encoder, sizeof(struct DIGEncoder));
    Private->Encoder.Private = Encoder;
    Private->Encoder.ModeValid = EncoderModeValid;
    Private->Encoder.Mode = EncoderSet;
    Private->Encoder.Power = EncoderPower;
    Private->Encoder.Save = EncoderSave;
    Private->Encoder.Restore = EncoderRestore;
    Private->Encoder.Destroy = EncoderDestroy;

    switch (ConnectorType) {
	case RHD_CONNECTOR_PANEL:
	    Private->EncoderMode = LVDS;
	    GetLVDSInfo(rhdPtr, Private);
#ifdef ATOM_BIOS
	    if (Private->BlLevel < 0) {
		Private->BlLevel = RhdAtomSetupBacklightControlProperty(Output,
									&Private->Transmitter.WrappedPropertyCallback,
									&Private->Transmitter.PropertyPrivate);
		if (Private->Transmitter.PropertyPrivate)
		    Private->Transmitter.Property = digTransmitterPropertyWrapper;
	    }
#endif
	    Private->Hdmi = NULL;
	    break;
	case RHD_CONNECTOR_DVI:
	    Private->RunDualLink = FALSE; /* will be set later acc to pxclk */
	    Private->EncoderMode = TMDS_DVI;
	    Private->Hdmi = RHDHdmiInit(rhdPtr, Output);
	    break;
	case RHD_CONNECTOR_DVI_SINGLE:
	    Private->RunDualLink = FALSE;
	    Private->EncoderMode = TMDS_DVI; /* changed later to HDMI if aplicateable */
	    Private->Hdmi = RHDHdmiInit(rhdPtr, Output);
	    break;
    }

    return Output;
}
