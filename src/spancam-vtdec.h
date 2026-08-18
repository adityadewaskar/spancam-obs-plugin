/*
Spancam OBS Studio plugin — VideoToolbox decoder (macOS)
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

Why this exists: linking libavcodec couples the plugin to the exact FFmpeg major
that a given OBS build ships. OBS bundles its own FFmpeg, so a plugin built
against 61 loaded into an OBS carrying 62 is reading struct fields whose offsets
FFmpeg only guarantees WITHIN a major. It happens to work today — verified across
61 and 62 — but "happens to work" is not something to ship to every OBS user on
every OBS version, and pinning cannot fix it either: pin 32.x and everyone on 31.x
breaks, pin 31.x and the reverse.

VideoToolbox has no such problem. It is a stable macOS system framework, the same
one the Spancam Mac app decodes with (mac/Spancam/Core/H264Decoder.swift), and it
is hardware-accelerated by default. Using it here means the macOS build carries no
FFmpeg dependency at all, so it loads correctly into any OBS version.

Windows and Linux keep the libavcodec path for now, behind SPANCAM_USE_VTDEC.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct spancam_vtdec spancam_vtdec_t;

/// One decoded frame, planes borrowed for the duration of the callback only.
struct spancam_vtdec_frame {
	int width, height;
	int plane_count;
	const uint8_t *data[3];
	int linesize[3];
	bool full_range;
	int64_t pts_us;
};

typedef void (*spancam_vtdec_cb)(void *opaque, const struct spancam_vtdec_frame *f);

/// codec: 0 = H.264, 1 = HEVC. The session is (re)built when parameter sets arrive.
spancam_vtdec_t *spancam_vtdec_create(uint8_t codec, spancam_vtdec_cb cb, void *opaque);

/// Feed a type-1 codecConfig payload (Annex-B parameter sets). Rebuilds the session
/// when the sets actually change, so a repeated identical config is free.
bool spancam_vtdec_set_parameter_sets(spancam_vtdec_t *d, const uint8_t *annexb, size_t len);

/// Feed one Annex-B access unit. Returns false if the frame could not be submitted
/// (no session yet, or a decode error the caller should answer with a keyframe request).
bool spancam_vtdec_decode(spancam_vtdec_t *d, const uint8_t *annexb, size_t len, int64_t pts_us);

void spancam_vtdec_destroy(spancam_vtdec_t *d);
