# SPDX-License-Identifier: GPL-2.0
#
# /dev/stpgps for the MT6853 consys. Uses the WMT/STP interfaces exported by
# ../wmt and has to be built with the same switches as that code.

ccflags-y += -I$(src)/../wmt/common/common_detect/drv_init/inc \
	-I$(src)/../wmt/common/common_main/include \
	-I$(src)/../wmt/common/common_main/linux/include \
	-I$(src)/../wmt/common/common_main/core/include \
	-I$(src)/../wmt/common/common_main/platform/include \
	-I$(src)/../wmt/compat/include

ccflags-y += -DCONFIG_MTK_WCN_ARM64 \
	-DWMT_IDC_SUPPORT=0 \
	-DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT

obj-$(CONFIG_MTK_CONSYS_GPS) += mtk-stpgps.o

mtk-stpgps-y := stp_chrdev_gps.o gps_emi.o gps_pwr.o
