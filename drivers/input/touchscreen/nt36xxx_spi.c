// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT36xxx host-download (HOSTDL) TDDI touchscreen controller, SPI.
 *
 * The touch MCU of these TDDI parts has no flash: the host downloads the
 * firmware into its SRAM after every panel power-up / reset.  The protocol
 * is derived from the Novatek NT36xxx HOSTDL SPI reference driver,
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#define NVT_TS_NAME			"NVTCapacitiveTouchScreen"
#define NVT_MAX_TOUCHES			10
#define NVT_DEFAULT_MAX_X		1080
#define NVT_DEFAULT_MAX_Y		2340

#define NVT_DUMMY_BYTES			1
#define NVT_TRANSFER_LEN		(63 * 1024)
#define NVT_XBUF_LEN			(NVT_TRANSFER_LEN + 1 + NVT_DUMMY_BYTES)
#define NVT_RBUF_LEN			128
#define NVT_POINT_DATA_LEN		65
#define NVT_MAX_FW_NAMES		4

#define NVT_SPI_WRITE			BIT(7)
#define NVT_CMD_SET_PAGE		0xff

#define NVT_ENG_RST_ADDR		0x7fff80
#define NVT_SWRST_N8_ADDR_DEFAULT	0x03f0fe
#define NVT_SPI_RD_FAST_ADDR_DEFAULT	0x03f310
#define NVT_CHIP_ID_ADDR		0x01f64e
#define NVT_CHIP_ID_LEN			6

/* NT36672A memory map */
#define NVT_EVENT_BUF_ADDR		0x21c00
#define NVT_BOOT_RDY_ADDR		0x3f10d
#define NVT_ILM_LENGTH_ADDR		0x3f118
#define NVT_DLM_LENGTH_ADDR		0x3f130
#define NVT_ILM_DES_ADDR		0x3f128
#define NVT_DLM_DES_ADDR		0x3f12c
#define NVT_G_ILM_CHECKSUM_ADDR		0x3f100
#define NVT_G_DLM_CHECKSUM_ADDR		0x3f104
#define NVT_BLD_CRC_EN_ADDR		0x3f30e

/* Offsets in the event buffer */
#define NVT_EVT_HOST_CMD		0x50
#define NVT_EVT_RESET_COMPLETE		0x60
#define NVT_EVT_FWINFO			0x78
#define NVT_EVT_PROJECTID		0x9a

#define NVT_CMD_DEEP_SLEEP		0x11
#define NVT_CMD_WAKEUP_GESTURE		0x13
#define NVT_GESTURE_DOUBLE_TAP		15
#define NVT_DATA_PROTOCOL		30
#define NVT_FUNC_PAGE_GESTURE		1
#define NVT_CMD_FW_CRC_EN		0xae

#define NVT_SWRST_SW_RESET_BLD		0x69
#define NVT_ENG_RST_CMD			0x5a

#define NVT_RESET_STATE_INIT		0xa0
#define NVT_RESET_STATE_REK		0xa1
#define NVT_RESET_STATE_MAX		0xaf

#define NVT_FW_SECTOR_SIZE		4096
#define NVT_FW_END_FLAG_LEN		3
#define NVT_FW_HDR_INFO_START		0x30
#define NVT_FW_HDR_OVLY_INFO		0x28
#define NVT_FW_MAX_PARTS		64
#define NVT_FW_DOWNLOAD_RETRIES		3

#define NVT_BOOT_LOAD_DELAY_MS		2000
#define NVT_LOAD_RETRY_MS		5000
#define NVT_LOAD_RETRIES		36
#define NVT_FOLLOWER_RETRY_MS		2000
#define NVT_FOLLOWER_RETRIES		30
#define NVT_WDT_RECOVERY_CNT		10
#define NVT_POWER_ON_DELAY_MS		20

struct nvt_trim_id {
	u8 id[NVT_CHIP_ID_LEN];
	u8 mask[NVT_CHIP_ID_LEN];
};

