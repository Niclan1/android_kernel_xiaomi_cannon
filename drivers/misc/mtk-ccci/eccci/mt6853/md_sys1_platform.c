/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6359/registers.h>
#include <linux/regulator/consumer.h>
#include <mt-plat/mtk_rtc.h>
#include "ccci_config.h"
#include <linux/clk.h>
#include <mach/mtk_pbm.h>
#ifdef FEATURE_CLK_BUF
#include <mtk_clkbuf_ctl.h>
#endif
#ifdef CONFIG_MTK_EMI_BWL
#include <emi_mbw.h>
#endif

#ifdef FEATURE_INFORM_NFC_VSIM_CHANGE
#include <mach/mt6605.h>
#endif

#include "include/pmic_api_buck.h"
#include <mt-plat/upmu_common.h>
#include <mtk_spm_sleep.h>

#ifdef CONFIG_MTK_QOS_SUPPORT
#include <linux/pm_qos.h>
#include <helio-dvfsrc-opp.h>
#endif
#include <clk-mt6853-pg.h>
#include "ccci_core.h"
#include "ccci_platform.h"

#include "md_sys1_platform.h"
#include "modem_secure_base.h"
#include "modem_reg_base.h"
#ifdef CCCI_PLATFORM_MT6877
#include "ap_md_reg_dump_6877.h"
#else
#include "ap_md_reg_dump.h"
#endif
#include <devapc_public.h>
#include "ccci_fsm.h"
#include "../../mtk-ccci-hw.h"

static struct ccci_clk_node clk_table[] = {
	{ NULL, "scp-sys-md1-main"},
	{ NULL, "infra-dpmaif-clk"},
	{ NULL, "infra-dpmaif-blk-clk"},
	{ NULL, "infra-ccif-ap"},
	{ NULL, "infra-ccif-md"},
	{ NULL, "infra-ccif1-ap"},
	{ NULL, "infra-ccif1-md"},
	{ NULL, "infra-ccif2-ap"},
	{ NULL, "infra-ccif2-md"},
	{ NULL, "infra-ccif4-md"},
	{ NULL, "infra-ccif5-md"},
};

unsigned int devapc_check_flag;
spinlock_t devapc_flag_lock;

#define TAG "mcd"

#define ROr2W(a, b, c)  ccci_write32(a, b, (ccci_read32(a, b)|c))
#define RAnd2W(a, b, c)  ccci_write32(a, b, (ccci_read32(a, b)&c))
#define RabIsc(a, b, c) ((ccci_read32(a, b)&c) != c)

static int s_md_start_completed;

int ccif_read32(void *b, unsigned long a)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&devapc_flag_lock, flags);
	ret = ((devapc_check_flag == 1) ?
			ioread32((void __iomem *)((b)+(a))) : 0);
	spin_unlock_irqrestore(&devapc_flag_lock, flags);

	return ret;
}

void md_cldma_hw_reset(unsigned char md_id)
{

}

void md1_subsys_debug_dump(enum subsys_id sys)
{
	struct ccci_modem *md = NULL;

	if (sys != SYS_MD1)
		return;
		/* add debug dump */

	CCCI_NORMAL_LOG(0, TAG, "%s\n", __func__);
	md = ccci_md_get_modem_by_id(0);
	if (md != NULL) {
		CCCI_NORMAL_LOG(0, TAG, "%s dump start\n", __func__);
		md->ops->dump_info(md, DUMP_FLAG_CCIF_REG | DUMP_FLAG_CCIF |
			DUMP_FLAG_REG | DUMP_FLAG_QUEUE_0_1 |
			DUMP_MD_BOOTUP_STATUS, NULL, 0);
		mdelay(1000);
		md->ops->dump_info(md, DUMP_FLAG_REG, NULL, 0);
	}
	CCCI_NORMAL_LOG(0, TAG, "%s exit\n", __func__);
}

struct pg_callbacks md1_subsys_handle = {
	.debug_dump = md1_subsys_debug_dump,
};

/*devapc_violation_triggered*/
static enum devapc_cb_status devapc_dump_adv_cb(uint32_t vio_addr)
{
	int count;

	CCCI_NORMAL_LOG(0, TAG,
		"[%s] vio_addr: 0x%x; is normal mdee: %d\n",
		__func__, vio_addr, ccci_fsm_is_normal_mdee());

	if (ccci_fsm_get_md_state(0) == EXCEPTION &&
		ccci_fsm_is_normal_mdee()) {
		count = ccci_fsm_increase_devapc_dump_counter();

		CCCI_NORMAL_LOG(0, TAG,
			"[%s] count: %d\n", __func__, count);

		if (count == 1)
			ccci_md_dump_in_interrupt((char *)__func__);

		return DEVAPC_NOT_KE;

	} else {
		ccci_md_dump_in_interrupt((char *)__func__);

		return DEVAPC_OK;
	}
}

static struct devapc_vio_callbacks devapc_test_handle = {
	.id = INFRA_SUBSYS_MD,
	.debug_dump_adv = devapc_dump_adv_cb,
};

void __weak register_devapc_vio_callback(
		struct devapc_vio_callbacks *viocb)
{
	CCCI_ERROR_LOG(-1, TAG, "[%s] is not supported!\n", __func__);
}

void ccci_md_devapc_register_cb(void)
{
	/*register handle function*/
	register_devapc_vio_callback(&devapc_test_handle);
}

void ccci_md_dump_in_interrupt(char *user_info)
{
	struct ccci_modem *md = NULL;

	CCCI_NORMAL_LOG(0, TAG, "%s called by %s\n", __func__, user_info);
	md = ccci_md_get_modem_by_id(0);
	if (md != NULL) {
		CCCI_NORMAL_LOG(0, TAG, "%s dump start\n", __func__);
		md->ops->dump_info(md, DUMP_FLAG_CCIF_REG | DUMP_FLAG_CCIF |
			DUMP_FLAG_REG | DUMP_FLAG_QUEUE_0_1 |
			DUMP_MD_BOOTUP_STATUS, NULL, 0);
	} else
		CCCI_NORMAL_LOG(0, TAG, "%s error\n", __func__);
	CCCI_NORMAL_LOG(0, TAG, "%s exit\n", __func__);
}
EXPORT_SYMBOL(ccci_md_dump_in_interrupt);

void ccci_md_debug_dump(char *user_info)
{
	struct ccci_modem *md = NULL;

	if (!s_md_start_completed) {
		CCCI_ERROR_LOG(0, TAG,
			"[%s] error: md start no call.\n", __func__);
		return;
	}

	CCCI_NORMAL_LOG(0, TAG, "%s called by %s\n", __func__, user_info);
	md = ccci_md_get_modem_by_id(0);
	if (md != NULL) {
		CCCI_NORMAL_LOG(0, TAG, "%s dump start\n", __func__);
		md->ops->dump_info(md, DUMP_FLAG_CCIF_REG | DUMP_FLAG_CCIF |
			DUMP_FLAG_REG | DUMP_FLAG_QUEUE_0_1 |
			DUMP_MD_BOOTUP_STATUS, NULL, 0);
		mdelay(1000);
		md->ops->dump_info(md, DUMP_FLAG_REG, NULL, 0);
	} else
		CCCI_NORMAL_LOG(0, TAG, "%s error\n", __func__);
	CCCI_NORMAL_LOG(0, TAG, "%s exit\n", __func__);
}
EXPORT_SYMBOL(ccci_md_debug_dump);

int md_cd_get_modem_hw_info(struct platform_device *dev_ptr,
	struct ccci_dev_cfg *dev_cfg, struct md_hw_info *hw_info)
{
	struct device_node *node = NULL;
	int idx = 0;
	int retval;

	spin_lock_init(&devapc_flag_lock);

	if (dev_ptr->dev.of_node == NULL) {
		CCCI_ERROR_LOG(0, TAG, "modem OF node NULL\n");
		return -1;
	}

	memset(dev_cfg, 0, sizeof(struct ccci_dev_cfg));
	of_property_read_u32(dev_ptr->dev.of_node,
		"mediatek,md_id", &dev_cfg->index);
	CCCI_DEBUG_LOG(dev_cfg->index, TAG,
		"modem hw info get idx:%d\n", dev_cfg->index);
	if (!get_modem_is_enabled(dev_cfg->index)) {
		CCCI_ERROR_LOG(dev_cfg->index, TAG,
			"modem %d not enable, exit\n", dev_cfg->index + 1);
		return -1;
	}

	switch (dev_cfg->index) {
	case 0:		/* MD_SYS1 */
		dev_cfg->major = 0;
		dev_cfg->minor_base = 0;
		of_property_read_u32(dev_ptr->dev.of_node,
			"mediatek,cldma_capability", &dev_cfg->capability);

		hw_info->ap_ccif_base =
		 (unsigned long)of_iomap(dev_ptr->dev.of_node, 0);
		hw_info->md_ccif_base =
		 (unsigned long)of_iomap(dev_ptr->dev.of_node, 1);

		hw_info->md_wdt_irq_id =
		 irq_of_parse_and_map(dev_ptr->dev.of_node, 0);
		hw_info->ap_ccif_irq0_id =
		 irq_of_parse_and_map(dev_ptr->dev.of_node, 1);
		hw_info->ap_ccif_irq1_id =
		 irq_of_parse_and_map(dev_ptr->dev.of_node, 2);

		hw_info->md_pcore_pccif_base =
		 ioremap_nocache(MD_PCORE_PCCIF_BASE, 0x20);
		CCCI_BOOTUP_LOG(dev_cfg->index, TAG,
		 "pccif:%x\n", MD_PCORE_PCCIF_BASE);

		/* Device tree using none flag to register irq,
		 * sensitivity has set at "irq_of_parse_and_map"
		 */
		hw_info->ap_ccif_irq0_flags = IRQF_TRIGGER_NONE;
		hw_info->ap_ccif_irq1_flags = IRQF_TRIGGER_NONE;
		hw_info->md_wdt_irq_flags = IRQF_TRIGGER_NONE;

		hw_info->sram_size = CCIF_SRAM_SIZE;
		hw_info->md_rgu_base = MD_RGU_BASE;
		hw_info->md_boot_slave_En = MD_BOOT_VECTOR_EN;
		for (idx = 0; idx < ARRAY_SIZE(clk_table); idx++) {
			clk_table[idx].clk_ref = devm_clk_get(&dev_ptr->dev,
				clk_table[idx].clk_name);
			if (IS_ERR(clk_table[idx].clk_ref)) {
				CCCI_ERROR_LOG(dev_cfg->index, TAG,
					 "md%d get %s failed\n",
						dev_cfg->index + 1,
						clk_table[idx].clk_name);
				clk_table[idx].clk_ref = NULL;
			}
		}

		/*
		 * MD1 MTCMOS is the genpd "md" domain of the md1-power child
		 * node; it is attached here but stays off until md start.
		 */
		retval = mtk_ccci_md_pwr_init(&dev_ptr->dev);
		if (retval)
			CCCI_ERROR_LOG(dev_cfg->index, TAG,
				"md1 power domain init fail %d\n", retval);

		node = of_find_compatible_node(NULL, NULL,
			"mediatek,md_ccif4");
		if (node) {
			hw_info->md_ccif4_base = of_iomap(node, 0);
			if (!hw_info->md_ccif4_base) {
				CCCI_ERROR_LOG(dev_cfg->index, TAG,
				"ccif4_base fail: 0x%p!\n",
				(void *)hw_info->md_ccif4_base);
				return -1;
			}
		}
		/* for ccif5 */
		node = of_find_compatible_node(NULL, NULL,
			"mediatek,md_ccif5");
		if (node) {
			hw_info->md_ccif5_base = of_iomap(node, 0);
			if (!hw_info->md_ccif5_base) {
				CCCI_ERROR_LOG(dev_cfg->index, TAG,
				"ccif5_base fail: 0x%p!\n",
				(void *)hw_info->md_ccif5_base);
				return -1;
			}
		}

		node = of_find_compatible_node(NULL, NULL,
					"mediatek,topckgen");
		if (node)
			hw_info->ap_topclkgen_base = of_iomap(node, 0);

		else {
			CCCI_ERROR_LOG(-1, TAG,
				"%s:ioremap topclkgen base address fail\n",
				__func__);
			return -1;
		}

		break;
	default:
		return -1;
	}

