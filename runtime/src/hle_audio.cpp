// sceAudio output + sceAtrac3plus (music) decoding.
//
// Output: every sceAudio channel writes its samples onto one 44.1 kHz timeline
// (a ring of mixed frames). The SDL callback plays that timeline ~50 ms behind
// "now". Blocking calls keep their PSP timing from the system clock, so the game
// is paced identically with or without an audio device (headless).
//
// ATRAC: the game's buffer is treated as a ring that receives the file
// sequentially (GetStreamDataInfo / AddStreamData); we keep our own copy of all
// received bytes and decode 2048-sample frames with FFmpeg's atrac3p decoder.
// Error codes and struct semantics follow PPSSPP Core/HLE (public, GPL).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "kernel.h"

// ================================================================ mixer timeline
static const int RATE = 44100;
static const int RING = 1 << 17;                 // frames (~3 s)
static const s64 LATENCY = 2048;                 // frames written ahead of "now"
static int32_t s_ring[RING][2];
static std::mutex s_ring_mtx;
static s64 s_read_frame;
static bool s_audio_open;

static s64 now_frame() { return (s64)(rt_time_us() * (double)RATE / 1e6); }

static void sdl_callback(void *, Uint8 *stream, int len) {
	int16_t *out = (int16_t *)stream;
	int frames = len / 4;
	std::lock_guard<std::mutex> lk(s_ring_mtx);
	s64 now = now_frame();
	// keep the device clock locked to the system clock
	if (s_read_frame < now - 4096 || s_read_frame > now + 4096) s_read_frame = now;
	for (int i = 0; i < frames; i++) {
		int32_t *f = s_ring[(s_read_frame + i) & (RING - 1)];
		out[2 * i] = (int16_t)std::clamp(f[0], -32768, 32767);
		out[2 * i + 1] = (int16_t)std::clamp(f[1], -32768, 32767);
		f[0] = f[1] = 0;
	}
	s_read_frame += frames;
}

void audio_init(bool headless) {
	if (headless || getenv("SSX_NOAUDIO")) return;
	SDL_AudioSpec want{}, have{};
	want.freq = RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = sdl_callback;
	SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (!dev) { rt_log_error("SDL audio: %s", SDL_GetError()); return; }
	s_read_frame = now_frame();
	SDL_PauseAudioDevice(dev, 0);
	s_audio_open = true;
	rt_log(LOG_AUDIO, "audio device open: %d Hz, %d ch, %d samples", have.freq, have.channels, have.samples);
}

// Mix n stereo frames at timeline position `at`.
static void mix_at(s64 at, const int16_t *src, int n, bool stereo, int lvol, int rvol) {
	if (!s_audio_open) return;
	std::lock_guard<std::mutex> lk(s_ring_mtx);
	for (int i = 0; i < n; i++) {
		int l = stereo ? src[2 * i] : src[i], r = stereo ? src[2 * i + 1] : src[i];
		int32_t *f = s_ring[(at + i) & (RING - 1)];
		f[0] += (l * lvol) >> 15;
		f[1] += (r * rvol) >> 15;
	}
}

// ================================================================ sceAudio
struct AudioChan {
	bool reserved = false;
	u32 samples = 0;
	u32 fmt = 0;           // 0 stereo, 0x10 mono
	s64 next = 0;          // timeline frame where the next buffer starts
	int lvol = 0x8000, rvol = 0x8000;
};
static AudioChan s_chans[9];   // 8 = the Output2 (SRC) channel

