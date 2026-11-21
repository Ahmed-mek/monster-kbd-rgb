// SPDX-License-Identifier: GPL-2.0
/*
 * monster_ec_probe - READ-ONLY EC probe for Monster TULPAR T7 V20.2
 * (Uniwill GK7MR0R) keyboard backlight registers.
 *
 * Path: WMI GUID ABBC0F6F-8EA1-11D1-00A0-C90629100000 (object "BC")
 *       -> \_SB.AMW0.WMBC, Arg1 (method id) = 4
 *       -> AC00 = input; OEMG(AC00)
 *
 * OEMG dispatches on SAC1 = DWORD at AC00[4..7] (little endian):
 *   SAC1 == 0x00000000 -> WKBC  (EC WRITE)   <-- must never happen here
 *   SAC1 == 0x00000001 -> WKBC  (EC WRITE)   <-- must never happen here
 *   SAC1 == 0x00000100 -> RKBC  (EC READ)    <-- the only thing we send
 *   SAC1 == 0x00000200 -> SCMD  (EC command) <-- must never happen here
 * So byte4=0, byte5=1, byte6=0, byte7=0 is the one and only read selector.
 *
 * RKBC itself still writes the EC mailbox (LDAT/HDAT/RFLG/DRDY/BFLG at EC
 * 0x8A-0x8C) and port 0x80 (POST code) - that is the read handshake, not a
 * write to the target register.
 *
 * Result: AC00[0] = CMDL (data low byte), AC00[1] = CMDH (data high byte).
 * Failure: AC00 filled with 0xFE (timeout / EC busy / ECON == 0).
 *
 * This module does nothing but read a fixed whitelist of registers at load
 * time and print them. No module parameters, no sysfs, no write path.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/string.h>

#define AMW0_BC_GUID   "ABBC0F6F-8EA1-11D1-00A0-C90629100000"
#define WMBC_METHOD_ID 4
#define AC00_LEN       40

static const struct {
	u16 addr;
	const char *name;
} regs[] = {
	{ 0x0766, "feature (bit2 = RGB kbd supported)" },
	{ 0x0767, "control (bit5 = apply color)" },
	{ 0x0769, "R (0..50)" },
	{ 0x076A, "G (0..50)" },
	{ 0x076B, "B (0..50)" },
	{ 0x078C, "kbd state / brightness" },
};

static int mec_wmi_read(u16 addr, u8 *val)
{
	u8 in[AC00_LEN] = { 0 };
	struct acpi_buffer input = { sizeof(in), in };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;
	u32 sel;

	in[0] = addr & 0xff;	/* SA00 -> LDAT */
	in[1] = addr >> 8;	/* SA01 -> HDAT */
	in[5] = 0x01;		/* SAC1 = 0x00000100 -> RKBC */

	/* Hard guard: refuse to send anything that is not the read selector. */
	sel = in[4] | in[5] << 8 | in[6] << 16 | (u32)in[7] << 24;
	if (sel != 0x00000100) {
		pr_err("monster_ec_probe: selector 0x%08x is not READ, aborting\n", sel);
		return -EINVAL;
	}

	status = wmi_evaluate_method(AMW0_BC_GUID, 0, WMBC_METHOD_ID,
				     &input, &output);
	if (ACPI_FAILURE(status)) {
		pr_err("monster_ec_probe: WMBC failed: %s\n",
		       acpi_format_exception(status));
		return -EIO;
	}

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length < 2) {
		pr_err("monster_ec_probe: unexpected return type %d\n",
		       obj ? obj->type : -1);
		goto out;
	}

	if (obj->buffer.pointer[0] == 0xFE && obj->buffer.pointer[1] == 0xFE &&
	    obj->buffer.length >= 8 &&
	    !memchr_inv(obj->buffer.pointer, 0xFE, 8)) {
		pr_warn("monster_ec_probe: 0x%04x -> EC timeout/busy (0xFE fill)\n",
			addr);
		ret = -ETIMEDOUT;
		goto out;
	}

	*val = obj->buffer.pointer[0];
	ret = 0;
out:
	kfree(output.pointer);
	return ret;
}

static int __init monster_ec_probe_init(void)
{
	int i;

	if (!wmi_has_guid(AMW0_BC_GUID)) {
		pr_err("monster_ec_probe: GUID %s not present\n", AMW0_BC_GUID);
		return -ENODEV;
	}

	pr_info("monster_ec_probe: read-only probe via AMW0.WMBC method 4\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u8 v;

		if (!mec_wmi_read(regs[i].addr, &v))
			pr_info("monster_ec_probe: EC[0x%04x] = 0x%02x (%3u)  %s\n",
				regs[i].addr, v, v, regs[i].name);
	}
	return 0;
}

static void __exit monster_ec_probe_exit(void)
{
	pr_info("monster_ec_probe: unloaded\n");
}

module_init(monster_ec_probe_init);
module_exit(monster_ec_probe_exit);

MODULE_DESCRIPTION("Read-only EC probe for Monster/Uniwill keyboard RGB registers");
MODULE_LICENSE("GPL");
