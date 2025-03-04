// SPDX-License-Identifier: GPL-2.0
// Copyright (C) STMicroelectronics 2019
// Authors: Gabriel Fernandez <gabriel.fernandez@st.com>
//          Pascal Paillet <p.paillet@st.com>.

#include <linux/arm-smccc.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

/*
 * Registers description
 */
#define REG_PWR_CR3 0x0C

#define USB_3_3_EN BIT(24)
#define USB_3_3_RDY BIT(26)
#define REG_1_8_EN BIT(28)
#define REG_1_8_RDY BIT(29)
#define REG_1_1_EN BIT(30)
#define REG_1_1_RDY BIT(31)

#define STM32_SMC_PWR		0x82001001
#define STM32_WRITE		0x1
#define STM32_SMC_REG_SET	0x2
#define STM32_SMC_REG_CLEAR	0x3

/* list of supported regulators */
enum {
	PWR_REG11,
	PWR_REG18,
	PWR_USB33,
	STM32PWR_REG_NUM_REGS
};

static u32 ready_mask_table[STM32PWR_REG_NUM_REGS] = {
	[PWR_REG11] = REG_1_1_RDY,
	[PWR_REG18] = REG_1_8_RDY,
	[PWR_USB33] = USB_3_3_RDY,
};

struct stm32_pwr_reg {
	int tzen;
	void __iomem *base;
	u32 ready_mask;
};

#define SMC(class, op, address, val)\
	({\
	struct arm_smccc_res res;\
	arm_smccc_smc(class, op, address, val,\
			0, 0, 0, 0, &res);\
	})

static int stm32_pwr_reg_is_ready(struct regulator_dev *rdev)
{
	struct stm32_pwr_reg *priv = rdev_get_drvdata(rdev);
	u32 val;

	val = readl_relaxed(priv->base + REG_PWR_CR3);

	return (val & priv->ready_mask);
}

static int stm32_pwr_reg_is_enabled(struct regulator_dev *rdev)
{
	struct stm32_pwr_reg *priv = rdev_get_drvdata(rdev);
	u32 val;

	val = readl_relaxed(priv->base + REG_PWR_CR3);

	return (val & rdev->desc->enable_mask);
}

static int stm32_pwr_reg_enable(struct regulator_dev *rdev)
{
	struct stm32_pwr_reg *priv = rdev_get_drvdata(rdev);
	int ret;
	u32 val;

	if (priv->tzen) {
		SMC(STM32_SMC_PWR, STM32_SMC_REG_SET, REG_PWR_CR3,
		    rdev->desc->enable_mask);
	} else {
		val = readl_relaxed(priv->base + REG_PWR_CR3);
		val |= rdev->desc->enable_mask;
		writel_relaxed(val, priv->base + REG_PWR_CR3);
	}


	/* use an arbitrary timeout of 20ms */
	ret = readx_poll_timeout(stm32_pwr_reg_is_ready, rdev, val, val,
				 100, 20 * 1000);
	if (ret)
		dev_err(&rdev->dev, "regulator enable timed out!\n");

	return ret;
}

static int stm32_pwr_reg_disable(struct regulator_dev *rdev)
{
	struct stm32_pwr_reg *priv = rdev_get_drvdata(rdev);
	int ret;
	u32 val;

	if (priv->tzen) {
		SMC(STM32_SMC_PWR, STM32_SMC_REG_CLEAR, REG_PWR_CR3,
		    rdev->desc->enable_mask);
	} else {
		val = readl_relaxed(priv->base + REG_PWR_CR3);
		val &= ~rdev->desc->enable_mask;
		writel_relaxed(val, priv->base + REG_PWR_CR3);
	}

	/* use an arbitrary timeout of 20ms */
	ret = readx_poll_timeout(stm32_pwr_reg_is_enabled, rdev, val, !val,
				 100, 20 * 1000);
	if (ret)
		dev_err(&rdev->dev, "regulator disable timed out!\n");

	return ret;
}

