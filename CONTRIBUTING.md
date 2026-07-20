# Contributing

Thanks for your interest. This is a small project; issues and pull requests are
welcome.

## Reporting a problem

Open an issue and include:

- OBS Studio version and your OS (and CPU — Apple Silicon vs Intel matters).
- The phone model and the Spancam app version.
- Whether it's Wi-Fi or USB, and which **Connection** mode.
- The relevant lines from the OBS log (**Help → Log Files → View Current Log**);
  the plugin logs everything it does with a `Spancam:` prefix.

Discovery and connection problems are usually the network, not the plugin —
broadcast doesn't cross subnets and a lot of guest/enterprise Wi-Fi blocks it
between devices. Try the manual **Wi-Fi** host as a first step, and say whether
that worked when you file.

## Building

See the [README](README.md#build-from-source) and
[docs/BUILDING.md](docs/BUILDING.md).

## Pull requests

- Match the existing style. C code is formatted with `clang-format` (the config is
  in the repo); CMake with `gersemi`. CI checks both, so run them before pushing.
- Keep commits focused and explain *why* in the message, not just what.
- The wire protocol is shared with the phone apps. Anything that changes
  [docs/PROTOCOL.md](docs/PROTOCOL.md) needs the matching app-side change or it
  will break existing users — flag it in the PR.
- By contributing you agree your changes are licensed under GPL-2.0-or-later.
