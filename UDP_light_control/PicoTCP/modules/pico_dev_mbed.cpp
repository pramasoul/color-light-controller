/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.
Do not redistribute without a written permission by the Copyright
holders.

Authors: Toon Peters, Maxime Vincent
*********************************************************************/
#include "mbed.h"
extern "C" {
#include "pico_device.h"
#include "pico_dev_mbed.h"
#include "pico_stack.h"
#include "ethernet_api.h"
}

struct pico_device_mbed {
  struct pico_device dev;
  int bytes_left_in_frame;
};

#define ETH_MTU 1514
uint8_t buf[ETH_MTU];

Serial pc(p9, p10, "Serial port"); // tx, rx

extern "C" {

static int pico_mbed_send(struct pico_device *dev, void *buf, int len)
{
  int ret, sent;
  struct pico_device_mbed *mb = (struct pico_device_mbed *) dev;

  if (len > ETH_MTU)
    return -1;

  /* Write buf content to dev and return amount written */
  ret = ethernet_write((const char *)buf, len);
  sent = ethernet_send();

  pc.printf("ETH> sent %d bytes\r\n",ret);
  if (len != ret || sent != ret)
    return -1;
  else
    return ret;
}

static int pico_mbed_poll(struct pico_device *dev, int loop_score)
{
  int len;
  struct pico_device_mbed *mb = (struct pico_device_mbed *) dev;
  
  while(loop_score > 0)
  {
    /* check for new frame(s) */
    len = (int) ethernet_receive();
    
    /* return if no frame has arrived */
    if (!len)
      return loop_score;
  
    /* read and process frame */
    len = ethernet_read((char*)buf, ETH_MTU);
    pc.printf("ETH> recv %d bytes: %x:%x\r\n", len, buf[0],buf[1]);
    pico_stack_recv(dev, buf, len);
    loop_score--;
  }
  return loop_score;
}

/* Public interface: create/destroy. */
void pico_mbed_destroy(struct pico_device *dev)
{
  ethernet_free();
  pico_device_destroy(dev);
}

struct pico_device *pico_mbed_create(char *name)
{
  std::uint8_t mac[PICO_SIZE_ETH];
  struct pico_device_mbed *mb = (struct pico_device_mbed*) pico_zalloc(sizeof(struct pico_device_mbed));

  if (!mb)
    return NULL;

  ethernet_address((char *)mac);
  pc.printf("ETH> Set MAC address to: %x:%x:%x:%x:%x:%x\r\n", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  if(0 != pico_device_init((struct pico_device *)mb, name, mac)) {
    pc.printf ("ETH> Loop init failed.\n");
    //pico_loop_destroy(mb);
    return NULL;
  }

  mb->dev.send = pico_mbed_send;
  mb->dev.poll = pico_mbed_poll;
  mb->dev.destroy = pico_mbed_destroy;
  mb->bytes_left_in_frame = 0;

  if(0 != ethernet_init()) {
    pc.printf("ETH> Failed to initialize hardware.\r\n");
    pico_device_destroy((struct pico_device *)mb);
    return NULL;
  }

  // future work: make the mac address configurable

  pc.printf("ETH> Device %s created.\r\n", mb->dev.name);

  return (struct pico_device *)mb;
}

void pico_mbed_get_address(char *mac)
{
  ethernet_address(mac);
}

}