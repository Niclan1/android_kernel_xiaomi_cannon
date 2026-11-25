// SPDX-License-Identifier: GPL-2.0
/*
 *  mt6853-mt6359.c  --  mt6853 mt6359 ALSA SoC machine driver
 *
 *  Copyright (c) 2019 MediaTek Inc.
 *  Author: Shane Chien <shane.chien@mediatek.com>
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt6853-afe-common.h"
#include "../common/mtk-afe-platform-driver.h"

static struct mtk_base_afe *mt6853_card_afe_from_rtd(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);

	return snd_soc_component_get_drvdata(component);
}
#include "mt6853-afe-clk.h"
#include "mt6853-afe-gpio.h"
#include "../../codecs/mt6359.h"
#include "../../codecs/mt6359-accdet.h"
#include "../common/mtk-sp-spk-amp.h"

#ifdef CONFIG_SND_SOC_AW87339
#include "aw87339.h"
#endif

/*
 * if need additional control for the ext spk amp that is connected
 * after Lineout Buffer / HP Buffer on the codec, put the control in
 * mt6853_mt6359_spk_amp_event()
 */
#define EXT_SPK_AMP_W_NAME "Ext_Speaker_Amp"
#define EXT_RCV_AMP_W_NAME "Ext_Receiver_Amp"

extern unsigned char aw87359_audio_dspk(void);
extern unsigned char aw87359_audio_abrcv(void);
extern unsigned char aw87359_audio_off(void);

extern unsigned char aw87519_audio_kspk(void);
extern unsigned char aw87519_audio_drcv(void);
extern unsigned char aw87519_audio_hvload(void);
extern unsigned char aw87519_audio_off(void);

static const char *const mt6853_spk_type_str[] = {MTK_SPK_NOT_SMARTPA_STR,
						  MTK_SPK_RICHTEK_RT5509_STR,
						  MTK_SPK_MEDIATEK_MT6660_STR,
						  MTK_SPK_NXP_TFA98XX_STR
						  };
static const char *const
	mt6853_spk_i2s_type_str[] = {MTK_SPK_I2S_0_STR,
				     MTK_SPK_I2S_1_STR,
				     MTK_SPK_I2S_2_STR,
				     MTK_SPK_I2S_3_STR,
				     MTK_SPK_I2S_5_STR,
				     MTK_SPK_I2S_6_STR,
				     MTK_SPK_I2S_7_STR,
				     MTK_SPK_I2S_8_STR,
				     MTK_SPK_I2S_9_STR,
				     MTK_SPK_TINYCONN_I2S_0_STR,
				     };

static const struct soc_enum mt6853_spk_type_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt6853_spk_type_str),
			    mt6853_spk_type_str),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt6853_spk_i2s_type_str),
			    mt6853_spk_i2s_type_str),
};

static int mt6853_spk_type_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	int idx = mtk_spk_get_type();

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int mt6853_spk_i2s_out_type_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	int idx = mtk_spk_get_i2s_out_type();

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int mt6853_spk_i2s_in_type_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	int idx = mtk_spk_get_i2s_in_type();

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int rcv_amp_mode;
static const char *rcv_amp_type_str[] = {"SPEAKER_MODE", "RECEIVER_MODE"};
static const struct soc_enum rcv_amp_type_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(rcv_amp_type_str), rcv_amp_type_str);

static int mt6853_rcv_amp_mode_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	pr_info("%s() = %d\n", __func__, rcv_amp_mode);
	ucontrol->value.integer.value[0] = rcv_amp_mode;
	return 0;
}

static int mt6853_rcv_amp_mode_set(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	if (ucontrol->value.enumerated.item[0] >= e->items)
		return -EINVAL;

	rcv_amp_mode = ucontrol->value.integer.value[0];
	pr_info("%s() = %d\n", __func__, rcv_amp_mode);
	return 0;
}

static int mt6853_mt6359_spk_amp_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol,
				       int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s(), event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* spk amp on control */
#ifdef CONFIG_SND_SOC_AW87339
		aw87339_spk_enable_set(true);
#endif
		aw87519_audio_kspk();
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* spk amp off control */
#ifdef CONFIG_SND_SOC_AW87339
		aw87339_spk_enable_set(false);
#endif
		aw87519_audio_off();
		break;
	default:
		break;
	}

	return 0;
};

static int mt6853_mt6359_rcv_amp_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol,
				       int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s(), event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* rcv amp on control */
		if (rcv_amp_mode) {
			pr_info("%s(), aw87359_audio_abrcv()\n", __func__);
			aw87359_audio_abrcv();
		} else {
			pr_info("%s(), aw87359_audio_dspk()\n", __func__);
			aw87359_audio_dspk();
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* rcv amp off control */
		aw87359_audio_off();
		break;
	default:
		break;
	}

	return 0;
};

static const struct snd_soc_dapm_widget mt6853_mt6359_widgets[] = {
	SND_SOC_DAPM_SPK(EXT_SPK_AMP_W_NAME, mt6853_mt6359_spk_amp_event),
	SND_SOC_DAPM_SPK(EXT_RCV_AMP_W_NAME, mt6853_mt6359_rcv_amp_event),
};

static const struct snd_soc_dapm_route mt6853_mt6359_routes[] = {
	{EXT_SPK_AMP_W_NAME, NULL, "LINEOUT L"},
	{EXT_RCV_AMP_W_NAME, NULL, "Receiver"},
	{EXT_SPK_AMP_W_NAME, NULL, "Headphone L Ext Spk Amp"},
	{EXT_RCV_AMP_W_NAME, NULL, "Headphone R Ext Spk Amp"},
};

static const struct snd_kcontrol_new mt6853_mt6359_controls[] = {
	SOC_DAPM_PIN_SWITCH(EXT_SPK_AMP_W_NAME),
	SOC_DAPM_PIN_SWITCH(EXT_RCV_AMP_W_NAME),
	SOC_ENUM_EXT("RCV_AMP_MODE", rcv_amp_type_enum,
		     mt6853_rcv_amp_mode_get, mt6853_rcv_amp_mode_set),
	SOC_ENUM_EXT("MTK_SPK_TYPE_GET", mt6853_spk_type_enum[0],
		     mt6853_spk_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_OUT_TYPE_GET", mt6853_spk_type_enum[1],
		     mt6853_spk_i2s_out_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_IN_TYPE_GET", mt6853_spk_type_enum[1],
		     mt6853_spk_i2s_in_type_get, NULL),
};

/*
 * define mtk_spk_i2s_mck node in dts when need mclk,
 * BE i2s need assign snd_soc_ops = mt6853_mt6359_i2s_ops
 */