/* NT36672A trim IDs */
static const struct nvt_trim_id nvt_nt36672a_ids[] = {
	{ { 0x0b, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0b, 0xff, 0xff, 0x82, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0b, 0xff, 0xff, 0x25, 0x65, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x65, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0a, 0xff, 0xff, 0x82, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0a, 0xff, 0xff, 0x70, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0b, 0xff, 0xff, 0x70, 0x66, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
	{ { 0x0a, 0xff, 0xff, 0x72, 0x67, 0x03 }, { 1, 0, 0, 1, 1, 1 } },
};

struct nvt_fw_part {
	u32 bin_addr;
	u32 sram_addr;
	u32 size;
	u32 crc;
};

struct nvt_ts {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties prop;
	struct gpio_desc *reset_gpio;
	int irq;

	/* Serialises bus access; taken by the IRQ thread. */
	struct mutex lock;
	/* Serialises power/firmware state changes; never taken by the IRQ thread. */
	struct mutex ctl_lock;

	u8 *xbuf;
	u8 *rbuf;

	u32 swrst_addr;
	u32 rd_fast_addr;
	const char *fw_names[NVT_MAX_FW_NAMES];
	int num_fw_names;
	const struct firmware *fw;
	const char *fw_name;
	struct nvt_fw_part parts[NVT_FW_MAX_PARTS];
	unsigned int num_parts;

	bool chip_ok;
	bool input_registered;
	bool fw_loaded;
	bool irq_enabled;
	bool panel_off;
	bool pm_suspended;
	bool gesture_enabled;
	bool gesture_mode;
	bool irq_wake_enabled;
	struct completion pm_resumed;
	bool removing;
	u8 fw_ver;
	u16 pid;
	u16 fw_max_x;
	u16 fw_max_y;
	unsigned int wdt_cnt;

	struct delayed_work load_work;
	unsigned int load_tries;

	struct drm_panel_follower follower;
	struct delayed_work follower_work;
	unsigned int follower_tries;
	bool follower_added;
};

static int nvt_spi_xfer(struct nvt_ts *ts, size_t len, bool read)
{
	struct spi_transfer t = {
		.tx_buf = ts->xbuf,
		.len = len,
	};
	int ret, i;

	if (read) {
		t.rx_buf = ts->rbuf;
		t.len += NVT_DUMMY_BYTES;
	}

	for (i = 0; i < 5; i++) {
		ret = spi_sync_transfer(ts->spi, &t, 1);
		if (!ret)
			return 0;
	}

	dev_err_ratelimited(&ts->spi->dev, "SPI %s failed: %d\n",
			    read ? "read" : "write", ret);
	return -EIO;
}

/* Write @len bytes to @offset of the current page. */
static int nvt_write(struct nvt_ts *ts, u8 offset, const u8 *data, size_t len)
{
	ts->xbuf[0] = offset | NVT_SPI_WRITE;
	memcpy(ts->xbuf + 1, data, len);

	return nvt_spi_xfer(ts, len + 1, false);
}

/* Read @len bytes from @offset of the current page into @data. */
static int nvt_read(struct nvt_ts *ts, u8 offset, u8 *data, size_t len)
{
	int ret;

	if (WARN_ON(len + 2 > NVT_RBUF_LEN))
		return -EINVAL;

	ts->xbuf[0] = offset & ~NVT_SPI_WRITE;
	memset(ts->xbuf + 1, 0, len + NVT_DUMMY_BYTES);

	ret = nvt_spi_xfer(ts, len + 1, true);
	if (ret)
		return ret;

	/* rx[0] is clocked in during the command byte, rx[1] is the dummy byte */
	memcpy(data, ts->rbuf + 2, len);
	return 0;
}

static int nvt_set_page(struct nvt_ts *ts, u32 addr)
{
	u8 buf[2] = { (addr >> 15) & 0xff, (addr >> 7) & 0xff };

	return nvt_write(ts, NVT_CMD_SET_PAGE, buf, sizeof(buf));
}

static int nvt_write_addr(struct nvt_ts *ts, u32 addr, u8 data)
{
	int ret;

	ret = nvt_set_page(ts, addr);
	if (ret)
		return ret;

	return nvt_write(ts, addr & 0x7f, &data, 1);
}

static int nvt_read_addr(struct nvt_ts *ts, u32 addr, u8 *data, size_t len)
{
	int ret;

	ret = nvt_set_page(ts, addr);
	if (ret)
		return ret;

	return nvt_read(ts, addr & 0x7f, data, len);
}

static void nvt_eng_reset(struct nvt_ts *ts)
{
	nvt_write_addr(ts, NVT_ENG_RST_ADDR, NVT_ENG_RST_CMD);
	usleep_range(1000, 1500);
}

static void nvt_bootloader_reset(struct nvt_ts *ts)
{
	nvt_write_addr(ts, ts->swrst_addr, NVT_SWRST_SW_RESET_BLD);
	usleep_range(5000, 6000);

	if (ts->rd_fast_addr)
		nvt_write_addr(ts, ts->rd_fast_addr, 0x00);
}

static int nvt_check_reset_state(struct nvt_ts *ts, u8 state, int retries)
{
	u8 buf[5] = { };
	int ret, i;

	ret = nvt_set_page(ts, NVT_EVENT_BUF_ADDR | NVT_EVT_RESET_COMPLETE);
	if (ret)
		return ret;

	for (i = 0; i <= retries; i++) {
		ret = nvt_read(ts, NVT_EVT_RESET_COMPLETE, buf, sizeof(buf));
		if (!ret && buf[0] >= state && buf[0] <= NVT_RESET_STATE_MAX)
			return 0;
		usleep_range(10000, 11000);
	}

	dev_dbg(&ts->spi->dev, "reset state 0x%02x not reached: %5ph\n",
		state, buf);
	return -ETIMEDOUT;
}

static int nvt_identify(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	u8 id[NVT_CHIP_ID_LEN] = { };
	int retry, i, j;

	/* Engineering reset must be sent while TP_RESX is held low. */
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(1000, 1500);
	nvt_eng_reset(ts);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(10);

	for (retry = 0; retry < 5; retry++) {
		nvt_bootloader_reset(ts);

		if (!nvt_read_addr(ts, NVT_CHIP_ID_ADDR, id, sizeof(id))) {
			for (i = 0; i < ARRAY_SIZE(nvt_nt36672a_ids); i++) {
				const struct nvt_trim_id *t = &nvt_nt36672a_ids[i];

				for (j = 0; j < NVT_CHIP_ID_LEN; j++)
					if (t->mask[j] && id[j] != t->id[j])
						break;

				if (j == NVT_CHIP_ID_LEN) {
					dev_info(dev, "NT36672A detected, trim id %6ph\n",
						 id);
					return 0;
				}
			}
		}

		msleep(10);
	}

	dev_warn(dev, "no supported chip found (trim id %6ph)\n", id);
	return -ENODEV;
}

static int nvt_fw_check(struct nvt_ts *ts, const struct firmware *fw)
{
	struct device *dev = &ts->spi->dev;
	size_t sectors = fw->size / NVT_FW_SECTOR_SIZE;
	size_t need = 0, i;
	const u8 *flag;

	for (i = sectors; i > 0; i--) {
		flag = fw->data + i * NVT_FW_SECTOR_SIZE - NVT_FW_END_FLAG_LEN;
		if (!memcmp(flag, "NVT", NVT_FW_END_FLAG_LEN) ||
		    !memcmp(flag, "MOD", NVT_FW_END_FLAG_LEN)) {
			need = i * NVT_FW_SECTOR_SIZE;
			break;
		}
	}

	if (!need) {
		dev_err(dev, "firmware end flag not found\n");
		return -EINVAL;
	}

	if (fw->data[need - NVT_FW_SECTOR_SIZE] +
	    fw->data[need - NVT_FW_SECTOR_SIZE + 1] != 0xff) {
		dev_err(dev, "bad firmware version bar %02x/%02x\n",
			fw->data[need - NVT_FW_SECTOR_SIZE],
			fw->data[need - NVT_FW_SECTOR_SIZE + 1]);
		return -EINVAL;
	}

	return 0;
}

static int nvt_fw_parse(struct nvt_ts *ts, const struct firmware *fw)
{
	struct device *dev = &ts->spi->dev;
	const u8 *data = fw->data;
	size_t size = fw->size;
	unsigned int info_num, ovly_num, n, i;
	struct nvt_fw_part *p;
	u32 end, pos;

	if (size < NVT_FW_HDR_INFO_START)
		return -EINVAL;

	end = get_unaligned_le32(data);
	info_num = end > NVT_FW_HDR_INFO_START ?
		   DIV_ROUND_UP(end - NVT_FW_HDR_INFO_START, 0x10) : 0;
	ovly_num = (data[NVT_FW_HDR_OVLY_INFO] & 0x10) ?
		   (data[NVT_FW_HDR_OVLY_INFO] & 0x0f) : 0;
	n = 2 + info_num + ovly_num;
	if (n > NVT_FW_MAX_PARTS) {
		dev_err(dev, "too many firmware partitions (%u)\n", n);
		return -EINVAL;
	}

	for (i = 0; i < n; i++) {
		p = &ts->parts[i];

		if (i < 2) {
			/* ILM / DLM: BIN, SRAM, size; CRC at 0x18 / 0x1c */
			pos = i * 12;
			p->bin_addr = get_unaligned_le32(data + pos);
			p->sram_addr = get_unaligned_le32(data + pos + 4);
			p->size = get_unaligned_le32(data + pos + 8);
			p->crc = get_unaligned_le32(data + 0x18 + i * 4);
			continue;
		}

		/* Info and overlay sections: SRAM, size, BIN, CRC */
		if (i < 2 + info_num)
			pos = NVT_FW_HDR_INFO_START + 0x10 * (i - 2);
		else
			pos = ts->parts[1].bin_addr + 0x10 * (i - 2 - info_num);

		if ((u64)pos + 16 > size) {
			dev_err(dev, "firmware header %u out of range\n", i);
			return -EINVAL;
		}

		p->sram_addr = get_unaligned_le32(data + pos);
		p->size = get_unaligned_le32(data + pos + 4);
		p->bin_addr = get_unaligned_le32(data + pos + 8);
		p->crc = get_unaligned_le32(data + pos + 12);
	}

	for (i = 0; i < n; i++) {
		p = &ts->parts[i];
		if ((u64)p->bin_addr + p->size > size) {
			dev_err(dev, "firmware partition %u (0x%x+0x%x) out of range\n",
				i, p->bin_addr, p->size);
			return -EINVAL;
		}
	}

	ts->num_parts = n;
	return 0;
}

static int nvt_request_fw(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	const struct firmware *fw;
	int i, ret = -ENOENT;

	if (ts->fw)
		return 0;

	for (i = 0; i < ts->num_fw_names; i++) {
		ret = firmware_request_nowarn(&fw, ts->fw_names[i], dev);
		if (ret)
			continue;

		ret = nvt_fw_check(ts, fw);
		if (!ret)
			ret = nvt_fw_parse(ts, fw);
		if (ret) {
			dev_err(dev, "invalid firmware %s\n", ts->fw_names[i]);
			release_firmware(fw);
			continue;
		}

		ts->fw = fw;
		ts->fw_name = ts->fw_names[i];
		dev_info(dev, "using firmware %s (%zu bytes, %u partitions)\n",
			 ts->fw_name, fw->size, ts->num_parts);
		return 0;
	}

	return ret;
}

static int nvt_write_sram(struct nvt_ts *ts, const u8 *src, u32 sram, u32 len)
{
	u32 chunk;
	int ret;

	while (len) {
		chunk = min_t(u32, len, NVT_TRANSFER_LEN);

		ret = nvt_set_page(ts, sram);
		if (ret)
			return ret;

		ts->xbuf[0] = (sram & 0x7f) | NVT_SPI_WRITE;
		memcpy(ts->xbuf + 1, src, chunk);
		ret = nvt_spi_xfer(ts, chunk + 1, false);
		if (ret)
			return ret;

		sram += chunk;
		src += chunk;
		len -= chunk;
	}

	return 0;
}

static int nvt_write_fw(struct nvt_ts *ts)
{
	const struct firmware *fw = ts->fw;
	struct nvt_fw_part *p;
	unsigned int i;
	u32 len;
	int ret;

	for (i = 0; i < ts->num_parts; i++) {
		p = &ts->parts[i];
		if (!p->size)
			continue;

		/* The reference implementation writes one extra byte. */
		len = min_t(u32, p->size + 1, fw->size - p->bin_addr);
		ret = nvt_write_sram(ts, fw->data + p->bin_addr, p->sram_addr, len);
		if (ret)
			return ret;
	}

	return 0;
}

static int nvt_set_crc_bank(struct nvt_ts *ts, u32 des_addr, u32 len_addr,
			    u32 cks_addr, const struct nvt_fw_part *p)
{
	u8 buf[4];
	int ret;

	/* All three registers live on the same page. */
	ret = nvt_set_page(ts, des_addr);
	if (ret)
		return ret;

	buf[0] = p->sram_addr & 0xff;
	buf[1] = (p->sram_addr >> 8) & 0xff;
	buf[2] = (p->sram_addr >> 16) & 0xff;
	ret = nvt_write(ts, des_addr & 0x7f, buf, 3);
	if (ret)
		return ret;

	buf[0] = p->size & 0xff;
	buf[1] = (p->size >> 8) & 0xff;
	ret = nvt_write(ts, len_addr & 0x7f, buf, 2);
	if (ret)
		return ret;

	put_unaligned_le32(p->crc, buf);
	return nvt_write(ts, cks_addr & 0x7f, buf, 4);
}

static int nvt_download_fw(struct nvt_ts *ts)
{
	u8 val;
	int ret;

	nvt_bootloader_reset(ts);

	ret = nvt_write_fw(ts);
	if (ret)
		return ret;

	ret = nvt_set_crc_bank(ts, NVT_ILM_DES_ADDR, NVT_ILM_LENGTH_ADDR,
			       NVT_G_ILM_CHECKSUM_ADDR, &ts->parts[0]);
	if (ret)
		return ret;

	ret = nvt_set_crc_bank(ts, NVT_DLM_DES_ADDR, NVT_DLM_LENGTH_ADDR,
			       NVT_G_DLM_CHECKSUM_ADDR, &ts->parts[1]);
	if (ret)
		return ret;

	/* Enable the bootloader hardware CRC check */
	ret = nvt_read_addr(ts, NVT_BLD_CRC_EN_ADDR, &val, 1);
	if (ret)
		return ret;
	val |= BIT(7);
	ret = nvt_write(ts, NVT_BLD_CRC_EN_ADDR & 0x7f, &val, 1);
	if (ret)
		return ret;

	/* Clear the reset status and enable the firmware CRC check */
	ret = nvt_set_page(ts, NVT_EVENT_BUF_ADDR);
	if (ret)
		return ret;
	val = 0;
	ret = nvt_write(ts, NVT_EVT_RESET_COMPLETE, &val, 1);
	if (ret)
		return ret;
	val = NVT_CMD_FW_CRC_EN;
	ret = nvt_write(ts, NVT_EVT_HOST_CMD, &val, 1);
	if (ret)
		return ret;

	ret = nvt_write_addr(ts, NVT_BOOT_RDY_ADDR, 1);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	return nvt_check_reset_state(ts, NVT_RESET_STATE_INIT, 10);
}

static int nvt_get_fw_info(struct nvt_ts *ts)
{
	u8 buf[16];
	int ret = -EIO, i;

	for (i = 0; i < 3; i++) {
		ret = nvt_read_addr(ts, NVT_EVENT_BUF_ADDR | NVT_EVT_FWINFO,
				    buf, sizeof(buf));
		if (!ret && buf[0] + buf[1] == 0xff)
			break;
		ret = -EIO;
	}

	if (!ret) {
		ts->fw_ver = buf[0];
		ts->fw_max_x = get_unaligned_be16(buf + 4);
		ts->fw_max_y = get_unaligned_be16(buf + 6);
	} else {
		dev_warn(&ts->spi->dev, "firmware info is broken\n");
	}

	if (!nvt_read_addr(ts, NVT_EVENT_BUF_ADDR | NVT_EVT_PROJECTID, buf, 2))
		ts->pid = get_unaligned_le16(buf);

	/* The IRQ handler reads from offset 0 of the event buffer page. */
	nvt_set_page(ts, NVT_EVENT_BUF_ADDR);

	return ret;
}

/* Called with ts->lock held. */
static int nvt_bring_up(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	int ret, i;

	if (!ts->chip_ok) {
		ret = nvt_identify(ts);
		if (ret)
			return ret;
		ts->chip_ok = true;
	}

	ret = nvt_request_fw(ts);
	if (ret) {
		dev_dbg(dev, "firmware not available yet: %d\n", ret);
		return ret;
	}

	for (i = 0; i < NVT_FW_DOWNLOAD_RETRIES; i++) {
		ret = nvt_download_fw(ts);
		if (!ret)
			break;
		dev_warn(dev, "firmware download attempt %d failed: %d\n",
			 i + 1, ret);
	}
	if (ret)
		return ret;

	nvt_get_fw_info(ts);
	if (nvt_check_reset_state(ts, NVT_RESET_STATE_REK, 50))
		dev_dbg(dev, "firmware did not report REK\n");
	nvt_set_page(ts, NVT_EVENT_BUF_ADDR);

	dev_info(dev, "firmware %s loaded: fw_ver=0x%02x pid=0x%04x max=%ux%u\n",
		 ts->fw_name, ts->fw_ver, ts->pid, ts->fw_max_x, ts->fw_max_y);
	return 0;
}

static void nvt_release_contacts(struct nvt_ts *ts)
{
	int i;

	if (!ts->input_registered)
		return;

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_inactive(ts->input);
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t nvt_irq_thread(int irq, void *data)
{
	struct nvt_ts *ts = data;
	u8 point[NVT_POINT_DATA_LEN + 1];
	unsigned int x, y, w, id, i;
	bool wdt = true;
	u8 *p;
	int ret;

	if (READ_ONCE(ts->pm_suspended)) {
		pm_wakeup_event(&ts->spi->dev, 1000);
		if (!wait_for_completion_timeout(&ts->pm_resumed,
						 msecs_to_jiffies(1000)))
			return IRQ_HANDLED;
	}

	mutex_lock(&ts->lock);

	if (!ts->fw_loaded || !ts->input_registered)
		goto out;

	/* point[0] is unused so that offsets match the reference layout */
	point[0] = 0;
	ret = nvt_read(ts, 0x00, point + 1, NVT_POINT_DATA_LEN);
	if (ret)
		goto out;

	if (ts->gesture_mode) {
		id = point[1] >> 3;
		if (id == NVT_DATA_PROTOCOL &&
		    point[2] == NVT_FUNC_PAGE_GESTURE)
			id = point[3];
		if (id == NVT_GESTURE_DOUBLE_TAP) {
			pm_wakeup_event(&ts->spi->dev, 1000);
			input_report_key(ts->input, KEY_WAKEUP, 1);
			input_sync(ts->input);
			input_report_key(ts->input, KEY_WAKEUP, 0);
			input_sync(ts->input);
		}
		goto out;
	}

	for (i = 1; i < 7; i++) {
		if (point[i] != 0xfd && point[i] != 0xfe) {
			wdt = false;
			break;
		}
	}
	if (wdt) {
		if (++ts->wdt_cnt > NVT_WDT_RECOVERY_CNT) {
			dev_warn(&ts->spi->dev,
				 "firmware watchdog reset detected, reloading\n");
			ts->wdt_cnt = 0;
			mod_delayed_work(system_wq, &ts->load_work, 0);
		}
		goto out;
	}
	ts->wdt_cnt = 0;

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		p = &point[1 + 6 * i];
		id = p[0] >> 3;
		if (!id || id > NVT_MAX_TOUCHES)
			continue;

		/* 1: finger enter, 2: finger moving */
		if ((p[0] & 0x07) != 0x01 && (p[0] & 0x07) != 0x02)
			continue;

		x = (p[1] << 4) | (p[3] >> 4);
		y = (p[2] << 4) | (p[3] & 0x0f);
		w = p[4] ? p[4] : 1;

		input_mt_slot(ts->input, id - 1);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input, &ts->prop,
				       min(x, ts->prop.max_x),
				       min(y, ts->prop.max_y), true);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, w);
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);

out:
	mutex_unlock(&ts->lock);
	return IRQ_HANDLED;
}

