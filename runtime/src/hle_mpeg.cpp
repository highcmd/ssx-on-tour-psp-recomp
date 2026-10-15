// sceMpeg: PSMF movie playback with FFmpeg (H.264). Semantics follow PPSSPP's
// sceMpeg.cpp (public, GPL): the ringbuffer is filled by a guest callback during
// sceMpegRingbufferPut, GetAvcAu peeks the next access unit, AvcDecode consumes
// it and writes RGBA pixels into the game's buffer. Movie audio is silent for now.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "kernel.h"

static const u32 MPEG_NO_DATA = 0x80618001;
static const u32 MPEG_AVC_DECODE_FATAL = 0x80628002;

// Ringbuffer layout (see PPSSPP SceMpegRingBuffer)
enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PKTSIZE = 16, RB_DATA = 20, RB_CB = 24,
       RB_CBARG = 28, RB_UPPER = 32, RB_SEMA = 36, RB_MPEG = 40 };

struct VideoAU {
	std::vector<u8> data;
	s64 pts;
};

struct MpegCtx {
	u32 ringbuffer = 0;
	u32 pixelMode = 3;
	std::vector<u8> ps;          // undemuxed program stream bytes
	std::vector<u8> videoES;     // video elementary stream not yet split
	s64 lastPts = 0;
	std::deque<VideoAU> aus;
	AVCodecContext *dec = nullptr;
	AVCodecParserContext *parser = nullptr;
	AVFrame *frame = nullptr;
	SwsContext *sws = nullptr;
	int frames = 0;
	int audioAus = 0;
};
static std::map<u32, MpegCtx> s_mpegs;   // keyed by mpeg handle address (guest)
static struct QuietFFmpeg { QuietFFmpeg() { av_log_set_level(AV_LOG_ERROR); } } s_quiet;

static MpegCtx *ctx_for(u32 mpeg) {
	auto it = s_mpegs.find(mpeg);
	return it == s_mpegs.end() ? nullptr : &it->second;
}

static void ctx_free(MpegCtx &m) {
	if (m.parser) av_parser_close(m.parser);
	if (m.dec) avcodec_free_context(&m.dec);
	if (m.frame) av_frame_free(&m.frame);
	if (m.sws) sws_freeContext(m.sws);
	m = MpegCtx{};
}

static s64 read_pts(const u8 *p) {
	return ((s64)((p[0] >> 1) & 7) << 30) | ((s64)p[1] << 22) | ((s64)(p[2] >> 1) << 15) | ((s64)p[3] << 7) | (p[4] >> 1);
}

// Split complete video ES into access units with the H.264 parser.
static void parse_video(MpegCtx &m, bool flush) {
	if (!m.parser) {
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		m.parser = av_parser_init(AV_CODEC_ID_H264);
		m.dec = avcodec_alloc_context3(codec);
		avcodec_open2(m.dec, codec, nullptr);
		m.frame = av_frame_alloc();
	}
	size_t off = 0;
	while (off < m.videoES.size() || flush) {
		u8 *out = nullptr;
		int outSize = 0;
		int used = av_parser_parse2(m.parser, m.dec, &out, &outSize, m.videoES.data() + off,
		                            (int)(m.videoES.size() - off), m.lastPts, m.lastPts, 0);
		off += used;
		if (outSize > 0) m.aus.push_back({std::vector<u8>(out, out + outSize), m.lastPts});
		if (used == 0 && outSize == 0) break;
		if (flush && off >= m.videoES.size() && outSize == 0) break;
	}
	m.videoES.erase(m.videoES.begin(), m.videoES.begin() + off);
}

// Minimal MPEG-2 program stream demuxer: pack headers, system headers, PES.
static void demux(MpegCtx &m) {
	std::vector<u8> &d = m.ps;
	size_t i = 0;
	while (i + 6 <= d.size()) {
		if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) { i++; continue; }
		u8 id = d[i + 3];
		if (id == 0xBA) {  // pack header (MPEG-2)
			if (i + 14 > d.size()) break;
			i += 14 + (d[i + 13] & 7);
			continue;
		}
		if (id == 0xB9) { i += 4; continue; }  // end code
		if (id < 0xBB) { i++; continue; }
		u32 len = (d[i + 4] << 8) | d[i + 5];
		if (i + 6 + len > d.size()) break;  // incomplete packet
		const u8 *p = &d[i + 6];
		if ((id >= 0xE0 && id <= 0xEF) || id == 0xBD) {
			u32 hdrLen = 3 + p[2];
			if ((p[1] & 0x80) && hdrLen >= 8) m.lastPts = id == 0xBD ? m.lastPts : read_pts(p + 3);
			if (id >= 0xE0) {
				m.videoES.insert(m.videoES.end(), p + hdrLen, p + len);
			} else {
				m.audioAus++;
			}
		}
		i += 6 + len;
	}
	d.erase(d.begin(), d.begin() + i);
	parse_video(m, false);
}