static int mt6853_mt6359_i2s_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs_ratio = 128;
	unsigned int mclk_fs = rate * mclk_fs_ratio;

	return snd_soc_dai_set_sysclk(snd_soc_rtd_to_cpu(rtd, 0),
				      0, mclk_fs, SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops mt6853_mt6359_i2s_ops = {
	.hw_params = mt6853_mt6359_i2s_hw_params,
};

static int mt6853_mt6359_mtkaif_calibration(struct snd_soc_pcm_runtime *rtd)
{
	struct mtk_base_afe *afe = mt6853_card_afe_from_rtd(rtd);
	struct mt6853_afe_private *afe_priv = afe->platform_priv;
	int phase;
	unsigned int monitor;
	int test_done_1, test_done_2, test_done_3;
	int cycle_1, cycle_2, cycle_3;
	int prev_cycle_1, prev_cycle_2, prev_cycle_3;
	int counter;
	int mtkaif_calib_ok;

	dev_info(afe->dev, "%s(), start\n", __func__);

	pm_runtime_get_sync(afe->dev);
	mt6853_afe_gpio_request(afe, true, MT6853_DAI_ADDA, 1);
	mt6853_afe_gpio_request(afe, true, MT6853_DAI_ADDA, 0);
	mt6853_afe_gpio_request(afe, true, MT6853_DAI_ADDA_CH34, 1);
	mt6853_afe_gpio_request(afe, true, MT6853_DAI_ADDA_CH34, 0);

	mt6359_mtkaif_calibration_enable(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME));

	/* set clock protocol 2 */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x38);
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x39);

	/* set test type to synchronizer pulse */
	regmap_update_bits(afe_priv->topckgen,
			   CKSYS_AUD_TOP_CFG, 0xffff, 0x4);

	mtkaif_calib_ok = true;
	afe_priv->mtkaif_calibration_num_phase = 42;	/* mt6359: 0 ~ 42 */
	afe_priv->mtkaif_chosen_phase[0] = -1;
	afe_priv->mtkaif_chosen_phase[1] = -1;
	afe_priv->mtkaif_chosen_phase[2] = -1;

	for (phase = 0;
	     phase <= afe_priv->mtkaif_calibration_num_phase &&
	     mtkaif_calib_ok;
	     phase++) {
		mt6359_set_mtkaif_calibration_phase(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME),
						    phase, phase, phase);

		regmap_update_bits(afe_priv->topckgen,
				   CKSYS_AUD_TOP_CFG, 0x1, 0x1);

		test_done_1 = 0;
		test_done_2 = 0;
		test_done_3 = 0;
		cycle_1 = -1;
		cycle_2 = -1;
		cycle_3 = -1;
		counter = 0;
		while (test_done_1 == 0 ||
		       test_done_2 == 0 ||
		       test_done_3 == 0) {
			regmap_read(afe_priv->topckgen,
				    CKSYS_AUD_TOP_MON, &monitor);

			test_done_1 = (monitor >> 28) & 0x1;
			test_done_2 = (monitor >> 29) & 0x1;
			test_done_3 = (monitor >> 30) & 0x1;
			if (test_done_1 == 1)
				cycle_1 = monitor & 0xf;

			if (test_done_2 == 1)
				cycle_2 = (monitor >> 4) & 0xf;

			if (test_done_3 == 1)
				cycle_3 = (monitor >> 8) & 0xf;

			/* handle if never test done */
			if (++counter > 10000) {
				dev_err(afe->dev, "%s(), test fail, cycle_1 %d, cycle_2 %d, cycle_3 %d, monitor 0x%x\n",
					__func__,
					cycle_1, cycle_2, cycle_3, monitor);
				mtkaif_calib_ok = false;
				break;
			}
		}

		if (phase == 0) {
			prev_cycle_1 = cycle_1;
			prev_cycle_2 = cycle_2;
			prev_cycle_3 = cycle_3;
		}

		if (cycle_1 != prev_cycle_1 &&
		    afe_priv->mtkaif_chosen_phase[0] < 0) {
			afe_priv->mtkaif_chosen_phase[0] = phase - 1;
			afe_priv->mtkaif_phase_cycle[0] = prev_cycle_1;
		}

		if (cycle_2 != prev_cycle_2 &&
		    afe_priv->mtkaif_chosen_phase[1] < 0) {
			afe_priv->mtkaif_chosen_phase[1] = phase - 1;
			afe_priv->mtkaif_phase_cycle[1] = prev_cycle_2;
		}

		if (cycle_3 != prev_cycle_3 &&
		    afe_priv->mtkaif_chosen_phase[2] < 0) {
			afe_priv->mtkaif_chosen_phase[2] = phase - 1;
			afe_priv->mtkaif_phase_cycle[2] = prev_cycle_3;
		}

		regmap_update_bits(afe_priv->topckgen,
				   CKSYS_AUD_TOP_CFG, 0x1, 0x0);

		if (afe_priv->mtkaif_chosen_phase[0] >= 0 &&
		    afe_priv->mtkaif_chosen_phase[1] >= 0 &&
		    afe_priv->mtkaif_chosen_phase[2] >= 0)
			break;
	}

	mt6359_set_mtkaif_calibration_phase(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME),
		(afe_priv->mtkaif_chosen_phase[0] < 0) ?
		0 : afe_priv->mtkaif_chosen_phase[0],
		(afe_priv->mtkaif_chosen_phase[1] < 0) ?
		0 : afe_priv->mtkaif_chosen_phase[1],
		(afe_priv->mtkaif_chosen_phase[2] < 0) ?
		0 : afe_priv->mtkaif_chosen_phase[2]);

	/* disable rx fifo */
	regmap_update_bits(afe->regmap, AFE_AUD_PAD_TOP, 0xff, 0x38);

	mt6359_mtkaif_calibration_disable(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME));

	mt6853_afe_gpio_request(afe, false, MT6853_DAI_ADDA, 1);
	mt6853_afe_gpio_request(afe, false, MT6853_DAI_ADDA, 0);
	mt6853_afe_gpio_request(afe, false, MT6853_DAI_ADDA_CH34, 1);
	mt6853_afe_gpio_request(afe, false, MT6853_DAI_ADDA_CH34, 0);
	pm_runtime_put(afe->dev);

	dev_info(afe->dev, "%s(), mtkaif_chosen_phase[0/1/2]:%d/%d/%d\n",
		 __func__,
		 afe_priv->mtkaif_chosen_phase[0],
		 afe_priv->mtkaif_chosen_phase[1],
		 afe_priv->mtkaif_chosen_phase[2]);

	return 0;
}

static struct snd_soc_component *mt6853_accdet;
static int accdet_defer_cnt;
static struct snd_soc_jack mt6853_headset_jack;