HLE(sceAudio, sceAudioChReserve) {
	int ch = (int)ARG(0);
	if (ch < 0) {
		for (int i = 7; i >= 0; i--)
			if (!s_chans[i].reserved) { ch = i; break; }
		if (ch < 0) { RET(0x80260002); return; }
	}
	if (ch > 7 || s_chans[ch].reserved) { RET(0x80260002); return; }
	s_chans[ch] = AudioChan{};
	s_chans[ch].reserved = true;
	s_chans[ch].samples = ARG(1);
	s_chans[ch].fmt = ARG(2);
	rt_log(LOG_AUDIO, "ChReserve(%d, %u, fmt %u) -> %d", (int)ARG(0), ARG(1), ARG(2), ch);
	RET(ch);
}
HLE(sceAudio, sceAudioChRelease) { if (ARG(0) < 8) s_chans[ARG(0)].reserved = false; RET(0); }
HLE(sceAudio, sceAudioSetChannelDataLen) { if (ARG(0) < 8) s_chans[ARG(0)].samples = ARG(1); RET(0); }
HLE(sceAudio, sceAudioChangeChannelConfig) { if (ARG(0) < 8) s_chans[ARG(0)].fmt = ARG(1); RET(0); }
HLE(sceAudio, sceAudioChangeChannelVolume) {
	if (ARG(0) < 8) { s_chans[ARG(0)].lvol = ARG(1) & 0xFFFF; s_chans[ARG(0)].rvol = ARG(2) & 0xFFFF; }
	RET(0);
}

// A blocking output returns once at most one buffer is queued ahead of playback.
static void audio_output(CPUState *c, u32 ch, int lvol, int rvol, u32 buf, bool blocking) {
	if (ch >= 9 || !s_chans[ch].reserved) { RET(0x80260008); return; }
	AudioChan &a = s_chans[ch];
	s64 n = a.samples;
	s64 now = now_frame();
	if (a.next < now + LATENCY) a.next = now + LATENCY;         // underrun: restart just ahead
	s64 ahead = a.next - (now + LATENCY);                         // frames queued beyond the latency
	if (ahead > n) {
		if (!blocking) { RET(0x80260009); return; }               // channel busy
		kernel_wait(WAIT_AUDIO, ch, (u64)((ahead - n) * 1e6 / RATE) + 1, false);
	}
	if (buf && gvalid(buf)) mix_at(a.next, (const int16_t *)gptr(buf), (int)n, a.fmt == 0, lvol, rvol);
	static bool dump = getenv("SSX_AUDIO_DUMP") != nullptr;
	if (dump && buf && gvalid(buf)) {   // raw s16 per channel: ch<N>.raw
		static FILE *files[9];
		if (!files[ch]) { char nm[32]; snprintf(nm, sizeof(nm), "ch%u.raw", ch); files[ch] = fopen(nm, "wb"); }
		fwrite(gptr(buf), 1, (size_t)n * (a.fmt == 0 ? 4 : 2), files[ch]);
	}
	a.next += n;
	RET(n);
}
HLE(sceAudio, sceAudioOutputBlocking) { audio_output(c, ARG(0), ARG(1), ARG(1), ARG(2), true); }
HLE(sceAudio, sceAudioOutputPannedBlocking) { audio_output(c, ARG(0), ARG(1), ARG(2), ARG(3), true); }
HLE(sceAudio, sceAudioOutputPanned) { audio_output(c, ARG(0), ARG(1), ARG(2), ARG(3), false); }
// Output2: one extra stereo 44.1 kHz channel with a single volume (sceAudioOutput2*).
static const u32 OUT2 = 8;
HLE(sceAudio, sceAudioOutput2Reserve) {
	if (s_chans[OUT2].reserved) { RET(0x80260002); return; }
	s_chans[OUT2] = AudioChan{};
	s_chans[OUT2].reserved = true;
	s_chans[OUT2].samples = ARG(0);
	rt_log(LOG_AUDIO, "Output2Reserve(%u)", ARG(0));
	RET(0);
}
HLE(sceAudio, sceAudioOutput2Release) { s_chans[OUT2].reserved = false; RET(0); }
HLE(sceAudio, sceAudioOutput2ChangeLength) { s_chans[OUT2].samples = ARG(0); RET(0); }
HLE(sceAudio, sceAudioOutput2OutputBlocking) { audio_output(c, OUT2, ARG(0), ARG(0), ARG(1), true); }
HLE(sceAudio, sceAudioOutput2GetRestSample) {
	s64 rest = s_chans[OUT2].next - (now_frame() + LATENCY);
	RET(rest > 0 ? (u32)rest : 0);
}

