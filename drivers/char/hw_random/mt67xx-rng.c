// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT67xx TRNG, read through the secure monitor.
 *
 * On these SoCs the TRNG block is only reachable from the secure world;
 * TF-A hands out 16 bytes per MTK_SIP_KERNEL_GET_RND call.
 *
 * Copyright (C) 2019 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/hw_random.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MTK_SIP_KERNEL_GET_RND	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
						   ARM_SMCCC_SMC_64, \
						   ARM_SMCCC_OWNER_SIP, 0x26a)
#define MT67XX_RNG_MAGIC	0x74726e67

static void mt67xx_rng_smc(u32 val[4])
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_GET_RND, MT67XX_RNG_MAGIC,
		      0, 0, 0, 0, 0, 0, &res);
	val[0] = res.a0;
	val[1] = res.a1;
	val[2] = res.a2;
	val[3] = res.a3;
}

static int mt67xx_rng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	u32 val[4];
	int ret = 0;

	while (max >= sizeof(val)) {
		mt67xx_rng_smc(val);
		memcpy(buf, val, sizeof(val));
		buf += sizeof(val);
		max -= sizeof(val);
		ret += sizeof(val);
	}
	memzero_explicit(val, sizeof(val));

	return ret;
}

static int mt67xx_rng_probe(struct platform_device *pdev)
{
	struct hwrng *rng;
	u32 val[4];

	/* firmware without the call answers SMCCC_RET_NOT_SUPPORTED in a0 */
	mt67xx_rng_smc(val);
	if (val[0] == (u32)SMCCC_RET_NOT_SUPPORTED && !val[1] && !val[2] && !val[3])
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "TRNG SMC not supported by firmware\n");
	memzero_explicit(val, sizeof(val));

	rng = devm_kzalloc(&pdev->dev, sizeof(*rng), GFP_KERNEL);
	if (!rng)
		return -ENOMEM;

	rng->name = KBUILD_MODNAME;
	rng->read = mt67xx_rng_read;
	rng->quality = 900;

	return devm_hwrng_register(&pdev->dev, rng);
}

static const struct of_device_id mt67xx_rng_match[] = {
	{ .compatible = "mediatek,mt67xx-rng" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt67xx_rng_match);

static struct platform_driver mt67xx_rng_driver = {
	.probe = mt67xx_rng_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mt67xx_rng_match,
	},
};
module_platform_driver(mt67xx_rng_driver);

MODULE_DESCRIPTION("MediaTek MT67xx secure-monitor TRNG driver");
MODULE_LICENSE("GPL");
