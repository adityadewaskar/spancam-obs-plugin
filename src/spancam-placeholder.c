/*
Spancam for OBS
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "spancam-placeholder.h"
#include "plugin-support.h"

#include <util/bmem.h>
#include <util/platform.h>

#include <stdio.h>
#include <string.h>

/*
 * data/images/no-phone-{landscape,portrait}.bin — run-length-encoded BGRA, written by
 * build-aux/placeholder/make-placeholder.py:
 *
 *   "SPCP" | version u32 | width u32 | height u32 | runs u32 | background u32
 *          | runs x { count u16, bgra u32 }
 *
 * TWO layouts, not one. A landscape card centred in a 1080x1920 vertical canvas leaves
 * most of the width empty and shrinks the text to nothing, so the portrait layout stacks
 * the code above the steps instead of beside them. The plugin picks by canvas aspect.
 *
 * The background colour travels in the header so this file never holds a second copy of it
 * that can drift from the generator.
 *
 * RLE rather than PNG so the plugin does not have to vendor a decoder (~250 KB of
 * third-party source) to read a file it produces itself. Costs about 175 KB on disk per
 * layout against 40 KB for a PNG, and the expander below is the entire price in code.
 *
 * Every field is read byte-by-byte little-endian rather than by casting the buffer to a
 * struct: a struct cast would be wrong on a big-endian host and is undefined wherever the
 * buffer is not suitably aligned.
 */

#define SPCP_HEADER_BYTES 24
#define SPCP_RUN_BYTES 6
#define SPCP_VERSION 2u
/* Bigger than anything this project would ever bake. The asset is our own build output, so
 * this is not a security boundary — it guards against a truncated or half-written file
 * turning into a multi-gigabyte allocation. */
#define SPCP_MAX_DIM 4096u

struct spcp_card {
	uint8_t *px; /* BGRA, w*h*4 */
	uint32_t w;
	uint32_t h;
	uint32_t bg;
};

enum { SPCP_LANDSCAPE = 0, SPCP_PORTRAIT = 1, SPCP_LAYOUTS = 2 };

static struct spcp_card s_cards[SPCP_LAYOUTS];
static bool s_tried = false;

/* The chosen card composed onto a frame matching the canvas aspect, plus what it was
 * composed for. Rebuilt only when the canvas changes, which is almost never. */
static uint8_t *s_frame = NULL;
static uint32_t s_frame_w = 0;
static uint32_t s_frame_h = 0;
static uint32_t s_for_canvas_w = 0;
static uint32_t s_for_canvas_h = 0;
static const struct spcp_card *s_for_card = NULL;

