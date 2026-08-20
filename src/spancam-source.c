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

Builds on macOS, Windows and Linux. The socket layer is BSD sockets with a small
set of Winsock aliases at the top of this file, and hardware decode goes through
FFmpeg's generic hwaccel API (VideoToolbox / D3D11VA / VA-API) with a software
fallback that is always available.
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

#include "spancam-placeholder.h"

// Sockets. Winsock is close enough to BSD sockets that a handful of aliases
// covers the whole difference for what this file does.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET spancam_socket_t;
#define SPANCAM_BAD_SOCKET INVALID_SOCKET
#define spancam_closesocket closesocket
#define spancam_popen _popen
#define spancam_pclose _pclose
#define SPANCAM_ADB "adb.exe"
#define SPANCAM_DEVNULL " 2>NUL"
typedef int spancam_socklen_t;
#define SPANCAM_SOCKOPT(p) ((const char *)(p))
#else
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <arpa/inet.h>
#include <netdb.h>
typedef int spancam_socket_t;
#define SPANCAM_BAD_SOCKET (-1)
#define spancam_closesocket close
#define spancam_popen popen
#define spancam_pclose pclose
#define SPANCAM_ADB "adb"
#define SPANCAM_DEVNULL " 2>/dev/null"
typedef socklen_t spancam_socklen_t;
#define SPANCAM_SOCKOPT(p) (p)
#endif

// On macOS the decode path is VideoToolbox, not libavcodec. OBS bundles its own
// FFmpeg, and a plugin that READS AVFrame/AVCodecContext fields is bound to the
// exact major it was compiled against — FFmpeg only guarantees layout within a
// major. Pinning cannot fix that for a plugin shipped to everyone: pin 32.x and
// OBS 31.x users break, pin 31.x and the reverse. VideoToolbox is a stable system
// framework, so the macOS build is correct on every OBS version, and it is the
// same decoder the Spancam Mac app uses. Windows and Linux keep libavcodec.
#if defined(__APPLE__)
#define SPANCAM_USE_VTDEC 1
#include "spancam-vtdec.h"
// libavcodec lets its bitstream readers run a little past the end of the input, so
// the read buffer carries this much zeroed slack. VideoToolbox has no such rule,
// but keeping the same padding on both backends means one buffer-sizing rule.
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#else
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#endif

#if defined(__APPLE__)
#include <dns_sd.h>     // Bonjour browse/resolve — finds the iOS sender (macOS system lib)
#include <sys/select.h> // select() to pump the DNSServiceRef synchronously
#endif

// ABR tuning — mirrors mac/Spancam/Core/BitrateController.swift so both receivers
// converge identically on the same link.
#define SPANCAM_ABR_QUEUE_HIGH_NS 250000000LL  // >250 ms of queue => congested
#define SPANCAM_ABR_QUEUE_LOW_NS 60000000LL    // <60 ms => clear
#define SPANCAM_ABR_COOLDOWN_NS 3000000000LL   // 3 s after a cut before any climb
#define SPANCAM_ABR_BASELINE_NS 20000000000LL  // 20 s windowed-min baseline
#define SPANCAM_ABR_INCREASE_BPS 300000        // +300 kbps additive increase

#define SPANCAM_DEFAULT_PORT 8892
#define SPANCAM_DISCOVERY_PORT 8891
#define SPANCAM_PROBE "SPANCAM-DISCOVER"
#define SPANCAM_MAGIC 0x53504331u // 'SPC1'
#define SPANCAM_RECV_TIMEOUT_MS 500
// How often Auto looks for a newly-attached USB cable while streaming over Wi-Fi. Each
// check forks adb, so this is a few seconds rather than every tick — fast enough that a
// cable feels immediate, cheap enough to run for the whole session.
#define SPANCAM_USB_PROBE_NS 2000000000LL

// Liveness + reconnect, mirroring the Mac receiver so both ends of the product
// behave the same way when a link dies (MacEffectRunner + ConnectionCore).
#define SPANCAM_HEADER_TIMEOUT_MS 5000 // silence after connect before we give up
#define SPANCAM_STREAM_TIMEOUT_MS 3000 // silence mid-stream before the link is dead
#define SPANCAM_DECODE_DEAD_SECS 5     // wire flowing, nothing decoding => rebuild
// The Mac's paced redial ladder. Past the end we hold at the last rung rather
// than stopping: the Mac can afford to stop because a Bonjour browse pushes the
// phone back the moment it returns, and this plugin has no such push — it can
// only find a phone by dialling. So the tail is a slow poll, not a dead end.
#define SPANCAM_MAX_REDIALS 10

// Connection mode (the "Connection" dropdown).
// Announced in the handshake so the phone can show which computer connected.
#if defined(__APPLE__)
#define SPANCAM_OS "macOS"
#elif defined(_WIN32)
#define SPANCAM_OS "Windows"
#else
#define SPANCAM_OS "Linux"
#endif

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
	// Rotation has TWO independent sources and they must not share a variable.
	// wire_* is what the phone reports (StreamHeader mirror seed + type-3), and it
	// changes whenever the phone is turned. user_rotation is the dropdown, an
	// OFFSET the user adds on top. They used to be one field, so whichever wrote
	// last won: every type-3 (one per connect, i.e. every redial) zeroed the
	// dropdown, and every settings edit — including just switching Connection to
	// USB — slammed a correctly-rotated phone back to the stale dropdown value.
	// That is why changing transport appeared to rotate the canvas.
	int wire_rotation; // 0/90/180/270 from type-3
	int wire_mirror;   // 0/1 from type-3 / header flags
	int user_rotation; // 0/90/180/270 from the property, added on top
	// The phone this source is PINNED to, by NAME (see struct spancam_device). Empty =
	// "any", the old first-one-wins behaviour. Re-resolved on every dial so a rotated
	// access token or a new DHCP lease heals itself.
	char *device;

	// Last rotation handed to OBS, so the setter only fires on a real change.
	long applied_rotation;

	// PTS pacing: map the wire pts_us onto a monotonic OBS timebase so OBS's async
	// source buffer can absorb network jitter, instead of presenting each frame the
	// instant it happens to arrive. Re-anchored per connection.
	bool ts_base_set;
	int64_t ts_base_pts_us;
	int64_t ts_base_obs_ns;

	// Upstream control channel (plugin -> phone). Single writer: the receive thread.
	spancam_socket_t ctrl_fd; // socket to send control frames on
	int abr_target;           // bitrate the phone is currently being asked for
	int abr_ceiling;          // StreamHeader bitrate — never ask for more than this
	bool abr_base_set;
	int64_t abr_base_arrival_ns; // anchor for the one-way delay measurement
	int64_t abr_base_pts_us;
	int64_t abr_delay_ewma_ns; // smoothed queuing delay above the rolling baseline
	int64_t abr_last_send_ns;  // when a control frame last went out
	int64_t abr_last_tick_ns;  // when the loop last EVALUATED (~1 Hz)
	// Rolling windowed-minimum skew, as two half-window buckets. The MINIMUM is the
	// zero point: absolute skew is meaningless across two unrelated clocks, its growth
	// is the queue. Never re-anchored to the current sample — that is what hid a
	// standing queue and made a draining one read as healthy.
	int64_t abr_min_cur;
	int64_t abr_min_prev;
	int64_t abr_bucket_start_ns;
	int64_t abr_cooldown_until_ns; // no increase before this (armed on every cut)
	int abr_healthy_ticks;         // consecutive clear ticks (2 needed to climb)
	int64_t last_kf_request_ns;    // debounce keyframe requests — no IDR storms

	// Reconnect + liveness, mirroring the Mac receiver (MacEffectRunner's
	// BLACK-STREAM WATCHDOG and ConnectionCore's paced redial ladder).
	int redials;             // consecutive failed/short connections
	uint64_t wire_packets;   // SDSP packets read off the socket this connection
	uint64_t decoded_frames; // frames actually emitted to OBS this connection
	uint64_t wd_last_wire, wd_last_decoded;
	int wd_dead_secs; // consecutive seconds of wire-but-no-decode
	int64_t wd_next_tick_ns;

	// Auto-mode USB cooldown. A USB connection that CONNECTS but never produces a
	// valid stream (flaky wireless-adb tunnel, a forward pointing at the wrong
	// device, app not discoverable) would otherwise loop forever: connect, no
	// header, disconnect, connect... Wi-Fi never gets a look in because the connect
	// itself keeps succeeding. So Auto benches USB until this deadline. Cleared the
	// moment USB does yield a header, so USB stays the preference. Auto only —
	// explicit USB mode never cools down, because the user asked for USB. (os_gettime_ns)
	int64_t usb_cooldown_until_ns;
	// Next time Auto may look for a USB cable to promote to (see the packet pump).
	int64_t usb_probe_next_ns;
	/// Set when the pump broke to promote to USB, so the loop redials at once.
	bool promote_now;
	/// Consecutive USB attempts that connected but never streamed. Backs off the probe.
	int promote_fails;
	/// Signalled on a show/hide edge so the idle wait wakes immediately.
	os_event_t *wake;
	/// True while parked because the source is hidden (drives the one-shot log lines).
	bool idled;
	// Phones seen by the last device scan, so the "install the app" hint in the
	// properties dialog can appear only when there is genuinely nothing to pick.
	int last_scan_n;

#if defined(SPANCAM_USE_VTDEC)
	spancam_vtdec_t *vtdec;
#else
	// Decoder (rebuilt per connection from the StreamHeader codec byte).
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;

	// Hardware decode. hw_decode = false means the pure software path, either
	// because nothing suitable was found or because a step failed and we fell
	// back. sw_frame is the reusable download target for the GPU surface.
	AVBufferRef *hw_device_ctx;
	AVFrame *sw_frame;
	bool hw_decode;
	enum AVPixelFormat hw_pix_fmt; // the format get_format must hold out for
#endif
};

// --------------------------------------------------------------------------
// Socket helpers.
// --------------------------------------------------------------------------

// "The read timed out / would block", which is the normal case on every recv
// timeout tick and must not be mistaken for the connection dying.
static bool spancam_would_block(void)
{
#if defined(_WIN32)
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT || e == WSAEINTR;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// The raw socket error, for diagnostics.
static int spancam_last_error(void)
{
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

static spancam_socket_t spancam_connect(const char *host, int port)
{
	char portstr[8];
	snprintf(portstr, sizeof(portstr), "%d", port);

	struct addrinfo hints = {0};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return SPANCAM_BAD_SOCKET;

	spancam_socket_t fd = SPANCAM_BAD_SOCKET;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == SPANCAM_BAD_SOCKET)
			continue;
		if (connect(fd, ai->ai_addr, (spancam_socklen_t)ai->ai_addrlen) == 0)
			break;
		spancam_closesocket(fd);
		fd = SPANCAM_BAD_SOCKET;
	}
	freeaddrinfo(res);

	if (fd != SPANCAM_BAD_SOCKET) {
		// A read timeout lets the receive loop notice the stop signal even
		// while the phone is silent.
		// SO_RCVTIMEO wants a DWORD of milliseconds on Windows and a timeval
		// everywhere else — the one socket option that isn't just a rename.
#if defined(_WIN32)
		DWORD tv = SPANCAM_RECV_TIMEOUT_MS;
#else
		struct timeval tv = {.tv_sec = SPANCAM_RECV_TIMEOUT_MS / 1000,
				     .tv_usec = (SPANCAM_RECV_TIMEOUT_MS % 1000) * 1000};
#endif
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, SPANCAM_SOCKOPT(&tv), sizeof(tv));
		// A 4K keyframe is a lot of bytes arriving at once; the default receive
		// buffer makes the kernel drop window and the sender stall on it.
		int rcvbuf = 256 * 1024;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, SPANCAM_SOCKOPT(&rcvbuf), sizeof(rcvbuf));
		int nodelay = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, SPANCAM_SOCKOPT(&nodelay), sizeof(nodelay));
		// Keepalive. A phone that dies without a FIN (airplane mode, out of AP
		// range, battery death) otherwise leaves this socket open forever: recv
		// just keeps timing out, and because the plugin sends nothing while idle
		// the kernel has no outstanding data to time out either. Keepalive is off
		// by default on Darwin and Windows, so it has to be asked for.
		int ka = 1;
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, SPANCAM_SOCKOPT(&ka), sizeof(ka));
#if defined(TCP_KEEPALIVE) && defined(__APPLE__)
		int kidle = 5; // seconds idle before the first probe
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, SPANCAM_SOCKOPT(&kidle), sizeof(kidle));
#elif defined(TCP_KEEPIDLE)
		int kidle = 5, kintvl = 2, kcnt = 3;
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, SPANCAM_SOCKOPT(&kidle), sizeof(kidle));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, SPANCAM_SOCKOPT(&kintvl), sizeof(kintvl));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, SPANCAM_SOCKOPT(&kcnt), sizeof(kcnt));
#endif
	}
	return fd;
}

