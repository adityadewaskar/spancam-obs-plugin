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
 * data/images/no-phone.bin — run-length-encoded BGRA, written by tools/make-placeholder.py:
 *
 *   "SPCP" | version u32 | width u32 | height u32 | runs u32 | runs x { count u16, bgra u32 }
 *
 * RLE rather than PNG so the plugin does not have to vendor a decoder (~250 KB of
 * third-party source) to read one file it produces itself. Costs about 166 KB on disk
 * against 39 KB for the PNG, and the expander below is the entire price in code.
 *
 * Every field is read byte-by-byte little-endian instead of casting the mapping to a
 * struct: a struct cast would be wrong on a big-endian host and is undefined on any host
 * where the buffer is not suitably aligned.
 */

#define SPCP_HEADER_BYTES 20
#define SPCP_RUN_BYTES 6
#define SPCP_VERSION 1u
/* Bigger than anything this project would ever bake. The asset is our own build output,
 * so this is not a security boundary — it is a guard against a truncated or half-written
 * file turning into a multi-gigabyte allocation. */
#define SPCP_MAX_DIM 4096u

static uint8_t *s_bgra = NULL;
static uint32_t s_width = 0;
static uint32_t s_height = 0;
static bool s_tried = false;

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

bool spancam_placeholder_init(void)
{
	if (s_bgra)
		return true;
	/* One attempt per process. Without this a missing asset would re-read the disk and
	 * re-log on every failed dial, which is once a second in the worst case. */
	if (s_tried)
		return false;
	s_tried = true;

	char *path = obs_module_file("images/no-phone.bin");
	if (!path) {
		obs_log(LOG_WARNING, "Spancam: images/no-phone.bin not found in the plugin data "
				     "directory — the source will show no picture when idle");
		return false;
	}

	size_t len = 0;
	uint8_t *blob = spcp_read_file(path, &len);
	bfree(path);
	if (!blob) {
		obs_log(LOG_WARNING, "Spancam: could not read images/no-phone.bin");
		return false;
	}

	if (len < SPCP_HEADER_BYTES || memcmp(blob, "SPCP", 4) != 0) {
		obs_log(LOG_WARNING, "Spancam: images/no-phone.bin is not a placeholder blob");
		bfree(blob);
		return false;
	}

	uint32_t version = spcp_rd32(blob + 4);
	uint32_t width = spcp_rd32(blob + 8);
	uint32_t height = spcp_rd32(blob + 12);
	uint32_t runs = spcp_rd32(blob + 16);

	if (version != SPCP_VERSION || width == 0 || height == 0 || width > SPCP_MAX_DIM ||
	    height > SPCP_MAX_DIM) {
		obs_log(LOG_WARNING, "Spancam: placeholder blob rejected (version %u, %ux%u)", version,
			width, height);
		bfree(blob);
		return false;
	}
	/* The run table must be exactly the size the header claims — a truncated file would
	 * otherwise be read past its end while expanding. */
	if (len != (size_t)SPCP_HEADER_BYTES + (size_t)runs * SPCP_RUN_BYTES) {
		obs_log(LOG_WARNING, "Spancam: placeholder blob is %zu bytes, expected %zu for %u runs",
			len, (size_t)SPCP_HEADER_BYTES + (size_t)runs * SPCP_RUN_BYTES, runs);
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
			obs_log(LOG_WARNING, "Spancam: placeholder blob run %u overruns %ux%u", i, width,
				height);
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
		obs_log(LOG_WARNING, "Spancam: placeholder blob covers %zu of %zu pixels", at, pixels);
		bfree(out);
		return false;
	}

	s_bgra = out;
	s_width = width;
	s_height = height;
	obs_log(LOG_INFO, "Spancam: idle card loaded (%ux%u)", width, height);
	return true;
}

void spancam_placeholder_free(void)
{
	bfree(s_bgra);
	s_bgra = NULL;
	s_width = s_height = 0;
	s_tried = false;
}

void spancam_placeholder_output(obs_source_t *source)
{
	if (!s_bgra && !spancam_placeholder_init())
		return;

	struct obs_source_frame frame = {0};
	frame.format = VIDEO_FORMAT_BGRA;
	frame.width = s_width;
	frame.height = s_height;
	frame.data[0] = s_bgra;
	frame.linesize[0] = s_width * 4;
	frame.timestamp = os_gettime_ns();
	/* Packed RGB carries no colour matrix, but OBS still reads full_range for it, and
	 * the artwork is authored in full-range sRGB. */
	frame.full_range = true;
	obs_source_output_video(source, &frame);
}
