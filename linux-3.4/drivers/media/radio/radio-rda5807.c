/*
 * radio-rda5807.c - Driver for using the RDA5807 FM tuner chip via I2C
 *
 * Copyright (c) 2011 Maarten ter Huurne <maarten@treewalker.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Many thanks to Jérôme VERES for his command line radio application that
 * demonstrates how the chip can be controlled via I2C.
 *
 * The RDA5807 has three ways of accessing registers:
 * - I2C address 0x10: sequential access, RDA5800 style
 * - I2C address 0x11: random access
 * - I2C address 0x60: sequential access, TEA5767 compatible
 * This driver uses random access and therefore the i2c_board_info should
 * specify address 0x11.
 * Note that while there are many similarities, the register map of the RDA5807
 * differs from that of the RDA5800 in several essential places.
 */


#include <asm/byteorder.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>


enum rda5807_reg {
	RDA5807_REG_CHIPID		= 0x00,
	RDA5807_REG_CTRL		= 0x02,
	RDA5807_REG_CHAN		= 0x03,
	RDA5807_REG_IOCFG		= 0x04,
	RDA5807_REG_INTM_THRESH_VOL	= 0x05,
	RDA5807_REG_SEEK_RESULT		= 0x0A,
	RDA5807_REG_SIGNAL		= 0x0B,
};

#define RDA5807_MASK_CTRL_DHIZ		BIT(15)
#define RDA5807_MASK_CTRL_DMUTE		BIT(14)
#define RDA5807_MASK_CTRL_MONO		BIT(13)
#define RDA5807_MASK_CTRL_BASS		BIT(12)
#define RDA5807_MASK_CTRL_SEEKUP	BIT(9)
#define RDA5807_MASK_CTRL_SEEK		BIT(8)
#define RDA5807_MASK_CTRL_SKMODE	BIT(7)
#define RDA5807_MASK_CTRL_CLKMODE	(7 << 4)
#define RDA5807_MASK_CTRL_SOFTRESET	BIT(1)
#define RDA5807_MASK_CTRL_ENABLE	BIT(0)

#define RDA5807_SHIFT_CHAN_WRCHAN	6
#define RDA5807_MASK_CHAN_WRCHAN	(0x3FF << RDA5807_SHIFT_CHAN_WRCHAN)
#define RDA5807_MASK_CHAN_TUNE		BIT(4)
#define RDA5807_SHIFT_CHAN_BAND		2
#define RDA5807_MASK_CHAN_BAND		(0x3 << RDA5807_SHIFT_CHAN_BAND)
#define RDA5807_SHIFT_CHAN_SPACE	0
#define RDA5807_MASK_CHAN_SPACE		(0x3 << RDA5807_SHIFT_CHAN_SPACE)

#define RDA5807_MASK_SEEKRES_COMPLETE	BIT(14)
#define RDA5807_MASK_SEEKRES_FAIL	BIT(13)
#define RDA5807_MASK_SEEKRES_STEREO	BIT(10)

#define RDA5807_MASK_DEEMPHASIS		BIT(11)

#define RDA5807_SHIFT_VOLUME_DAC	0
#define RDA5807_MASK_VOLUME_DAC		(0xF << RDA5807_SHIFT_VOLUME_DAC)

#define RDA5807_SHIFT_RSSI		9
#define RDA5807_MASK_RSSI		(0x7F << RDA5807_SHIFT_RSSI)

#define RDA5807_FREQ_MIN_KHZ  76000
#define RDA5807_FREQ_MAX_KHZ 108000

static int rda5807_i2c_read(struct i2c_client *client, enum rda5807_reg reg)
{
	__u8  reg_buf = reg;
	__u16 val_buf;
	struct i2c_msg msgs[] = {
		{ /* write register number */
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(reg_buf),
			.buf = &reg_buf,
		},
		{ /* read register contents */
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(val_buf),
			.buf = (__u8 *)&val_buf,
		},
	};
	int err;

	err = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (err < 0) return err;
	if (err < ARRAY_SIZE(msgs)) return -EIO;

	dev_info(&client->dev, "reg[%02X] = %04X\n", reg, be16_to_cpu(val_buf));
	return be16_to_cpu(val_buf);
}