	if (hw_info->ap_ccif_base == 0 ||
		hw_info->md_ccif_base == 0) {
		CCCI_ERROR_LOG(dev_cfg->index, TAG,
			"ap_ccif_base:0x%p, md_ccif_base:0x%p\n",
			(void *)hw_info->ap_ccif_base,
			(void *)hw_info->md_ccif_base);
		return -1;
	}
	if (hw_info->ap_ccif_irq0_id == 0 ||
		hw_info->ap_ccif_irq1_id == 0 ||
		hw_info->md_wdt_irq_id == 0) {
		CCCI_ERROR_LOG(dev_cfg->index, TAG,
			"ccif_irq0:%d,ccif_irq0:%d,md_wdt_irq:%d\n",
			hw_info->ap_ccif_irq0_id, hw_info->ap_ccif_irq1_id,
			hw_info->md_wdt_irq_id);
		return -1;
	}

	/* Get spm sleep base */
	node = of_find_compatible_node(NULL, NULL, "mediatek,sleep");
	hw_info->spm_sleep_base = (unsigned long)of_iomap(node, 0);
	if (!hw_info->spm_sleep_base) {
		CCCI_ERROR_LOG(0, TAG,
			"%s: spm_sleep_base of_iomap failed\n",
			__func__);
		return -1;
	}
	CCCI_INIT_LOG(-1, TAG, "spm_sleep_base:0x%lx\n",
			(unsigned long)hw_info->spm_sleep_base);

	CCCI_DEBUG_LOG(dev_cfg->index, TAG,
		"dev_major:%d,minor_base:%d,capability:%d\n",
		dev_cfg->major, dev_cfg->minor_base, dev_cfg->capability);

	CCCI_DEBUG_LOG(dev_cfg->index, TAG,
		"ap_ccif_base:0x%p, md_ccif_base:0x%p\n",
					(void *)hw_info->ap_ccif_base,
					(void *)hw_info->md_ccif_base);
	CCCI_DEBUG_LOG(dev_cfg->index, TAG,
		"ccif_irq0:%d,ccif_irq1:%d,md_wdt_irq:%d\n",
		hw_info->ap_ccif_irq0_id, hw_info->ap_ccif_irq1_id,
		hw_info->md_wdt_irq_id);

	register_pg_callback(&md1_subsys_handle);
	return 0;
}

/* md1 sys_clk_cg no need set in this API*/
void ccci_set_clk_cg(struct ccci_modem *md, unsigned int on)
{
	struct md_hw_info *hw_info = md->hw_info;
	int idx = 0;
	int ret = 0;
	unsigned long flags;

	CCCI_NORMAL_LOG(md->index, TAG, "%s: on=%d\n", __func__, on);

	/* Clean MD_PCCIF5_SW_READY and MD_PCCIF5_PWR_ON */
	if (!on)
#ifdef CCCI_PLATFORM_MT6877
		ccif_write32(pericfg_base, 0x200, 0x0);
#else
		ccif_write32(pericfg_base, 0x30C, 0x0);
#endif

	for (idx = 3; idx < ARRAY_SIZE(clk_table); idx++) {
		if (clk_table[idx].clk_ref == NULL)
			continue;
		if (on) {
			ret = clk_prepare_enable(clk_table[idx].clk_ref);
			if (ret)
				CCCI_ERROR_LOG(md->index, TAG,
					"%s: on=%d,ret=%d\n",
					__func__, on, ret);
			spin_lock_irqsave(&devapc_flag_lock, flags);
			devapc_check_flag = 1;
			spin_unlock_irqrestore(&devapc_flag_lock, flags);
		} else {
			if (strcmp(clk_table[idx].clk_name, "infra-ccif4-md")
				== 0) {
				udelay(1000);
				CCCI_NORMAL_LOG(md->index, TAG,
					"ccif4 %s: after 1ms, set 0x%p + 0x14 = 0xFF\n",
					__func__, hw_info->md_ccif4_base);
				ccci_write32(hw_info->md_ccif4_base, 0x14,
					0xFF); /* special use ccci_write32 */
			}
			if (strcmp(clk_table[idx].clk_name, "infra-ccif5-md")
				== 0) {
				udelay(1000);
				CCCI_NORMAL_LOG(md->index, TAG,
					"ccif5 %s: after 1ms, set 0x%p + 0x14 = 0xFF\n",
					__func__, hw_info->md_ccif5_base);
				ccci_write32(hw_info->md_ccif5_base, 0x14,
					0xFF); /* special use ccci_write32 */
			}

			spin_lock_irqsave(&devapc_flag_lock, flags);
			devapc_check_flag = 0;
			spin_unlock_irqrestore(&devapc_flag_lock, flags);
			clk_disable_unprepare(clk_table[idx].clk_ref);
		}
	}
	/* Set MD_PCCIF5_PWR_ON */
	if (on) {
		CCCI_NORMAL_LOG(md->index, TAG,
			"ccif5 current base_addr %s:  0x%lx, val:0x%x\n",
			__func__, (unsigned long)pericfg_base,
#ifdef CCCI_PLATFORM_MT6877
			ccif_read32((void *)pericfg_base, 0x200));
#else
			ccif_read32((void *)pericfg_base, 0x30C));
#endif
	}
}

void ccci_set_clk_by_id(int idx, unsigned int on)
{
	int ret = 0;

	if (idx >= ARRAY_SIZE(clk_table))
		return;
	/* idx 1 is infra-dpmaif-clk: its leaf CG is driven by the glue */
	if (idx == 1 && !on)
		mtk_ccci_dpmaif_cg(false);
	if (clk_table[idx].clk_ref == NULL)
		;
	else if (on) {
		ret = clk_prepare_enable(clk_table[idx].clk_ref);
		if (ret)
			CCCI_ERROR_LOG(-1, TAG,
				"%s: idx = %d, on=%d,ret=%d\n",
				__func__, idx, on, ret);
	} else
		clk_disable_unprepare(clk_table[idx].clk_ref);
	if (idx == 1 && on)
		mtk_ccci_dpmaif_cg(true);
}

int md_cd_io_remap_md_side_register(struct ccci_modem *md)
{
	struct md_pll_reg *md_reg = NULL;
	struct md_sys1_info *md_info = (struct md_sys1_info *)md->private_data;

	/* call internal_dump io_remap */
	md_io_remap_internal_dump_register(md);

	md_info->md_boot_slave_En =
	 ioremap_nocache(md->hw_info->md_boot_slave_En, 0x4);
	md_info->md_rgu_base =
	 ioremap_nocache(md->hw_info->md_rgu_base, 0x300);

	md_reg = kzalloc(sizeof(struct md_pll_reg), GFP_KERNEL);
	if (md_reg == NULL) {
		CCCI_ERROR_LOG(-1, TAG,
		 "md_sw_init:alloc md reg map mem fail\n");
		return -1;
	}

	md_reg->md_boot_stats_select =
		ioremap_nocache(MD1_BOOT_STATS_SELECT, 4);
	md_reg->md_boot_stats = ioremap_nocache(MD1_CFG_BOOT_STATS, 4);
	/*just for dump end*/

	md_info->md_pll_base = md_reg;

#ifdef MD_PEER_WAKEUP
	md_info->md_peer_wakeup = ioremap_nocache(MD_PEER_WAKEUP, 0x4);
#endif
	return 0;
}

void md_cd_lock_cldma_clock_src(int locked)
{
	/* spm_ap_mdsrc_req(locked); */
}

void md_cd_lock_modem_clock_src(int locked)
{
	size_t ret;

	/* spm_ap_mdsrc_req(locked); */
	mt_secure_call(MD_CLOCK_REQUEST,
			MD_REG_AP_MDSRC_REQ, locked, 0, 0, 0, 0);
	if (locked) {
		int settle = mt_secure_call(MD_CLOCK_REQUEST,
				MD_REG_AP_MDSRC_SETTLE, 0, 0, 0, 0, 0);

		if (!(settle > 0 && settle < 10))
			settle = 3;

		mdelay(settle);
		ret = mt_secure_call(MD_CLOCK_REQUEST,
				MD_REG_AP_MDSRC_ACK, 0, 0, 0, 0, 0);

		CCCI_NOTICE_LOG(-1, TAG,
				"settle = %d; ret = %lu\n", settle, ret);
	}
}

void md_cd_dump_md_bootup_status(struct ccci_modem *md)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL,
		MD_POWER_CONFIG, MD_BOOT_STATUS,
		0, 0, 0, 0, 0, &res);

	CCCI_NOTICE_LOG(md->index, TAG,
		"[%s] AP: boot_ret=%lu, boot_status_0=%lX, boot_status_1=%lX\n",
		__func__, res.a0, res.a1, res.a2);

}

void md_cd_get_md_bootup_status(
	struct ccci_modem *md, unsigned int *buff, int length)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL,
		MD_POWER_CONFIG, MD_BOOT_STATUS,
		0, 0, 0, 0, 0, &res);

	if (buff && (length >= 2)) {
		buff[0] = (unsigned int)res.a1;
		buff[1] = (unsigned int)res.a2;
	}

	CCCI_NOTICE_LOG(md->index, TAG,
		"boot_ret=%lu; md_boot_stats0 / 1:0x%lX / 0x%lX\n",
		res.a0, res.a1, res.a2);
}

static int dump_emi_last_bm(struct ccci_modem *md)
{
	u32 buf_len = 1024;
	u32 i, j;
	char temp_char;
	char *buf = NULL;
	char *temp_buf = NULL;

	buf = kzalloc(buf_len, GFP_ATOMIC);
	if (!buf) {
		CCCI_MEM_LOG_TAG(md->index, TAG,
			"alloc memory failed for emi last bm\n");
		return -1;
	}
#ifdef CONFIG_MTK_EMI_BWL
	dump_last_bm(buf, buf_len);
#endif
	CCCI_MEM_LOG_TAG(md->index, TAG, "Dump EMI last bm\n");
	buf[buf_len - 1] = '\0';
	temp_buf = buf;
	for (i = 0, j = 1; i < buf_len - 1; i++, j++) {
		if (buf[i] == 0x0) /* 0x0 end of hole string. */
			break;
		if (buf[i] == 0x0A && j < 256) {
			/* 0x0A stands for end of string, no 0x0D */
			buf[i] = '\0';
			CCCI_MEM_LOG(md->index, TAG,
				"%s\n", temp_buf);/* max 256 bytes */
			temp_buf = buf + i + 1;
			j = 0;
		} else if (unlikely(j >= 255)) {
			/* ccci_mem_log max buffer length: 256,
			 * but dm log maybe only less than 50 bytes.
			 */
			temp_char = buf[i];
			buf[i] = '\0';
			CCCI_MEM_LOG(md->index, TAG, "%s\n", temp_buf);
			temp_buf = buf + i;
			j = 0;
			buf[i] = temp_char;
		}
	}

	kfree(buf);

	return 0;
}

void __weak dump_emi_outstanding(void)
{
	CCCI_DEBUG_LOG(-1, TAG, "No %s\n", __func__);
}

void md_cd_dump_debug_register(struct ccci_modem *md)
{
	/* MD no need dump because of bus hang happened - open for debug */
	unsigned int reg_value[2] = { 0 };
	unsigned int ccif_sram[
		CCCI_EE_SIZE_CCIF_SRAM/sizeof(unsigned int)] = { 0 };

	/*dump_emi_latency();*/
	dump_emi_outstanding();
	dump_emi_last_bm(md);

	/* MD-side registers stall the bus while MD1 is off or isolated */
	if (!mtk_ccci_md_pwr_is_on()) {
		mtk_ccci_log_err("MD1 off, skipping MD register dump\n");
		md->ops->dump_info(md, DUMP_FLAG_CCIF, ccif_sram, 0);
		return;
	}
	md_cd_get_md_bootup_status(md, reg_value, 2);
	md->ops->dump_info(md, DUMP_FLAG_CCIF, ccif_sram, 0);
	/* copy from HS1 timeout */
	if ((reg_value[0] == 0) && (ccif_sram[1] == 0))
		return;
	else if (!((reg_value[0] == 0x5443000C) || (reg_value[0] == 0) ||
		(reg_value[0] >= 0x53310000 && reg_value[0] <= 0x533100FF)))
		return;

	md_cd_lock_modem_clock_src(1);

	/* This function needs to be cancelled temporarily for bringup */
	internal_md_dump_debug_register(md->index);

	md_cd_lock_modem_clock_src(0);

}

