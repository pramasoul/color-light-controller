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
 
#ifndef ETHERNETINTERFACE_H_
#define ETHERNETINTERFACE_H_

#if !defined(TARGET_LPC1768)
#error The Ethernet Interface library is supported only on the mbed NXP LPC1768
#endif

#include "rtos.h"

#ifdef __cplusplus
extern "C" {
#endif
    extern void (*linkCb)(uint32_t link);
#ifdef __cplusplus    
}
#endif
 /** Interface using Ethernet to connect to an IP-based network
 *
 */
class EthernetInterface {
public:
  /** Initialize the interface with DHCP.
  * Initialize the interface and configure it to use DHCP (no connection at this point).
  * \return 0 on success, a negative number on failure
  */
  static int init(); //With DHCP

  /** Initialize the interface with a static IP address.
  * Initialize the interface and configure it with the following static configuration (no connection at this point).
  * \param ip the IP address to use
  * \param mask the IP address mask
  * \param gateway the gateway to use
  * \return 0 on success, a negative number on failure
  */
  static int init(const char* ip, const char* mask, const char* gateway);

  /** Connect
  * Bring the interface up, start DHCP if needed.
  * \param   timeout_ms  timeout in ms (default: (10)s).
  * \return 0 on success, a negative number on failure
  */
  static int connect(unsigned int timeout_ms=15000);
  
  /** Disconnect
  * Bring the interface down
  * \return 0 on success, a negative number on failure
  */
  static int disconnect();
  
  /** Get the MAC address of your Ethernet interface
   * \return a pointer to a string containing the MAC address
   */
  static char* getMACAddress();
  
  /** Get the IP address of your Ethernet interface
   * \return a pointer to a string containing the IP address
   */
  static char* getIPAddress();
  
  
  /** Register a callback to tell the status of the link.
   * \return 0 if callback was registered.
   */
  static int registerLinkStatus(void (*cb)(uint32_t linkStatus));
  
  
  /** Register a callback to tell the status of the link.
   * \return 0 if callback was registered.
   */
  static int setDnsServer(const char *);
  
};

#include "TCPSocketConnection.h"
#include "TCPSocketServer.h"

#include "Endpoint.h"
#include "UDPSocket.h"

#endif /* ETHERNETINTERFACE_H_ */
