# SPDX-License-Identifier: GPL-2.0-only
#
# MediaTek MT6853 legacy camera stack (4.14 imgsensor/seninf ABI).

IMGSENSOR := $(src)/imgsensor

ccflags-y += -I$(IMGSENSOR) -I$(IMGSENSOR)/inc -I$(IMGSENSOR)/camera_hw \
	     -I$(IMGSENSOR)/camera_hw/gpio -I$(IMGSENSOR)/camera_hw/mclk \
	     -I$(IMGSENSOR)/camera_hw/regulator
ccflags-y += -DS5KGM1STOFILM_MIPI_RAW -DS5K3L6OFILM_MIPI_RAW \
	     -DHI259OFILM_MIPI_RAW -DOV02B10OFILM_MIPI_RAW \
	     -DOV8856OFILM_MIPI_RAW -DSENSOR_PARALLEISM
# Vendor code: keep the warnings that only flag style, not behaviour.
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations \
	     -Wno-unused-function -Wno-unused-variable \
	     -Wno-unused-but-set-variable -Wno-unused-const-variable
UBSAN_SANITIZE := n
# ISP P1: MT6853 has no camsv0/camsv1 (camsv3..8 in the DT are SV2..SV7).
CFLAGS_isp/camera_isp.o := -DDISABLE_SV_TOP0 -I$(src)
# ISP P2 (DIP): MT6853 has one DIP and the MSF/MSS block at 0x1581xxxx.
CFLAGS_dip/camera_dip.o := -DMTK_DIP_COUNT=1 -DMTK_MSF_OFFSET=1 -I$(src)/dip
CFLAGS_mdp/mdp_compat.o := -I$(src)/mdp

obj-$(CONFIG_MTK_CAM_PORT) += mtk-cam.o

mtk-cam-y := mtk_cam.o \
	cam_cal/cam_cal.o \
	lens/main_lens.o \
	isp/camera_isp.o \
	dip/camera_dip.o \
	ldo/fan53870.o \
	ldo/pm8008.o \
	imgsensor/imgsensor.o \
	imgsensor/imgsensor_hw.o \
	imgsensor/imgsensor_i2c.o \
	imgsensor/imgsensor_legacy.o \
	imgsensor/imgsensor_proc.o \
	imgsensor/imgsensor_pwr_seq.o \
	imgsensor/imgsensor_sensor_list.o \
	imgsensor/seninf.o \
	imgsensor/seninf_clk.o \
	imgsensor/seninf_irq.o \
	imgsensor/camera_hw/imgsensor_cfg_table.o \
	imgsensor/camera_hw/gpio/gpio.o \
	imgsensor/camera_hw/mclk/mclk.o \
	imgsensor/camera_hw/regulator/regulator.o \
	imgsensor/sensors/s5kgm1stofilm_mipi_raw/s5kgm1stofilm_mipiraw_Sensor.o \
	imgsensor/sensors/s5k3l6ofilm_mipi_raw/s5k3l6ofilm_mipiraw_Sensor.o \
	imgsensor/sensors/ov02b10ofilm_mipi_raw/ov02b10ofilm_mipiraw_Sensor.o \
	imgsensor/sensors/ov8856ofilm_mipi_raw/ov8856ofilm_mipiraw_Sensor.o \
	imgsensor/sensors/hi259ofilm_mipi_raw/hi259ofilm_mipiraw_Sensor.o

# MDP needs the built-in mainline GCE helper, flashlight the LED flash class.
ifeq ($(CONFIG_MTK_CMDQ),y)
mtk-cam-y += mdp/mdp_compat.o
endif
ifeq ($(CONFIG_LEDS_CLASS_FLASH),y)
mtk-cam-y += flash/flashlight_compat.o
endif
