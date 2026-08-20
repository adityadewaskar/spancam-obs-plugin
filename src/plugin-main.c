/*
Spancam OBS Studio plugin
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
with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include <obs-module.h>
#include <plugin-support.h>

#if !defined(__APPLE__)
#include <libavcodec/avcodec.h>
#endif
#if !defined(__APPLE__)
#include <libavutil/avutil.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* src/spancam-source.c */
extern struct obs_source_info spancam_source_info;

// macOS decodes with VideoToolbox and links no FFmpeg at all, so there is nothing
// to check there — which is the entire point of that backend. Windows and Linux
// still decode with the libavcodec OBS ships, and that library has no
// cross-major ABI promise: a plugin compiled against major N can misbehave in
// obscure ways beside a runtime major N±1, which is exactly what happens the day
// OBS bumps its FFmpeg. Compare the majors at load and say so plainly rather than
// letting it become a mystery bug report. Logged, not fatal: minor drift is
// harmless and refusing to load would be worse than a warning.
#if defined(__APPLE__)
static void spancam_check_ffmpeg(void) {}
#else
static void spancam_check_ffmpeg(void)
{
	unsigned built = LIBAVCODEC_VERSION_MAJOR;
	unsigned runtime = AV_VERSION_MAJOR(avcodec_version());
	if (built != runtime) {
		obs_log(LOG_WARNING,
			"Spancam: built against libavcodec %u but OBS is running %u — "
			"decode may misbehave; rebuild the plugin against this OBS version",
			built, runtime);
	} else {
		obs_log(LOG_INFO, "Spancam: libavcodec %u (%s)", runtime, av_version_info());
	}
}
#endif

#include "spancam-placeholder.h"

bool obs_module_load(void)
{
#if defined(_WIN32)
	// Winsock has to be initialised per process before any socket call. OBS
	// itself already does this, but a plugin can't assume load order.
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	spancam_check_ffmpeg();
	obs_register_source(&spancam_source_info);
	obs_log(LOG_INFO, "Spancam plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	spancam_placeholder_free();
#if defined(_WIN32)
	WSACleanup();
#endif
	obs_log(LOG_INFO, "Spancam plugin unloaded");
}
