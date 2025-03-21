// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/usb/ch9.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/ctype.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/redriver.h>
#include <linux/gpio/consumer.h>

#define MOTO_ALTMODE(fmt, ...) pr_debug("MMI_DETECT: REDRIVER[%s]: " fmt "\n", __func__, ##__VA_ARGS__)

/* priority: INT_MAX >= x >= 0 */
#define NOTIFIER_PRIORITY		1

/* Registers Address */
#define GEN_DEV_SET_REG			0x00
#define CHIP_VERSION_REG		0x17
#define AUX_ORIENTATION_REG		0x09

#define REDRIVER_REG_MAX		0x1f

#define EQ_SET_REG_BASE			0x01
#define FLAT_GAIN_REG_BASE		0x18
#define OUT_COMP_AND_POL_REG_BASE	0x02
#define LOSS_MATCH_REG_BASE		0x19

/* Default Register Value */
#define GEN_DEV_SET_REG_DEFAULT		0xFB

/* Register bits */
/* General Device Settings Register Bits */
#define CHIP_EN		BIT(0)
#define CHNA_EN		BIT(4)
#define CHNB_EN		BIT(5)
#define CHNC_EN		BIT(6)
#define CHND_EN		BIT(7)

#define CHANNEL_NUM		4

#define OP_MODE_SHIFT		1

#define EQ_SETTING_MASK			0x07
#define OUTPUT_COMPRESSION_MASK		0x0b
#define LOSS_MATCH_MASK			0x03
#define FLAT_GAIN_MASK			0x03

#define EQ_SETTING_SHIFT		0x01
#define OUTPUT_COMPRESSION_SHIFT	0x01
#define LOSS_MATCH_SHIFT		0x00
#define FLAT_GAIN_SHIFT			0x00

#define CHNA_INDEX		0
#define CHNB_INDEX		1
#define CHNC_INDEX		2
#define CHND_INDEX		3

enum operation_mode {
	OP_MODE_NONE,		/* 4 lanes disabled */
	OP_MODE_USB,		/* 2 lanes for USB and 2 lanes disabled */
	OP_MODE_DP,		/* 4 lanes DP */
	OP_MODE_USB_AND_DP,	/* 2 lanes for USB and 2 lanes DP */
	OP_MODE_DEFAULT,	/* 4 lanes USB */
};

#define CHAN_MODE_USB		0
#define CHAN_MODE_DP		1
#define CHAN_MODE_NUM		2

#define CHAN_MODE_DISABLE	0xff /* when disable, not configure eq, gain ... */

#define CHIP_MAX_PWR_UA		260000
#define CHIP_MIN_PWR_UV		1710000
#define CHIP_MAX_PWR_UV		1890000

struct ssusb_redriver {
	struct device		*dev;
	struct regmap		*regmap;
	struct i2c_client	*client;
	struct regulator	*vdd;

	int orientation_gpio;
	enum plug_orientation typec_orientation;
	enum operation_mode op_mode;

	struct notifier_block ucsi_nb;

	u8	chan_mode[CHANNEL_NUM];

	u8	eq[CHAN_MODE_NUM][CHANNEL_NUM];
	u8	output_comp[CHAN_MODE_NUM][CHANNEL_NUM];
	u8	loss_match[CHAN_MODE_NUM][CHANNEL_NUM];
	u8	flat_gain[CHAN_MODE_NUM][CHANNEL_NUM];

	u8	gen_dev_val;
	bool	lane_channel_swap;
	bool	vdd_enable;

	struct workqueue_struct *pullup_wq;
	struct work_struct	pullup_work;
	int			pullup_req;
	bool			work_ongoing;

	struct work_struct	host_work;

	struct dentry	*debug_root;
};

static int ssusb_redriver_channel_update(struct ssusb_redriver *redriver);
static void ssusb_redriver_debugfs_entries(struct ssusb_redriver *redriver);

static const char * const opmode_string[] = {
	[OP_MODE_NONE] = "NONE",
	[OP_MODE_USB] = "USB",
	[OP_MODE_DP] = "DP",
	[OP_MODE_USB_AND_DP] = "USB and DP",
	[OP_MODE_DEFAULT] = "DEFAULT",
};
#define OPMODESTR(x) opmode_string[x]

static int redriver_i2c_reg_set(struct ssusb_redriver *redriver,
		u8 reg, u8 val)
{
	int ret;

	ret = regmap_write(redriver->regmap, (unsigned int)reg,
			(unsigned int)val);
	if (ret < 0) {
		dev_err(redriver->dev, "writing reg 0x%02x failure\n", reg);
		return ret;
	}

	dev_dbg(redriver->dev, "writing reg 0x%02x=0x%02x\n", reg, val);

	return 0;
}

static void redriver_vdd_enable(struct ssusb_redriver *redriver, bool on)
{
	int l, v, s;

	if (!redriver->vdd || redriver->op_mode != OP_MODE_NONE) {
		dev_dbg(redriver->dev, "no vdd regulator operation\n");
		return;
	}

	if (on && !redriver->vdd_enable) {
		redriver->vdd_enable = true;
		l = regulator_set_load(redriver->vdd, CHIP_MAX_PWR_UA);
		v = regulator_set_voltage(redriver->vdd, CHIP_MIN_PWR_UV, CHIP_MAX_PWR_UV);
		s = regulator_enable(redriver->vdd);
		dev_dbg(redriver->dev, "vdd regulator enable return %d-%d-%d\n", l, v, s);
	} else if (!on && redriver->vdd_enable) {
		redriver->vdd_enable = false;
		s = regulator_disable(redriver->vdd);
		v = regulator_set_voltage(redriver->vdd, 0, CHIP_MAX_PWR_UV);
		l = regulator_set_load(redriver->vdd, 0);
		dev_dbg(redriver->dev, "vdd regulator disable return %d-%d-%d\n", l, v, s);
	}
}