/* Android reads the jack state from the SW_HEADPHONE/MICROPHONE_INSERT input events */
static void mt6853_mt6359_accdet_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret;

	if (!mt6853_accdet)
		return;

	ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
				    SND_JACK_HEADSET | SND_JACK_BTN_0 |
				    SND_JACK_BTN_1 | SND_JACK_BTN_2 |
				    SND_JACK_BTN_3,
				    &mt6853_headset_jack);
	if (ret) {
		dev_err(rtd->dev, "headset jack create failed %d\n", ret);
		return;
	}

	ret = mt6359_accdet_enable_jack_detect(mt6853_accdet,
					       &mt6853_headset_jack);
	if (ret)
		dev_err(rtd->dev, "headset jack enable failed %d\n", ret);
}

static int mt6853_mt6359_init(struct snd_soc_pcm_runtime *rtd)
{
	struct mt6359_codec_ops ops;
	struct mtk_base_afe *afe = mt6853_card_afe_from_rtd(rtd);
	struct mt6853_afe_private *afe_priv = afe->platform_priv;
	struct snd_soc_dapm_context *dapm = &rtd->card->dapm;

	ops.enable_dc_compensation = mt6853_enable_dc_compensation;
	ops.set_lch_dc_compensation = mt6853_set_lch_dc_compensation;
	ops.set_rch_dc_compensation = mt6853_set_rch_dc_compensation;
	ops.adda_dl_gain_control = mt6853_adda_dl_gain_control;
	mt6359_set_codec_ops(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME), &ops);

	/* set mtkaif protocol */
	mt6359_set_mtkaif_protocol(snd_soc_rtdcom_lookup(rtd, CODEC_MT6359_NAME),
				   MT6359_MTKAIF_PROTOCOL_2_CLK_P2);
	afe_priv->mtkaif_protocol = MTKAIF_PROTOCOL_2_CLK_P2;

	/* mtkaif calibration */
	if (afe_priv->mtkaif_protocol == MTKAIF_PROTOCOL_2_CLK_P2)
		mt6853_mt6359_mtkaif_calibration(rtd);

	/* disable ext amp connection */
	snd_soc_dapm_disable_pin(dapm, EXT_SPK_AMP_W_NAME);
	snd_soc_dapm_disable_pin(dapm, EXT_RCV_AMP_W_NAME);

	mt6853_mt6359_accdet_init(rtd);

	return 0;
}

static int mt6853_i2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	dev_info(rtd->dev, "%s(), fix format to 32bit\n", __func__);

	/* fix BE i2s format to 32bit, clean param mask first */
	snd_mask_reset_range(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			     0, SNDRV_PCM_FORMAT_LAST);

	params_set_format(params, SNDRV_PCM_FORMAT_S32_LE);
	return 0;
}

#ifdef CONFIG_MTK_VOW_SUPPORT
static const struct snd_pcm_hardware mt6853_mt6359_vow_hardware = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.period_bytes_min = 256,
	.period_bytes_max = 2 * 1024,
	.periods_min = 2,
	.periods_max = 4,
	.buffer_bytes_max = 2 * 2 * 1024,
};

static int mt6853_mt6359_vow_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_afe *afe = mt6853_card_afe_from_rtd(rtd);
	struct snd_soc_component *component;
	struct snd_soc_rtdcom_list *rtdcom;

	dev_info(afe->dev, "%s(), start\n", __func__);
	snd_soc_set_runtime_hwparams(substream, &mt6853_mt6359_vow_hardware);

	mt6853_afe_gpio_request(afe, true, MT6853_DAI_VOW, 0);

	/* ASoC will call pm_runtime_get, but vow don't need */
	for_each_rtdcom(rtd, rtdcom) {
		component = rtdcom->component;
		pm_runtime_put_autosuspend(component->dev);
	}

	return 0;
}

static void mt6853_mt6359_vow_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct mtk_base_afe *afe = mt6853_card_afe_from_rtd(rtd);
	struct snd_soc_component *component;
	struct snd_soc_rtdcom_list *rtdcom;

	dev_info(afe->dev, "%s(), end\n", __func__);
	mt6853_afe_gpio_request(afe, false, MT6853_DAI_VOW, 0);

	/* restore to fool ASoC */
	for_each_rtdcom(rtd, rtdcom) {
		component = rtdcom->component;
		pm_runtime_get_sync(component->dev);
	}
}

static const struct snd_soc_ops mt6853_mt6359_vow_ops = {
	.startup = mt6853_mt6359_vow_startup,
	.shutdown = mt6853_mt6359_vow_shutdown,
};
#endif  // #ifdef CONFIG_MTK_VOW_SUPPORT

