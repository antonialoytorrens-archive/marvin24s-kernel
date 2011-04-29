/*
 * seaboard.c - Seaboard machine ASoC driver
 *
 * Author: Stephen Warren <swarren@nvidia.com>
 * Copyright (C) 2010-2011 - NVIDIA, Inc.
 *
 * Based on code copyright/by:
 *
 * (c) 2009, 2010 Nvidia Graphics Pvt. Ltd.
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/gpio.h>

#include <mach/seaboard_audio.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/wm8903.h"

#include "tegra_das.h"
#include "tegra_i2s.h"
#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#define DRV_NAME "tegra-snd-seaboard"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)

struct tegra_seaboard {
	struct tegra_asoc_utils_data util_data;
	struct seaboard_audio_platform_data *pdata;
	int gpio_requested;
	struct regulator *vdd_dmic;
	bool vdd_dmic_enabled;
};

static int seaboard_asoc_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	int srate, mclk, mclk_change;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&seaboard->util_data, srate, mclk,
					&mclk_change);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(codec_dai,
					SND_SOC_DAIFMT_I2S |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_I2S |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	if (mclk_change) {
		err = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
					     SND_SOC_CLOCK_IN);
		if (err < 0) {
			dev_err(card->dev, "codec_dai clock not set\n");
			return err;
		}
	}

	return 0;
}

static struct snd_soc_ops seaboard_asoc_ops = {
	.hw_params = seaboard_asoc_hw_params,
};

static int seaboard_spdif_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	int srate, mclk, mclk_change;
	int err;

	/*
	 * FIXME: Refactor mclk into PCM-specific function; SPDIF doesn't
	 * need it
	 */
	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&seaboard->util_data, srate, mclk,
					&mclk_change);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	return 0;
}

static struct snd_soc_ops seaboard_spdif_ops = {
	.hw_params = seaboard_spdif_hw_params,
};

static struct snd_soc_jack seaboard_hp_jack;

static struct snd_soc_jack_pin seaboard_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static struct snd_soc_jack_gpio seaboard_hp_jack_gpios[] = {
	{
		.name = "headphone detect",
		.report = SND_JACK_HEADPHONE,
		.debounce_time = 150,
		.invert = 1,
	}
};

static struct snd_soc_jack seaboard_mic_jack;

static struct snd_soc_jack_pin seaboard_mic_jack_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int seaboard_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	struct seaboard_audio_platform_data *pdata = seaboard->pdata;

	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int seaboard_event_hp(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *k, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	struct seaboard_audio_platform_data *pdata = seaboard->pdata;

	if (!(seaboard->gpio_requested & GPIO_HP_MUTE))
		return 0;

	gpio_set_value_cansleep(pdata->gpio_hp_mute,
				!SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int seaboard_event_dmic(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *k, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	bool new_enabled;
	int ret;

	if (IS_ERR(seaboard->vdd_dmic))
		return 0;

	new_enabled = !!SND_SOC_DAPM_EVENT_ON(event);
	if (seaboard->vdd_dmic_enabled == new_enabled)
		return 0;

	if (new_enabled)
		ret = regulator_enable(seaboard->vdd_dmic);
	else
		ret = regulator_disable(seaboard->vdd_dmic);

	if (!ret)
		seaboard->vdd_dmic_enabled = new_enabled;

	return ret;
}

static const struct snd_soc_dapm_widget seaboard_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", seaboard_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", seaboard_event_hp),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("Digital Mic", seaboard_event_dmic),
};

static const struct snd_soc_dapm_route seaboard_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1R", NULL, "Mic Bias"},
	{"DMICDAT", NULL, "Digital Mic"},
};

static const struct snd_soc_dapm_route kaen_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "ROP"},
	{"Int Spk", NULL, "RON"},
	{"Int Spk", NULL, "LOP"},
	{"Int Spk", NULL, "LON"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN2R", NULL, "Mic Bias"},
	{"DMICDAT", NULL, "Digital Mic"},
};

