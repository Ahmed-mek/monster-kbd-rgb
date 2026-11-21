# EC interface notes (MONSTER TULPAR T7 V20.2 / Uniwill GK7MR0R)

Everything here was read from the machine's own ACPI tables and confirmed on
real hardware, unless marked *unverified*.

## Path

| Item | Value |
|---|---|
| WMI GUID | `ABBC0F6F-8EA1-11D1-00A0-C90629100000` (object id `BC`) |
| ACPI device | `\_SB.AMW0` (`PNP0C14`, `_UID` 1) |
| Method | `WMBC`, method id **4** |

`WMBC(…, 4, buf)` copies the 40-byte input into `AC00`, then calls `OEMG(AC00)`.

> There is a second `WMBC` under `\_SB.OTA0` (GUID `DBBC0F6F-…`) whose
> method 7 triggers an SMI. Do not use it.

## Buffer (`AC00`, 40 bytes)

| Bytes | Meaning |
|---|---|
| 0 | EC address, low byte (`LDAT`) |
| 1 | EC address, high byte (`HDAT`) |
| 2 | data low byte (`CMDL`), for writes |
| 3 | data high byte (`CMDH`), for writes; 0 |
| 4..7 | **selector** `SAC1`, little-endian DWORD |

| `SAC1` | AML | Operation |
|---|---|---|
| `0x00000000` | `WKBC` | **write** |
| `0x00000001` | `WKBC` | **write** |
| `0x00000100` | `RKBC` | read |
| `0x00000200` | `SCMD` | EC command |

⚠️ An all-zero selector is a **write**. A read must set byte 5 = 1 and keep
bytes 4, 6 and 7 at 0.

The result comes back in the same buffer: byte 0 = value read. If the EC
times out, the buffer is filled with `0xFE`.

Both `RKBC` and `WKBC` use the EC mailbox at EC RAM `0x8A..0x8E`
(`LDAT`, `HDAT`, flags byte, `CMDL`, `CMDH`) and poll with `Sleep(50)`, so every call takes
about **60 ms**.

## Registers

The names come from `LightingModel.EC_SPEC` in the MONSTER Control Center
service (`GCUService.exe`). The same addresses are used by `tuxedo-drivers`
and by mainline `uniwill-acpi.c`.

| Address | Name | Notes |
|---|---|---|
| `0x0740` | project id | *unverified* |
| `0x0766` | `EC_SUPPORT_BYTE2` | bit `0x04` = RGB keyboard present (reads `0xF4` here) |
| `0x0767` | `EC_TRIGGER_BYTE` | bit `0x20` = apply color. `EC_SPEC` also defines a welcome-effect bit `0x80`, register *unverified* |
| `0x0769` | `EC_RGBKB_LEVEL_R` | 0..50 |
| `0x076A` | `EC_RGBKB_LEVEL_G` | 0..50 |
| `0x076B` | `EC_RGBKB_LEVEL_B` | 0..50 |
| `0x076C..E` | default R/G/B | *unverified* |
| `0x078C` | keyboard status | bits 7..5 = Fn brightness level 0..4 (0 = off); bit 3 always 1 here |

## Verified behaviour

- **Set color:** write R, G and B, then write `0x767 | 0x20`. The EC applies
  the color and clears bit `0x20` by itself after about 250–500 ms.
- **Update rate:** at most about 2 visible color changes per second.
- **Black:** writing `0 0 0` and applying makes the EC go back to its default
  color, probably `0x76C..E`, instead of turning the light off.
- **Brightness:** writing bits 7..5 of `0x78C` is stored and reads back, but
  does **not** change the light, even when followed by the apply bit. The
  register only reports the Fn level. The driver never writes it.
- **Effects:** `EC_SPEC` has no effect or speed registers. The Windows
  software appears to run breathing and rainbow on a host timer.

## Probing safely

`tools/ec-probe` is a read-only module. It only sends selector `0x100` and
refuses to send anything else. It prints `0x766`, `0x767`, `0x769..B` and
`0x78C`.
