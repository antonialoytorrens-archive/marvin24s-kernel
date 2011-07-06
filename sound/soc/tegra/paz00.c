/*
 * paz00.c - PAZ00 machine ASoC driver
 */

#include <asm/mach-types.h>

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>

#include <mach/paz00_audio.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/alc5632.h"

#include "tegra_das.h"
#include "tegra_i2s.h"
#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#define DRV_NAME "tegra-snd-paz00"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_INT_MIC_EN BIT(1)
#define GPIO_EXT_MIC_EN BIT(2)

struct tegra_paz00 {
	struct tegra_asoc_utils_data util_data;
	struct paz00_audio_platform_data *pdata;
	int gpio_requested;
};

static int paz00_asoc_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_paz00 *paz00 = snd_soc_card_get_drvdata(card);
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
		mclk = 512 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&paz00->util_data, srate, mclk,
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

static struct snd_soc_ops paz00_asoc_ops = {
	.hw_params = paz00_asoc_hw_params,
};

static struct snd_soc_jack paz00_hp_jack;

static struct snd_soc_jack_pin paz00_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static struct snd_soc_jack_gpio paz00_hp_jack_gpios[] = {
	{
		.name = "headphone detect",
		.report = SND_JACK_HEADPHONE,
		.debounce_time = 150,
		.invert = 1,
	}
};

static struct snd_soc_jack paz00_mic_jack;

static struct snd_soc_jack_pin paz00_mic_jack_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int paz00_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{

        printk("spk event: %d\n", event);
/* 
    ToDo: speaker enable via nvec
	struct snd_soc_codec *codec = w->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_paz00 *paz00 = snd_soc_card_get_drvdata(card);
	struct paz00_audio_platform_data *pdata = paz00->pdata;

	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				SND_SOC_DAPM_EVENT_ON(event));
*/
	return 0;
}

static const struct snd_soc_dapm_widget paz00_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Int Spk", paz00_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
};

static const struct snd_soc_dapm_route paz00_audio_map[] = {
	{"Headphone Jack", NULL, "HPR"},
	{"Headphone Jack", NULL, "HPL"},
	{"Int Spk", NULL, "HPL"},
	{"Int Spk", NULL, "SPKOUTN"},
	{"Mic Bias1", NULL, "Mic Jack"},
};

static const struct snd_kcontrol_new paz00_controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
};

static int paz00_asoc_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_paz00 *paz00 = snd_soc_card_get_drvdata(card);
	struct paz00_audio_platform_data *pdata = paz00->pdata;
	int ret;

/* Todo: speaker enable via nvec */
/*	ret = gpio_request(pdata->gpio_spkr_en, "spkr_en");
	if (ret) {
		dev_err(card->dev, "cannot get spkr_en gpio\n");
		return ret;
	}
	paz00->gpio_requested |= GPIO_SPKR_EN;

	gpio_direction_output(pdata->gpio_spkr_en, 0);

	ret = gpio_request(pdata->gpio_int_mic_en, "int_mic_en");
	if (ret) {
		dev_err(card->dev, "cannot get int_mic_en gpio\n");
		return ret;
	}
	paz00->gpio_requested |= GPIO_INT_MIC_EN;
*/
	/* Disable int mic; enable signal is active-high */
/*	gpio_direction_output(pdata->gpio_int_mic_en, 0);

	ret = gpio_request(pdata->gpio_ext_mic_en, "ext_mic_en");
	if (ret) {
		dev_err(card->dev, "cannot get ext_mic_en gpio\n");
		return ret;
	}
	paz00->gpio_requested |= GPIO_EXT_MIC_EN;
*/
	/* Enable ext mic; enable signal is active-low */
/*	gpio_direction_output(pdata->gpio_ext_mic_en, 0);
*/
	ret = snd_soc_add_controls(codec, paz00_controls,
				   ARRAY_SIZE(paz00_controls));
	if (ret < 0)
		return ret;

	snd_soc_dapm_new_controls(dapm, paz00_dapm_widgets,
					ARRAY_SIZE(paz00_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, paz00_audio_map,
				ARRAY_SIZE(paz00_audio_map));

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias1");

	snd_soc_dapm_nc_pin(dapm, "AUXOUT");
	snd_soc_dapm_nc_pin(dapm, "LINEINL");
	snd_soc_dapm_nc_pin(dapm, "LINEINR");
	snd_soc_dapm_nc_pin(dapm, "PHONEP");
	snd_soc_dapm_nc_pin(dapm, "PHONEN");
	snd_soc_dapm_nc_pin(dapm, "MIC2");

	snd_soc_dapm_sync(dapm);

	return 0;
}

static struct snd_soc_dai_link paz00_alc5632_dai = {
	.name = "ALC5632",
	.stream_name = "ALC5632 PCM",
	.codec_name = "alc5632.0-001e",
	.platform_name = "tegra-pcm-audio",
	.cpu_dai_name = "tegra-i2s.0",
	.codec_dai_name = "alc5632-hifi",
	.init = paz00_asoc_init,
	.ops = &paz00_asoc_ops,
};

static struct snd_soc_card snd_soc_paz00 = {
	.name = "tegra-paz00",
	.dai_link = &paz00_alc5632_dai,
	.num_links = 1,
};

static __devinit int tegra_snd_paz00_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_paz00;
	struct tegra_paz00 *paz00;
	struct paz00_audio_platform_data *pdata;
	int ret;

	if (!machine_is_paz00()) {
		dev_err(&pdev->dev, "Not running on Toshiba AC100!\n");
		return -ENODEV;
	}

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -EINVAL;
	}

	paz00 = kzalloc(sizeof(struct tegra_paz00), GFP_KERNEL);
	if (!paz00) {
		dev_err(&pdev->dev, "Can't allocate tegra_paz00\n");
		return -ENOMEM;
	}

	paz00->pdata = pdata;

	ret = tegra_asoc_utils_init(&paz00->util_data, &pdev->dev);
	if (ret)
		goto err_free_paz00;

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, paz00);

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
	tegra_asoc_utils_fini(&paz00->util_data);
err_free_paz00:
	kfree(paz00);
	return ret;
}

static int __devexit tegra_snd_paz00_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_paz00 *paz00 = snd_soc_card_get_drvdata(card);
	struct paz00_audio_platform_data *pdata = paz00->pdata;

	snd_soc_unregister_card(card);

	snd_soc_card_set_drvdata(card, NULL);
	platform_set_drvdata(pdev, NULL);
	card->dev = NULL;

	tegra_asoc_utils_fini(&paz00->util_data);

/*	if (paz00->gpio_requested & GPIO_EXT_MIC_EN)
		gpio_free(pdata->gpio_ext_mic_en);
	if (paz00->gpio_requested & GPIO_INT_MIC_EN)
		gpio_free(pdata->gpio_int_mic_en);
	if (paz00->gpio_requested & GPIO_SPKR_EN)
		gpio_free(pdata->gpio_spkr_en);
*/
	kfree(paz00);

	return 0;
}

static struct platform_driver tegra_snd_paz00_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_snd_paz00_probe,
	.remove = __devexit_p(tegra_snd_paz00_remove),
};

static int __init snd_tegra_paz00_init(void)
{
	return platform_driver_register(&tegra_snd_paz00_driver);
}
module_init(snd_tegra_paz00_init);

static void __exit snd_tegra_paz00_exit(void)
{
	platform_driver_unregister(&tegra_snd_paz00_driver);
}
module_exit(snd_tegra_paz00_exit);

MODULE_AUTHOR("Stephen Warren <swarren@nvidia.com>");
MODULE_DESCRIPTION("Harmony machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