static int ssusb_redriver_gen_dev_set(struct ssusb_redriver *redriver)
{
	u8 val = 0;

	switch (redriver->op_mode) {
	case OP_MODE_DEFAULT:
		/* Enable channel A, B, C and D */
		val |= (CHNA_EN | CHNB_EN);
		val |= (CHNC_EN | CHND_EN);
		val |= (0x5 << OP_MODE_SHIFT);
		val |= CHIP_EN;
		break;
	case OP_MODE_USB:
		/* Use source side I/O mapping */
		if (redriver->typec_orientation
				== ORIENTATION_CC1) {
			/* Enable channel C and D */
			val &= ~(CHNA_EN | CHNB_EN);
			val |= (CHNC_EN | CHND_EN);
		} else {
			/* Enable channel A and B*/
			val |= (CHNA_EN | CHNB_EN);
			val &= ~(CHNC_EN | CHND_EN);
		}
		dev_dbg(redriver->dev, "ssusb:: OpModeUSB-:CC %d\n",
				redriver->typec_orientation);
		/* Set to default USB Mode */
		val |= (0x5 << OP_MODE_SHIFT);
		val |= CHIP_EN;
		break;
	case OP_MODE_DP:
		/* Enable channel A, B, C and D */
		val |= (CHNA_EN | CHNB_EN);
		val |= (CHNC_EN | CHND_EN);

		/* Set to DP 4 Lane Mode (OP Mode 2) */
		val |= (0x2 << OP_MODE_SHIFT);
		val |= CHIP_EN;
		dev_dbg(redriver->dev, "ssusb:: OpModeDP- 4-LN: %d\n",
				redriver->typec_orientation);
		break;
	case OP_MODE_USB_AND_DP:
		/* Enable channel A, B, C and D */
		val |= (CHNA_EN | CHNB_EN);
		val |= (CHNC_EN | CHND_EN);
		val |= CHIP_EN;

		if (redriver->typec_orientation
				== ORIENTATION_CC1)
			val |= (0x1 << OP_MODE_SHIFT);
		/* it is mode 0 when ORIENTATION_CC2 */

		dev_dbg(redriver->dev, "ssusb:: OpModeUSB+DP: CC %d\n",
				redriver->typec_orientation);
		break;
	default:
		val &= ~CHIP_EN;
		break;
	}

	redriver->gen_dev_val = val;

	return redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, val);
}

static int ssusb_redriver_param_config(struct ssusb_redriver *redriver,
		u8 reg_base, u8 channel, u8 chan_mode, u8 mask, u8 shift, u8 val,
		u8 (*stored_val)[CHANNEL_NUM])
{
	int i, j, ret = -EINVAL;
	u8 reg_addr, reg_val;

	if (channel == CHANNEL_NUM) {
		for (i = 0; i < CHAN_MODE_NUM; i++)
			for (j = 0; j < CHANNEL_NUM; j++) {
				if (redriver->chan_mode[j] == i) {
					reg_addr = reg_base + (j << 1);

					reg_val =  (val  << shift);
					reg_val &= (mask << shift);

					ret = redriver_i2c_reg_set(redriver,
							reg_addr, reg_val);
					if (ret < 0)
						return ret;
				}

				stored_val[i][j] = val;
			}
	} else {
		if (redriver->chan_mode[channel] == chan_mode) {
			reg_addr = reg_base + (channel << 1);

			reg_val =  (val  << shift);
			reg_val &= (mask << shift);

			ret = redriver_i2c_reg_set(redriver,
					reg_addr, reg_val);
			if (ret < 0)
				return ret;
		}

		stored_val[chan_mode][channel] = val;
	}

	return 0;
}

static int ssusb_redriver_eq_config(
	struct ssusb_redriver *redriver, u8 channel, u8 chan_mode, u8 val)
{
	return ssusb_redriver_param_config(redriver,
			EQ_SET_REG_BASE, channel, chan_mode,
			EQ_SETTING_MASK, EQ_SETTING_SHIFT,
			val, redriver->eq);
}

static int ssusb_redriver_flat_gain_config(
	struct ssusb_redriver *redriver, u8 channel, u8 chan_mode, u8 val)
{
	return ssusb_redriver_param_config(redriver,
			FLAT_GAIN_REG_BASE, channel, chan_mode,
			FLAT_GAIN_MASK, FLAT_GAIN_SHIFT,
			val, redriver->flat_gain);
}

static int ssusb_redriver_output_comp_config(
	struct ssusb_redriver *redriver, u8 channel, u8 chan_mode, u8 val)
{
	return ssusb_redriver_param_config(redriver,
			OUT_COMP_AND_POL_REG_BASE, channel, chan_mode,
			OUTPUT_COMPRESSION_MASK, OUTPUT_COMPRESSION_SHIFT,
			val, redriver->output_comp);
}

static int ssusb_redriver_loss_match_config(
	struct ssusb_redriver *redriver, u8 channel, u8 chan_mode, u8 val)
{
	return ssusb_redriver_param_config(redriver,
			LOSS_MATCH_REG_BASE, channel, chan_mode,
			LOSS_MATCH_MASK, LOSS_MATCH_SHIFT, val,
			redriver->loss_match);
}

