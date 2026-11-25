.. SPDX-License-Identifier: GPL-2.0

MT6853 vendor VCU encoder
========================

CONFIG_VIDEO_MEDIATEK_MT6853_VCU_ENC provides the MT6853 stateful V4L2 encoder
for Android's vendor c2.mtk codec2 service. It uses the existing MTK_VCU bridge
and vendor vpud, not an SCP or VPU firmware image. The cannong_defconfig builds
it into the kernel. The mainline VIDEO_MEDIATEK_VCODEC driver must be disabled
because its firmware ABI and supported platforms differ.

The source is adapted from xiaomi-mt6853-devs/android_kernel_xiaomi_cannon,
commit 61923102fe542f96a4a993b78760c975c0b7508f, drivers/media/platform/mtk-vcodec
and its mt6853 platform directory. The limitbeer0 MT6853 6.12 modules tree at
28923475d0c884aa85cd74d90f6d5254cace3670 was also inspected; its current codec
implementation targets newer VCP/SMMU platforms and is not the phone's VCU ABI.

Probe and userspace
-------------------

The driver binds to ``mediatek,mt6853-vcodec-enc`` at /venc@17000000, whose
register resource is 0x17020000. The duplicate /venc@17020000 node remains
disabled. All nine encoder ports on SMI larb7 must appear in the codec's iommus
property. The existing MT6853 clock and SMI drivers supply the encoder clock,
larb clocks and power domain; no additional clock driver is needed.

Boot with ``mtk_vcu.enable=1`` and start the matching vendor vpud service.
``/dev/vcu`` is the transport. The encoder creates a dynamically numbered
``/dev/videoN`` with QUERYCAP driver/card name ``mtk-vcodec-enc``. The vendor
codec2 blobs scan /dev/video* by this name, so a fixed video minor is not needed.
There is no separate /dev/venc node and this change adds no decoder device.

Format and frame-size capabilities are queried with IPI_VENC_COMMON. AVC,
legacy H265 FourCC, and HEIF/grid controls retain the vendor's private IDs and
shared-memory layouts. The vpud initialization enc_capability word is an
optional feature mask, not the supported-format table: zero must not be used
to suppress all encoders. No capability bits are fabricated by this driver.
The stock MT6853 path passes IOVAs and zero FD fields; it does not install FDs
into vpud. Generic vb2 DMA-contig owns buffer mappings and cache maintenance.
GCE submissions install the clock/lock hooks and teardown waits for pending
GCE completions before releasing a codec context.

The existing cannon ueventd rules give /dev/video* and /dev/vcu mode 0660,
owner camera, group system. Android's base policy labels /dev/video* as
video_device, and MediaTek's mtk_hal_c2 policy permits access. No new SELinux
policy is necessary for these nodes.

Validation and limitations
--------------------------

Run the focused host checks from the kernel root::

    python3 tools/testing/mt6853-vcodec/run.py

They exercise the actual shared-memory mapping, capability-ACK target checks
and completion-ring helpers. They do not emulate codec hardware.

Targeted kernel builds use the same Clang r596125 environment as the ROM kernel
build, without cleaning either source or output directories::

    make O=<existing-KERNEL_OBJ> ARCH=arm64 LLVM=1 LLVM_IAS=1 olddefconfig
    make O=<existing-KERNEL_OBJ> ARCH=arm64 LLVM=1 LLVM_IAS=1 \
      drivers/media/platform/mediatek/mt6853-vcodec/ drivers/misc/mtk-vcu/
    make O=<existing-KERNEL_OBJ> ARCH=arm64 LLVM=1 LLVM_IAS=1 \
      mediatek/mt6853-cannong.dtb

Encoder and VCU object builds use -Werror. Their objects were also partially
linked together to check for duplicate symbols and unresolved codec helpers.
The combined cannong DT was checked for the nine larb7 encoder ports.

Secure encoding, auxiliary ROI maps and HDR metadata buffers are unsupported
and return EOPNOTSUPP. Vendor DVFS/MMQoS/SLBC hooks are not ported; clock rates
remain under the existing clock-tree policy. A decoder needs its own stateful
V4L2/VCU front-end port; enabling the upstream decoder is not a substitute.

Hardware encode and Android HEIC have not been tested. Runtime qualification
must verify vpud format discovery, AVC/HEVC bitstreams, HEIF grid/flush/drain,
multiple contexts and suspend/resume before claiming hardware support working.
The matching codec2/vpud binaries and vendor codec XML configuration remain
userspace requirements; this change does not add or patch vendor blobs.