// Read exactly len bytes, giving up after timeout_ms without a single byte of
// progress. The deadline is the whole point: SO_RCVTIMEO here is only a tick so
// the loop can notice the stop signal, so without an elapsed-time bound a peer
// that goes silent WITHOUT closing (the common Wi-Fi death) parks this thread
// forever — the caller never returns, the source is never blanked, and the
// reconnect below never runs. Returns false on EOF, error, stop, or deadline.
static bool spancam_read_full(struct spancam_source *ctx, spancam_socket_t fd, uint8_t *buf, size_t len, int timeout_ms)
{
	size_t got = 0;
	int64_t last_progress = (int64_t)os_gettime_ns();
	while (got < len) {
		if (os_event_try(ctx->stop_signal) != EAGAIN)
			return false;
		int n = recv(fd, (char *)buf + got, (int)(len - got), 0);
		if (n > 0) {
			got += (size_t)n;
			last_progress = (int64_t)os_gettime_ns();
		} else if (n < 0 && spancam_would_block()) {
			if ((int64_t)os_gettime_ns() - last_progress > (int64_t)timeout_ms * 1000000LL) {
				obs_log(LOG_WARNING, "Spancam: no data for %d ms — treating the link as dead",
					timeout_ms);
				return false;
			}
			continue; // timeout tick — re-check stop, keep reading
		} else {
			obs_log(LOG_WARNING, "Spancam: DIAG read ended n=%d err=%d (got %zu/%zu)", n,
				spancam_last_error(), got, len);
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
	if (ctx->ctrl_fd == SPANCAM_BAD_SOCKET)
		return;
	uint8_t hdr[8] = {
		(uint8_t)(type >> 24), (uint8_t)(type >> 16), (uint8_t)(type >> 8), (uint8_t)type,
		(uint8_t)(len >> 24),  (uint8_t)(len >> 16),  (uint8_t)(len >> 8),  (uint8_t)len,
	};
	if (send(ctx->ctrl_fd, (const char *)hdr, (int)sizeof(hdr), 0) < 0)
		return;
	if (len > 0 && payload)
		send(ctx->ctrl_fd, (const char *)payload, (int)len, 0);
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
// Closed-loop ABR — the SAME AIMD the Mac receiver runs (mac/Spancam/Core/
// BitrateController.swift), not an approximation of it.
//
// The previous loop was a 30-line stand-in that got four of the algorithm's invariants
// wrong, and bounding the phone's SO_SNDBUF (so its backpressure valve can finally
// engage) made the consequence impossible to miss on real hardware: the phone logged
// `BACKPRESSURE latch exit: dropping 260ms` bursts every two seconds while this end
// walked the target 5287 -> 5816 -> 6220 kbps straight back to the ceiling, re-saturating
// the link and triggering another drop burst plus an IDR storm. A ~0.5 Hz sawtooth that
// never converges, where the Mac settles on the same link.
//
// The four corrections:
//   1. ADDITIVE increase (+300 kbps), not multiplicative (+10%). Near a 6.2 Mbps ceiling
//      +10% is ~620 kbps per step, which overshoots capacity in one move.
//   2. A post-cut COOLDOWN (3 s) plus two consecutive HEALTHY ticks before any increase.
//      Previously a cut at t could be followed by a raise at t+1s.
//   3. The delay baseline is a WINDOWED MINIMUM, never re-anchored. Re-anchoring to the
//      current (already-delayed) arrival and zeroing the EWMA after every send made a
//      STANDING queue structurally unobservable, and a DRAINING queue read as healthy
//      because arrivals run ahead of the pts grid and the negative delay was clamped to
//      zero — so the loop always concluded "clear" one second after cutting.
//   4. A send DEADBAND of max(50 kbps, 5%), so tiny corrections don't thrash
//      MediaCodec.setParameters.
//
// The skew here is (arrival - pts) on this end's monotonic clock. Its ABSOLUTE value is
// meaningless (the two clocks are unrelated) but its GROWTH is exactly the queuing delay,
// which is why the minimum over the window is the right zero point.
static void spancam_abr_observe(struct spancam_source *ctx, int64_t pts_us)
{
	const int64_t now = (int64_t)os_gettime_ns();
	const int64_t skew = now - pts_us * 1000;

	// Rolling baseline: the smallest skew seen in the last SPANCAM_ABR_BASELINE_NS. Kept
	// as two buckets so the window slides without storing every sample.
	if (!ctx->abr_base_set || now - ctx->abr_bucket_start_ns > SPANCAM_ABR_BASELINE_NS / 2) {
		ctx->abr_min_prev = ctx->abr_base_set ? ctx->abr_min_cur : skew;
		ctx->abr_min_cur = skew;
		ctx->abr_bucket_start_ns = now;
		ctx->abr_base_set = true;
	}
	if (skew < ctx->abr_min_cur)
		ctx->abr_min_cur = skew;
	const int64_t baseline = ctx->abr_min_prev < ctx->abr_min_cur ? ctx->abr_min_prev : ctx->abr_min_cur;
	int64_t queue_ns = skew - baseline;
	if (queue_ns < 0)
		queue_ns = 0;
	ctx->abr_delay_ewma_ns = (ctx->abr_delay_ewma_ns * 7 + queue_ns) / 8;

	if (ctx->ctrl_fd == SPANCAM_BAD_SOCKET || ctx->abr_ceiling <= 0)
		return;
	if (now - ctx->abr_last_tick_ns < 1000000000LL) // evaluate at ~1 Hz
		return;
	ctx->abr_last_tick_ns = now;

	const int floor_bps = ctx->abr_ceiling * 45 / 100 > 800000 ? ctx->abr_ceiling * 45 / 100 : 800000;
	int target = ctx->abr_target;

	if (ctx->abr_delay_ewma_ns > SPANCAM_ABR_QUEUE_HIGH_NS) {
		// Congested: multiplicative decrease, and arm the cooldown so the climb cannot
		// start again until the queue has had time to actually drain.
		target = target * 85 / 100;
		if (target < floor_bps)
			target = floor_bps;
		ctx->abr_cooldown_until_ns = now + SPANCAM_ABR_COOLDOWN_NS;
		ctx->abr_healthy_ticks = 0;
	} else if (ctx->abr_delay_ewma_ns < SPANCAM_ABR_QUEUE_LOW_NS) {
		// Clear. Require the cooldown to have expired AND two consecutive healthy ticks.
		ctx->abr_healthy_ticks++;
		if (now < ctx->abr_cooldown_until_ns || ctx->abr_healthy_ticks < 2)
			return;
		if (target >= ctx->abr_ceiling)
			return;
		target += SPANCAM_ABR_INCREASE_BPS;
		if (target > ctx->abr_ceiling)
			target = ctx->abr_ceiling;
	} else {
		// In between: hold, and do NOT accrue healthy ticks.
		ctx->abr_healthy_ticks = 0;
		return;
	}

	// Deadband — max(50 kbps, 5%) — so setParameters is not called for noise.
	const int delta = target > ctx->abr_target ? target - ctx->abr_target : ctx->abr_target - target;
	const int need = ctx->abr_target / 20 > 50000 ? ctx->abr_target / 20 : 50000;
	if (delta < need)
		return;

	ctx->abr_target = target;
	ctx->abr_last_send_ns = now;
	spancam_send_bitrate(ctx, ctx->abr_target);
	obs_log(LOG_DEBUG, "Spancam: abr target=%d kbps qDelay=%d ms", ctx->abr_target / 1000,
		(int)(ctx->abr_delay_ewma_ns / 1000000));
}

// --------------------------------------------------------------------------
// Discovery (UDP broadcast) + USB (adb), so nobody has to read an IP address off
// a phone screen and type it into OBS.
// --------------------------------------------------------------------------

// Find adb. A Finder/Dock-launched OBS.app inherits launchd's PATH
// (/usr/bin:/bin:/usr/sbin:/sbin), which contains no adb on any platform — not
// Android Studio's, not either Homebrew prefix — so a bare `adb` through popen
// exits 127 and USB looks like "no device plugged in". It works when OBS is
// started from a terminal, which is why this never showed up in development.
// Resolved once per process; NULL means genuinely not installed.
static const char *spancam_adb_path(void)
{
	static char cached[1024];
	static bool resolved = false;
	if (resolved)
		return cached[0] ? cached : NULL;
	resolved = true;

	char cands[8][1024];
	int n = 0;
	const char *env;
#if defined(_WIN32)
	if ((env = getenv("ANDROID_SDK_ROOT")))
		snprintf(cands[n++], sizeof(cands[0]), "%s\\platform-tools\\adb.exe", env);
	if ((env = getenv("ANDROID_HOME")))
		snprintf(cands[n++], sizeof(cands[0]), "%s\\platform-tools\\adb.exe", env);
	if ((env = getenv("LOCALAPPDATA")))
		snprintf(cands[n++], sizeof(cands[0]), "%s\\Android\\Sdk\\platform-tools\\adb.exe", env);
	snprintf(cands[n++], sizeof(cands[0]), "adb.exe"); // PATH, for a console launch
#else
	if ((env = getenv("ANDROID_SDK_ROOT")))
		snprintf(cands[n++], sizeof(cands[0]), "%s/platform-tools/adb", env);
	if ((env = getenv("ANDROID_HOME")))
		snprintf(cands[n++], sizeof(cands[0]), "%s/platform-tools/adb", env);
	if ((env = getenv("HOME")))
		snprintf(cands[n++], sizeof(cands[0]), "%s/Library/Android/sdk/platform-tools/adb", env);
	snprintf(cands[n++], sizeof(cands[0]), "/opt/homebrew/bin/adb");
	snprintf(cands[n++], sizeof(cands[0]), "/usr/local/bin/adb");
	snprintf(cands[n++], sizeof(cands[0]), "/usr/bin/adb");
	snprintf(cands[n++], sizeof(cands[0]), "adb"); // PATH, for a terminal launch
#endif

	for (int i = 0; i < n; i++) {
		char probe[1100];
		snprintf(probe, sizeof(probe), "\"%.*s\" version" SPANCAM_DEVNULL, (int)sizeof(probe) - 32, cands[i]);
		FILE *f = spancam_popen(probe, "r");
		if (!f)
			continue;
		char line[256];
		bool got = fgets(line, sizeof(line), f) && strstr(line, "Android Debug Bridge");
		while (fgets(line, sizeof(line), f)) {
		}
		spancam_pclose(f);
		if (got) {
			snprintf(cached, sizeof(cached), "%.*s", (int)sizeof(cached) - 1, cands[i]);
			obs_log(LOG_INFO, "Spancam: using adb at %s", cached);
			return cached;
		}
	}
	obs_log(LOG_WARNING, "Spancam: adb not found — USB needs Android Platform Tools "
			     "(set ANDROID_SDK_ROOT, or install to ~/Library/Android/sdk)");
	return NULL;
}

static void spancam_run(const char *cmd)
{
	FILE *f = spancam_popen(cmd, "r");
	if (!f)
		return;
	char buf[256];
	while (fgets(buf, sizeof(buf), f)) {
	} // drain
	spancam_pclose(f);
}

// Find a device attached BY CABLE, writing its serial to serial_out.
//
// `adb devices` lists wireless devices too, and they are the whole problem: adb
// over Wi-Fi forwards through adbd, so the connection still arrives on the
// phone's 127.0.0.1 and the phone -- which infers the transport from the peer
// address -- concludes "USB, cable, plenty of bandwidth" and commits the session
// to its top rung (4K30 at ~25 Mbps on an S24 Ultra). Those bytes then cross the
// same Wi-Fi as everything else, which delivers a fraction of it, so the sender's
// 3-frame queue sheds continuously and re-keyframes, and queuing delay grows
// without bound. Measured on wireless adb: 4K/24.9 Mbps advertised, 10 fps and
// 6.3 Mbps delivered, ~3.1 s of accumulated delay.
//
// A cable serial is a plain device id; a wireless one is either an mDNS instance
// ("adb-<serial>-xxxxx._adb-tls-connect._tcp") or "<ip>:<port>". Both contain a
// character a cable serial never does, so the test is exact rather than a guess.
// Cable devices only, with a COUNT so a pinned source can refuse an ambiguous USB path.
static bool spancam_adb_usb_device_count(char *serial_out, size_t serialsz, int *count_out)
{
	const char *adb = spancam_adb_path();
	if (!adb)
		return false;
	char cmd[1100];
	snprintf(cmd, sizeof(cmd), "\"%s\" devices" SPANCAM_DEVNULL, adb);
	FILE *f = spancam_popen(cmd, "r");
	if (!f)
		return false;
	char line[256];
	bool found = false, saw_wireless = false;
	int cables = 0;
	while (fgets(line, sizeof(line), f)) {
		char *tab = strstr(line, "\tdevice"); // "<serial>\tdevice"; skips the header
		if (!tab)
			continue;
		*tab = 0;
		if (strstr(line, "._tcp") || strchr(line, ':')) {
			saw_wireless = true; // adb over Wi-Fi — not a cable
			continue;
		}
		cables++;
		if (!found) {
			snprintf(serial_out, serialsz, "%.*s", (int)serialsz - 1, line);
			found = true;
		}
	}
	spancam_pclose(f);
	if (count_out)
		*count_out = cables;
	if (!found && saw_wireless)
		obs_log(LOG_INFO, "Spancam: adb sees only a WIRELESS device — that is not a cable, "
				  "so USB is unavailable and Wi-Fi is the honest path");
	return found;
}

// Convenience wrapper for callers that only need "is a cable present".
static bool spancam_adb_usb_device(char *serial_out, size_t serialsz)
{
	return spancam_adb_usb_device_count(serial_out, serialsz, NULL);
}

// Broadcast a probe and parse the first reply.
// Reply: "SPANCAM-OBS|name|port|token|codec|w|h"; the host is the reply's SOURCE
// address, not anything the phone claims about itself — a phone behind NAT or
// with several interfaces up doesn't reliably know which of its addresses we can
// actually reach, but the packet that just arrived proves one of them.
// --------------------------------------------------------------------------
// Device enumeration — one row per reachable phone, so a source can be PINNED
// to a specific handset.
//
// Without this the plugin was single-device by construction: the UDP path took
// "the first reply" and the Bonjour path took the first browse result, so two
// sources in one scene both raced onto the same phone (and, because each phone
// accepts exactly one SDSP client and evicts the previous one, they then evicted
// each other on the 1.5 s redial forever — neither ever got a stable stream).
// The Mac app has always had a device picker; this is the OBS equivalent.
//
// The row's IDENTITY is the phone's NAME, never its address or key, and the dial
// re-resolves it every attempt. That is deliberate: the SDSP access token is
// regenerated on every app launch and the phone's DHCP lease can move, so a
// pinned host+token goes stale the moment the phone's app restarts (measured:
// "bad or missing stream header" forever until retyped). Re-resolving by name
// makes both self-healing.
struct spancam_device {
	char name[128];
	char host[128];
	int port;
	char token[128];
	bool via_mdns;
};

#define SPANCAM_MAX_DEVICES 8

// Collect every phone that answers the UDP broadcast (Android). Unlike
// spancam_udp_discover this drains the whole reply window instead of stopping at
// the first datagram, and de-dupes by source address.
static int spancam_udp_enumerate(struct spancam_source *ctx, struct spancam_device *out, int max)
{
	int count = 0;
	spancam_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == SPANCAM_BAD_SOCKET)
		return 0;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, SPANCAM_SOCKOPT(&yes), sizeof(yes));
#if defined(_WIN32)
	DWORD tv = 400;
#else
	struct timeval tv = {.tv_sec = 0, .tv_usec = 400000};
#endif
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, SPANCAM_SOCKOPT(&tv), sizeof(tv));

	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(SPANCAM_DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, SPANCAM_PROBE, (int)sizeof(SPANCAM_PROBE) - 1, 0, (struct sockaddr *)&to, sizeof(to));

	// Several probes: one datagram can be lost, and a phone that just woke its
	// Wi-Fi may miss the first.
	for (int round = 0; round < 6 && count < max && os_event_try(ctx->stop_signal) == EAGAIN; round++) {
		char buf[256];
		struct sockaddr_in from = {0};
		spancam_socklen_t flen = sizeof(from);
		int n = recvfrom(fd, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
		if (n <= 0) {
			sendto(fd, SPANCAM_PROBE, (int)sizeof(SPANCAM_PROBE) - 1, 0, (struct sockaddr *)&to,
			       sizeof(to));
			continue;
		}
		buf[n] = 0;
		if (strncmp(buf, "SPANCAM-OBS|", 12) != 0)
			continue;
		char ip[INET_ADDRSTRLEN] = {0};
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
		bool dup = false;
		for (int i = 0; i < count; i++)
			if (strcmp(out[i].host, ip) == 0)
				dup = true;
		if (dup)
			continue;
		// SPANCAM-OBS|name|port|token|codec|w|h — the NAME may legitimately contain
		// spaces, so split on '|' by hand rather than with strtok (which also
		// collapses empty fields and would shift every later column).
		char *f[8] = {0};
		int nf = 0;
		char *cur = buf;
		while (nf < 8) {
			f[nf++] = cur;
			char *bar = strchr(cur, '|');
			if (!bar)
				break;
			*bar = 0;
			cur = bar + 1;
		}
		if (nf < 4)
			continue;
		struct spancam_device *d = &out[count++];
		snprintf(d->name, sizeof(d->name), "%s", f[1]);
		snprintf(d->host, sizeof(d->host), "%s", ip);
		d->port = atoi(f[2]);
		if (d->port <= 0)
			d->port = SPANCAM_DEFAULT_PORT;
		snprintf(d->token, sizeof(d->token), "%s", f[3]);
		d->via_mdns = false;
	}
	spancam_closesocket(fd);
	return count;
}

static bool spancam_udp_discover(struct spancam_source *ctx, char *host, size_t hostsz, int *port, char *token,
				 size_t toksz)
{
	spancam_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == SPANCAM_BAD_SOCKET)
		return false;
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, SPANCAM_SOCKOPT(&yes), sizeof(yes));
#if defined(_WIN32)
	DWORD tv = 600;
