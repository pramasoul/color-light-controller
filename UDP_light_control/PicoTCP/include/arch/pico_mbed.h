/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

File: pico_mbed.h
Author: Toon Peters
*********************************************************************/

#ifndef PICO_SUPPORT_MBED
#define PICO_SUPPORT_MBED
#include <stdio.h>

//#include "mbed.h"
//#include "serial_api.h"

/*
Debug needs initialization:
* void serial_init       (serial_t *obj, PinName tx, PinName rx);
* void serial_baud       (serial_t *obj, int baudrate);
* void serial_format     (serial_t *obj, int data_bits, SerialParity parity, int stop_bits);
*/

#define dbg(...) 
#define pico_zalloc(x) calloc(x, 1)
#define pico_free(x) free(x)

#ifdef MEMORY_MEASURE // in case, comment out the two defines above me.
extern uint32_t max_mem;
extern uint32_t cur_mem;

static inline void * pico_zalloc(int x)
{
    uint32_t *ptr;
    if ((cur_mem + x )> (10 * 1024))
        return NULL;
        
    ptr = (uint32_t *)calloc(x + 4, 1);
    *ptr = (uint32_t)x;
    cur_mem += x;
    if (cur_mem > max_mem) {
        max_mem = cur_mem;
        printf("max mem: %lu\n", max_mem);
    }
    return (void*)(ptr + 1);
}

static inline void pico_free(void *x)
{
    uint32_t *ptr = (uint32_t*)(((uint8_t *)x) - 4);
    cur_mem -= *ptr;
    free(ptr);
}
#endif

#define PICO_SUPPORT_MUTEX
extern void *pico_mutex_init(void);
extern void pico_mutex_lock(void*);
extern void pico_mutex_unlock(void*);


extern uint32_t os_time;

static inline unsigned long PICO_TIME(void)
{
  return (unsigned long)os_time / 1000;
}

static inline unsigned long PICO_TIME_MS(void)
{
  return (unsigned long)os_time;
}

static inline void PICO_IDLE(void)
{
  // TODO needs implementation
}
/*
static inline void PICO_DEBUG(const char * formatter, ... )
{
  char buffer[256];
  char *ptr;
  va_list args;
  va_start(args, formatter);
  vsnprintf(buffer, 256, formatter, args);
  ptr = buffer;
  while(*ptr != '\0')
    serial_putc(serial_t *obj, (int) (*(ptr++)));
  va_end(args);
  //TODO implement serial_t
}*/

#endif
