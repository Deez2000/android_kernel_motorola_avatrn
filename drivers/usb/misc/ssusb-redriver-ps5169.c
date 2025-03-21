// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/usb/ucsi_glink.h>
#include <linux/slab.h>
#include <linux/usb/ch9.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_gpio.h>
#include <linux/usb/redriver.h>
#include <linux/regulator/consumer.h>

/* dp eq */
#define PS5169_DP_EQ0_REG		0x52
#define PS5169_DP_EQ0_MASK		0x70
#define PS5169_DP_EQ1_REG		0x5e
#define PS5169_DP_EQ1_MASK		0x07

#define PS5169_DP_EQ_MAX		5
#define PS5169_DP_EQ_DEFAULT	0
static const struct ps5169_dp_eq {
	unsigned int eq0; /* 0x52 bit 6:4 */
	unsigned int eq1; /* 0x5e bit 2:0 */
} dp_eq[] = {
	{0x00, 0x04}, /* 0:2db */
	{0x10, 0x05}, /* 1:5.5db */
	{0x20, 0x06}, /* 2:6.5db */
	{0x30, 0x06}, /* 3:7.5db */
	{0x40, 0x06}, /* 4:8db */
	{0x50, 0x07}, /* 5:8.5db */
	{0x60, 0x07}, /* 6:9.5db */
	{0x70, 0x07}, /* 7:10db ??? */
};

#define PS5169_DP_EQ0_VAL(eq) dp_eq[eq].eq0
#define PS5169_DP_EQ1_VAL(eq) dp_eq[eq].eq1

/* dp gain */
#define PS5169_DP_GAIN_REG		0x5c
#define PS5169_DP_GAIN_MASK		BIT(4)
#define PS5169_DP_GAIN_MAX		1 /* -0.9db */
#define PS5169_DP_GAIN_DEFAULT		0 /* 0 db */

/* usb tx eq */
#define PS5169_USB_TX_EQ0_REG		0x50
#define PS5169_USB_TX_EQ0_MASK		0x70
#define PS5169_USB_TX_EQ1_REG		0x5d
#define PS5169_USB_TX_EQ1_MASK		0x70
#define PS5169_USB_TX_EQ2_REG		0x54
#define PS5169_USB_TX_EQ2_MASK		0xf0

#define PS5169_USB_TX_EQ_MAX		7
#define PS5169_USB_TX_EQ_DEFAULT	1
static const struct ps5169_usb_tx_eq {
	unsigned int eq0; /* 0x50 bit 6:4 */
	unsigned int eq1; /* 0x5d bit 6:4 */
	unsigned int eq2; /* 0x54 bit 7:4 */
} usb_tx_eq[] = {
	{0x00, 0x40, 0x00}, /* 0:2db */
	{0x10, 0x50, 0x10}, /* 1:5.5db */
	{0x20, 0x60, 0x10}, /* 2:6.5db */
	{0x30, 0x60, 0x50}, /* 3:7.5db */
	{0x40, 0x60, 0xc0}, /* 4:8db */
	{0x50, 0x70, 0x50}, /* 5:8.5db */
	{0x60, 0x70, 0xf0}, /* 6:9.5db */
	{0x70, 0x70, 0xf0}, /* 7:10db ???? */
};

#define PS5169_USB_TX_EQ0_VAL(eq) usb_tx_eq[eq].eq0
#define PS5169_USB_TX_EQ1_VAL(eq) usb_tx_eq[eq].eq1
#define PS5169_USB_TX_EQ2_VAL(eq) usb_tx_eq[eq].eq2

/* usb tx gain */
#define PS5169_USB_TX_GAIN_REG		0x5c
#define PS5169_USB_TX_GAIN_MASK		BIT(0)
#define PS5169_USB_TX_GAIN_MAX		1 /* -0.9db */
#define PS5169_USB_TX_GAIN_DEFAULT	0 /* 0db */

/* usb rx eq */
#define PS5169_USB_RX_EQ0_REG		0x51
#define PS5169_USB_RX_EQ0_MASK		0x70
#define PS5169_USB_RX_EQ1_REG		0x77
#define PS5169_USB_RX_EQ1_MASK		0xf0
#define PS5169_USB_RX_EQ2_REG		0x54
#define PS5169_USB_RX_EQ2_MASK		0x0f
#define PS5169_USB_RX_EQ3_REG		0x78
#define PS5169_USB_RX_EQ3_MASK		0xe1

#define PS5169_USB_RX_EQ_MAX		7
static const struct ps5169_usb_rx_eq {
	unsigned int eq0; /* 0x51 bit 7:4 2:1 */
	unsigned int eq1; /* 0x77 bit 7:4 */
	unsigned int eq2; /* 0x54 bit 3:0 */
	unsigned int eq3; /* 0x78 bit 7:5 0 */
} usb_rx_eq[] = {
	{0x86, 0x00, 0x00, 0x20}, /* 0:5.2db */
	{0x96, 0x00, 0x01, 0x20}, /* 1:6db */
	{0xa6, 0x50, 0x01, 0x40}, /* 2:7db */
	{0xb6, 0x50, 0x05, 0x40}, /* 3:8db */
	{0xc6, 0xb0, 0x0c, 0x80}, /* 4:8.8db */
	{0xd6, 0xf0, 0x05, 0x80}, /* 5:9.6db */
	{0xe6, 0xf0, 0x0f, 0x80}, /* 6:10.4db */
	{0xf6, 0x30, 0x0f, 0xa1}, /* 7:11.2db */
};

