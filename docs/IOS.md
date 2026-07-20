# iOS sender

**Status: in progress.** The iOS Spancam app does not yet ship an SDSP sender, so
the plugin can't take an iPhone feed today. This note is the design for it — the
plugin end already works, since SDSP is transport-agnostic. When the app ships the
sender, an iPhone will appear to the plugin exactly like an Android phone.

## Why it's cheap

The load-bearing fact: **accepting an inbound TCP connection on iOS does not require
Local Network permission — only Bonjour discovery does**
([TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)).
So the iPhone is the **server** (an `NWListener`) and the plugin is the client,
identical to Android. USB and manual-IP Wi-Fi need no privacy permission at all;
discovery is a convenience on top.

## The sender

- An `NWListener` on unconstrained `NWParameters`. Do **not** set
  `requiredInterfaceType = .wifi` or `prohibitExpensivePaths` on the listener — USB
  (usbmux) arrives on loopback and those flags would kill it. Decide wired-vs-Wi-Fi
  for the UI label from the accepted connection's path, not the listener.
- `includePeerToPeer = false`, or users get the local-network prompt with no Wi-Fi
  symbol (AWDL).
- Byte-identical SDSP to Android: same Annex-B framing, same type-1 config packet
  at connection head, same PTS domain, same `0x30`/`0x34` back-channel. VideoToolbox
  on iOS produces the same NV12/Annex-B the plugin already decodes.
- Port strategy is split: a **fixed** port for USB (usbmux connects to a raw port,
  there's no discovery over the cable) and an **ephemeral** port published in the
  Bonjour TXT record for Wi-Fi. The ephemeral port sidesteps `NWListener`'s
  `TIME_WAIT` port-reuse bug on restart.

## Info.plist

```xml
<key>NSLocalNetworkUsageDescription</key>
<string>Spancam advertises this iPhone so OBS on your computer can find it.</string>
<key>NSBonjourServices</key>
<array><string>_spancam-sdsp._tcp</string></array>
<key>NSCameraUsageDescription</key>
<string>Spancam streams this iPhone's camera to your computer.</string>
```

- The Bonjour type must be the exact string with the leading underscore and
  `._tcp`, or registration fails with a policy error and the app never even appears
  under Settings → Privacy → Local Network.
- On iOS 18 the permission prompt does not appear at all if
  `NSLocalNetworkUsageDescription` is missing. The Simulator doesn't implement
  local-network privacy — this has to be tested on a device.

## No multicast entitlement

`com.apple.developer.networking.multicast` is **not** needed. It's required only
for raw UDP multicast/broadcast or for browsing/advertising *undeclared* Bonjour
types. Advertising a **declared** type with `NWListener` uses the system mDNS
responder, which does the multicast outside the app's process. (This is also why
discovery is Bonjour, not the UDP broadcast the Android path uses — Network
framework has no UDP broadcast, and going through Bonjour avoids the entitlement.)

## Background and lock

Every comparable app is foreground-only on iOS, and this should be too, stated
honestly in the UI:

- The camera can't run in the background (`AVCaptureSession` is interrupted with
  `videoDeviceNotAvailableInBackground`), and process suspension stops networking.
- On resign-active: send a "pausing" control frame, `stopRunning()`, and **cancel**
  the `NWListener`. On become-active: build a **fresh** listener (never `start()`
  one twice), re-advertise, resume capture. The plugin shows "phone asleep —
  reconnecting" and re-dials with backoff.
- Set `isIdleTimerDisabled` while a client is connected so the screen doesn't lock
  mid-stream, and release it on disconnect.

## USB over usbmux

usbmux lets a host process open a TCP connection to a localhost port inside the app
over the cable — macOS ships the daemon, Windows gets it from Apple Mobile Device
Support, Linux runs the open-source one. Because OBS is not sandboxed, the plugin
can reach it. This is a later addition; Wi-Fi is the first target.