static const struct snd_soc_dapm_route aebl_audio_map[] = {
	{"Headphone Jack", NULL, "HPOUTR"},
	{"Headphone Jack", NULL, "HPOUTL"},
	{"Int Spk", NULL, "LINEOUTR"},
	{"Int Spk", NULL, "LINEOUTL"},
	{"Mic Bias", NULL, "Mic Jack"},
	{"IN1R", NULL, "Mic Bias"},
	{"DMICDAT", NULL, "Digital Mic"},
};

static const struct snd_kcontrol_new seaboard_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
};

static int seaboard_asoc_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	struct seaboard_audio_platform_data *pdata = seaboard->pdata;
	int ret;

	ret = gpio_request(pdata->gpio_spkr_en, "spkr_en");
	if (ret) {
		dev_err(card->dev, "cannot get spkr_en gpio\n");
		return ret;
	}
	seaboard->gpio_requested |= GPIO_SPKR_EN;

	gpio_direction_output(pdata->gpio_spkr_en, 0);

	if (pdata->gpio_hp_mute != -1) {
		ret = gpio_request(pdata->gpio_hp_mute, "hp_mute");
		if (ret) {
			dev_err(card->dev, "cannot get hp_mute gpio\n");
			return ret;
		}
		seaboard->gpio_requested |= GPIO_HP_MUTE;

		gpio_direction_output(pdata->gpio_hp_mute, 0);
	}

	ret = snd_soc_add_controls(codec, seaboard_controls,
				   ARRAY_SIZE(seaboard_controls));
	if (ret < 0)
		return ret;

	snd_soc_dapm_new_controls(dapm, seaboard_dapm_widgets,
					ARRAY_SIZE(seaboard_dapm_widgets));

	if (machine_is_seaboard()) {
		snd_soc_dapm_add_routes(dapm, seaboard_audio_map,
					ARRAY_SIZE(seaboard_audio_map));
	} else if (machine_is_kaen()) {
		snd_soc_dapm_add_routes(dapm, kaen_audio_map,
					ARRAY_SIZE(kaen_audio_map));
	} else {
		snd_soc_dapm_add_routes(dapm, aebl_audio_map,
					ARRAY_SIZE(aebl_audio_map));
	}

	seaboard_hp_jack_gpios[0].gpio = pdata->gpio_hp_det;
	snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
			 &seaboard_hp_jack);
	snd_soc_jack_add_pins(&seaboard_hp_jack,
			      ARRAY_SIZE(seaboard_hp_jack_pins),
			      seaboard_hp_jack_pins);
	snd_soc_jack_add_gpios(&seaboard_hp_jack,
			       ARRAY_SIZE(seaboard_hp_jack_gpios),
			       seaboard_hp_jack_gpios);

	snd_soc_jack_new(codec, "Mic Jack", SND_JACK_MICROPHONE,
			 &seaboard_mic_jack);
	snd_soc_jack_add_pins(&seaboard_mic_jack,
			      ARRAY_SIZE(seaboard_mic_jack_pins),
			      seaboard_mic_jack_pins);
	wm8903_mic_detect(codec, &seaboard_mic_jack, SND_JACK_MICROPHONE, 0);

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias");

	snd_soc_dapm_nc_pin(dapm, "IN1L");
	if (machine_is_kaen())
		snd_soc_dapm_nc_pin(dapm, "IN1R");
	snd_soc_dapm_nc_pin(dapm, "IN2L");
	if (!machine_is_kaen())
		snd_soc_dapm_nc_pin(dapm, "IN2R");
	snd_soc_dapm_nc_pin(dapm, "IN2L");
	snd_soc_dapm_nc_pin(dapm, "IN3R");
	snd_soc_dapm_nc_pin(dapm, "IN3L");

	if (machine_is_aebl()) {
		snd_soc_dapm_nc_pin(dapm, "LON");
		snd_soc_dapm_nc_pin(dapm, "RON");
		snd_soc_dapm_nc_pin(dapm, "ROP");
		snd_soc_dapm_nc_pin(dapm, "LOP");
	} else {
		snd_soc_dapm_nc_pin(dapm, "LINEOUTR");
		snd_soc_dapm_nc_pin(dapm, "LINEOUTL");
	}

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link seaboard_links[] = {
	{
		.name = "WM8903",
		.stream_name = "WM8903 PCM",
		.codec_name = "wm8903.0-001a",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra-i2s.0",
		.codec_dai_name = "wm8903-hifi",
		.init = seaboard_asoc_init,
		.ops = &seaboard_asoc_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "spdif",
		.codec_name = "spdif-dit",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &seaboard_spdif_ops,
	},
};