int md_cd_pccif_send(struct ccci_modem *md, int channel_id)
{
	int busy = 0;
	struct md_hw_info *hw_info = md->hw_info;

	md_cd_lock_modem_clock_src(1);

	busy = ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_BUSY);
	if (busy & (1 << channel_id)) {
		md_cd_lock_modem_clock_src(0);
		return -1;
	}
	ccif_write32(hw_info->md_pcore_pccif_base,
		APCCIF_BUSY, 1 << channel_id);
	ccif_write32(hw_info->md_pcore_pccif_base,
		APCCIF_TCHNUM, channel_id);

	md_cd_lock_modem_clock_src(0);
	return 0;
}

void md_cd_dump_pccif_reg(struct ccci_modem *md)
{
	struct md_hw_info *hw_info = md->hw_info;

	md_cd_lock_modem_clock_src(1);

	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_CON(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_CON,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_CON));
	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_BUSY(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_BUSY,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_BUSY));
	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_START(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_START,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_START));
	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_TCHNUM(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_TCHNUM,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_TCHNUM));
	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_RCHNUM(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_RCHNUM,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_RCHNUM));
	CCCI_MEM_LOG_TAG(md->index, TAG,
		"AP_ACK(%p)=%x\n",
		hw_info->md_pcore_pccif_base + APCCIF_ACK,
		ccif_read32(hw_info->md_pcore_pccif_base, APCCIF_ACK));

	md_cd_lock_modem_clock_src(0);
}

void md_cd_check_emi_state(struct ccci_modem *md, int polling)
{
}

/*
 * Stock vmd1_pmic_setting_on(): the MD DVFS reprograms VRFDIG (MT6359 buck
 * VPU) behind the AP's back; the boot value is recorded on the first power
 * on and restored on every restart. The MT6315 VMODEM/VSRAM_MD restore of
 * stock needs SPMI, which this kernel does not drive.
 */
static struct regulator *md1_vrfdig;
static unsigned int md1_vrfdig_vosel;

static bool md1_vrfdig_reg(struct regmap **map, unsigned int *reg,
	unsigned int *mask)
{
	if (!md1_vrfdig) {
		struct regulator *r = regulator_get_optional(NULL, "vpu");

		if (IS_ERR(r)) {
			mtk_ccci_log_err("vrfdig (vpu) regulator: %ld\n",
				PTR_ERR(r));
			return false;
		}
		md1_vrfdig = r;
	}
	*map = regulator_get_regmap(md1_vrfdig);
	if (IS_ERR_OR_NULL(*map) ||
	    regulator_get_hardware_vsel_register(md1_vrfdig, reg, mask))
		return false;
	return true;
}

#define MT6359_RTC_XTAL_PRESENT	BIT(6)

bool crystal_exist_status(void)
{
	struct regmap *map;
	unsigned int reg, mask, spar0;
	int ret;

	if (!md1_vrfdig_reg(&map, &reg, &mask)) {
		pr_warn("mtk-ccci: cannot read RTC crystal status: PMIC unavailable\n");
		return true;
	}
	/* LK records the crystal detection result in RTC_SPAR0, not DT. */
	ret = regmap_read(map, MT6359_RTC_SPAR0, &spar0);
	if (ret) {
		pr_warn("mtk-ccci: cannot read RTC crystal status: %d\n", ret);
		return true;
	}
	pr_info("mtk-ccci: RTC_SPAR0=%04x crystal=%u 32k-less=%u\n",
		spar0, !!(spar0 & MT6359_RTC_XTAL_PRESENT),
		!(spar0 & MT6359_RTC_XTAL_PRESENT));
	return !!(spar0 & MT6359_RTC_XTAL_PRESENT);
}

/*
 * Stock PMIC_LP_INIT_SETTING() (mt6359p pmic_initial_setting.c, MT6853):
 * the RF/MD rails are handed to the SRCLKEN lines so that they follow the
 * MD's clock request (SRCLKEN1) and the RF clock buffer (SRCLKEN14). The
 * mainline MT6359P driver never programs this; without it VRF18/VFE28/
 * VRF09/VRFCK/VBBCK stay off when the MD switches the transceiver on and
 * the MD asserts in DigRF (digrf_on_irq.c) right after RADIO_POWER on.
 * Only OP_EN/OP_CFG/OP_MODE are written; the SW enable bits are untouched.
 */
static bool md1_pmic_lp_en = true;
module_param_named(pmic_lp, md1_pmic_lp_en, bool, 0444);
MODULE_PARM_DESC(pmic_lp, "program the stock MT6359P SRCLKEN control of the MD/RF rails");

#define MT6359_HWCID		0x8
#define MT6359_TOP_CON		0x20
#define MT6359_DCXO_CW00	0x788
#define LP_CFG_SHIFT		6	/* OP_CFG = OP_EN + 6 */
#define LP_LDO_MODE_SHIFT	10	/* LDO CON1 op_mode bit = user + 10 */
#define LP_SRCLKEN0		0
#define LP_SRCLKEN1		1
#define LP_SRCLKEN2		2
#define LP_SRCLKEN14		14
#define LP_HW_OFF		0
#define LP_HW_LP		1

struct md1_pmic_lp {
	const char *name;
	u16 op_en;	/* OP_EN register */
	u16 mode;	/* buck OP_MODE / LDO CON1 */
	u16 mon;	/* buck DBG1 / LDO MON (bit0: DA enable) */
	u8 user;
	u8 op_mode;
	u8 cfg;
	bool buck;
};

static const struct md1_pmic_lp md1_pmic_lp_tbl[] = {
	{ "vpu",    0x1494, 0x14a0, 0x14a8, LP_SRCLKEN1,  1, LP_HW_LP,  true },
	{ "vmodem", 0x1694, 0x16a0, 0x16a8, LP_SRCLKEN1,  0, LP_HW_OFF, true },
	{ "vrf18",  0x1bb2, 0x1bae, 0x1bb0, LP_SRCLKEN1,  0, LP_HW_OFF, false },
	{ "vrf12",  0x1bc4, 0x1bc0, 0x1bc2, LP_SRCLKEN0,  1, LP_HW_LP,  false },
	{ "vrfck",  0x1d22, 0x1d1e, 0x1d20, LP_SRCLKEN14, 1, LP_HW_OFF, false },
	{ "vbbck",  0x1d34, 0x1d30, 0x1d32, LP_SRCLKEN14, 1, LP_HW_OFF, false },
	{ "vfe28",  0x1b8e, 0x1b8a, 0x1b8c, LP_SRCLKEN1,  0, LP_HW_OFF, false },
	{ "va09",   0x1c46, 0x1c42, 0x1c44, LP_SRCLKEN1,  1, LP_HW_LP,  false },
	{ "vxo22",  0x1ba0, 0x1b9c, 0x1b9e, LP_SRCLKEN0,  1, LP_HW_LP,  false },
	{ "va12",   0x1c6a, 0x1c66, 0x1c68, LP_SRCLKEN0,  1, LP_HW_LP,  false },
};

static void md1_pmic_lp_setting(struct regmap *map)
{
	const struct md1_pmic_lp *e;
	unsigned int id = 0, en0, cfg0, en1, cfg1, mon;
	int i;

	if (regmap_read(map, MT6359_HWCID, &id) || id < 0x5930) {
		mtk_ccci_log_err("pmic lp: HWCID %x is not MT6359P, skipped\n",
			id);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(md1_pmic_lp_tbl); i++) {
		e = &md1_pmic_lp_tbl[i];
		en0 = cfg0 = en1 = cfg1 = mon = 0;
		regmap_read(map, e->op_en, &en0);
		regmap_read(map, e->op_en + LP_CFG_SHIFT, &cfg0);
		if (e->buck)
			regmap_update_bits(map, e->mode, BIT(e->user),
				e->op_mode ? BIT(e->user) : 0);
		else if (e->user <= LP_SRCLKEN2)
			regmap_update_bits(map, e->mode,
				BIT(e->user + LP_LDO_MODE_SHIFT),
				e->op_mode ?
				BIT(e->user + LP_LDO_MODE_SHIFT) : 0);
		regmap_update_bits(map, e->op_en, BIT(e->user), BIT(e->user));
		regmap_update_bits(map, e->op_en + LP_CFG_SHIFT, BIT(e->user),
			e->cfg ? BIT(e->user) : 0);
		regmap_read(map, e->op_en, &en1);
		regmap_read(map, e->op_en + LP_CFG_SHIFT, &cfg1);
		regmap_read(map, e->mon, &mon);
		pr_info("mtk-ccci: pmic lp %s: user=%u op_en %04x->%04x op_cfg %04x->%04x mon=%04x\n",
			e->name, e->user, en0, en1, cfg0, cfg1, mon);
	}
}

/* read-only snapshot of what the MD's RF needs, for EE/boot-fail logs */
static void md1_pmic_rf_dump(struct regmap *map, const char *tag)
{
	unsigned int v[ARRAY_SIZE(md1_pmic_lp_tbl)], top = 0, xo = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(md1_pmic_lp_tbl); i++) {
		v[i] = 0;
		regmap_read(map, md1_pmic_lp_tbl[i].mon, &v[i]);
	}
	regmap_read(map, MT6359_TOP_CON, &top);
	regmap_read(map, MT6359_DCXO_CW00, &xo);
	pr_info("mtk-ccci: rf %s: top_con=%04x dcxo_cw00=%04x vpu=%04x vmodem=%04x vrf18=%04x vrf12=%04x vrfck=%04x vbbck=%04x vfe28=%04x va09=%04x\n",
		tag, top, xo, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/*
 * MT6315 (SPMI slave 3) VMODEM/VSRAM_MD: the MD DVFS moves them and the
 * restarted MD boots at whatever was left, so stock mt6315_vmd1_pmic_
 * setting_on() records the boot vosel and restores it before every power
 * on. 6.18 has no driver for the MT6853 PMIF; LK initialised it and the
 * kernel never gates its clocks, so the AP SWINF channel of the stock
 * driver (ap_swinf_no) is driven directly, with bounded polls.
 */
static bool md1_vosel_en = true;
module_param_named(md_vosel, md1_vosel_en, bool, 0444);
MODULE_PARM_DESC(md_vosel, "record/restore the MT6315 vmodem/vsram_md vosel over SPMI");

#define PMIF_INIT_DONE		0x000
#define PMIF_INF_EN		0x024
#define PMIF_SWINF_ACC(n)	(0xc00 + 0x40 * (n))
#define PMIF_SWINF_WDATA(n)	(0xc04 + 0x40 * (n))
#define PMIF_SWINF_RDATA(n)	(0xc14 + 0x40 * (n))
#define PMIF_SWINF_VLD_CLR(n)	(0xc24 + 0x40 * (n))
#define PMIF_SWINF_STA(n)	(0xc28 + 0x40 * (n))
#define PMIF_FSM(x)		(((x) >> 1) & 0x7)
#define PMIF_FSM_IDLE		0
#define PMIF_FSM_WFVLDCLR	6
#define PMIF_CMD_EXT_REG_LONG	3
#define PMIF_POLL_US		10000

#define MT6315_SID		3
#define MT6315_HWCID_H		0x0009
#define MT6315_VBUCK1_RG_VOSEL	0x1449	/* vmodem: BUCK_TOP_ELR0 */
#define MT6315_VBUCK4_RG_VOSEL	0x144f	/* vsram_md: BUCK_TOP_ELR6 */
#define MT6315_VBUCK1_DA_VOSEL	0x1499	/* BUCK_VBUCK1_DBG0 */
#define MT6315_VBUCK4_DA_VOSEL	0x1619	/* BUCK_VBUCK4_DBG0 */

static DEFINE_SPINLOCK(md1_spmi_lock);
static void __iomem *md1_pmif_base;
static unsigned int md1_pmif_ch;
static bool md1_pmif_failed;

static void __iomem *md1_pmif(void)
{
	struct device_node *np;
	void __iomem *base;
	u32 start = 4, no = 2, done, inf;

	if (md1_pmif_base || md1_pmif_failed)
		return md1_pmif_base;
	md1_pmif_failed = true;
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6853-pmif-m");
	if (!np) {
		mtk_ccci_log_err("spmi: no mt6853-pmif-m node\n");
		return NULL;
	}
	of_property_read_u32(np, "swinf_ch_start", &start);
	of_property_read_u32(np, "ap_swinf_no", &no);
	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base || no > 3 || start + no > 31) {
		mtk_ccci_log_err("spmi: pmif map failed (ch %u)\n", no);
		if (base)
			iounmap(base);
		return NULL;
	}
	done = readl(base + PMIF_INIT_DONE);
	inf = readl(base + PMIF_INF_EN);
	if (!(done & 1) || !(inf & BIT(start + no))) {
		mtk_ccci_log_err("spmi: pmif not ready init=%x inf_en=%x ch=%u\n",
			done, inf, no);
		iounmap(base);
		return NULL;
	}
	md1_pmif_ch = no;
	md1_pmif_base = base;
	md1_pmif_failed = false;
	mtk_ccci_log("spmi: pmif ready, swinf %u (inf_en=%x)\n", no, inf);
	return base;
}

