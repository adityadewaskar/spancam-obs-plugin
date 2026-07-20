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

#if defined(_WIN32)
#include <winsock2.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

/* src/spancam-source.c */
extern struct obs_source_info spancam_source_info;

bool obs_module_load(void)
{
#if defined(_WIN32)
	// Winsock has to be initialised per process before any socket call. OBS
	// itself already does this, but a plugin can't assume load order.
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
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
