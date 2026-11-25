# SPDX-License-Identifier: GPL-2.0
#
# MT6853 (consys, soc2_0 A-die) Wi-Fi: vendor gen4m driver (SOC2_2X2,
# connac 1.0, AXI host interface) plus the /dev/wmtWifi adaptor, built into
# the kernel. The code is registered by the WMT detect path and does not touch
# the hardware before userspace powers the Wi-Fi function on.

# Vendor code relies on old-style [0]/[1] trailing arrays and walks firmware
# TLV buffers through them; bounds traps here are false positives.
UBSAN_SANITIZE := n
WMT := $(src)/../wmt

ccflags-y += -include $(WMT)/compat/include/mtk_compat.h
ccflags-y += -I$(src)/compat \
	-I$(src)/gen4m/os -I$(src)/gen4m/os/linux/include \
	-I$(src)/gen4m/include -I$(src)/gen4m/include/nic \
	-I$(src)/gen4m/include/mgmt -I$(src)/gen4m/include/chips \
	-I$(src)/gen4m/os/linux/hif/common/include \
	-I$(src)/gen4m/os/linux/hif/axi/include \
	-I$(src)/gen4m/wlan_service/include \
	-I$(src)/gen4m/wlan_service/service/include \
	-I$(src)/gen4m/wlan_service/glue/osal/include \
	-I$(src)/gen4m/wlan_service/glue/hal/include \
	-I$(src)/adaptor/include \
	-I$(WMT)/compat/include \
	-I$(WMT)/common \
	-I$(WMT)/common/common_main/include \
	-I$(WMT)/common/common_main/linux/include \
	-I$(WMT)/common/debug_utility \
	-I$(srctree)/net

ccflags-y += -DLINUX -DSOC2_2X2 -DCONFIG_MTK_WIFI_VHT80 \
	-D_HIF_AXI=1 -DCFG_WIFI_IP_SET=3 -DCFG_MTK_ANDROID_WMT=1 \
	-DCFG_MTK_ANDROID_EMI=1 -DCONFIG_WLAN_MTK_EMI=1 \
	-DCFG_BUILT_IN_DRIVER=1 -DCONFIG_WLAN_DRV_BUILD_IN=1 \
	-DMTK_WCN_REMOVE_KERNEL_MODULE -DMTK_WCN_BUILT_IN_DRIVER \
	-DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT -DWMT_IDC_SUPPORT=0 \
	-DCONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH -DCREATE_NODE_DYNAMIC=1 \
	-DCFG_SUPPORT_CONNINFRA=0 -DCFG_ANDORID_CONNINFRA_SUPPORT=0 \
	-DCFG_ANDORID_CONNINFRA_COREDUMP_SUPPORT=0 \
	-DCFG_SUPPORT_CONNAC2X=0 -DCFG_SUPPORT_CONNAC2X_2x2=0 \
	-DCFG_SUPPORT_802_11AX=0 -DCFG_SUPPORT_802_11BE=0 \
	-DCFG_SUPPORT_WIFI_6G=0 -DCFG_SUPPORT_TWT=0 -DCFG_TWT_SMART_STA=0 \
	-DCFG_ENABLE_HOST_BUS_TIMEOUT=0 -DCFG_SUPPORT_SET_IPV6_NETWORK=0 \
	-DCFG_DRIVER_INITIAL_RUNNING_MODE=3 -DCFG_SUPPORT_DUAL_STA=1 \
	-DCFG_SUPPORT_PRE_ON_PHY_ACTION=0 -DCFG_SUPPORT_POWER_THROTTLING=0 \
	-DCFG_SUPPORT_CHNL_CONFLICT_REVISE=0 -DCFG_MTK_MDDP_SUPPORT=0 \
	-DCFG_SUPPORT_PKT_OFLD=0 -DCONFIG_WLAN_SERVICE=1 \
	-DCONFIG_TEST_ENGINE_OFFLOAD=1 \
	-DCFG_SUPPORT_DEBUG_FS=0 -DWLAN_INCLUDE_PROC \
	-DCFG_SUPPORT_AGPS_ASSIST=0 -DCFG_SUPPORT_TSF_USING_BOOTTIME=1 \
	-DARP_MONITER_ENABLE=1 -DCFG_SUPPORT_AEE=0 -DBUILD_QA_DBG=0 \
	-DCFG_WPS_DISCONNECT=1 -DCFG_DOWNLOAD_DYN_MEMORY_MAP=0 \
	-DCFG_SUPPORT_PASSPOINT=1 -DCFG_HS20_DEBUG=1 \
	-DCFG_MET_PACKET_TRACE_SUPPORT=1 -DCFG_MET_TAG_SUPPORT=0 \
	-DCFG_TC3_FEATURE=0 -DCFG_TC10_FEATURE=0 -DCFG_TC1_FEATURE=0 \
	-DCONFIG_MTK_MD_SUPPORT=0 -DCFG_SUPPORT_NAN=0 -DDBG=0

# Vendor code: keep its warning policy local to this directory.
ccflags-y += -Wno-unused-value -Wno-unused-result -Wno-format \
	-Wno-parentheses -Wno-frame-larger-than -Wno-missing-prototypes \
	-Wno-null-pointer-subtraction -Wno-array-bounds \
	-Wno-incompatible-pointer-types-discards-qualifiers \
	-Wno-implicit-fallthrough -Wno-unused-but-set-variable \
	-Wno-unused-variable -Wno-unused-function \
	-Wno-cast-function-type-strict -D__NO_FORTIFY

obj-$(CONFIG_MTK_CONSYS_WLAN) += mtk-consys-wlan.o

