// SPDX-License-Identifier: GPL-2.0
/*
 * monster_kbd - RGB keyboard backlight for Monster TULPAR T7 V20.2
 * (Uniwill GK7MR0R) via WMI GUID ABBC0F6F-... (\_SB.AMW0.WMBC, method 4).
 *
 * Registered as a standard multicolor LED (led-class-multicolor), so any
 * tool that speaks the kernel LED API can drive it:
 *
 * /sys/class/leds/monster::kbd_backlight/
 *   brightness       0..255, software: scales the color written to the EC
 *   max_brightness   255
 *   multi_index      "red green blue"
 *   multi_intensity  "R G B", each 0..255 (standard multicolor attribute)
 *   color            "R G B", each 0..50 (EC native scale, same state as
 *                    multi_intensity; kept for monster-kbd-fx / the extension)
 *   hw_level         read-only, EC brightness level 0..4 set by the Fn keys
 *                    (EC 0x78C bits 7..5)
 *
 * EC channel = intensity/255 * brightness/255 * 50.
 * Effective light = that x the Fn level (EC, hardware).
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
#include <linux/led-class-multicolor.h>
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
#define MC_MAX          255

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
static struct mc_subled mkbd_subleds[3] = {
	{ .color_index = LED_COLOR_ID_RED,   .channel = 0 },
	{ .color_index = LED_COLOR_ID_GREEN, .channel = 1 },
	{ .color_index = LED_COLOR_ID_BLUE,  .channel = 2 },
};
static struct led_classdev_mc mkbd_mc = {
	.subled_info = mkbd_subleds,
	.num_colors = ARRAY_SIZE(mkbd_subleds),
};

/* protected by ec_lock */
static unsigned int intens[3];	/* 0..MC_MAX, color at full brightness */
static unsigned int sw_level = MC_MAX;
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

static u8 to_ec(unsigned int intensity, unsigned int level)
{
	return DIV_ROUND_CLOSEST(intensity * level * KBD_CH_MAX, MC_MAX * MC_MAX);
}

/* caller holds ec_lock: write intens scaled by sw_level, then apply */
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
		u8 v = to_ec(intens[i], sw_level);

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

/*
 * Called by the LED core for writes to both `brightness` and
 * `multi_intensity` (the core stores the new intensities in subled_info
 * first), so take the color from subled_info here.
 */
static int mkbd_brightness_set(struct led_classdev *cdev,
			       enum led_brightness value)
{
	int i, ret;

	mutex_lock(&ec_lock);
	sw_level = min_t(unsigned int, value, MC_MAX);
	for (i = 0; i < 3; i++)
		intens[i] = min_t(unsigned int, mkbd_subleds[i].intensity, MC_MAX);
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
	n = sysfs_emit(buf, "%u %u %u\n",
		       DIV_ROUND_CLOSEST(intens[0] * KBD_CH_MAX, MC_MAX),
		       DIV_ROUND_CLOSEST(intens[1] * KBD_CH_MAX, MC_MAX),
		       DIV_ROUND_CLOSEST(intens[2] * KBD_CH_MAX, MC_MAX));
	mutex_unlock(&ec_lock);
	return n;
}

static ssize_t color_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	unsigned int v[3];
	int i, ret;

	if (sscanf(buf, "%u %u %u", &v[0], &v[1], &v[2]) != 3)
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (v[i] > KBD_CH_MAX)
			return -ERANGE;

	/* led_access keeps us in sync with multi_intensity writes */
	mutex_lock(&cdev->led_access);
	mutex_lock(&ec_lock);
	for (i = 0; i < 3; i++) {
		intens[i] = DIV_ROUND_CLOSEST(v[i] * MC_MAX, KBD_CH_MAX);
		mkbd_subleds[i].intensity = intens[i];
	}
	ret = mkbd_push();
	mutex_unlock(&ec_lock);
	mutex_unlock(&cdev->led_access);

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
	struct led_classdev *cdev = &mkbd_mc.led_cdev;
	u8 feat, rgb[3];
	int i, ret;

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
		ret = ec_rd(EC_KBD_R, &rgb[0]);
	if (!ret)
		ret = ec_rd(EC_KBD_G, &rgb[1]);
	if (!ret)
		ret = ec_rd(EC_KBD_B, &rgb[2]);
	if (!ret) {
		for (i = 0; i < 3; i++) {
			rgb[i] = min_t(u8, rgb[i], KBD_CH_MAX);
			intens[i] = DIV_ROUND_CLOSEST(rgb[i] * MC_MAX, KBD_CH_MAX);
			mkbd_subleds[i].intensity = intens[i];
		}
		memcpy(hw_rgb, rgb, sizeof(hw_rgb));
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

	cdev->name = "monster::kbd_backlight";
	cdev->max_brightness = MC_MAX;
	cdev->brightness = MC_MAX;
	cdev->brightness_set_blocking = mkbd_brightness_set;
	/*
	 * Not cdev->groups: led_classdev_multicolor_register() replaces it with
	 * the multicolor group. Our attributes are added after registration.
	 */
	/* keep the light as-is on rmmod instead of forcing it off */
	cdev->flags = LED_RETAIN_AT_SHUTDOWN;

	ret = led_classdev_multicolor_register(&mkbd_pdev->dev, &mkbd_mc);
	if (ret) {
		platform_device_unregister(mkbd_pdev);
		return ret;
	}

	ret = sysfs_create_groups(&cdev->dev->kobj, mkbd_groups);
	if (ret) {
		led_classdev_multicolor_unregister(&mkbd_mc);
		platform_device_unregister(mkbd_pdev);
		return ret;
	}
	/* let udev apply permissions to the attributes added above */
	kobject_uevent(&cdev->dev->kobj, KOBJ_CHANGE);

	pr_info("monster_kbd: loaded as multicolor LED, color %u %u %u\n",
		rgb[0], rgb[1], rgb[2]);
	return 0;
}

static void __exit monster_kbd_exit(void)
{
	sysfs_remove_groups(&mkbd_mc.led_cdev.dev->kobj, mkbd_groups);
	led_classdev_multicolor_unregister(&mkbd_mc);
	platform_device_unregister(mkbd_pdev);
}

module_init(monster_kbd_init);
module_exit(monster_kbd_exit);

MODULE_DESCRIPTION("Monster/Uniwill RGB keyboard backlight via AMW0 WMI");
MODULE_LICENSE("GPL");
