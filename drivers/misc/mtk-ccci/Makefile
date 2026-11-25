# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek MT6853 CCCI (MD1 modem control/data path), vendor eccci 4.14
# stack adapted to mainline. Everything is built into one object so that
# the boot gate in mtk-ccci-hw.c controls all initcalls.

UBSAN_SANITIZE := n

ccflags-y += -include $(src)/compat/include/mtk_ccci_compat.h
ccflags-y += -I$(src) -I$(src)/compat/include -I$(src)/include
ccflags-y += -I$(src)/eccci -I$(src)/eccci/inc -I$(src)/eccci/hif
ccflags-y += -I$(src)/eccci/port -I$(src)/eccci/fsm -I$(src)/eccci/mt6853
ccflags-y += -I$(src)/eccci/udc -I$(src)/eccci/udc/udc_lib_inc
ccflags-y += -I$(src)/util -I$(src)/ccmni
ccflags-y += -Wno-frame-larger-than

obj-$(CONFIG_MTK_CCCI) += mtk-ccci.o

mtk-ccci-y := mtk-ccci-hw.o

mtk-ccci-y += util/ccci_private_log.o util/ccci_util_broadcast.o \
	util/ccci_util_ld_md_errno.o util/ccci_util_lib_fo.o \
	util/ccci_util_lib_load_img.o util/ccci_util_lib_main.o \
	util/ccci_util_lib_sys.o util/ccci_util_lib_time.o \
	util/ccci_util_md_rat.o util/ccci_util_pin_broadcast.o \
	util/ccci_util_sib.o

mtk-ccci-y += eccci/ccci_bm.o eccci/ccci_core.o eccci/ccci_modem.o \
	eccci/modem_sys1.o

mtk-ccci-y += eccci/fsm/ccci_fsm.o eccci/fsm/ccci_fsm_ee.o \
	eccci/fsm/ccci_fsm_ioctl.o eccci/fsm/ccci_fsm_monitor.o \
	eccci/fsm/ccci_fsm_poller.o eccci/fsm/ccci_fsm_scp.o \
	eccci/fsm/ccci_fsm_sys.o eccci/fsm/mdee_dumper_v1.o \
	eccci/fsm/mdee_dumper_v2.o eccci/fsm/mdee_dumper_v3.o \
	eccci/fsm/mdee_dumper_v5.o

mtk-ccci-y += eccci/hif/ccci_hif.o eccci/hif/ccci_hif_ccif.o \
	eccci/hif/ccci_hif_dpmaif.o eccci/hif/ccci_ringbuf.o \
	eccci/hif/dpmaif_drv.o

mtk-ccci-y += eccci/port/ccci_swtp.o eccci/port/port_cfg.o \
	eccci/port/port_char.o eccci/port/port_ctlmsg.o \
	eccci/port/port_ipc.o eccci/port/port_misc.o eccci/port/port_net.o \
	eccci/port/port_poller.o eccci/port/port_proxy.o \
	eccci/port/port_rpc.o eccci/port/port_smem.o \
	eccci/port/port_sysmsg.o eccci/port/port_udc.o

mtk-ccci-y += eccci/udc/udc.o

mtk-ccci-y += eccci/mt6853/ap_md_reg_dump.o eccci/mt6853/ccci_platform.o \
	eccci/mt6853/md_sys1_platform.o

mtk-ccci-y += ccmni/ccmni.o