static int ssusb_redriver_read_orientation(struct ssusb_redriver *redriver)
{
	int ret;

	if (!gpio_is_valid(redriver->orientation_gpio))
		return -EINVAL;

	ret = gpio_get_value(redriver->orientation_gpio);
	if (ret < 0) {
		dev_err(redriver->dev, "ssusb::fail to read cc-gpio[%d] retVal: %d\n",
				redriver->orientation_gpio, ret);
		return -EINVAL;
	}

	/*
	 * Support some board layouts in which the channels are reversed.
	 * i.e. channels C&D are used for the USB CC1 orientation and
	 * channels A&B are used for USB CC2
	 */
	if (redriver->lane_channel_swap)
		ret = !ret;

	if (ret == 0)
		redriver->typec_orientation = ORIENTATION_CC1;
	else
		redriver->typec_orientation = ORIENTATION_CC2;

	dev_dbg(redriver->dev, "ssusb::typec_orient: %d, lane_swap: %d\n",
				redriver->typec_orientation, redriver->lane_channel_swap);

	return 0;
}
static int ssusb_redriver_channel_update(struct ssusb_redriver *redriver)
{
	int ret;
	u8 i, chan_mode;

	/* CC orientation is updated fusb302 */
	ssusb_redriver_read_orientation(redriver);

	switch (redriver->op_mode) {
	case OP_MODE_DEFAULT:
		redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_USB;
		redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_USB;
		redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_USB;
		redriver->chan_mode[CHND_INDEX] = CHAN_MODE_USB;
		break;
	case OP_MODE_USB:
		if (redriver->typec_orientation == ORIENTATION_CC1) {
			redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_DISABLE;
			redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_DISABLE;
			redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHND_INDEX] = CHAN_MODE_USB;
		} else {
			redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_DISABLE;
			redriver->chan_mode[CHND_INDEX] = CHAN_MODE_DISABLE;
		}
		dev_dbg(redriver->dev, "ssusb:: OpModeUSB:CC %d\n",
				redriver->typec_orientation);
		break;
	case OP_MODE_USB_AND_DP:
		if (redriver->typec_orientation == ORIENTATION_CC1) {
			redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_DP;
			redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_DP;
			redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHND_INDEX] = CHAN_MODE_USB;
		} else {
			redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_USB;
			redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_DP;
			redriver->chan_mode[CHND_INDEX] = CHAN_MODE_DP;
		}
		dev_dbg(redriver->dev, "ssusb:: OpModeUSB+DP:CC %d\n",
				redriver->typec_orientation);

		break;
	case OP_MODE_DP:
		redriver->chan_mode[CHNA_INDEX] = CHAN_MODE_DP;
		redriver->chan_mode[CHNB_INDEX] = CHAN_MODE_DP;
		redriver->chan_mode[CHNC_INDEX] = CHAN_MODE_DP;
		redriver->chan_mode[CHND_INDEX] = CHAN_MODE_DP;
		dev_dbg(redriver->dev, "ssusb:: OpModeDP: CC%d\n",
					redriver->typec_orientation);
		break;
	default:
		return 0;
	}

	for (i = 0; i < CHANNEL_NUM; i++) {
		if (redriver->chan_mode[i] == CHAN_MODE_DISABLE)
			continue;

		chan_mode = redriver->chan_mode[i];

		ret = ssusb_redriver_eq_config(redriver, i, chan_mode,
				redriver->eq[chan_mode][i]);
		if (ret)
			goto err;

		ret = ssusb_redriver_flat_gain_config(redriver, i, chan_mode,
				redriver->flat_gain[chan_mode][i]);
		if (ret)
			goto err;

		ret = ssusb_redriver_output_comp_config(redriver, i, chan_mode,
				redriver->output_comp[chan_mode][i]);
		if (ret)
			goto err;

		ret = ssusb_redriver_loss_match_config(redriver, i, chan_mode,
				redriver->loss_match[chan_mode][i]);
		if (ret)
			goto err;
	}

	return 0;

err:
	dev_err(redriver->dev, "channel parameters update failure(%d).\n", ret);
	return ret;
}

static int ssusb_redriver_read_configuration(struct ssusb_redriver *redriver)
{
	struct device_node *node = redriver->dev->of_node;
	int ret = 0;

	if (of_find_property(node, "eq", NULL)) {
		ret = of_property_read_u8_array(node, "eq",
				redriver->eq[0], sizeof(redriver->eq));
		if (ret)
			goto err;
	}

	if (of_find_property(node, "flat-gain", NULL)) {
		ret = of_property_read_u8_array(node,
				"flat-gain", redriver->flat_gain[0],
				sizeof(redriver->flat_gain));
		if (ret)
			goto err;
	}

	if (of_find_property(node, "output-comp", NULL)) {
		ret = of_property_read_u8_array(node,
				"output-comp", redriver->output_comp[0],
				sizeof(redriver->output_comp));
		if (ret)
			goto err;
	}

	if (of_find_property(node, "loss-match", NULL)) {
		ret = of_property_read_u8_array(node,
				"loss-match", redriver->loss_match[0],
				sizeof(redriver->loss_match));
		if (ret)
			goto err;
	}

	return 0;

err:
	dev_err(redriver->dev,
			"%s: error read parameters.\n", __func__);
	return ret;
}

static inline void *check_devnode(struct device_node *node)
{
	struct i2c_client *client;

	if (!node)
		return ERR_PTR(-ENODEV);

	client = of_find_i2c_device_by_node(node);
	if (!client)
		return ERR_PTR(-ENODEV);

	return i2c_get_clientdata(client);
}