HLE(sceAudio, sceAudioGetChannelRestLength) {
	u32 ch = ARG(0);
	if (ch >= 8) { RET(0x80260008); return; }
	s64 rest = s_chans[ch].next - (now_frame() + LATENCY);
	RET(rest > 0 ? (u32)rest : 0);
}

// ================================================================ sceAtrac3plus
static const u32 ATRAC_ALL_DATA_DECODED = 0x80630024, ATRAC_BUFFER_IS_EMPTY = 0x80630023,
                 ATRAC_BAD_ID = 0x80630005, ATRAC_UNKNOWN_FORMAT = 0x80630006, ATRAC_NO_ID = 0x80630003,
                 ATRAC_ADD_TOO_BIG = 0x80630018;

struct Atrac {
	bool used = false;
	u32 bufAddr = 0, bufSize = 0;
	std::vector<u8> file;          // file bytes as they arrive (file.size() = fileSize)
	u32 fileSize = 0;
	u32 dataOff = 0, dataSize = 0; // 'data' chunk
	u32 blockAlign = 0, channels = 2;
	u32 totalSamples = 0, sampleOffset = 0;  // from 'fact'
	bool allLoaded = false;        // the whole file sits in the buffer: no streaming
	u32 streamPos = 0;             // file offset of the next byte the game should add
	u32 ringWrite = 0;             // where in the buffer it goes
	u32 avail = 0;                 // streamed bytes not decoded yet
	u32 readPos = 0;               // file offset of the next frame
	u32 skip = 0;                  // samples to drop from the next decoded frame
	s64 decodedSamples = 0;        // play position (samples)
	u32 resetPos = 0;              // file offset GetBufferInfoForResetting asked for
	int loopNum = 0;
	bool at3plus = true;
	AVCodecContext *dec = nullptr;
	AVFrame *frame = nullptr;
	u32 dataEnd() const { return dataOff + dataSize; }
	u32 spf() const { return at3plus ? 2048 : 1024; }
};
static Atrac s_atrac[6];

static void atrac_free(Atrac &a) {
	if (a.dec) avcodec_free_context(&a.dec);
	if (a.frame) av_frame_free(&a.frame);
	a = Atrac{};
}

static bool atrac_parse_header(Atrac &a, const u8 *d, u32 n) {
	auto rd32 = [&](u32 o) { u32 v; memcpy(&v, d + o, 4); return v; };
	auto rd16 = [&](u32 o) { u16 v; memcpy(&v, d + o, 2); return (u32)v; };
	if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
	a.fileSize = rd32(4) + 8;
	u32 o = 12;
	while (o + 8 <= n) {
		u32 sz = rd32(o + 4);
		if (!memcmp(d + o, "fmt ", 4)) {
			u32 tag = rd16(o + 8);
			a.channels = rd16(o + 10);
			a.blockAlign = rd16(o + 20);
			a.at3plus = tag == 0xFFFE;
			if (!a.at3plus && tag != 0x270) return false;
		} else if (!memcmp(d + o, "fact", 4)) {
			a.totalSamples = rd32(o + 8);
			if (sz >= 8) a.sampleOffset = rd32(o + 12);
		} else if (!memcmp(d + o, "data", 4)) {
			a.dataOff = o + 8;
			a.dataSize = sz;
			break;
		}
		o += 8 + sz + (sz & 1);
	}
	if (!a.dataOff || !a.blockAlign) return false;
	a.fileSize = std::max(a.fileSize, a.dataOff + a.dataSize);
	return true;
}