static int rda5807_i2c_write(struct i2c_client *client, enum rda5807_reg reg,
			     u16 val)
{
	__u8 buf[] = { reg, val >> 8, val & 0xFF };
	struct i2c_msg msgs[] = {
		{ /* write register number and contents */
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(buf),
			.buf = buf,
		},
	};
	int err;

	err = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (err < 0) return err;
	if (err < ARRAY_SIZE(msgs)) return -EIO;

	dev_info(&client->dev, "reg[%02X] := %04X\n", reg, val);
	return 0;
}

struct rda5807_driver {
	struct v4l2_ctrl_handler	ctrl_handler;
	struct video_device		video_dev;
	struct i2c_client		*i2c_client;
};

static const struct v4l2_file_operations rda5807_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
};

static int rda5807_update_reg(struct rda5807_driver *radio,
			      enum rda5807_reg reg, u16 mask, u16 val)
{
	int err = 0;
	// TODO: Locking.
	//       Or do locking in the caller, in case we ever need to update
	//       two registers in one operation?
	err = rda5807_i2c_read(radio->i2c_client, reg);
	if (err >= 0) {
		val |= ((u16)err & ~mask);
		err = rda5807_i2c_write(radio->i2c_client, reg, val);
	}
	return err;
}

static int rda5807_set_enable(struct rda5807_driver *radio, int enabled)
{
	u16 val = enabled ? RDA5807_MASK_CTRL_ENABLE : 0;
	dev_info(&radio->i2c_client->dev, "set enabled to %d\n", enabled);
	return rda5807_update_reg(radio, RDA5807_REG_CTRL,
				  RDA5807_MASK_CTRL_ENABLE, val);
}

static int rda5807_set_mute(struct rda5807_driver *radio, int muted)
{
	u16 val = muted ? 0 : RDA5807_MASK_CTRL_DMUTE /* disable mute */;
	dev_info(&radio->i2c_client->dev, "set mute to %d\n", muted);
	return rda5807_update_reg(radio, RDA5807_REG_CTRL,
				  RDA5807_MASK_CTRL_DMUTE, val);
}

static int rda5807_set_volume(struct rda5807_driver *radio, int volume)
{
	dev_info(&radio->i2c_client->dev, "set volume to %d\n", volume);
	return rda5807_update_reg(radio, RDA5807_REG_INTM_THRESH_VOL,
				  RDA5807_MASK_VOLUME_DAC,
				  volume << RDA5807_SHIFT_VOLUME_DAC);
}

static int rda5807_set_preemphasis(struct rda5807_driver *radio,
				   enum v4l2_preemphasis preemp)
{
	dev_info(&radio->i2c_client->dev, "set preemphasis to %d\n", preemp);
	return rda5807_update_reg(radio, RDA5807_REG_IOCFG,
				  RDA5807_MASK_DEEMPHASIS,
				  preemp == V4L2_PREEMPHASIS_50_uS
				          ? RDA5807_MASK_DEEMPHASIS : 0);
}

static int rda5807_set_frequency(struct rda5807_driver *radio, u32 freq_khz)
{
	u16 mask = 0;
	u16 val = 0;

	dev_info(&radio->i2c_client->dev, "set freq to %u kHz\n", freq_khz);

	if (freq_khz < RDA5807_FREQ_MIN_KHZ)
		return -ERANGE;
	if (freq_khz > RDA5807_FREQ_MAX_KHZ)
		return -ERANGE;

	/* select widest band */
	mask |= RDA5807_MASK_CHAN_BAND;
	val  |= 2 << RDA5807_SHIFT_CHAN_BAND;
	/* select 50 kHz channel spacing */
	mask |= RDA5807_MASK_CHAN_SPACE;
	val  |= 2 << RDA5807_SHIFT_CHAN_SPACE;
	/* select frequency */
	mask |= RDA5807_MASK_CHAN_WRCHAN;
	val  |= ((freq_khz - RDA5807_FREQ_MIN_KHZ + 25) / 50)
			<< RDA5807_SHIFT_CHAN_WRCHAN;
	/* start tune operation */
	mask |= RDA5807_MASK_CHAN_TUNE;
	val  |= RDA5807_MASK_CHAN_TUNE;

	return rda5807_update_reg(radio, RDA5807_REG_CHAN, mask, val);
}

static inline struct rda5807_driver *ctrl_to_radio(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct rda5807_driver, ctrl_handler);
}