#else
	struct timeval tv = {.tv_sec = 0, .tv_usec = 600000};
#endif
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, SPANCAM_SOCKOPT(&tv), sizeof(tv));

	struct sockaddr_in to = {0};
	to.sin_family = AF_INET;
	to.sin_port = htons(SPANCAM_DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, SPANCAM_PROBE, (int)sizeof(SPANCAM_PROBE) - 1, 0, (struct sockaddr *)&to, sizeof(to));

	bool ok = false;
	for (int tries = 0; tries < 4 && os_event_try(ctx->stop_signal) == EAGAIN; tries++) {
		char buf[256];
		struct sockaddr_in from = {0};
		spancam_socklen_t flen = sizeof(from);
		int n = recvfrom(fd, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
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
			char ip[INET_ADDRSTRLEN] = {0};
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			snprintf(host, hostsz, "%s", ip);
			*port = atoi(fields[2]);
			snprintf(token, toksz, "%s", fields[3]);
			ok = true;
			break;
		}
	}
	spancam_closesocket(fd);
	return ok;
}

#if defined(__APPLE__)
// Bonjour discovery (macOS). The iOS sender can't answer the UDP broadcast above
// without the multicast entitlement, so it advertises `_spancam-sdsp._tcp` over
// mDNS instead; this browses for it. Uses the SRV record's port (so the port is
// never guessed) and the "token" TXT value. macOS-only — Windows/Linux fall back
// to the manual host for iOS. (Android is found by the UDP path above.)
struct spancam_mdns_state {
	bool have_service;
	char name[128], regtype[64], domain[128];
	uint32_t ifIndex;
	bool have_resolve;
	char hosttarget[256];
	int port;
	char token[128];
};

static void spancam_mdns_pump(DNSServiceRef ref, int timeout_ms)
{
	int fd = DNSServiceRefSockFD(ref);
	if (fd < 0)
		return;
	fd_set set;
	FD_ZERO(&set);
	FD_SET(fd, &set);
	struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
	if (select(fd + 1, &set, NULL, NULL, &tv) > 0 && FD_ISSET(fd, &set))
		DNSServiceProcessResult(ref);
}

static void DNSSD_API spancam_browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifIndex,
					DNSServiceErrorType err, const char *name, const char *regtype,
					const char *domain, void *ctxv)
{
	UNUSED_PARAMETER(ref);
	struct spancam_mdns_state *st = ctxv;
	if (err == kDNSServiceErr_NoError && (flags & kDNSServiceFlagsAdd) && !st->have_service) {
		snprintf(st->name, sizeof(st->name), "%s", name);
		snprintf(st->regtype, sizeof(st->regtype), "%s", regtype);
		snprintf(st->domain, sizeof(st->domain), "%s", domain);
		st->ifIndex = ifIndex;
		st->have_service = true;
	}
}

static void DNSSD_API spancam_resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifIndex,
					 DNSServiceErrorType err, const char *fullname, const char *hosttarget,
					 uint16_t port, uint16_t txtLen, const unsigned char *txt, void *ctxv)
{
	UNUSED_PARAMETER(ref);
	UNUSED_PARAMETER(flags);
	UNUSED_PARAMETER(ifIndex);
	UNUSED_PARAMETER(fullname);
	struct spancam_mdns_state *st = ctxv;
	if (err != kDNSServiceErr_NoError)
		return;
	snprintf(st->hosttarget, sizeof(st->hosttarget), "%s", hosttarget);
	st->port = ntohs(port);
	uint8_t tl = 0;
	const void *tv = TXTRecordGetValuePtr(txtLen, txt, "token", &tl);
	if (tv && tl > 0 && tl < sizeof(st->token)) {
		memcpy(st->token, tv, tl);
		st->token[tl] = 0;
	}
	st->have_resolve = true;
}