/* Called with ctl_lock held and ts->lock NOT held. */
static void nvt_irq_set(struct nvt_ts *ts, bool on)
{
	if (on == ts->irq_enabled)
		return;

	if (on)
		enable_irq(ts->irq);
	else
		disable_irq(ts->irq);
	ts->irq_enabled = on;
}

static int nvt_register_input(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	struct input_dev *input = ts->input;
	unsigned int max_x = NVT_DEFAULT_MAX_X, max_y = NVT_DEFAULT_MAX_Y;
	int ret;

	if (ts->fw_max_x && ts->fw_max_y) {
		max_x = ts->fw_max_x;
		max_y = ts->fw_max_y;
	}

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, max_x - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, max_y - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	touchscreen_parse_properties(input, true, &ts->prop);

	ret = input_mt_init_slots(input, NVT_MAX_TOUCHES,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "failed to register input device: %d\n", ret);
		return ret;
	}

	ts->input_registered = true;
	return 0;
}

/* Called with ctl_lock held and the IRQ disabled. */
static void nvt_disable_irq_wake(struct nvt_ts *ts)
{
	if (ts->irq_wake_enabled) {
		disable_irq_wake(ts->irq);
		ts->irq_wake_enabled = false;
	}
}

/* Called with ctl_lock held. */
static void nvt_sleep(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	u8 cmd = NVT_CMD_DEEP_SLEEP;
	bool gesture = !ts->removing && ts->gesture_enabled && device_may_wakeup(dev);
	int ret = 0;

	nvt_irq_set(ts, false);
	nvt_disable_irq_wake(ts);

	mutex_lock(&ts->lock);
	ts->gesture_mode = false;
	if (ts->fw_loaded) {
		if (gesture) {
			ret = enable_irq_wake(ts->irq);
			if (!ret) {
				ts->irq_wake_enabled = true;
				cmd = NVT_CMD_WAKEUP_GESTURE;
			}
		}
		if (!ret)
			ret = nvt_write_addr(ts, NVT_EVENT_BUF_ADDR |
					     NVT_EVT_HOST_CMD, cmd);
		if (!ret && ts->irq_wake_enabled)
			ts->gesture_mode = true;
	}
	if (!ts->gesture_mode)
		ts->fw_loaded = false;
	mutex_unlock(&ts->lock);

	nvt_release_contacts(ts);
	if (ts->gesture_mode) {
		nvt_irq_set(ts, true);
	} else {
		nvt_disable_irq_wake(ts);
		if (ret)
			dev_warn(dev, "failed to enter sleep mode: %d\n", ret);
	}
}

