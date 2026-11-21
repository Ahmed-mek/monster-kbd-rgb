#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# User part: GNOME Shell extension.  Run as your normal user (no sudo).
# Undo:  ./install-user.sh --remove
set -euo pipefail
[ "$(id -u)" -ne 0 ] || { echo "do NOT run this with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(ls -d "$HERE"/extension/*/ | head -1)
UUID=$(basename "$SRC")
DEST="$HOME/.local/share/gnome-shell/extensions/$UUID"

if [ "${1:-}" = "--remove" ]; then
	gnome-extensions disable "$UUID" 2>/dev/null || true
	systemctl --user stop 'monster-kbd-fx@*.service' 2>/dev/null || true
	rm -rf "$DEST"
	echo "extension removed"
	exit 0
fi

systemctl --user daemon-reload
rm -rf "$DEST"
install -d "$DEST"
install -m644 "$SRC"/metadata.json "$SRC"/extension.js "$DEST/"
gnome-extensions enable "$UUID" 2>/dev/null || true
echo "Installed $UUID."
echo "On Wayland, log out and back in, then (if needed): gnome-extensions enable $UUID"
