# Building

This repo is built from the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
scaffolding, which is vendored here (the `cmake/`, `.github/`, `build-aux/` trees
and the presets). You do not need to fetch anything to build — a clone is enough.

## Prerequisites

- CMake 3.28+
- A C toolchain: Xcode (macOS), Visual Studio 2022 (Windows), or GCC/Clang + Ninja
  (Linux). On Ubuntu 24.04 you also need `pkg-config`, `ninja-build` and
  `libsimde-dev`.

The OBS SDK, prebuilt dependencies and FFmpeg are downloaded automatically by the
configure step, at the versions pinned in `buildspec.json`.

## Configure and build

```sh
cmake --preset macos            # macos | windows-x64 | ubuntu-x86_64
cmake --build --preset macos
```

The result is under `build_macos/` (or the platform equivalent): a `.plugin`
bundle on macOS, a `.dll` on Windows, a `.so` on Linux.

To try it locally, copy the built artifact into your user plugins directory:

- macOS: `~/Library/Application Support/obs-studio/plugins/`
- Windows: `%ProgramData%\obs-studio\plugins\`
- Linux: `~/.config/obs-studio/plugins/`

## Formatting

CI enforces both formatters, so run them before pushing:

```sh
./build-aux/.run-format.zsh                 # clang-format over src/
gersemi -i CMakeLists.txt                   # CMake formatting
```

## Releases (CI)

The GitHub Actions workflows from the template build all three platforms on every
push and PR. A **release** is cut by pushing a tag in bare semver — `1.0.0`,
`1.0.0-rc1` — **with no `v` prefix** (the release job silently skips a `v`-prefixed
tag). That flips the build to Release config, signs and notarizes on macOS, and
uploads the per-platform artifacts to a draft GitHub Release.

### macOS signing and notarization

OBS on Apple Silicon will not load an unsigned plugin, and an unsigned `.pkg`
downloads as "damaged". A real release therefore needs, configured as repository
secrets (see `.github/workflows` and the template's
[Codesigning wiki](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS)).
The values are the same Apple-account material the Spancam desktop app's CI uses
(team `539PMW6EW4`) — copy them across:

| Secret | Value |
|---|---|
| `MACOS_SIGNING_CERT` | base64 of one `.p12` holding **both** "Developer ID Application" **and** "Developer ID Installer" (export both together from Keychain Access) |
| `MACOS_SIGNING_CERT_PASSWORD` | the `.p12` export password |
| `MACOS_SIGNING_APPLICATION_IDENTITY` | `Developer ID Application: Aditya Dewaskar (539PMW6EW4)` |
| `MACOS_SIGNING_INSTALLER_IDENTITY` | `Developer ID Installer: Aditya Dewaskar (539PMW6EW4)` |
| `MACOS_KEYCHAIN_PASSWORD` | any random string (throwaway CI keychain) |
| `MACOS_NOTARIZATION_USERNAME` | your Apple ID email — the spancam repo's `APPLE_ID` |
| `MACOS_NOTARIZATION_PASSWORD` | app-specific password — the spancam repo's `APPLE_APP_SPECIFIC_PASSWORD` |

No provisioning profile is needed (that input is for apps, not plugins).

after which CI signs, `notarytool submit --wait`, and staples. Self-builders who
don't sign can load their own build after clearing quarantine
(`xattr -dr com.apple.quarantine <bundle>`), which is fine for development but not
how you'd distribute.