// Call a guest function from HLE code on the current thread, preserving registers.
static u32 call_guest(CPUState *c, u32 func, u32 a0, u32 a1, u32 a2) {
	u32 saved[32];
	memcpy(saved, c->r, sizeof(saved));
	c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
	c->r[29] -= 0x40;
	rt_call(c, func);
	u32 ret = c->r[2];
	memcpy(c->r, saved, sizeof(saved));
	return ret;
}

HLE(sceMpeg, sceMpegInit) { RET(0); }
HLE(sceMpeg, sceMpegFinish) { RET(0); }
HLE(sceMpeg, sceMpegQueryMemSize) { RET(0x10000); }
HLE(sceMpeg, sceMpegRingbufferQueryMemSize) { RET(ARG(0) * (2048 + 104)); }

HLE(sceMpeg, sceMpegRingbufferConstruct) {
	u32 rb = ARG(0), n = ARG(1), data = ARG(2);
	SW(rb + RB_PACKETS, n);
	SW(rb + RB_READ, 0);
	SW(rb + RB_WRITEPOS, 0);
	SW(rb + RB_AVAIL, 0);
	SW(rb + RB_PKTSIZE, 2048);
	SW(rb + RB_DATA, data);
	SW(rb + RB_CB, ARG(4));
	SW(rb + RB_CBARG, ARG(5));
	SW(rb + RB_UPPER, data + n * 2048);
	SW(rb + RB_SEMA, (u32)-1);
	SW(rb + RB_MPEG, 0);
	RET(0);
}
HLE(sceMpeg, sceMpegRingbufferDestruct) { RET(0); }

HLE(sceMpeg, sceMpegRingbufferAvailableSize) {
	u32 rb = ARG(0);
	RET(LW(rb + RB_PACKETS) - LW(rb + RB_AVAIL));
}

HLE(sceMpeg, sceMpegCreate) {
	u32 mpeg = ARG(0), data = ARG(1), rb = ARG(3);
	if (s_mpegs.count(mpeg)) ctx_free(s_mpegs[mpeg]);
	MpegCtx &m = s_mpegs[mpeg];
	m.ringbuffer = rb;
	if (rb) {
		SW(rb + RB_AVAIL, 0);
		SW(rb + RB_MPEG, mpeg);
	}
	u32 h = data + 0x30;
	SW(mpeg, h);
	memcpy(gptr(h), "LIBMPEG\0001\0", 12);
	SW(h + 12, (u32)-1);
	SW(h + 16, rb);
	SW(h + 20, rb ? LW(rb + RB_UPPER) : 0);
	rt_log(LOG_MISC, "MpegCreate(%08x, rb %08x, frameWidth %u)", mpeg, rb, ARG(4));
	RET(0);
}

HLE(sceMpeg, sceMpegDelete) {
	auto it = s_mpegs.find(ARG(0));
	if (it != s_mpegs.end()) { ctx_free(it->second); s_mpegs.erase(it); }
	RET(0);
}

HLE(sceMpeg, sceMpegRingbufferPut) {
	u32 rb = ARG(0);
	int num = std::min((int)ARG(1), (int)ARG(2));
	int packets = (int)LW(rb + RB_PACKETS);
	num = std::min(num, packets - (int)LW(rb + RB_AVAIL));
	MpegCtx *m = ctx_for(LW(rb + RB_MPEG));
	if (num <= 0 || !m) { RET(0); return; }
	u32 cb = LW(rb + RB_CB), data = LW(rb + RB_DATA), cbarg = LW(rb + RB_CBARG);
	int total = 0;
	while (num > 0) {
		int wpos = (int)LW(rb + RB_WRITEPOS) % packets;
		int round = std::min(num, packets - wpos);
		u32 dst = data + wpos * 2048;
		int got = (int)call_guest(c, cb, dst, round, cbarg);
		if (got <= 0) break;
		m->ps.insert(m->ps.end(), gptr(dst), gptr(dst) + got * 2048);
		SW(rb + RB_WRITEPOS, LW(rb + RB_WRITEPOS) + got);
		SW(rb + RB_READ, LW(rb + RB_READ) + got);
		total += got;
		num -= round;
		if (got < round) break;
	}
	demux(*m);
	// Everything put is demuxed at once, so the ring is always free again.
	SW(rb + RB_AVAIL, 0);
	RET(total);
}