static void nvt_load_work(struct work_struct *work)
{
	struct nvt_ts *ts = container_of(to_delayed_work(work), struct nvt_ts,
					 load_work);
	struct device *dev = &ts->spi->dev;
	int ret;

	mutex_lock(&ts->ctl_lock);

	if (ts->removing || ts->pm_suspended ||
	    (ts->follower_added && ts->panel_off))
		goto out;

	nvt_irq_set(ts, false);
	nvt_disable_irq_wake(ts);
	nvt_release_contacts(ts);

	mutex_lock(&ts->lock);
	ts->gesture_mode = false;
	ts->fw_loaded = false;
	ret = nvt_bring_up(ts);
	if (!ret) {
		ts->fw_loaded = true;
		ts->wdt_cnt = 0;
	}
	mutex_unlock(&ts->lock);

	if (!ret && !ts->input_registered)
		ret = nvt_register_input(ts);

	if (!ret) {
		ts->load_tries = 0;
		nvt_irq_set(ts, true);
	} else if (++ts->load_tries < NVT_LOAD_RETRIES) {
		queue_delayed_work(system_wq, &ts->load_work,
				   msecs_to_jiffies(NVT_LOAD_RETRY_MS));
	} else {
		dev_err(dev, "touch bring-up failed (%d), giving up\n", ret);
		ts->load_tries = 0;
	}

out:
	mutex_unlock(&ts->ctl_lock);
}