#define PS5169_USB_RX_EQ0_VAL(eq) usb_rx_eq[eq].eq0
#define PS5169_USB_RX_EQ1_VAL(eq) usb_rx_eq[eq].eq1
#define PS5169_USB_RX_EQ2_VAL(eq) usb_rx_eq[eq].eq2
#define PS5169_USB_RX_EQ3_VAL(eq) usb_rx_eq[eq].eq3

/* usb rx gain */
#define PS5169_USB_RX_GAIN_REG		0x5c
#define PS5169_USB_RX_GAIN_MASK		BIT(2)
#define PS5169_USB_RX_GAIN_MAX		1 /* -0.9db */
#define PS5169_USB_RX_GAIN_DEFAULT	0 /* 0db */

/* configuration */
#define PS5169_CONFIG_REG			0x40
#define PS5169_CONFIG_REG_DEFAULT	0x40
#define PS5169_PD_ACTIVE			BIT(7)
#define PS5169_USB3_EN				BIT(6)
#define PS5169_DP_EN				BIT(5)
#define PS5169_FLIPPED				BIT(4)
#define PS5169_EMODE				BIT(3)

/* chip id */
#define PS5169_CHIPID_REG			0xAC
#define PS5169_CHIP_ID				0x6987

enum operation_mode {
	OP_MODE_NONE,
	OP_MODE_USB3,
	OP_MODE_DP,
	OP_MODE_USB3_AND_DP,
	OP_MODE_DEFAULT,	/* 4 lanes USB */
};

#define NOTIFIER_PRIORITY		1

struct ps5169_redriver {
	struct device		*dev;
	struct i2c_client	*client;
	struct regulator *vcc;
	struct regulator *vio;
	struct regmap		*regmap;
	struct reg_sequence *config_seqs;
	unsigned int config_seqs_cnt;
	struct notifier_block	vbus_nb;
	struct notifier_block	id_nb;
	struct notifier_block	dp_nb;
	struct dentry		*debug_root;

	/* tuning purpose */
	unsigned int dpeq;
	unsigned int dpgain;
	unsigned int usbtxeq;
	unsigned int usbtxgain;
	unsigned int usbrxeq;
	unsigned int usbrxgain;

	bool host_active;
	bool vbus_active;
	enum plug_orientation typec_orientation;
	enum operation_mode op_mode;
	struct notifier_block ucsi_nb;
	int ucsi_i2c_write_err;
	int orientation_gpio;
	int enable_gpio;
	struct workqueue_struct *pullup_wq;
	struct work_struct	pullup_work;
	int			pullup_req;
	bool			work_ongoing;
	bool			config_dp_eq;

	struct work_struct	host_work;
};

static const char * const opmode_string[] = {
	[OP_MODE_NONE] = "NONE",
	[OP_MODE_USB3] = "USB3",
	[OP_MODE_DP] = "DP",
	[OP_MODE_USB3_AND_DP] = "USB3 and DP",
	[OP_MODE_DEFAULT] = "DEFAULT",
};
#define OPMODESTR(x) opmode_string[x]


static inline int ps5169_redriver_write_reg_bits(struct ps5169_redriver *ps5169,
		unsigned int reg, unsigned int val, unsigned int mask);
static void ps5169_config_work_mode(struct ps5169_redriver *ps5169,
		enum operation_mode mode);
static int ps5169_config_seqs_init(struct ps5169_redriver *ps5169);

static int ssusb_redriver_enable_chip(struct ps5169_redriver *ps5169, bool en)
{
	if (!gpio_is_valid(ps5169->enable_gpio)) {
		dev_err(ps5169->dev, "%s: Invalid enable_gpio: %d\n", __func__,
				ps5169->enable_gpio);
		return -EINVAL;
	}

	gpio_set_value(ps5169->enable_gpio, en);
	if (en)
		mdelay(10);

	dev_info(ps5169->dev, "%s: readback enable gpio status=:%d\n", __func__,
				gpio_get_value(ps5169->enable_gpio));
	return 0;
}
static int ssusb_redriver_read_orientation(struct ps5169_redriver *ps5169)
{
	int ret;

	if (!gpio_is_valid(ps5169->orientation_gpio))
		return -EINVAL;

	ret = gpio_get_value(ps5169->orientation_gpio);
	if (ret < 0) {
		dev_err(ps5169->dev, "fail to read CC out, ret=%d\n", ret);
		return ret;
	}

	if (ps5169->op_mode == OP_MODE_NONE) {
		ps5169->typec_orientation = ORIENTATION_UNKNOWN;
	} else if (ret == 0) {
		ps5169->typec_orientation = ORIENTATION_CC1;
	} else {
		ps5169->typec_orientation = ORIENTATION_CC2;
	}

	return 0;
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
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	if (!gpio_is_valid(ps5169->orientation_gpio))
		return -EINVAL;

	return gpio_get_value(ps5169->orientation_gpio);
}
EXPORT_SYMBOL(redriver_orientation_get);