static struct snd_soc_dai_link_component mt6853_mt6359_link_0_cpus[] = {
	{ .dai_name = "DL1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_0_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_0_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_1_cpus[] = {
	{ .dai_name = "DL12" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_1_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_1_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_2_cpus[] = {
	{ .dai_name = "DL2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_2_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_2_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_3_cpus[] = {
	{ .dai_name = "DL3" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_3_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_3_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_4_cpus[] = {
	{ .dai_name = "DL4" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_4_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_4_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_5_cpus[] = {
	{ .dai_name = "DL5" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_5_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_5_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_6_cpus[] = {
	{ .dai_name = "DL6" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_6_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_6_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_7_cpus[] = {
	{ .dai_name = "DL7" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_7_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_7_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_8_cpus[] = {
	{ .dai_name = "DL8" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_8_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_8_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_9_cpus[] = {
	{ .dai_name = "DL9" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_9_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_9_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_10_cpus[] = {
	{ .dai_name = "UL1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_10_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_10_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_11_cpus[] = {
	{ .dai_name = "UL2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_11_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_11_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_12_cpus[] = {
	{ .dai_name = "UL3" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_12_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_12_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_13_cpus[] = {
	{ .dai_name = "UL4" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_13_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_13_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_14_cpus[] = {
	{ .dai_name = "UL5" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_14_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_14_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_15_cpus[] = {
	{ .dai_name = "UL6" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_15_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_15_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_16_cpus[] = {
	{ .dai_name = "UL7" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_16_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_16_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_17_cpus[] = {
	{ .dai_name = "UL8" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_17_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_17_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_18_cpus[] = {
	{ .dai_name = "UL_MONO_1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_18_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_18_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_19_cpus[] = {
	{ .dai_name = "UL_MONO_2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_19_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_19_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_20_cpus[] = {
	{ .dai_name = "UL_MONO_3" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_20_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_20_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_21_cpus[] = {
	{ .dai_name = "HDMI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_21_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_21_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_22_cpus[] = {
	{ .dai_name = "Hostless LPBK DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_22_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_22_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_23_cpus[] = {
	{ .dai_name = "Hostless FM DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_23_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_23_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_24_cpus[] = {
	{ .dai_name = "Hostless Speech DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_24_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_24_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_25_cpus[] = {
	{ .dai_name = "Hostless_Sph_Echo_Ref_DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_25_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_25_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_26_cpus[] = {
	{ .dai_name = "Hostless_Spk_Init_DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_26_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_26_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_27_cpus[] = {
	{ .dai_name = "Hostless_ADDA_DL_I2S_OUT DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_27_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_27_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_28_cpus[] = {
	{ .dai_name = "Hostless_SRC_1_DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_28_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_28_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_29_cpus[] = {
	{ .dai_name = "Hostless_SRC_Bargein_DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_29_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_29_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_30_cpus[] = {
	{ .dai_name = "ADDA" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_30_codecs[] = {
	{ .name = NULL, .dai_name = "mt6359-snd-codec-aif1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_30_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_31_cpus[] = {
	{ .dai_name = "ADDA_CH34" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_31_codecs[] = {
	{ .name = NULL, .dai_name = "mt6359-snd-codec-aif2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_31_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_32_cpus[] = {
	{ .dai_name = "AP_DMIC" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_32_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_32_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_33_cpus[] = {
	{ .dai_name = "AP_DMIC_CH34" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_33_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_33_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_34_cpus[] = {
	{ .dai_name = "I2S3" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_34_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_34_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_35_cpus[] = {
	{ .dai_name = "I2S0" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_35_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_35_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_36_cpus[] = {
	{ .dai_name = "I2S1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_36_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_36_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_37_cpus[] = {
	{ .dai_name = "I2S2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_37_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_37_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_38_cpus[] = {
	{ .dai_name = "I2S5" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_38_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_38_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_39_cpus[] = {
	{ .dai_name = "HW Gain 1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_39_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_39_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_40_cpus[] = {
	{ .dai_name = "HW Gain 2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_40_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_40_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_41_cpus[] = {
	{ .dai_name = "HW_SRC_1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_41_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_41_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_42_cpus[] = {
	{ .dai_name = "HW_SRC_2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_42_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_42_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_43_cpus[] = {
	{ .dai_name = "CONNSYS_I2S" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_43_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_43_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_44_cpus[] = {
	{ .dai_name = "PCM 1" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_44_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_44_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_45_cpus[] = {
	{ .dai_name = "PCM 2" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_45_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_45_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_46_cpus[] = {
	{ .dai_name = "TDM" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_46_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_46_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_47_cpus[] = {
	{ .dai_name = "Hostless_UL1 DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_47_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_47_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_48_cpus[] = {
	{ .dai_name = "Hostless_UL2 DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_48_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_48_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_49_cpus[] = {
	{ .dai_name = "Hostless_UL3 DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_49_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_49_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_50_cpus[] = {
	{ .dai_name = "Hostless_UL6 DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_50_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_50_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_51_cpus[] = {
	{ .dai_name = "Hostless_DSP_DL DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_51_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_51_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_52_cpus[] = {
	{ .dai_name = "Hostless HW Gain AAudio DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_52_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_52_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_53_cpus[] = {
	{ .dai_name = "Hostless SRC AAudio DAI" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_53_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_53_platforms[] = {
	{ .name = NULL },
};

#ifdef CONFIG_SND_SOC_MTK_BTCVSD
static struct snd_soc_dai_link_component mt6853_mt6359_link_54_cpus[] = {
	{ .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_54_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_54_platforms[] = {
	{ .name = "18050000.mtk-btcvsd-snd" },
};
#endif


#if defined(CONFIG_SND_SOC_MTK_AUDIO_DSP)
static struct snd_soc_dai_link_component mt6853_mt6359_link_55_cpus[] = {
	{ .dai_name = "audio_task_offload_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_55_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_55_platforms[] = {
	{ .name = "mt_soc_offload_common" },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_56_cpus[] = {
	{ .dai_name = "audio_task_voip_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_56_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_56_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_57_cpus[] = {
	{ .dai_name = "audio_task_primary_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_57_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_57_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_58_cpus[] = {
	{ .dai_name = "audio_task_deepbuf_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_58_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_58_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_59_cpus[] = {
	{ .dai_name = "audio_task_Playback_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_59_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_59_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_60_cpus[] = {
	{ .dai_name = "audio_task_capture_ul1_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_60_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_60_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_61_cpus[] = {
	{ .dai_name = "audio_task_call_final_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_61_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_61_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_62_cpus[] = {
	{ .dai_name = "audio_task_fast_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_62_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_62_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_63_cpus[] = {
	{ .dai_name = "audio_task_ktv_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_63_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_63_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_64_cpus[] = {
	{ .dai_name = "audio_task_capture_raw_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_64_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_64_platforms[] = {
	{ .name = "snd_audio_dsp" },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_65_cpus[] = {
	{ .dai_name = "audio_task_fm_adsp_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_65_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_65_platforms[] = {
	{ .name = NULL },
};

static struct snd_soc_dai_link_component mt6853_mt6359_link_66_cpus[] = {
	{ .dai_name = "audio_task_a2dp_dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_66_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_66_platforms[] = {
	{ .name = NULL },
};
#endif


#ifdef CONFIG_MTK_VOW_SUPPORT
static struct snd_soc_dai_link_component mt6853_mt6359_link_67_cpus[] = {
	{ .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_67_codecs[] = {
	{ .name = NULL, .dai_name = "mt6359-snd-codec-vow" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_67_platforms[] = {
	{ .name = NULL },
};
#endif


#if defined(CONFIG_SND_SOC_MTK_SCP_SMARTPA)
static struct snd_soc_dai_link_component mt6853_mt6359_link_68_cpus[] = {
	{ .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_68_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_68_platforms[] = {
	{ .name = "snd_scp_spk" },
};
#endif


#if defined(CONFIG_MTK_ULTRASND_PROXIMITY)
static struct snd_soc_dai_link_component mt6853_mt6359_link_69_cpus[] = {
	{ .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_69_codecs[] = {
	{ .name = "snd-soc-dummy", .dai_name = "snd-soc-dummy-dai" },
};
static struct snd_soc_dai_link_component mt6853_mt6359_link_69_platforms[] = {
	{ .name = "snd_scp_ultra" },
};
#endif


static struct snd_soc_dai_link mt6853_mt6359_dai_links[] = {
	/* Front End DAI links */
	{
		.name = "Playback_1",
		.cpus = mt6853_mt6359_link_0_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_0_cpus),
		.codecs = mt6853_mt6359_link_0_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_0_codecs),
		.platforms = mt6853_mt6359_link_0_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_0_platforms),
		.playback_only = 1,
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_12",
		.cpus = mt6853_mt6359_link_1_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_1_cpus),
		.codecs = mt6853_mt6359_link_1_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_1_codecs),
		.platforms = mt6853_mt6359_link_1_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_1_platforms),
		.playback_only = 1,
		.stream_name = "Playback_12",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_2",
		.cpus = mt6853_mt6359_link_2_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_2_cpus),
		.codecs = mt6853_mt6359_link_2_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_2_codecs),
		.platforms = mt6853_mt6359_link_2_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_2_platforms),
		.playback_only = 1,
		.stream_name = "Playback_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_3",
		.cpus = mt6853_mt6359_link_3_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_3_cpus),
		.codecs = mt6853_mt6359_link_3_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_3_codecs),
		.platforms = mt6853_mt6359_link_3_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_3_platforms),
		.playback_only = 1,
		.stream_name = "Playback_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_4",
		.cpus = mt6853_mt6359_link_4_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_4_cpus),
		.codecs = mt6853_mt6359_link_4_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_4_codecs),
		.platforms = mt6853_mt6359_link_4_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_4_platforms),
		.playback_only = 1,
		.stream_name = "Playback_4",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_5",
		.cpus = mt6853_mt6359_link_5_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_5_cpus),
		.codecs = mt6853_mt6359_link_5_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_5_codecs),
		.platforms = mt6853_mt6359_link_5_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_5_platforms),
		.playback_only = 1,
		.stream_name = "Playback_5",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_6",
		.cpus = mt6853_mt6359_link_6_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_6_cpus),
		.codecs = mt6853_mt6359_link_6_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_6_codecs),
		.platforms = mt6853_mt6359_link_6_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_6_platforms),
		.playback_only = 1,
		.stream_name = "Playback_6",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_7",
		.cpus = mt6853_mt6359_link_7_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_7_cpus),
		.codecs = mt6853_mt6359_link_7_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_7_codecs),
		.platforms = mt6853_mt6359_link_7_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_7_platforms),
		.playback_only = 1,
		.stream_name = "Playback_7",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_8",
		.cpus = mt6853_mt6359_link_8_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_8_cpus),
		.codecs = mt6853_mt6359_link_8_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_8_codecs),
		.platforms = mt6853_mt6359_link_8_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_8_platforms),
		.playback_only = 1,
		.stream_name = "Playback_8",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_9",
		.cpus = mt6853_mt6359_link_9_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_9_cpus),
		.codecs = mt6853_mt6359_link_9_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_9_codecs),
		.platforms = mt6853_mt6359_link_9_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_9_platforms),
		.playback_only = 1,
		.stream_name = "Playback_9",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_1",
		.cpus = mt6853_mt6359_link_10_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_10_cpus),
		.codecs = mt6853_mt6359_link_10_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_10_codecs),
		.platforms = mt6853_mt6359_link_10_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_10_platforms),
		.capture_only = 1,
		.stream_name = "Capture_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_2",
		.cpus = mt6853_mt6359_link_11_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_11_cpus),
		.codecs = mt6853_mt6359_link_11_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_11_codecs),
		.platforms = mt6853_mt6359_link_11_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_11_platforms),
		.capture_only = 1,
		.stream_name = "Capture_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_3",
		.cpus = mt6853_mt6359_link_12_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_12_cpus),
		.codecs = mt6853_mt6359_link_12_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_12_codecs),
		.platforms = mt6853_mt6359_link_12_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_12_platforms),
		.capture_only = 1,
		.stream_name = "Capture_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_4",
		.cpus = mt6853_mt6359_link_13_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_13_cpus),
		.codecs = mt6853_mt6359_link_13_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_13_codecs),
		.platforms = mt6853_mt6359_link_13_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_13_platforms),
		.capture_only = 1,
		.stream_name = "Capture_4",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_5",
		.cpus = mt6853_mt6359_link_14_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_14_cpus),
		.codecs = mt6853_mt6359_link_14_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_14_codecs),
		.platforms = mt6853_mt6359_link_14_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_14_platforms),
		.capture_only = 1,
		.stream_name = "Capture_5",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_6",
		.cpus = mt6853_mt6359_link_15_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_15_cpus),
		.codecs = mt6853_mt6359_link_15_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_15_codecs),
		.platforms = mt6853_mt6359_link_15_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_15_platforms),
		.capture_only = 1,
		.stream_name = "Capture_6",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_7",
		.cpus = mt6853_mt6359_link_16_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_16_cpus),
		.codecs = mt6853_mt6359_link_16_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_16_codecs),
		.platforms = mt6853_mt6359_link_16_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_16_platforms),
		.capture_only = 1,
		.stream_name = "Capture_7",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_8",
		.cpus = mt6853_mt6359_link_17_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_17_cpus),
		.codecs = mt6853_mt6359_link_17_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_17_codecs),
		.platforms = mt6853_mt6359_link_17_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_17_platforms),
		.capture_only = 1,
		.stream_name = "Capture_8",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_Mono_1",
		.cpus = mt6853_mt6359_link_18_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_18_cpus),
		.codecs = mt6853_mt6359_link_18_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_18_codecs),
		.platforms = mt6853_mt6359_link_18_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_18_platforms),
		.capture_only = 1,
		.stream_name = "Capture_Mono_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_Mono_2",
		.cpus = mt6853_mt6359_link_19_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_19_cpus),
		.codecs = mt6853_mt6359_link_19_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_19_codecs),
		.platforms = mt6853_mt6359_link_19_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_19_platforms),
		.capture_only = 1,
		.stream_name = "Capture_Mono_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Capture_Mono_3",
		.cpus = mt6853_mt6359_link_20_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_20_cpus),
		.codecs = mt6853_mt6359_link_20_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_20_codecs),
		.platforms = mt6853_mt6359_link_20_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_20_platforms),
		.capture_only = 1,
		.stream_name = "Capture_Mono_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Playback_HDMI",
		.cpus = mt6853_mt6359_link_21_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_21_cpus),
		.codecs = mt6853_mt6359_link_21_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_21_codecs),
		.platforms = mt6853_mt6359_link_21_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_21_platforms),
		.playback_only = 1,
		.stream_name = "Playback_HDMI",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
	},
	{
		.name = "Hostless_LPBK",
		.cpus = mt6853_mt6359_link_22_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_22_cpus),
		.codecs = mt6853_mt6359_link_22_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_22_codecs),
		.platforms = mt6853_mt6359_link_22_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_22_platforms),
		.stream_name = "Hostless_LPBK",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_FM",
		.cpus = mt6853_mt6359_link_23_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_23_cpus),
		.codecs = mt6853_mt6359_link_23_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_23_codecs),
		.platforms = mt6853_mt6359_link_23_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_23_platforms),
		.stream_name = "Hostless_FM",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_Speech",
		.cpus = mt6853_mt6359_link_24_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_24_cpus),
		.codecs = mt6853_mt6359_link_24_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_24_codecs),
		.platforms = mt6853_mt6359_link_24_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_24_platforms),
		.stream_name = "Hostless_Speech",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_Sph_Echo_Ref",
		.cpus = mt6853_mt6359_link_25_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_25_cpus),
		.codecs = mt6853_mt6359_link_25_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_25_codecs),
		.platforms = mt6853_mt6359_link_25_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_25_platforms),
		.stream_name = "Hostless_Sph_Echo_Ref",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_Spk_Init",
		.cpus = mt6853_mt6359_link_26_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_26_cpus),
		.codecs = mt6853_mt6359_link_26_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_26_codecs),
		.platforms = mt6853_mt6359_link_26_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_26_platforms),
		.stream_name = "Hostless_Spk_Init",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_ADDA_DL_I2S_OUT",
		.cpus = mt6853_mt6359_link_27_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_27_cpus),
		.codecs = mt6853_mt6359_link_27_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_27_codecs),
		.platforms = mt6853_mt6359_link_27_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_27_platforms),
		.playback_only = 1,
		.stream_name = "Hostless_ADDA_DL_I2S_OUT",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_SRC_1",
		.cpus = mt6853_mt6359_link_28_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_28_cpus),
		.codecs = mt6853_mt6359_link_28_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_28_codecs),
		.platforms = mt6853_mt6359_link_28_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_28_platforms),
		.stream_name = "Hostless_SRC_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_SRC_Bargein",
		.cpus = mt6853_mt6359_link_29_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_29_cpus),
		.codecs = mt6853_mt6359_link_29_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_29_codecs),
		.platforms = mt6853_mt6359_link_29_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_29_platforms),
		.stream_name = "Hostless_SRC_Bargein",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	/* Back End DAI links */
	{
		.name = "Primary Codec",
		.cpus = mt6853_mt6359_link_30_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_30_cpus),
		.codecs = mt6853_mt6359_link_30_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_30_codecs),
		.platforms = mt6853_mt6359_link_30_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_30_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
		.init = mt6853_mt6359_init,
	},
	{
		.name = "Primary Codec CH34",
		.cpus = mt6853_mt6359_link_31_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_31_cpus),
		.codecs = mt6853_mt6359_link_31_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_31_codecs),
		.platforms = mt6853_mt6359_link_31_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_31_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "AP_DMIC",
		.cpus = mt6853_mt6359_link_32_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_32_cpus),
		.codecs = mt6853_mt6359_link_32_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_32_codecs),
		.platforms = mt6853_mt6359_link_32_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_32_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "AP_DMIC_CH34",
		.cpus = mt6853_mt6359_link_33_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_33_cpus),
		.codecs = mt6853_mt6359_link_33_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_33_codecs),
		.platforms = mt6853_mt6359_link_33_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_33_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "I2S3",
		.cpus = mt6853_mt6359_link_34_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_34_cpus),
		.codecs = mt6853_mt6359_link_34_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_34_codecs),
		.platforms = mt6853_mt6359_link_34_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_34_platforms),
		.playback_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6853_i2s_hw_params_fixup,
	},
	{
		.name = "I2S0",
		.cpus = mt6853_mt6359_link_35_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_35_cpus),
		.codecs = mt6853_mt6359_link_35_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_35_codecs),
		.platforms = mt6853_mt6359_link_35_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_35_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6853_i2s_hw_params_fixup,
	},
	{
		.name = "I2S1",
		.cpus = mt6853_mt6359_link_36_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_36_cpus),
		.codecs = mt6853_mt6359_link_36_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_36_codecs),
		.platforms = mt6853_mt6359_link_36_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_36_platforms),
		.playback_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6853_i2s_hw_params_fixup,
	},
	{
		.name = "I2S2",
		.cpus = mt6853_mt6359_link_37_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_37_cpus),
		.codecs = mt6853_mt6359_link_37_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_37_codecs),
		.platforms = mt6853_mt6359_link_37_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_37_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6853_i2s_hw_params_fixup,
	},
	{
		.name = "I2S5",
		.cpus = mt6853_mt6359_link_38_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_38_cpus),
		.codecs = mt6853_mt6359_link_38_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_38_codecs),
		.platforms = mt6853_mt6359_link_38_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_38_platforms),
		.playback_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt6853_i2s_hw_params_fixup,
	},
	{
		.name = "HW Gain 1",
		.cpus = mt6853_mt6359_link_39_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_39_cpus),
		.codecs = mt6853_mt6359_link_39_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_39_codecs),
		.platforms = mt6853_mt6359_link_39_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_39_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "HW Gain 2",
		.cpus = mt6853_mt6359_link_40_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_40_cpus),
		.codecs = mt6853_mt6359_link_40_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_40_codecs),
		.platforms = mt6853_mt6359_link_40_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_40_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "HW_SRC_1",
		.cpus = mt6853_mt6359_link_41_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_41_cpus),
		.codecs = mt6853_mt6359_link_41_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_41_codecs),
		.platforms = mt6853_mt6359_link_41_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_41_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "HW_SRC_2",
		.cpus = mt6853_mt6359_link_42_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_42_cpus),
		.codecs = mt6853_mt6359_link_42_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_42_codecs),
		.platforms = mt6853_mt6359_link_42_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_42_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "CONNSYS_I2S",
		.cpus = mt6853_mt6359_link_43_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_43_cpus),
		.codecs = mt6853_mt6359_link_43_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_43_codecs),
		.platforms = mt6853_mt6359_link_43_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_43_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "PCM 1",
		.cpus = mt6853_mt6359_link_44_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_44_cpus),
		.codecs = mt6853_mt6359_link_44_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_44_codecs),
		.platforms = mt6853_mt6359_link_44_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_44_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "PCM 2",
		.cpus = mt6853_mt6359_link_45_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_45_cpus),
		.codecs = mt6853_mt6359_link_45_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_45_codecs),
		.platforms = mt6853_mt6359_link_45_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_45_platforms),
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "TDM",
		.cpus = mt6853_mt6359_link_46_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_46_cpus),
		.codecs = mt6853_mt6359_link_46_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_46_codecs),
		.platforms = mt6853_mt6359_link_46_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_46_platforms),
		.playback_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	/* dummy BE for ul memif to record from dl memif */
	{
		.name = "Hostless_UL1",
		.cpus = mt6853_mt6359_link_47_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_47_cpus),
		.codecs = mt6853_mt6359_link_47_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_47_codecs),
		.platforms = mt6853_mt6359_link_47_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_47_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_UL2",
		.cpus = mt6853_mt6359_link_48_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_48_cpus),
		.codecs = mt6853_mt6359_link_48_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_48_codecs),
		.platforms = mt6853_mt6359_link_48_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_48_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_UL3",
		.cpus = mt6853_mt6359_link_49_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_49_cpus),
		.codecs = mt6853_mt6359_link_49_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_49_codecs),
		.platforms = mt6853_mt6359_link_49_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_49_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_UL6",
		.cpus = mt6853_mt6359_link_50_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_50_cpus),
		.codecs = mt6853_mt6359_link_50_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_50_codecs),
		.platforms = mt6853_mt6359_link_50_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_50_platforms),
		.capture_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_DSP_DL",
		.cpus = mt6853_mt6359_link_51_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_51_cpus),
		.codecs = mt6853_mt6359_link_51_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_51_codecs),
		.platforms = mt6853_mt6359_link_51_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_51_platforms),
		.playback_only = 1,
		.no_pcm = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_HW_Gain_AAudio",
		.cpus = mt6853_mt6359_link_52_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_52_cpus),
		.codecs = mt6853_mt6359_link_52_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_52_codecs),
		.platforms = mt6853_mt6359_link_52_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_52_platforms),
		.capture_only = 1,
		.stream_name = "Hostless_HW_Gain_AAudio",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	{
		.name = "Hostless_SRC_AAudio",
		.cpus = mt6853_mt6359_link_53_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_53_cpus),
		.codecs = mt6853_mt6359_link_53_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_53_codecs),
		.platforms = mt6853_mt6359_link_53_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_53_platforms),
		.stream_name = "Hostless_SRC_AAudio",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.ignore_suspend = 1,
	},
	/* BTCVSD */