static void nvt_schedule_load(struct nvt_ts *ts, unsigned long delay_ms)
{
	ts->load_tries = 0;
	mod_delayed_work(system_wq, &ts->load_work, msecs_to_jiffies(delay_ms));
}

static int nvt_panel_prepared(struct drm_panel_follower *follower)
{
	struct nvt_ts *ts = container_of(follower, struct nvt_ts, follower);

	mutex_lock(&ts->ctl_lock);
	ts->panel_off = false;
	nvt_irq_set(ts, false);
	nvt_disable_irq_wake(ts);
	mutex_lock(&ts->lock);
	ts->gesture_mode = false;
	ts->fw_loaded = false;
	mutex_unlock(&ts->lock);
	if (!ts->removing)
		nvt_schedule_load(ts, 10);
	mutex_unlock(&ts->ctl_lock);

	return 0;
}

static int nvt_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nvt_ts *ts = container_of(follower, struct nvt_ts, follower);

	cancel_delayed_work_sync(&ts->load_work);

	mutex_lock(&ts->ctl_lock);
	ts->panel_off = true;
	nvt_sleep(ts);
	mutex_unlock(&ts->ctl_lock);

	return 0;
}

static bool nvt_panel_wakeup_enabled(struct drm_panel_follower *follower)
{
	struct nvt_ts *ts = container_of(follower, struct nvt_ts, follower);
	bool enabled;

	mutex_lock(&ts->ctl_lock);
	enabled = ts->gesture_mode && !ts->removing;
	mutex_unlock(&ts->ctl_lock);

	return enabled;
}