static int ssusb_redriver_ucsi_notifier(struct notifier_block *nb,
								unsigned long action, void *data)
{
	struct ps5169_redriver *ps5169 =
		container_of(nb, struct ps5169_redriver, ucsi_nb);
	struct ucsi_glink_constat_info *info = data;
	enum operation_mode op_mode;
	int ret;

	if (info->connect && !info->partner_change)
		return NOTIFY_DONE;

	if (!info->connect) {
		if (info->partner_usb || info->partner_alternate_mode)
			dev_err(ps5169->dev, "set partner when no connection\n");
		op_mode = OP_MODE_NONE;
	} else if (info->partner_usb && info->partner_alternate_mode) {
		/*
		 * when connect a DP only cable,
		 * ucsi set usb flag first, then set usb and alternate mode
		 * after dp start link training.
		 * it should only set alternate_mode flag ???
		 */
		if (ps5169->op_mode == OP_MODE_DP) {
			return NOTIFY_OK;
		}
		op_mode = OP_MODE_USB3_AND_DP;
	} else if (info->partner_usb) {
		if (ps5169->op_mode == OP_MODE_DP) {
			return NOTIFY_OK;
		}
		op_mode = OP_MODE_USB3;
	} else if (info->partner_alternate_mode) {
		op_mode = OP_MODE_DP;
	} else
		op_mode = OP_MODE_NONE;

	if (ps5169->op_mode == op_mode)
		return NOTIFY_OK;

	dev_info(ps5169->dev, "op mode %s -> %s\n",
			 OPMODESTR(ps5169->op_mode), OPMODESTR(op_mode));
	ps5169->op_mode = op_mode;

	if (ps5169->op_mode == OP_MODE_NONE) {
		ssusb_redriver_enable_chip(ps5169, false);
		return NOTIFY_OK;
	}

	if (ps5169->op_mode == OP_MODE_USB3 ||
			ps5169->op_mode == OP_MODE_USB3_AND_DP) {
		ssusb_redriver_read_orientation(ps5169);

		dev_info(ps5169->dev, "orientation %s\n",
				 ps5169->typec_orientation == ORIENTATION_CC1 ?
				 "CC1" : "CC2");
	}
	ssusb_redriver_enable_chip(ps5169, true);
	ret = ps5169_config_seqs_init(ps5169);
	if (ret) {
		dev_info(ps5169->dev, "i2c bus may not resume(%d)\n", ret);
		ps5169->ucsi_i2c_write_err = ret;
		return NOTIFY_DONE;
	}
	ps5169_config_work_mode(ps5169, ps5169->op_mode);
	return NOTIFY_OK;
}

int redriver_notify_connect(struct device_node *node, enum plug_orientation orientation)
{
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	/* 1. no operation in recovery mode.
	 * 2. needed when usb related mode set.
	 * 3. currently ucsi notification arrive to redriver earlier than usb,
	 * in ucsi notification callback, save mode even i2c write failed,
	 * but add ucsi_i2c_write_err to indicate i2c write error,
	 * this allow usb trigger i2c write again by check it.
	 * !!! if future remove ucsi, ucsi_i2c_write_err can be removed,
	 * and this function also need update !!!.
	 */
	if ((ps5169->op_mode == OP_MODE_DEFAULT) ||
			((ps5169->op_mode != OP_MODE_USB3) &&
			 (ps5169->op_mode != OP_MODE_USB3_AND_DP)) ||
			(!ps5169->ucsi_i2c_write_err))
		return 0;

	dev_info(ps5169->dev, "op mode %s\n",
			 OPMODESTR(ps5169->op_mode));

	/* !!! assume i2c resume complete here !!! */
	ssusb_redriver_enable_chip(ps5169, true);
	ps5169_config_seqs_init(ps5169);
	ssusb_redriver_read_orientation(ps5169);
	ps5169_config_work_mode(ps5169, ps5169->op_mode);

	ps5169->ucsi_i2c_write_err = 0;

	return 0;
}
EXPORT_SYMBOL(redriver_notify_connect);

int redriver_notify_disconnect(struct device_node *node)
{
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	/* 1. no operation in recovery mode.
	 * 2. there is case for 4 lane display, first report usb mode,
	 * second call usb release super speed lanes,
	 * then stop usb host and call this disconnect,
	 * it should not disable chip.
	 * 3. if already disabled, no need to disable again.
	 */
	if ((ps5169->op_mode == OP_MODE_DEFAULT) ||
			(ps5169->op_mode == OP_MODE_DP) ||
			(ps5169->op_mode == OP_MODE_NONE))
		return 0;

	dev_dbg(ps5169->dev, "op mode %s -> %s\n",
			OPMODESTR(ps5169->op_mode), OPMODESTR(OP_MODE_NONE));

	ps5169->op_mode = OP_MODE_NONE;
	ssusb_redriver_enable_chip(ps5169, false);

	return 0;
}
EXPORT_SYMBOL(redriver_notify_disconnect);

int redriver_release_usb_lanes(struct device_node *node)
{
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	if (ps5169->op_mode == OP_MODE_DP)
		return 0;

	dev_info(ps5169->dev, "display notify 4 lane mode\n");
	dev_info(ps5169->dev, "op mode %s -> %s\n",
			 OPMODESTR(ps5169->op_mode), OPMODESTR(OP_MODE_DP));
	ps5169->op_mode = OP_MODE_DP;
	ssusb_redriver_enable_chip(ps5169, true);
	ps5169_config_seqs_init(ps5169);
	ps5169_config_work_mode(ps5169, OP_MODE_DP);
	return 0;
}
EXPORT_SYMBOL(redriver_release_usb_lanes);

