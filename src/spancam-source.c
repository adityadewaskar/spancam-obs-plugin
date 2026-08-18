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

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h> // VideoToolbox decode, via the generic hwaccel API

#if defined(__APPLE__)
#include <dns_sd.h>     // Bonjour browse/resolve — finds the iOS sender (macOS system lib)
#include <sys/select.h> // select() to pump the DNSServiceRef synchronously
#endif

#define SPANCAM_DEFAULT_PORT 8892
#define SPANCAM_DISCOVERY_PORT 8891
#define SPANCAM_PROBE "SPANCAM-DISCOVER"
#define SPANCAM_MAGIC 0x53504331u // 'SPC1'
#define SPANCAM_RECV_TIMEOUT_MS 500

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
	spancam_socket_t ctrl_fd; // socket to send control frames on
	int abr_target;           // bitrate the phone is currently being asked for
	int abr_ceiling;          // StreamHeader bitrate — never ask for more than this
	bool abr_base_set;
	int64_t abr_base_arrival_ns; // anchor for the one-way delay measurement
	int64_t abr_base_pts_us;
	int64_t abr_delay_ewma_ns;  // smoothed queuing delay since the anchor
	int64_t abr_last_send_ns;   // rate-limit control frames to ~1 Hz
	int64_t last_kf_request_ns; // debounce keyframe requests — no IDR storms

	// Reconnect + liveness, mirroring the Mac receiver (MacEffectRunner's
	// BLACK-STREAM WATCHDOG and ConnectionCore's paced redial ladder).
	int redials;              // consecutive failed/short connections
	uint64_t wire_packets;    // SDSP packets read off the socket this connection
	uint64_t decoded_frames;  // frames actually emitted to OBS this connection
	uint64_t wd_last_wire, wd_last_decoded;
	int wd_dead_secs;         // consecutive seconds of wire-but-no-decode
	int64_t wd_next_tick_ns;

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

	// Hardware decode. hw_decode = false means the pure software path, either
	// because nothing suitable was found or because a step failed and we fell
	// back. sw_frame is the reusable download target for the GPU surface.
	AVBufferRef *hw_device_ctx;
	AVFrame *sw_frame;
	bool hw_decode;
	enum AVPixelFormat hw_pix_fmt; // the format get_format must hold out for
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
static bool spancam_read_full(struct spancam_source *ctx, spancam_socket_t fd, uint8_t *buf, size_t len,
			      int timeout_ms)
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
	if (ctx->ctrl_fd == SPANCAM_BAD_SOCKET || ctx->abr_ceiling <= 0)
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
		snprintf(probe, sizeof(probe), "\"%s\" version" SPANCAM_DEVNULL, cands[i]);
		FILE *f = spancam_popen(probe, "r");
		if (!f)
			continue;
		char line[256];
		bool got = fgets(line, sizeof(line), f) && strstr(line, "Android Debug Bridge");
		while (fgets(line, sizeof(line), f)) {
		}
		spancam_pclose(f);
		if (got) {
			snprintf(cached, sizeof(cached), "%s", cands[i]);
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
static bool spancam_adb_usb_device(char *serial_out, size_t serialsz)
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
	while (fgets(line, sizeof(line), f)) {
		char *tab = strstr(line, "\tdevice"); // "<serial>\tdevice"; skips the header
		if (!tab)
			continue;
		*tab = 0;
		if (strstr(line, "._tcp") || strchr(line, ':')) {
			saw_wireless = true; // adb over Wi-Fi — not a cable
			continue;
		}
		if (!found) {
			snprintf(serial_out, serialsz, "%s", line);
			found = true;
		}
	}
	spancam_pclose(f);
	if (!found && saw_wireless)
		obs_log(LOG_INFO, "Spancam: adb sees only a WIRELESS device — that is not a cable, "
				  "so USB is unavailable and Wi-Fi is the honest path");
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
#endif // __APPLE__

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

// Latency settings, applied before avcodec_open2 on both the HW and SW paths.
// LOW_DELAY tells the decoder not to hold frames back for reordering, which is
// correct here because the phone encodes without B-frames — DTS equals PTS, so
// there is nothing to reorder and any buffering is pure added latency.
// FF_THREAD_SLICE rather than FF_THREAD_FRAME for the same reason: frame
// threading wins throughput by working on several frames at once, and paying a
// frame or more of latency for throughput is the wrong trade for a live camera.
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
				spancam_xform_plane(f->data[1], w / 2, h / 2, f->linesize[1] / 2, duv, ow / 2, rot, mir,
						    2);
				frame.width = (uint32_t)ow;
				frame.height = (uint32_t)oh;
				frame.data[0] = dy;
				frame.linesize[0] = (uint32_t)ow;
				frame.data[1] = duv;
				frame.linesize[1] = (uint32_t)ow; // (ow/2 pairs) * 2 bytes per row
			} else {                                  // I420, the software-decode path
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

		enum video_range_type range = (f->color_range == AVCOL_RANGE_JPEG) ? VIDEO_RANGE_FULL
										   : VIDEO_RANGE_PARTIAL;
		frame.full_range = (range == VIDEO_RANGE_FULL);
		video_format_get_parameters_for_format(VIDEO_CS_709, range, fmt, frame.color_matrix,
						       frame.color_range_min, frame.color_range_max);

		obs_source_output_video(ctx->source, &frame);
		ctx->decoded_frames++;
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
static spancam_socket_t spancam_dial(struct spancam_source *ctx, char *token_out, size_t toksz, char *label_out,
				     size_t lblsz, bool *used_usb)
{
	pthread_mutex_lock(&ctx->cfg_lock);
	int mode = ctx->connection;
	char *mhost = bstrdup(ctx->host ? ctx->host : "");
	int mport = ctx->port > 0 ? ctx->port : SPANCAM_DEFAULT_PORT;
	char *mtoken = bstrdup(ctx->token ? ctx->token : "");
	pthread_mutex_unlock(&ctx->cfg_lock);

	spancam_socket_t fd = SPANCAM_BAD_SOCKET;
	token_out[0] = 0;
	*used_usb = false;

	// In Auto, skip USB while it's benched (see usb_cooldown_until_ns).
	bool usb_cooling = (mode == SPANCAM_CONN_AUTO) && (ctx->usb_cooldown_until_ns > (int64_t)os_gettime_ns());

	// ---- USB, if a phone is on the end of a CABLE ----
	char adb_serial[128] = {0};
	if ((mode == SPANCAM_CONN_USB || (mode == SPANCAM_CONN_AUTO && !usb_cooling)) &&
	    spancam_adb_usb_device(adb_serial, sizeof(adb_serial))) {
		char cmd[1160];
		// -s scopes the forward: with two devices attached an unscoped `adb forward`
		// errors out or silently tunnels to the wrong phone.
		snprintf(cmd, sizeof(cmd), "\"%s\" -s \"%s\" forward tcp:%d tcp:%d" SPANCAM_DEVNULL,
			 spancam_adb_path(), adb_serial, SPANCAM_DEFAULT_PORT, SPANCAM_DEFAULT_PORT);
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
		obs_log(LOG_WARNING, "Spancam: USB selected but %s — set Connection to Auto or Wi-Fi, "
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
	return fd;
}

static void spancam_stream_once(struct spancam_source *ctx)
{
	char token[128] = {0};
	char label[160] = {0};
	bool used_usb = false;
	bool got_stream = false; // a valid StreamHeader arrived => this path actually works
	spancam_socket_t fd = spancam_dial(ctx, token, sizeof(token), label, sizeof(label), &used_usb);
	if (fd == SPANCAM_BAD_SOCKET)
		return;
	obs_log(LOG_INFO, "Spancam: connected (%s)", label);

	// Handshake: "SPANCAM/1 k=<token> app=OBS os=<os>\n". The token gates Wi-Fi
	// access; app/os are optional and only let the phone show who connected.
	struct dstr hello;
	dstr_init(&hello);
	dstr_printf(&hello, "SPANCAM/1 k=%s app=OBS os=%s\n", token, SPANCAM_OS);
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
			spancam_decode(ctx, payload, (int)size, pts_us);
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

static void *spancam_receive_loop(void *data)
{
	struct spancam_source *ctx = data;
	while (os_event_try(ctx->stop_signal) == EAGAIN) {
		spancam_stream_once(ctx);
		int wait = spancam_redial_ms(ctx->redials);
		if (ctx->redials == SPANCAM_MAX_REDIALS)
			obs_log(LOG_INFO,
				"Spancam: %d redials with no video — backing off to %d s; "
				"the phone is not reachable, check it is on this network and Discoverable",
				ctx->redials, wait / 1000);
		os_event_timedwait(ctx->stop_signal, wait);
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
	obs_data_set_default_bool(settings, "buffering", false);
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
	obs_property_list_add_int(rot, "0° (follow phone)", 0);
	obs_property_list_add_int(rot, "90°", 90);
	obs_property_list_add_int(rot, "180°", 180);
	obs_property_list_add_int(rot, "270°", 270);
	obs_properties_add_text(props, "host", obs_module_text("Spancam.Prop.Host"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", obs_module_text("Spancam.Prop.Port"), 1, 65535, 1);
	obs_properties_add_text(props, "token", obs_module_text("Spancam.Prop.Token"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "buffering", obs_module_text("Spancam.Prop.Buffering"));
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
	ctx->user_rotation = (int)obs_data_get_int(settings, "rotation");
	pthread_mutex_unlock(&ctx->cfg_lock);

	// Unbuffered by default. OBS's buffered async path holds frames in a queue up to
	// MAX_ASYNC_FRAMES (30) and paces them against its own clock -- a second delay on
	// top of the PTS pacing this file already applies, which at 30 fps is up to a
	// second of latency for a source that is supposed to be live. Unbuffered drains to
	// the newest frame each tick, which is what a camera wants. Every capture plugin in
	// obs-studio does this, including plugins/win-dshow, which this file follows.
	obs_source_set_async_unbuffered(ctx->source, !obs_data_get_bool(settings, "buffering"));
}

static void *spancam_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct spancam_source *ctx = bzalloc(sizeof(struct spancam_source));
	ctx->source = source;
	ctx->ctrl_fd = SPANCAM_BAD_SOCKET; // no control socket until a connection is live
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