static const struct drm_panel_follower_funcs nvt_follower_funcs = {
	.wakeup_enabled = nvt_panel_wakeup_enabled,
	.panel_prepared = nvt_panel_prepared,
	.panel_unpreparing = nvt_panel_unpreparing,
};

static void nvt_follower_work(struct work_struct *work)
{
	struct nvt_ts *ts = container_of(to_delayed_work(work), struct nvt_ts,
					 follower_work);
	struct device *dev = &ts->spi->dev;
	int ret;

	ret = drm_panel_add_follower(dev, &ts->follower);
	if (!ret) {
		mutex_lock(&ts->ctl_lock);
		ts->follower_added = true;
		mutex_unlock(&ts->ctl_lock);
		dev_info(dev, "following panel power state\n");
		return;
	}

	if (ret == -EPROBE_DEFER && ++ts->follower_tries < NVT_FOLLOWER_RETRIES) {
		queue_delayed_work(system_wq, &ts->follower_work,
				   msecs_to_jiffies(NVT_FOLLOWER_RETRY_MS));
		return;
	}

	dev_warn(dev, "panel not available (%d), not following panel power\n",
		 ret);
}

static void nvt_release_fw(void *data)
{
	struct nvt_ts *ts = data;

	release_firmware(ts->fw);
	ts->fw = NULL;
}