static void redriver_gadget_pullup_work(struct work_struct *w)
{
	struct ps5169_redriver *ps5169 =
		container_of(w, struct ps5169_redriver, pullup_work);

	ssusb_redriver_enable_chip(ps5169, false);
	usleep_range(1000, 1500);
	ssusb_redriver_enable_chip(ps5169, true);
	ps5169_config_seqs_init(ps5169);
	ps5169_config_work_mode(ps5169, ps5169->op_mode);

	ps5169->work_ongoing = false;
}

int redriver_gadget_pullup_enter(struct device_node *node, int is_on)
{
	struct ps5169_redriver *ps5169;
	u64 time = 0;

	if (!is_on)
		return 0;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	if (ps5169->op_mode != OP_MODE_USB3)
		return 0;

	while (ps5169->work_ongoing) {
		udelay(1);
		if (time++ > 500000) {
			dev_warn(ps5169->dev, "pullup timeout\n");
			break;
		}
	}

	dev_info(ps5169->dev, "pull-up disable work took %llu us\n", time);
	return 0;
}
EXPORT_SYMBOL(redriver_gadget_pullup_enter);

int redriver_gadget_pullup_exit(struct device_node *node, int is_on)
{
	struct ps5169_redriver *ps5169;

	if (is_on)
		return 0;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	ps5169->pullup_req = is_on;

	dev_info(ps5169->dev, "%s: mode %s, %d, %d\n", __func__,
		OPMODESTR(ps5169->op_mode), is_on, ps5169->work_ongoing);
	if (ps5169->op_mode != OP_MODE_USB3)
		return 0;

	ps5169->work_ongoing = true;
	queue_work(ps5169->pullup_wq, &ps5169->pullup_work);

	return 0;
}
EXPORT_SYMBOL(redriver_gadget_pullup_exit);

static void redriver_host_work(struct work_struct *w)
{
	struct ps5169_redriver *ps5169 =
			container_of(w, struct ps5169_redriver, host_work);

	ssusb_redriver_enable_chip(ps5169, false);
	usleep_range(2000, 2500);
	ssusb_redriver_enable_chip(ps5169, true);
	ps5169_config_seqs_init(ps5169);
	ps5169_config_work_mode(ps5169, ps5169->op_mode);
}

int redriver_powercycle(struct device_node *node)
{
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	if (ps5169->op_mode != OP_MODE_USB3)
		return -EINVAL;

	schedule_work(&ps5169->host_work);

	return 0;
}
EXPORT_SYMBOL(redriver_powercycle);

static void ssusb_redriver_gpio_init(
	struct ps5169_redriver *ps5169)
{
	struct device *dev = ps5169->dev;
	int rc;

	ps5169->orientation_gpio = of_get_named_gpio(dev->of_node, "redriver,orientation-gpio", 0);
	if (!gpio_is_valid(ps5169->orientation_gpio)) {
		dev_err(dev, "Failed to get orientation gpio\n");
		return;
	}

	rc = devm_gpio_request(dev, ps5169->orientation_gpio, "redriver-orientation-gpio");
	if (rc < 0) {
		dev_err(dev, "Failed to request orientation gpio\n");
		ps5169->orientation_gpio = -EINVAL;
		return;
	}

	ps5169->enable_gpio = of_get_named_gpio(dev->of_node, "redriver,enable-gpio", 0);
	if (!gpio_is_valid(ps5169->enable_gpio)) {
		dev_err(dev, "Failed to get enable gpio\n");
		return;
	}

	rc = devm_gpio_request(dev, ps5169->enable_gpio, "redriver-enable-gpio");
	if (rc < 0) {
		dev_err(dev, "Failed to request orientation gpio\n");
		ps5169->enable_gpio = -EINVAL;
		return;
	}
}

static inline int ps5169_redriver_write_reg_bits(struct ps5169_redriver *ps5169,
		unsigned int reg, unsigned int val, unsigned int mask)
{
	int ret = regmap_update_bits(ps5169->regmap, reg, mask, val);

	if (ret < 0)
		dev_err(&ps5169->client->dev, "error update reg %u\n", reg);

	dev_dbg(&ps5169->client->dev, "update reg[%u]=[%x], %x\n", reg, val, mask);
	return ret;
}

static int ps5169_config_dp_eq(struct ps5169_redriver *ps5169,
		unsigned int eq)
{
	if (eq > PS5169_DP_EQ_MAX) {
		dev_err(&ps5169->client->dev, "error dp eq value %u\n", eq);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_DP_EQ0_REG,
			PS5169_DP_EQ0_VAL(eq), PS5169_DP_EQ0_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_DP_EQ1_REG,
			PS5169_DP_EQ1_VAL(eq), PS5169_DP_EQ1_MASK);

	return 0;
}

static int ps5169_config_dp_gain(struct ps5169_redriver *ps5169,
		unsigned int gain)
{
	if (gain > PS5169_DP_GAIN_MAX) {
		dev_err(&ps5169->client->dev, "error dp gain value %u\n", gain);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_DP_GAIN_REG, gain,
			PS5169_DP_GAIN_MASK);

	return 0;
}

