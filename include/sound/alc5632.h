/*
* alc5632.h  --  Platform data for ALC5632 
*
* Copyright (C) 2011 The AC100 Kernel Team <ac100@lists.lauchpad.net>
*
* Authors:  Leon Romanovsky <leon@leon.nu>
*           Andrey Danin <danindrey@mail.ru>
*           Ilya Petrov <ilya.muromec@gmail.com>
*           Marc Dietrich <marvin24@gmx.de>
*
* Based on alc5623.h by Arnaud Patard
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*/

#ifndef _INCLUDE_SOUND_ALC5632_H
#define _INCLUDE_SOUND_ALC5632_H

struct alc5632_platform_data {
	/* configure :                              */
	/* Lineout/Speaker Amps Vmid ratio control  */
	/* enable/disable adc/dac high pass filters */
	unsigned int add_ctrl;
	/* configure :                              */
	/* output to enable when jack is low        */
	/* output to enable when jack is high       */
	/* jack detect (gpio/nc/jack detect [12]    */
	unsigned int jack_det_ctrl;
};

#endif
