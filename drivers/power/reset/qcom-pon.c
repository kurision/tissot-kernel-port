// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/panic.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_PS_HOLD_RST_CTL		0x5a
#define PON_PS_HOLD_RST_CTL2		0x5b
#define PON_PS_HOLD_TYPE_MASK		GENMASK(3, 0)
#define PON_PS_HOLD_WARM_RESET		0x1
#define PON_PS_HOLD_RESET_EN		BIT(7)
#define PON_SOFT_RB_SPARE		0x8f

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

/*
 * The boot firmware arms PS_HOLD for a hard reset, which drops DDR and with
 * it the ramoops record of the panic. Switch to a warm reset when the
 * restart comes from a panic, as the downstream msm-poweroff did. The type
 * may only change while the reset is disabled, for 10 sleep clock cycles.
 */
static int qcom_pon_panic_restart(struct sys_off_data *data)
{
	struct qcom_pon *pon = data->cb_data;

	if (!panic_in_progress())
		return NOTIFY_DONE;

	regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2,
			   PON_PS_HOLD_RESET_EN, 0);
	udelay(500);
	regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL,
			   PON_PS_HOLD_TYPE_MASK, PON_PS_HOLD_WARM_RESET);
	regmap_update_bits(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2,
			   PON_PS_HOLD_RESET_EN, PON_PS_HOLD_RESET_EN);

	return NOTIFY_DONE;
}

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	unsigned int val;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	/*
	 * Only PMICs with the separate PS_HOLD reset enable register, found
	 * armed here by the boot firmware. Priority 130 runs ahead of the
	 * PSCI (129) and PS_HOLD (128) restart handlers.
	 */
	if (reason_shift == GEN1_REASON_SHIFT &&
	    !regmap_read(pon->regmap, pon->baseaddr + PON_PS_HOLD_RST_CTL2,
			 &val) && (val & PON_PS_HOLD_RESET_EN)) {
		error = devm_register_sys_off_handler(&pdev->dev,
						      SYS_OFF_MODE_RESTART, 130,
						      qcom_pon_panic_restart,
						      pon);
		if (error)
			return error;
	}

	platform_set_drvdata(pdev, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
