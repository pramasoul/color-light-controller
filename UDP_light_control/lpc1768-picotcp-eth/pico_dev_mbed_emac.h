/******************************************************************************
PicoTCP. Copyright (c) 2012-2013 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage. https://github.com/tass-belgium/picotcp

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License Version 2
as published by the Free Software Foundation;

Some of the code contained in this file is based on mbed.org 
mbed/libraries/mbed/vendor/NXP/capi/ethernet_api.c module, 
licensed under the Apache License, Version 2.0
and is Copyright (c) 2006-2013 ARM Limited

Authors: Maxime Vincent, Andrei Carp

******************************************************************************/

#ifndef __PICO_DEV_MBED_EMAC_H
#define __PICO_DEV_MBED_EMAC_H

#include "cmsis_os.h"
#include <stdint.h>

/********************
 * Public functions *
 ********************/
extern "C" {
struct pico_device *pico_emac_create(char *name);
extern void pico_emac_init(void);
extern void ENET_IRQHandler(void);
}

#endif