static int ps5169_config_usb_tx_eq(struct ps5169_redriver *ps5169,
		unsigned int eq)
{
	if (eq > PS5169_USB_TX_EQ_MAX) {
		dev_err(&ps5169->client->dev, "error usb tx eq value %u\n", eq);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_TX_EQ0_REG,
			PS5169_USB_TX_EQ0_VAL(eq), PS5169_USB_TX_EQ0_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_TX_EQ1_REG,
			PS5169_USB_TX_EQ1_VAL(eq), PS5169_USB_TX_EQ1_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_TX_EQ2_REG,
			PS5169_USB_TX_EQ2_VAL(eq), PS5169_USB_TX_EQ2_MASK);

	return 0;
}

static int ps5169_config_usb_tx_gain(struct ps5169_redriver *ps5169,
		unsigned int gain)
{
	if (gain > PS5169_USB_TX_GAIN_MAX) {
		dev_err(&ps5169->client->dev,
				"error usb tx gain value %u\n", gain);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_TX_GAIN_REG, gain,
			PS5169_USB_TX_GAIN_MASK);

	return 0;
}

static int ps5169_config_usb_rx_eq(struct ps5169_redriver *ps5169,
		unsigned int eq)
{
	if (eq > PS5169_USB_RX_EQ_MAX) {
		dev_err(&ps5169->client->dev, "error usb rx eq value %u\n", eq);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_RX_EQ0_REG,
			PS5169_USB_RX_EQ0_VAL(eq), PS5169_USB_RX_EQ0_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_RX_EQ1_REG,
			PS5169_USB_RX_EQ1_VAL(eq), PS5169_USB_RX_EQ1_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_RX_EQ2_REG,
			PS5169_USB_RX_EQ2_VAL(eq), PS5169_USB_RX_EQ2_MASK);

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_RX_EQ3_REG,
			PS5169_USB_RX_EQ3_VAL(eq), PS5169_USB_RX_EQ3_MASK);

	return 0;
}

static int ps5169_config_usb_rx_gain(struct ps5169_redriver *ps5169,
		unsigned int gain)
{
	if (gain > PS5169_USB_RX_GAIN_MAX) {
		dev_err(&ps5169->client->dev,
				"error usb rx gain value %u\n", gain);
		return -EINVAL;
	}

	ps5169_redriver_write_reg_bits(ps5169, PS5169_USB_RX_GAIN_REG, gain,
			PS5169_USB_RX_GAIN_MASK);

	return 0;
}

/* save user configuration and dump for device tree */
static inline void update_config_reg(struct ps5169_redriver *ps5169,
		unsigned int reg, unsigned int val, unsigned int mask)
{
	int i;

	for (i = 0; i < ps5169->config_seqs_cnt; i++)
		if (ps5169->config_seqs[i].reg == reg) {
			ps5169->config_seqs[i].def &= ~mask;
			ps5169->config_seqs[i].def |= (val & mask);
			break;
		}

	if (i == ps5169->config_seqs_cnt)
		pr_err("can't find reg %u in init regs\n", reg);
}

static int dp_eq_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->dpeq;

	return 0;
}

static int dp_eq_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_dp_eq(ps5169, val);
	if (ret)
		return ret;

	ps5169->dpeq = val;

	update_config_reg(ps5169, PS5169_DP_EQ0_REG,
			PS5169_DP_EQ0_VAL(val), PS5169_DP_EQ0_MASK);

	update_config_reg(ps5169, PS5169_DP_EQ1_REG,
			PS5169_DP_EQ1_VAL(val), PS5169_DP_EQ1_MASK);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(dp_eq_ops, dp_eq_get, dp_eq_set, "%llu\n");

static int dp_gain_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->dpgain;

	return 0;
}

static int dp_gain_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_dp_gain(ps5169, val);
	if (ret)
		return ret;

	ps5169->dpgain = val;

	update_config_reg(ps5169, PS5169_DP_GAIN_REG, val,
			PS5169_DP_GAIN_MASK);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(dp_gain_ops, dp_gain_get, dp_gain_set, "%llu\n");

static int usb_tx_eq_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->usbtxeq;

	return 0;
}

static int usb_tx_eq_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_usb_tx_eq(ps5169, val);
	if (ret)
		return ret;

	ps5169->usbtxeq = val;

	update_config_reg(ps5169, PS5169_USB_TX_EQ0_REG,
			PS5169_USB_TX_EQ0_VAL(val), PS5169_USB_TX_EQ0_MASK);

	update_config_reg(ps5169, PS5169_USB_TX_EQ1_REG,
			PS5169_USB_TX_EQ1_VAL(val), PS5169_USB_TX_EQ1_MASK);

	update_config_reg(ps5169, PS5169_USB_TX_EQ2_REG,
			PS5169_USB_TX_EQ2_VAL(val), PS5169_USB_TX_EQ2_MASK);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(usb_tx_eq_ops, usb_tx_eq_get, usb_tx_eq_set, "%llu\n");

static int usb_tx_gain_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->usbtxgain;

	return 0;
}

static int usb_tx_gain_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_usb_tx_gain(ps5169, val);
	if (ret)
		return ret;

	ps5169->usbtxgain = val;

	update_config_reg(ps5169, PS5169_USB_TX_GAIN_REG, val,
			PS5169_USB_TX_GAIN_MASK);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(usb_tx_gain_ops,
		usb_tx_gain_get, usb_tx_gain_set, "%llu\n");

static int usb_rx_eq_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->usbrxeq;

	return 0;
}