int redriver_orientation_get(struct device_node *node)
{
	struct ssusb_redriver *redriver;
	int orient_val;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	if (!gpio_is_valid(redriver->orientation_gpio)) {
		dev_err(redriver->dev, "%s: invalid gpio: %d\n",
			__func__, redriver->orientation_gpio);
		return -EINVAL;
	}
	orient_val = gpio_get_value(redriver->orientation_gpio);
	dev_dbg(redriver->dev, "%s: %d\n", __func__, orient_val);

	return orient_val;
}
EXPORT_SYMBOL(redriver_orientation_get);

#if IS_ENABLED(CONFIG_TYPEC_QTI_ALTMODE)
#include <linux/usb/typec_dp.h>

static int redriver_i2c_reg_get(struct ssusb_redriver *redriver,
		u8 reg, u8 *val)
{
	int ret;

	ret = regmap_read(redriver->regmap, (unsigned int)reg,
			(unsigned int *)val);
	if (ret < 0) {
		dev_err(redriver->dev, "reading reg 0x%02x failure\n", reg);
		return ret;
	}

	dev_dbg(redriver->dev, "reading reg 0x%02x=0x%02x\n", reg, *val);

	return 0;
}

static int ssusb_redriver_mux_set(void *priv, u8 state)
{
	struct ssusb_redriver *redriver = priv;
	int ret;
	u8 val;

	switch (state) {
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		redriver->op_mode = OP_MODE_DP;
			break;
	case TYPEC_DP_STATE_D:
		redriver->op_mode = OP_MODE_USB_AND_DP;
			break;
	case TYPEC_STATE_USB:
		redriver->op_mode = OP_MODE_USB;
			break;
	default:
		redriver->op_mode = OP_MODE_NONE;
		MOTO_ALTMODE("DP state %d", state);
	}

	ret = redriver_i2c_reg_get(redriver, AUX_ORIENTATION_REG, &val);
	if (ret)
		dev_err(redriver->dev, "AUX orientation read failed; rc=%d", ret);

	/* AUX connected, normal */
	val &= ~0x03;
	if (redriver->op_mode == OP_MODE_DP ||
		redriver->op_mode == OP_MODE_USB_AND_DP) {
		/* Set AUX lines to flipped state */
		if (redriver->typec_orientation == ORIENTATION_CC2)
			val |= 0x01;
	} else {
		/* Isolate AUX lines for non-DP modes */
		val |= 0x02;
	}
	MOTO_ALTMODE("AUX orientation reg(0x%02x)=0x%02x", AUX_ORIENTATION_REG, val);

	ret = redriver_i2c_reg_set(redriver, AUX_ORIENTATION_REG, val);
	if (ret)
		dev_err(redriver->dev, "AUX orientation write failed; rc=%d", ret);
	ssusb_redriver_channel_update(redriver);

	return ssusb_redriver_gen_dev_set(redriver);
}

extern int register_dp_altmode_mux_control(void *priv, int (*set)(void *, u8));

/* stubs */
static int ssusb_redriver_ucsi_notifier(struct notifier_block *nb,
		unsigned long action, void *data) { return -ENOTSUPP; }
static int register_ucsi_glink_notifier(struct notifier_block *nb) { return -ENOTSUPP; }
static void unregister_ucsi_glink_notifier(struct notifier_block *nb) { }
#else
#include <linux/usb/ucsi_glink.h>

static int ssusb_redriver_ucsi_notifier(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct ssusb_redriver *redriver =
			container_of(nb, struct ssusb_redriver, ucsi_nb);
	struct ucsi_glink_constat_info *info = data;
	enum operation_mode op_mode;
	int ret;

	if (info->connect && !info->partner_change)
		return NOTIFY_DONE;

	if (!info->connect) {
		if (info->partner_usb || info->partner_alternate_mode)
			dev_err(redriver->dev, "set partner when no connection\n");
		op_mode = OP_MODE_NONE;
	} else if (info->partner_usb && info->partner_alternate_mode) {
		/*
		 * when connect a DP only cable,
		 * ucsi set usb flag first, then set usb and alternate mode
		 * after dp start link training.
		 * it should only set alternate_mode flag ???
		 */
		if (redriver->op_mode == OP_MODE_DP)
			return NOTIFY_OK;
		op_mode = OP_MODE_USB_AND_DP;
	} else if (info->partner_usb) {
		if (redriver->op_mode == OP_MODE_DP)
			return NOTIFY_OK;
		op_mode = OP_MODE_USB;
	} else if (info->partner_alternate_mode) {
		op_mode = OP_MODE_DP;
	} else
		op_mode = OP_MODE_NONE;

	if (redriver->op_mode == op_mode)
		return NOTIFY_OK;

	/*
	 * if regulator was turn off during disocnnect, when connect and turn on regulator,
	 * it will enter default 4 lanes USB mode which is different from behavior that
	 * regulator always on.
	 */
	redriver_vdd_enable(redriver, true);
	dev_dbg(redriver->dev, "op mode %s -> %s\n",
		OPMODESTR(redriver->op_mode), OPMODESTR(op_mode));
	redriver->op_mode = op_mode;

	if (redriver->op_mode == OP_MODE_USB ||
			redriver->op_mode == OP_MODE_USB_AND_DP) {
		ssusb_redriver_read_orientation(redriver);

		dev_dbg(redriver->dev, "orientation %s\n",
			redriver->typec_orientation == ORIENTATION_CC1 ?
			"CC1" : "CC2");
	}

	ret = ssusb_redriver_channel_update(redriver);
	if (ret) {
		dev_dbg(redriver->dev, "i2c bus may not resume(%d)\n", ret);
		return NOTIFY_DONE;
	}
	ssusb_redriver_gen_dev_set(redriver);

	redriver_vdd_enable(redriver, false);

	return NOTIFY_OK;
}

