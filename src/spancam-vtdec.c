/*
Spancam OBS Studio plugin — VideoToolbox decoder (macOS)
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

Ported from the Spancam Mac app's H264Decoder.swift so both ends of the product
decode the same way. VideoToolbox is a C framework, so this is a direct port.
*/

#include "spancam-vtdec.h"
#include <plugin-support.h>
#include <obs-module.h>

#include <string.h>
#include <stdlib.h>

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#define MAX_PARAM_SETS 4

struct spancam_vtdec {
	uint8_t codec; // 0 = H.264, 1 = HEVC
	VTDecompressionSessionRef session;
	CMVideoFormatDescriptionRef fmt;
	spancam_vtdec_cb cb;
	void *opaque;

	// The parameter sets the current session was built from, kept so an identical
	// repeat is a no-op rather than a session teardown mid-stream.
	uint8_t *sets[MAX_PARAM_SETS];
	size_t set_sizes[MAX_PARAM_SETS];
	int set_count;

	// Annex-B -> AVCC scratch, grown as needed.
	uint8_t *avcc;
	size_t avcc_cap;
};

// ---------------------------------------------------------------- Annex-B

// Walk Annex-B NALs. Returns the payload (start code stripped) or NULL at the end.
static const uint8_t *next_nal(const uint8_t *p, const uint8_t *end, size_t *out_len)
{
	// find the next start code
	while (p + 3 <= end) {
		if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p + 4 <= end && p[2] == 0 && p[3] == 1)))
			break;
		p++;
	}
	if (p + 3 > end)
		return NULL;
	const uint8_t *nal = (p[2] == 1) ? p + 3 : p + 4;
	// find the following start code, which bounds this NAL
	const uint8_t *q = nal;
	while (q + 3 <= end) {
		if (q[0] == 0 && q[1] == 0 && (q[2] == 1 || (q + 4 <= end && q[2] == 0 && q[3] == 1)))
			break;
		q++;
	}
	const uint8_t *nal_end = (q + 3 <= end) ? q : end;
	if (nal_end <= nal)
		return NULL;
	*out_len = (size_t)(nal_end - nal);
	return nal;
}

static int nal_type(const struct spancam_vtdec *d, uint8_t first)
{
	return d->codec == 1 ? ((first >> 1) & 0x3F) : (first & 0x1F);
}

static bool is_parameter_set(const struct spancam_vtdec *d, int t)
{
	return d->codec == 1 ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
}

// ---------------------------------------------------------------- session

static void free_sets(struct spancam_vtdec *d)
{
	for (int i = 0; i < d->set_count; i++)
		bfree(d->sets[i]);
	d->set_count = 0;
}

static void teardown_session(struct spancam_vtdec *d)
{
	if (d->session) {
		VTDecompressionSessionWaitForAsynchronousFrames(d->session);
		VTDecompressionSessionInvalidate(d->session);
		CFRelease(d->session);
		d->session = NULL;
	}
	if (d->fmt) {
		CFRelease(d->fmt);
		d->fmt = NULL;
	}
}