#ifdef CONFIG_SND_SOC_MTK_BTCVSD
	{
		.name = "BTCVSD",
		.cpus = mt6853_mt6359_link_54_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_54_cpus),
		.codecs = mt6853_mt6359_link_54_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_54_codecs),
		.platforms = mt6853_mt6359_link_54_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_54_platforms),
		.stream_name = "BTCVSD",
	},
#endif
#if defined(CONFIG_SND_SOC_MTK_AUDIO_DSP)
	{
		.name = "Offload_Playback",
		.cpus = mt6853_mt6359_link_55_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_55_cpus),
		.codecs = mt6853_mt6359_link_55_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_55_codecs),
		.platforms = mt6853_mt6359_link_55_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_55_platforms),
		.stream_name = "Offload_Playback",
	},
	{
		.name = "DSP_Playback_Voip",
		.cpus = mt6853_mt6359_link_56_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_56_cpus),
		.codecs = mt6853_mt6359_link_56_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_56_codecs),
		.platforms = mt6853_mt6359_link_56_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_56_platforms),
		.stream_name = "DSP_Playback_Voip",
	},
	{
		.name = "DSP_Playback_Primary",
		.cpus = mt6853_mt6359_link_57_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_57_cpus),
		.codecs = mt6853_mt6359_link_57_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_57_codecs),
		.platforms = mt6853_mt6359_link_57_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_57_platforms),
		.stream_name = "DSP_Playback_Primary",
	},
	{
		.name = "DSP_Playback_DeepBuf",
		.cpus = mt6853_mt6359_link_58_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_58_cpus),
		.codecs = mt6853_mt6359_link_58_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_58_codecs),
		.platforms = mt6853_mt6359_link_58_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_58_platforms),
		.stream_name = "DSP_Playback_DeepBuf",
	},
	{
		.name = "DSP_Playback_Playback",
		.cpus = mt6853_mt6359_link_59_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_59_cpus),
		.codecs = mt6853_mt6359_link_59_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_59_codecs),
		.platforms = mt6853_mt6359_link_59_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_59_platforms),
		.stream_name = "DSP_Playback_Playback",
	},
	{
		.name = "DSP_Capture_Ul1",
		.cpus = mt6853_mt6359_link_60_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_60_cpus),
		.codecs = mt6853_mt6359_link_60_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_60_codecs),
		.platforms = mt6853_mt6359_link_60_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_60_platforms),
		.stream_name = "DSP_Capture_Ul1",
	},
	{
		.name = "DSP_Call_Final",
		.cpus = mt6853_mt6359_link_61_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_61_cpus),
		.codecs = mt6853_mt6359_link_61_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_61_codecs),
		.platforms = mt6853_mt6359_link_61_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_61_platforms),
		.stream_name = "DSP_Call_Final",
	},
	{
		.name = "DSP_Playback_Fast",
		.cpus = mt6853_mt6359_link_62_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_62_cpus),
		.codecs = mt6853_mt6359_link_62_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_62_codecs),
		.platforms = mt6853_mt6359_link_62_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_62_platforms),
		.stream_name = "DSP_Playback_Fast",
	},
	{
		.name = "DSP_Playback_Ktv",
		.cpus = mt6853_mt6359_link_63_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_63_cpus),
		.codecs = mt6853_mt6359_link_63_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_63_codecs),
		.platforms = mt6853_mt6359_link_63_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_63_platforms),
		.stream_name = "DSP_Playback_Ktv",
	},
	{
		.name = "DSP_Capture_Raw",
		.cpus = mt6853_mt6359_link_64_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_64_cpus),
		.codecs = mt6853_mt6359_link_64_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_64_codecs),
		.platforms = mt6853_mt6359_link_64_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_64_platforms),
		.stream_name = "DSP_Capture_Raw",
	},
	{
		.name = "DSP_Playback_Fm_Adsp",
		.cpus = mt6853_mt6359_link_65_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_65_cpus),
		.codecs = mt6853_mt6359_link_65_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_65_codecs),
		.platforms = mt6853_mt6359_link_65_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_65_platforms),
		.stream_name = "DSP_Playback_Fm_Adsp",
	},
	{
		.name = "DSP_Playback_A2DP",
		.cpus = mt6853_mt6359_link_66_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_66_cpus),
		.codecs = mt6853_mt6359_link_66_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_66_codecs),
		.platforms = mt6853_mt6359_link_66_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_66_platforms),
		.stream_name = "DSP_Playback_A2DP",
	},
