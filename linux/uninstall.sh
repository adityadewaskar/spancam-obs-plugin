#!/bin/sh
# Removes the per-user Spancam for OBS plugin installed by install.sh.
set -eu

PLUGIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/spancam-for-obs"

if [ -d "$PLUGIN_DIR" ]; then
    rm -rf "$PLUGIN_DIR"
    echo "Removed $PLUGIN_DIR"
else
    echo "Nothing to remove — $PLUGIN_DIR does not exist."
fi