static bool spancam_mdns_discover(struct spancam_source *ctx, char *host, size_t hostsz, int *port, char *token,
				  size_t toksz)
{
	struct spancam_mdns_state st = {0};
	DNSServiceRef browse = NULL;
	if (DNSServiceBrowse(&browse, 0, kDNSServiceInterfaceIndexAny, "_spancam-sdsp._tcp", NULL, spancam_browse_cb,
			     &st) != kDNSServiceErr_NoError)
		return false;
	for (int i = 0; i < 7 && !st.have_service && os_event_try(ctx->stop_signal) == EAGAIN; i++)
		spancam_mdns_pump(browse, 100);
	DNSServiceRefDeallocate(browse);
	if (!st.have_service)
		return false;

	DNSServiceRef resolve = NULL;
	if (DNSServiceResolve(&resolve, 0, st.ifIndex, st.name, st.regtype, st.domain, spancam_resolve_cb, &st) !=
	    kDNSServiceErr_NoError)
		return false;
	for (int i = 0; i < 7 && !st.have_resolve && os_event_try(ctx->stop_signal) == EAGAIN; i++)
		spancam_mdns_pump(resolve, 100);
	DNSServiceRefDeallocate(resolve);
	if (!st.have_resolve)
		return false;

	// hosttarget is an mDNS name like "iPhone.local." — getaddrinfo resolves it.
	snprintf(host, hostsz, "%s", st.hosttarget);
	*port = st.port;
	snprintf(token, toksz, "%s", st.token);
	return true;
}

// Enumerate EVERY advertised `_spancam-sdsp._tcp` instance (this is how iPhones are
// found — they cannot answer the UDP broadcast without the multicast entitlement).
// spancam_mdns_discover above deliberately stops at the first service because it only
// needs *a* phone; the picker needs them all.
struct spancam_mdns_names {
	char names[SPANCAM_MAX_DEVICES][128];
	uint32_t ifIndex[SPANCAM_MAX_DEVICES];
	char regtype[64], domain[128];
	int count;
};

static void DNSSD_API spancam_browse_all_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifIndex,
					    DNSServiceErrorType err, const char *name, const char *regtype,
					    const char *domain, void *ctxv)
{
	UNUSED_PARAMETER(ref);
	struct spancam_mdns_names *st = ctxv;
	if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd))
		return;
	for (int i = 0; i < st->count; i++)
		if (strcmp(st->names[i], name) == 0)
			return; // same instance on another interface
	if (st->count >= SPANCAM_MAX_DEVICES)
		return;
	snprintf(st->names[st->count], sizeof(st->names[0]), "%s", name);
	st->ifIndex[st->count] = ifIndex;
	st->count++;
	snprintf(st->regtype, sizeof(st->regtype), "%s", regtype);
	snprintf(st->domain, sizeof(st->domain), "%s", domain);
}

static int spancam_mdns_enumerate(struct spancam_source *ctx, struct spancam_device *out, int max)
{
	struct spancam_mdns_names names = {0};
	DNSServiceRef browse = NULL;
	if (DNSServiceBrowse(&browse, 0, kDNSServiceInterfaceIndexAny, "_spancam-sdsp._tcp", NULL,
			     spancam_browse_all_cb, &names) != kDNSServiceErr_NoError)
		return 0;
	// Keep pumping the whole window rather than stopping at the first Add, so a
	// second phone that answers a little later is still listed.
	for (int i = 0; i < 10 && os_event_try(ctx->stop_signal) == EAGAIN; i++)
		spancam_mdns_pump(browse, 100);
	DNSServiceRefDeallocate(browse);

	int count = 0;
	for (int i = 0; i < names.count && count < max; i++) {
		struct spancam_mdns_state st = {0};
		DNSServiceRef resolve = NULL;
		if (DNSServiceResolve(&resolve, 0, names.ifIndex[i], names.names[i], names.regtype, names.domain,
				      spancam_resolve_cb, &st) != kDNSServiceErr_NoError)
			continue;
		for (int k = 0; k < 7 && !st.have_resolve && os_event_try(ctx->stop_signal) == EAGAIN; k++)
			spancam_mdns_pump(resolve, 100);
		DNSServiceRefDeallocate(resolve);
		if (!st.have_resolve)
			continue;
		struct spancam_device *d = &out[count++];
		snprintf(d->name, sizeof(d->name), "%s", names.names[i]);
		snprintf(d->host, sizeof(d->host), "%s", st.hosttarget);
		d->port = st.port > 0 ? st.port : SPANCAM_DEFAULT_PORT;
		snprintf(d->token, sizeof(d->token), "%s", st.token);
		d->via_mdns = true;
	}
	return count;
}
#endif // __APPLE__

// Every reachable phone, both transports, de-duped by name. Android answers the UDP
// broadcast; iOS advertises over mDNS. On platforms without a Bonjour client only the
// UDP half runs, which is exactly the pre-existing iOS limitation — the picker simply
// shows fewer rows there rather than behaving differently.
static int spancam_enumerate_devices(struct spancam_source *ctx, struct spancam_device *out, int max)
{
	int count = spancam_udp_enumerate(ctx, out, max);
#if defined(__APPLE__)
	if (count < max) {
		struct spancam_device m[SPANCAM_MAX_DEVICES];
		int mc = spancam_mdns_enumerate(ctx, m, max - count);
		for (int i = 0; i < mc && count < max; i++) {
			bool dup = false;
			for (int k = 0; k < count; k++)
				if (strcmp(out[k].name, m[i].name) == 0)
					dup = true;
			if (!dup)
				out[count++] = m[i];
		}
	}
#endif
	return count;
}

// --------------------------------------------------------------------------
// FFmpeg decode -> OBS.
// --------------------------------------------------------------------------

// Rotation and mirroring are done by OBS, on the GPU, not here.
//
// This file used to rotate and mirror every plane on the CPU — a scalar per-pixel
// permutation with a transposing write pattern, run on the socket thread. Measured
// at 4K with rotation on an Apple M4: 10.9 ms/frame, i.e. a 92 fps single-core
// ceiling on one of the fastest cores available; on the mid and low-end laptops
// most OBS users actually have it could not hold 30 fps at all, and the symptom —
// judder plus climbing delay — looks exactly like a network fault.
//
// libobs already rotates async sources on the GPU (obs_source_set_async_rotation,
// applied in obs-source.c's render path) and flips them via GS_FLIP_V, and it
// swaps async_width/height for 90/270 by itself. GS_FLIP_V is a VERTICAL flip, but
// a horizontal mirror is just R180 composed with it, so every combination the wire
// can ask for maps onto that pair:
//
// DIRECTION: the wire angle is CLOCKWISE — shared/PROTOCOL.md §3 (0x21) says "the
// rotation (clockwise, 0/90/180/270) the receiver must apply". libobs rotates the
// OTHER way: rotate_async_video (obs-source.c) ends with
//     gs_matrix_rotaa4f(0.0f, 0.0f, -1.0f, RAD(rotation));
// i.e. about NEGATIVE Z, which in OBS's Y-down space is counter-clockwise. Tracking
// the corners of a 1920x1080 frame through the rotation=90 case (translate(0,w) then
// rotate) puts the source's TOP edge on the LEFT edge of the output, confirming CCW.
//
// So a clockwise wire angle must be negated before it is handed to OBS. This was the
// "OBS output is upside down" bug: 0 and 180 are unaffected (180 is its own inverse),
// so only the two PORTRAIT angles were wrong — and they were wrong by exactly 180
// degrees, which reads as an inverted image rather than a sideways one. Measured on
// device: an S24 Ultra held portrait sends rot=270 (OrientationTracker.effective() =
// sensorOrientation 90 - display 0, i.e. 90 for the front lens path / 270 here), the
// Mac receiver renders it upright via EXIF 8 (OrientationStage.swift:133), and OBS
// rendered it inverted from the identical wire value.
//
// frame.flip is a VERTICAL flip (GS_FLIP_V), and a horizontal mirror is R180 composed
// with it. Writing Rccw/Rcw for the two directions, Hm for the wire's horizontal
// mirror and Vf for OBS's vertical flip, the wire asks for Hm . Rcw(rot) while OBS
// produces Rccw(obs_rotation) . Vf. Using Hm = R180 . Vf and Vf . Rcw(t) = Rccw(t) . Vf:
//
//     mirror 0:  Rccw(obs) = Rcw(rot)                  -> obs = (360 - rot) % 360
//     mirror 1:  Rccw(obs) . Vf = R180 . Vf . Rcw(rot)
//                              = Rccw(rot + 180) . Vf  -> obs = (rot + 180) % 360
//
//     wire rot  mirror  ->  obs_rotation  frame.flip
//            0       0            0         false
//            0       1          180          true
//           90       0          270         false
//           90       1          270          true
//          180       0          180         false
//          180       1            0          true
//          270       0           90         false
//          270       1           90          true
//
// (The previous table was verified against the old CPU kernel rather than against an
// upright image, so both implementations shared the same inverted convention and the
// comparison could not catch it. Verify this one against a real portrait capture.)
//
// NOTE for users who had set the Rotation dropdown to compensate: it composes on top
// of the wire angle, so a previously-compensating value must go back to 0.
static void spancam_wire_to_obs_transform(int rot, int mir, long *obs_rotation, bool *flip)
{
	*flip = mir != 0;
	rot = ((rot % 360) + 360) % 360;
	*obs_rotation = mir ? (rot + 180) % 360 : (360 - rot) % 360;
}

// One frame out to OBS. Both decode backends land here, so the geometry, colour
// and pacing rules live in exactly one place.
static void spancam_emit_frame(struct spancam_source *ctx, enum video_format fmt, int width, int height,
			       const uint8_t *const *planes, const int *strides, bool full_range, int64_t pts_us)
{
	struct obs_source_frame frame = {0};
	frame.format = fmt;

	// Pace on the wire PTS laid onto a monotonic OBS timebase. Re-anchor on the
	// first frame, on a PTS that goes backwards (the encoder restarted) and on a
	// wild jump forwards, so a corrupt PTS cannot strand the source in the future
	// with every later frame dropped as too old.
	int64_t rel_ns = (pts_us - ctx->ts_base_pts_us) * 1000;
	if (!ctx->ts_base_set || pts_us < ctx->ts_base_pts_us || rel_ns > 10000000000LL) {
		ctx->ts_base_set = true;
		ctx->ts_base_pts_us = pts_us;
		ctx->ts_base_obs_ns = (int64_t)os_gettime_ns();
		rel_ns = 0;
	}
	frame.timestamp = (uint64_t)(ctx->ts_base_obs_ns + rel_ns);

	// Geometry is the GPU's job — see spancam_wire_to_obs_transform.
	pthread_mutex_lock(&ctx->cfg_lock);
	int rot = (ctx->wire_rotation + ctx->user_rotation) % 360;
	int mir = ctx->wire_mirror;
	pthread_mutex_unlock(&ctx->cfg_lock);
	long obs_rotation = 0;
	bool flip = false;
	spancam_wire_to_obs_transform(rot, mir, &obs_rotation, &flip);
	if (obs_rotation != ctx->applied_rotation) {
		obs_source_set_async_rotation(ctx->source, obs_rotation);
		ctx->applied_rotation = obs_rotation;
	}
	frame.flip = flip;
	frame.width = (uint32_t)width;
	frame.height = (uint32_t)height;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		frame.data[i] = (uint8_t *)planes[i];
		frame.linesize[i] = (uint32_t)strides[i];
	}

	enum video_range_type range = full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	frame.full_range = full_range;
	video_format_get_parameters_for_format(VIDEO_CS_709, range, fmt, frame.color_matrix, frame.color_range_min,
					       frame.color_range_max);

	obs_source_output_video(ctx->source, &frame);
	ctx->decoded_frames++;
}

