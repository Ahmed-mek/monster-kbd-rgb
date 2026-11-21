#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Removes everything install-system.sh added.  Run:  sudo ./uninstall-system.sh
set -uo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/driver/dkms.conf")

rmmod monster_kbd 2>/dev/null
dkms remove "monster-kbd/$VER" --all 2>/dev/null
rm -rf "/usr/src/monster-kbd-$VER"
rm -f /etc/modules-load.d/monster-kbd.conf /etc/udev/rules.d/90-monster-kbd.rules
rm -f /usr/local/bin/monster-kbd-fx /usr/local/lib/systemd/user/monster-kbd-fx@.service
udevadm control --reload
echo "system part removed (the dkms package itself is left installed)"