HLE(sceMpeg, sceMpegQueryStreamOffset) {
	u32 buf = ARG(1);
	const u8 *h = gptr(buf);
	if (memcmp(h, "PSMF", 4) != 0) { RET(0x806101FE); return; }
	u32 off = (h[8] << 24) | (h[9] << 16) | (h[10] << 8) | h[11];
	SW(ARG(2), off);
	RET(0);
}
HLE(sceMpeg, sceMpegQueryStreamSize) {
	const u8 *h = gptr(ARG(0));
	u32 size = (h[12] << 24) | (h[13] << 16) | (h[14] << 8) | h[15];
	SW(ARG(1), size);
	RET(0);
}

static int s_stream_id = 1;
HLE(sceMpeg, sceMpegRegistStream) { RET(s_stream_id++); }
HLE(sceMpeg, sceMpegUnRegistStream) { RET(0); }
HLE(sceMpeg, sceMpegMallocAvcEsBuf) { RET(1); }
HLE(sceMpeg, sceMpegFreeAvcEsBuf) { RET(0); }
HLE(sceMpeg, sceMpegInitAu) {
	u32 au = ARG(2);
	SW(au + 0, (u32)-1); SW(au + 4, (u32)-1);
	SW(au + 8, (u32)-1); SW(au + 12, (u32)-1);
	SW(au + 16, ARG(1));
	SW(au + 20, 0);
	RET(0);
}
HLE(sceMpeg, sceMpegQueryAtracEsSize) { SW(ARG(1), 2112); SW(ARG(2), 8192); RET(0); }

static void write_pts(u32 au, s64 pts) {
	// SceMpegAu: pts/dts are stored high word first (PPSSPP SceMpegAu::write)
	SW(au + 0, (u32)(pts >> 32)); SW(au + 4, (u32)pts);
	s64 dts = pts - 3003;
	SW(au + 8, (u32)(dts >> 32)); SW(au + 12, (u32)dts);
}

HLE(sceMpeg, sceMpegGetAvcAu) {
	MpegCtx *m = ctx_for(ARG(0));
	if (!m) { RET(-1); return; }
	if (m->aus.empty()) parse_video(*m, LW(m->ringbuffer + RB_READ) > 0 && m->ps.empty());
	if (m->aus.empty()) {
		write_pts(ARG(2), -1);
		kernel_wait(WAIT_HLE, 0, 1000, false);  // give the streamer a chance
		RET(MPEG_NO_DATA);
		return;
	}
	write_pts(ARG(2), m->aus.front().pts);
	SW(ARG(2) + 20, (u32)m->aus.front().data.size());
	if (ARG(3)) SW(ARG(3), 1);
	RET(0);
}

HLE(sceMpeg, sceMpegAvcDecode) {
	MpegCtx *m = ctx_for(ARG(0));
	u32 frameWidth = ARG(2) ? ARG(2) : 512;
	u32 buffer = LW(ARG(3));
	u32 initAddr = ARG(4);
	if (!m || m->aus.empty()) { RET(MPEG_AVC_DECODE_FATAL); return; }
	VideoAU au = std::move(m->aus.front());
	m->aus.pop_front();
	AVPacket *pkt = av_packet_alloc();
	pkt->data = au.data.data();
	pkt->size = (int)au.data.size();
	int status = 0;
	if (avcodec_send_packet(m->dec, pkt) == 0 && avcodec_receive_frame(m->dec, m->frame) == 0) {
		int w = m->frame->width, h = m->frame->height;
		m->sws = sws_getCachedContext(m->sws, w, h, (AVPixelFormat)m->frame->format, w, h, AV_PIX_FMT_RGBA,
		                              SWS_BILINEAR, nullptr, nullptr, nullptr);
		u8 *dst[1] = {gptr(buffer)};
		int stride[1] = {(int)frameWidth * 4};
		sws_scale(m->sws, m->frame->data, m->frame->linesize, 0, h, dst, stride);
		status = 1;
		if (m->frames++ == 0) rt_log(LOG_MISC, "movie: first frame %dx%d -> %08x", w, h, buffer);
	}
	av_packet_free(&pkt);
	write_pts(ARG(1), au.pts);
	if (initAddr) SW(initAddr, status);
	RET(0);
}

HLE(sceMpeg, sceMpegAvcDecodeDetail) { RET(0); }

HLE(sceMpeg, sceMpegGetAtracAu) {
	MpegCtx *m = ctx_for(ARG(0));
	if (!m || m->aus.empty()) { RET(MPEG_NO_DATA); return; }
	write_pts(ARG(2), m->aus.front().pts);
	if (ARG(3)) SW(ARG(3), 0);
	RET(0);
}

HLE(sceMpeg, sceMpegAtracDecode) {
	if (gvalid(ARG(2))) memset(gptr(ARG(2)), 0, 8192);
	RET(0);
}