static int usb_rx_eq_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_usb_rx_eq(ps5169, val);
	if (ret)
		return ret;

	ps5169->usbtxeq = val;

	update_config_reg(ps5169, PS5169_USB_RX_EQ0_REG,
			PS5169_USB_RX_EQ0_VAL(val), PS5169_USB_RX_EQ0_MASK);

	update_config_reg(ps5169, PS5169_USB_RX_EQ1_REG,
			PS5169_USB_RX_EQ1_VAL(val), PS5169_USB_RX_EQ1_MASK);

	update_config_reg(ps5169, PS5169_USB_RX_EQ2_REG,
			PS5169_USB_RX_EQ2_VAL(val), PS5169_USB_RX_EQ2_MASK);

	update_config_reg(ps5169, PS5169_USB_RX_EQ3_REG,
			PS5169_USB_RX_EQ3_VAL(val), PS5169_USB_RX_EQ3_MASK);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(usb_rx_eq_ops,
		usb_rx_eq_get, usb_rx_eq_set, "%llu\n");

static int usb_rx_gain_get(void *data, u64 *val)
{
	struct ps5169_redriver *ps5169 = data;

	*val = ps5169->usbrxgain;

	return 0;
}

static int usb_rx_gain_set(void *data, u64 val)
{
	int ret;

	struct ps5169_redriver *ps5169 = data;

	ret = ps5169_config_usb_rx_gain(ps5169, val);
	if (ret)
		return ret;

	ps5169->usbrxgain = val;

	update_config_reg(ps5169, PS5169_USB_RX_GAIN_REG, val,
			PS5169_USB_RX_GAIN_MASK);

	return ret;
}