static int md1_spmi_rw(bool write, u16 addr, u8 *val)
{
	void __iomem *p = md1_pmif();
	unsigned int ch = md1_pmif_ch;
	unsigned long flags;
	u32 sta = 0;
	int ret;

	if (!p)
		return -ENODEV;
	spin_lock_irqsave(&md1_spmi_lock, flags);
	ret = readl_poll_timeout_atomic(p + PMIF_SWINF_STA(ch), sta,
		PMIF_FSM(sta) == PMIF_FSM_IDLE, 1, PMIF_POLL_US);
	if (ret) {
		if (PMIF_FSM(sta) == PMIF_FSM_WFVLDCLR)
			writel(1, p + PMIF_SWINF_VLD_CLR(ch));
		goto out;
	}
	if (write)
		writel(*val, p + PMIF_SWINF_WDATA(ch));
	writel((PMIF_CMD_EXT_REG_LONG << 30) | ((write ? 1 : 0) << 29) |
		(MT6315_SID << 24) | addr, p + PMIF_SWINF_ACC(ch));
	if (write) {
		ret = readl_poll_timeout_atomic(p + PMIF_SWINF_STA(ch), sta,
			PMIF_FSM(sta) == PMIF_FSM_IDLE, 1, PMIF_POLL_US);
		goto out;
	}
	ret = readl_poll_timeout_atomic(p + PMIF_SWINF_STA(ch), sta,
		PMIF_FSM(sta) == PMIF_FSM_WFVLDCLR, 1, PMIF_POLL_US);
	if (!ret) {
		*val = readl(p + PMIF_SWINF_RDATA(ch)) & 0xff;
		writel(1, p + PMIF_SWINF_VLD_CLR(ch));
	}
out:
	spin_unlock_irqrestore(&md1_spmi_lock, flags);
	if (ret)
		mtk_ccci_log_err("spmi %s %04x timeout, swinf sta=%x\n",
			write ? "write" : "read", addr, sta);
	return ret;
}

static bool md1_mt6315_ok;
static u8 md1_vmodem_vosel, md1_vsram_md_vosel;

static void md1_mt6315_vosel(bool restore)
{
	u8 id = 0, vm = 0, vs = 0, vm1 = 0, vs1 = 0;

	if (!md1_vosel_en)
		return;
	if (!restore) {
		if (md1_spmi_rw(false, MT6315_HWCID_H, &id) || id != 0x15) {
			mtk_ccci_log_err("mt6315: HWCID_H %x, vosel restore off\n",
				id);
			return;
		}
		if (md1_spmi_rw(false, MT6315_VBUCK1_DA_VOSEL, &vm) ||
		    md1_spmi_rw(false, MT6315_VBUCK4_DA_VOSEL, &vs) || !vm)
			return;
		md1_vmodem_vosel = vm;
		md1_vsram_md_vosel = vs;
		md1_mt6315_ok = true;
		mtk_ccci_log("mt6315 vosel recorded: vmodem=%x vsram_md=%x\n",
			vm, vs);
		return;
	}
	if (!md1_mt6315_ok)
		return;
	md1_spmi_rw(false, MT6315_VBUCK1_DA_VOSEL, &vm);
	md1_spmi_rw(false, MT6315_VBUCK4_DA_VOSEL, &vs);
	vs1 = md1_vsram_md_vosel;
	vm1 = md1_vmodem_vosel;
	md1_spmi_rw(true, MT6315_VBUCK4_RG_VOSEL, &vs1);
	md1_spmi_rw(true, MT6315_VBUCK1_RG_VOSEL, &vm1);
	mtk_ccci_log("mt6315 vosel restore: vmodem %x -> %x, vsram_md %x -> %x\n",
		vm, md1_vmodem_vosel, vs, md1_vsram_md_vosel);
}

/*
 * Read-only DVFSRC/SPM snapshot. Stock 4.14 initialises the DVFSRC MD
 * bandwidth tables and kicks the SPM vcore-DVFS firmware via SMC; 6.18 has
 * no driver for that, so log what the hardware is doing for the MD.
 */
#define MD1_DVFSRC_BASE		0x10012000
#define MD1_SPM_BASE		0x10006000

static void md1_dvfs_dump(const char *tag)
{
	static void __iomem *dvfsrc, *spm;

	if (!dvfsrc)
		dvfsrc = ioremap(MD1_DVFSRC_BASE, 0x1000);
	if (!spm)
		spm = ioremap(MD1_SPM_BASE, 0x1000);
	if (!dvfsrc || !spm)
		return;
	pr_info("mtk-ccci: dvfs %s: basic=%08x qos_en=%08x sw_req3=%08x md_lat=%08x cur=%08x tgt=%08x dbg0=%08x force=%08x\n",
		tag, readl(dvfsrc + 0x0), readl(dvfsrc + 0x280),
		readl(dvfsrc + 0xc), readl(dvfsrc + 0x30c),
		readl(dvfsrc + 0xd44), readl(dvfsrc + 0xd48),
		readl(dvfsrc + 0x700), readl(dvfsrc + 0xd6c));
	pr_info("mtk-ccci: dvfs %s: spm pwron=%08x flag0=%08x pc_sta=%08x dvfs_lvl=%08x dvs_dfs=%08x dvfs_sta=%08x\n",
		tag, readl(spm + 0x0), readl(spm + 0x600), readl(spm + 0x194),
		readl(spm + 0x4a4), readl(spm + 0x4f8), readl(spm + 0x1f8));
}

/*
 * mtk_ccci.dvfsrc (default 0, applied once before the first MD power-on):
 *  bit0: if the bootloader left DVFSRC running, raise the AP software floor
 *        (DVFSRC_SW_REQ3) to the top DDR (level 6) and vcore (level 3) OPP,
 *        so MD L1 never runs on a low DDR/vcore OPP;
 *  bit1: if the SPM firmware is not running (SMC VCOREFS CMD_5 == 0), start
 *        it like stock mtk_spmfw_init(0, 1): CMD_0 then CMD_1 with
 *        RUN_COMMON_SCENARIO | DIS_VCORE_DVS | DIS_VCORE_DFS, i.e. the SPM
 *        MD sleep/srclken handling without any vcore/DDR switching (6.18 has
 *        no vcore pwrap table, so DVS/DFS must stay off).
 *  bit2: enable the DVFSRC like stock helio-dvfsrc-mt6853 platform_init
 *        (MD HRT/scenario bandwidth tables, level labels, QoS enable, then
 *        BASIC_CONTROL 0x6698444B -> 0x6698054B). The bootloader leaves it
 *        disabled (basic=00000008), so MD DDR/EMI level requests are never
 *        served. DVFSRC IRQ stays off (no handler).
 *  bit3: before bit2, re-run the stock SPM init sequence even if the SPM
 *        firmware already runs (stock mtk_spmfw_init(x, skip_check=1)):
 *        CMD_0 then CMD_1 with RUN_COMMON | DIS_VCORE_DVS | DIS_VCORE_DFS.
 * The SMC queries (CMD_5 status, CMD_6 type) are read-only and always logged.
 */
static int md1_dvfsrc_mode;
module_param_named(dvfsrc, md1_dvfsrc_mode, int, 0444);

#define MD1_SIP_SPM_VCOREFS	0xC2000226
#define MD1_SPM_FLAG_DIS_VCORE_DVS	BIT(3)
#define MD1_SPM_FLAG_DIS_VCORE_DFS	BIT(4)
#define MD1_SPM_FLAG_RUN_COMMON		BIT(10)

static unsigned long md1_vcorefs_smc(unsigned long cmd, unsigned long a1)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MD1_SIP_SPM_VCOREFS, cmd, a1, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

/* stock helio-dvfsrc-mt6853.c dvfsrc_init_configs[0], mtk_dvfsrc_reg_v2.h */
static const u32 md1_dvfsrc_init_tbl[][2] = {
	{ 0xA60, 0x0000001E },	/* HRT_REQ_UNIT */
	{ 0x314, 0x00001965 },	/* DEBOUNCE_TIME */
	{ 0x0F8, 0x00000015 },	/* TIMEOUT_NEXTREQ */
	{ 0x318, 0x000EE000 },	/* LEVEL_MASK */
	{ 0xA34, 0x00000019 },	/* DDR_QOS0 */
	{ 0xA38, 0x00000026 },
	{ 0xA3C, 0x00000033 },
	{ 0xA40, 0x0000003B },
	{ 0xA44, 0x0000004C },
	{ 0xD18, 0x00000066 },	/* DDR_QOS5 */
	{ 0xD1C, 0x00000066 },	/* DDR_QOS6 */
	{ 0x100, 0x50436053 },	/* LEVEL_LABEL_0_1 */
	{ 0x104, 0x40335042 },
	{ 0x108, 0x40314032 },
	{ 0x10C, 0x30223023 },
	{ 0x110, 0x20133021 },
	{ 0x114, 0x20112012 },
	{ 0x118, 0x10032010 },
	{ 0x11C, 0x10011002 },
	{ 0xD4C, 0x00131000 },	/* LEVEL_LABEL_16_17 */
	{ 0xD50, 0x00110012 },
	{ 0xD54, 0x00000010 },
	{ 0x30C, 0x00000040 },	/* MD_LATENCY_IMPROVE */
	{ 0x294, 0x00000004 },	/* HRT_BW_BASE */
	{ 0xA64, 0x000D20D2 },	/* HRT_REQ_MD_URG */
	{ 0xA68, 0x00200802 },	/* HRT_REQ_MD_BW_0..7 */
	{ 0xA6C, 0x00200802 },
	{ 0xA70, 0x00200800 },
	{ 0xA74, 0x00400802 },
	{ 0xA78, 0x00601404 },
	{ 0xA7C, 0x00D02C09 },
	{ 0xA80, 0x00000012 },
	{ 0xA84, 0x00000024 },
	{ 0xAA8, 0x00000000 },	/* HRT_REQ_MD_BW_8..10 */
	{ 0xAAC, 0x00000000 },
	{ 0xAB0, 0x00034800 },
	{ 0xA88, 0x04B12C4B },	/* HRT1_REQ_MD_BW_0..7 */
	{ 0xA8C, 0x04B12C4B },
	{ 0xA90, 0x04B12C00 },
	{ 0xA94, 0x04B12C4B },
	{ 0xA98, 0x04B12C4B },
	{ 0xA9C, 0x04B12C4B },
	{ 0xAA0, 0x0000004B },
	{ 0xAA4, 0x0000005C },
	{ 0xAB4, 0x00000000 },	/* HRT1_REQ_MD_BW_8..10 */
	{ 0xAB8, 0x00000000 },
	{ 0xABC, 0x00034800 },
	{ 0x534, 0x40444440 },	/* 95MD_SCEN_BW0_T..3_T */
	{ 0x538, 0x22244444 },
	{ 0x53C, 0x00400444 },
	{ 0x540, 0x60000000 },
	{ 0x524, 0x20222220 },	/* 95MD_SCEN_BW0..4 */
	{ 0x528, 0x00022222 },
	{ 0x52C, 0x00200222 },
	{ 0x530, 0x60000000 },
	{ 0x544, 0x00000006 },
	{ 0x614, 0x00000001 },	/* RSRV_5 */
	{ 0xA00, 0x00004321 },	/* DDR_REQUEST */
	{ 0xA08, 0x00000065 },	/* DDR_REQUEST3 */
	{ 0xAE0, 0x66543210 },	/* DDR_ADD_REQUEST */
	{ 0xAC4, 0x66654321 },	/* HRT_REQUEST */
	{ 0xA10, 0x54321000 },	/* DDR_REQUEST5 */
	{ 0xA18, 0x66000000 },	/* DDR_REQUEST7 */
	{ 0x308, 0x4C2D0000 },	/* EMI_MON_DEBOUNCE_TIME */
	{ 0xA14, 0x66543210 },	/* DDR_REQUEST6 */
	{ 0x0E4, 0x00010A29 },	/* VCORE_USER_REQ */
	{ 0xD38, 0x18A618A6 },	/* HRT_HIGH_3 */
	{ 0xAC8, 0x18A61183 },	/* HRT_HIGH_2 */
	{ 0xACC, 0x0D690B80 },	/* HRT_HIGH_1 */
	{ 0xAD0, 0x070804B0 },	/* HRT_HIGH */
	{ 0xD3C, 0x18A518A5 },	/* HRT_LOW_3 */
	{ 0xAD4, 0x18A51182 },	/* HRT_LOW_2 */
	{ 0xAD8, 0x0D680B7F },	/* HRT_LOW_1 */
	{ 0xADC, 0x070704AF },	/* HRT_LOW */
	{ 0x310, 0x00000006 },	/* BASIC_CONTROL_3 */
	{ 0x280, 0x0000407C },	/* QOS_EN */
	{ 0xD6C, 0x00000001 },	/* CURRENT_FORCE */
	{ 0x000, 0x6698444B },	/* BASIC_CONTROL */
	{ 0x000, 0x6698054B },
	{ 0xD6C, 0x00000000 },
};