#if defined(SPANCAM_USE_VTDEC)
// ---------------------------------------------------------------- VideoToolbox

static void spancam_vt_on_frame(void *opaque, const struct spancam_vtdec_frame *f)
{
	struct spancam_source *ctx = opaque;
	const uint8_t *planes[MAX_AV_PLANES] = {0};
	int strides[MAX_AV_PLANES] = {0};
	for (int i = 0; i < f->plane_count && i < (int)MAX_AV_PLANES; i++) {
		planes[i] = f->data[i];
		strides[i] = f->linesize[i];
	}
	// We ask VideoToolbox for NV12 and it is what the hardware decoder produces,
	// so this is the only format that can arrive.
	spancam_emit_frame(ctx, VIDEO_FORMAT_NV12, f->width, f->height, planes, strides, f->full_range, f->pts_us);
}

// What this machine can decode and present. VideoToolbox is hardware-backed on
// every Mac that can run OBS, so the only real question is how much the rest of
// the pipeline can carry — and with rotation now on the GPU that is generous.
static void spancam_probe_caps(uint32_t *max_w, uint32_t *max_h)
{
	static uint32_t cw = 0, ch = 0;
	if (!cw) {
		int cores = (int)os_get_logical_cores();
		cw = cores >= 4 ? 3840 : 1920;
		ch = cores >= 4 ? 2160 : 1080;
		obs_log(LOG_INFO, "Spancam: decode caps %ux%u (VideoToolbox, %d cores)", cw, ch, cores);
	}
	*max_w = cw;
	*max_h = ch;
}

static bool spancam_open_decoder(struct spancam_source *ctx, uint8_t codec_byte)
{
	ctx->vtdec = spancam_vtdec_create(codec_byte, spancam_vt_on_frame, ctx);
	return ctx->vtdec != NULL;
}

static void spancam_close_decoder(struct spancam_source *ctx)
{
	if (ctx->vtdec) {
		spancam_vtdec_destroy(ctx->vtdec);
		ctx->vtdec = NULL;
	}
}

// type 1 = parameter sets, type 2 = one access unit. Returns false when the frame
// could not be decoded, which the caller answers with a keyframe request.
static bool spancam_feed(struct spancam_source *ctx, uint8_t type, const uint8_t *data, int size, int64_t pts_us)
{
	if (!ctx->vtdec)
		return false;
	if (type == 1)
		return spancam_vtdec_set_parameter_sets(ctx->vtdec, data, (size_t)size);
	return spancam_vtdec_decode(ctx->vtdec, data, (size_t)size, pts_us);
}

#else
// ---------------------------------------------------------------- libavcodec
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

// Latency settings, applied before avcodec_open2 on both the HW and SW paths.
// LOW_DELAY tells the decoder not to hold frames back for reordering, which is
// correct here because the phone encodes without B-frames — DTS equals PTS, so
// there is nothing to reorder and any buffering is pure added latency.
//
// FF_THREAD_SLICE, and deliberately NOT FF_THREAD_FRAME. Measured, on 5575 frames
// of real 720p off an S24 Ultra, decoding to null on an M4:
//
//     thread_type   threads   low_delay      time
//     slice               1   on            7.17s
//     slice               8   on            7.31s
//     frame+slice         8   on            7.21s   <- no effect
//     slice               8   off           7.15s   <- slice never parallelises
//     frame+slice         8   off           1.90s   <- 3.8x, but only without low_delay
//
// Two things fall out. Slice threading gains nothing here at all, because
// MediaCodec emits ONE SLICE PER PICTURE — no slice-size key, B-frames off — so
// there is nothing to divide. And frame threading is mutually exclusive with
// LOW_DELAY: asking for both silently gets neither, which is why adding
// FF_THREAD_FRAME on its own was a no-op.
//
// So the real choice is latency against throughput, and low latency wins, because
// the throughput problem is already solved upstream: spancam_probe_caps pins a
// machine with no hardware decoder to 720p, and 720p decodes single-threaded at
// ~775 fps here — still 75-150 fps on a machine five to ten times slower, i.e.
// comfortably clear of 30. Buying 3.8x we do not need would cost up to N-1 frames
// of pipeline (~230 ms at 30 fps with 8 threads) on a live camera.
static void spancam_tune_decoder(AVCodecContext *c)
{
	c->flags |= AV_CODEC_FLAG_LOW_DELAY;
	c->flags2 |= AV_CODEC_FLAG2_FAST;
	c->thread_type = FF_THREAD_SLICE;
}

// libavcodec's get_format callback. Hardware decode does not work without it:
// the default callback only auto-selects hardware pixel formats flagged
// METHOD_INTERNAL, and the ones we want (VideoToolbox, D3D11VA, VA-API) are
// METHOD_HW_DEVICE_CTX, so it quietly settles for the software format instead.
// No error, no warning, frames still arrive — just off the CPU decoder. Holding
// out for our format here is what actually engages the hwaccel. Installed before
// avcodec_open2, since open can call it.
static enum AVPixelFormat spancam_get_format(struct AVCodecContext *avctx, const enum AVPixelFormat *fmts)
{
	struct spancam_source *ctx = avctx->opaque;
	if (ctx) {
		for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == ctx->hw_pix_fmt)
				return *p;
		}
	}
	return fmts[0]; // not on offer — take the software format and carry on
}

// Hardware decoders worth trying, best first, per platform. Falling off the end
// of the list is not a failure — it just means software decode.
static const enum AVHWDeviceType spancam_hw_types[] = {
#if defined(__APPLE__)
	AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(_WIN32)
	AV_HWDEVICE_TYPE_D3D11VA,
	AV_HWDEVICE_TYPE_CUDA,
#else
	AV_HWDEVICE_TYPE_VAAPI,
	AV_HWDEVICE_TYPE_CUDA,
	AV_HWDEVICE_TYPE_VDPAU,
#endif
	AV_HWDEVICE_TYPE_NONE,
};

// Ask the decoder itself which pixel format goes with this device type. Guessing
// is how you end up with a get_format that never matches.
static enum AVPixelFormat spancam_hw_pix_fmt(const AVCodec *codec, enum AVHWDeviceType type)
{
	for (int i = 0;; i++) {
		const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
		if (!cfg)
			return AV_PIX_FMT_NONE;
		if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type)
			return cfg->pix_fmt;
	}
}

// What this computer can realistically DECODE AND TRANSFORM at 30 fps, declared to
// the phone so it never picks a rung this end cannot keep up with.
//
// The phone alone cannot know this. It picks its rung from the transport, so a
// cabled phone asks for its top rung — 4K30 on an S24 Ultra — and a mid-range
// laptop then has to hardware-download 12.4 MB per frame and run a scalar
// per-pixel rotate over it. Measured at 4K with rotation on an Apple M4: 10.9
// ms/frame for the transform alone, i.e. a 92 fps single-core ceiling on one of
// the fastest cores available. On the low-end laptops most OBS users actually
// have, and with software decode, 4K30 cannot hold 30 fps at all — it presents as
// judder and growing delay, which looks exactly like a network problem.
//
// The test is deliberately conservative: 4K only with a working hardware decoder
// AND enough cores to absorb the transform; software decode is capped at 720p,
// because software 4K H.264 is not a real-time proposition on this class of
// machine. Probed once per process — opening a hw device is not free.
static void spancam_probe_caps(uint32_t *max_w, uint32_t *max_h)
{
	static uint32_t cached_w = 0, cached_h = 0;
	if (cached_w) {
		*max_w = cached_w;
		*max_h = cached_h;
		return;
	}
	bool hw = false;
	for (int i = 0; spancam_hw_types[i] != AV_HWDEVICE_TYPE_NONE && !hw; i++) {
		AVBufferRef *probe = NULL;
		if (av_hwdevice_ctx_create(&probe, spancam_hw_types[i], NULL, NULL, 0) == 0) {
			hw = true;
			av_buffer_unref(&probe);
		}
	}
	int cores = (int)os_get_logical_cores();
	if (hw && cores >= 8) {
		cached_w = 3840;
		cached_h = 2160;
	} else if (hw || cores >= 8) {
		cached_w = 1920;
		cached_h = 1080;
	} else {
		cached_w = 1280;
		cached_h = 720;
	}
	obs_log(LOG_INFO, "Spancam: decode caps %ux%u (hardware decode %s, %d cores)", cached_w, cached_h,
		hw ? "available" : "NOT available", cores);
	*max_w = cached_w;
	*max_h = cached_h;
}

static bool spancam_open_decoder(struct spancam_source *ctx, uint8_t codec_byte)
{
	enum AVCodecID id = (codec_byte == 1) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	ctx->codec = avcodec_find_decoder(id);
	if (!ctx->codec) {
		obs_log(LOG_ERROR, "Spancam: no decoder for %s", codec_byte == 1 ? "HEVC" : "H.264");
		return false;
	}

	ctx->hw_decode = false;
	ctx->hw_pix_fmt = AV_PIX_FMT_NONE;

	// Try each candidate hardware decoder in turn. Best-effort throughout: any
	// step failing moves to the next candidate and ultimately to the software
	// open below, so a machine with no usable GPU decoder — or a profile one
	// won't take — still streams. Ordering is load-bearing: create the device,
	// set hw_device_ctx AND get_format AND opaque, and only then avcodec_open2.
	for (int i = 0; spancam_hw_types[i] != AV_HWDEVICE_TYPE_NONE; i++) {
		enum AVHWDeviceType type = spancam_hw_types[i];
		enum AVPixelFormat pix = spancam_hw_pix_fmt(ctx->codec, type);
		if (pix == AV_PIX_FMT_NONE)
			continue; // this decoder can't drive that device type
		if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, type, NULL, NULL, 0) != 0)
			continue; // no such device on this machine
		ctx->ctx = avcodec_alloc_context3(ctx->codec);
		if (ctx->ctx) {
			ctx->hw_pix_fmt = pix;
			ctx->ctx->opaque = ctx;
			ctx->ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
			ctx->ctx->get_format = spancam_get_format;
			spancam_tune_decoder(ctx->ctx);
			if (ctx->ctx->hw_device_ctx && avcodec_open2(ctx->ctx, ctx->codec, NULL) == 0) {
				ctx->frame = av_frame_alloc();
				ctx->sw_frame = av_frame_alloc();
				if (ctx->frame && ctx->sw_frame) {
					ctx->hw_decode = true;
					obs_log(LOG_INFO, "Spancam: %s hardware decode enabled",
						av_hwdevice_get_type_name(type));
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
		ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
	}
	obs_log(LOG_INFO, "Spancam: no hardware decoder available — using software decode");

	// Software path. Allocate a fresh context so a half-configured HW one is
	// never reused.
	ctx->ctx = avcodec_alloc_context3(ctx->codec);
	if (!ctx->ctx)
		return false;
	spancam_tune_decoder(ctx->ctx);
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
	ctx->hw_pix_fmt = AV_PIX_FMT_NONE;
}

// Feed one Annex-B access unit (or the codec config) to the decoder and emit
// whatever frames come back out.
static bool spancam_decode(struct spancam_source *ctx, const uint8_t *data, int size, int64_t pts_us)
{
	if (!ctx->ctx)
		return false;

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
		return false;
	}

	while (avcodec_receive_frame(ctx->ctx, ctx->frame) == 0) {
		AVFrame *f = ctx->frame;

		// A hardware-decoded surface lives on the GPU. Pull it down into the
		// reusable sw_frame (leaving the format unset lets FFmpeg choose, which is
		// NV12 on every backend here) and run the rest of the loop against that.
		// A failed transfer drops this one frame rather than tearing the decoder
		// down — a transient surface hiccup shouldn't end the stream.
		if (ctx->hw_decode && f->format == ctx->hw_pix_fmt) {
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

		// Compose the two sources at use time. The phone's live orientation still
		// follows the device, and the dropdown still corrects a phone that reports
		// wrong (or an iPhone, which reports 0 today) — neither erases the other.
		pthread_mutex_lock(&ctx->cfg_lock);
		int rot = (ctx->wire_rotation + ctx->user_rotation) % 360;
		int mir = ctx->wire_mirror;
		pthread_mutex_unlock(&ctx->cfg_lock);

		// Hand OBS the decoded planes as they are; the GPU does the geometry.
		long obs_rotation = 0;
		bool flip = false;
		spancam_wire_to_obs_transform(rot, mir, &obs_rotation, &flip);
		if (obs_rotation != ctx->applied_rotation) {
			obs_source_set_async_rotation(ctx->source, obs_rotation);
			ctx->applied_rotation = obs_rotation;
		}
		frame.flip = flip;
		frame.width = (uint32_t)f->width;
		frame.height = (uint32_t)f->height;
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			frame.data[i] = f->data[i];
			frame.linesize[i] = (uint32_t)f->linesize[i];
		}

		enum video_range_type range = (f->color_range == AVCOL_RANGE_JPEG) ? VIDEO_RANGE_FULL
										   : VIDEO_RANGE_PARTIAL;
		frame.full_range = (range == VIDEO_RANGE_FULL);
		video_format_get_parameters_for_format(VIDEO_CS_709, range, fmt, frame.color_matrix,
						       frame.color_range_min, frame.color_range_max);

		obs_source_output_video(ctx->source, &frame);
		ctx->decoded_frames++;
		av_frame_unref(f);
	}

	return true;
}

