/*
 * Copyright 2022 NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MQTT_FREERTOS_H
#define MQTT_FREERTOS_H

#include "lwip/netif.h"
#include "fsl_gpio.h"
#include "app.h"
/*
gpio_pin_config_t led_config = {
       kGPIO_DigitalOutput,
       0,
   };*/
//Flags for topics
typedef union Topic_flags
{
	uint8_t All_Flags;
	struct
	{
		uint8_t		laser_slider  		:	1;
		uint8_t		mode_selector 		:	1;
		uint8_t 	bumper				:	1;
		uint8_t 	release				:	1;
		uint8_t		reserved	 		:	4;
	}topics;
}Topic_Flags;

// global variable holding flags.

extern Topic_Flags topic_status;
/*!
 * @brief Create and run example thread
 *
 * @param netif  netif which example should use
 */
void mqtt_freertos_run_thread(struct netif *netif);

#endif /* MQTT_FREERTOS_H */
