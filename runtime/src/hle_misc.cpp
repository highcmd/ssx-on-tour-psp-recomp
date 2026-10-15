// Audio, SAS, ATRAC, MPEG and utility dialogs.
// First milestone: correct *timing* and state machines, silent output.
#include <cstring>
#include <map>

#include "kernel.h"

// ------------------------------------------------------------- sceAudio
struct AudioChan {
	bool reserved = false;
	u32 samples = 0;
	u32 fmt = 0;
	u64 busyUntil = 0;
};
static AudioChan s_chans[8];

static int reserve_chan(int ch) {
	if (ch < 0) {
		for (int i = 7; i >= 0; i--)
			if (!s_chans[i].reserved) { ch = i; break; }
		if (ch < 0) return 0x80260002;
	}
	if (ch > 7) return 0x80260002;
	s_chans[ch].reserved = true;
	return ch;
}

HLE(sceAudio, sceAudioChReserve) {
	int ch = reserve_chan((int)ARG(0));
	if (ch >= 0 && ch < 8) { s_chans[ch].samples = ARG(1); s_chans[ch].fmt = ARG(2); }
	rt_log(LOG_AUDIO, "ChReserve(%d, %u, %u) -> %d", (int)ARG(0), ARG(1), ARG(2), ch);
	RET(ch);
}
HLE(sceAudio, sceAudioChRelease) { if (ARG(0) < 8) s_chans[ARG(0)].reserved = false; RET(0); }
HLE(sceAudio, sceAudioSetChannelDataLen) { if (ARG(0) < 8) s_chans[ARG(0)].samples = ARG(1); RET(0); }
HLE(sceAudio, sceAudioChangeChannelConfig) { if (ARG(0) < 8) s_chans[ARG(0)].fmt = ARG(1); RET(0); }
HLE(sceAudio, sceAudioChangeChannelVolume) { RET(0); }

// A blocking output returns once the previous buffer has been played (44.1 kHz).
static void audio_output(CPUState *c, bool blocking) {
	u32 ch = ARG(0);
	if (ch >= 8 || !s_chans[ch].reserved) { RET(0x80260008); return; }
	AudioChan &a = s_chans[ch];
	u64 now = rt_time_us();
	if (a.busyUntil > now) {
		if (!blocking) { RET(0x80260009); return; }  // busy
		kernel_wait(WAIT_AUDIO, ch, a.busyUntil - now, false);
		now = rt_time_us();
	}
	u64 dur = (u64)a.samples * 1000000ull / 44100;
	a.busyUntil = std::max(a.busyUntil, now) + dur;
	RET(a.samples);
}
HLE(sceAudio, sceAudioOutputBlocking) { audio_output(c, true); }
HLE(sceAudio, sceAudioOutputPannedBlocking) { audio_output(c, true); }
HLE(sceAudio, sceAudioOutputPanned) { audio_output(c, false); }
HLE(sceAudio, sceAudioGetChannelRestLength) {
	u32 ch = ARG(0);
	if (ch >= 8) { RET(0x80260008); return; }
	u64 now = rt_time_us();
	RET(s_chans[ch].busyUntil > now ? (u32)((s_chans[ch].busyUntil - now) * 44100 / 1000000) : 0);
}

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

// ------------------------------------------------------------- sceAtrac3plus
static int s_next_atrac = 0;
HLE(sceAtrac3plus, sceAtracSetDataAndGetID) {
	int id = s_next_atrac++ & 3;
	rt_log(LOG_AUDIO, "AtracSetDataAndGetID(%08x, %x) -> %d", ARG(0), ARG(1), id);
	RET(id);
}
HLE(sceAtrac3plus, sceAtracReleaseAtracID) { RET(0); }
HLE(sceAtrac3plus, sceAtracDecodeData) {
	// (id, outbuf, *samples, *finish, *remainFrames)
	if (ARG(2)) SW(ARG(2), 0);
	if (ARG(3)) SW(ARG(3), 1);
	if (ARG(4)) SW(ARG(4), (u32)-1);
	RET(0x80630024);  // ALL_DATA_DECODED
}
HLE(sceAtrac3plus, sceAtracGetMaxSample) { if (ARG(1)) SW(ARG(1), 2048); RET(0); }
HLE(sceAtrac3plus, sceAtracGetNextSample) { if (ARG(1)) SW(ARG(1), 0); RET(0); }
HLE(sceAtrac3plus, sceAtracGetStreamDataInfo) {
	if (ARG(1)) SW(ARG(1), 0);
	if (ARG(2)) SW(ARG(2), 0);
	if (ARG(3)) SW(ARG(3), 0);
	RET(0);
}
HLE(sceAtrac3plus, sceAtracAddStreamData) { RET(0); }
HLE(sceAtrac3plus, sceAtracSetLoopNum) { RET(0); }

// ------------------------------------------------------------- sceUtility dialogs
// Status: 0 none, 1 init, 2 running, 3 finished, 4 shutdown.
struct Dialog {
	int status = 0;
	u32 param = 0;
	int ticks = 0;
};
static Dialog s_save, s_msg, s_osk, s_net;

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

HLE(sceUtility, sceUtilitySavedataInitStart) {
	dlg_start(s_save, ARG(0), "Savedata");
	u32 p = ARG(0);
	u32 mode = LW(p + 0x30);
	std::string game((const char *)gptr(p + 0x3C), strnlen((const char *)gptr(p + 0x3C), 13));
	std::string save((const char *)gptr(p + 0x4C), strnlen((const char *)gptr(p + 0x4C), 20));
	rt_log(LOG_MISC, "  savedata mode %u game '%s' save '%s'", mode, game.c_str(), save.c_str());
	// No save system yet: every load/list reports "no data", every save "succeeds".
	bool load = mode == 0 || mode == 2 || mode == 4 || mode == 6 || mode == 11 || mode == 12 || mode == 15;
	SW(p + 0x1C, load ? 0x80110307 : 0);
	RET(0);
}
HLE(sceUtility, sceUtilitySavedataGetStatus) { RET(dlg_status(s_save)); }
HLE(sceUtility, sceUtilitySavedataUpdate) { if (s_save.status == 2) s_save.status = 3; RET(0); }
HLE(sceUtility, sceUtilitySavedataShutdownStart) { s_save.status = 4; RET(0); }

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