static bool spancam_feed(struct spancam_source *ctx, uint8_t type, const uint8_t *data, int size, int64_t pts_us)
{
	UNUSED_PARAMETER(type); // libavcodec takes parameter sets through the same call
	return spancam_decode(ctx, data, size, pts_us);
}
#endif // SPANCAM_USE_VTDEC

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
static spancam_socket_t spancam_dial(struct spancam_source *ctx, char *token_out, size_t toksz, char *label_out,
				     size_t lblsz, bool *used_usb)
{
	pthread_mutex_lock(&ctx->cfg_lock);
	int mode = ctx->connection;
	char *mhost = bstrdup(ctx->host ? ctx->host : "");
	int mport = ctx->port > 0 ? ctx->port : SPANCAM_DEFAULT_PORT;
	char *mtoken = bstrdup(ctx->token ? ctx->token : "");
	char *mdevice = bstrdup(ctx->device ? ctx->device : "");
	pthread_mutex_unlock(&ctx->cfg_lock);

	spancam_socket_t fd = SPANCAM_BAD_SOCKET;
	token_out[0] = 0;
	*used_usb = false;

	// In Auto, skip USB while it's benched (see usb_cooldown_until_ns).
	bool usb_cooling = (mode == SPANCAM_CONN_AUTO) && (ctx->usb_cooldown_until_ns > (int64_t)os_gettime_ns());

	// ---- USB, if a phone is on the end of a CABLE ----
	//
	// A source PINNED to a phone may only take the USB path when exactly one cable is
	// attached. `adb forward` is keyed on an adb serial, and an adb serial cannot be matched
	// to the SDSP device name the user picked — so with two cabled phones the USB branch
	// would tunnel to whichever one adb happened to list first and quietly serve the WRONG
	// camera, which is precisely the guarantee the Phone dropdown exists to make. Wi-Fi can
	// target a phone by name, so that is the honest fallback; the only thing lost is some
	// latency, and only in the two-cables case.
	char adb_serial[128] = {0};
	int cable_count = 0;
	bool usb_ok = (mode == SPANCAM_CONN_USB || (mode == SPANCAM_CONN_AUTO && !usb_cooling)) &&
		      spancam_adb_usb_device_count(adb_serial, sizeof(adb_serial), &cable_count);
	if (usb_ok && *mdevice && cable_count > 1) {
		obs_log(LOG_WARNING,
			"Spancam[%s]: %d cabled phones but this source is pinned to \"%s\" — adb cannot "
			"tell them apart, so staying on Wi-Fi to guarantee the right phone",
			obs_source_get_name(ctx->source), cable_count, mdevice);
		usb_ok = false;
	}
	if (usb_ok) {
		char cmd[1160];
		// -s scopes the forward: with two devices attached an unscoped `adb forward`
		// errors out or silently tunnels to the wrong phone.
		snprintf(cmd, sizeof(cmd), "\"%s\" -s \"%s\" forward tcp:%d tcp:%d" SPANCAM_DEVNULL, spancam_adb_path(),
			 adb_serial, SPANCAM_DEFAULT_PORT, SPANCAM_DEFAULT_PORT);
		spancam_run(cmd);
		fd = spancam_connect("localhost", SPANCAM_DEFAULT_PORT);
		if (fd != SPANCAM_BAD_SOCKET) {
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
	} else if (mode == SPANCAM_CONN_USB) {
		// Explicit USB with no reachable adb device. This used to return with no
		// log at all, so the source sat black and the OBS log said NOTHING for the
		// whole session — the single most confusing failure this plugin had.
		obs_log(LOG_WARNING,
			"Spancam: USB selected but %s — set Connection to Auto or Wi-Fi, "
			"or enable USB debugging and plug the phone in",
			spancam_adb_path() ? "no phone is attached by CABLE (a wireless-adb device is not USB)"
					   : "adb was not found");
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
		} else if (*mdevice) {
			// PINNED to one phone: re-resolve it by name on every dial, so two sources
			// in one scene stay on their own handsets and a relaunched phone app (new
			// access token) or a fresh DHCP lease is picked up automatically.
			struct spancam_device devs[SPANCAM_MAX_DEVICES];
			int n = spancam_enumerate_devices(ctx, devs, SPANCAM_MAX_DEVICES);
			for (int i = 0; i < n; i++) {
				if (strcmp(devs[i].name, mdevice) != 0)
					continue;
				snprintf(host, sizeof(host), "%s", devs[i].host);
				snprintf(token, sizeof(token), "%s", devs[i].token);
				port = devs[i].port;
				resolved = true;
				break;
			}
			if (resolved)
				obs_log(LOG_INFO, "Spancam[%s]: pinned to \"%s\" -> %s:%d",
					obs_source_get_name(ctx->source), mdevice, host, port);
			else
				obs_log(LOG_WARNING,
					"Spancam[%s]: pinned phone \"%s\" is NOT on the network — showing the "
					"setup card rather than grabbing a different phone",
					obs_source_get_name(ctx->source), mdevice);
		} else if (spancam_udp_discover(ctx, host, sizeof(host), &port, token, sizeof(token))) {
			resolved = true; // Android answers the UDP broadcast
#if defined(__APPLE__)
		} else if (spancam_mdns_discover(ctx, host, sizeof(host), &port, token, sizeof(token))) {
			resolved = true; // iOS advertises over Bonjour (mDNS)
#endif
		}
		if (resolved) {
			fd = spancam_connect(host, port);
			if (fd != SPANCAM_BAD_SOCKET) {
				snprintf(token_out, toksz, "%s", token);
				snprintf(label_out, lblsz, "Wi-Fi %s:%d", host, port);
			}
		}
	}

done:
	bfree(mhost);
	bfree(mtoken);
	bfree(mdevice);
	return fd;
}