static void decode_output(void *decompressionOutputRefCon, void *sourceFrameRefCon, OSStatus status,
			  VTDecodeInfoFlags infoFlags, CVImageBufferRef imageBuffer, CMTime presentationTimeStamp,
			  CMTime presentationDuration)
{
	UNUSED_PARAMETER(sourceFrameRefCon);
	UNUSED_PARAMETER(infoFlags);
	UNUSED_PARAMETER(presentationDuration);
	struct spancam_vtdec *d = decompressionOutputRefCon;
	if (status != noErr || !imageBuffer || !d->cb)
		return;

	if (CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return;

	struct spancam_vtdec_frame f = {0};
	f.width = (int)CVPixelBufferGetWidth(imageBuffer);
	f.height = (int)CVPixelBufferGetHeight(imageBuffer);
	f.pts_us = CMTIME_IS_VALID(presentationTimeStamp)
			   ? (int64_t)(CMTimeGetSeconds(presentationTimeStamp) * 1000000.0)
			   : 0;
	OSType pf = CVPixelBufferGetPixelFormatType(imageBuffer);
	f.full_range = (pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);

	if (CVPixelBufferIsPlanar(imageBuffer)) {
		size_t n = CVPixelBufferGetPlaneCount(imageBuffer);
		if (n > 3)
			n = 3;
		f.plane_count = (int)n;
		for (size_t i = 0; i < n; i++) {
			f.data[i] = CVPixelBufferGetBaseAddressOfPlane(imageBuffer, i);
			f.linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, i);
		}
	} else {
		f.plane_count = 1;
		f.data[0] = CVPixelBufferGetBaseAddress(imageBuffer);
		f.linesize[0] = (int)CVPixelBufferGetBytesPerRow(imageBuffer);
	}
	d->cb(d->opaque, &f);
	CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

static bool create_session(struct spancam_vtdec *d)
{
	teardown_session(d);
	if (d->set_count < 2)
		return false;

	const uint8_t *ptrs[MAX_PARAM_SETS];
	size_t sizes[MAX_PARAM_SETS];
	for (int i = 0; i < d->set_count; i++) {
		ptrs[i] = d->sets[i];
		sizes[i] = d->set_sizes[i];
	}

	OSStatus st;
	if (d->codec == 1)
		st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, (size_t)d->set_count,
									 ptrs, sizes, 4, NULL, &d->fmt);
	else
		st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, (size_t)d->set_count,
									 ptrs, sizes, 4, &d->fmt);
	if (st != noErr || !d->fmt) {
		obs_log(LOG_ERROR, "Spancam: could not build a format description from the parameter sets (%d)",
			(int)st);
		return false;
	}

	// NV12 out: it is what the hardware decoder produces natively and what OBS
	// takes as VIDEO_FORMAT_NV12, so nothing converts anywhere.
	const void *pf_keys[] = {kCVPixelBufferPixelFormatTypeKey};
	SInt32 pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
	const void *pf_vals[] = {pf_num};
	CFDictionaryRef dest = CFDictionaryCreate(kCFAllocatorDefault, pf_keys, pf_vals, 1,
						  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	const void *spec_keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
	const void *spec_vals[] = {kCFBooleanTrue};
	CFDictionaryRef spec = CFDictionaryCreate(kCFAllocatorDefault, spec_keys, spec_vals, 1,
						  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	VTDecompressionOutputCallbackRecord cbrec = {.decompressionOutputCallback = decode_output,
						     .decompressionOutputRefCon = d};
	st = VTDecompressionSessionCreate(kCFAllocatorDefault, d->fmt, spec, dest, &cbrec, &d->session);
	CFRelease(pf_num);
	CFRelease(dest);
	CFRelease(spec);
	if (st != noErr || !d->session) {
		obs_log(LOG_ERROR, "Spancam: VTDecompressionSessionCreate failed (%d)", (int)st);
		teardown_session(d);
		return false;
	}
	VTSessionSetProperty(d->session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);

	CFBooleanRef hw = NULL;
	bool using_hw = false;
	if (VTSessionCopyProperty(d->session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
				  kCFAllocatorDefault, &hw) == noErr &&
	    hw) {
		using_hw = CFBooleanGetValue(hw);
		CFRelease(hw);
	}
	CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(d->fmt);
	obs_log(LOG_INFO, "Spancam: VideoToolbox decode %dx%d %s (%s)", dims.width, dims.height,
		d->codec == 1 ? "HEVC" : "H.264", using_hw ? "hardware" : "software");
	return true;
}

// ---------------------------------------------------------------- public

spancam_vtdec_t *spancam_vtdec_create(uint8_t codec, spancam_vtdec_cb cb, void *opaque)
{
	struct spancam_vtdec *d = bzalloc(sizeof(*d));
	d->codec = codec;
	d->cb = cb;
	d->opaque = opaque;
	return d;
}

bool spancam_vtdec_set_parameter_sets(spancam_vtdec_t *d, const uint8_t *annexb, size_t len)
{
	const uint8_t *p = annexb, *end = annexb + len;
	const uint8_t *nal;
	size_t nlen;
	uint8_t *found[MAX_PARAM_SETS] = {0};
	size_t found_sizes[MAX_PARAM_SETS] = {0};
	int n = 0;

	while (n < MAX_PARAM_SETS && (nal = next_nal(p, end, &nlen)) != NULL) {
		p = nal + nlen;
		if (nlen < 1)
			continue;
		if (!is_parameter_set(d, nal_type(d, nal[0])))
			continue;
		found[n] = bmalloc(nlen);
		memcpy(found[n], nal, nlen);
		found_sizes[n] = nlen;
		n++;
	}
	if (n < 2) {
		for (int i = 0; i < n; i++)
			bfree(found[i]);
		return false;
	}

	// Identical to what the live session was built from? Then leave it alone —
	// tearing the session down mid-stream costs a visible hitch for nothing.
	bool same = (n == d->set_count);
	for (int i = 0; same && i < n; i++)
		same = (found_sizes[i] == d->set_sizes[i]) && memcmp(found[i], d->sets[i], n ? found_sizes[i] : 0) == 0;
	if (same && d->session) {
		for (int i = 0; i < n; i++)
			bfree(found[i]);
		return true;
	}

	free_sets(d);
	for (int i = 0; i < n; i++) {
		d->sets[i] = found[i];
		d->set_sizes[i] = found_sizes[i];
	}
	d->set_count = n;
	return create_session(d);
}

bool spancam_vtdec_decode(spancam_vtdec_t *d, const uint8_t *annexb, size_t len, int64_t pts_us)
{
	if (!d->session)
		return false;

	// Annex-B -> AVCC: every start code becomes a 4-byte big-endian length.
	if (len + 8 > d->avcc_cap) {
		d->avcc_cap = len + 8;
		d->avcc = brealloc(d->avcc, d->avcc_cap);
	}
	size_t out = 0;
	const uint8_t *p = annexb, *end = annexb + len;
	const uint8_t *nal;
	size_t nlen;
	while ((nal = next_nal(p, end, &nlen)) != NULL) {
		p = nal + nlen;
		if (out + 4 + nlen > d->avcc_cap) {
			d->avcc_cap = (out + 4 + nlen) * 2;
			d->avcc = brealloc(d->avcc, d->avcc_cap);
		}
		d->avcc[out++] = (uint8_t)(nlen >> 24);
		d->avcc[out++] = (uint8_t)(nlen >> 16);
		d->avcc[out++] = (uint8_t)(nlen >> 8);
		d->avcc[out++] = (uint8_t)nlen;
		memcpy(d->avcc + out, nal, nlen);
		out += nlen;
	}
	if (out == 0)
		return false;

	CMBlockBufferRef block = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, d->avcc, out, kCFAllocatorNull, NULL, 0, out, 0,
					       &block) != noErr)
		return false;

	CMSampleBufferRef sample = NULL;
	const size_t sample_size = out;
	CMSampleTimingInfo timing = {.duration = kCMTimeInvalid,
				     .presentationTimeStamp = CMTimeMake(pts_us, 1000000),
				     .decodeTimeStamp = kCMTimeInvalid};
	OSStatus st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, d->fmt, 1, 1, &timing, 1, &sample_size,
						&sample);
	CFRelease(block);
	if (st != noErr || !sample)
		return false;

	VTDecodeInfoFlags info = 0;
	st = VTDecompressionSessionDecodeFrame(d->session, sample,
					       kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info);
	CFRelease(sample);
	return st == noErr;
}

void spancam_vtdec_destroy(spancam_vtdec_t *d)
{
	if (!d)
		return;
	teardown_session(d);
	free_sets(d);
	bfree(d->avcc);
	bfree(d);
}

#endif // __APPLE__
