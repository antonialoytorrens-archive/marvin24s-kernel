/*
 * Generic PWM backlight driver data - see drivers/video/backlight/pwm_bl.c
 */
#ifndef __LINUX_PWM_BACKLIGHT_H
#define __LINUX_PWM_BACKLIGHT_H

#include <linux/backlight.h>
#include <linux/power_seq.h>

struct platform_pwm_backlight_data {
	unsigned int max_brightness;
	unsigned int dft_brightness;
	unsigned int lth_brightness;
	unsigned int *levels;
	/*
	 * New interface using power sequences. Must include exactly
	 * two power sequences named 'power-on' and 'power-off'. If NULL,
	 * the legacy interface is used.
	 */
	struct platform_power_seq_set *power_seqs;

	/*
	 * Legacy interface - use power sequences instead!
	 *
	 * pwm_id and pwm_period_ns need only be specified
	 * if get_pwm(dev, NULL) would return NULL.
	 */
	int pwm_id;
	unsigned int pwm_period_ns;
	int (*init)(struct device *dev);
	int (*notify)(struct device *dev, int brightness);
	void (*notify_after)(struct device *dev, int brightness);
	void (*exit)(struct device *dev);
	int (*check_fb)(struct device *dev, struct fb_info *info);
};

#endif