static void spancam_stream_once(struct spancam_source *ctx)
{
	char token[128] = {0};
	char label[160] = {0};
	bool used_usb = false;
	// Declared before any `goto done` so the teardown can read it. Auto is sampled once so
	// the promotion check never forks adb on a source the user pinned to Wi-Fi or USB.
	bool promoted = false;
	pthread_mutex_lock(&ctx->cfg_lock);
	const bool mode_is_auto = ctx->connection == SPANCAM_CONN_AUTO;
	pthread_mutex_unlock(&ctx->cfg_lock);
	bool got_stream = false; // a valid StreamHeader arrived => this path actually works
	spancam_socket_t fd = spancam_dial(ctx, token, sizeof(token), label, sizeof(label), &used_usb);
	if (fd == SPANCAM_BAD_SOCKET)
		return;
	obs_log(LOG_INFO, "Spancam[%s]: connected (%s)", obs_source_get_name(ctx->source), label);

	// Handshake: "SPANCAM/1 k=<token> app=OBS os=<os> link=<usb|wifi>\n". The token
	// gates Wi-Fi access; app/os only let the phone show who connected.
	//
	// `link` exists because the phone CANNOT work this out for itself. It infers the
	// transport from the peer address, and `adb forward` always arrives on the
	// phone's loopback whether adb is running over a cable or over Wi-Fi — so a
	// wireless-adb tunnel looked like a cable, the phone committed to its 4K rung,
	// and ~25 Mbps went out over Wi-Fi (measured: 10 fps, ~3 s of accumulating
	// delay). This end knows the truth, because it resolved either a cable serial or
	// a wireless one, so it simply says which. A sender that does not understand
	// `link=` ignores it and keeps its old inference.
	struct dstr hello;
	dstr_init(&hello);
	uint32_t cap_w = 0, cap_h = 0;
	spancam_probe_caps(&cap_w, &cap_h);
	dstr_printf(&hello, "SPANCAM/1 k=%s app=OBS os=%s link=%s maxw=%u maxh=%u\n", token, SPANCAM_OS,
		    used_usb ? "usb" : "wifi", cap_w, cap_h);
	send(fd, hello.array, (int)hello.len, 0);
	dstr_free(&hello);

	uint8_t hdr[24];
	if (!spancam_read_full(ctx, fd, hdr, sizeof(hdr), SPANCAM_HEADER_TIMEOUT_MS) || be32(hdr) != SPANCAM_MAGIC) {
		obs_log(LOG_WARNING, "Spancam: bad or missing stream header (%s)", label);
		goto done;
	}
	// flags bit0 seeds mirror at connect time; the live type-3 packet that follows
	// is the actual source of truth for both mirror and rotation.
	pthread_mutex_lock(&ctx->cfg_lock);
	ctx->wire_mirror = hdr[5] & 1;
	pthread_mutex_unlock(&ctx->cfg_lock);
	uint8_t codec_byte = hdr[4];
	if (!spancam_open_decoder(ctx, codec_byte))
		goto done;
	got_stream = true;
	if (used_usb)
		ctx->usb_cooldown_until_ns = 0; // USB delivered — keep preferring it
	ctx->ts_base_set = false;               // re-anchor the pacing grid for this connection
	ctx->wire_packets = ctx->decoded_frames = 0;
	ctx->wd_last_wire = ctx->wd_last_decoded = 0;
	ctx->wd_dead_secs = 0;
	ctx->wd_next_tick_ns = (int64_t)os_gettime_ns() + 1000000000LL;
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

	// Packet pump. The decoder input buffer carries AV_INPUT_BUFFER_PADDING_SIZE
	// zeroed bytes past the payload: libavcodec's bitstream readers are allowed to
	// read a little beyond the end of the data, and without the padding that is an
	// out-of-bounds read that mostly gets away with it.
	size_t cap = 1 << 20;
	uint8_t *payload = bmalloc(cap + AV_INPUT_BUFFER_PADDING_SIZE);
	for (;;) {
		// BLACK-STREAM WATCHDOG, the Mac receiver's decodeWatchdogTick: packets
		// arriving but zero frames coming out means the decode pipeline is wedged,
		// and no amount of waiting fixes it. Wire-quiet is NOT this watchdog's
		// business — the read deadline above owns that. Five sustained seconds
		// cannot be a transient hiccup, so tear the connection down and let the
		// redial rebuild the decoder from a clean slate.
		int64_t wnow = (int64_t)os_gettime_ns();
		if (wnow >= ctx->wd_next_tick_ns) {
			ctx->wd_next_tick_ns = wnow + 1000000000LL;
			bool wire_adv = ctx->wire_packets != ctx->wd_last_wire;
			bool dec_adv = ctx->decoded_frames != ctx->wd_last_decoded;
			ctx->wd_last_wire = ctx->wire_packets;
			ctx->wd_last_decoded = ctx->decoded_frames;
			if (dec_adv || !wire_adv)
				ctx->wd_dead_secs = 0;
			else if (++ctx->wd_dead_secs >= SPANCAM_DECODE_DEAD_SECS) {
				obs_log(LOG_ERROR,
					"Spancam: packets flowing but ZERO frames decoded for %d s — "
					"decode pipeline dead, reconnecting",
					ctx->wd_dead_secs);
				break;
			}

			// USB PROMOTION, the Mac path's Wi-Fi->USB hot swap.
			//
			// Transport used to be decided once, inside spancam_dial, so a cable plugged
			// in DURING a Wi-Fi session was invisible until that session happened to drop
			// — the switch eventually arrived but on no predictable schedule, and the
			// latency win with it. The Mac receiver does not wait: the phone orchestrates
			// the promotion the moment the accessory opens. Here the plugin is the dialer,
			// so the plugin has to watch.
			//
			// Only while streaming over Wi-Fi in Auto, only every few seconds (each check
			// forks adb), and only when a CABLE device is present — a wireless-adb device
			// is not a cable and promoting to it would push the USB rung over Wi-Fi.
			if (!used_usb && !promoted && mode_is_auto && wnow >= ctx->usb_probe_next_ns) {
				ctx->usb_probe_next_ns = wnow + SPANCAM_USB_PROBE_NS;
				char cable[128] = {0};
				if (spancam_adb_usb_device(cable, sizeof(cable))) {
					obs_log(LOG_INFO,
						"Spancam[%s]: USB cable attached — promoting from Wi-Fi to USB",
						obs_source_get_name(ctx->source));
					promoted = true;
					break;
				}
				// Back off the PROBE itself once promotions keep failing. Without this a
				// USB path that connects but never streams is retried the instant the 8 s
				// bench expires, forever: promote, fail, bench, fall back, promote… which
				// on a phone that rebuilds its camera per rung is a permanent reconnect
				// storm rather than a graceful "USB is not working, stay on Wi-Fi".
				if (ctx->promote_fails >= 2)
					ctx->usb_probe_next_ns = wnow + 60000000000LL; // 60 s
			}
		}

		uint8_t ph[16];
		if (!spancam_read_full(ctx, fd, ph, sizeof(ph), SPANCAM_STREAM_TIMEOUT_MS))
			break;
		ctx->wire_packets++;
		uint8_t type = ph[0];
		int64_t pts_us = (int64_t)be64(ph + 4);
		uint32_t size = be32(ph + 12);
		if (size == 0 || size > (64u << 20)) // sanity cap 64 MiB
			break;
		if (size > cap) {
			cap = size;
			payload = brealloc(payload, cap + AV_INPUT_BUFFER_PADDING_SIZE);
		}
		if (!spancam_read_full(ctx, fd, payload, size, SPANCAM_STREAM_TIMEOUT_MS))
			break;
		memset(payload + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		if (type == 1 || type == 2) { // config and frames both go to the decoder
			if (type == 2)
				spancam_abr_observe(ctx, pts_us);
			if (!spancam_feed(ctx, type, payload, (int)size, pts_us)) {
				// Decode error or desync: ask for a fresh IDR rather than showing
				// smeared garbage until the next scheduled one. Debounced, because
				// a corrupt burst is many failures in a row and one IDR each would
				// bury a link that is already in trouble.
				int64_t kfnow = (int64_t)os_gettime_ns();
				if (kfnow - ctx->last_kf_request_ns > 500000000LL) {
					ctx->last_kf_request_ns = kfnow;
					spancam_send_keyframe(ctx);
				}
			}
		} else if (type == 3 && size >= 2) {
			// Transform: mirror + rotation, applied on this end. Written under
			// the same lock the decode loop reads it with, so a change lands on
			// the very next frame.
			pthread_mutex_lock(&ctx->cfg_lock);
			ctx->wire_mirror = payload[0] & 1;
			ctx->wire_rotation = (payload[1] & 3) * 90;
			pthread_mutex_unlock(&ctx->cfg_lock);
		}
	}
	bfree(payload);

done:
	// Connected over USB but never got a header => bench USB so Auto tries Wi-Fi
	// for a while instead of spinning on a tunnel that connects and then says
	// nothing. A USB connection that DID stream cleared the cooldown above, so an
	// ordinary disconnect reconnects straight back over USB.
	// A connection that produced frames resets the ladder: an ordinary disconnect
	// after a good session must redial in ~1 s, not at whatever rung an earlier
	// outage had climbed to.
	if (ctx->decoded_frames > 0)
		ctx->redials = 0;
	else if (ctx->redials < SPANCAM_MAX_REDIALS)
		ctx->redials++;
	if (used_usb && !got_stream)
		ctx->usb_cooldown_until_ns = (int64_t)os_gettime_ns() + 8000000000LL; // 8 s
	// A deliberate promotion is not a failure: reset the redial ladder so the USB dial
	// happens on the next tick instead of waiting out a backoff earned by real errors.
	if (promoted) {
		ctx->redials = 0;
		ctx->promote_now = true;
	}
	// A promotion that connected over USB but produced no stream is a FAILED promotion.
	// Count them so the probe above can stop hammering a USB path that does not work.
	if (used_usb) {
		if (got_stream)
			ctx->promote_fails = 0;
		else if (ctx->promote_fails < 100)
			ctx->promote_fails++;
	}
	ctx->ctrl_fd = SPANCAM_BAD_SOCKET; // the control channel goes with the connection
	spancam_close_decoder(ctx);
	spancam_closesocket(fd);
	// Tell OBS the source has no picture. Without this the async texture stays
	// active and OBS keeps rendering the last frame it was given — forever. Pull
	// the cable mid-stream and your face stays on the broadcast, frozen, with no
	// indication anything is wrong. Idempotent, so calling it on every disconnect
	// is fine.
	obs_source_output_video(ctx->source, NULL);
}

// The card is authored upright — but obs_source_set_async_rotation is STICKY. Whatever
// rotation the phone's video last needed stays on the source, and OBS keeps applying it to
// every later frame, including this one. A source whose phone streamed at 90 degrees showed
// the setup card lying on its side, which read as "the orientation logic is broken" when the
// card itself was fine.
//
// Zero the rotation AND the cached value, so the next real frame sees a mismatch and
// re-applies whatever the phone actually needs. Flip needs no undoing: it travels per frame,
// and the card simply does not set it.
static void spancam_show_card(struct spancam_source *ctx)
{
	if (ctx->applied_rotation != 0) {
		obs_source_set_async_rotation(ctx->source, 0);
		ctx->applied_rotation = 0;
	}
	spancam_placeholder_output(ctx->source);
}

// The Mac receiver's paced redial ladder: 1/6/12/24/48/60 s. A phone that is
// simply gone should not be dialled at 1.5 s forever — that is a busy loop
// against the void which, with a blocking connect, also keeps a thread hot and
// (in Auto) re-runs `adb devices` every cycle. Anything that streamed resets to
// the bottom rung, so a real blip still recovers in about a second.
static int spancam_redial_ms(int redials)
{
	static const int ladder[] = {1000, 6000, 12000, 24000, 48000, 60000};
	const int rungs = (int)(sizeof(ladder) / sizeof(ladder[0]));
	if (redials < 0)
		redials = 0;
	return redials >= rungs ? ladder[rungs - 1] : ladder[redials];
}

// Stop dialling while the source is neither on-screen nor in the program feed.
//
// The receive thread used to run from create to destroy, so a Spancam source sitting in an
// inactive scene kept a phone streaming: its camera stayed lit (privacy indicator on), its
// battery drained, its encoder ran, and this end decoded every frame — for a source nobody
// could see. It also meant a phone could be held by a hidden source while the user wondered
// why the scene they were looking at could not have it.
//
// Keyed on active OR showing, deliberately: `active` is the program feed, `showing` covers
// the multiview and the properties preview, and a preview-only workflow must keep working.
static bool spancam_should_run(struct spancam_source *ctx)
{
	return obs_source_active(ctx->source) || obs_source_showing(ctx->source);
}

// OBS calls these on the show/hide and activate/deactivate edges. Nothing to do but wake the
// loop: it re-checks spancam_should_run itself, so the edge only needs to interrupt the wait.
static void spancam_source_show(void *data)
{
	struct spancam_source *ctx = data;
	os_event_signal(ctx->wake);
}

static void spancam_source_hide(void *data)
{
	struct spancam_source *ctx = data;
	os_event_signal(ctx->wake);
}

static void *spancam_receive_loop(void *data)
{
	struct spancam_source *ctx = data;
	while (os_event_try(ctx->stop_signal) == EAGAIN) {
		if (!spancam_should_run(ctx)) {
			// Idle: release the phone and wait to be shown again. Logged once per
			// transition so the OBS log explains a source that stopped on purpose.
			if (!ctx->idled) {
				ctx->idled = true;
				obs_log(LOG_INFO, "Spancam[%s]: source hidden — releasing the phone's camera",
					obs_source_get_name(ctx->source));
			}
			os_event_reset(ctx->wake);
			os_event_timedwait(ctx->wake, 500);
			continue;
		}
		if (ctx->idled) {
			ctx->idled = false;
			obs_log(LOG_INFO, "Spancam[%s]: source visible again — reconnecting",
				obs_source_get_name(ctx->source));
			ctx->redials = 0; // a deliberate resume is not a failure
		}
		// ANY time this source has no live video, the user gets the card. No phone
		// picked, a phone that is not on the network, a phone that just dropped — from
		// where they are sitting these are one situation: nothing is on screen and they
		// need to be told what to do about it.
		//
		// Pushed BEFORE the dial as well as after, because a dial to an unreachable
		// phone blocks on connect for seconds, and earlier versions of this showed a
		// black rectangle for that whole time. Also note a FAILED dial never reaches the
		// disconnect path further down, so posting the card only there left exactly the
		// most common case — a phone that is simply not there — showing nothing at all.
		if (spancam_should_run(ctx))
			spancam_show_card(ctx);

		spancam_stream_once(ctx);

		// stream_once only ever returns when there is no video: the dial failed, or the
		// connection it had just ended. Either way, put the card back up.
		if (spancam_should_run(ctx))
			spancam_show_card(ctx);

		int wait = spancam_redial_ms(ctx->redials);
		if (ctx->promote_now) {
			ctx->promote_now = false;
			wait = 100; // promoting to a cable — reconnect immediately
		}
		if (ctx->redials == SPANCAM_MAX_REDIALS)
			obs_log(LOG_INFO,
				"Spancam: %d redials with no video — backing off to %d s; "
				"the phone is not reachable, check it is on this network and Discoverable",
				ctx->redials, wait / 1000);
		// Wait out the redial ladder in SLICES. The card is composed for one specific
		// canvas, and the ladder backs off to 60 s — so a single long sleep meant that
		// changing the canvas resolution, or flipping it to vertical, left a
		// wrongly-shaped card on screen for up to a minute. Re-push only once the
		// composition has actually gone stale, so the normal case costs two integer
		// compares per slice and no frame traffic at all.
		int left = wait;
		while (left > 0 && os_event_try(ctx->stop_signal) == EAGAIN) {
			const int slice = left > 500 ? 500 : left;
			os_event_timedwait(ctx->stop_signal, slice);
			left -= slice;
			if (spancam_should_run(ctx) && spancam_placeholder_stale())
				spancam_show_card(ctx);
		}
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
	obs_data_set_default_int(settings, "rotation", 0);
}

// Keep the "Name" field and the real source name in agreement.
//
// Two directions, and BOTH are needed. Writing the live name into the setting means the
// field shows the current name instead of being blank. Listening for OBS's own "rename"
// signal means a right-click rename updates the setting too — without that, the stored
// label would still hold the OLD name and the next properties-OK would silently rename the
// source back, which is worse than not having the field at all.
static void spancam_sync_label(struct spancam_source *ctx)
{
	const char *name = obs_source_get_name(ctx->source);
	if (!name)
		return;
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	if (!settings)
		return;
	const char *cur = obs_data_get_string(settings, "label");
	if (!cur || strcmp(cur, name) != 0)
		obs_data_set_string(settings, "label", name);
	obs_data_release(settings);
}

static void spancam_renamed(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	spancam_sync_label((struct spancam_source *)data);
}

// Fill the device dropdown from a live scan. "Any" stays first so the default is the
// old auto-pick behaviour; every phone found is then listed by name.
static void spancam_fill_devices(struct spancam_source *ctx, obs_property_t *list)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Spancam.Device.Any"), "");
	if (!ctx)
		return;
	struct spancam_device devs[SPANCAM_MAX_DEVICES];
	int n = spancam_enumerate_devices(ctx, devs, SPANCAM_MAX_DEVICES);

	pthread_mutex_lock(&ctx->cfg_lock);
	char *pinned = bstrdup(ctx->device ? ctx->device : "");
	pthread_mutex_unlock(&ctx->cfg_lock);
	bool pinned_listed = false;

	for (int i = 0; i < n; i++) {
		struct dstr label;
		dstr_init(&label);
		dstr_printf(&label, "%s (%s)", devs[i].name, devs[i].host);
		obs_property_list_add_string(list, label.array, devs[i].name);
		dstr_free(&label);
		if (*pinned && strcmp(devs[i].name, pinned) == 0)
			pinned_listed = true;
	}

	// A LIST-type combo snaps to item 0 whenever the stored value is missing, and OBS
	// writes that back — so a single scan that did not see the phone (asleep, Discoverable
	// just toggled, a lost mDNS packet) silently reset the source to "Any", which then
	// auto-picked whichever phone answered first. That is exactly the "I selected iPhone
	// but Android was streaming" failure. Re-adding the pinned name keeps the selection
	// intact across a miss, and says plainly that it is not currently reachable.
	if (*pinned && !pinned_listed) {
		struct dstr label;
		dstr_init(&label);
		dstr_printf(&label, "%s %s", pinned, obs_module_text("Spancam.Device.Offline"));
		obs_property_list_add_string(list, label.array, pinned);
		dstr_free(&label);
	}
	ctx->last_scan_n = n;
	obs_log(LOG_INFO, "Spancam[%s]: device scan found %d phone(s), pinned=\"%s\"%s",
		obs_source_get_name(ctx->source), n, pinned, (*pinned && !pinned_listed) ? " (not reachable)" : "");
	bfree(pinned);
}

// The hint is only shown when the dropdown has nothing real in it. An always-on line of
// text next to a working dropdown is noise; the same line when the list is empty is the
// only thing on screen that says a phone app exists at all.
static void spancam_sync_getapp(obs_properties_t *props, struct spancam_source *ctx)
{
	obs_property_t *hint = obs_properties_get(props, "getapp");
	if (hint && ctx)
		obs_property_set_visible(hint, ctx->last_scan_n == 0);
}

// "Refresh" re-scans. A scan takes about a second (UDP window + Bonjour resolve), so it
// is deliberately a button rather than something that runs on every dialog repaint.
static bool spancam_refresh_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	obs_property_t *list = obs_properties_get(props, "device");
	if (list)
		spancam_fill_devices((struct spancam_source *)data, list);
	spancam_sync_getapp(props, (struct spancam_source *)data);
	return true;
}