static int nvt_parse_fw_names(struct nvt_ts *ts)
{
	struct device *dev = &ts->spi->dev;
	int n;

	n = device_property_string_array_count(dev, "firmware-name");
	if (n <= 0) {
		ts->fw_names[0] = "novatek_ts_fw.bin";
		ts->num_fw_names = 1;
		return 0;
	}

	n = min(n, NVT_MAX_FW_NAMES);
	n = device_property_read_string_array(dev, "firmware-name",
					      ts->fw_names, n);
	if (n < 0)
		return n;

	ts->num_fw_names = n;
	return 0;
}

struct nvt_touchpanel {
	struct kobject kobj;
	struct nvt_ts *ts;
};

static ssize_t double_tap_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct nvt_touchpanel *tp = container_of(kobj, struct nvt_touchpanel, kobj);

	return sysfs_emit(buf, "%u\n", READ_ONCE(tp->ts->gesture_enabled));
}

static ssize_t double_tap_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct nvt_touchpanel *tp = container_of(kobj, struct nvt_touchpanel, kobj);
	bool enabled;
	int ret;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	mutex_lock(&tp->ts->ctl_lock);
	WRITE_ONCE(tp->ts->gesture_enabled, enabled);
	mutex_unlock(&tp->ts->ctl_lock);

	return count;
}

static struct kobj_attribute double_tap_attr = __ATTR_RW(double_tap);

static struct attribute *nvt_touchpanel_attrs[] = {
	&double_tap_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(nvt_touchpanel);

static void nvt_touchpanel_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct nvt_touchpanel, kobj));
}

static const struct kobj_type nvt_touchpanel_type = {
	.release = nvt_touchpanel_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = nvt_touchpanel_groups,
};

static void nvt_remove_touchpanel(void *data)
{
	struct nvt_touchpanel *tp = data;

	sysfs_remove_groups(&tp->kobj, nvt_touchpanel_groups);
	kobject_put(&tp->kobj);
}

static int nvt_create_touchpanel(struct nvt_ts *ts)
{
	struct nvt_touchpanel *tp;
	int ret;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;
	tp->ts = ts;
	ret = kobject_init_and_add(&tp->kobj, &nvt_touchpanel_type, NULL,
				   "touchpanel");
	if (ret) {
		kobject_put(&tp->kobj);
		return ret;
	}

	return devm_add_action_or_reset(&ts->spi->dev, nvt_remove_touchpanel, tp);
}

static void nvt_remove_wakeup(void *data)
{
	device_init_wakeup(data, false);
}

