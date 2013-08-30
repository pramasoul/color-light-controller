/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

*********************************************************************/
#ifndef _INCLUDE_PICO_CONFIG
#define _INCLUDE_PICO_CONFIG
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pico_constants.h"


#define MBED
//#define PICO_SUPPORT_CRC
#define PICO_SUPPORT_DEVLOOP
#define PICO_SUPPORT_DHCPC
#define PICO_SUPPORT_DHCPD
#define PICO_SUPPORT_DNS_CLIENT
#define PICO_SUPPORT_HTTP_CLIENT
#define PICO_SUPPORT_HTTP
#define PICO_SUPPORT_HTTP_SERVER
#define PICO_SUPPORT_ICMP4
#define PICO_SUPPORT_PING
#define PICO_SUPPORT_IGMP2
//#define PICO_SUPPORT_IPFILTER
//#define PICO_SUPPORT_IPFRAG
#define PICO_SUPPORT_IPV4
#define PICO_SUPPORT_MCAST
#define PICO_SUPPORT_NAT
#define PICO_SUPPORT_TCP
#define PICO_SUPPORT_UDP
# include "arch/pico_mbed.h"
#endif