static struct snd_soc_card snd_soc_seaboard = {
	.name = "tegra-seaboard",
	.dai_link = seaboard_links,
	.num_links = ARRAY_SIZE(seaboard_links),
};

static __devinit int tegra_snd_seaboard_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_seaboard;
	struct tegra_seaboard *seaboard;
	struct seaboard_audio_platform_data *pdata;
	int ret;

	if (!machine_is_seaboard() && !machine_is_kaen() &&
	    !machine_is_aebl()) {
		dev_err(&pdev->dev, "Not running on a supported board!\n");
		return -ENODEV;
	}

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -EINVAL;
	}

	seaboard = kzalloc(sizeof(struct tegra_seaboard), GFP_KERNEL);
	if (!seaboard) {
		dev_err(&pdev->dev, "Can't allocate tegra_seaboard\n");
		return -ENOMEM;
	}

	seaboard->pdata = pdata;

	ret = tegra_asoc_utils_init(&seaboard->util_data, &pdev->dev);
	if (ret)
		goto err_free_seaboard;

	seaboard->vdd_dmic = regulator_get(&pdev->dev, "vdd_dmic");
	if (IS_ERR(seaboard->vdd_dmic)) {
		dev_info(&pdev->dev, "regulator_get() returned error %ld\n",
			 PTR_ERR(seaboard->vdd_dmic));
		ret = PTR_ERR(seaboard->vdd_dmic);
		goto err_fini_utils;
	}

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, seaboard);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_clear_drvdata;
	}

	return 0;

err_clear_drvdata:
	snd_soc_card_set_drvdata(card, NULL);
	platform_set_drvdata(pdev, NULL);
	card->dev = NULL;
	regulator_put(seaboard->vdd_dmic);
err_fini_utils:
	tegra_asoc_utils_fini(&seaboard->util_data);
err_free_seaboard:
	kfree(seaboard);
	return ret;
}

static int __devexit tegra_snd_seaboard_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_seaboard *seaboard = snd_soc_card_get_drvdata(card);
	struct seaboard_audio_platform_data *pdata = seaboard->pdata;

	snd_soc_unregister_card(card);

	snd_soc_card_set_drvdata(card, NULL);
	platform_set_drvdata(pdev, NULL);
	card->dev = NULL;

	regulator_put(seaboard->vdd_dmic);

	tegra_asoc_utils_fini(&seaboard->util_data);

	if (seaboard->gpio_requested & GPIO_HP_MUTE)
		gpio_free(pdata->gpio_hp_mute);
	if (seaboard->gpio_requested & GPIO_SPKR_EN)
		gpio_free(pdata->gpio_spkr_en);

	kfree(seaboard);

	return 0;
}

static struct platform_driver tegra_snd_seaboard_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_snd_seaboard_probe,
	.remove = __devexit_p(tegra_snd_seaboard_remove),
};

static int __init snd_tegra_seaboard_init(void)
{
	return platform_driver_register(&tegra_snd_seaboard_driver);
}
module_init(snd_tegra_seaboard_init);

static void __exit snd_tegra_seaboard_exit(void)
{
	platform_driver_unregister(&tegra_snd_seaboard_driver);
}
module_exit(snd_tegra_seaboard_exit);

MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_DESCRIPTION("Seaboard machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