/* stubs */
static int ssusb_redriver_mux_set(void *priv, u8 orient) { return -ENOTSUPP; }
static int register_dp_altmode_mux_control(void *priv, int (*set)(void *, u8)) { return -ENOTSUPP; }
#endif

int redriver_notify_connect(struct device_node *node, enum plug_orientation orientation)
{
	struct ssusb_redriver *redriver;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	if ((redriver->op_mode == OP_MODE_DEFAULT) ||
	    (redriver->op_mode == OP_MODE_DP))
		return 0;

	dev_dbg(redriver->dev, "orientation %d\n", orientation);

	if (orientation != ORIENTATION_UNKNOWN) {
		if (redriver->lane_channel_swap) {
			redriver->typec_orientation =
				orientation == ORIENTATION_CC1 ? ORIENTATION_CC2 : ORIENTATION_CC1;
		} else {
			redriver->typec_orientation = orientation;
		}
	} else if (redriver->op_mode == OP_MODE_NONE) {
		ssusb_redriver_read_orientation(redriver);
	}

	redriver_vdd_enable(redriver, true);

	if (redriver->op_mode == OP_MODE_NONE)
		redriver->op_mode = OP_MODE_USB;

	dev_dbg(redriver->dev, "connect op mode %s\n",
		OPMODESTR(redriver->op_mode));

	ssusb_redriver_channel_update(redriver);
	ssusb_redriver_gen_dev_set(redriver);

	return 0;
}
EXPORT_SYMBOL(redriver_notify_connect);

int redriver_notify_disconnect(struct device_node *node)
{
	struct ssusb_redriver *redriver;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	/* 1. no operation in recovery mode.
	 * 2. there is case for 4 lane display, first report usb mode,
	 * second call usb release super speed lanes,
	 * then stop usb host and call this disconnect,
	 * it should not disable chip.
	 * 3. if already disabled, no need to disable again.
	 */
	if ((redriver->op_mode == OP_MODE_DEFAULT) ||
	    (redriver->op_mode == OP_MODE_DP) ||
	    (redriver->op_mode == OP_MODE_NONE))
		return 0;

	dev_dbg(redriver->dev, "disconnect op mode %s\n",
		OPMODESTR(redriver->op_mode));

	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, 0);

	redriver_vdd_enable(redriver, false);

	return ret;
}
EXPORT_SYMBOL(redriver_notify_disconnect);

int redriver_release_usb_lanes(struct device_node *node)
{
	struct ssusb_redriver *redriver;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	if (redriver->op_mode == OP_MODE_DP)
		return 0;

	redriver_vdd_enable(redriver, true);

	MOTO_ALTMODE("display notify 4 lane mode");
	redriver->op_mode = OP_MODE_DP;

	ssusb_redriver_channel_update(redriver);
	ssusb_redriver_gen_dev_set(redriver);

	return 0;
}
EXPORT_SYMBOL(redriver_release_usb_lanes);

static void redriver_gadget_pullup_work(struct work_struct *w)
{
	struct ssusb_redriver *redriver =
			container_of(w, struct ssusb_redriver, pullup_work);
	u8 val = redriver->gen_dev_val;

	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, val & ~CHIP_EN);
	usleep_range(1000, 1500);
	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, val);

	redriver->work_ongoing = false;
}

int redriver_gadget_pullup_enter(struct device_node *node, int is_on)
{
	struct ssusb_redriver *redriver;
	u64 time = 0;

	if (!is_on)
		return 0;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	if (redriver->op_mode != OP_MODE_USB &&
	    redriver->op_mode != OP_MODE_DEFAULT)
		return 0;

	while (redriver->work_ongoing) {
		udelay(1);
		if (time++ > 500000) {
			dev_warn(redriver->dev, "pullup timeout\n");
			break;
		}
	}

	dev_dbg(redriver->dev, "pull-up disable work took %llu us\n", time);

	return 0;
}
EXPORT_SYMBOL(redriver_gadget_pullup_enter);

int redriver_gadget_pullup_exit(struct device_node *node, int is_on)
{
	struct ssusb_redriver *redriver;

	if (is_on)
		return 0;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	redriver->pullup_req = is_on;

	if (redriver->op_mode != OP_MODE_USB &&
	    redriver->op_mode != OP_MODE_DEFAULT)
		return 0;

	redriver->work_ongoing = true;
	queue_work(redriver->pullup_wq, &redriver->pullup_work);

	return 0;
}
EXPORT_SYMBOL(redriver_gadget_pullup_exit);

static void redriver_host_work(struct work_struct *w)
{
	struct ssusb_redriver *redriver =
			container_of(w, struct ssusb_redriver, host_work);
	u8 val = redriver->gen_dev_val;

	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, val & ~CHIP_EN);
	usleep_range(2000, 2500);
	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG, val);
}

int redriver_powercycle(struct device_node *node)
{
	struct ssusb_redriver *redriver;

	redriver = check_devnode(node);
	if (IS_ERR_OR_NULL(redriver))
		return -EINVAL;

	if (redriver->op_mode != OP_MODE_USB)
		return -EINVAL;

	schedule_work(&redriver->host_work);

	return 0;
}
EXPORT_SYMBOL(redriver_powercycle);

static void ssusb_redriver_orientation_gpio_init(
		struct ssusb_redriver *redriver)
{
	struct device *dev = redriver->dev;
	int orient_state;

