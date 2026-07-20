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

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

#if defined(_WIN32)
#include <winsock2.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* src/spancam-source.c */
extern struct obs_source_info spancam_source_info;

// The plugin decodes with the libavcodec that OBS ships (obs-deps). That library
// has no cross-major-version ABI promise, so a plugin compiled against major N
// can crash in obscure ways when loaded next to a runtime major N±1 — which is
// exactly what happens the day OBS bumps its FFmpeg. Compare the two majors at
// load and say so plainly, rather than letting it turn into a mystery segfault in
// a bug report. A mismatch is logged, not fatal: the common minor-version drift
// is harmless, and refusing to load would be worse than a warning.
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
#if defined(_WIN32)
	WSACleanup();
#endif
	obs_log(LOG_INFO, "Spancam plugin unloaded");
}
