// Audio, SAS, ATRAC, MPEG and utility dialogs.
// First milestone: correct *timing* and state machines, silent output.
#include <cstring>
#include <map>

#include "kernel.h"

// ------------------------------------------------------------- sceSasCore
static u32 s_sas_grain = 256, s_sas_outmode = 0;
HLE(sceSasCore, __sceSasInit) {
	s_sas_grain = ARG(1);
	s_sas_outmode = ARG(3);
	rt_log(LOG_AUDIO, "SasInit(core %08x, grain %u, voices %u, outmode %u, rate %u)", ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
	RET(0);
}
HLE(sceSasCore, __sceSasCore) {
	u32 bytes = s_sas_grain * (s_sas_outmode == 0 ? 4 : 2);
	if (gvalid(ARG(1))) memset(gptr(ARG(1)), 0, bytes);
	RET(0);
}
HLE(sceSasCore, __sceSasGetEndFlag) { RET(0xFFFFFFFF); }  // all voices ended
HLE(sceSasCore, __sceSasGetEnvelopeHeight) { RET(0); }

// ------------------------------------------------------------- sceUtility dialogs
// Status: 0 none, 1 init, 2 running, 3 finished, 4 shutdown.
struct Dialog {
	int status = 0;
	u32 param = 0;
	int ticks = 0;
};
static Dialog s_msg, s_osk, s_net;

static int dlg_status(Dialog &d) {
	int s = d.status;
	if (d.status == 1) d.status = 2;
	else if (d.status == 4) d.status = 0;
	return s;
}

static void dlg_start(Dialog &d, u32 param, const char *what) {
	d.status = 1;
	d.param = param;
	d.ticks = 0;
	rt_log(LOG_MISC, "%s InitStart(%08x)", what, param);
}

HLE(sceUtility, sceUtilityMsgDialogInitStart) {
	dlg_start(s_msg, ARG(0), "MsgDialog");
	u32 p = ARG(0);
	rt_log(LOG_MISC, "  message: %s", gstr(p + 0x3C).c_str());
	SW(p + 0x1C, 0);
	RET(0);
}
HLE(sceUtility, sceUtilityMsgDialogGetStatus) { RET(dlg_status(s_msg)); }
HLE(sceUtility, sceUtilityMsgDialogUpdate) {
	if (s_msg.status == 2 && ++s_msg.ticks > 30) {
		s_msg.status = 3;
		SW(s_msg.param + 0x240, 1);  // buttonPressed = YES
	}
	RET(0);
}
HLE(sceUtility, sceUtilityMsgDialogShutdownStart) { s_msg.status = 4; RET(0); }

HLE(sceUtility, sceUtilityOskInitStart) { dlg_start(s_osk, ARG(0), "Osk"); RET(0); }
HLE(sceUtility, sceUtilityOskGetStatus) { RET(dlg_status(s_osk)); }
HLE(sceUtility, sceUtilityOskUpdate) { if (s_osk.status == 2) s_osk.status = 3; RET(0); }
HLE(sceUtility, sceUtilityOskShutdownStart) { s_osk.status = 4; RET(0); }

HLE(sceUtility, sceUtilityNetconfInitStart) { dlg_start(s_net, ARG(0), "Netconf"); SW(ARG(0) + 0x1C, 0x80110601); RET(0); }
HLE(sceUtility, sceUtilityNetconfGetStatus) { RET(dlg_status(s_net)); }
HLE(sceUtility, sceUtilityNetconfUpdate) { if (s_net.status == 2) s_net.status = 3; RET(0); }
HLE(sceUtility, sceUtilityNetconfShutdownStart) { s_net.status = 4; RET(0); }

HLE(sceUtility, sceUtilityGetSystemParamInt) {
	u32 id = ARG(0), v = 0;
	switch (id) {
	case 1: v = 0; break;      // adhoc channel
	case 2: v = 0; break;      // wlan power save
	case 3: v = 0; break;      // date format
	case 4: v = 0; break;      // time format 24h
	case 5: v = 0; break;      // timezone
	case 6: v = 0; break;      // daylight saving
	case 8: v = 1; break;      // language: English
	case 9: v = 1; break;      // button: cross = confirm
	default: break;
	}
	SW(ARG(1), v);
	RET(0);
}

// ------------------------------------------------------------- SAS effects (no-ops for now)
HLE(sceSasCore, __sceSasRevType) { RET(0); }
HLE(sceSasCore, __sceSasRevVON) { RET(0); }
HLE(sceSasCore, __sceSasRevEVOL) { RET(0); }
HLE(sceSasCore, __sceSasRevParam) { RET(0); }
