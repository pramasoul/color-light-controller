/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

File: pico_dev_mbed.h
Author: Toon Peters
*********************************************************************/

#ifndef PICO_DEV_MBED_H
#define    PICO_DEV_MBED_H

#include "pico_config.h"
#include "pico_device.h"

void pico_mbed_destroy(struct pico_device *vde);
struct pico_device *pico_mbed_create(char *name);

#endif    /* PICO_DEV_MBED_H */