static obs_properties_t *spancam_source_get_properties(void *data)
{
	struct spancam_source *sctx = data;
	obs_properties_t *props = obs_properties_create();

	// THIS SOURCE's name, editable right here. With several phones a scene otherwise reads
	// "Spancam Camera", "Spancam Camera 2"… and nothing says which handset each one holds.
	// OBS's own right-click Rename works but is easy to miss, so the name sits directly above
	// the phone it is bound to. Seeded with the live source name (see spancam_sync_label) so
	// the field always shows the current name rather than being confusingly blank.
	obs_properties_add_text(props, "label", obs_module_text("Spancam.Prop.Label"), OBS_TEXT_DEFAULT);
	if (sctx)
		spancam_sync_label(sctx);

	// WHICH PHONE this source uses. Populated by a live scan on dialog open, so two
	// sources in one scene can be pinned to two different handsets — previously both
	// raced onto whichever phone answered first and then evicted each other forever,
	// because each phone serves exactly one SDSP client.
	obs_property_t *dev = obs_properties_add_list(props, "device", obs_module_text("Spancam.Prop.Device"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	spancam_fill_devices((struct spancam_source *)data, dev);
	obs_properties_add_button2(props, "refresh", obs_module_text("Spancam.Prop.Refresh"), spancam_refresh_clicked,
				   data);

	// Nothing in the properties dialog can render an image — there is no image property
	// type, and the source-list icon comes from the fixed obs_icon_type enum — so the
	// branding lives on the idle card in the canvas instead. What CAN go here is the
	// thing a stuck user actually needs: where to get the phone app. Hidden whenever the
	// scan found something, so it never nags someone whose phone is already listed.
	obs_properties_add_text(props, "getapp", obs_module_text("Spancam.Prop.GetApp"), OBS_TEXT_INFO);
	spancam_sync_getapp(props, sctx);

	obs_property_t *conn = obs_properties_add_list(props, "connection", obs_module_text("Spancam.Prop.Connection"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Auto"), SPANCAM_CONN_AUTO);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Usb"), SPANCAM_CONN_USB);
	obs_property_list_add_int(conn, obs_module_text("Spancam.Conn.Wifi"), SPANCAM_CONN_WIFI);
	obs_property_t *rot = obs_properties_add_list(props, "rotation", obs_module_text("Spancam.Prop.Rotation"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rot, "0° (follow phone)", 0);
	obs_property_list_add_int(rot, "90°", 90);
	obs_property_list_add_int(rot, "180°", 180);
	obs_property_list_add_int(rot, "270°", 270);
	obs_properties_add_text(props, "host", obs_module_text("Spancam.Prop.Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "token", obs_module_text("Spancam.Prop.Token"), OBS_TEXT_DEFAULT);
	return props;
}

static void spancam_source_update(void *data, obs_data_t *settings)
{
	struct spancam_source *ctx = data;
	pthread_mutex_lock(&ctx->cfg_lock);
	bfree(ctx->host);
	bfree(ctx->token);
	bfree(ctx->device);
	ctx->host = NULL;
	ctx->token = NULL;
	// WHICH PHONE this source is bound to. Reading this is what makes the "Phone"
	// dropdown authoritative; while it was missing here `ctx->device` stayed NULL no
	// matter what the dialog showed, so every source silently fell through to
	// auto-discovery and grabbed whichever phone answered first. With two sources that
	// meant both landed on the SAME phone and — since a phone serves one SDSP client and
	// evicts the previous one — they evicted each other about twice a second. That is
	// exactly the "I selected iPhone but Android was streaming" report and the ~2 Hz
	// flicker in both the OBS preview and the phone's own UI.
	ctx->device = bstrdup(obs_data_get_string(settings, "device"));
	ctx->connection = (int)obs_data_get_int(settings, "connection");
	// One address field, not two. The port is 8892 on every phone unless Android's
	// bind fell back to an ephemeral one, which is rare enough that a whole spin box
	// for it was worse than letting the rare user type "192.168.1.5:41337". Only a
	// single trailing ":<digits>" counts, so an IPv6 literal or a bare name is left
	// alone.
	const char *host_in = obs_data_get_string(settings, "host");
	ctx->port = SPANCAM_DEFAULT_PORT;
	const char *colon = strrchr(host_in, ':');
	if (colon && colon != host_in && strchr(host_in, ':') == colon && colon[1]) {
		const char *d = colon + 1;
		bool digits = true;
		for (; *d; d++)
			if (*d < '0' || *d > '9')
				digits = false;
		int p = digits ? atoi(colon + 1) : 0;
		if (p >= 1 && p <= 65535) {
			ctx->host = bstrdup(host_in);
			ctx->host[colon - host_in] = 0; // split at the colon
			ctx->port = p;
		}
	}
	if (!ctx->host)
		ctx->host = bstrdup(host_in);
	ctx->token = bstrdup(obs_data_get_string(settings, "token"));
	ctx->user_rotation = (int)obs_data_get_int(settings, "rotation");
	pthread_mutex_unlock(&ctx->cfg_lock);

	// Unbuffered by default. OBS's buffered async path holds frames in a queue up to
	// MAX_ASYNC_FRAMES (30) and paces them against its own clock -- a second delay on
	// top of the PTS pacing this file already applies, which at 30 fps is up to a
	// second of latency for a source that is supposed to be live. Unbuffered drains to
	// the newest frame each tick, which is what a camera wants. Every capture plugin in
	// obs-studio does this, including plugins/win-dshow, which this file follows.
	obs_source_set_async_unbuffered(ctx->source, true);

	// Rename the source to the typed label. Done AFTER the lock is released because
	// obs_source_set_name emits a signal that handlers may act on. Only on a real change,
	// and never to a blank, so leaving the field empty simply keeps the current name.
	const char *label = obs_data_get_string(settings, "label");
	if (label && *label) {
		const char *cur = obs_source_get_name(ctx->source);
		if (!cur || strcmp(cur, label) != 0)
			obs_source_set_name(ctx->source, label);
	}
}

static void *spancam_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct spancam_source *ctx = bzalloc(sizeof(struct spancam_source));
	ctx->source = source;
	ctx->ctrl_fd = SPANCAM_BAD_SOCKET; // no control socket until a connection is live
	pthread_mutex_init(&ctx->cfg_lock, NULL);
	os_event_init(&ctx->stop_signal, OS_EVENT_TYPE_MANUAL);
	os_event_init(&ctx->wake, OS_EVENT_TYPE_MANUAL);
	spancam_source_update(ctx, settings);
	// Track renames so the "Name" field never holds a stale value (see spancam_sync_label).
	signal_handler_connect(obs_source_get_signal_handler(source), "rename", spancam_renamed, ctx);
	spancam_sync_label(ctx);
	ctx->thread_running = (pthread_create(&ctx->thread, NULL, spancam_receive_loop, ctx) == 0);
	return ctx;
}

static void spancam_source_destroy(void *data)
{
	struct spancam_source *ctx = data;
	signal_handler_disconnect(obs_source_get_signal_handler(ctx->source), "rename", spancam_renamed, ctx);
	if (ctx->thread_running) {
		os_event_signal(ctx->stop_signal);
		os_event_signal(ctx->wake); // break the idle wait immediately
		pthread_join(ctx->thread, NULL);
	}
	os_event_destroy(ctx->stop_signal);
	os_event_destroy(ctx->wake);
	pthread_mutex_destroy(&ctx->cfg_lock);
	bfree(ctx->host);
	bfree(ctx->token);
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
	.show = spancam_source_show,
	.hide = spancam_source_hide,
	.activate = spancam_source_show,
	.deactivate = spancam_source_hide,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};