static const struct regulator_ops stm32_pwr_reg_ops = {
	.enable		= stm32_pwr_reg_enable,
	.disable	= stm32_pwr_reg_disable,
	.is_enabled	= stm32_pwr_reg_is_enabled,
};

#define PWR_REG(_id, _name, _volt, _en, _supply) \
	[_id] = { \
		.id = _id, \
		.name = _name, \
		.of_match = of_match_ptr(_name), \
		.n_voltages = 1, \
		.type = REGULATOR_VOLTAGE, \
		.fixed_uV = _volt, \
		.ops = &stm32_pwr_reg_ops, \
		.enable_mask = _en, \
		.owner = THIS_MODULE, \
		.supply_name = _supply, \
	} \

static struct regulator_desc stm32_pwr_desc[] = {
	PWR_REG(PWR_REG11, "reg11", 1100000, REG_1_1_EN, "vdd"),
	PWR_REG(PWR_REG18, "reg18", 1800000, REG_1_8_EN, "vdd"),
	PWR_REG(PWR_USB33, "usb33", 3300000, USB_3_3_EN, "vdd_3v3_usbfs"),
};

static int is_stm32_soc_secured(struct platform_device *pdev, int *val)
{
	struct device_node *np = pdev->dev.of_node;
	struct regmap *syscon;
	u32 reg, mask;
	int tzc_val = 0;
	int err;

	syscon = syscon_regmap_lookup_by_phandle(np, "st,tzcr");
	if (IS_ERR(syscon)) {
		if (PTR_ERR(syscon) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "tzcr syscon required\n");
		return PTR_ERR(syscon);
	}

	err = of_property_read_u32_index(np, "st,tzcr", 1, &reg);
	if (err) {
		dev_err(&pdev->dev, "tzcr offset required !\n");
		return err;
	}

	err = of_property_read_u32_index(np, "st,tzcr", 2, &mask);
	if (err) {
		dev_err(&pdev->dev, "tzcr mask required !\n");
		return err;
	}

	err = regmap_read(syscon, reg, &tzc_val);
	if (err) {
		dev_err(&pdev->dev, "failed to read tzcr status !\n");
		return err;
	}

	*val = tzc_val & mask;

	return 0;
}

static int stm32_pwr_regulator_probe(struct platform_device *pdev)
{
	struct stm32_pwr_reg *priv;
	void __iomem *base;
	struct regulator_dev *rdev;
	struct regulator_config config = { };
	int i, ret = 0;
	int tzen = 0;

	ret = is_stm32_soc_secured(pdev, &tzen);
	if (ret)
		return ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Unable to map IO memory\n");
		return PTR_ERR(base);
	}

	config.dev = &pdev->dev;

	for (i = 0; i < STM32PWR_REG_NUM_REGS; i++) {
		priv = devm_kzalloc(&pdev->dev, sizeof(struct stm32_pwr_reg),
				    GFP_KERNEL);
		if (!priv)
			return -ENOMEM;
		priv->tzen = tzen;
		priv->base = base;
		priv->ready_mask = ready_mask_table[i];
		config.driver_data = priv;

		rdev = devm_regulator_register(&pdev->dev,
					       &stm32_pwr_desc[i],
					       &config);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			dev_err(&pdev->dev,
				"Failed to register regulator: %d\n", ret);
			break;
		}
	}
	return ret;
}

static const struct of_device_id __maybe_unused stm32_pwr_of_match[] = {
	{ .compatible = "st,stm32mp1,pwr-reg", },
	{},
};
MODULE_DEVICE_TABLE(of, stm32_pwr_of_match);

static struct platform_driver stm32_pwr_driver = {
	.probe = stm32_pwr_regulator_probe,
	.driver = {
		.name  = "stm32-pwr-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(stm32_pwr_of_match),
	},
};
module_platform_driver(stm32_pwr_driver);

MODULE_DESCRIPTION("STM32MP1 PWR voltage regulator driver");
MODULE_AUTHOR("Pascal Paillet <p.paillet@st.com>");