static inline uint16_t spcp_rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t spcp_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *spcp_read_file(const char *path, size_t *out_len)
{
	FILE *f = os_fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long end = ftell(f);
	if (end <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	size_t len = (size_t)end;
	uint8_t *buf = bmalloc(len);
	size_t got = fread(buf, 1, len, f);
	fclose(f);
	if (got != len) {
		bfree(buf);
		return NULL;
	}
	*out_len = len;
	return buf;
}

static bool spcp_load(const char *name, struct spcp_card *card)
{
	char *path = obs_module_file(name);
	if (!path) {
		obs_log(LOG_WARNING, "Spancam: %s not found in the plugin data directory", name);
		return false;
	}

	size_t len = 0;
	uint8_t *blob = spcp_read_file(path, &len);
	bfree(path);
	if (!blob) {
		obs_log(LOG_WARNING, "Spancam: could not read %s", name);
		return false;
	}

	if (len < SPCP_HEADER_BYTES || memcmp(blob, "SPCP", 4) != 0) {
		obs_log(LOG_WARNING, "Spancam: %s is not a placeholder blob", name);
		bfree(blob);
		return false;
	}

	uint32_t version = spcp_rd32(blob + 4);
	uint32_t width = spcp_rd32(blob + 8);
	uint32_t height = spcp_rd32(blob + 12);
	uint32_t runs = spcp_rd32(blob + 16);
	uint32_t background = spcp_rd32(blob + 20);

	if (version != SPCP_VERSION || width == 0 || height == 0 || width > SPCP_MAX_DIM || height > SPCP_MAX_DIM) {
		obs_log(LOG_WARNING, "Spancam: %s rejected (version %u, %ux%u)", name, version, width, height);
		bfree(blob);
		return false;
	}
	/* The run table must be exactly the size the header claims — a truncated file would
	 * otherwise be read past its end while expanding. */
	if (len != (size_t)SPCP_HEADER_BYTES + (size_t)runs * SPCP_RUN_BYTES) {
		obs_log(LOG_WARNING, "Spancam: %s is %zu bytes, expected %zu for %u runs", name, len,
			(size_t)SPCP_HEADER_BYTES + (size_t)runs * SPCP_RUN_BYTES, runs);
		bfree(blob);
		return false;
	}

	const size_t pixels = (size_t)width * (size_t)height;
	uint8_t *out = bmalloc(pixels * 4);
	size_t at = 0;
	const uint8_t *p = blob + SPCP_HEADER_BYTES;
	for (uint32_t i = 0; i < runs; i++, p += SPCP_RUN_BYTES) {
		uint32_t count = spcp_rd16(p);
		uint32_t bgra = spcp_rd32(p + 2);
		/* A run that would overflow the image means the blob and the header disagree.
		 * Stop rather than clamp: a partially expanded card is a corrupt card. */
		if (count == 0 || at + count > pixels) {
			obs_log(LOG_WARNING, "Spancam: %s run %u overruns %ux%u", name, i, width, height);
			bfree(out);
			bfree(blob);
			return false;
		}
		uint8_t *dst = out + at * 4;
		for (uint32_t n = 0; n < count; n++, dst += 4) {
			dst[0] = (uint8_t)(bgra & 0xFF);
			dst[1] = (uint8_t)((bgra >> 8) & 0xFF);
			dst[2] = (uint8_t)((bgra >> 16) & 0xFF);
			dst[3] = (uint8_t)((bgra >> 24) & 0xFF);
		}
		at += count;
	}
	bfree(blob);

	if (at != pixels) {
		obs_log(LOG_WARNING, "Spancam: %s covers %zu of %zu pixels", name, at, pixels);
		bfree(out);
		return false;
	}

	card->px = out;
	card->w = width;
	card->h = height;
	card->bg = background;
	return true;
}

bool spancam_placeholder_init(void)
{
	if (s_cards[SPCP_LANDSCAPE].px || s_cards[SPCP_PORTRAIT].px)
		return true;
	/* One attempt per process. Without this a missing asset would re-read the disk and
	 * re-log on every failed dial, which is once a second in the worst case. */
	if (s_tried)
		return false;
	s_tried = true;

	bool land = spcp_load("images/no-phone-landscape.bin", &s_cards[SPCP_LANDSCAPE]);
	bool port = spcp_load("images/no-phone-portrait.bin", &s_cards[SPCP_PORTRAIT]);
	if (!land && !port) {
		obs_log(LOG_WARNING, "Spancam: no idle card available — the source will show no "
				     "picture when there is no phone");
		return false;
	}
	obs_log(LOG_INFO, "Spancam: idle card loaded (landscape %s, portrait %s)", land ? "yes" : "no",
		port ? "yes" : "no");
	return true;
}

void spancam_placeholder_free(void)
{
	for (int i = 0; i < SPCP_LAYOUTS; i++) {
		bfree(s_cards[i].px);
		memset(&s_cards[i], 0, sizeof(s_cards[i]));
	}
	bfree(s_frame);
	s_frame = NULL;
	s_frame_w = s_frame_h = 0;
	s_for_canvas_w = s_for_canvas_h = 0;
	s_for_card = NULL;
	s_tried = false;
}

/*
 * Compose the card onto a frame with the CANVAS's aspect ratio.
 *
 * Emitting the card by itself makes the source as wide as the card, and OBS then fits that
 * to the canvas however the user's transform happens to be set — which on a mismatched
 * aspect means letterboxed small or cropped. Matching the canvas aspect means "fit to
 * screen" lands the card at a predictable, legible size whichever way round the canvas is.
 *
 * The frame is only as large as it needs to be — the card at native pixels plus padding —
 * rather than canvas-sized, so a 4K canvas does not cost a 33 MB allocation. The card is
 * never rescaled, so its text and QR modules stay exactly as authored.
 */
static bool spancam_compose(const struct spcp_card *card, uint32_t canvas_w, uint32_t canvas_h)
{
	if (s_frame && card == s_for_card && canvas_w == s_for_canvas_w && canvas_h == s_for_canvas_h)
		return true;
	if (!card->px || canvas_w == 0 || canvas_h == 0)
		return false;

	/* Smallest frame that contains the padded card AND matches the canvas aspect. */
	const double pad = 1.12;
	const double aspect = (double)canvas_w / (double)canvas_h;
	double fw = (double)card->w * pad;
	const double need = (double)card->h * pad * aspect;
	if (need > fw)
		fw = need;
	double fh = fw / aspect;

	uint32_t w = (uint32_t)(fw + 0.5);
	uint32_t h = (uint32_t)(fh + 0.5);
	if (w < card->w)
		w = card->w;
	if (h < card->h)
		h = card->h;
	if (w > SPCP_MAX_DIM || h > SPCP_MAX_DIM) {
		/* An absurd canvas aspect. Show the card alone rather than nothing at all. */
		w = card->w;
		h = card->h;
	}
	w &= ~1u; /* keep both even; some scalers dislike odd dimensions */
	h &= ~1u;
	if (w < card->w || h < card->h) {
		w = card->w;
		h = card->h;
	}

	uint8_t *frame = bmalloc((size_t)w * h * 4);
	const uint8_t b = (uint8_t)(card->bg & 0xFF), g = (uint8_t)((card->bg >> 8) & 0xFF),
		      r = (uint8_t)((card->bg >> 16) & 0xFF), a = (uint8_t)((card->bg >> 24) & 0xFF);
	for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
		frame[i * 4 + 0] = b;
		frame[i * 4 + 1] = g;
		frame[i * 4 + 2] = r;
		frame[i * 4 + 3] = a;
	}

	const uint32_t ox = (w - card->w) / 2;
	const uint32_t oy = (h - card->h) / 2;
	for (uint32_t y = 0; y < card->h; y++)
		memcpy(frame + (((size_t)(oy + y) * w) + ox) * 4, card->px + (size_t)y * card->w * 4,
		       (size_t)card->w * 4);

	bfree(s_frame);
	s_frame = frame;
	s_frame_w = w;
	s_frame_h = h;
	s_for_canvas_w = canvas_w;
	s_for_canvas_h = canvas_h;
	s_for_card = card;
	obs_log(LOG_INFO, "Spancam: idle card composed %ux%u (%s) for a %ux%u canvas", w, h,
		card == &s_cards[SPCP_PORTRAIT] ? "portrait" : "landscape", canvas_w, canvas_h);
	return true;
}

