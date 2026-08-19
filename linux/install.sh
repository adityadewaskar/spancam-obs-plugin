#!/bin/sh
# Spancam for OBS — per-user install for any Linux distro.
# Run from inside the extracted spancam-for-obs-*.tar.xz:
#   ./install.sh
# Copies the plugin into ~/.config/obs-studio/plugins/spancam-for-obs.
# For Debian/Ubuntu you can use the .deb instead — it installs system-wide.
set -eu

cd "$(dirname "$0")"

PLUGIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/spancam-for-obs"

SO_FILE="$(ls lib/*/obs-plugins/spancam-for-obs.so 2>/dev/null | head -n 1 || true)"
if [ -z "$SO_FILE" ]; then
    echo "error: lib/*/obs-plugins/spancam-for-obs.so not found — run this from the extracted tarball directory." >&2
    exit 1
fi

mkdir -p "$PLUGIN_DIR/bin/64bit"
cp "$SO_FILE" "$PLUGIN_DIR/bin/64bit/spancam-for-obs.so"

if [ -d share/obs/obs-plugins/spancam-for-obs ]; then
    mkdir -p "$PLUGIN_DIR/data"
    cp -r share/obs/obs-plugins/spancam-for-obs/. "$PLUGIN_DIR/data/"
fi

echo "Installed to $PLUGIN_DIR"
echo "Restart OBS Studio, then add a source: + -> Spancam Camera"
