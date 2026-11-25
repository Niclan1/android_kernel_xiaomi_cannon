// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek legacy UFS_IOCTL_RPMB compatibility.
 *
 * Vendor TEE daemons from MediaTek BSPs (e.g. Microtrust teei_daemon) access
 * the UFS RPMB well-known LU by issuing ioctl(UFS_IOCTL_RPMB) on any UFS
 * block device (/dev/block/sdX). The argument is an array of three
 * struct rpmb_cmd entries, each describing a SECURITY PROTOCOL IN/OUT
 * transfer of 512-byte RPMB frames that is sent to the RPMB W-LU in order.
 */

#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_proto.h>
#include <ufs/ufshcd.h>
#include "ufshcd-priv.h"

#define UFS_IOCTL_RPMB			0x5391

#define MTK_RPMB_F_WRITE		BIT(0)
#define MTK_RPMB_NCMDS			3
#define MTK_RPMB_FRAME_SIZE		512
#define MTK_RPMB_DEFAULT_RW_SIZE	64

#define SEC_PROTOCOL_UFS		0xEC
#define SEC_SPECIFIC_UFS_RPMB		0x0001
#define SEC_PROTOCOL_CMD_SIZE		12
#define SEC_PROTOCOL_RETRIES		3
#define SEC_PROTOCOL_RETRIES_ON_RESET	10
#define SEC_PROTOCOL_TIMEOUT		msecs_to_jiffies(30000)

/* Userspace layout: { u32 flags; u32 nframes; struct rpmb_frame *frames __aligned(8); } */
struct mtk_rpmb_cmd {
	u32 flags;
	u32 nframes;
	u64 frames;
};

static DEFINE_MUTEX(mtk_rpmb_lock);

static u32 mtk_rpmb_rw_size(struct ufs_hba *hba)
{
	static u8 rw_size;
	int ret;

	if (rw_size)
		return rw_size;

	ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_GEOMETRY, 0,
				     GEOMETRY_DESC_PARAM_RPMB_RW_SIZE,
				     &rw_size, sizeof(rw_size));
	if (ret || !rw_size) {
		dev_warn(hba->dev, "rpmb: cannot read rw size (%d), using %d\n",
			 ret, MTK_RPMB_DEFAULT_RW_SIZE);
		rw_size = MTK_RPMB_DEFAULT_RW_SIZE;
	} else {
		dev_info(hba->dev, "rpmb: rw_size %u\n", rw_size);
	}

	return rw_size;
}

static int mtk_rpmb_security_cmd(struct scsi_device *sdev, bool write,
				 void *frames, u32 nframes)
{
	struct scsi_sense_hdr sshdr = {};
	const struct scsi_exec_args exec_args = { .sshdr = &sshdr };
	int reset_retries = SEC_PROTOCOL_RETRIES_ON_RESET;
	u32 len = nframes * MTK_RPMB_FRAME_SIZE;
	u8 cmd[SEC_PROTOCOL_CMD_SIZE] = {};
	int ret;

	cmd[0] = write ? SECURITY_PROTOCOL_OUT : SECURITY_PROTOCOL_IN;
	cmd[1] = SEC_PROTOCOL_UFS;
	put_unaligned_be16(SEC_SPECIFIC_UFS_RPMB, cmd + 2);
	put_unaligned_be32(len, cmd + 6);

	ret = scsi_autopm_get_device(sdev);
	if (ret)
		return ret;

	do {
		ret = scsi_execute_cmd(sdev, cmd,
				       write ? REQ_OP_DRV_OUT : REQ_OP_DRV_IN,
				       frames, len, SEC_PROTOCOL_TIMEOUT,
				       SEC_PROTOCOL_RETRIES, &exec_args);
	} while (ret > 0 && scsi_sense_valid(&sshdr) &&
		 sshdr.sense_key == UNIT_ATTENTION && --reset_retries > 0);

	scsi_autopm_put_device(sdev);

	if (ret) {
		sdev_printk(KERN_ERR, sdev, "rpmb: security %s failed: %d\n",
			    write ? "out" : "in", ret);
		if (ret > 0 && scsi_sense_valid(&sshdr))
			scsi_print_sense_hdr(sdev, "rpmb", &sshdr);
	}

	return ret;
}