#endif
#ifdef CONFIG_MTK_VOW_SUPPORT
	{
		.name = "VOW_Capture",
		.cpus = mt6853_mt6359_link_67_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_67_cpus),
		.codecs = mt6853_mt6359_link_67_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_67_codecs),
		.platforms = mt6853_mt6359_link_67_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_67_platforms),
		.stream_name = "VOW_Capture",
		.ignore_suspend = 1,
		.ops = &mt6853_mt6359_vow_ops,
	},
#endif  // #ifdef CONFIG_MTK_VOW_SUPPORT
#if defined(CONFIG_SND_SOC_MTK_SCP_SMARTPA)
	{
		.name = "SCP_SPK_Playback",
		.cpus = mt6853_mt6359_link_68_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_68_cpus),
		.codecs = mt6853_mt6359_link_68_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_68_codecs),
		.platforms = mt6853_mt6359_link_68_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_68_platforms),
		.stream_name = "SCP_SPK_Playback",
	},
#endif
#if defined(CONFIG_MTK_ULTRASND_PROXIMITY)
	{
		.name = "SCP_ULTRA_Playback",
		.cpus = mt6853_mt6359_link_69_cpus,
		.num_cpus = ARRAY_SIZE(mt6853_mt6359_link_69_cpus),
		.codecs = mt6853_mt6359_link_69_codecs,
		.num_codecs = ARRAY_SIZE(mt6853_mt6359_link_69_codecs),
		.platforms = mt6853_mt6359_link_69_platforms,
		.num_platforms = ARRAY_SIZE(mt6853_mt6359_link_69_platforms),
		.stream_name = "SCP_ULTRA_Playback",
	},