static void md1_dvfsrc_enable(void __iomem *dvfsrc)
{
	unsigned int i;

	pr_info("mtk-ccci: dvfsrc: stock init, before basic=%08x qos_en=%08x cur=%08x\n",
		readl(dvfsrc + 0x0), readl(dvfsrc + 0x280),
		readl(dvfsrc + 0xd44));
	for (i = 0; i < ARRAY_SIZE(md1_dvfsrc_init_tbl); i++)
		writel(md1_dvfsrc_init_tbl[i][1],
			dvfsrc + md1_dvfsrc_init_tbl[i][0]);
	mdelay(5);
	pr_info("mtk-ccci: dvfsrc: stock init done, basic=%08x qos_en=%08x md_lat=%08x cur=%08x tgt=%08x dbg0=%08x md_scen=%08x\n",
		readl(dvfsrc + 0x0), readl(dvfsrc + 0x280),
		readl(dvfsrc + 0x30c), readl(dvfsrc + 0xd44),
		readl(dvfsrc + 0xd48), readl(dvfsrc + 0x700),
		readl(dvfsrc + 0xaec));
}

static void md1_dvfsrc_setup(void)
{
	static bool done;
	void __iomem *dvfsrc;
	unsigned long fw, type;
	u32 basic, req;

	if (done)
		return;
	done = true;

	fw = md1_vcorefs_smc(5, 0);
	type = md1_vcorefs_smc(6, 0);
	dvfsrc = ioremap(MD1_DVFSRC_BASE, 0x1000);
	if (!dvfsrc)
		return;
	basic = readl(dvfsrc + 0x0);
	pr_info("mtk-ccci: dvfsrc: spmfw status=%ld type=%ld basic=%08x rsrv4=%08x mode=%d\n",
		(long)fw, (long)type, basic, readl(dvfsrc + 0x610),
		md1_dvfsrc_mode);

	if ((md1_dvfsrc_mode & 2) && fw == 0) {
		md1_vcorefs_smc(0, 0);
		md1_vcorefs_smc(1, MD1_SPM_FLAG_RUN_COMMON |
			MD1_SPM_FLAG_DIS_VCORE_DVS |
			MD1_SPM_FLAG_DIS_VCORE_DFS);
		pr_info("mtk-ccci: dvfsrc: spmfw kicked (common scenario, no DVS/DFS), status now %ld\n",
			(long)md1_vcorefs_smc(5, 0));
	} else if (md1_dvfsrc_mode & 2) {
		pr_info("mtk-ccci: dvfsrc: spmfw kick skipped (status %ld)\n",
			(long)fw);
	}

	if ((md1_dvfsrc_mode & 8) && fw != 0) {
		md1_vcorefs_smc(0, 0);
		md1_vcorefs_smc(1, MD1_SPM_FLAG_RUN_COMMON |
			MD1_SPM_FLAG_DIS_VCORE_DVS |
			MD1_SPM_FLAG_DIS_VCORE_DFS);
		pr_info("mtk-ccci: dvfsrc: spmfw re-init (stock skip_check), status now %ld\n",
			(long)md1_vcorefs_smc(5, 0));
	}

	if (md1_dvfsrc_mode & 4) {
		if (fw == 0)
			pr_info("mtk-ccci: dvfsrc: spmfw not running, stock init skipped\n");
		else
			md1_dvfsrc_enable(dvfsrc);
	}

	if (md1_dvfsrc_mode & 1) {
		basic = readl(dvfsrc + 0x0);
		if ((basic & 0x101) != 0x101) {
			pr_info("mtk-ccci: dvfsrc: not running (basic=%08x), DDR/vcore stay at the boot OPP; pin skipped\n",
				basic);
		} else {
			req = readl(dvfsrc + 0xc);
			req &= ~((0x7 << 12) | (0x7 << 4));
			req |= (6 << 12) | (3 << 4);
			writel(req, dvfsrc + 0xc);
			pr_info("mtk-ccci: dvfsrc: AP floor pinned to max DDR/vcore, sw_req3=%08x\n",
				readl(dvfsrc + 0xc));
		}
	}
	iounmap(dvfsrc);
}

/*
 * MD sleep / 26 MHz resource chain: SPM src requests -> SRCLKEN_RC ->
 * MT6359 DCXO (XO_SOC/XO_CEL buffers).  Stock 4.14 keeps every CPU power-down
 * state blocked (PM QoS 2 us) until MCUPM is ready and uptime > 60 s, and its
 * lpm/MCUPM stack arbitrates mcusysoff/bus26m against the MD; 6.18 enters the
 * PSCI mcusysoff/s2idle states from boot without that stack.
 *
 * mtk_ccci.md_lpm (default 0), applied while MD1 is powered:
 *  bit0: CPU latency QoS 300 us: blocks the mcusysoff/s2idle cpuidle states
 *        (exit 1200/1400 us), keeps cpuoff/clusteroff;
 *  bit1: CPU latency QoS 2 us (the stock pre-ready value): blocks all CPU
 *        power-down states;
 *  bit2: reconfigure the SRCLKEN_RC channels in mtk_ccci.rc_force (default
 *        0x3: M00 SPM/suspend, M01 RF) per mtk_ccci.rc_mode: 0 = SW mode +
 *        FPM request (26 MHz full power always), 1 = HW mode (the subsystem's
 *        own srclken request is honoured); restored at MD power-off.
 * Writing anything to /sys/module/mtk_ccci/parameters/lpm_dump dumps the
 * chain on demand; mtk_ccci.md_mon=<s> (0 = off, min 10) logs a one-line
 * RC/DCXO/vmodem monitor every <s> seconds while MD1 is powered.
 */
static int md1_lpm_mode;
module_param_named(md_lpm, md1_lpm_mode, int, 0444);
MODULE_PARM_DESC(md_lpm, "MD sleep workarounds: bit0 qos 300us, bit1 qos 2us, bit2 srclken_rc channel override");
static unsigned int md1_rc_force_mask = 0x3;
module_param_named(rc_force, md1_rc_force_mask, uint, 0444);
MODULE_PARM_DESC(rc_force, "SRCLKEN_RC channel mask overridden by md_lpm bit2");
static int md1_rc_mode;
module_param_named(rc_mode, md1_rc_mode, int, 0444);
MODULE_PARM_DESC(rc_mode, "md_lpm bit2 override: 0 SW+FPM, 1 HW mode");

#define MD1_SRCLKEN_RC_BASE	0x1000f800
#define MD1_RC_CH_NUM		13
#define MD1_RC_SW_FPM		((1 << 3) | (1 << 4))
#define MD1_RC_SW_MSK		(0x7 << 3)
/* stock __subsys_ctl_show(): channel 0 status at +0x114, channel N>0 at +0x118 + 4N */
#define MD1_RC_STA(id)		(0x114 + 4 * ((id) ? (id) + 1 : 0))

static void __iomem *md1_rc_base;
static u32 md1_rc_saved[MD1_RC_CH_NUM];
static unsigned int md1_rc_forced;
static struct pm_qos_request md1_lpm_qos;

static void __iomem *md1_rc(void)
{
	struct device_node *np;

	if (md1_rc_base)
		return md1_rc_base;
	np = of_find_compatible_node(NULL, NULL, "mediatek,srclken");
	if (np) {
		md1_rc_base = of_iomap(np, 0);
		of_node_put(np);
	}
	if (!md1_rc_base)
		md1_rc_base = ioremap(MD1_SRCLKEN_RC_BASE, 0x1000);
	return md1_rc_base;
}

static void md1_lpm_dump(const char *tag)
{
	static void __iomem *spm;
	void __iomem *rc = md1_rc();
	static const u16 cw_reg[12] = { 0x788, 0x78e, 0x790, 0x792, 0x794,
		0x796, 0x798, 0x79a, 0x79c, 0x79e, 0x7a4, 0x7a6 };
	struct regmap *map;
	unsigned int reg, mask, cw[12] = { 0 };
	char buf[160];
	int i, n = 0;

	if (!spm)
		spm = ioremap(MD1_SPM_BASE, 0x1000);
	if (rc) {
		pr_info("mtk-ccci: lpm %s: rc cfg=%08x c1=%08x c2=%08x c3=%08x c4=%08x arb=%08x pmrc=%08x fpm=%08x misc=%08x spm_ctl=%08x intf=%08x\n",
			tag, readl(rc + 0x0), readl(rc + 0x4), readl(rc + 0x8),
			readl(rc + 0x1c), readl(rc + 0x58), readl(rc + 0xc),
			readl(rc + 0x10), readl(rc + 0x18), readl(rc + 0xb4),
			readl(rc + 0xb8), readl(rc + 0xbc));
		for (i = 0; i < MD1_RC_CH_NUM; i++)
			n += scnprintf(buf + n, sizeof(buf) - n, " %x",
				readl(rc + 0x20 + 4 * i));
		pr_info("mtk-ccci: lpm %s: rc m_cfg%s\n", tag, buf);
		n = 0;
		for (i = 0; i < MD1_RC_CH_NUM; i++)
			n += scnprintf(buf + n, sizeof(buf) - n, " %x",
				readl(rc + 0x114 + 4 * i));
		pr_info("mtk-ccci: lpm %s: rc fsm=%08x cmd=%08x/%08x spi=%08x pipo=%08x m_req%s\n",
			tag, readl(rc + 0x100), readl(rc + 0x104),
			readl(rc + 0x108), readl(rc + 0x10c), readl(rc + 0x110),
			buf);
	}
	if (spm) {
		pr_info("mtk-ccci: lpm %s: spm pwr_on1=%08x clk_con=%08x io_en=%08x src_req=%08x src_mask=%08x m2-7=%08x %08x %08x %08x %08x %08x\n",
			tag, readl(spm + 0x8), readl(spm + 0xc),
			readl(spm + 0x2c), readl(spm + 0xb8), readl(spm + 0xbc),
			readl(spm + 0xc0), readl(spm + 0xc4), readl(spm + 0xc8),
			readl(spm + 0xcc), readl(spm + 0xd0), readl(spm + 0xd8));
		pr_info("mtk-ccci: lpm %s: spm req_sta0-4=%08x %08x %08x %08x %08x idle=%08x pcm_sta=%08x wake=%08x evt vtcxo=%u infra=%u vrf18=%u apsrc=%u ddren=%u\n",
			tag, readl(spm + 0x114), readl(spm + 0x118),
			readl(spm + 0x11c), readl(spm + 0x168),
			readl(spm + 0x12c), readl(spm + 0x160),
			readl(spm + 0x164), readl(spm + 0x138),
			readl(spm + 0x17c), readl(spm + 0x180),
			readl(spm + 0x184), readl(spm + 0x188),
			readl(spm + 0x18c));
	}
	pr_info("mtk-ccci: lpm %s: infra md_srcclkena=%08x qos=%d rc_forced=%x\n",
		tag, infra_ao_base ?
		ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA) : 0,
		cpu_latency_qos_request_active(&md1_lpm_qos) ?
		(md1_lpm_mode & 2 ? 2 : 300) : -1, md1_rc_forced);
	if (!md1_vrfdig_reg(&map, &reg, &mask))
		return;
	for (i = 0; i < ARRAY_SIZE(cw); i++)
		regmap_read(map, cw_reg[i], &cw[i]);
	pr_info("mtk-ccci: lpm %s: dcxo cw00=%04x cw01-09,10,11=%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x\n",
		tag, cw[0], cw[1], cw[2], cw[3], cw[4], cw[5], cw[6], cw[7],
		cw[8], cw[9], cw[10], cw[11]);
}

