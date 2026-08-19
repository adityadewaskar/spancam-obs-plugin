# Spancam for OBS

Use your phone as a camera source in [OBS Studio](https://obsproject.com) — over
Wi-Fi or USB, with no desktop helper app sitting in between. The phone
hardware-encodes H.264/HEVC (cheap, dedicated silicon, so it stays cool) and this
plugin decodes it on the computer and hands raw frames to OBS.

> **Spancam for OBS is a third-party plugin. It is not affiliated with, endorsed
> by, or part of the OBS Project.** "OBS" and "OBS Studio" are trademarks of their
> respective owners. This plugin talks only to the Spancam phone app over a
> documented TCP protocol ([docs/PROTOCOL.md](docs/PROTOCOL.md)) — it does not link
> to or bundle any part of the (separate, closed-source) Spancam desktop app.

It needs the **Spancam** app on your phone:

- Android — [Google Play](https://adewaskar.com/apps/spancam)
- iOS — see [docs/IOS.md](docs/IOS.md) for status

---

## What it does

Add a **Spancam Camera** source in OBS and it connects to the phone's direct-stream
server, receives hardware-encoded H.264/HEVC over TCP, and decodes it — with
hardware decode (VideoToolbox on macOS, and D3D11VA/VA-API elsewhere) and a software
fallback. It finds the phone for you: over USB if a cable is plugged in, otherwise
by a broadcast on the local network, otherwise from a host you type in.

This is the raw camera. Spancam's on-device effects (eye contact, background,
super-resolution) live in the desktop app and are not part of this path — the point
of the plugin is to avoid needing that app at all.

### Adding effects

OBS is built around *sources and filters*, so effects compose on top of this source
rather than being baked into it:

- **Background removal, blur, colour** — add an OBS filter to the Spancam Camera
  source. The community [Background Removal](https://github.com/locaal-ai/obs-backgroundremoval)
  filter works well and runs on your computer, not the phone.
- **Spancam's own processing** (eye contact, super-resolution, background) — install
  the Spancam desktop app and pick its **virtual camera** as a regular *Video Capture
  Device* in OBS. That path runs Spancam's effects on your computer and doesn't use
  this plugin.

Either way the phone only ever captures and encodes; all image processing happens on
the computer.

## Install

Grab the release for your platform from
[Releases](https://github.com/adityadewaskar/spancam-obs-plugin/releases), then:

**macOS** — open the `.pkg` and follow the installer. It installs to
`~/Library/Application Support/obs-studio/plugins/`. The build is a signed,
notarized universal binary (Apple Silicon + Intel); OBS on Apple Silicon will not
load unsigned plugins, so use the release `.pkg` rather than a self-built bundle
unless you sign it yourself.

**Windows** — run the `-windows-x64-Installer.exe` and follow the prompts (it
installs into `%ProgramData%\obs-studio\plugins\`; remove it again via *Add or
remove programs*). Or unzip the `.zip` into that same directory yourself. For the
USB path you need `adb` on your `PATH` (Android Platform Tools).

**Linux** — install the `.deb` (`sudo apt install ./spancam-for-obs-*.deb`), or
unpack the `.tar.xz` and run `./install.sh` — it puts the plugin into
`~/.config/obs-studio/plugins/` (`./uninstall.sh` removes it). `adb` on `PATH`
for USB.

Restart OBS, then **Sources → + → Spancam Camera**.

## Using it

1. Open the Spancam app on your phone and turn on the OBS / direct-stream mode.
2. In OBS, add a **Spancam Camera** source.
3. Leave **Connection** on **Auto**. Plugged in over USB, it uses the cable; on the
   same Wi-Fi, it finds the phone by broadcast. Either way you shouldn't have to
   type anything.
4. If auto-discovery can't reach the phone (guest networks and many corporate
   access points block broadcast between devices), switch **Connection** to
   **Wi-Fi** and type the **Phone IP** and **Access key** shown in the app.

**USB** needs USB debugging enabled on Android and `adb` installed on the computer.
The plugin runs `adb forward` itself and connects over loopback.

## Compatibility

| | Status |
|---|---|
| OBS Studio | 31.x (built against 31.1.1) |
| macOS | 12.0+, Apple Silicon + Intel — **verified** |
| Windows | 10/11 x64 — builds in CI, community testing wanted |
| Linux | Ubuntu 24.04 x86_64 — builds in CI, community testing wanted |
| Android app | H.264 + HEVC sender — **verified** (Galaxy S24 Ultra) |
| iOS app | sender implemented (Bonjour discovery on macOS, or manual IP anywhere) — pending device verification, see [docs/IOS.md](docs/IOS.md) |

## Build from source

```sh
cmake --preset macos          # or: windows-x64, ubuntu-x86_64
cmake --build --preset macos
```

The configure step downloads the OBS sources and prebuilt dependencies pinned in
`buildspec.json`. See [docs/BUILDING.md](docs/BUILDING.md) for signing,
notarization, and the CI release flow.

## How it works

The wire protocol is written up in [docs/PROTOCOL.md](docs/PROTOCOL.md): one TCP
socket, a 24-byte stream header, then framed Annex-B access units, with a small
back-channel the plugin uses to ask for keyframes and to steer the bitrate when the
link gets congested. The phone-app side is in [docs/ANDROID.md](docs/ANDROID.md)
and [docs/IOS.md](docs/IOS.md).

## AI tool disclosure

This plugin was written with substantial help from an AI coding assistant (Claude).
I used it throughout — drafting the SDSP client and the FFmpeg decode path, the
Winsock portability layer, and this documentation. I have read and tested the code
on macOS (Apple Silicon and Intel) with a Galaxy S24 Ultra as the sender, and I
maintain it. I'm stating this plainly because it's true and because the OBS forum
requires it; it is not written to imply more hand-authorship than there was. If
you're evaluating this for the OBS resource directory, please read the policy on
AI-assisted resources first — this disclosure exists to comply with it, not to
sidestep it.

## License

GPL-2.0-or-later. OBS plugins link `libobs`, which is GPL-2.0, so a plugin is a
combined work and must be GPL with source available — see [LICENSE](LICENSE). The
Spancam phone and desktop apps are separate programs and are not covered by this
license.
