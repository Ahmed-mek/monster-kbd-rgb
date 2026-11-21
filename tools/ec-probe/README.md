# ec-probe (read-only)

Prints the keyboard-related EC registers through `\_SB.AMW0.WMBC` method 4.
It only sends the read selector (`0x100`) and refuses anything else.

```bash
make
sudo insmod monster_ec_probe.ko
sudo dmesg | grep monster_ec_probe
sudo rmmod monster_ec_probe
```

It only works if your machine has WMI GUID `ABBC0F6F-8EA1-11D1-00A0-C90629100000`
under `\_SB.AMW0`. Check with:

```bash
ls /sys/bus/wmi/devices/ | grep ABBC0F6F
cat /sys/bus/platform/devices/PNP0C14:*/firmware_node/path
```
