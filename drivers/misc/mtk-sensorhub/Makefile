# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_MTK_AW8622_VIBRATOR)	+= aw8622_vibrator.o
obj-$(CONFIG_MTK_NQ_NCI)		+= mtk_nq_nci.o
obj-$(CONFIG_MTK_TEE_FINGERPRINT)	+= mtk_tee_fingerprint.o
obj-$(CONFIG_MTK_SCP_PROBE)		+= mtk_scp_probe.o

# uuid_fp and struct TEEC_UUID come from the Microtrust TEE driver
CFLAGS_mtk_tee_fingerprint.o	+= -I$(srctree)/drivers/tee/teei/400/common/include \
				   -I$(srctree)/drivers/tee/teei/400/tz_driver/include