static int rda5807_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rda5807_driver *radio = ctrl_to_radio(ctrl);

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE: {
		/* Disable the radio while muted, to save power.
		 * TODO: We can't seek while the radio is disabled;
		 *       is that a problem?
		 */
		int err1 = rda5807_set_enable(radio, !ctrl->val);
		int err2 = rda5807_set_mute(radio, ctrl->val);
		return err1 ? err1 : err2;
	}
	case V4L2_CID_AUDIO_VOLUME:
		return rda5807_set_volume(radio, ctrl->val);
	case V4L2_CID_TUNE_PREEMPHASIS:
		return rda5807_set_preemphasis(radio, ctrl->val);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops rda5807_ctrl_ops = {
	.s_ctrl = rda5807_s_ctrl,
};

static int rda5807_vidioc_g_audio(struct file *file, void *fh,
				  struct v4l2_audio *a)
{
	if (a->index != 0)
		return -EINVAL;

	*a = (struct v4l2_audio) {
		.name = "Radio",
		.capability = V4L2_AUDCAP_STEREO,
		.mode = 0,
	};

	return 0;
}

static int rda5807_vidioc_g_tuner(struct file *file, void *fh,
				  struct v4l2_tuner *a)
{
	struct rda5807_driver *radio = video_drvdata(file);
	int err;
	u16 seekres, signal;
	__u32 rxsubchans;

	if (a->index != 0)
		return -EINVAL;

	err = rda5807_i2c_read(radio->i2c_client, RDA5807_REG_SEEK_RESULT);
	if (err < 0)
		return err;
	seekres = (u16)err;
	if ((seekres & (RDA5807_MASK_SEEKRES_COMPLETE
						| RDA5807_MASK_SEEKRES_FAIL))
				== RDA5807_MASK_SEEKRES_COMPLETE)
		/* mono/stereo known */
		rxsubchans = seekres & RDA5807_MASK_SEEKRES_STEREO
				? V4L2_TUNER_SUB_STEREO : V4L2_TUNER_SUB_MONO;
	else
		/* mono/stereo unknown */
		rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;

	err = rda5807_i2c_read(radio->i2c_client, RDA5807_REG_SIGNAL);
	if (err < 0)
		return err;
	signal = ((u16)err & RDA5807_MASK_RSSI) >> RDA5807_SHIFT_RSSI;

	*a = (struct v4l2_tuner) {
		.name = "FM",
		.type = V4L2_TUNER_RADIO,
		.capability = V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO,
		/* unit is 1/16 kHz */
		.rangelow   = RDA5807_FREQ_MIN_KHZ * 16,
		.rangehigh  = RDA5807_FREQ_MAX_KHZ * 16,
		.rxsubchans = rxsubchans,
		/* TODO: Implement forced mono (RDA5807_MASK_CTRL_MONO). */
		.audmode = V4L2_TUNER_MODE_STEREO,
		.signal = signal << (16 - 7),
		.afc = 0, /* automatic frequency control */
	};

	return 0;
}

static int rda5807_vidioc_s_frequency(struct file *file, void *fh,
				      struct v4l2_frequency *a)
{
	struct rda5807_driver *radio = video_drvdata(file);

	if (a->tuner != 0)
		return -EINVAL;
	if (a->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	return rda5807_set_frequency(radio, (a->frequency * 625) / 10000);
}

static const struct v4l2_ioctl_ops rda5807_ioctl_ops = {
	.vidioc_g_audio     = rda5807_vidioc_g_audio,
	.vidioc_g_tuner     = rda5807_vidioc_g_tuner,
	.vidioc_s_frequency = rda5807_vidioc_s_frequency,
};

static int __devinit rda5807_i2c_probe(struct i2c_client *client,
				       const struct i2c_device_id *id)
{
	struct rda5807_driver *radio;
	int chipid;
	int err;

	chipid = rda5807_i2c_read(client, RDA5807_REG_CHIPID);
	if (chipid < 0) {
		dev_warn(&client->dev, "Failed to read chip ID (%d)\n", chipid);
		return chipid;
	}
	if ((chipid & 0xFF00) != 0x5800) {
		dev_warn(&client->dev, "Chip ID mismatch: "
				       "expected 58xx, got %04X\n", chipid);
		return -ENODEV;
	}
	dev_info(&client->dev, "Found FM radio receiver\n");

	// TODO: Resetting the chip would be good.

	radio = kzalloc(sizeof(*radio), GFP_KERNEL);
	if (!radio) {
		dev_warn(&client->dev, "Failed to allocate driver data\n");
		return -ENOMEM;
	}

	radio->i2c_client = client;

	/* Initialize controls. */
	v4l2_ctrl_handler_init(&radio->ctrl_handler, 3);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_MUTE, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&radio->ctrl_handler, &rda5807_ctrl_ops,
			  V4L2_CID_AUDIO_VOLUME, 0, 15, 1, 8);
	v4l2_ctrl_new_std_menu(&radio->ctrl_handler, &rda5807_ctrl_ops,
			       V4L2_CID_TUNE_PREEMPHASIS,
			       V4L2_PREEMPHASIS_75_uS,
			       BIT(V4L2_PREEMPHASIS_DISABLED),
			       V4L2_PREEMPHASIS_50_uS);
	err = radio->ctrl_handler.error;
	if (err) {
		dev_warn(&client->dev, "Failed to init controls handler"
			 " (%d)\n", err);
		goto err_ctrl_free;
	}

