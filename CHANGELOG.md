# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- "Spancam Camera" source: connects to the phone's direct-stream server (SDSP) and
  decodes H.264/HEVC into OBS.
- Auto connection: USB (`adb forward`) when a cable is present, else Wi-Fi
  broadcast discovery, else a manually entered host.
- Hardware decode through FFmpeg's hwaccel API — VideoToolbox (macOS), D3D11VA
  (Windows), VA-API (Linux) — with a software fallback.
- Closed-loop bitrate control and keyframe recovery over the SDSP back-channel.
- Receiver-side mirror and rotation following the phone (SDSP type-3 transform),
  plus a manual rotation override.
- macOS, Windows and Linux builds.

Nothing is released yet. The first tagged release will be `1.0.0`.