static bool atrac_open_codec(Atrac &a) {
	const AVCodec *codec = avcodec_find_decoder(a.at3plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
	if (!codec) return false;
	a.dec = avcodec_alloc_context3(codec);
	a.dec->sample_rate = RATE;
	av_channel_layout_default(&a.dec->ch_layout, (int)a.channels);
	a.dec->block_align = (int)a.blockAlign;
	if (!a.at3plus) {
		// ATRAC3 needs the 14-byte codec extradata from the fmt chunk; joint stereo default.
		static const u8 extra[14] = {1, 0, 0x44, 0xAC, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
		a.dec->extradata = (u8 *)av_mallocz(14 + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(a.dec->extradata, extra, 14);
		a.dec->extradata_size = 14;
	}
	if (avcodec_open2(a.dec, codec, nullptr) < 0) return false;
	a.frame = av_frame_alloc();
	return true;
}

static Atrac *atrac_get(u32 id) {
	return id < 6 && s_atrac[id].used ? &s_atrac[id] : nullptr;
}

// Common to SetDataAndGetID / SetHalfwayBufferAndGetID: the buffer holds the first
// `got` bytes of the file and is `size` bytes long.
static void atrac_set_data(CPUState *c, u32 buf, u32 got, u32 size, const char *what) {
	int id = -1;
	for (int i = 0; i < 6; i++) if (!s_atrac[i].used) { id = i; break; }
	if (id < 0) { RET(ATRAC_NO_ID); return; }
	Atrac &a = s_atrac[id];
	atrac_free(a);
	if (!atrac_parse_header(a, gptr(buf), got) || !atrac_open_codec(a)) {
		atrac_free(a);
		rt_log(LOG_AUDIO, "%s(%08x, %x): unknown format", what, buf, got);
		RET(ATRAC_UNKNOWN_FORMAT);
		return;
	}
	a.used = true;
	a.bufAddr = buf;
	a.bufSize = std::max(size, got);
	a.file.assign(a.fileSize, 0);
	got = std::min(got, a.fileSize);
	memcpy(a.file.data(), gptr(buf), got);
	a.allLoaded = got >= a.fileSize;
	a.streamPos = got;
	a.ringWrite = got % a.bufSize;
	a.readPos = a.dataOff;
	a.avail = got > a.dataOff ? got - a.dataOff : 0;
	a.skip = a.sampleOffset % a.spf();   // encoder delay
	rt_log(LOG_AUDIO, "%s(%08x, %x/%x) -> %d: %s %u ch, frame %u B, file %u B, %u samples", what,
	       buf, got, size, id, a.at3plus ? "ATRAC3+" : "ATRAC3", a.channels, a.blockAlign, a.fileSize, a.totalSamples);
	RET(id);
}
HLE(sceAtrac3plus, sceAtracSetDataAndGetID) { atrac_set_data(c, ARG(0), ARG(1), ARG(1), "AtracSetDataAndGetID"); }
HLE(sceAtrac3plus, sceAtracSetHalfwayBufferAndGetID) {
	atrac_set_data(c, ARG(0), ARG(1), ARG(2), "AtracSetHalfwayBufferAndGetID");
}

HLE(sceAtrac3plus, sceAtracReleaseAtracID) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	atrac_free(*a);
	RET(0);
}

// The buffer is a ring: the game adds file bytes at ringWrite (read from file offset
// streamPos); decoding frees them. A looping stream continues at the data start.
static void stream_info(const Atrac &a, u32 &writePtr, u32 &writable, u32 &readOffset) {
	readOffset = a.streamPos;
	writePtr = a.bufAddr + a.ringWrite;
	if (a.allLoaded || a.streamPos >= a.dataEnd()) { writable = 0; return; }
	u32 freeBytes = a.bufSize > a.avail ? a.bufSize - a.avail : 0;
	writable = std::min({freeBytes, a.bufSize - a.ringWrite, a.dataEnd() - a.streamPos});
}

HLE(sceAtrac3plus, sceAtracGetStreamDataInfo) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 wp, wb, ro;
	stream_info(*a, wp, wb, ro);
	static int n1; if (getenv("SSX_ATRACDBG") && n1++ < 40) rt_log(LOG_AUDIO, "GetStreamDataInfo -> wp %08x writable %x readOff %x (avail %x readPos %x)", wp, wb, ro, a->avail, a->readPos);
	if (ARG(1)) SW(ARG(1), wp);
	if (ARG(2)) SW(ARG(2), wb);
	if (ARG(3)) SW(ARG(3), ro);
	RET(0);
}

static void stream_add(Atrac &a, u32 from, u32 n) {
	n = std::min(n, a.fileSize - std::min(a.streamPos, a.fileSize));
	memcpy(a.file.data() + a.streamPos, gptr(from), n);
	a.streamPos += n;
	a.avail += n;
	a.ringWrite = (a.ringWrite + n) % a.bufSize;
	if (a.streamPos >= a.dataEnd() && a.loopNum != 0) a.streamPos = a.dataOff;
}

HLE(sceAtrac3plus, sceAtracAddStreamData) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 wp, wb, ro;
	stream_info(*a, wp, wb, ro);
	u32 n = ARG(1);
	static int n2; if (getenv("SSX_ATRACDBG") && n2++ < 40) rt_log(LOG_AUDIO, "AddStreamData(%x) writable %x", n, wb);
	if (n > wb) { RET(ATRAC_ADD_TOO_BIG); return; }
	stream_add(*a, wp, n);
	RET(0);
}