bool spancam_placeholder_stale(void)
{
	/* Nothing composed yet is not stale — the ordinary path will compose it. */
	if (!s_frame)
		return false;
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	return ovi.base_width != s_for_canvas_w || ovi.base_height != s_for_canvas_h;
}

void spancam_placeholder_output(obs_source_t *source)
{
	if (!s_cards[SPCP_LANDSCAPE].px && !s_cards[SPCP_PORTRAIT].px && !spancam_placeholder_init())
		return;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	/* Taller than wide gets the stacked layout. Fall back to whichever card did load, so
	 * one missing asset degrades to a wrong-shaped card rather than to no card. */
	int want = (ovi.base_height > ovi.base_width) ? SPCP_PORTRAIT : SPCP_LANDSCAPE;
	const struct spcp_card *card = s_cards[want].px ? &s_cards[want] : &s_cards[want ^ 1];
	if (!spancam_compose(card, ovi.base_width, ovi.base_height))
		return;

	struct obs_source_frame frame = {0};
	frame.format = VIDEO_FORMAT_BGRA;
	frame.width = s_frame_w;
	frame.height = s_frame_h;
	frame.data[0] = s_frame;
	frame.linesize[0] = s_frame_w * 4;
	frame.timestamp = os_gettime_ns();
	/* Packed RGB carries no colour matrix, but OBS still reads full_range for it, and the
	 * artwork is authored in full-range sRGB. */
	frame.full_range = true;
	obs_source_output_video(source, &frame);
}