static int config_seqs_show(struct seq_file *s, void *unused)
{
	struct ps5169_redriver *ps5169 = s->private;
	int i;

	for (i = 0; i < ps5169->config_seqs_cnt; i++)
		seq_printf(s, "0x%02x 0x%02x %u\n",
				ps5169->config_seqs[i].reg,
				ps5169->config_seqs[i].def,
				ps5169->config_seqs[i].delay_us);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(config_seqs);

DEFINE_DEBUGFS_ATTRIBUTE(usb_rx_gain_ops,
		usb_rx_gain_get, usb_rx_gain_set, "%llu\n");

static void ps5169_debugfs_create(struct ps5169_redriver *ps5169)
{
	struct i2c_client *client = ps5169->client;

	ps5169->debug_root = debugfs_create_dir("ps5169", NULL);
	if (!ps5169->debug_root) {
		dev_warn(&client->dev, "couldn't create debug dir\n");
		return;
	}

	debugfs_create_file("dp-eq", 0644,
			ps5169->debug_root, ps5169, &dp_eq_ops);

	debugfs_create_file("dp-gain", 0644,
			ps5169->debug_root, ps5169, &dp_gain_ops);

	debugfs_create_file("usb-tx-eq", 0644,
			ps5169->debug_root, ps5169, &usb_tx_eq_ops);

	debugfs_create_file("usb-tx-gain", 0644,
			ps5169->debug_root, ps5169, &usb_tx_gain_ops);

	debugfs_create_file("usb-rx-eq", 0644,
			ps5169->debug_root, ps5169, &usb_rx_eq_ops);

	debugfs_create_file("usb-rx-gain", 0644,
			ps5169->debug_root, ps5169, &usb_rx_gain_ops);

	debugfs_create_file("config_seqs", 0444,
			ps5169->debug_root, ps5169, &config_seqs_fops);
}

static int ps5169_config_seqs_init(struct ps5169_redriver *ps5169)
{
	int ret = 0;

	regmap_register_patch(ps5169->regmap,
		ps5169->config_seqs, ps5169->config_seqs_cnt);

	ret = ps5169_redriver_write_reg_bits(ps5169, PS5169_CONFIG_REG,
					PS5169_PD_ACTIVE, PS5169_PD_ACTIVE);

	return ret;
}

int redriver_config_dp_eq(struct device_node *node)
{
	struct ps5169_redriver *ps5169;

	ps5169 = check_devnode(node);
	if (IS_ERR_OR_NULL(ps5169))
		return -EINVAL;

	if (ps5169->config_dp_eq) {
		ps5169_redriver_write_reg_bits(ps5169, 0x52, 0x00, 0xff);
		ps5169_redriver_write_reg_bits(ps5169, 0x5e, 0x04, 0xff);
		dev_info(ps5169->dev, "%s, done.\n", __func__);
	}

	return 0;
}
EXPORT_SYMBOL(redriver_config_dp_eq);

static void ps5169_config_work_mode(struct ps5169_redriver *ps5169,
		enum operation_mode mode)
{
	struct i2c_client *client = ps5169->client;
	unsigned int val, mask;

	switch (mode) {
	case OP_MODE_NONE:
		mask = PS5169_PD_ACTIVE | PS5169_USB3_EN | PS5169_DP_EN;
		val = PS5169_PD_ACTIVE;
		dev_dbg(&client->dev, "write config %02x | %02x\n", val, mask);
		ps5169_redriver_write_reg_bits(ps5169,
				PS5169_CONFIG_REG, val, mask);
		ps5169_redriver_write_reg_bits(ps5169, 0xa0, 0x02, 0xff);
		ps5169_redriver_write_reg_bits(ps5169, 0xa1, 0x00, 0xff);
		break;
	case OP_MODE_USB3:
		mask = PS5169_PD_ACTIVE | PS5169_USB3_EN | PS5169_DP_EN |
			   PS5169_FLIPPED;
		val = PS5169_PD_ACTIVE | PS5169_USB3_EN;
		if (ps5169->typec_orientation == ORIENTATION_CC2)
			val |= PS5169_FLIPPED;
		dev_dbg(&client->dev, "write config %02x | %02x\n", val, mask);
		ps5169_redriver_write_reg_bits(ps5169,
				PS5169_CONFIG_REG, val, mask);
		break;
	case OP_MODE_DP:
		mask = PS5169_PD_ACTIVE | PS5169_USB3_EN | PS5169_DP_EN |
			PS5169_FLIPPED;
		val = PS5169_PD_ACTIVE | PS5169_DP_EN;
		if (ps5169->typec_orientation == ORIENTATION_CC2)
			val |= PS5169_FLIPPED;
		dev_dbg(&client->dev, "write config %02x | %02x\n", val, mask);
		ps5169_redriver_write_reg_bits(ps5169,
				PS5169_CONFIG_REG, val, mask);
		ps5169_redriver_write_reg_bits(ps5169, 0xa0, 0x00, 0xff);
		ps5169_redriver_write_reg_bits(ps5169, 0xa1, 0x04, 0xff);
		break;
	case OP_MODE_USB3_AND_DP:
		mask = PS5169_PD_ACTIVE | PS5169_USB3_EN | PS5169_DP_EN |
			PS5169_FLIPPED;
		val = PS5169_PD_ACTIVE | PS5169_USB3_EN | PS5169_DP_EN;
		if (ps5169->typec_orientation == ORIENTATION_CC2)
			val |= PS5169_FLIPPED;
		dev_dbg(&client->dev, "write config %02x | %02x\n", val, mask);
		ps5169_redriver_write_reg_bits(ps5169,
				PS5169_CONFIG_REG, val, mask);
		ps5169_redriver_write_reg_bits(ps5169, 0xa0, 0x00, 0xff);
		ps5169_redriver_write_reg_bits(ps5169, 0xa1, 0x04, 0xff);
		break;
	default:
		pr_err("operation mode is not support\n");
		return;
	}
}


static const struct regmap_config ps5169_regmap = {
	.max_register = 0xff,
	.reg_bits = 8,
	.val_bits = 8,
};

static int ps5169_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *dev_id)
{
	struct device *dev = &client->dev;
	struct ps5169_redriver *ps5169;
	int ret, size = 0;
	u32 device_id;

	ps5169 = devm_kzalloc(dev, sizeof(*ps5169), GFP_KERNEL);
	if (!ps5169)
		return -ENOMEM;

	ps5169->dev = dev;
	ps5169->client = client;
	ssusb_redriver_gpio_init(ps5169);

	ps5169->pullup_wq = alloc_workqueue("%s:pullup",
				WQ_UNBOUND | WQ_HIGHPRI, 0,
				dev_name(&client->dev));
	if (!ps5169->pullup_wq) {
		dev_err(&client->dev, "Failed to create pullup workqueue\n");
		return -ENOMEM;
	}

	of_get_property(dev->of_node, "config-seq", &size);
	if (!size || size % (3 * sizeof(unsigned int))) {
		dev_err(dev, "no config-seq or size is wrong\n");
		return -EINVAL;
	}

	ps5169->config_seqs = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!ps5169->config_seqs)
		return -ENOMEM;
	ps5169->config_seqs_cnt = size / (3 * sizeof(unsigned int));
	of_property_read_u32_array(dev->of_node,
				"config-seq",
				(u32 *)ps5169->config_seqs,
				ps5169->config_seqs_cnt * 3);

	ps5169->regmap = devm_regmap_init_i2c(client, &ps5169_regmap);
	if (IS_ERR(ps5169->regmap)) {
		ret = PTR_ERR(ps5169->regmap);
		dev_err(dev, "failed to allocate register map: %d\n", ret);
		return ret;
	}

	ps5169->config_dp_eq = of_property_read_bool(dev->of_node, "redriver,config-dp-eq");

	ps5169->vcc = devm_regulator_get_optional(&client->dev, "vcc");
	if (IS_ERR(ps5169->vcc) || ps5169->vcc == NULL) {
		dev_err(&client->dev, "Could not get vcc power regulator\n");
	} else {
		ret = regulator_enable(ps5169->vcc);
		if (ret)
			dev_err(&client->dev, "Could not enable vcc power regulator\n");
	}
	ps5169->vio = devm_regulator_get_optional(&client->dev, "vio");
	if (IS_ERR(ps5169->vio) || ps5169->vio == NULL) {
		dev_err(&client->dev, "Could not get vio power regulator\n");
	} else {
		ret = regulator_enable(ps5169->vio);
		if (ret)
			dev_err(&client->dev, "Could not enable vio power regulator\n");
	}

	INIT_WORK(&ps5169->pullup_work, redriver_gadget_pullup_work);
	INIT_WORK(&ps5169->host_work, redriver_host_work);

	ssusb_redriver_enable_chip(ps5169, true);
	ssusb_redriver_read_orientation(ps5169);
	ret = regmap_raw_read(ps5169->regmap, PS5169_CHIPID_REG, &device_id, 2);
	if (ret != 0) {
		dev_err(dev, "%s,device id read failed:%d\n", __func__, ret);
		goto err_detect;
	} else if (device_id != PS5169_CHIP_ID) {
		dev_err(dev, "%s,device id unknown: 0x%x\n", __func__, device_id);
		goto err_detect;
	}
	dev_info(dev, "%s,device_id=0x%x\n", __func__, device_id);

	i2c_set_clientdata(client, ps5169);

	ps5169_config_seqs_init(ps5169);

	/* how about USB enumerated before this driver load */
	ps5169_config_work_mode(ps5169, OP_MODE_NONE);
	ps5169->op_mode = OP_MODE_DEFAULT;
	ps5169->ucsi_nb.notifier_call = ssusb_redriver_ucsi_notifier;
	register_ucsi_glink_notifier(&ps5169->ucsi_nb);

	ps5169_debugfs_create(ps5169);

	dev_info(dev, "%s,ps5169 probe done.\n", __func__);

	return 0;