	redriver->orientation_gpio = of_get_named_gpio(redriver->dev->of_node,
			"redriver,orientation_gpio", 0);

	orient_state = gpio_get_value(redriver->orientation_gpio);
	dev_err(dev, "ssusb::INIT CC-Orientn gpio:[%d] RD val: [%d]\n",
			redriver->orientation_gpio, orient_state);

}

static const struct regmap_config redriver_regmap = {
	.max_register = REDRIVER_REG_MAX,
	.reg_bits = 8,
	.val_bits = 8,
};

static int redriver_i2c_probe(struct i2c_client *client,
			       const struct i2c_device_id *dev_id)
{
	struct ssusb_redriver *redriver;
	int ret;

	redriver = devm_kzalloc(&client->dev, sizeof(struct ssusb_redriver),
			GFP_KERNEL);
	if (!redriver)
		return -ENOMEM;

	redriver->pullup_wq = alloc_workqueue("%s:pullup",
				WQ_UNBOUND | WQ_HIGHPRI, 0,
				dev_name(&client->dev));
	if (!redriver->pullup_wq) {
		dev_err(&client->dev, "Failed to create pullup workqueue\n");
		return -ENOMEM;
	}

	redriver->regmap = devm_regmap_init_i2c(client, &redriver_regmap);
	if (IS_ERR(redriver->regmap)) {
		ret = PTR_ERR(redriver->regmap);
		dev_err(&client->dev,
			"Failed to allocate register map: %d\n", ret);
		return ret;
	}

	redriver->dev = &client->dev;
	i2c_set_clientdata(client, redriver);

	ret = ssusb_redriver_read_configuration(redriver);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to read default configuration: %d\n", ret);
		return ret;
	}

	redriver->vdd = devm_regulator_get_optional(&client->dev, "vdd");
	if (IS_ERR(redriver->vdd)) {
		ret = PTR_ERR(redriver->vdd);
		redriver->vdd = NULL;
		if (ret != -ENODEV)
			dev_err(&client->dev, "Failed to get vdd regulator %d\n", ret);
	}

	INIT_WORK(&redriver->pullup_work, redriver_gadget_pullup_work);
	INIT_WORK(&redriver->host_work, redriver_host_work);

	redriver->lane_channel_swap =
	    of_property_read_bool(redriver->dev->of_node, "lane-channel-swap");

	redriver->op_mode = OP_MODE_NONE;
	redriver_vdd_enable(redriver, true);
	ssusb_redriver_gen_dev_set(redriver);
	/* when private vdd present and change to none mode, it can simply disable vdd regulator,
	 * but to keep things simple and avoid if/else operation, keep one same rule as,
	 * allow original register write operation then control vdd regulator.
	 * also it will keep consistent behavior if it still need vdd control when multiple
	 * clients share the same vdd regulator.
	 */
	redriver_vdd_enable(redriver, false);

	ssusb_redriver_orientation_gpio_init(redriver);

	redriver->ucsi_nb.notifier_call = ssusb_redriver_ucsi_notifier;
	register_ucsi_glink_notifier(&redriver->ucsi_nb);

	MOTO_ALTMODE("registering mux control");
	register_dp_altmode_mux_control(redriver, ssusb_redriver_mux_set);

	ssusb_redriver_debugfs_entries(redriver);

	return 0;
}

static int redriver_i2c_remove(struct i2c_client *client)
{
	struct ssusb_redriver *redriver = i2c_get_clientdata(client);

	debugfs_remove(redriver->debug_root);
	unregister_ucsi_glink_notifier(&redriver->ucsi_nb);
	redriver->work_ongoing = false;
	destroy_workqueue(redriver->pullup_wq);

	if (redriver->vdd)
		regulator_disable(redriver->vdd);

	return 0;
}

static ssize_t channel_config_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos,
		int (*config_func)(struct ssusb_redriver *redriver,
			u8 channel, u8 chan_mode, u8 val))
{
	struct seq_file *s = file->private_data;
	struct ssusb_redriver *redriver = s->private;
	char buf[40];
	char *token_chan, *token_val, *this_buf;
	u8 channel, chan_mode;
	int ret = 0;

	memset(buf, 0, sizeof(buf));

	this_buf = buf;

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (isdigit(buf[0])) {
		ret = config_func(redriver, CHANNEL_NUM, -1, buf[0] - '0');
		if (ret < 0)
			goto err;
	} else if (isalpha(buf[0])) {
		while ((token_chan = strsep(&this_buf, " ")) != NULL) {
			switch (*token_chan) {
			case 'A':
			case 'B':
			case 'C':
			case 'D':
				channel = *token_chan - 'A';
				chan_mode = CHAN_MODE_USB;
				token_val = strsep(&this_buf, " ");
				if (!isdigit(*token_val))
					goto err;
				break;
			case 'a':
			case 'b':
			case 'c':
			case 'd':
				channel = *token_chan - 'a';
				chan_mode = CHAN_MODE_DP;
				token_val = strsep(&this_buf, " ");
				if (!isdigit(*token_val))
					goto err;
				break;
			default:
				goto err;
			}

			ret = config_func(redriver, channel, chan_mode,
					*token_val - '0');
			if (ret < 0)
				goto err;
		}
	} else
		goto err;


	return count;

err:
	pr_err("Used to config redriver A/B/C/D channels' parameters\n"
		"A/B/C/D represent for re-driver parameters for USB\n"
		"a/b/c/d represent for re-driver parameters for DP\n"
		"1. Set all channels to same value(both USB and DP)\n"
		"echo n > [eq|output_comp|flat_gain|loss_match]\n"
		"- eq: Equalization, range 0-7\n"
		"- output_comp: Output Compression, range 0-3\n"
		"- loss_match: LOSS Profile Matching, range 0-3\n"
		"- flat_gain: Flat Gain, range 0-3\n"
		"Example: Set all channels to same EQ value\n"
		"echo 1 > eq\n"
		"2. Set two channels to different values leave others unchanged\n"
		"echo [A|B|C|D] n [A|B|C|D] n > [eq|output_comp|flat_gain|loss_match]\n"
		"Example2: USB mode: set channel B flat gain to 2, set channel C flat gain to 3\n"
		"echo B 2 C 3 > flat_gain\n"
		"Example3: DP mode: set channel A equalization to 6, set channel B equalization to 4\n"
		"echo a 6 b 4 > eq\n");

	return -EFAULT;
}

