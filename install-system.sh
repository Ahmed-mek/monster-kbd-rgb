#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# System part: DKMS driver, autoload, udev permissions, effects program and
# user service template.  Run:  sudo ./install-system.sh
# Undo:                        sudo ./uninstall-system.sh
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$HERE/driver/dkms.conf")

echo "== 1/5 dkms"
command -v dkms >/dev/null || apt-get install -y dkms

echo "== 2/5 driver -> /usr/src/monster-kbd-$VER (DKMS)"
# remove any installed version (upgrades included)
for old in $(dkms status monster-kbd 2>/dev/null | sed -n 's#^monster-kbd/\([^,:]*\).*#\1#p' | sort -u); do
	dkms remove "monster-kbd/$old" --all || true
	rm -rf "/usr/src/monster-kbd-$old"
done
rm -rf "/usr/src/monster-kbd-$VER"
install -d "/usr/src/monster-kbd-$VER"
install -m644 "$HERE"/driver/{monster_kbd.c,Makefile,dkms.conf} "/usr/src/monster-kbd-$VER/"
dkms add "monster-kbd/$VER"
# Build for every installed kernel that has headers, not only the running one,
# so a kernel installed before this script (and booted later) also gets it.
for k in /lib/modules/*; do
	k=$(basename "$k")
	[ -e "/lib/modules/$k/build" ] || continue
	dkms install "monster-kbd/$VER" -k "$k" || echo "warning: build failed for $k"
done

echo "== 3/5 autoload + udev rule"
install -m644 "$HERE/modules-load/monster-kbd.conf" /etc/modules-load.d/monster-kbd.conf
install -m644 "$HERE/udev/90-monster-kbd.rules" /etc/udev/rules.d/90-monster-kbd.rules
udevadm control --reload

echo "== 4/5 effects program + user service"
install -m755 "$HERE/fx/monster-kbd-fx" /usr/local/bin/monster-kbd-fx
install -d /usr/local/lib/systemd/user
install -m644 "$HERE/systemd/monster-kbd-fx@.service" /usr/local/lib/systemd/user/

echo "== 5/5 load driver"
rmmod monster_kbd 2>/dev/null || true
# brightness scale changed in 1.1 (0..10 -> 0..255): drop the value
# systemd-backlight saved for the old scale so it is not restored as "dim"
rm -f /var/lib/systemd/backlight/*monster::kbd_backlight* 2>/dev/null || true
if ! modprobe monster_kbd; then
	echo "driver did not load: see 'sudo dmesg | grep monster_kbd'"
	exit 1
fi
udevadm settle
ls -l /sys/class/leds/monster::kbd_backlight/{color,brightness}
echo
echo "done. Now run ./install-user.sh as your normal user (without sudo)."