static int md1_lpm_dump_set(const char *val, const struct kernel_param *kp)
{
	md1_lpm_dump("request");
	md1_dvfs_dump("request");
	return 0;
}

static const struct kernel_param_ops md1_lpm_dump_ops = {
	.set = md1_lpm_dump_set,
};
module_param_cb(lpm_dump, &md1_lpm_dump_ops, NULL, 0200);

static void md1_lpm_apply(bool on)
{
	void __iomem *rc;
	s32 lat = (md1_lpm_mode & 2) ? 2 : 300;
	int i;

	if (on && (md1_lpm_mode & 3)) {
		if (!cpu_latency_qos_request_active(&md1_lpm_qos))
			cpu_latency_qos_add_request(&md1_lpm_qos, lat);
		mtk_ccci_log("lpm: cpu latency qos %d us (deep idle blocked)\n",
			lat);
	} else if (!on && cpu_latency_qos_request_active(&md1_lpm_qos)) {
		cpu_latency_qos_remove_request(&md1_lpm_qos);
		mtk_ccci_log("lpm: cpu latency qos released\n");
	}

	rc = md1_rc();
	if (!rc)
		return;
	if (on && (md1_lpm_mode & 4) && !md1_rc_forced) {
		for (i = 0; i < MD1_RC_CH_NUM; i++) {
			u32 v;

			if (!(md1_rc_force_mask & BIT(i)))
				continue;
			v = readl(rc + 0x20 + 4 * i);
			md1_rc_saved[i] = v;
			writel((v & ~MD1_RC_SW_MSK) |
				(md1_rc_mode == 1 ? 0 : MD1_RC_SW_FPM),
				rc + 0x20 + 4 * i);
			md1_rc_forced |= BIT(i);
			mtk_ccci_log("lpm: rc m%02d cfg %x -> %x (%s) sta %x\n",
				i, v, readl(rc + 0x20 + 4 * i),
				md1_rc_mode == 1 ? "hw" : "sw-fpm",
				readl(rc + MD1_RC_STA(i)));
		}
	} else if (!on && md1_rc_forced) {
		for (i = 0; i < MD1_RC_CH_NUM; i++) {
			if (!(md1_rc_forced & BIT(i)))
				continue;
			writel(md1_rc_saved[i], rc + 0x20 + 4 * i);
			mtk_ccci_log("lpm: rc m%02d cfg restored %x\n", i,
				readl(rc + 0x20 + 4 * i));
		}
		md1_rc_forced = 0;
	}
}

/*
 * md_mon: one line per period while MD1 is powered.  rc chN: FPM req/ack,
 * BBLPM req/ack and current/target DCXO mode of M00 (suspend), M01 (RF)
 * and M03 (MD); cdac = DCXO CW04 (XO_CDAC_FPM low byte, LPM high byte):
 * a constant value while searching means the MD never trims the 26 MHz.
 */
static unsigned int md1_mon_period;
static bool md1_mon_on;
static unsigned int md1_mon_seq;

/*
 * MTS (Modem Temp Share): the MT6359 AUXADC "MDRT" engine periodically
 * converts the 26 MHz crystal thermistor (TSX, ch7) and DCXO temperature
 * for the MD, which uses them for its digital AFC.  The preloader sets it
 * up; mainline mt6359-auxadc resets the whole AUXADC (HK_TOP_RST_CON0 0x9)
 * at probe and, unlike stock auxadc_reset(), never re-requests ch7 and the
 * DCXO channel afterwards, which stock relies on to restart MDRT.
 * mt6359-auxadc now restores this itself; kept as a fallback.
 * mtk_ccci.mdrt (default 3): bit0 = at MD power-on (and from md_mon when the
 * MDRT output stops changing) issue the stock post-reset requests
 * (AUXADC_RQST0 bit7, AUXADC_RQST1 bit6); bit1 = also set
 * AUXADC_MDRT_0.MDRT_DET_EN if it is clear.
 */
static int md1_mdrt_mode = 3;
module_param_named(mdrt, md1_mdrt_mode, int, 0644);
MODULE_PARM_DESC(mdrt, "MT6359 AUXADC MDRT restart: bit0 stock post-reset requests, bit1 force MDRT_DET_EN");

#define MT6359_AUXADC_ADC16		0x10a4	/* ch7 (TSX) by MD */
#define MT6359_AUXADC_ADC33		0x10be	/* DCXO by MD */
#define MT6359_AUXADC_RQST0		0x1108
#define MT6359_AUXADC_RQST1		0x110a
#define MT6359_AUXADC_MDRT_0		0x125e	/* [15] DET_EN, [1:0] PRD_SEL */
#define MT6359_AUXADC_MDRT_2		0x1262
#define MT6359_AUXADC_MDRT_4		0x1266	/* [15] RDY, [14:0] OUT */
#define MT6359_AUXADC_DCXO_MDRT_3	0x126e	/* [15] RDY, [14:0] OUT */

static unsigned int md1_mdrt_last = ~0U, md1_mdrt_same;

static void md1_mdrt_kick(struct regmap *map, const char *why)
{
	unsigned int m0 = 0;

	if (!(md1_mdrt_mode & 3))
		return;
	if (md1_mdrt_mode & 2) {
		regmap_read(map, MT6359_AUXADC_MDRT_0, &m0);
		if (!(m0 & BIT(15)))
			regmap_update_bits(map, MT6359_AUXADC_MDRT_0, BIT(15),
				BIT(15));
	}
	regmap_update_bits(map, MT6359_AUXADC_RQST0, 0x80, 0x80);
	regmap_update_bits(map, MT6359_AUXADC_RQST1, 0x40, 0x40);
	regmap_read(map, MT6359_AUXADC_MDRT_0, &m0);
	pr_info_ratelimited("mtk-ccci: mdrt kick (%s): mdrt0=%04x\n", why, m0);
}

static void md1_mdrt_dump(struct regmap *map, const char *tag, char *out,
	size_t len)
{
	unsigned int m0 = 0, m2 = 0, m4 = 0, d3 = 0, a16 = 0, a33 = 0;

	regmap_read(map, MT6359_AUXADC_MDRT_0, &m0);
	regmap_read(map, MT6359_AUXADC_MDRT_2, &m2);
	regmap_read(map, MT6359_AUXADC_MDRT_4, &m4);
	regmap_read(map, MT6359_AUXADC_DCXO_MDRT_3, &d3);
	regmap_read(map, MT6359_AUXADC_ADC16, &a16);
	regmap_read(map, MT6359_AUXADC_ADC33, &a33);
	scnprintf(out, len, "mdrt0=%04x mdrt2=%04x mdrt=%04x dcxo_mdrt=%04x tsx_md=%04x dcxo_md=%04x",
		m0, m2, m4, d3, a16, a33);
	if (!tag)
		return;
	if (m4 == md1_mdrt_last) {
		if (++md1_mdrt_same == 3) {
			pr_info("mtk-ccci: mdrt output stuck at %04x for 3 samples\n",
				m4);
			md1_mdrt_kick(map, "stuck");
			md1_mdrt_same = 0;
		}
	} else {
		md1_mdrt_same = 0;
	}
	md1_mdrt_last = m4;
}

static void md1_mon_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(md1_mon_work, md1_mon_fn);

static unsigned long md1_mon_delay(void)
{
	return (unsigned long)max(md1_mon_period, 10U) * HZ;
}

static int md1_pinchk(const char *why);

static void md1_mon_line(void)
{
	static void __iomem *spm;
	static const u8 ch[3] = { 0, 1, 3 };
	void __iomem *rc = md1_rc();
	struct regmap *map;
	unsigned int reg, mask, cw0 = 0, cw4 = 0, cw5 = 0;
	u8 vm = 0, vs = 0;
	char buf[128], mb[96] = "mdrt=?";
	int i, n = 0;

	if (!spm)
		spm = ioremap(MD1_SPM_BASE, 0x1000);
	for (i = 0; rc && i < ARRAY_SIZE(ch); i++) {
		u32 s = readl(rc + MD1_RC_STA(ch[i]));

		n += scnprintf(buf + n, sizeof(buf) - n,
			" m%u=%x fpm%u/%u bb%u/%u dcxo%u>%u", ch[i],
			readl(rc + 0x20 + 4 * ch[i]) & 0xff, s & 1,
			(s >> 1) & 1, (s >> 2) & 1, (s >> 3) & 1,
			(s >> 7) & 7, (s >> 10) & 7);
	}
	if (md1_vrfdig_reg(&map, &reg, &mask)) {
		regmap_read(map, 0x788, &cw0);
		regmap_read(map, 0x794, &cw4);
		regmap_read(map, 0x796, &cw5);
		md1_mdrt_dump(map, "mon", mb, sizeof(mb));
		md1_pinchk("mon");
	}
	if (md1_mt6315_ok) {
		md1_spmi_rw(false, MT6315_VBUCK1_DA_VOSEL, &vm);
		md1_spmi_rw(false, MT6315_VBUCK4_DA_VOSEL, &vs);
	}
	pr_info("mtk-ccci: mon #%u:%s cw00=%04x cdac=%04x cw05=%04x src_req=%08x md_srcclkena=%08x vmodem=%x vsram_md=%x %s\n",
		md1_mon_seq++, rc ? buf : " rc=?", cw0, cw4, cw5,
		spm ? readl(spm + 0xb8) : 0,
		infra_ao_base ?
		ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA) : 0, vm, vs,
		mb);
}

static void md1_mon_fn(struct work_struct *work)
{
	if (!READ_ONCE(md1_mon_on) || !md1_mon_period)
		return;
	md1_mon_line();
	if (READ_ONCE(md1_mon_on))
		schedule_delayed_work(&md1_mon_work, md1_mon_delay());
}

/*
 * The MD drives its RF front end (BPI bus, MIPI RFFE, antenna switches) and
 * SRCLKENA through pins the preloader muxes from the DWS table, which the
 * board DT carries as gpio_init_default (7 cells per pin: pin, mode, dir,
 * dout, pullen, pullsel, smt).  If any AP driver re-muxes one of them the
 * MD sees a cell and then loses it once it switches the front end.
 * mtk_ccci.pinchk: bit0 (default) = log pins whose live mode/dir/dout
 * differs from the table; bit1 = restore the DT mode of mismatched MD pins
 * (143/144 SRCLKENA0/1 and 162..191 MIPI/BPI).
 */
static int md1_pinchk_mode = 1;
module_param_named(pinchk, md1_pinchk_mode, int, 0644);
MODULE_PARM_DESC(pinchk, "bit0 log gpio_init_default mismatches, bit1 restore MD RF pin modes");

#define MD1_GPIO_BASE		0x10005000
#define MD1_GPIO_NPINS		212