static int eq_status(struct seq_file *s, void *p)
{
	struct ssusb_redriver *redriver = s->private;

	seq_puts(s, "\t\t\t A(USB)\t B(USB)\t C(USB)\t D(USB)\t"
			"A(DP)\t B(DP)\t C(DP)\t D(DP)\n");
	seq_printf(s, "Equalization:\t\t %d\t %d\t %d\t %d\t"
			"%d\t %d\t %d\t %d\n",
			redriver->eq[CHAN_MODE_USB][CHNA_INDEX],
			redriver->eq[CHAN_MODE_USB][CHNB_INDEX],
			redriver->eq[CHAN_MODE_USB][CHNC_INDEX],
			redriver->eq[CHAN_MODE_USB][CHND_INDEX],
			redriver->eq[CHAN_MODE_DP][CHNA_INDEX],
			redriver->eq[CHAN_MODE_DP][CHNB_INDEX],
			redriver->eq[CHAN_MODE_DP][CHNC_INDEX],
			redriver->eq[CHAN_MODE_DP][CHND_INDEX]);
	return 0;
}

static int eq_status_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, eq_status, inode->i_private);
}

static ssize_t eq_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	return channel_config_write(file, ubuf, count, ppos,
			ssusb_redriver_eq_config);
}

static const struct file_operations eq_ops = {
	.open	= eq_status_open,
	.read	= seq_read,
	.write	= eq_write,
};

static int flat_gain_status(struct seq_file *s, void *p)
{
	struct ssusb_redriver *redriver = s->private;

	seq_puts(s, "\t\t\t A(USB)\t B(USB)\t C(USB)\t D(USB)\t"
			"A(DP)\t B(DP)\t C(DP)\t D(DP)\n");
	seq_printf(s, "TX/RX Flat Gain:\t %d\t %d\t %d\t %d\t"
			"%d\t %d\t %d\t %d\n",
			redriver->flat_gain[CHAN_MODE_USB][CHNA_INDEX],
			redriver->flat_gain[CHAN_MODE_USB][CHNB_INDEX],
			redriver->flat_gain[CHAN_MODE_USB][CHNC_INDEX],
			redriver->flat_gain[CHAN_MODE_USB][CHND_INDEX],
			redriver->flat_gain[CHAN_MODE_DP][CHNA_INDEX],
			redriver->flat_gain[CHAN_MODE_DP][CHNB_INDEX],
			redriver->flat_gain[CHAN_MODE_DP][CHNC_INDEX],
			redriver->flat_gain[CHAN_MODE_DP][CHND_INDEX]);
	return 0;
}

static int flat_gain_status_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, flat_gain_status, inode->i_private);
}

static ssize_t flat_gain_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	return channel_config_write(file, ubuf, count, ppos,
			ssusb_redriver_flat_gain_config);
}

static const struct file_operations flat_gain_ops = {
	.open	= flat_gain_status_open,
	.read	= seq_read,
	.write	= flat_gain_write,
};

static int output_comp_status(struct seq_file *s, void *p)
{
	struct ssusb_redriver *redriver = s->private;

	seq_puts(s, "\t\t\t A(USB)\t B(USB)\t C(USB)\t D(USB)\t"
			"A(DP)\t B(DP)\t C(DP)\t D(DP)\n");
	seq_printf(s, "Output Compression:\t %d\t %d\t %d\t %d\t"
			"%d\t %d\t %d\t %d\n",
			redriver->output_comp[CHAN_MODE_USB][CHNA_INDEX],
			redriver->output_comp[CHAN_MODE_USB][CHNB_INDEX],
			redriver->output_comp[CHAN_MODE_USB][CHNC_INDEX],
			redriver->output_comp[CHAN_MODE_USB][CHND_INDEX],
			redriver->output_comp[CHAN_MODE_DP][CHNA_INDEX],
			redriver->output_comp[CHAN_MODE_DP][CHNB_INDEX],
			redriver->output_comp[CHAN_MODE_DP][CHNC_INDEX],
			redriver->output_comp[CHAN_MODE_DP][CHND_INDEX]);
	return 0;
}

static int output_comp_status_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, output_comp_status, inode->i_private);
}

static ssize_t output_comp_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	return channel_config_write(file, ubuf, count, ppos,
			ssusb_redriver_output_comp_config);
}

static const struct file_operations output_comp_ops = {
	.open	= output_comp_status_open,
	.read	= seq_read,
	.write	= output_comp_write,
};