mtk-consys-wlan-y := \
	adaptor/wmt_cdev_wifi.o \
	adaptor/fw_log_wifi.o \
	adaptor/fw_log_ics.o \
	adaptor/wlan_ring.o \
	gen4m/os/linux/plat/mt6853/plat_priv.o \
	gen4m/wlan_service/agent/agent.o \
	gen4m/wlan_service/service/service_test.o \
	gen4m/wlan_service/service/test_engine.o \
	gen4m/wlan_service/glue/osal/gen4m/sys_adaption_gen4m.o \
	gen4m/wlan_service/glue/osal/gen4m/net_adaption_gen4m.o \
	gen4m/wlan_service/glue/hal/gen4m/operation_gen4m.o \
	gen4m/common/dump.o \
	gen4m/common/wlan_lib.o \
	gen4m/common/wlan_oid.o \
	gen4m/common/wlan_bow.o \
	gen4m/common/debug.o \
	gen4m/common/wlan_p2p.o \
	gen4m/common/wlan_he.o \
	gen4m/nic/nic.o \
	gen4m/nic/nic_tx.o \
	gen4m/nic/nic_txd_v1.o \
	gen4m/nic/nic_rxd_v1.o \
	gen4m/nic/nic_rx.o \
	gen4m/nic/nic_pwr_mgt.o \
	gen4m/nic/nic_rate.o \
	gen4m/nic/cmd_buf.o \
	gen4m/nic/que_mgt.o \
	gen4m/nic/nic_cmd_event.o \
	gen4m/nic/nic_umac.o \
	gen4m/nic/p2p_nic.o \
	gen4m/os/linux/gl_init.o \
	gen4m/os/linux/gl_kal.o \
	gen4m/os/linux/gl_bow.o \
	gen4m/os/linux/gl_wext.o \
	gen4m/os/linux/gl_wext_priv.o \
	gen4m/os/linux/gl_ate_agent.o \
	gen4m/os/linux/gl_qa_agent.o \
	gen4m/os/linux/gl_hook_api.o \
	gen4m/os/linux/gl_rst.o \
	gen4m/os/linux/gl_cfg80211.o \
	gen4m/os/linux/gl_proc.o \
	gen4m/os/linux/gl_sys.o \
	gen4m/os/linux/gl_vendor.o \
	gen4m/os/linux/platform.o \
	gen4m/os/linux/gl_p2p.o \
	gen4m/os/linux/gl_p2p_cfg80211.o \
	gen4m/os/linux/gl_p2p_init.o \
	gen4m/os/linux/gl_p2p_kal.o \
	gen4m/os/linux/hif/common/hal_pdma.o \
	gen4m/os/linux/hif/common/kal_pdma.o \
	gen4m/os/linux/hif/common/dbg_pdma.o \
	gen4m/os/linux/hif/common/sw_wfdma.o \
	gen4m/os/linux/hif/axi/axi.o \
	gen4m/mgmt/ais_fsm.o \
	gen4m/mgmt/aaa_fsm.o \
	gen4m/mgmt/assoc.o \
	gen4m/mgmt/auth.o \
	gen4m/mgmt/bss.o \
	gen4m/mgmt/cnm.o \
	gen4m/mgmt/cnm_timer.o \
	gen4m/mgmt/cnm_mem.o \
	gen4m/mgmt/hem_mbox.o \
	gen4m/mgmt/mib.o \
	gen4m/mgmt/privacy.o \
	gen4m/mgmt/rate.o \
	gen4m/mgmt/rlm.o \
	gen4m/mgmt/rlm_domain.o \
	gen4m/mgmt/reg_rule.o \
	gen4m/mgmt/rlm_obss.o \
	gen4m/mgmt/rlm_protection.o \
	gen4m/mgmt/rrm.o \
	gen4m/mgmt/rsn.o \
	gen4m/mgmt/saa_fsm.o \
	gen4m/mgmt/scan.o \
	gen4m/mgmt/scan_fsm.o \
	gen4m/mgmt/scan_cache.o \
	gen4m/mgmt/swcr.o \
	gen4m/mgmt/roaming_fsm.o \
	gen4m/mgmt/tkip_mic.o \
	gen4m/mgmt/hs20.o \
	gen4m/mgmt/tdls.o \
	gen4m/mgmt/wnm.o \
	gen4m/mgmt/qosmap.o \
	gen4m/mgmt/ap_selection.o \
	gen4m/mgmt/wmm.o \
	gen4m/mgmt/mddp.o \
	gen4m/mgmt/thrm.o \
	gen4m/mgmt/mscs.o \
	gen4m/mgmt/ie_sort.o \
	gen4m/mgmt/stats.o \
	gen4m/mgmt/p2p_dev_fsm.o \
	gen4m/mgmt/p2p_dev_state.o \
	gen4m/mgmt/p2p_role_fsm.o \
	gen4m/mgmt/p2p_role_state.o \
	gen4m/mgmt/p2p_func.o \
	gen4m/mgmt/p2p_scan.o \
	gen4m/mgmt/p2p_ie.o \
	gen4m/mgmt/p2p_rlm.o \
	gen4m/mgmt/p2p_assoc.o \
	gen4m/mgmt/p2p_bss.o \
	gen4m/mgmt/p2p_rlm_obss.o \
	gen4m/mgmt/p2p_fsm.o \
	gen4m/mgmt/wapi.o \
	gen4m/chips/common/cmm_asic_connac.o \
	gen4m/chips/common/dbg_connac.o \
	gen4m/chips/common/fw_dl.o \
	gen4m/chips/soc2_2x2/soc2_2x2.o
