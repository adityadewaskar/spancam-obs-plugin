# obs-spancam

An OBS Studio source plugin for [Spancam](https://adewaskar.com/apps/spancam) —
use your phone's camera as a capture source in OBS, over Wi-Fi or USB, with no
desktop helper app running in between.

Status: early. The plugin builds against OBS 31 and does nothing useful yet.

## Build

```sh
cmake --preset macos
cmake --build --preset macos
```

The configure step downloads the OBS sources and prebuilt dependencies pinned in
`buildspec.json`.

## License

GPL-2.0-or-later. OBS plugins link `libobs`, which is GPL-2.0, so this has to be
GPL too — see [LICENSE](LICENSE).
