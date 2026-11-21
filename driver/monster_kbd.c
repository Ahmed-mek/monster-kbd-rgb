// SPDX-License-Identifier: GPL-2.0
/*
 * monster_kbd - RGB keyboard backlight for Monster TULPAR T7 V20.2
 * (Uniwill GK7MR0R) via WMI GUID ABBC0F6F-... (\_SB.AMW0.WMBC, method 4).
 *
 * /sys/class/leds/monster::kbd_backlight/
 *   brightness      0..10, software: scales the color written to the EC
 *   max_brightness  10
 *   color           "R G B" base color at full brightness, each 0..50
 *   hw_level        read-only, EC brightness level 0..4 set by the Fn keys
 *                   (EC 0x78C bits 7..5)
 *
 * Effective light = Fn level (EC, hardware) x brightness/10 (this driver).
 *
 * Verified on this machine: writing 0x78C is stored but does NOT change the
 * light (even followed by the 0x767 apply bit), so this driver never writes
 * it. Only 0x767 (apply bit) and 0x769/0x76A/0x76B (R/G/B) are writable.
 *
 * AC00 (40 bytes): [0]=addr lo [1]=addr hi [2]=data lo [3]=data hi
 *                  DWORD [4..7]: 0x000 = WKBC write, 0x100 = RKBC read.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/bitfield.h>
#include <linux/math.h>
#include <linux/delay.h>
#include <linux/dmi.h>

#define AMW0_BC_GUID    "ABBC0F6F-8EA1-11D1-00A0-C90629100000"
#define WMBC_METHOD_ID  4
#define AC00_LEN        40

#define EC_FEATURE      0x0766
#define  FEATURE_RGB    BIT(2)
#define EC_KBD_CTRL     0x0767
#define  KBD_APPLY      BIT(5)
#define EC_KBD_R        0x0769
#define EC_KBD_G        0x076A
#define EC_KBD_B        0x076B
#define EC_KBD_STATUS   0x078C
#define  KBD_LEVEL      GENMASK(7, 5)
#define KBD_CH_MAX      50
#define SW_MAX          10

#define SEL_WRITE       0x00000000
#define SEL_READ        0x00000100

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "load on machines not in the tested DMI list (at your own risk)");

/* Machines this driver has actually been tested on. */
static const struct dmi_system_id mkbd_dmi_table[] = {
	{
		.ident = "MONSTER TULPAR T7 V20.2 (Uniwill GK7MR0R)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MONSTER"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TULPAR T7 V20.2"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, mkbd_dmi_table);

static DEFINE_MUTEX(ec_lock);
static struct platform_device *mkbd_pdev;
static struct led_classdev mkbd_led;

/* protected by ec_lock */
static u8 base_rgb[3];		/* color at full brightness */
static unsigned int sw_level = SW_MAX;
static u8 hw_rgb[3];		/* last values written to / read from the EC */
static bool hw_valid;

static bool write_allowed(u16 addr)
{
	return addr == EC_KBD_CTRL || addr == EC_KBD_R ||
	       addr == EC_KBD_G || addr == EC_KBD_B;
}

/* caller holds ec_lock */
static int mkbd_wmbc(u16 addr, u8 data, u32 sel, u8 *out)
{
	u8 in[AC00_LEN] = { 0 };
	struct acpi_buffer input = { sizeof(in), in };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;

	lockdep_assert_held(&ec_lock);

	if (sel != SEL_READ && sel != SEL_WRITE)
		return -EINVAL;
	if (sel == SEL_WRITE && !write_allowed(addr))
		return -EPERM;

	in[0] = addr & 0xff;
	in[1] = addr >> 8;
	if (sel == SEL_WRITE)
		in[2] = data;
	in[4] = sel & 0xff;
	in[5] = (sel >> 8) & 0xff;
	in[6] = (sel >> 16) & 0xff;
	in[7] = sel >> 24;

	status = wmi_evaluate_method(AMW0_BC_GUID, 0, WMBC_METHOD_ID,
				     &input, &output);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length < 8)
		goto out;
	if (!memchr_inv(obj->buffer.pointer, 0xFE, 8)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (out)
		*out = obj->buffer.pointer[0];
	ret = 0;
out:
	kfree(output.pointer);
	return ret;
}

static int ec_rd(u16 addr, u8 *v)
{
	return mkbd_wmbc(addr, 0, SEL_READ, v);
}

static int ec_wr(u16 addr, u8 v)
{
	return mkbd_wmbc(addr, v, SEL_WRITE, NULL);
}

/* caller holds ec_lock: write base_rgb scaled by sw_level, then apply */
static int mkbd_push(void)
{
	static const u16 ch[3] = { EC_KBD_R, EC_KBD_G, EC_KBD_B };
	u8 ctrl;
	int i, ret;

	bool changed = false;

	/*
	 * Each WMI call costs ~60 ms (ACPI Sleep loop in WKBC/RKBC), so only
	 * write the channels that actually changed. Animations usually move
	 * one or two channels per step.
	 */
	for (i = 0; i < 3; i++) {
		u8 v = DIV_ROUND_CLOSEST(base_rgb[i] * sw_level, SW_MAX);

		if (hw_valid && hw_rgb[i] == v)
			continue;
		ret = ec_wr(ch[i], v);
		if (ret) {
			hw_valid = false;
			return ret;
		}
		hw_rgb[i] = v;
		changed = true;
	}
	if (!changed)
		return 0;
	ret = ec_rd(EC_KBD_CTRL, &ctrl);
	if (ret)
		return ret;
	/*
	 * The EC seems to latch the apply bit on a 0 -> 1 edge and clears it
	 * itself only after a long, variable time. If it is still set from the
	 * previous change, drop it first so the new color is applied.
	 */
	if (ctrl & KBD_APPLY) {
		ret = ec_wr(EC_KBD_CTRL, ctrl & ~KBD_APPLY);
		if (ret)
			return ret;
		msleep(50);
	}
	return ec_wr(EC_KBD_CTRL, (ctrl & ~KBD_APPLY) | KBD_APPLY);
}

/* ---- brightness (software) ---- */

static int mkbd_brightness_set(struct led_classdev *cdev,
			       enum led_brightness value)
{
	int ret;

	mutex_lock(&ec_lock);
	sw_level = min_t(unsigned int, value, SW_MAX);
	ret = mkbd_push();
	mutex_unlock(&ec_lock);
	return ret;
}

/* ---- color ---- */

static ssize_t color_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	ssize_t n;

	mutex_lock(&ec_lock);
	n = sysfs_emit(buf, "%u %u %u\n", base_rgb[0], base_rgb[1], base_rgb[2]);
	mutex_unlock(&ec_lock);
	return n;
}

static ssize_t color_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	unsigned int r, g, b;
	int ret;

	if (sscanf(buf, "%u %u %u", &r, &g, &b) != 3)
		return -EINVAL;
	if (r > KBD_CH_MAX || g > KBD_CH_MAX || b > KBD_CH_MAX)
		return -ERANGE;

	mutex_lock(&ec_lock);
	base_rgb[0] = r;
	base_rgb[1] = g;
	base_rgb[2] = b;
	ret = mkbd_push();
	mutex_unlock(&ec_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(color);

/* ---- hardware (Fn) level, read-only ---- */

static ssize_t hw_level_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	u8 st;
	int ret;

	mutex_lock(&ec_lock);
	ret = ec_rd(EC_KBD_STATUS, &st);
	mutex_unlock(&ec_lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%lu\n", FIELD_GET(KBD_LEVEL, st));
}
static DEVICE_ATTR_RO(hw_level);

/* debug, read-only: raw EC 0x767 and the R/G/B actually in the EC */
static ssize_t ec_state_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	u8 c, r, g, b;
	int ret;

	mutex_lock(&ec_lock);
	ret = ec_rd(EC_KBD_CTRL, &c);
	if (!ret)
		ret = ec_rd(EC_KBD_R, &r);
	if (!ret)
		ret = ec_rd(EC_KBD_G, &g);
	if (!ret)
		ret = ec_rd(EC_KBD_B, &b);
	mutex_unlock(&ec_lock);
	if (ret)
		return ret;

	return sysfs_emit(buf, "ctrl=0x%02x rgb=%u %u %u\n", c, r, g, b);
}
static DEVICE_ATTR_RO(ec_state);

static struct attribute *mkbd_attrs[] = {
	&dev_attr_color.attr,
	&dev_attr_hw_level.attr,
	&dev_attr_ec_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mkbd);

/* ---- module ---- */

static int __init monster_kbd_init(void)
{
	u8 feat;
	int ret;

	if (!dmi_check_system(mkbd_dmi_table)) {
		if (!force) {
			pr_info("monster_kbd: untested machine, not loading (use force=1 at your own risk)\n");
			return -ENODEV;
		}
		pr_warn("monster_kbd: force=1 on an untested machine\n");
	}

	if (!wmi_has_guid(AMW0_BC_GUID))
		return -ENODEV;

	mutex_lock(&ec_lock);
	ret = ec_rd(EC_FEATURE, &feat);
	/* current EC color becomes the base color, brightness = full */
	if (!ret)
		ret = ec_rd(EC_KBD_R, &base_rgb[0]);
	if (!ret)
		ret = ec_rd(EC_KBD_G, &base_rgb[1]);
	if (!ret)
		ret = ec_rd(EC_KBD_B, &base_rgb[2]);
	if (!ret) {
		memcpy(hw_rgb, base_rgb, sizeof(hw_rgb));
		hw_valid = true;
	}
	mutex_unlock(&ec_lock);
	if (ret)
		return ret;
	if (!(feat & FEATURE_RGB)) {
		pr_err("monster_kbd: EC 0x766=0x%02x, no RGB keyboard\n", feat);
		return -ENODEV;
	}

	mkbd_pdev = platform_device_register_simple("monster-kbd",
						    PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(mkbd_pdev))
		return PTR_ERR(mkbd_pdev);

	mkbd_led.name = "monster::kbd_backlight";
	mkbd_led.max_brightness = SW_MAX;
	mkbd_led.brightness = SW_MAX;
	mkbd_led.brightness_set_blocking = mkbd_brightness_set;
	mkbd_led.groups = mkbd_groups;
	/* keep the light as-is on rmmod instead of forcing it off */
	mkbd_led.flags = LED_RETAIN_AT_SHUTDOWN;

	ret = led_classdev_register(&mkbd_pdev->dev, &mkbd_led);
	if (ret) {
		platform_device_unregister(mkbd_pdev);
		return ret;
	}

	pr_info("monster_kbd: loaded, base color %u %u %u\n",
		base_rgb[0], base_rgb[1], base_rgb[2]);
	return 0;
}

static void __exit monster_kbd_exit(void)
{
	led_classdev_unregister(&mkbd_led);
	platform_device_unregister(mkbd_pdev);
}

module_init(monster_kbd_init);
module_exit(monster_kbd_exit);

MODULE_DESCRIPTION("Monster/Uniwill RGB keyboard backlight via AMW0 WMI");
MODULE_LICENSE("GPL");