static bool md1_pin_is_md(u32 pin)
{
	return pin == 143 || pin == 144 || (pin >= 162 && pin <= 191);
}

static int md1_pinchk(const char *why)
{
	static void __iomem *gpio;
	static int last_bad = -1;
	struct device_node *np;
	const __be32 *p;
	int len, i, bad = 0, fixed = 0;

	if (!(md1_pinchk_mode & 3))
		return 0;
	np = of_find_compatible_node(NULL, NULL, "mediatek,gpio");
	if (!np)
		return -ENODEV;
	p = of_get_property(np, "gpio_init_default", &len);
	if (!p || len < 7 * 4) {
		of_node_put(np);
		if (last_bad != -2)
			pr_info("mtk-ccci: pinchk (%s): no gpio_init_default\n",
				why);
		last_bad = -2;
		return -ENOENT;
	}
	if (!gpio)
		gpio = ioremap(MD1_GPIO_BASE, 0x1000);
	if (!gpio) {
		of_node_put(np);
		return -ENOMEM;
	}
	for (i = 0; i + 7 <= len / 4; i += 7) {
		u32 pin = be32_to_cpu(p[i]), mode = be32_to_cpu(p[i + 1]);
		u32 dir = be32_to_cpu(p[i + 2]), dout = be32_to_cpu(p[i + 3]);
		u32 moff = 0x300 + 0x10 * (pin / 8), msh = (pin % 8) * 4;
		u32 boff = 0x10 * (pin / 32), bit = BIT(pin % 32);
		u32 cmode, cdir, cdout;

		if (pin >= MD1_GPIO_NPINS)
			continue;
		cmode = (readl(gpio + moff) >> msh) & 0xf;
		cdir = !!(readl(gpio + boff) & bit);
		cdout = !!(readl(gpio + 0x100 + boff) & bit);
		if (cmode == mode && (mode || dir != 1 ||
		    (cdir == 1 && cdout == dout)))
			continue;
		bad++;
		if (last_bad == -1 || (md1_pin_is_md(pin) && strcmp(why, "mon")))
			pr_info_ratelimited("mtk-ccci: pinchk (%s): gpio%u mode %u (dt %u) dir %u (dt %u) dout %u (dt %u)%s\n",
				why, pin, cmode, mode, cdir, dir, cdout, dout,
				md1_pin_is_md(pin) ? " MD" : "");
		if ((md1_pinchk_mode & 2) && md1_pin_is_md(pin) &&
		    cmode != mode) {
			writel((readl(gpio + moff) & ~(0xf << msh)) |
				(mode << msh), gpio + moff);
			fixed++;
		}
	}
	of_node_put(np);
	if (bad != last_bad || fixed)
		pr_info("mtk-ccci: pinchk (%s): %d pins differ from gpio_init_default, %d MD pins restored\n",
			why, bad, fixed);
	last_bad = bad;
	return bad;
}

static void md1_mdrt_power_on(void)
{
	struct regmap *map;
	unsigned int reg, mask;
	char mb[96];

	if (!md1_vrfdig_reg(&map, &reg, &mask))
		return;
	md1_mdrt_dump(map, NULL, mb, sizeof(mb));
	pr_info("mtk-ccci: mdrt power-on: %s mode=%d\n", mb, md1_mdrt_mode);
	md1_mdrt_last = ~0U;
	md1_mdrt_same = 0;
	if (md1_mdrt_mode & 3) {
		md1_mdrt_kick(map, "power-on");
		md1_mdrt_dump(map, NULL, mb, sizeof(mb));
		pr_info("mtk-ccci: mdrt after kick: %s\n", mb);
	}
}

static void md1_mon_set_state(bool on)
{
	WRITE_ONCE(md1_mon_on, on);
	if (on) {
		md1_mon_seq = 0;
		if (md1_mon_period)
			mod_delayed_work(system_wq, &md1_mon_work,
				md1_mon_delay());
	} else {
		/* not _sync: power-off may hold locks the dump path takes */
		cancel_delayed_work(&md1_mon_work);
	}
}

static int md1_mon_param_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret && md1_mon_period && READ_ONCE(md1_mon_on))
		mod_delayed_work(system_wq, &md1_mon_work, 0);
	return ret;
}

static const struct kernel_param_ops md1_mon_param_ops = {
	.set = md1_mon_param_set,
	.get = param_get_uint,
};
module_param_cb(md_mon, &md1_mon_param_ops, &md1_mon_period, 0644);
MODULE_PARM_DESC(md_mon, "periodic MD clock/power monitor interval in s (0 off, min 10)");

void mtk_ccci_md_rf_dump(const char *tag)
{
	struct regmap *map;
	unsigned int reg, mask;
	u8 vm = 0, vs = 0;

	if (!mtk_ccci_power)
		return;
	md1_dvfs_dump(tag);
	md1_lpm_dump(tag);
	if (!md1_vrfdig_reg(&map, &reg, &mask))
		return;
	md1_pmic_rf_dump(map, tag);
	if (md1_mt6315_ok &&
	    !md1_spmi_rw(false, MT6315_VBUCK1_DA_VOSEL, &vm) &&
	    !md1_spmi_rw(false, MT6315_VBUCK4_DA_VOSEL, &vs))
		pr_info("mtk-ccci: rf %s: mt6315 vmodem=%x vsram_md=%x\n",
			tag, vm, vs);
}

void md1_pmic_setting_on(void)
{
	static bool lp_done;
	struct regmap *map;
	unsigned int reg, mask, val = 0;

	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] vmd1_pmic_setting_on() start.\n");
	md1_dvfsrc_setup();
	if (!md1_vrfdig_reg(&map, &reg, &mask))
		return;
	if (!lp_done) {
		lp_done = true;
		if (md1_pmic_lp_en)
			md1_pmic_lp_setting(map);
		else
			mtk_ccci_log("pmic lp: skipped (mtk_ccci.pmic_lp=0)\n");
	}
	if (!md1_vrfdig_vosel) {
		if (!regmap_read(map, reg, &val))
			md1_vrfdig_vosel = val & mask;
		mtk_ccci_log("vrfdig vosel recorded: reg=%x val=%x\n", reg,
			md1_vrfdig_vosel);
		md1_mt6315_vosel(false);
		md1_pmic_rf_dump(map, "power-on");
		md1_dvfs_dump("power-on");
		return;
	}
	regmap_read(map, reg, &val);
	regmap_update_bits(map, reg, mask, md1_vrfdig_vosel);
	mtk_ccci_log("vrfdig vosel restore: reg=%x %x -> %x\n", reg,
		val & mask, md1_vrfdig_vosel);
	md1_mt6315_vosel(true);
	md1_pmic_rf_dump(map, "power-on");
	md1_dvfs_dump("power-on");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] vmd1_pmic_setting_on() end.\n");
}

/* VMD1/VMODEM/VSRAM_MD are owned by the MD power domain and regulators. */
void md1_pmic_setting_off(void)
{
}

/* callback for system power off*/
void ccci_power_off(void)
{
	md1_pmic_setting_on();
}

void __attribute__((weak)) kicker_pbm_by_md(enum pbm_kicker kicker,
	bool status)
{
	CCCI_NORMAL_LOG(-1, TAG, "kicker_pbm_by_md() no support.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "kicker_pbm_by_md() no support.\n");
}

#ifdef FEATURE_CLK_BUF
static void flight_mode_set_by_atf(struct ccci_modem *md,
		unsigned int flightMode)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_FLIGHT_MODE_SET,
		flightMode, 0, 0, 0, 0, 0, &res);

	CCCI_BOOTUP_LOG(md->index, TAG,
		"[%s] flag_1=%lu, flag_2=%lu, flag_3=%lu, flag_4=%lu\n",
		__func__, res.a0, res.a1, res.a2, res.a3);
}
#endif

int md_cd_soft_power_off(struct ccci_modem *md, unsigned int mode)
{
	if (!mtk_ccci_power)
		return 0;
#ifdef FEATURE_CLK_BUF
	flight_mode_set_by_atf(md, true);
#endif
	return 0;
}

int md_cd_soft_power_on(struct ccci_modem *md, unsigned int mode)
{
	if (!mtk_ccci_power)
		return 0;
#ifdef FEATURE_CLK_BUF
	flight_mode_set_by_atf(md, false);
#endif
	return 0;
}

int md_start_platform(struct ccci_modem *md)
{
	struct arm_smccc_res res;
	int timeout = 100; /* 100 * 20ms = 2s */
	unsigned long t0 = jiffies;
	bool was_on;
	int ret = -1;

	s_md_start_completed = 1;

	if ((md->per_md_data.config.setting&MD_SETTING_FIRST_BOOT) == 0)
		return 0;

	/*
	 * As on 4.14, LK leaves MD1 powered and running its BROM (LK log:
	 * "MD1 MTCMOS power on done!", "Trigger MD run"); the power-on below
	 * is a no-op then. If MD1 is found off, it is powered so that ATF
	 * can be asked, although the BROM check is then expected to fail.
	 */
	was_on = mtk_ccci_md_pwr_is_on();
	mtk_ccci_log("first boot: MD1 %s at start, checking BROM\n",
		was_on ? "on" : "off");
	ret = mtk_ccci_md_pwr_on();
	if (ret) {
		CCCI_ERROR_LOG(md->index, TAG, "BROM power on fail %d\n", ret);
		mtk_ccci_log_err("BROM check: MD1 power-on failed %d\n", ret);
		return ret;
	}

	while (timeout > 0) {
		ret = mt_secure_call(MD_POWER_CONFIG, MD_CHECK_DONE,
			0, 0, 0, 0, 0);
		if (!ret) {
			CCCI_BOOTUP_LOG(md->index, TAG, "BROM PASS\n");
			break;
		}
		timeout--;
		msleep(20);
	}
	if (ret)
		mtk_ccci_log_err("BROM FAIL ret=%d after %u ms (MD1 was %s)\n",
			ret, jiffies_to_msecs(jiffies - t0),
			was_on ? "on" : "off");
	else
		mtk_ccci_log("BROM PASS after %u ms\n",
			jiffies_to_msecs(jiffies - t0));
	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_POWER_CONFIG,
		MD_CHECK_FLAG, 0, 0, 0, 0, 0, &res);
	CCCI_BOOTUP_LOG(md->index, TAG,
		"flag_1=%lu, flag_2=%lu, flag_3=%lu, flag_4=%lu\n",
		res.a0, res.a1, res.a2, res.a3);
	mtk_ccci_log("MD_CHECK_FLAG: %lx %lx %lx %lx\n",
		res.a0, res.a1, res.a2, res.a3);

	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_POWER_CONFIG,
		MD_BOOT_STATUS, 0, 0, 0, 0, 0, &res);
	CCCI_BOOTUP_LOG(md->index, TAG,
		"AP: boot_ret=%lu, boot_status_0=%lX, boot_status_1=%lX\n",
		res.a0, res.a1, res.a2);
	mtk_ccci_log("MD_BOOT_STATUS: ret=%lu status=%lx/%lx\n",
		res.a0, res.a1, res.a2);

	if (ret != 0) {
		/* BROM */
		CCCI_ERROR_LOG(md->index, TAG, "BROM Failed\n");
	}
	/* real off/on cycle: md_cd_power_on() then boots MD1 from reset */
	md_cd_power_off(md, 0);
	return ret;
}


static int mtk_ccci_cfg_srclken_o1_on(struct ccci_modem *md)
{
	unsigned int val;
	struct md_hw_info *hw_info = md->hw_info;

	if (hw_info->spm_sleep_base) {
		CCCI_NORMAL_LOG(-1, "POWER ON", "%s() start.\n", __func__);
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] %s() start.\n", __func__);

		ccci_write32(hw_info->spm_sleep_base, 0, 0x0B160001);
		val = ccci_read32(hw_info->spm_sleep_base, 0);
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] spm_sleep_base: val:0x%x\n", val);


		val = ccci_read32(hw_info->spm_sleep_base, 8);
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] spm_sleep_base+8: val:0x%x +\n", val);
#ifdef CCCI_PLATFORM_MT6877
		val |= 0x1<<15;
#else
		val |= 0x1<<21;
