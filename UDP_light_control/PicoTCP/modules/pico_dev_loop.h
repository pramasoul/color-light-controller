/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

*********************************************************************/
#ifndef _INCLUDE_PICO_LOOP
#define _INCLUDE_PICO_LOOP
#include "pico_config.h"
#include "pico_device.h"

void pico_loop_destroy(struct pico_device *loop);
struct pico_device *pico_loop_create(void);

#endif

