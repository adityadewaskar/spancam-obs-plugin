/*
Spancam OBS Studio plugin — Android Open Accessory (AOA) transport
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

The cable path, and the reason it exists: `adb forward` is not a cable. It needs
USB debugging bound to the USB transport, and when adb is running wirelessly it
tunnels over Wi-Fi while still emerging on the phone's loopback — so the phone
reads it as "USB, plenty of bandwidth", commits to its 4K rung, and pushes ~25
Mbps across Wi-Fi. Measured on an S24 Ultra: 10 fps delivered and ~3 s of
accumulating delay, against a flat ~0 ms on the honest Wi-Fi rung.

AOA is the transport the Spancam Mac app already uses (mac/Spancam/Core/
AOAReceiver.swift), so this makes OBS behave the same way as the desktop app on
the same cable. Flow: find an Android device -> AOA 51/52/53 control handshake
with identity strings matching the app's accessory_filter.xml -> the phone
re-enumerates as 0x18D1:0x2D0x -> open its bulk interface -> pump bulk reads.

macOS only for now, through IOUSBLib. IOUSBLib rather than IOUSBHost because
that is what is authorized under the App Sandbox (com.apple.security.device.usb)
for the Mac app; the plugin is not sandboxed, but sharing one proven code path
across both is worth more than using the newer API here. A Windows backend
(WinUSB) slots in behind this same header.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct spancam_aoa spancam_aoa_t;

/// True if an Android device — or one already switched to accessory mode — is on
/// the USB bus. Side-effect free: does not open anything or run the handshake.
bool spancam_aoa_device_present(void);

/// Discover, run the AOA handshake if needed, and open the bulk interface.
/// Returns NULL on failure, writing a human-readable reason into err.
spancam_aoa_t *spancam_aoa_open(char *err, size_t errsz);

/// Blocking bulk read. Returns 1 on data (*len set), 0 on a benign timeout
/// (normal between frames), -1 on a fatal error (cable pulled, device gone).
int spancam_aoa_read(spancam_aoa_t *a, void *buf, uint32_t *len, uint32_t timeout_ms);

/// Blocking bulk write of the whole buffer. Returns true on success.
bool spancam_aoa_write(spancam_aoa_t *a, const void *buf, uint32_t len, uint32_t timeout_ms);

/// Close the interface and free. Safe on NULL. Closing while a read is in flight
/// unblocks that read, which is how the receive loop is woken on shutdown.
void spancam_aoa_close(spancam_aoa_t *a);
