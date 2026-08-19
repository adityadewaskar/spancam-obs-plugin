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
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

// blogva() comes from here, with the platform-correct linkage. Declaring it
// ourselves collides with that declaration on Windows (MSVC C2375).
extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);

// Declared here instead of including <util/base.h>, because the plugin-support
// target has no OBS include dirs. The declaration must match base.h exactly —
// on MSVC a plain extern collides with their __declspec(dllexport) (C2375).
#ifdef _MSC_VER
__declspec(dllexport) void blogva(int log_level, const char *format, va_list args);
#else
extern void blogva(int log_level, const char *format, va_list args);
#endif

#ifdef __cplusplus
}
#endif
