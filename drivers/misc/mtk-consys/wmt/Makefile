# SPDX-License-Identifier: GPL-2.0
#
# MT6853 connectivity (consys) WMT/STP core, vendor sources adapted to
# mainline. Nothing in here runs unless mtk_consys.hw=1 is on the kernel
# command line (see mtk-consys-hw.c).

# Vendor code relies on old-style [0]/[1] trailing arrays and walks firmware
# TLV buffers through them; bounds traps here are false positives.
UBSAN_SANITIZE := n
ccflags-y += -I$(src) \
	-I$(src)/btif/inc \
	-I$(src)/btif/plat_inc \
	-I$(src)/common \
	-I$(src)/common/common_detect \
	-I$(src)/common/common_detect/drv_init/inc \
	-I$(src)/common/common_main/core/include \
	-I$(src)/common/common_main/include \
	-I$(src)/common/common_main/linux/include \
	-I$(src)/common/common_main/platform/include \
	-I$(src)/common/debug_utility \
	-I$(src)/compat/include

ccflags-y += -DCONFIG_MTK_WCN_ARM64 \
	-DWMT_IDC_SUPPORT=0 \
	-DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT \
	-DCONSYS_PMIC_CTRL_6635=1 \
	-DWMT_PLAT_ALPS=1 \
	-DWMT_UART_RX_MODE_WORK=0 \
	-DWMT_SDIO_MODE=1 \
	-DWMT_CREATE_NODE_DYNAMIC=1 \
	-DLOG_STP_DEBUG_DISABLE \
	-DWMT_DBG_SUPPORT=0 \
	-DWMT_DEVAPC_DBG_SUPPORT=0 \
	-DCONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH=1 \
	-DMTK_WCN_REMOVE_KERNEL_MODULE \
	-DMTK_WCN_BUILT_IN_DRIVER \
	-DBTIF_DBG_SUPPORT=0

# The vendor code keeps large buffers on the stack; with CONFIG_WERROR the
# frame size check would turn that into a build failure.
ccflags-y += $(call cc-option,-Wno-frame-larger-than)

obj-$(CONFIG_MTK_CONSYS_WMT) += mtk-consys-wmt.o

mtk-consys-wmt-y := \
	mtk-consys-hw.o \
	btif/btif_dma_plat.o \
	btif/btif_plat.o \
	btif/mtk_btif.o \
	btif/mtk_btif_exp.o \
	common/common_detect/drv_init/bluetooth_drv_init.o \
	common/common_detect/drv_init/common_drv_init.o \
	common/common_detect/drv_init/conn_drv_init.o \
	common/common_detect/drv_init/fm_drv_init.o \
	common/common_detect/drv_init/gps_drv_init.o \
	common/common_detect/drv_init/wlan_drv_init.o \
	common/common_detect/mtk_wcn_stub_alps.o \
	common/common_detect/sdio_detect.o \
	common/common_detect/wmt_detect.o \
	common/common_detect/wmt_detect_pwr.o \
	common/common_detect/wmt_gpio.o \
	common/common_main/core/btm_core.o \
	common/common_main/core/psm_core.o \
	common/common_main/core/stp_core.o \
	common/common_main/core/stp_exp.o \
	common/common_main/core/wmt_conf.o \
	common/common_main/core/wmt_core.o \
	common/common_main/core/wmt_ctrl.o \
	common/common_main/core/wmt_exp.o \
	common/common_main/core/wmt_func.o \
	common/common_main/core/wmt_ic_6620.o \
	common/common_main/core/wmt_ic_6628.o \
	common/common_main/core/wmt_ic_6630.o \
	common/common_main/core/wmt_ic_6632.o \
	common/common_main/core/wmt_ic_soc.o \
	common/common_main/core/wmt_lib.o \
	common/common_main/linux/bgw_desense.o \
	common/common_main/linux/fw_log_wmt.o \
	common/common_main/linux/hif_sdio.o \
	common/common_main/linux/osal.o \
	common/common_main/linux/stp_btif.o \
	common/common_main/linux/stp_dbg.o \
	common/common_main/linux/stp_dbg_combo.o \
	common/common_main/linux/stp_dbg_soc.o \
	common/common_main/linux/stp_sdio.o \
	common/common_main/linux/wmt_alarm.o \
	common/common_main/linux/wmt_dbg.o \
	common/common_main/linux/wmt_dev.o \
	common/common_main/linux/wmt_idc.o \
	common/common_main/linux/wmt_proc_dbg.o \
	common/common_main/linux/wmt_user_proc.o \
	common/common_main/platform/mt6853.o \
	common/common_main/platform/mt6853_dbg.o \
	common/common_main/platform/mtk_wcn_cmb_hw.o \
	common/common_main/platform/mtk_wcn_consys_hw.o \
	common/common_main/platform/mtk_wcn_dump_util.o \
	common/common_main/platform/wmt_plat_alps.o \
	common/debug_utility/connsys_debug_utility.o \
	common/debug_utility/ring.o \
	common/debug_utility/ring_emi.o \
	common/mtk-consys-adapter.o \
	common/wmt_build_in_adapter.o
