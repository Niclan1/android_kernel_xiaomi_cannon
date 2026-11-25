#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host regression checks of the actual VCU payload/completion helpers."""
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]
CODEC = ROOT / "drivers/media/platform/mediatek/mt6853-vcodec"
VCU = ROOT / "drivers/misc/mtk-vcu/mtk_vcu.c"


def block(path, name, kind="function"):
    text = path.read_text()
    if kind == "function":
        match = re.search(r"^.*\b" + name + r"\([^;]*?\)\s*\{", text, re.M)
    else:
        match = re.search(r"^" + kind + r" " + name + r"\s*\{", text, re.M)
    if not match:
        raise RuntimeError(f"cannot find {name} in {path}")
    start = match.start()
    opening = text.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + (";\n" if kind != "function" else "\n")


def define(path, name):
    return re.search(r"^#define\s+" + name + r"\s+[^\n]+", path.read_text(), re.M)[0] + "\n"


shim = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/videodev2.h>
#define CONFIG_64BIT 1
typedef uint64_t u64;
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) >= (uintptr_t)-4095)
#define dev_info(...) ((void)0)
#define mtk_vcodec_debug(...) ((void)0)
struct mtk_vcu { char *dmem_va; uintptr_t dmem_vma; };
struct platform_device { struct mtk_vcu *data; };
static void *platform_get_drvdata(struct platform_device *pdev) { return pdev->data; }
struct venc_vcu_inst {
    struct platform_device *dev;
    void *query_out;
    unsigned int query_id;
};
struct venc_inst { int unused; };
"""
source = shim + define(VCU, "VCU_SHMEM_SIZE")
source += define(CODEC / "venc_ipi_msg.h", "MTK_MAX_ENC_CODECS_SUPPORT")
source += define(CODEC / "venc_ipi_msg.h", "VENC_MAX_FB_NUM")
for path, name, kind in [
    ("vcodec_ipi_msg.h", "mtk_fmt_type", "enum"),
    ("vcodec_ipi_msg.h", "mtk_video_fmt", "struct"),
    ("vcodec_ipi_msg.h", "mtk_codec_framesizes", "struct"),
    ("venc_ipi_msg.h", "venc_get_param_type", "enum"),
    ("venc_drv_if.h", "venc_done_result", "struct"),
    ("venc_ipi_msg.h", "venc_vcu_ipi_query_cap_ack", "struct"),
    ("venc_ipi_msg.h", "ring_input_list", "struct"),
]:
    source += block(CODEC / path, name, kind)
source += block(VCU, "vcu_mapping_dm_addr_range")
source += block(CODEC / "venc_vcu_if.c", "handle_query_cap_ack_msg")
source += block(CODEC / "venc/venc_common_if.c", "venc_get_free_buffers")
source += r"""
static char memory[VCU_SHMEM_SIZE];
int main(void)
{
    struct mtk_vcu bridge = { memory, 0x80000000 };
    struct platform_device pdev = { &bridge };
    union {
        struct mtk_video_fmt formats[MTK_MAX_ENC_CODECS_SUPPORT];
        struct mtk_codec_framesizes sizes[MTK_MAX_ENC_CODECS_SUPPORT];
    } output;
    struct venc_vcu_inst inst = {
        &pdev, &output, VENC_GET_PARAM_CAPABILITY_SUPPORTED_FORMATS
    };
    struct venc_vcu_ipi_query_cap_ack ack = {
        .id = inst.query_id,
        .ap_data_addr = (uintptr_t)&output,
        .vcu_data_addr = bridge.dmem_vma + 64,
    };
    struct ring_input_list ring = {0};
    struct venc_done_result result = {0};
    size_t size = sizeof(output.formats);

    assert(vcu_mapping_dm_addr_range(NULL, bridge.dmem_vma, 4) == NULL);
    assert(vcu_mapping_dm_addr_range(&pdev, bridge.dmem_vma - 1, 4) == NULL);
    assert(vcu_mapping_dm_addr_range(&pdev, bridge.dmem_vma, 0) == NULL);
    assert(vcu_mapping_dm_addr_range(&pdev,
           bridge.dmem_vma + VCU_SHMEM_SIZE - 4, 4) == memory + VCU_SHMEM_SIZE - 4);
    assert(vcu_mapping_dm_addr_range(&pdev,
           bridge.dmem_vma + VCU_SHMEM_SIZE - 4, 5) == NULL);

    memset(memory + 64, 0xa5, size);
    assert(handle_query_cap_ack_msg(&inst, &ack) == 0);
    assert(memcmp(&output, memory + 64, size) == 0);
    memset(&output, 0x5a, sizeof(output));
    ack.ap_data_addr++;
    assert(handle_query_cap_ack_msg(&inst, &ack) == -EINVAL);
    assert(((unsigned char *)&output)[0] == 0x5a);
    ack.ap_data_addr--;
    ack.id++;
    assert(handle_query_cap_ack_msg(&inst, &ack) == -EINVAL);
    ack.id = inst.query_id;
    ack.vcu_data_addr = bridge.dmem_vma + VCU_SHMEM_SIZE - size + 1;
    assert(handle_query_cap_ack_msg(&inst, &ack) == -EINVAL);
    assert(((unsigned char *)&output)[0] == 0x5a);

    ring.read_idx = VENC_MAX_FB_NUM - 1;
    ring.count = 2;
    ring.venc_bs_va_list[ring.read_idx] = 123;
    ring.venc_bs_va_list[0] = 456;
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 123 && ring.read_idx == 0 && ring.count == 1);
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 456 && ring.count == 0);
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 0 && result.frm_va == 0);
    ring.count = 1;
    ring.read_idx = -1;
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 0 && ring.count == 1);
    ring.read_idx = VENC_MAX_FB_NUM;
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 0);
    ring.read_idx = 0;
    ring.count = VENC_MAX_FB_NUM + 1;
    venc_get_free_buffers(NULL, &ring, &result);
    assert(result.bs_va == 0);
    puts("mt6853-vcodec: shared-memory, capability-reply and completion-ring tests PASS");
    return 0;
}
"""
work = Path(__file__).resolve().parent / ".build"
work.mkdir(exist_ok=False)
cfile = work / "test.c"
binary = work / "test"
try:
    cfile.write_text(source)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Werror", str(cfile), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
finally:
    binary.unlink(missing_ok=True)
    cfile.unlink(missing_ok=True)
    work.rmdir()