static int loss_match_status(struct seq_file *s, void *p)
{
	struct ssusb_redriver *redriver = s->private;

	seq_puts(s, "\t\t\t A(USB)\t B(USB)\t C(USB)\t D(USB)\t"
			"A(DP)\t B(DP)\t C(DP)\t D(DP)\n");
	seq_printf(s, "Loss Profile Match:\t %d\t %d\t %d\t %d\t"
			"%d\t %d\t %d\t %d\n",
			redriver->loss_match[CHAN_MODE_USB][CHNA_INDEX],
			redriver->loss_match[CHAN_MODE_USB][CHNB_INDEX],
			redriver->loss_match[CHAN_MODE_USB][CHNC_INDEX],
			redriver->loss_match[CHAN_MODE_USB][CHND_INDEX],
			redriver->loss_match[CHAN_MODE_DP][CHNA_INDEX],
			redriver->loss_match[CHAN_MODE_DP][CHNB_INDEX],
			redriver->loss_match[CHAN_MODE_DP][CHNC_INDEX],
			redriver->loss_match[CHAN_MODE_DP][CHND_INDEX]);
	return 0;
}

static int loss_match_status_open(struct inode *inode,
		struct file *file)
{
	return single_open(file, loss_match_status, inode->i_private);
}

static ssize_t loss_match_write(struct file *file,
		const char __user *ubuf, size_t count, loff_t *ppos)
{
	return channel_config_write(file, ubuf, count, ppos,
			ssusb_redriver_loss_match_config);
}

static const struct file_operations loss_match_ops = {
	.open	= loss_match_status_open,
	.read	= seq_read,
	.write	= loss_match_write,
};

static void ssusb_redriver_debugfs_entries(
		struct ssusb_redriver *redriver)
{
	struct dentry *ent;

	redriver->debug_root = debugfs_create_dir("ssusb_redriver", NULL);
	if (!redriver->debug_root) {
		dev_warn(redriver->dev, "Couldn't create debug dir\n");
		return;
	}

	ent = debugfs_create_file("eq", 0600,
			redriver->debug_root, redriver, &eq_ops);
	if (IS_ERR_OR_NULL(ent))
		dev_warn(redriver->dev, "Couldn't create eq file\n");

	ent = debugfs_create_file("flat_gain", 0600,
			redriver->debug_root, redriver, &flat_gain_ops);
	if (IS_ERR_OR_NULL(ent))
		dev_warn(redriver->dev, "Couldn't create flat_gain file\n");

	ent = debugfs_create_file("output_comp", 0600,
			redriver->debug_root, redriver, &output_comp_ops);
	if (IS_ERR_OR_NULL(ent))
		dev_warn(redriver->dev, "Couldn't create output_comp file\n");

	ent = debugfs_create_file("loss_match", 0600,
			redriver->debug_root, redriver, &loss_match_ops);
	if (IS_ERR_OR_NULL(ent))
		dev_warn(redriver->dev, "Couldn't create loss_match file\n");
}

static int __maybe_unused redriver_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ssusb_redriver *redriver = i2c_get_clientdata(client);

	dev_dbg(redriver->dev, "%s: SS USB redriver suspend.\n",
			__func__);

	/*
	 * 1. when in 4 lanes display mode, it can't disable;
	 * 2. when in NONE mode, there is no need to re-disable;
	 * 3. when in DEFAULT mode, there is no adsp and can't disable;
	 */
	if (redriver->op_mode == OP_MODE_DP ||
	    redriver->op_mode == OP_MODE_NONE ||
	    redriver->op_mode == OP_MODE_DEFAULT)
		return 0;

	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG,
				redriver->gen_dev_val & ~CHIP_EN);

	return 0;
}

static int __maybe_unused redriver_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ssusb_redriver *redriver = i2c_get_clientdata(client);

	dev_dbg(redriver->dev, "%s: SS USB redriver resume.\n",
			__func__);

	/* no suspend happen in following mode */
	if (redriver->op_mode == OP_MODE_DP ||
	    redriver->op_mode == OP_MODE_NONE ||
	    redriver->op_mode == OP_MODE_DEFAULT)
		return 0;

	redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG,
				redriver->gen_dev_val);

	return 0;
}

static SIMPLE_DEV_PM_OPS(redriver_i2c_pm, redriver_i2c_suspend,
			 redriver_i2c_resume);

static void redriver_i2c_shutdown(struct i2c_client *client)
{
	struct ssusb_redriver *redriver = i2c_get_clientdata(client);
	int ret;

	/* Set back to USB mode with four channel enabled */
	ret = redriver_i2c_reg_set(redriver, GEN_DEV_SET_REG,
			GEN_DEV_SET_REG_DEFAULT);
	if (ret < 0)
		dev_err(&client->dev,
			"%s: fail to set USB mode with 4 channel enabled.\n",
			__func__);
	else
		dev_dbg(&client->dev,
			"%s: successfully set back to USB mode.\n",
			__func__);
}

static const struct of_device_id redriver_match_table[] = {
	{ .compatible = "onnn,redriver",},
	{ },
};

static const struct i2c_device_id redriver_i2c_id[] = {
	{ "ssusb-redriver", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, redriver_i2c_id);

static struct i2c_driver redriver_i2c_driver = {
	.driver = {
		.name	= "ssusb-redriver",
		.of_match_table	= redriver_match_table,
		.pm	= &redriver_i2c_pm,
	},

	.probe		= redriver_i2c_probe,
	.remove		= redriver_i2c_remove,

	.shutdown	= redriver_i2c_shutdown,

	.id_table	= redriver_i2c_id,
};

module_i2c_driver(redriver_i2c_driver);

MODULE_DESCRIPTION("USB Super Speed Linear Re-Driver Driver");
MODULE_LICENSE("GPL v2");
