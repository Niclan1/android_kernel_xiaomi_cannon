# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_MEDIATEK_MT6853_VCU_ENC) += mtk-mt6853-vcodec-enc.o
mtk-mt6853-vcodec-enc-y := mtk_vcodec_enc_drv.o mtk_vcodec_enc.o \
	mtk_vcodec_util.o mtk_vcodec_pm.o mtk_vcodec_intr.o \
	venc_drv_if.o venc_vcu_if.o venc/venc_common_if.o venc/venc_log_if.o
ccflags-y += -Werror -I$(srctree)/drivers/misc/mtk-vcu \
	-I$(srctree)/drivers/media/platform/mediatek/mt6853-vcodec
