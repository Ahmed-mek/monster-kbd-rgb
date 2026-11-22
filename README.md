# monster-kbd-rgb

Keyboard RGB backlight control on Linux for the **MONSTER TULPAR T7 V20.2**
(Uniwill barebone **GK7MR0R**, single-zone RGB keyboard), with a GNOME Shell
Quick Settings toggle and software lighting effects.

[العربية](#بالعربي) below.

> ⚠️ **Unofficial, community project.** Not affiliated with MONSTER or Uniwill.
> The driver writes to the laptop's Embedded Controller (EC). It is limited to
> the four keyboard-color registers and has only been tested on the machine
> above. Use at your own risk.

## Why

The upstream `uniwill_laptop` driver binds to ACPI `INOU0000`, which this
machine does not have, and `tuxedo-drivers` only loads on TUXEDO machines.
This project talks to the same EC registers through the WMI method that the
MONSTER Control Center uses on Windows.

## What you get

- **`monster_kbd` kernel module**, registered as a standard **multicolor LED**
  (`led-class-multicolor`), so tools that use the kernel LED API can drive it →
  `/sys/class/leds/monster::kbd_backlight/`
  - `multi_index` — `red green blue`
  - `multi_intensity` — `R G B`, each `0..255`
  - `brightness` — `0..255` (software, scales the color)
  - `color` — `R G B`, each `0..50` (EC native scale, same state as `multi_intensity`)
  - `hw_level` — read-only, brightness level `0..4` set with the Fn keys
- **`monster-kbd-fx`** — effects: rainbow, breathe, fire, aurora, ocean,
  heartbeat, random, party, police, and status effects (CPU load, CPU
  temperature, battery level)
- **GNOME Shell extension** (GNOME 50) — a *Keyboard* toggle in Quick Settings
  with the effects and preset colors; your choice is restored at login

### Hardware limits

The EC accepts a new color only about **twice per second** and the whole
keyboard is **one zone**, so there are no per-key effects (wave, ripple…) and
no truly smooth fades. Effects are designed around that. Writing `0 0 0`
makes the EC fall back to its default color, so the keyboard cannot be
switched off from software; use the Fn keys.

## Requirements

- MONSTER TULPAR T7 V20.2 (other Uniwill machines: see [Other models](#other-models))
- Ubuntu 26.04 / GNOME 50 (other distros should work for the driver and `monster-kbd-fx`)
- Kernel headers for your kernel, `dkms`, `python3`
- Secure Boot: disabled, or sign the module with your MOK (DKMS does this on Ubuntu if a MOK is enrolled)

## Install

```bash
git clone https://github.com/Ahmed-mek/monster-kbd-rgb.git
cd monster-kbd-rgb
sudo ./install-system.sh     # DKMS driver, autoload, udev rule, effects service
./install-user.sh            # GNOME extension (no sudo)
```

Log out and back in (Wayland loads new extensions only at login).
Your user must be in the `plugdev` group (default on Ubuntu desktop).

The GNOME extension is also on
[extensions.gnome.org](https://extensions.gnome.org/extension/11895/monster-keyboard-rgb/). You can install it from there instead of
running `./install-user.sh`, but you still need `sudo ./install-system.sh` for
the driver.

### Uninstall

```bash
./install-user.sh --remove
sudo ./uninstall-system.sh
```

## Command line

```bash
echo "255 0 128" > /sys/class/leds/monster::kbd_backlight/multi_intensity
echo 128 > /sys/class/leds/monster::kbd_backlight/brightness
echo "50 0 25" > /sys/class/leds/monster::kbd_backlight/color   # EC scale 0..50
monster-kbd-fx --list
monster-kbd-fx fire                 # Ctrl+C restores the previous color
monster-kbd-fx static --color 0 50 50
systemctl --user start monster-kbd-fx@aurora.service
```

## Other models

The driver refuses to load on machines that are not in its DMI table. Many
Uniwill-based laptops (sold as MONSTER, XMG, Schenker, Eluktronics, …) use
the same WMI interface. If yours has a single-zone RGB keyboard:

1. Build and load the **read-only** probe in [`tools/ec-probe`](tools/ec-probe)
   and check `EC[0x0766]` has bit `0x04` set.
2. Try the driver with `sudo modprobe monster_kbd force=1` (after `install-system.sh`).
3. If it works, open an issue with the output of
   `cat /sys/class/dmi/id/{sys_vendor,product_name,board_name}` so the model
   can be added.

Read [docs/EC.md](docs/EC.md) before experimenting.

## How it works

See [docs/EC.md](docs/EC.md): WMI GUID, the 40-byte buffer format, the
register map, and what was verified on real hardware.

## License

GPL-2.0. See [LICENSE](LICENSE).

---

## بالعربي

تحكم في إضاءة الكيبورد RGB على لينكس للابتوب **MONSTER TULPAR T7 V20.2**
(Uniwill GK7MR0R، كيبورد RGB منطقة واحدة)، مع زرار في Quick Settings بتاع
GNOME وتأثيرات إضاءة.

> ⚠️ **مشروع غير رسمي** ومالوش علاقة بشركة MONSTER أو Uniwill. الدرايفر بيكتب
> في الـEC بتاع اللابتوب، والكتابة مسموحة على سجلات لون الكيبورد الأربعة بس،
> واتجرب على الموديل ده بس. الاستخدام على مسؤوليتك.

### التثبيت

```bash
git clone https://github.com/Ahmed-mek/monster-kbd-rgb.git
cd monster-kbd-rgb
sudo ./install-system.sh
./install-user.sh
```

بعدها اعمل تسجيل خروج ودخول، وهتلاقي زرار **Keyboard** في القايمة اللي
فوق على اليمين، وفيه التأثيرات والألوان.

الإضافة موجودة كمان على [extensions.gnome.org](https://extensions.gnome.org/extension/11895/monster-keyboard-rgb/)، وتقدر تسطّبها من هناك
بدل `./install-user.sh`. بس لازم تشغّل `sudo ./install-system.sh` الأول علشان
الدرايفر.

### حدود الهاردوير

- الـEC بيقبل لون جديد حوالي **مرتين في الثانية** بس.
- الكيبورد **منطقة واحدة**، فمفيش تأثيرات على كل زرار لوحده.
- مينفعش تطفي الإضاءة من السوفت وير، لأن الـEC بيرجع للون الافتراضي لما يستقبل
  `0 0 0`. استخدم زراير Fn.

### الإزالة

```bash
./install-user.sh --remove
sudo ./uninstall-system.sh
```