static int nvt_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct nvt_ts *ts;
	int ret;

	if (spi->controller->flags & SPI_CONTROLLER_HALF_DUPLEX)
		return dev_err_probe(dev, -EINVAL, "full duplex SPI required\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	ts->panel_off = false;
	mutex_init(&ts->lock);
	mutex_init(&ts->ctl_lock);
	init_completion(&ts->pm_resumed);
	complete_all(&ts->pm_resumed);
	INIT_DELAYED_WORK(&ts->load_work, nvt_load_work);
	INIT_DELAYED_WORK(&ts->follower_work, nvt_follower_work);
	ts->follower.funcs = &nvt_follower_funcs;
	spi_set_drvdata(spi, ts);

	ts->xbuf = devm_kzalloc(dev, NVT_XBUF_LEN, GFP_KERNEL);
	ts->rbuf = devm_kzalloc(dev, NVT_RBUF_LEN, GFP_KERNEL);
	if (!ts->xbuf || !ts->rbuf)
		return -ENOMEM;

	if (device_property_read_u32(dev, "novatek,swrst-n8-addr",
				     &ts->swrst_addr))
		ts->swrst_addr = NVT_SWRST_N8_ADDR_DEFAULT;
	if (device_property_read_u32(dev, "novatek,spi-rd-fast-addr",
				     &ts->rd_fast_addr))
		ts->rd_fast_addr = NVT_SPI_RD_FAST_ADDR_DEFAULT;

	ret = nvt_parse_fw_names(ts);
	if (ret)
		return dev_err_probe(dev, ret, "invalid firmware-name\n");

	ret = devm_add_action_or_reset(dev, nvt_release_fw, ts);
	if (ret)
		return ret;

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to get reset gpio\n");

	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable vdd supply\n");
	if (!ret)
		msleep(NVT_POWER_ON_DELAY_MS);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "SPI setup failed\n");

	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "no interrupt specified\n");
	ts->irq = spi->irq;

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = NVT_TS_NAME;
	ts->input->phys = "input/ts";
	ts->input->id.bustype = BUS_SPI;
	input_set_drvdata(ts->input, ts);
	input_set_capability(ts->input, EV_KEY, KEY_WAKEUP);

	ret = devm_request_threaded_irq(dev, ts->irq, NULL, nvt_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"nt36xxx-spi", ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	ret = device_init_wakeup(dev, true);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, nvt_remove_wakeup, dev);
	if (ret)
		return ret;
	ret = nvt_create_touchpanel(ts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create gesture control\n");

	/* Probe the chip now if the panel left it powered (bootloader splash). */
	mutex_lock(&ts->lock);
	ts->chip_ok = !nvt_identify(ts);
	mutex_unlock(&ts->lock);

	if (drm_is_panel_follower(dev))
		queue_delayed_work(system_wq, &ts->follower_work, 0);

	queue_delayed_work(system_wq, &ts->load_work,
			   msecs_to_jiffies(NVT_BOOT_LOAD_DELAY_MS));

	dev_info(dev, "probed (irq %d, %u Hz, chip %s)\n", ts->irq,
		 spi->max_speed_hz, ts->chip_ok ? "found" : "not responding yet");
	return 0;
}

static void nvt_remove(struct spi_device *spi)
{
	struct nvt_ts *ts = spi_get_drvdata(spi);
	bool follower;

	mutex_lock(&ts->ctl_lock);
	ts->removing = true;
	complete_all(&ts->pm_resumed);
	nvt_irq_set(ts, false);
	nvt_disable_irq_wake(ts);
	mutex_unlock(&ts->ctl_lock);

	cancel_delayed_work_sync(&ts->follower_work);

	mutex_lock(&ts->ctl_lock);
	follower = ts->follower_added;
	mutex_unlock(&ts->ctl_lock);
	if (follower)
		drm_panel_remove_follower(&ts->follower);

	cancel_delayed_work_sync(&ts->load_work);
}

static int nvt_pm_suspend(struct device *dev)
{
	struct nvt_ts *ts = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&ts->load_work);

	mutex_lock(&ts->ctl_lock);
	if (!ts->follower_added || !ts->panel_off)
		nvt_sleep(ts);
	reinit_completion(&ts->pm_resumed);
	WRITE_ONCE(ts->pm_suspended, true);
	mutex_unlock(&ts->ctl_lock);

	return 0;
}

static int nvt_pm_resume(struct device *dev)
{
	struct nvt_ts *ts = dev_get_drvdata(dev);

	WRITE_ONCE(ts->pm_suspended, false);
	complete_all(&ts->pm_resumed);

	mutex_lock(&ts->ctl_lock);
	if (!ts->removing && (!ts->follower_added || !ts->panel_off))
		nvt_schedule_load(ts, 0);
	mutex_unlock(&ts->ctl_lock);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nvt_pm_ops, nvt_pm_suspend, nvt_pm_resume);

static const struct of_device_id nvt_of_match[] = {
	{ .compatible = "novatek,nt36672a-spi" },
	{ .compatible = "novatek,NVT-ts-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_of_match);

static const struct spi_device_id nvt_spi_ids[] = {
	{ "nt36672a-spi" },
	{ "NVT-ts-spi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, nvt_spi_ids);

static struct spi_driver nvt_driver = {
	.driver = {
		.name = "nt36xxx-spi",
		.of_match_table = nvt_of_match,
		.pm = pm_sleep_ptr(&nvt_pm_ops),
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = nvt_spi_ids,
	.probe = nvt_probe,
	.remove = nvt_remove,
};
module_spi_driver(nvt_driver);

MODULE_DESCRIPTION("Novatek NT36xxx host-download SPI touchscreen driver");
MODULE_LICENSE("GPL");