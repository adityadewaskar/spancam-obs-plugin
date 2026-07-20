/*
Spancam OBS Studio plugin — "Spancam Camera" video source
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

An OBS async video source fed by a Spancam phone over SDSP (docs/PROTOCOL.md).
A worker thread owns the socket: connect, handshake, read the StreamHeader, then
pump packets until the far end goes away, then reconnect.

No decoder yet — packets are read and dropped. This exists to prove the framing
is right before libavcodec gets involved.
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

#define SPANCAM_DEFAULT_PORT 8892
#define SPANCAM_MAGIC 0x53504331u // 'SPC1'
#define SPANCAM_RECONNECT_MS 1500
#define SPANCAM_RECV_TIMEOUT_MS 500

struct spancam_source {
	obs_source_t *source;

	pthread_t thread;
	os_event_t *stop_signal;
	bool thread_running;

	pthread_mutex_t cfg_lock;
	char *host;
	int port;
	char *token;
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
// One connection: handshake, StreamHeader, packet pump.
// --------------------------------------------------------------------------

static void spancam_stream_once(struct spancam_source *ctx)
{
	pthread_mutex_lock(&ctx->cfg_lock);
	char *host = bstrdup(ctx->host ? ctx->host : "");
	char *token = bstrdup(ctx->token ? ctx->token : "");
	int port = ctx->port > 0 ? ctx->port : SPANCAM_DEFAULT_PORT;
	pthread_mutex_unlock(&ctx->cfg_lock);

	int fd = -1;
	if (*host)
		fd = spancam_connect(host, port);
	if (fd < 0) {
		bfree(host);
		bfree(token);
		return;
	}
	obs_log(LOG_INFO, "Spancam: connected to %s:%d", host, port);

	// Handshake: "SPANCAM/1 k=<token>\n"
	struct dstr hello;
	dstr_init(&hello);
	dstr_printf(&hello, "SPANCAM/1 k=%s\n", token);
	send(fd, hello.array, hello.len, 0);
	dstr_free(&hello);

	uint8_t hdr[24];
	if (!spancam_read_full(ctx, fd, hdr, sizeof(hdr)) || be32(hdr) != SPANCAM_MAGIC) {
		obs_log(LOG_WARNING, "Spancam: bad or missing stream header");
		goto done;
	}
	obs_log(LOG_INFO, "Spancam: stream %ux%u %s @ %u fps", be32(hdr + 8), be32(hdr + 12),
		hdr[4] == 1 ? "HEVC" : "H.264", be32(hdr + 16));

	// Packet pump. Payloads are read and dropped until there is a decoder.
	size_t cap = 1 << 20;
	uint8_t *payload = bmalloc(cap);
	for (;;) {
		uint8_t ph[16];
		if (!spancam_read_full(ctx, fd, ph, sizeof(ph)))
			break;
		uint32_t size = be32(ph + 12);
		if (size == 0 || size > (64u << 20)) // sanity cap 64 MiB
			break;
		if (size > cap) {
			cap = size;
			payload = brealloc(payload, cap);
		}
		if (!spancam_read_full(ctx, fd, payload, size))
			break;
	}
	bfree(payload);

done:
	close(fd);
	bfree(host);
	bfree(token);
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
	obs_data_set_default_int(settings, "port", SPANCAM_DEFAULT_PORT);
}

static obs_properties_t *spancam_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
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
	ctx->host = bstrdup(obs_data_get_string(settings, "host"));
	ctx->token = bstrdup(obs_data_get_string(settings, "token"));
	ctx->port = (int)obs_data_get_int(settings, "port");
	pthread_mutex_unlock(&ctx->cfg_lock);
}

static void *spancam_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct spancam_source *ctx = bzalloc(sizeof(struct spancam_source));
	ctx->source = source;
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