err_detect:
	dev_err(dev, "%s,probe failed.\n", __func__);
	if (!IS_ERR_OR_NULL(ps5169->vcc)) {
		if (regulator_is_enabled(ps5169->vcc))
			regulator_disable(ps5169->vcc);
		devm_regulator_put(ps5169->vcc);
	}
	if (!IS_ERR_OR_NULL(ps5169->vio)) {
		if (regulator_is_enabled(ps5169->vio))
			regulator_disable(ps5169->vio);
		devm_regulator_put(ps5169->vio);
	}
	devm_kfree(&client->dev, ps5169);
	return ret;
}

static int ps5169_i2c_remove(struct i2c_client *client)
{
	struct ps5169_redriver *ps5169 = i2c_get_clientdata(client);

	debugfs_remove(ps5169->debug_root);
	unregister_ucsi_glink_notifier(&ps5169->ucsi_nb);
	destroy_workqueue(ps5169->pullup_wq);
	if (!IS_ERR_OR_NULL(ps5169->vcc)) {
		if (regulator_is_enabled(ps5169->vcc))
			regulator_disable(ps5169->vcc);
		devm_regulator_put(ps5169->vcc);
	}
	if (!IS_ERR_OR_NULL(ps5169->vio)) {
		if (regulator_is_enabled(ps5169->vio))
			regulator_disable(ps5169->vio);
		devm_regulator_put(ps5169->vio);
	}

	if (gpio_is_valid(ps5169->enable_gpio)) {
		devm_gpio_free(ps5169->dev, ps5169->enable_gpio);
		ps5169->enable_gpio = -EINVAL;
	}

	if (gpio_is_valid(ps5169->orientation_gpio)) {
		devm_gpio_free(ps5169->dev, ps5169->orientation_gpio);
		ps5169->orientation_gpio = -EINVAL;
	}

	return 0;
}

static int __maybe_unused redriver_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ps5169_redriver *ps5169 = i2c_get_clientdata(client);

	dev_dbg(ps5169->dev, "%s: SS USB redriver suspend.\n",
			__func__);

	/*
	 * 1. when in 4 lanes display mode, it can't disable;
	 * 2. when in NONE mode, there is no need to re-disable;
	 * 3. when in DEFAULT mode, there is no adsp and can't disable;
	 */
	if (ps5169->op_mode == OP_MODE_DP ||
		ps5169->op_mode == OP_MODE_NONE ||
		ps5169->op_mode == OP_MODE_DEFAULT)
		return 0;

	ssusb_redriver_enable_chip(ps5169, false);

	return 0;
}

static int __maybe_unused redriver_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ps5169_redriver *ps5169 = i2c_get_clientdata(client);

	dev_dbg(ps5169->dev, "%s: SS USB redriver resume.\n",
			__func__);

	/* no suspend happen in following mode */
	if (ps5169->op_mode == OP_MODE_DP ||
		ps5169->op_mode == OP_MODE_NONE ||
		ps5169->op_mode == OP_MODE_DEFAULT)
		return 0;

	ssusb_redriver_enable_chip(ps5169, true);
	ps5169_config_seqs_init(ps5169);
	ps5169_config_work_mode(ps5169, ps5169->op_mode);

	return 0;
}

static SIMPLE_DEV_PM_OPS(redriver_i2c_pm, redriver_i2c_suspend,
		redriver_i2c_resume);

static void ps5169_i2c_shutdown(struct i2c_client *client)
{
	struct ps5169_redriver *ps5169 = i2c_get_clientdata(client);
	int ret;

	ret = ssusb_redriver_enable_chip(ps5169, false);
	if (ret < 0)
		dev_err(&client->dev,
			"%s: fail to disable redriver.\n",
			__func__);
	else
		dev_dbg(&client->dev,
			"%s: successfully disable redriver.\n",
			__func__);
}

static const struct of_device_id ps5169_match_table[] = {
	{ .compatible = "parade,ps5169-redriver",},
	{ },
};

static const struct i2c_device_id ps5169_i2c_id[] = {
	{ "ps5169_redriver", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, ps5169_i2c_id);

static struct i2c_driver ps5169_i2c_driver = {
	.driver = {
		.name	= "ps5169_redriver",
		.of_match_table	= ps5169_match_table,
		.pm = &redriver_i2c_pm,
	},

	.probe		= ps5169_i2c_probe,
	.remove		= ps5169_i2c_remove,

	.shutdown	= ps5169_i2c_shutdown,

	.id_table	= ps5169_i2c_id,
};
module_i2c_driver(ps5169_i2c_driver);

MODULE_DESCRIPTION("Parade PS5169 USB3.1 Gen2 Type-C 10Gbps Linear Redriver");
MODULE_LICENSE("GPL v2");