static int ufshcd_mtk_rpmb_ioctl_common(struct scsi_device *dev,
					void __user *arg, bool compat)
{
	struct ufs_hba *hba = shost_priv(dev->host);
	struct mtk_rpmb_cmd cmds[MTK_RPMB_NCMDS];
	struct scsi_device *sdev;
	void *buf = NULL;
	u32 rw_size;
	int ret, i;

	if (!arg)
		return -EINVAL;

	if (!hba->ufs_device_wlun)
		return -ENODEV;

	if (copy_from_user(cmds, arg, sizeof(cmds)))
		return -EFAULT;

	sdev = scsi_device_lookup(hba->host, 0, 0,
			(UFS_UPIU_RPMB_WLUN & ~UFS_UPIU_WLUN_ID) | SCSI_W_LUN_BASE);
	if (!sdev) {
		dev_err(hba->dev, "rpmb: RPMB W-LU not available\n");
		return -ENODEV;
	}

	ufshcd_rpm_get_sync(hba);
	rw_size = mtk_rpmb_rw_size(hba);

	for (i = 0; i < MTK_RPMB_NCMDS; i++) {
		if (cmds[i].nframes > rw_size) {
			dev_err(hba->dev, "rpmb: %u frames exceeds limit %u\n",
				cmds[i].nframes, rw_size);
			ret = -EINVAL;
			goto out;
		}
	}

	buf = kmalloc_array(rw_size, MTK_RPMB_FRAME_SIZE, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	ret = 0;
	mutex_lock(&mtk_rpmb_lock);
	for (i = 0; i < MTK_RPMB_NCMDS && cmds[i].nframes; i++) {
		size_t size = (size_t)cmds[i].nframes * MTK_RPMB_FRAME_SIZE;
		bool write = cmds[i].flags & MTK_RPMB_F_WRITE;
		void __user *uframes;

		uframes = compat ? compat_ptr((u32)cmds[i].frames) :
				   u64_to_user_ptr(cmds[i].frames);

		if (copy_from_user(buf, uframes, size)) {
			ret = -EFAULT;
			break;
		}

		ret = mtk_rpmb_security_cmd(sdev, write, buf, cmds[i].nframes);
		if (ret)
			break;

		if (!write && copy_to_user(uframes, buf, size)) {
			ret = -EFAULT;
			break;
		}
	}
	mutex_unlock(&mtk_rpmb_lock);

out:
	kfree(buf);
	ufshcd_rpm_put_sync(hba);
	scsi_device_put(sdev);
	return ret;
}

int ufshcd_mtk_rpmb_ioctl(struct scsi_device *dev, unsigned int cmd,
			  void __user *arg)
{
	if (cmd != UFS_IOCTL_RPMB)
		return -ENOIOCTLCMD;

	return ufshcd_mtk_rpmb_ioctl_common(dev, arg, false);
}

#ifdef CONFIG_COMPAT
int ufshcd_mtk_rpmb_compat_ioctl(struct scsi_device *dev, unsigned int cmd,
				 void __user *arg)
{
	if (cmd != UFS_IOCTL_RPMB)
		return -ENOIOCTLCMD;

	return ufshcd_mtk_rpmb_ioctl_common(dev, arg, true);
}
#endif

/*
 * MediaTek legacy /dev/rpmb0 interface used by Microtrust teei_daemon
 * (drivers/char/rpmb/rpmb-mtk.c in the vendor 4.14 tree). Commands are plain
 * numbers; data commands pass { u32 size; u8 frames[size]; }.
 */
#define RPMB_IOCTL_SOTER_WRITE_DATA	5
#define RPMB_IOCTL_SOTER_READ_DATA	6
#define RPMB_IOCTL_SOTER_GET_CNT	7
#define RPMB_IOCTL_SOTER_GET_WR_SIZE	8
#define RPMB_IOCTL_SOTER_SET_KEY	9

#define MTK_RPMB_BUF_SIZE		(24 * 1024)
#define RPMB_F_NONCE			484
#define RPMB_F_WCOUNTER			500
#define RPMB_F_BLKS			506
#define RPMB_F_RESULT			508
#define RPMB_F_REQ			510
#define RPMB_REQ_GET_WC			0x0002
#define RPMB_REQ_RESULT_READ		0x0005

static struct ufs_hba *mtk_rpmb_hba;
static u8 *mtk_rpmb_fbuf;

static int mtk_rpmb_write_seq(struct scsi_device *sdev, u8 *frames, u32 n)
{
	u8 *res;
	int ret;

	res = kzalloc(MTK_RPMB_FRAME_SIZE, GFP_KERNEL);
	if (!res)
		return -ENOMEM;
	ret = mtk_rpmb_security_cmd(sdev, true, frames, n);
	if (!ret) {
		put_unaligned_be16(RPMB_REQ_RESULT_READ, res + RPMB_F_REQ);
		ret = mtk_rpmb_security_cmd(sdev, true, res, 1);
	}
	if (!ret)
		ret = mtk_rpmb_security_cmd(sdev, false, res, 1);
	if (!ret)
		memcpy(frames, res, MTK_RPMB_FRAME_SIZE);
	kfree(res);
	return ret;
}

static long mtk_rpmb_cdev_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct ufs_hba *hba = mtk_rpmb_hba;
	struct scsi_device *sdev;
	u8 *fr = mtk_rpmb_fbuf;
	u8 nonce[16];
	u32 size = 0, n = 0, val = 0;
	int ret;

	if (!hba || !fr)
		return -ENODEV;

	switch (cmd) {
	case RPMB_IOCTL_SOTER_WRITE_DATA:
	case RPMB_IOCTL_SOTER_READ_DATA:
	case RPMB_IOCTL_SOTER_SET_KEY:
		if (copy_from_user(&size, uarg, sizeof(size)))
			return -EFAULT;
		if (!size || size % MTK_RPMB_FRAME_SIZE ||
		    size > MTK_RPMB_BUF_SIZE - 4)
			return -EINVAL;
		n = size / MTK_RPMB_FRAME_SIZE;
		break;
	case RPMB_IOCTL_SOTER_GET_CNT:
	case RPMB_IOCTL_SOTER_GET_WR_SIZE:
		break;
	default:
		return -ENOTTY;
	}

	sdev = scsi_device_lookup(hba->host, 0, 0,
			(UFS_UPIU_RPMB_WLUN & ~UFS_UPIU_WLUN_ID) | SCSI_W_LUN_BASE);
	if (!sdev)
		return -ENODEV;

	ufshcd_rpm_get_sync(hba);
	mutex_lock(&mtk_rpmb_lock);

	if (size && copy_from_user(fr, uarg + 4, size)) {
		ret = -EFAULT;
		goto out;
	}

	switch (cmd) {
	case RPMB_IOCTL_SOTER_WRITE_DATA:
	case RPMB_IOCTL_SOTER_SET_KEY:
		ret = mtk_rpmb_write_seq(sdev, fr, n);
		break;
	case RPMB_IOCTL_SOTER_READ_DATA:
		put_unaligned_be16(n, fr + RPMB_F_BLKS);
		ret = mtk_rpmb_security_cmd(sdev, true, fr, 1);
		if (!ret)
			ret = mtk_rpmb_security_cmd(sdev, false, fr, n);
		break;
	case RPMB_IOCTL_SOTER_GET_CNT:
		memset(fr, 0, MTK_RPMB_FRAME_SIZE);
		get_random_bytes(nonce, sizeof(nonce));
		memcpy(fr + RPMB_F_NONCE, nonce, sizeof(nonce));
		put_unaligned_be16(RPMB_REQ_GET_WC, fr + RPMB_F_REQ);
		ret = mtk_rpmb_security_cmd(sdev, true, fr, 1);
		if (!ret)
			ret = mtk_rpmb_security_cmd(sdev, false, fr, 1);
		if (!ret && (memcmp(fr + RPMB_F_NONCE, nonce, sizeof(nonce)) ||
			     get_unaligned_be16(fr + RPMB_F_RESULT)))
			ret = -EIO;
		val = get_unaligned_be32(fr + RPMB_F_WCOUNTER);
		break;
	default:
		val = mtk_rpmb_rw_size(hba);
		ret = 0;
		break;
	}

	if (ret > 0)
		ret = -EIO;
	if (!ret && size && copy_to_user(uarg + 4, fr, size))
		ret = -EFAULT;
	if (!ret && !size && copy_to_user(uarg, &val, sizeof(val)))
		ret = -EFAULT;
	pr_debug("teei-diag: rpmb0 ioctl %u frames %u ret %d result 0x%x\n",
		cmd, n, ret, get_unaligned_be16(fr + RPMB_F_RESULT));
out:
	mutex_unlock(&mtk_rpmb_lock);
	ufshcd_rpm_put_sync(hba);
	scsi_device_put(sdev);
	return ret;
}

static const struct file_operations mtk_rpmb_cdev_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mtk_rpmb_cdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice mtk_rpmb_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "rpmb0",
	.fops	= &mtk_rpmb_cdev_fops,
	.mode	= 0660,
};

void ufshcd_mtk_rpmb_cdev_init(struct ufs_hba *hba)
{
	int ret;

	if (mtk_rpmb_hba)
		return;
	mtk_rpmb_fbuf = kmalloc(MTK_RPMB_BUF_SIZE, GFP_KERNEL);
	if (!mtk_rpmb_fbuf)
		return;
	mtk_rpmb_hba = hba;
	ret = misc_register(&mtk_rpmb_miscdev);
	if (ret) {
		dev_err(hba->dev, "rpmb: /dev/rpmb0 register failed %d\n", ret);
		mtk_rpmb_hba = NULL;
		kfree(mtk_rpmb_fbuf);
		mtk_rpmb_fbuf = NULL;
		return;
	}
	dev_info(hba->dev, "rpmb: /dev/rpmb0 ready\n");
}