/* 
 *
 * PicoTCP Socket interface for mbed.
 * Copyright (C) 2013 TASS Belgium NV
 * 
 * Released under GPL v2
 *
 * Other licensing models might apply at the sole discretion of the copyright holders.
 *
 *
 * This software is based on the mbed.org EthernetInterface implementation:
 * Copyright (C) 2012 mbed.org, MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "EthernetInterface.h"
#include "Queue.h"
#include "wrapper.h"
#include "proxy_endpoint.h"
#include "pico_dev_mbed_emac.h"
#include "mbed.h"
#include "PicoCondition.h"
extern "C"{
#include "pico_stack.h"
#include "pico_config.h"
#include "cmsis_os.h"
#include "pico_dhcp_client.h"
#include "pico_dns_client.h"

//#define eth_dbg mbed_dbg
#define eth_dbg(...)

void (*linkCb)(uint32_t link) = NULL;
}

//DigitalOutput led(LED3);
/* TCP/IP and Network Interface Initialisation */
static struct pico_device *lpc_eth;

static uint32_t dhcp_xid = 0;
static PicoCondition dhcp_mx;
static int dhcp_retval = -1;

static char mac_addr[19];
static char ip_addr[17] = "\0";
static char gw_addr[17] = "\0";
static bool use_dhcp = false;
static bool is_initialized = false;

static void dhcp_cb(void *cli, int code)
{
    void *id = NULL;
    struct pico_ip4 address, gateway, zero = {};
    
    if (PICO_DHCP_ERROR == code)
        goto fail;
        
    id = pico_dhcp_get_identifier(dhcp_xid);
    if (!id)
        goto fail;
    address = pico_dhcp_get_address(id);
    gateway = pico_dhcp_get_gateway(id);
    //if (address) ? // still needed
    pico_ipv4_to_string(ip_addr, address.addr);
    printf("IP assigned : %s\n",ip_addr);
    
    if (gateway.addr != 0) {
        pico_ipv4_to_string(gw_addr, gateway.addr);
        printf("Default gateway assigned : %s\n",gw_addr);
        pico_ipv4_route_add(zero, zero, gateway, 1, NULL);
    }

    dhcp_retval = 0;
    dhcp_mx.unlock();
    
    return;
    
fail:
    eth_dbg("DHCP request failed!\n");
    dhcp_retval = -1;
    dhcp_mx.unlock();
}

static void init_eth(void) 
{
   if (!is_initialized) {
        pico_stack_init();
        picotcp_init();
        lpc_eth = pico_emac_create("mbed0");
        is_initialized = true;
        pico_dns_client_init();
    }
    if (lpc_eth) {
        snprintf(mac_addr, 19, "%02X:%02X:%02X:%02X:%02X:%02X", lpc_eth->eth->mac.addr[0], lpc_eth->eth->mac.addr[1], 
            lpc_eth->eth->mac.addr[2], lpc_eth->eth->mac.addr[3], lpc_eth->eth->mac.addr[4], lpc_eth->eth->mac.addr[5]);
    }
    
    if(lpc_eth)
        eth_dbg("Ethernet initialized...\n");
    else
        eth_dbg("Failed to start Ethernet...\n");
       
}

int EthernetInterface::init() 
{
    init_eth();    
    /* use dhcp to retrieve address and gateway. */
    use_dhcp = true;
    return 0; 
}

int EthernetInterface::init(const char* ip, const char* mask, const char* gateway) {
    pico_ip4 pico_addr, pico_netmask, pico_gw = {0}, zero = {0};
    
    init_eth();
    
    use_dhcp = false;
    strcpy(ip_addr, ip);
    
    pico_string_to_ipv4(ip, &pico_addr.addr);
    pico_string_to_ipv4(mask, &pico_netmask.addr);
    if (gateway) {
        pico_string_to_ipv4(ip, &pico_gw.addr);
    }
    pico_ipv4_link_add(lpc_eth, pico_addr, pico_netmask);
    
    if (pico_gw.addr)
        pico_ipv4_route_add(zero, zero, pico_gw, 1, NULL);
    
    return 0;
}

int EthernetInterface::connect(unsigned int timeout_ms) {
    //dhcp_mx.lock(); do we still need this ?
    if (use_dhcp) {
        if (pico_dhcp_initiate_negotiation(lpc_eth, &dhcp_cb, &dhcp_xid) < 0)
            return -1;
      
        dhcp_mx.lock(timeout_ms); // wait for a sign
        
        return dhcp_retval;
        
    } else {
        return 0;
    }
}

int EthernetInterface::disconnect() {
    if (use_dhcp) {
    } 
    pico_device_destroy(lpc_eth);
    lpc_eth = NULL;
    return 0;
}

char* EthernetInterface::getMACAddress() {
    return mac_addr;
}

char* EthernetInterface::getIPAddress() {
    return ip_addr;
}

int EthernetInterface::registerLinkStatus(void (*cb)(uint32_t linkStatus))
{
    ::linkCb = cb;
    return 0;
}

int EthernetInterface::setDnsServer(const char * name)
{
    struct pico_ip4 addr;
    pico_string_to_ipv4(name,&addr.addr);
    return pico_dns_client_nameserver(&addr,PICO_DNS_NS_ADD);
}
