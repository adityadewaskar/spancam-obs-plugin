# Android sender

The Android Spancam app is the **server** in [SDSP](PROTOCOL.md): it binds
`0.0.0.0:8892`, hardware-encodes the camera with `MediaCodec`, and answers the
plugin's discovery broadcast. This note records what the app side has to get right
for the plugin to "just work" — it lives here so the two ends of the protocol stay
in sync. The app itself is a separate, closed-source project; this is the contract,
not its source.

## Discovery

- The app answers the plugin's `SPANCAM-DISCOVER` UDP broadcast on port 8891 with
  `SPANCAM-OBS|name|port|token|codec|w|h`. The plugin uses the **reply's source
  address** as the host, so the app doesn't have to know which of its interfaces
  is reachable.
- Broadcast is link-local and is dropped by AP/client isolation, guest VLANs and a
  lot of enterprise Wi-Fi. The app therefore shows the raw **IP : port** and the
  access key in large type on the streaming screen, so a user on such a network can
  type them into the plugin's Wi-Fi fields. This is the single most important
  fallback — treat manual entry as a first-class path, not an afterthought.
- To receive the broadcast at all, the app must hold a `WifiManager.MulticastLock`
  for the life of the streaming service (permission
  `CHANGE_WIFI_MULTICAST_STATE`). Without it the Wi-Fi chip drops the incoming UDP
  and the phone is discoverable-but-unreachable — a real bug we hit.

## Foreground service

Streaming with the screen off requires a foreground service typed `camera`:

```xml
<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_CAMERA"/>
<uses-permission android:name="android.permission.CAMERA"/>
<uses-permission android:name="android.permission.WAKE_LOCK"/>
<service android:name=".StreamService"
         android:foregroundServiceType="camera"
         android:exported="false"/>
```

Notes that bite:

- `CAMERA` is a while-in-use type, so on Android 14+ the service **cannot be
  started from the background** — start it from a user action in a visible
  activity. Return `START_NOT_STICKY`, or an OS relaunch counts as a background
  start and throws.
- Do **not** type the streaming service `dataSync` — Android 15's 6-hour FGS
  timeout applies to `dataSync`/`mediaProcessing`, not `camera`. A long stream
  under `camera` is fine.
- Hold a `PARTIAL_WAKE_LOCK` inside the service. OEM battery management (Samsung
  especially, which this project targets) is the usual cause of a stream that dies
  after a while; an in-app check plus a user-initiated
  `ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` prompt is the honest mitigation.

## Encoder settings

For frames that decode cleanly and with low latency on the plugin side:

- `COLOR_FormatSurface` input (zero-copy from Camera2), CBR bitrate mode, B-frames
  disabled so DTS == PTS and the plugin needs no reorder buffer.
- Send the codec config (SPS/PPS, plus VPS for HEVC) as an SDSP type-1 packet at
  the head of every connection, and repeat SPS/PPS in-band on each IDR — but scan
  the IDR first and only prepend if absent, since some vendor encoders already
  emit them.
- A long GOP is fine **because** the plugin can ask for a keyframe (SDSP `0x34`):
  it requests one on connect and on any decode error. If a no-back-channel mode
  ever ships, drop the GOP to ≤ 2 s.
- Honour the plugin's `0x30` bitrate requests with
  `MediaCodec.setParameters(PARAMETER_KEY_VIDEO_BITRATE, …)` (no reconfigure), and
  `0x34` with `PARAMETER_KEY_REQUEST_SYNC_FRAME`. Clamp anything the plugin asks
  for to what the encoder can actually deliver — the plugin isn't trusted to know
  the hardware's limits.

## USB

USB is `adb forward tcp:8892 tcp:8892` on the computer, which the plugin runs
itself; on the phone the connection arrives on loopback and the token check is
skipped. It requires USB debugging enabled, which is the biggest single source of
USB support questions — the app should say so where it offers the USB option.

## Android 16 / 17 local-network permission

Android 16 gates local networking behind a new runtime permission (opt-in on 16,
mandatory at `targetSdk ≥ 37`), and it gates **accepting inbound TCP** — which is
exactly the phone-as-server design here. Declare `ACCESS_LOCAL_NETWORK` only when
targeting SDK 37+, ship a rationale screen, and keep the manual IP path working for
users who decline.