#endif
		ccci_write32(hw_info->spm_sleep_base, 8, val);
		val = ccci_read32(hw_info->spm_sleep_base, 8);
		CCCI_NORMAL_LOG(-1, "POWER ON", "spm_sleep_base+8: val:0x%x -\n", val);
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] spm_sleep_base+8: val:0x%x -\n", val);
		CCCI_NORMAL_LOG(-1, "POWER ON", "%s() end.\n", __func__);
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] %s() end.\n", __func__);
	}
	return 0;
}


static int md_cd_topclkgen_on(struct ccci_modem *md)
{
	unsigned int reg_value;

	reg_value = ccci_read32(md->hw_info->ap_topclkgen_base, 0);
	reg_value &= ~((1<<8) | (1<<9));
	ccci_write32(md->hw_info->ap_topclkgen_base, 0, reg_value);

	CCCI_NORMAL_LOG(md->index, "POWER ON", "%s: set md1_clk_mod = 0x%x\n",
		__func__, ccci_read32(md->hw_info->ap_topclkgen_base, 0));
	CCCI_BOOTUP_LOG(md->index, TAG, "[POWER ON] %s: set md1_clk_mod = 0x%x\n",
		__func__, ccci_read32(md->hw_info->ap_topclkgen_base, 0));
	return 0;
}

int md_cd_power_on(struct ccci_modem *md)
{
	int ret = 0;
#ifndef CCCI_PLATFORM_MT6877
	unsigned int reg_value;
#endif
	if (!mtk_ccci_power)
		return -EPERM;
	/* step 1: PMIC setting */
	md1_pmic_setting_on();

	/* modem topclkgen on setting */
	md_cd_topclkgen_on(md);

#ifndef CCCI_PLATFORM_MT6877
	/* step 2: MD srcclkena setting */
	CCCI_NORMAL_LOG(-1, "POWER ON", "set md1 srcclkena start.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] set md1 srcclkena start.\n");
	reg_value = ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA);
	reg_value &= ~(0xFF);
	reg_value |= 0x21;
	ccci_write32(infra_ao_base, INFRA_AO_MD_SRCCLKENA, reg_value);
	CCCI_NORMAL_LOG(-1, "POWER ON",
		"set md1 srcclkena end. bit(0x1000_0F0C)=0x%x\n",
		ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA));
	CCCI_BOOTUP_LOG(-1, TAG,
		"[POWER ON] set md1 srcclkena end. bit(0x1000_0F0C)=0x%x\n",
		ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA));
#endif
	mtk_ccci_cfg_srclken_o1_on(md);
	/* steip 3: power on MD_INFRA and MODEM_TOP */
	switch (md->index) {
	case MD_SYS1:
#ifdef FEATURE_CLK_BUF
		flight_mode_set_by_atf(md, false);
#endif
		CCCI_NORMAL_LOG(-1, "POWER ON", "clk_prepare_enable() start.\n");
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] clk_prepare_enable() start.\n");
		md1_lpm_apply(true);
		md1_lpm_dump("power-on");
		md1_mdrt_power_on();
		md1_pinchk("power-on");
		md1_mon_set_state(true);
		ret = mtk_ccci_md_pwr_on();
		CCCI_NORMAL_LOG(-1, "POWER ON",
			"clk_prepare_enable() end. ret=%d\n", ret);
		CCCI_BOOTUP_LOG(-1, TAG,
			"[POWER ON] clk_prepare_enable() end. ret=%d\n", ret);
		kicker_pbm_by_md(KR_MD1, true);

		break;
	}
	mtk_ccci_log("md_cd_power_on: ret=%d srcclkena=%x md1_pwr=%d\n", ret,
		ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA),
		mtk_ccci_md_pwr_is_on());
	if (ret)
		return ret;

#ifdef FEATURE_INFORM_NFC_VSIM_CHANGE
	/* notify NFC */
	CCCI_NORMAL_LOG(-1, "POWER ON", "inform_nfc_vsim_change() start.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] inform_nfc_vsim_change() start.\n");
	inform_nfc_vsim_change(md->index, 1, 0);
	CCCI_NORMAL_LOG(-1, "POWER ON", "inform_nfc_vsim_change() end.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] inform_nfc_vsim_change() end.\n");
#endif
	return 0;
}

int md_cd_bootup_cleanup(struct ccci_modem *md, int success)
{
	return 0;
}

int md_cd_let_md_go(struct ccci_modem *md)
{
	struct arm_smccc_res res;

	CCCI_NORMAL_LOG(-1, "POWER ON", "%s() start.\n", __func__);
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] %s() start.\n", __func__);

	if (MD_IN_DEBUG(md))
		return -1;

	/* make boot vector take effect */
	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_POWER_CONFIG,
		MD_KERNEL_BOOT_UP, 0, 0, 0, 0, 0, &res);
	CCCI_NORMAL_LOG(md->index, "POWER ON",
		"MD: boot_ret=%lu, boot_status_0=%lu, boot_status_1=%lu\n",
		res.a0, res.a1, res.a2);
	CCCI_BOOTUP_LOG(md->index, TAG,
		"[POWER ON] MD: boot_ret=%lu, boot_status_0=%lu, boot_status_1=%lu\n",
		res.a0, res.a1, res.a2);
	mtk_ccci_log("MD_KERNEL_BOOT_UP: ret=%lu status=%lx/%lx\n",
		res.a0, res.a1, res.a2);

	CCCI_NORMAL_LOG(-1, "POWER ON", "%s() end.\n", __func__);
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER ON] %s() end.\n", __func__);

	return 0;
}

static int md_cd_topclkgen_off(struct ccci_modem *md)
{
	unsigned int reg_value;

	reg_value = ccci_read32(md->hw_info->ap_topclkgen_base, 0);
	reg_value |= ((1<<8) | (1<<9));
	ccci_write32(md->hw_info->ap_topclkgen_base, 0, reg_value);

	CCCI_NORMAL_LOG(md->index, "POWER OFF", "%s: set md1_clk_mod = 0x%x\n",
		__func__, ccci_read32(md->hw_info->ap_topclkgen_base, 0));
	CCCI_BOOTUP_LOG(md->index, TAG, "[POWER OFF] %s: set md1_clk_mod = 0x%x\n",
		__func__, ccci_read32(md->hw_info->ap_topclkgen_base, 0));

	return 0;
}

int md_cd_power_off(struct ccci_modem *md, unsigned int timeout)
{
	int ret = 0;
#ifndef CCCI_PLATFORM_MT6877
	unsigned int reg_value;
#endif

	if (!mtk_ccci_power)
		return 0;
#ifdef FEATURE_INFORM_NFC_VSIM_CHANGE
	/* notify NFC */
	CCCI_NORMAL_LOG(-1, "POWER OFF", "inform_nfc_vsim_change() start.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] inform_nfc_vsim_change() start.\n");
	inform_nfc_vsim_change(md->index, 0, 0);
	CCCI_NORMAL_LOG(-1, "POWER OFF", "inform_nfc_vsim_change() end.\n");
	CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] inform_nfc_vsim_change() end.\n");
#endif

	/* power off MD_INFRA and MODEM_TOP */
	switch (md->index) {
	case MD_SYS1:
		/* 1. power off MD MTCMOS */
		CCCI_NORMAL_LOG(-1, "POWER OFF", "clk_disable_unprepare() start.\n");
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] clk_disable_unprepare() start.\n");
		ret = mtk_ccci_md_pwr_off();
		CCCI_NORMAL_LOG(-1, "POWER OFF", "clk_disable_unprepare() end.\n");
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] clk_disable_unprepare() end.\n");
		if (ret) {
			/* MD1 still runs: keep its clock request and clocks */
			mtk_ccci_log_err("md_cd_power_off: MD1 left on (%d)\n",
				ret);
			break;
		}
		/* 2. disable srcclkena */
#ifndef CCCI_PLATFORM_MT6877
		CCCI_NORMAL_LOG(-1, "POWER OFF", "set infra_ao_base start.\n");
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] set infra_ao_base start.\n");
		reg_value = ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA);
		reg_value &= ~(0xFF);
		ccci_write32(infra_ao_base, INFRA_AO_MD_SRCCLKENA, reg_value);
		CCCI_NORMAL_LOG(md->index, "POWER OFF",
			"%s: set md1_srcclkena=0x%x\n", __func__,
			ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA));
		CCCI_BOOTUP_LOG(md->index, TAG,
			"[POWER OFF] %s: set md1_srcclkena=0x%x\n", __func__,
			ccci_read32(infra_ao_base, INFRA_AO_MD_SRCCLKENA));
		CCCI_NORMAL_LOG(-1, "POWER OFF", "set infra_ao_base end.\n");
		CCCI_BOOTUP_LOG(-1, TAG, "[POWER OFF] set infra_ao_base end.\n");
#endif

#ifdef FEATURE_CLK_BUF
		flight_mode_set_by_atf(md, true);
#endif
		/* modem topclkgen off setting */
		md_cd_topclkgen_off(md);

		/* 3. PMIC off */
		md1_pmic_setting_off();
		md1_mon_set_state(false);
		md1_lpm_apply(false);

		/* 5. DLPT */
		kicker_pbm_by_md(KR_MD1, false);

		break;
	}
	return ret;
}

void ccci_modem_remove(struct platform_device *dev)
{
}

void ccci_modem_shutdown(struct platform_device *dev)
{
}

int ccci_modem_suspend(struct platform_device *dev, pm_message_t state)
{
	struct ccci_modem *md = (struct ccci_modem *)dev->dev.platform_data;

	CCCI_DEBUG_LOG(md->index, TAG, "%s\n", __func__);
	return 0;
}

int ccci_modem_resume(struct platform_device *dev)
{
	struct ccci_modem *md = (struct ccci_modem *)dev->dev.platform_data;

	CCCI_DEBUG_LOG(md->index, TAG, "%s\n", __func__);
	return 0;
}

int ccci_modem_pm_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	if (pdev == NULL) {
		CCCI_ERROR_LOG(MD_SYS1, TAG, "%s pdev == NULL\n", __func__);
		return -1;
	}
	return ccci_modem_suspend(pdev, PMSG_SUSPEND);
}

int ccci_modem_pm_resume(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	if (pdev == NULL) {
		CCCI_ERROR_LOG(MD_SYS1, TAG, "%s pdev == NULL\n", __func__);
		return -1;
	}
	return ccci_modem_resume(pdev);
}

int ccci_modem_pm_restore_noirq(struct device *device)
{
	struct ccci_modem *md = (struct ccci_modem *)device->platform_data;

	/* set flag for next md_start */
	md->per_md_data.config.setting |= MD_SETTING_RELOAD;
	md->per_md_data.config.setting |= MD_SETTING_FIRST_BOOT;
	/* restore IRQ */
#ifdef FEATURE_PM_IPO_H
	irq_set_irq_type(md_ctrl->cldma_irq_id, IRQF_TRIGGER_HIGH);
	irq_set_irq_type(md_ctrl->md_wdt_irq_id, IRQF_TRIGGER_RISING);
#endif
	return 0;
}

void ccci_hif_cldma_restore_reg(struct ccci_modem *md)
{
}

void ccci_modem_restore_reg(struct ccci_modem *md)
{
	enum MD_STATE md_state = ccci_fsm_get_md_state(md->index);

	if (md_state == GATED || md_state == WAITING_TO_STOP ||
		md_state == INVALID) {
		CCCI_NORMAL_LOG(md->index, TAG,
			"Resume no need restore for md_state=%d\n", md_state);
		return;
	}

	if (md->hif_flag & (1 << CLDMA_HIF_ID))
		ccci_hif_cldma_restore_reg(md);

	ccci_hif_resume(md->index, md->hif_flag);
}

int ccci_modem_syssuspend(void)
{
	struct ccci_modem *md;

	CCCI_DEBUG_LOG(0, TAG, "%s\n", __func__);
	md = ccci_md_get_modem_by_id(0);
	if (md != NULL)
		ccci_hif_suspend(md->index, md->hif_flag);
	return 0;
}

void ccci_modem_sysresume(void)
{
	struct ccci_modem *md;

	CCCI_DEBUG_LOG(0, TAG, "%s\n", __func__);
	md = ccci_md_get_modem_by_id(0);
	if (md != NULL)
		ccci_modem_restore_reg(md);
}