static int remain_frames(const Atrac &a) {
	if (a.allLoaded) return -1;   // everything is loaded
	return (int)(a.avail / a.blockAlign);
}

// Seeking: the file offset of the frame holding `sample`, and how far into it.
static u32 frame_of_sample(const Atrac &a, u32 sample, u32 &skip) {
	u32 s = sample + a.sampleOffset;
	skip = s % a.spf();
	return a.dataOff + (s / a.spf()) * a.blockAlign;
}

// (id, sample, *info): where to put the file data for a seek. info = two
// {writePtr, writableBytes, minWriteBytes, filePos} records; we use the first.
HLE(sceAtrac3plus, sceAtracGetBufferInfoForResetting) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 sample = ARG(1), info = ARG(2), skip;
	if (sample > a->totalSamples) { RET(0x80630015); return; }   // ATRAC_BAD_SAMPLE
	a->resetPos = frame_of_sample(*a, sample, skip);
	if (info) {
		bool all = a->allLoaded;
		SW(info + 0, a->bufAddr);
		SW(info + 4, all ? 0 : std::min(a->bufSize, a->fileSize - a->resetPos));
		SW(info + 8, all ? 0 : a->blockAlign * 2);
		SW(info + 12, all ? 0 : a->resetPos);
		for (u32 k = 16; k < 32; k += 4) SW(info + k, 0);
		if (!all) SW(info + 16, a->bufAddr);
	}
	RET(0);
}

// (id, sample, bytesWrittenFirst, bytesWrittenSecond)
HLE(sceAtrac3plus, sceAtracResetPlayPosition) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 sample = ARG(1), skip;
	u32 pos = frame_of_sample(*a, sample, skip);
	if (!a->allLoaded) {
		a->streamPos = pos;
		a->ringWrite = 0;
		a->avail = 0;
		stream_add(*a, a->bufAddr, ARG(2));
	}
	a->readPos = pos;
	a->skip = skip;
	a->decodedSamples = sample;
	avcodec_flush_buffers(a->dec);
	rt_log(LOG_AUDIO, "AtracResetPlayPosition(%u, sample %u, %x bytes) -> file %x", ARG(0), sample, ARG(2), pos);
	RET(0);
}

HLE(sceAtrac3plus, sceAtracGetNextDecodePosition) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	if (a->decodedSamples >= a->totalSamples && a->loopNum == 0) { RET(ATRAC_ALL_DATA_DECODED); return; }
	if (ARG(1)) SW(ARG(1), (u32)a->decodedSamples);
	RET(0);
}

// (id, *endSample, *loopStart, *loopEnd): no loop points in the files we play
HLE(sceAtrac3plus, sceAtracGetSoundSample) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	if (ARG(1)) SW(ARG(1), a->totalSamples ? a->totalSamples - 1 : 0);
	if (ARG(2)) SW(ARG(2), 0xFFFFFFFF);
	if (ARG(3)) SW(ARG(3), 0xFFFFFFFF);
	RET(0);
}