#endif
};

static struct snd_soc_card mt6853_mt6359_soc_card = {
	.name = "mt6853-mt6359",
	.owner = THIS_MODULE,
	.dai_link = mt6853_mt6359_dai_links,
	.num_links = ARRAY_SIZE(mt6853_mt6359_dai_links),

	.controls = mt6853_mt6359_controls,
	.num_controls = ARRAY_SIZE(mt6853_mt6359_controls),
	.dapm_widgets = mt6853_mt6359_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6853_mt6359_widgets),
	.dapm_routes = mt6853_mt6359_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6853_mt6359_routes),
};

static int mt6853_mt6359_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6853_mt6359_soc_card;
	struct device_node *platform_node, *codec_node, *spk_node, *dsp_node;
	struct device_node *accdet_node;
	struct snd_soc_dai_link *spk_out_dai_link, *spk_iv_dai_link;
	int ret, i;
	int spk_out_dai_link_idx, spk_iv_dai_link_idx;
	const char *name;

	/* resolve the accdet component first; a defer must not leave partial state */
	mt6853_accdet = NULL;
	accdet_node = of_parse_phandle(pdev->dev.of_node, "mediatek,accdet", 0);
	if (accdet_node && of_device_is_available(accdet_node)) {
		struct platform_device *accdet_pdev =
			of_find_device_by_node(accdet_node);

		if (accdet_pdev) {
			mt6853_accdet = snd_soc_lookup_component(
						&accdet_pdev->dev, NULL);
			put_device(&accdet_pdev->dev);
		}
		if (!mt6853_accdet && ++accdet_defer_cnt <= 8) {
			of_node_put(accdet_node);
			return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
					     "waiting for accdet\n");
		}
		if (!mt6853_accdet)
			dev_warn(&pdev->dev, "accdet absent, no jack detection\n");
	}
	of_node_put(accdet_node);

	ret = mtk_spk_update_info(card, pdev,
				  &spk_out_dai_link_idx, &spk_iv_dai_link_idx,
				  &mt6853_mt6359_i2s_ops);
	if (ret) {
		dev_err(&pdev->dev, "%s(), mtk_spk_update_info error\n",
			__func__);
		return -EINVAL;
	}

	spk_out_dai_link = &mt6853_mt6359_dai_links[spk_out_dai_link_idx];
	spk_iv_dai_link = &mt6853_mt6359_dai_links[spk_iv_dai_link_idx];
	if (!spk_out_dai_link->num_codecs &&
	    !spk_iv_dai_link->num_codecs) {
		spk_node = of_get_child_by_name(pdev->dev.of_node,
					"mediatek,speaker-codec");
		if (!spk_node) {
			dev_err(&pdev->dev,
				"spk_codec of_get_child_by_name fail\n");
			return -EINVAL;
		}
		ret = snd_soc_of_get_dai_link_codecs(
				&pdev->dev, spk_node, spk_out_dai_link);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"i2s out get_dai_link_codecs fail\n");
			return -EINVAL;
		}
		ret = snd_soc_of_get_dai_link_codecs(
				&pdev->dev, spk_node, spk_iv_dai_link);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"i2s in get_dai_link_codecs fail\n");
			return -EINVAL;
		}
	}

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_err(&pdev->dev, "Property 'platform' missing or invalid\n");
		return -EINVAL;
	}

	dsp_node = of_parse_phandle(pdev->dev.of_node,
				    "mediatek,snd_audio_dsp", 0);
	if (!dsp_node)
		dev_info(&pdev->dev, "Property 'snd_audio_dsp' missing or invalid\n");

	for (i = 0; i < card->num_links; i++) {
		if (mt6853_mt6359_dai_links[i].platforms[0].name)
			continue;
		/* no platform assign and with dsp playback node. */
		name = mt6853_mt6359_dai_links[i].name;
		if (!strncmp(name, "DSP", strlen("DSP")) &&
		    mt6853_mt6359_dai_links[i].platforms[0].name == NULL) {
			mt6853_mt6359_dai_links[i].platforms[0].of_node = dsp_node;
			continue;
		}
		mt6853_mt6359_dai_links[i].platforms[0].of_node = platform_node;
	}

	codec_node = of_parse_phandle(pdev->dev.of_node,
				      "mediatek,audio-codec", 0);
	if (!codec_node) {
		dev_err(&pdev->dev,
			"Property 'audio-codec' missing or invalid\n");
		return -EINVAL;
	}
	for (i = 0; i < card->num_links; i++) {
		if ((mt6853_mt6359_dai_links[i].num_codecs &&
		     mt6853_mt6359_dai_links[i].codecs[0].name) ||
		    i == spk_out_dai_link_idx ||
		    i == spk_iv_dai_link_idx)
			continue;
		mt6853_mt6359_dai_links[i].codecs[0].of_node = codec_node;
	}

	card->dev = &pdev->dev;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "%s snd_soc_register_card fail %d\n",
			__func__, ret);
	return ret;
}

#ifdef CONFIG_OF
static const struct of_device_id mt6853_mt6359_dt_match[] = {
	{.compatible = "mediatek,mt6853-mt6359-sound",},
	{}
};
#endif

static const struct dev_pm_ops mt6853_mt6359_pm_ops = {
	.poweroff = snd_soc_poweroff,
	.restore = snd_soc_resume,
};

static struct platform_driver mt6853_mt6359_driver = {
	.driver = {
		.name = "mt6853-mt6359",
#ifdef CONFIG_OF
		.of_match_table = mt6853_mt6359_dt_match,
#endif
		.pm = &mt6853_mt6359_pm_ops,
	},
	.probe = mt6853_mt6359_dev_probe,
};

module_platform_driver(mt6853_mt6359_driver);

/* Module information */
MODULE_DESCRIPTION("MT6853 MT6359 ALSA SoC machine driver");
MODULE_AUTHOR("Shane Chien <shane.chien@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt6853 mt6359 soc card");