	radio->video_dev = (struct video_device) {
		.name = "RDA5807 FM receiver",
		.flags = V4L2_DEBUG_IOCTL | V4L2_DEBUG_IOCTL_ARG,
		.ctrl_handler = &radio->ctrl_handler,
		.fops = &rda5807_fops,
		.ioctl_ops = &rda5807_ioctl_ops,
		.release = video_device_release_empty,
		//.lock = &radio->lock,
	};
	i2c_set_clientdata(client, radio);
	video_set_drvdata(&radio->video_dev, radio);

	err = video_register_device(&radio->video_dev, VFL_TYPE_RADIO, -1);
	if (err < 0) {
		dev_warn(&client->dev, "Failed to register video device (%d)\n",
				       err);
		goto err_ctrl_free;
	}

	err = v4l2_ctrl_handler_setup(&radio->ctrl_handler);
	if (err < 0) {
		dev_warn(&client->dev, "Failed to set default control values"
				       " (%d)\n", err);
		goto err_video_unreg;
	}

	return 0;

err_video_unreg:
	video_unregister_device(&radio->video_dev);

err_ctrl_free:
	v4l2_ctrl_handler_free(&radio->ctrl_handler);

/*err_radio_rel:*/
	video_device_release_empty(&radio->video_dev);
	kfree(radio);

	return err;
}

static int __devexit rda5807_i2c_remove(struct i2c_client *client)
{
	struct rda5807_driver *radio = i2c_get_clientdata(client);

	video_unregister_device(&radio->video_dev);
	v4l2_ctrl_handler_free(&radio->ctrl_handler);
	video_device_release_empty(&radio->video_dev);
	kfree(radio);

	return 0;
}

#ifdef CONFIG_PM

static int rda5807_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);

	return rda5807_set_enable(radio, 0);
}

static int rda5807_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rda5807_driver *radio = i2c_get_clientdata(client);
	struct v4l2_ctrl *mute_ctrl = v4l2_ctrl_find(&radio->ctrl_handler,
						     V4L2_CID_AUDIO_MUTE);
	s32 mute_val = v4l2_ctrl_g_ctrl(mute_ctrl);
	int enabled = !mute_val;

	if (enabled)
		return rda5807_set_enable(radio, enabled);
	else
		return 0;
}

static SIMPLE_DEV_PM_OPS(rda5807_pm_ops, rda5807_suspend, rda5807_resume);
#define RDA5807_PM_OPS (&rda5807_pm_ops)

#else

#define RDA5807_PM_OPS NULL

#endif

static const struct i2c_device_id rda5807_id[] = {
	{ "radio-rda5807", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rda5807_id);

static struct i2c_driver rda5807_i2c_driver = {
	.probe = rda5807_i2c_probe,
	.remove = __devexit_p(rda5807_i2c_remove),
	.id_table = rda5807_id,
	.driver = {
		.name	= "radio-rda5807",
		.owner	= THIS_MODULE,
		.pm	= RDA5807_PM_OPS,
	},
};

static int __init rda5807_init(void)
{
	return i2c_add_driver(&rda5807_i2c_driver);
}

static void __exit rda5807_exit(void)
{
	i2c_del_driver(&rda5807_i2c_driver);
}

module_init(rda5807_init);
module_exit(rda5807_exit);

MODULE_AUTHOR("Maarten ter Huurne <maarten@treewalker.org>");
MODULE_DESCRIPTION("RDA5807 FM tuner driver");
MODULE_LICENSE("GPL");
