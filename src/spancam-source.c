/*
Spancam OBS Studio plugin — "Spancam Camera" video source
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

An OBS async video source fed by a Spancam phone over SDSP (docs/PROTOCOL.md).
A worker thread owns the socket: connect, handshake, read the StreamHeader, then
pump packets until the far end goes away, then reconnect. Each access unit goes
to libavcodec (which OBS already ships via obs-deps) and the decoded planes go
straight to obs_source_output_video — no swscale, OBS takes planar YUV as-is.
Frames are paced on the encoder's PTS rather than on arrival, so OBS's async
buffer is the jitter buffer.

Decode path follows obs-studio's own plugins/win-dshow/ffmpeg-decode.c.
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/bmem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h> // VideoToolbox decode, via the generic hwaccel API

#define SPANCAM_DEFAULT_PORT 8892
#define SPANCAM_DISCOVERY_PORT 8891
#define SPANCAM_PROBE "SPANCAM-DISCOVER"
#define SPANCAM_MAGIC 0x53504331u // 'SPC1'
#define SPANCAM_RECONNECT_MS 1500
#define SPANCAM_RECV_TIMEOUT_MS 500

// Connection mode (the "Connection" dropdown).
#define SPANCAM_CONN_AUTO 0
#define SPANCAM_CONN_USB 1
#define SPANCAM_CONN_WIFI 2

struct spancam_source {
	obs_source_t *source;

	pthread_t thread;
	os_event_t *stop_signal;
	bool thread_running;

	pthread_mutex_t cfg_lock;
	int connection; // SPANCAM_CONN_*
	char *host;     // blank = find a phone by broadcast
	int port;
	char *token;
	int rotation; // 0/90/180/270, applied here — the phone never resamples pixels
	int mirror;   // 0/1 horizontal flip, composed with rotation

	// Reusable output buffer for the flipped/rotated frame. Grown as needed.
	uint8_t *rotbuf;
	size_t rotcap;

	// PTS pacing: map the wire pts_us onto a monotonic OBS timebase so OBS's async
	// source buffer can absorb network jitter, instead of presenting each frame the
	// instant it happens to arrive. Re-anchored per connection.
	bool ts_base_set;
	int64_t ts_base_pts_us;
	int64_t ts_base_obs_ns;

	// Upstream control channel (plugin -> phone). Single writer: the receive thread.
	int ctrl_fd;                  // socket to send control frames on (-1 = none)
	int abr_target;               // bitrate the phone is currently being asked for
	int abr_ceiling;              // StreamHeader bitrate — never ask for more than this
	bool abr_base_set;
	int64_t abr_base_arrival_ns;  // anchor for the one-way delay measurement
	int64_t abr_base_pts_us;
	int64_t abr_delay_ewma_ns;    // smoothed queuing delay since the anchor
	int64_t abr_last_send_ns;     // rate-limit control frames to ~1 Hz
	int64_t last_kf_request_ns;   // debounce keyframe requests — no IDR storms

	// Auto-mode USB cooldown. A USB connection that CONNECTS but never produces a
	// valid stream (flaky wireless-adb tunnel, a forward pointing at the wrong
	// device, app not discoverable) would otherwise loop forever: connect, no
	// header, disconnect, connect... Wi-Fi never gets a look in because the connect
	// itself keeps succeeding. So Auto benches USB until this deadline. Cleared the
	// moment USB does yield a header, so USB stays the preference. Auto only —
	// explicit USB mode never cools down, because the user asked for USB. (os_gettime_ns)
	int64_t usb_cooldown_until_ns;

	// Decoder (rebuilt per connection from the StreamHeader codec byte).
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;

	// VideoToolbox decode on macOS. hw_decode = false means the pure software
	// path, either because this isn't Apple or because some step below failed and
	// we fell back. sw_frame is the reusable NV12 download target.
	AVBufferRef *hw_device_ctx;
	AVFrame *sw_frame;
	bool hw_decode;
};

// --------------------------------------------------------------------------
// Socket helpers (POSIX for now; Windows will need winsock shims).
// --------------------------------------------------------------------------

static int spancam_connect(const char *host, int port)
{
	char portstr[8];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return -1;

	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd >= 0) {
		// A read timeout lets the receive loop notice the stop signal even
		// while the phone is silent.
		struct timeval tv = {.tv_sec = SPANCAM_RECV_TIMEOUT_MS / 1000,
				     .tv_usec = (SPANCAM_RECV_TIMEOUT_MS % 1000) * 1000};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

// Read exactly len bytes; returns true on success, false on EOF/error/stop.
static bool spancam_read_full(struct spancam_source *ctx, int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		if (os_event_try(ctx->stop_signal) != EAGAIN)
			return false;
		ssize_t n = recv(fd, buf + got, len - got, 0);
		if (n > 0) {
			got += (size_t)n;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			continue; // timeout tick — re-check stop, keep reading
		} else {
			return false; // EOF or hard error
		}
	}
	return true;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

// --------------------------------------------------------------------------
// Upstream control channel (plugin -> phone).
//
// The stream socket is bidirectional and the phone already speaks a
// [type:u32 BE][len:u32 BE][payload] control framing, so there's no second
// connection and no new parser on the phone side. Written only from the receive
// thread, which is the only thread that has a live fd.
// --------------------------------------------------------------------------
static void spancam_send_control(struct spancam_source *ctx, uint32_t type, const uint8_t *payload, uint32_t len)
{
	if (ctx->ctrl_fd < 0)
		return;
	uint8_t hdr[8] = {
		(uint8_t)(type >> 24), (uint8_t)(type >> 16), (uint8_t)(type >> 8), (uint8_t)type,
		(uint8_t)(len >> 24),  (uint8_t)(len >> 16),  (uint8_t)(len >> 8),  (uint8_t)len,
	};
	if (send(ctx->ctrl_fd, hdr, sizeof(hdr), 0) < 0)
		return;
	if (len > 0 && payload)
		send(ctx->ctrl_fd, payload, len, 0);
}

static void spancam_send_keyframe(struct spancam_source *ctx)
{
	spancam_send_control(ctx, 0x34, NULL, 0); // requestKeyFrame, empty payload
}

static void spancam_send_bitrate(struct spancam_source *ctx, int bps)
{
	uint8_t p[5] = {(uint8_t)(bps >> 24), (uint8_t)(bps >> 16), (uint8_t)(bps >> 8), (uint8_t)bps, 0};
	spancam_send_control(ctx, 0x30, p, sizeof(p)); // [targetBitrate:u32][resolutionIndex:u8=0]
}

// Closed-loop bitrate control, delay-gradient style. Watch how far each frame's
// arrival drifts from where its PTS says it should have arrived: a delay that
// keeps growing means bytes are queuing somewhere on the path, which is
// congestion showing up well before packet loss does. Step down hard (x0.85) and
// climb back gently (+10%) toward the StreamHeader's bitrate, which is the
// ceiling the encoder was configured for and not ours to exceed.
//
// Rate-limited to ~1 Hz so the phone's encoder isn't reconfigured constantly, and
// re-anchored after every change so the next measurement reflects the new rate
// rather than the backlog left over from the old one.
static void spancam_abr_observe(struct spancam_source *ctx, int64_t pts_us)
{
	int64_t now = (int64_t)os_gettime_ns();
	if (!ctx->abr_base_set) {
		ctx->abr_base_set = true;
		ctx->abr_base_arrival_ns = now;
		ctx->abr_base_pts_us = pts_us;
		ctx->abr_delay_ewma_ns = 0;
	}
	int64_t expected = ctx->abr_base_arrival_ns + (pts_us - ctx->abr_base_pts_us) * 1000;
	int64_t delay = now - expected; // how far behind the grid we've fallen
	if (delay < 0)
		delay = 0;
	ctx->abr_delay_ewma_ns = (ctx->abr_delay_ewma_ns * 7 + delay) / 8;
	if (ctx->ctrl_fd < 0 || ctx->abr_ceiling <= 0)
		return;
	if (now - ctx->abr_last_send_ns < 1000000000LL) // ~1 Hz
		return;
	if (ctx->abr_delay_ewma_ns > 250000000LL) { // >250 ms of queue — congested
		ctx->abr_target = ctx->abr_target * 85 / 100;
		if (ctx->abr_target < 800000)
			ctx->abr_target = 800000;
	} else if (ctx->abr_delay_ewma_ns < 60000000LL && ctx->abr_target < ctx->abr_ceiling) {
		ctx->abr_target += ctx->abr_target / 10; // +10%
		if (ctx->abr_target > ctx->abr_ceiling)
			ctx->abr_target = ctx->abr_ceiling;
	} else {
		return; // in between — hold, keep measuring
	}
	ctx->abr_last_send_ns = now;
	spancam_send_bitrate(ctx, ctx->abr_target);
	ctx->abr_base_set = false; // re-anchor so the next reading reflects the new rate
}

// --------------------------------------------------------------------------
// Discovery (UDP broadcast) + USB (adb), so nobody has to read an IP address off
// a phone screen and type it into OBS.
// --------------------------------------------------------------------------

static void spancam_run(const char *cmd)
{
	FILE *f = popen(cmd, "r");
	if (!f)
		return;
	char buf[256];
	while (fgets(buf, sizeof(buf), f)) {
	} // drain
	pclose(f);
}

// True if `adb devices` lists at least one connected device.
static bool spancam_adb_has_device(void)
{
	FILE *f = popen("adb devices 2>/dev/null", "r");
	if (!f)
		return false;
	char line[256];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "\tdevice")) // "<serial>\tdevice"; skips the header line
			found = true;
	}
	pclose(f);
	return found;
}

// Broadcast a probe and parse the first reply.
// Reply: "SPANCAM-OBS|name|port|token|codec|w|h"; the host is the reply's SOURCE
// address, not anything the phone claims about itself — a phone behind NAT or
// with several interfaces up doesn't reliably know which of its addresses we can
// actually reach, but the packet that just arrived proves one of them.
static bool spancam_udp_discover(struct spancam_source *ctx, char *host, size_t hostsz, int *port, char *token,
				 size_t toksz)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	struct timeval tv = {.tv_sec = 0, .tv_usec = 600000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(SPANCAM_DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, SPANCAM_PROBE, sizeof(SPANCAM_PROBE) - 1, 0, (struct sockaddr *)&to, sizeof(to));

	bool ok = false;
	for (int tries = 0; tries < 4 && os_event_try(ctx->stop_signal) == EAGAIN; tries++) {
		char buf[256];
		struct sockaddr_in from = {0};
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
		if (n <= 0)
			break;
		buf[n] = 0;
		if (strncmp(buf, "SPANCAM-OBS|", 12) != 0)
			continue;
		char *fields[8] = {0};
		int nf = 0;
		for (char *p = strtok(buf, "|"); p && nf < 8; p = strtok(NULL, "|"))
			fields[nf++] = p;
		if (nf >= 4) {
			snprintf(host, hostsz, "%s", inet_ntoa(from.sin_addr));
			*port = atoi(fields[2]);
			snprintf(token, toksz, "%s", fields[3]);
			ok = true;
			break;
		}
	}
	close(fd);
	return ok;
}

// --------------------------------------------------------------------------
// FFmpeg decode -> OBS.
// --------------------------------------------------------------------------

// Rotate and/or horizontally flip one 8-bit plane, here on the computer. The
// phone deliberately never does this: it would mean a second pass over every
// frame on the device whose battery and thermal budget we care about, when this
// end is idle by comparison. src is sw x sh with stride sstride; dstride is the
// output row width, which for 90/270 is the rotated geometry.
//
// ORDER MATTERS. Rotate first with no mirror, THEN flip horizontally in DISPLAY
// space (dx = dstride - 1 - dx). Flipping the source x before rotating flips
// whichever axis the rotation happens to map it onto — at 90 and 270 that is the
// vertical one, and a vertical flip on top of a quarter turn reads to the eye as
// a 180° rotation. dstride is the post-rotation row width, so it doubles as the
// flip axis and the two cases can't drift apart.
//
// bpe is bytes per ELEMENT: 1 for an 8-bit plane (I420 Y/U/V, or NV12's Y), 2 for
// NV12's interleaved UV where one element is the whole (Cb,Cr) pair. sw/sh/sstride
// and dstride are all in ELEMENTS, not bytes. For UV the mirror moves the pair's
// position in display space; the two bytes inside it are copied in order and never
// swapped, because swapping them is how you turn NV12 into NV21. Being a pure
// permutation of samples, chroma siting comes out unchanged.
static void spancam_xform_plane(const uint8_t *src, int sw, int sh, int sstride, uint8_t *dst, int dstride, int rot,
				int mir, int bpe)
{
	for (int y = 0; y < sh; y++) {
		const uint8_t *srow = src + (size_t)y * sstride * bpe;
		for (int x = 0; x < sw; x++) {
			int dx, dy;
			if (rot == 90) {
				dx = sh - 1 - y;
				dy = x;
			} else if (rot == 180) {
				dx = sw - 1 - x;
				dy = sh - 1 - y;
			} else if (rot == 270) {
				dx = y;
				dy = sw - 1 - x;
			} else { // rot 0
				dx = x;
				dy = y;
			}
			if (mir)
				dx = dstride - 1 - dx; // h-flip in display space, AFTER rotation
			uint8_t *dpx = dst + ((size_t)dy * dstride + dx) * bpe;
			const uint8_t *spx = srow + (size_t)x * bpe;
			for (int b = 0; b < bpe; b++) // 1 byte, or the 2-byte UV pair
				dpx[b] = spx[b];
		}
	}
}

static enum video_format spancam_obs_format(enum AVPixelFormat f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

// libavcodec's get_format callback, and VideoToolbox does not work without it.
// Setting hw_device_ctx alone is not enough: the default callback only picks up
// hardware pixel formats flagged METHOD_INTERNAL, and VT is METHOD_HW_DEVICE_CTX,
// so it quietly chooses the software format instead. No error, no warning, frames
// still arrive — just as YUV420P off the CPU decoder. Returning the VT format here
// is what actually engages the hwaccel. Installed before avcodec_open2, since it
// can be called during open.
#if defined(__APPLE__)
static enum AVPixelFormat spancam_get_format(struct AVCodecContext *avctx, const enum AVPixelFormat *fmts)
{
	UNUSED_PARAMETER(avctx);
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_VIDEOTOOLBOX)
			return AV_PIX_FMT_VIDEOTOOLBOX;
	}
	return fmts[0]; // VT not on offer — take the software format and carry on
}
#endif

static bool spancam_open_decoder(struct spancam_source *ctx, uint8_t codec_byte)
{
	enum AVCodecID id = (codec_byte == 1) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	ctx->codec = avcodec_find_decoder(id);
	if (!ctx->codec) {
		obs_log(LOG_ERROR, "Spancam: no decoder for %s", codec_byte == 1 ? "HEVC" : "H.264");
		return false;
	}

	ctx->hw_decode = false;

#if defined(__APPLE__)
	// Try VideoToolbox first. Best-effort throughout: any step failing falls
	// through to the software open below, so a machine without VT — or a profile
	// it won't take — still streams. Ordering is load-bearing: create the device,
	// set hw_device_ctx AND get_format, and only then avcodec_open2.
	if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) == 0) {
		ctx->ctx = avcodec_alloc_context3(ctx->codec);
		if (ctx->ctx) {
			ctx->ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
			ctx->ctx->get_format = spancam_get_format;
			if (ctx->ctx->hw_device_ctx && avcodec_open2(ctx->ctx, ctx->codec, NULL) == 0) {
				ctx->frame = av_frame_alloc();
				ctx->sw_frame = av_frame_alloc();
				if (ctx->frame && ctx->sw_frame) {
					ctx->hw_decode = true;
					obs_log(LOG_INFO, "Spancam: VideoToolbox decode enabled");
					return true;
				}
				if (ctx->frame)
					av_frame_free(&ctx->frame);
				if (ctx->sw_frame)
					av_frame_free(&ctx->sw_frame);
			}
			avcodec_free_context(&ctx->ctx); // also drops the ref'd device copy
		}
		av_buffer_unref(&ctx->hw_device_ctx);
		obs_log(LOG_INFO, "Spancam: VideoToolbox unavailable — using software decode");
	}
#endif

	// Software path. Allocate a fresh context so a half-configured HW one is
	// never reused.
	ctx->ctx = avcodec_alloc_context3(ctx->codec);
	if (!ctx->ctx)
		return false;
	if (avcodec_open2(ctx->ctx, ctx->codec, NULL) < 0) {
		avcodec_free_context(&ctx->ctx);
		return false;
	}
	ctx->frame = av_frame_alloc();
	return ctx->frame != NULL;
}

static void spancam_close_decoder(struct spancam_source *ctx)
{
	// Order: frames, then the codec context (which drops libavcodec's ref on the
	// device), then our own master ref.
	if (ctx->frame)
		av_frame_free(&ctx->frame);
	if (ctx->sw_frame)
		av_frame_free(&ctx->sw_frame);
	if (ctx->ctx)
		avcodec_free_context(&ctx->ctx);
	av_buffer_unref(&ctx->hw_device_ctx); // NULL-safe
	ctx->codec = NULL;
	ctx->hw_decode = false;
}

// Feed one Annex-B access unit (or the codec config) to the decoder and emit
// whatever frames come back out.
static void spancam_decode(struct spancam_source *ctx, const uint8_t *data, int size, int64_t pts_us)
{
	if (!ctx->ctx)
		return;

	AVPacket *pkt = av_packet_alloc();
	pkt->data = (uint8_t *)data;
	pkt->size = size;
	pkt->pts = pts_us;

	int ret = avcodec_send_packet(ctx->ctx, pkt);
	av_packet_free(&pkt);
	if (ret < 0) {
		// Decode error / desync: ask for a fresh IDR rather than showing smeared
		// garbage until the next scheduled one. Debounced ~500ms, because a corrupt
		// burst is many failed packets in a row and each one asking for its own IDR
		// would bury an already-struggling link in keyframes.
		int64_t now = (int64_t)os_gettime_ns();
		if (now - ctx->last_kf_request_ns > 500000000LL) {
			ctx->last_kf_request_ns = now;
			spancam_send_keyframe(ctx);
		}
		return;
	}

	while (avcodec_receive_frame(ctx->ctx, ctx->frame) == 0) {
		AVFrame *f = ctx->frame;

		// With VideoToolbox the decoded surface lives on the GPU. Pull it down
		// into the reusable sw_frame (leaving the format unset lets FFmpeg pick,
		// which is NV12) and run the rest of the loop against that. A failed
		// transfer drops this one frame rather than tearing the decoder down — an
		// IOSurface hiccup shouldn't end the stream.
		if (f->format == AV_PIX_FMT_VIDEOTOOLBOX) {
			av_frame_unref(ctx->sw_frame);
			ctx->sw_frame->format = AV_PIX_FMT_NONE;
			if (av_hwframe_transfer_data(ctx->sw_frame, f, 0) < 0) {
				obs_log(LOG_WARNING, "Spancam: HW frame download failed");
				av_frame_unref(f);
				continue;
			}
			// The transfer moves pixels only — colour and timing metadata do not
			// come with them. Copy the props across before f is reassigned, since
			// the range check further down reads color_range off the frame.
			av_frame_copy_props(ctx->sw_frame, f);
			av_frame_unref(f); // the GPU surface has been copied out
			f = ctx->sw_frame;
		}

		enum video_format fmt = spancam_obs_format(f->format);
		if (fmt == VIDEO_FORMAT_NONE) {
			obs_log(LOG_WARNING, "Spancam: unsupported pixel format %d", f->format);
			av_frame_unref(f);
			continue;
		}

		struct obs_source_frame frame = {0};
		frame.format = fmt;
		// Pace on the wire PTS laid onto a monotonic OBS timebase, so OBS's async
		// buffer smooths jitter out instead of showing frames the moment they land.
		// Re-anchor on the first frame, on a PTS that goes backwards (the encoder
		// restarted) and on a wild jump forwards (a corrupt PTS shouldn't strand the
		// source seconds in the future, with every later frame dropped as too old).
		int64_t rel_ns = (pts_us - ctx->ts_base_pts_us) * 1000;
		if (!ctx->ts_base_set || pts_us < ctx->ts_base_pts_us || rel_ns > 10000000000LL) {
			ctx->ts_base_set = true;
			ctx->ts_base_pts_us = pts_us;
			ctx->ts_base_obs_ns = (int64_t)os_gettime_ns();
			rel_ns = 0;
		}
		frame.timestamp = (uint64_t)(ctx->ts_base_obs_ns + rel_ns);

		pthread_mutex_lock(&ctx->cfg_lock);
		int rot = ctx->rotation;
		int mir = ctx->mirror;
		pthread_mutex_unlock(&ctx->cfg_lock);

		if ((rot != 0 || mir) && f->width > 0 && f->height > 0 &&
		    (fmt == VIDEO_FORMAT_I420 || fmt == VIDEO_FORMAT_NV12)) {
			int w = f->width, h = f->height;
			int ow = (rot == 90 || rot == 270) ? h : w; // 90/270 swap W/H
			int oh = (rot == 90 || rot == 270) ? w : h;
			if (fmt == VIDEO_FORMAT_NV12) {
				// Y as 1-byte elements, then interleaved UV as 2-byte elements
				// over a w/2 x h/2 grid. f->linesize is in BYTES, so the UV
				// plane's ELEMENT stride is linesize[1] / 2.
				size_t ysz = (size_t)ow * oh;
				size_t uvsz = (size_t)ow * (oh / 2); // (ow/2 pairs) * 2 bytes * (oh/2 rows)
				size_t need = ysz + uvsz;
				if (need > ctx->rotcap) {
					ctx->rotbuf = brealloc(ctx->rotbuf, need);
					ctx->rotcap = need;
				}
				uint8_t *dy = ctx->rotbuf, *duv = dy + ysz;
				spancam_xform_plane(f->data[0], w, h, f->linesize[0], dy, ow, rot, mir, 1);
				spancam_xform_plane(f->data[1], w / 2, h / 2, f->linesize[1] / 2, duv, ow / 2, rot,
						    mir, 2);
				frame.width = (uint32_t)ow;
				frame.height = (uint32_t)oh;
				frame.data[0] = dy;
				frame.linesize[0] = (uint32_t)ow;
				frame.data[1] = duv;
				frame.linesize[1] = (uint32_t)ow; // (ow/2 pairs) * 2 bytes per row
			} else { // I420, the software-decode path
				size_t ysz = (size_t)ow * oh, csz = (size_t)(ow / 2) * (oh / 2);
				size_t need = ysz + 2 * csz;
				if (need > ctx->rotcap) {
					ctx->rotbuf = brealloc(ctx->rotbuf, need);
					ctx->rotcap = need;
				}
				uint8_t *dy = ctx->rotbuf, *du = dy + ysz, *dv = du + csz;
				spancam_xform_plane(f->data[0], w, h, f->linesize[0], dy, ow, rot, mir, 1);
				spancam_xform_plane(f->data[1], w / 2, h / 2, f->linesize[1], du, ow / 2, rot, mir, 1);
				spancam_xform_plane(f->data[2], w / 2, h / 2, f->linesize[2], dv, ow / 2, rot, mir, 1);
				frame.width = (uint32_t)ow;
				frame.height = (uint32_t)oh;
				frame.data[0] = dy;
				frame.linesize[0] = (uint32_t)ow;
				frame.data[1] = du;
				frame.linesize[1] = (uint32_t)(ow / 2);
				frame.data[2] = dv;
				frame.linesize[2] = (uint32_t)(ow / 2);
			}
		} else {
			frame.width = (uint32_t)f->width;
			frame.height = (uint32_t)f->height;
			for (size_t i = 0; i < MAX_AV_PLANES; i++) {
				frame.data[i] = f->data[i];
				frame.linesize[i] = (uint32_t)f->linesize[i];
			}
		}

		enum video_range_type range =
			(f->color_range == AVCOL_RANGE_JPEG) ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		frame.full_range = (range == VIDEO_RANGE_FULL);
		video_format_get_parameters_for_format(VIDEO_CS_709, range, fmt, frame.color_matrix,
						       frame.color_range_min, frame.color_range_max);

		obs_source_output_video(ctx->source, &frame);
		av_frame_unref(f);
	}
}

// --------------------------------------------------------------------------
// One connection: handshake, StreamHeader, packet pump.
// --------------------------------------------------------------------------

// Work out where the phone is and connect to it. Returns a connected socket
// (caller closes) and fills token_out with the key the handshake should carry,
// plus a label for the log line. -1 if nothing is reachable this round.
//
// Auto tries USB first (cheap, zero-config) when adb reports a device, and if that
// CONNECT fails it falls through to Wi-Fi. It used to commit to USB the instant
// adb saw a device, so a phone that was plugged in but not forwarding — screen
// locked, app not open, a stale forward — left Auto retrying a dead socket forever
// with a perfectly good Wi-Fi path sitting right there. Explicit USB mode is USB
// only; explicit Wi-Fi is Wi-Fi only. Only Auto gets to change its mind.
//
// The USB host is the NAME "localhost", never the literal 127.0.0.1: getaddrinfo
// then hands back both ::1 and 127.0.0.1 and spancam_connect tries each in turn,
// so a machine whose IPv4 loopback has been remapped or removed still connects
// over ::1. `adb forward` listens on both.
static int spancam_dial(struct spancam_source *ctx, char *token_out, size_t toksz, char *label_out, size_t lblsz,
			bool *used_usb)
{
	pthread_mutex_lock(&ctx->cfg_lock);
	int mode = ctx->connection;
	char *mhost = bstrdup(ctx->host ? ctx->host : "");
	int mport = ctx->port > 0 ? ctx->port : SPANCAM_DEFAULT_PORT;
	char *mtoken = bstrdup(ctx->token ? ctx->token : "");
	pthread_mutex_unlock(&ctx->cfg_lock);

	int fd = -1;
	token_out[0] = 0;
	*used_usb = false;

	// In Auto, skip USB while it's benched (see usb_cooldown_until_ns).
	bool usb_cooling = (mode == SPANCAM_CONN_AUTO) && (ctx->usb_cooldown_until_ns > (int64_t)os_gettime_ns());

	// ---- USB, if a phone is plugged in ----
	if ((mode == SPANCAM_CONN_USB || (mode == SPANCAM_CONN_AUTO && !usb_cooling)) && spancam_adb_has_device()) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "adb forward tcp:%d tcp:%d", SPANCAM_DEFAULT_PORT, SPANCAM_DEFAULT_PORT);
		spancam_run(cmd);
		fd = spancam_connect("localhost", SPANCAM_DEFAULT_PORT);
		if (fd >= 0) {
			token_out[0] = 0; // tokenless on loopback
			*used_usb = true;
			snprintf(label_out, lblsz, "USB localhost:%d", SPANCAM_DEFAULT_PORT);
			goto done;
		}
		obs_log(LOG_INFO, "Spancam: USB loopback connect failed%s",
			mode == SPANCAM_CONN_USB ? "" : " — falling back to Wi-Fi");
		if (mode == SPANCAM_CONN_USB)
			goto done; // USB-only: do not fall back
		// Auto: fall through to Wi-Fi below.
	}

	// ---- Wi-Fi: a typed-in host wins, otherwise go looking ----
	if (mode != SPANCAM_CONN_USB) {
		char host[128] = {0};
		int port = mport;
		char token[128] = {0};
		bool resolved = false;
		if (*mhost) {
			snprintf(host, sizeof(host), "%s", mhost);
			snprintf(token, sizeof(token), "%s", mtoken);
			resolved = true;
		} else if (spancam_udp_discover(ctx, host, sizeof(host), &port, token, sizeof(token))) {
			resolved = true;
		}
		if (resolved) {
			fd = spancam_connect(host, port);
			if (fd >= 0) {
				snprintf(token_out, toksz, "%s", token);
				snprintf(label_out, lblsz, "Wi-Fi %s:%d", host, port);
			}
		}
	}

done:
	bfree(mhost);
	bfree(mtoken);
	return fd;
}

static void spancam_stream_once(struct spancam_source *ctx)
{
	char token[128] = {0};
	char label[160] = {0};
	bool used_usb = false;
	bool got_stream = false; // a valid StreamHeader arrived => this path actually works
	int fd = spancam_dial(ctx, token, sizeof(token), label, sizeof(label), &used_usb);
	if (fd < 0)
		return;
	obs_log(LOG_INFO, "Spancam: connected (%s)", label);

	// Handshake: "SPANCAM/1 k=<token>\n"
	struct dstr hello;
	dstr_init(&hello);
	dstr_printf(&hello, "SPANCAM/1 k=%s\n", token);
	send(fd, hello.array, hello.len, 0);
	dstr_free(&hello);

	uint8_t hdr[24];
	if (!spancam_read_full(ctx, fd, hdr, sizeof(hdr)) || be32(hdr) != SPANCAM_MAGIC) {
		obs_log(LOG_WARNING, "Spancam: bad or missing stream header (%s)", label);
		goto done;
	}
	// flags bit0 seeds mirror at connect time; the live type-3 packet that follows
	// is the actual source of truth for both mirror and rotation.
	pthread_mutex_lock(&ctx->cfg_lock);
	ctx->mirror = hdr[5] & 1;
	pthread_mutex_unlock(&ctx->cfg_lock);
	uint8_t codec_byte = hdr[4];
	if (!spancam_open_decoder(ctx, codec_byte))
		goto done;
	got_stream = true;
	if (used_usb)
		ctx->usb_cooldown_until_ns = 0; // USB delivered — keep preferring it
	ctx->ts_base_set = false; // re-anchor the pacing grid for this connection
	// Ask for a clean IDR straight away: the phone runs a long GOP, so joining
	// mid-stream otherwise shows nothing at all until the next scheduled keyframe.
	ctx->ctrl_fd = fd;
	ctx->abr_ceiling = (int)be32(hdr + 20);
	ctx->abr_target = ctx->abr_ceiling;
	ctx->abr_base_set = false;
	ctx->abr_last_send_ns = 0;
	ctx->last_kf_request_ns = 0;
	spancam_send_keyframe(ctx);
	obs_log(LOG_INFO, "Spancam: stream %ux%u %s @ %u fps", be32(hdr + 8), be32(hdr + 12),
		codec_byte == 1 ? "HEVC" : "H.264", be32(hdr + 16));

	// Packet pump.
	size_t cap = 1 << 20;
	uint8_t *payload = bmalloc(cap);
	for (;;) {
		uint8_t ph[16];
		if (!spancam_read_full(ctx, fd, ph, sizeof(ph)))
			break;
		uint8_t type = ph[0];
		int64_t pts_us = (int64_t)be64(ph + 4);
		uint32_t size = be32(ph + 12);
		if (size == 0 || size > (64u << 20)) // sanity cap 64 MiB
			break;
		if (size > cap) {
			cap = size;
			payload = brealloc(payload, cap);
		}
		if (!spancam_read_full(ctx, fd, payload, size))
			break;
		if (type == 1 || type == 2) { // config and frames both go to the decoder
			if (type == 2)
				spancam_abr_observe(ctx, pts_us);
			spancam_decode(ctx, payload, (int)size, pts_us);
		} else if (type == 3 && size >= 2) {
			// Transform: mirror + rotation, applied on this end. Written under
			// the same lock the decode loop reads it with, so a change lands on
			// the very next frame.
			pthread_mutex_lock(&ctx->cfg_lock);
			ctx->mirror = payload[0] & 1;
			ctx->rotation = (payload[1] & 3) * 90;
			pthread_mutex_unlock(&ctx->cfg_lock);
		}
	}
	bfree(payload);

done:
	// Connected over USB but never got a header => bench USB so Auto tries Wi-Fi
	// for a while instead of spinning on a tunnel that connects and then says
	// nothing. A USB connection that DID stream cleared the cooldown above, so an
	// ordinary disconnect reconnects straight back over USB.
	if (used_usb && !got_stream)
		ctx->usb_cooldown_until_ns = (int64_t)os_gettime_ns() + 8000000000LL; // 8 s
	ctx->ctrl_fd = -1; // the control channel goes away with the connection
	spancam_close_decoder(ctx);
	close(fd);
	// Tell OBS the source has no picture. Without this the async texture stays
	// active and OBS keeps rendering the last frame it was given — forever. Pull
	// the cable mid-stream and your face stays on the broadcast, frozen, with no
	// indication anything is wrong. Idempotent, so calling it on every disconnect
	// is fine.
	obs_source_output_video(ctx->source, NULL);
}

static void *spancam_receive_loop(void *data)
{
	struct spancam_source *ctx = data;
	while (os_event_try(ctx->stop_signal) == EAGAIN) {
		spancam_stream_once(ctx);
		os_event_timedwait(ctx->stop_signal, SPANCAM_RECONNECT_MS);
	}
	return NULL;
}

// --------------------------------------------------------------------------
// OBS source plumbing.
// --------------------------------------------------------------------------

static const char *spancam_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Spancam.Source.Name");
}

static void spancam_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "connection", SPANCAM_CONN_AUTO);
	obs_data_set_default_int(settings, "port", SPANCAM_DEFAULT_PORT);
	obs_data_set_default_int(settings, "rotation", 0);
}

static obs_properties_t *spancam_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *conn = obs_properties_add_list(props, "connection", obs_module_text("Spancam.Prop.Connection"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Auto"), SPANCAM_CONN_AUTO);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Usb"), SPANCAM_CONN_USB);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Wifi"), SPANCAM_CONN_WIFI);
	obs_property_t *rot = obs_properties_add_list(props, "rotation", obs_module_text("Spancam.Prop.Rotation"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rot, "0°", 0);
	obs_property_list_add_int(rot, "90°", 90);
	obs_property_list_add_int(rot, "180°", 180);
	obs_property_list_add_int(rot, "270°", 270);
	obs_properties_add_text(props, "host", obs_module_text("Spancam.Prop.Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Spancam.Prop.Port"), 1, 65535, 1);
	obs_properties_add_text(props, "token", obs_module_text("Spancam.Prop.Token"), OBS_TEXT_DEFAULT);
	return props;
}

static void spancam_source_update(void *data, obs_data_t *settings)
{
	struct spancam_source *ctx = data;
	pthread_mutex_lock(&ctx->cfg_lock);
	bfree(ctx->host);
	bfree(ctx->token);
	ctx->connection = (int)obs_data_get_int(settings, "connection");
	ctx->host = bstrdup(obs_data_get_string(settings, "host"));
	ctx->token = bstrdup(obs_data_get_string(settings, "token"));
	ctx->port = (int)obs_data_get_int(settings, "port");
	ctx->rotation = (int)obs_data_get_int(settings, "rotation");
	pthread_mutex_unlock(&ctx->cfg_lock);
}

static void *spancam_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct spancam_source *ctx = bzalloc(sizeof(struct spancam_source));
	ctx->source = source;
	ctx->ctrl_fd = -1; // no control socket until a connection is live
	pthread_mutex_init(&ctx->cfg_lock, NULL);
	os_event_init(&ctx->stop_signal, OS_EVENT_TYPE_MANUAL);
	spancam_source_update(ctx, settings);
	ctx->thread_running = (pthread_create(&ctx->thread, NULL, spancam_receive_loop, ctx) == 0);
	return ctx;
}

static void spancam_source_destroy(void *data)
{
	struct spancam_source *ctx = data;
	if (ctx->thread_running) {
		os_event_signal(ctx->stop_signal);
		pthread_join(ctx->thread, NULL);
	}
	os_event_destroy(ctx->stop_signal);
	pthread_mutex_destroy(&ctx->cfg_lock);
	bfree(ctx->host);
	bfree(ctx->token);
	bfree(ctx->rotbuf);
	bfree(ctx);
}

struct obs_source_info spancam_source_info = {
	.id = "spancam_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = spancam_source_get_name,
	.create = spancam_source_create,
	.destroy = spancam_source_destroy,
	.update = spancam_source_update,
	.get_defaults = spancam_source_get_defaults,
	.get_properties = spancam_source_get_properties,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