// kbps, computed like the PSP (PPSSPP sceAtrac.cpp)
HLE(sceAtrac3plus, sceAtracGetBitrate) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 rate = a->blockAlign * 352800 / 1000;
	rate = a->at3plus ? ((rate >> 11) + 8) & 0xFFFFFFF0 : (rate + 511) >> 10;
	if (ARG(1)) SW(ARG(1), rate);
	RET(0);
}

HLE(sceAtrac3plus, sceAtracDecodeData) {
	// (id, outbuf, *samples, *finish, *remainFrames)
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	u32 out = ARG(1);
	auto finish = [&](u32 samples, u32 fin) {
		if (ARG(2)) SW(ARG(2), samples);
		if (ARG(3)) SW(ARG(3), fin);
		if (ARG(4)) SW(ARG(4), (u32)remain_frames(*a));
	};
	if (a->readPos + a->blockAlign > a->dataEnd()) {
		if (a->loopNum != 0) {
			if (a->loopNum > 0) a->loopNum--;
			a->readPos = a->dataOff;
			a->decodedSamples = 0;
			a->skip = a->sampleOffset % a->spf();
		} else {
			finish(0, 1);
			RET(ATRAC_ALL_DATA_DECODED);
			return;
		}
	}
	if (!a->allLoaded && a->avail < a->blockAlign) {
		static int n4; if (getenv("SSX_ATRACDBG") && n4++ < 40) rt_log(LOG_AUDIO, "DecodeData: buffer empty readPos %x avail %x", a->readPos, a->avail);
		finish(0, 0);
		RET(ATRAC_BUFFER_IS_EMPTY);
		return;
	}
	AVPacket *pkt = av_packet_alloc();
	pkt->data = a->file.data() + a->readPos;
	pkt->size = (int)a->blockAlign;
	a->readPos += a->blockAlign;
	if (!a->allLoaded) a->avail -= a->blockAlign;
	u32 samples = 0;
	if (avcodec_send_packet(a->dec, pkt) == 0 && avcodec_receive_frame(a->dec, a->frame) == 0) {
		samples = (u32)a->frame->nb_samples;
		int ch = a->frame->ch_layout.nb_channels;
		int16_t *dst = out && gvalid(out) ? (int16_t *)gptr(out) : nullptr;
		u32 skip = std::min(a->skip, samples);
		for (u32 i = skip; dst && i < samples; i++)
			for (int k = 0; k < 2; k++) {
				const float *src = (const float *)a->frame->extended_data[std::min(k, ch - 1)];
				dst[2 * (i - skip) + k] = (int16_t)std::clamp((int)lrintf(src[i] * 32767.0f), -32768, 32767);
			}
		samples -= skip;
		a->skip = 0;
	}
	av_packet_free(&pkt);
	a->decodedSamples += samples;
	static FILE *dec = getenv("SSX_ATRAC_DUMP") ? fopen("atrac_dec.raw", "wb") : nullptr;
	if (dec && out) fwrite(gptr(out), 1, samples * 4, dec);
	static int n3; if (getenv("SSX_ATRACDBG") && n3++ < 60) rt_log(LOG_AUDIO, "DecodeData -> %u samples, readPos %x avail %x remain %d", samples, a->readPos, a->avail, remain_frames(*a));
	bool last = a->readPos + a->blockAlign > a->dataEnd() && a->loopNum == 0;
	finish(samples, last ? 1 : 0);
	RET(0);
}

HLE(sceAtrac3plus, sceAtracGetMaxSample) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	if (ARG(1)) SW(ARG(1), a->at3plus ? 2048 : 1024);
	RET(0);
}
HLE(sceAtrac3plus, sceAtracGetNextSample) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	bool done = a->readPos + a->blockAlign > a->dataOff + a->dataSize && a->loopNum == 0;
	if (ARG(1)) SW(ARG(1), done ? 0 : (a->at3plus ? 2048 : 1024));
	RET(0);
}
HLE(sceAtrac3plus, sceAtracSetLoopNum) {
	Atrac *a = atrac_get(ARG(0));
	if (!a) { RET(ATRAC_BAD_ID); return; }
	a->loopNum = (int)ARG(1);
	RET(0);
}
