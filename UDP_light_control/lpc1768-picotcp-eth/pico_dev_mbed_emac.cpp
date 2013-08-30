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

#include "mbed.h"
extern "C" {
#include "pico_dev_mbed_emac.h"
#include "pico_dev_mbed_emac_private.h"
#include "pico_config.h"
#include "pico_device.h"
#include "pico_stack.h"
#include "LPC17xx.h"
#include <string.h>
}
#include "PicoCondition.h"
#include "proxy_endpoint.h"

//static PicoCondition rx_condition;

/*******************************
 * Local structs and typedefs  *
 *******************************/
__packed struct RX_DESC_TypeDef {                        /* RX Descriptor struct              */
   unsigned int Packet;
   unsigned int Ctrl;
};
typedef struct RX_DESC_TypeDef RX_DESC_TypeDef;

__packed struct RX_STAT_TypeDef {                        /* RX Status struct                  */
   unsigned int Info;
   unsigned int HashCRC;
};
typedef struct RX_STAT_TypeDef RX_STAT_TypeDef;

__packed struct TX_DESC_TypeDef {                        /* TX Descriptor struct              */
   unsigned int Packet;
   unsigned int Ctrl;
};
typedef struct TX_DESC_TypeDef TX_DESC_TypeDef;

__packed struct TX_STAT_TypeDef {                        /* TX Status struct                  */
   unsigned int Info;
};
typedef struct TX_STAT_TypeDef TX_STAT_TypeDef;

// To be allocated in the ETH AHB RAM
__packed typedef struct emac_dma_data {
  RX_STAT_TypeDef   p_rx_stat[NUM_RX_FRAG];   /**< Pointer to RX statuses */
  RX_DESC_TypeDef   p_rx_desc[NUM_RX_FRAG];   /**< Pointer to RX descriptor list */
  TX_STAT_TypeDef   p_tx_stat[NUM_TX_FRAG];   /**< Pointer to TX statuses */
  TX_DESC_TypeDef   p_tx_desc[NUM_TX_FRAG];   /**< Pointer to TX descriptor list */
  uint8_t           rx_buf[NUM_RX_FRAG][ETH_MAX_MTU];      /**< RX pbuf pointer list, zero-copy mode */
  uint8_t           tx_buf[NUM_TX_FRAG][ETH_MAX_MTU];      /**< TX pbuf pointer list, zero-copy mode */
} EMAC_DMA_DATA_T;

struct pico_device_mbed_emac {
  struct pico_device dev;
  uint16_t mtu;
  uint8_t mac[PICO_SIZE_ETH];
  EMAC_DMA_DATA_T * dma_data;
};

/********************
 * Global variables *
 ********************/

/*******************
 * Local variables *
 *******************/
static uint16_t *rptr;
static uint16_t *tptr;
static EMAC_DMA_DATA_T dma_data_ahbsram __attribute__((section("AHBSRAM1")));

DigitalOut led_link(LED2);  /* Link */
DigitalOut led_rx(LED3);    /* Rx */
DigitalOut led_tx(LED4);    /* Tx */

/**************************************
 * Private helper function prototypes *
 **************************************/
static void _emac_init(struct pico_device_mbed_emac * mbdev);
static void _emac_destroy(struct pico_device *dev);
static int _emac_write_PHY (int PhyReg, int Value);
static int _emac_read_PHY (unsigned char PhyReg);
static void _emac_rx_descr_init (struct pico_device_mbed_emac * mbdev);
static void _emac_tx_descr_init (struct pico_device_mbed_emac * mbdev);
static int _emac_send_frame(struct pico_device * dev, void * buf, int len);
static void _emac_phy_status(void const * dev);
static inline unsigned int _emac_clockselect();
static int _pico_emac_poll(struct pico_device *dev, int loop_score);

struct pico_device *interrupt_mbdev;

/*****************
 * CMSIS defines *
 *****************/
// Timer: For periodic PHY update
osTimerDef(_emac_phy_status, _emac_phy_status);

/******************************
 * Public interface functions *
 ******************************/
uint32_t intStatus;


void ENET_IRQHandler(void)
{
    // Get interrupt flag for enabled interrupts
    intStatus = (LPC_EMAC->IntStatus & LPC_EMAC->IntEnable);
    
    if(intStatus & INT_TX_UNDERRUN)
    {
      // this case should be treated
      //printf("TX_UNDERRUN\r\n");
    }

    if(intStatus & INT_RX_OVERRUN)
    {
      // this case should be treated
      //printf("INT_RX_OVERRUN\r\n");
    }
 
    if(intStatus & INT_RX_DONE)
    {
        picotcp_async_interrupt(interrupt_mbdev);
        //rx_condition.unlock();
 
    }
    
    // Clears _ALL_ EMAC interrupt flags
    LPC_EMAC->IntClear = intStatus;
}

static int _emac_poll(struct pico_device_mbed_emac * mbdev);
/*
void rxThreadCore(void const *arg)
    {
    struct pico_device_mbed_emac *dev = (struct pico_device_mbed_emac *)arg;
    printf("rx Thread started.\n");

    while(true) {
        rx_condition.lock();
        _emac_poll(dev);

    }
}
*/



struct pico_device *pico_emac_create(char *name)
{
  struct pico_device_mbed_emac *mbdev = (struct pico_device_mbed_emac*) pico_zalloc(sizeof(struct pico_device_mbed_emac));

  if (!mbdev)
    return NULL;

  // Set pointer to ETH AHB RAM
  mbdev->dma_data = &dma_data_ahbsram;

  // Read MAC address from HW
  mbed_mac_address((char *)mbdev->mac);
  
  //printf("ETH> Set MAC address to: %x:%x:%x:%x:%x:%x\r\n", mbdev->mac[0], mbdev->mac[1], mbdev->mac[2], mbdev->mac[3], mbdev->mac[4], mbdev->mac[5]);

  mbdev->mtu = ETH_MAX_MTU;

  if(0 != pico_device_init((struct pico_device *)mbdev, name, mbdev->mac)) {
    //printf ("ETH> Loop init failed.\n");
    _emac_destroy(&mbdev->dev);
    return NULL;
  }
  
  //pico_queue_protect(((struct pico_device *)mbdev)->q_in);

  // Set function pointers
  mbdev->dev.send = _emac_send_frame;
  //mbdev->dev.poll = _pico_emac_poll; /* IRQ MODE */
  mbdev->dev.dsr = _pico_emac_poll;
  mbdev->dev.destroy = _emac_destroy;

  // Init EMAC and PHY
  _emac_init(mbdev);

  // Create periodic PHY status update thread
  osTimerId phy_timer = osTimerCreate(osTimer(_emac_phy_status), osTimerPeriodic, (void *)mbdev);
  osTimerStart(phy_timer, 100);
  
  //Thread *rxThread = new Thread(rxThreadCore, (void*)mbdev);
  
  //rxThread->set_priority(osPriorityLow);

  //printf("ETH> Device %s created.\r\n", mbdev->dev.name);

  return (struct pico_device *)mbdev;
}

/****************************
 * Private helper functions *
 ****************************/
 
// Public interface: create/destroy.
void _emac_destroy(struct pico_device *dev)
{
  pico_device_destroy(dev);
}

//extern unsigned int SystemFrequency;
static inline unsigned int _emac_clockselect() {  
  if(SystemCoreClock < 10000000) {
    return 1;
  } else if(SystemCoreClock <  15000000) {
    return 2;
  } else if(SystemCoreClock <  20000000) {
    return 3;
  } else if(SystemCoreClock <  25000000) {
    return 4;
  } else if(SystemCoreClock <  35000000) {
    return 5;
  } else if(SystemCoreClock <  50000000) {
    return 6;
  } else if(SystemCoreClock <  70000000) {
    return 7;
  } else if(SystemCoreClock <  80000000) {
    return 8;
  } else if(SystemCoreClock <  90000000) {
    return 9;
  } else if(SystemCoreClock < 100000000) {
    return 10;
  } else if(SystemCoreClock < 120000000) {
    return 11;
  } else if(SystemCoreClock < 130000000) {
    return 12;
  } else if(SystemCoreClock < 140000000) {
    return 13;
  } else if(SystemCoreClock < 150000000) {
    return 15;
  } else if(SystemCoreClock < 160000000) {
    return 16;
  } else {
    return 0;
  }
}

// configure port-pins for use with LAN-controller,
// reset it and send the configuration-sequence
void _emac_init(struct pico_device_mbed_emac * mbdev)
{
  unsigned int clock = _emac_clockselect();
  // the DP83848C PHY clock can be up to 25 Mhz
  // that means sysclock = 96 Mhz divided by 4 = 24 Mhz
  // So we *could* set the clock divider to 4 (meaning "clock" = 0)
  //clock = 0;
  
  // Initializes the EMAC ethernet controller
  unsigned int regv,tout,id1,id2;

  // Power Up the EMAC controller.
  LPC_SC->PCONP |= 0x40000000;

  // on rev. 'A' and later, P1.6 should NOT be set.
  LPC_PINCON->PINSEL2 = 0x50150105;
  LPC_PINCON->PINSEL3 = (LPC_PINCON->PINSEL3 & ~0x0000000F) | 0x00000005;

  // Reset all EMAC internal modules.
  LPC_EMAC->MAC1 = MAC1_RES_TX | MAC1_RES_MCS_TX | MAC1_RES_RX |
                   MAC1_RES_MCS_RX | MAC1_SIM_RES | MAC1_SOFT_RES;
  LPC_EMAC->Command = CR_REG_RES | CR_TX_RES | CR_RX_RES | CR_PASS_RUNT_FRM;

  // A short delay after reset.
  for (tout = 100; tout; tout--) __NOP();            // A short delay

  // Initialize MAC control registers.
  LPC_EMAC->MAC1 = MAC1_PASS_ALL;
  LPC_EMAC->MAC2 = MAC2_CRC_EN | MAC2_PAD_EN;
  //  MAC2 = MAC2_CRC_EN | MAC2_PAD_EN | MAC2_VLAN_PAD_EN;

  LPC_EMAC->MAXF = ETH_MAX_MTU; 
  LPC_EMAC->CLRT = CLRT_DEF;
  LPC_EMAC->IPGR = IPGR_DEF;

  // Enable Reduced MII interface.
  LPC_EMAC->Command = CR_RMII | CR_PASS_RUNT_FRM;
  
  LPC_EMAC->MCFG = (clock << 0x2) & MCFG_CLK_SEL;    // Set clock
  LPC_EMAC->MCFG |= MCFG_RES_MII;                    // and reset

  for(tout = 100; tout; tout--) __NOP();             // A short delay

  LPC_EMAC->MCFG = (clock << 0x2) & MCFG_CLK_SEL;
  LPC_EMAC->MCMD = 0;

  LPC_EMAC->SUPP = SUPP_RES_RMII;                    // Reset Reduced MII Logic.

  for (tout = 100; tout; tout--) __NOP();            // A short delay

  LPC_EMAC->SUPP = 0;

  // Put the DP83848C in reset mode
  _emac_write_PHY (PHY_REG_BMCR, 0x8000);

  // Wait for hardware reset to end.
  for (tout = 0; tout < 0x100000; tout++) {
    regv = _emac_read_PHY (PHY_REG_BMCR);
    if (!(regv & 0x8000)) {
      // Reset complete
      break;
    }
  }

  // Check if this is a DP83848C PHY.
  id1 = _emac_read_PHY (PHY_REG_IDR1);
  id2 = _emac_read_PHY (PHY_REG_IDR2);
  if (((id1 << 16) | (id2 & 0xFFF0)) == DP83848C_ID) {
    // Configure the PHY device
    //printf("PHY> DP83848C_ID PHY found!\r\n");
    // Use autonegotiation about the link speed.
    _emac_write_PHY (PHY_REG_BMCR, PHY_AUTO_NEG);
    // Wait to complete Auto_Negotiation.
    for (tout = 0; tout < 0x100000; tout++) {
      regv = _emac_read_PHY (PHY_REG_BMSR);
      if (regv & 0x0020) {
        // Autonegotiation Complete.
        break;
      }
    }
  }

  /* Check the link status. */
  for (tout = 0; tout < 0x10000; tout++) {
    regv = _emac_read_PHY (PHY_REG_STS);
    if (regv & 0x0001) {
      // Link is on
      //printf("PHY> Link active!\r\n");
      break;
    }
  }

  // Configure Full/Half Duplex mode.
  if (regv & 0x0004) {
    // Full duplex is enabled.
    LPC_EMAC->MAC2    |= MAC2_FULL_DUP;
    LPC_EMAC->Command |= CR_FULL_DUP;
    LPC_EMAC->IPGT     = IPGT_FULL_DUP;
  }
  else {
    // Half duplex mode.
    LPC_EMAC->IPGT = IPGT_HALF_DUP;
  }

  // Configure 100MBit/10MBit mode
  if (regv & 0x0002) {
    // 10MBit mode
    LPC_EMAC->SUPP = 0;
  }
  else {
    // 100MBit mode
    LPC_EMAC->SUPP = SUPP_SPEED;
  }

  // Set the Ethernet MAC Address registers
  LPC_EMAC->SA0 = (mbdev->mac[0] << 8) | mbdev->mac[1];
  LPC_EMAC->SA1 = (mbdev->mac[2] << 8) | mbdev->mac[3];
  LPC_EMAC->SA2 = (mbdev->mac[4] << 8) | mbdev->mac[5];

  // Initialize Tx and Rx DMA Descriptors
  _emac_rx_descr_init(mbdev);
  _emac_tx_descr_init(mbdev);
  
  // Receive Unicast, Broadcast and Perfect Match Packets
  LPC_EMAC->RxFilterCtrl = RFC_UCAST_EN | RFC_MCAST_EN | RFC_BCAST_EN | RFC_PERFECT_EN;

  // Enable receive and transmit mode of MAC Ethernet core
  LPC_EMAC->Command  |= (CR_RX_EN | CR_TX_EN);
  LPC_EMAC->MAC1     |= MAC1_REC_EN;

  // Enable EMAC interrupts.
  LPC_EMAC->IntEnable = INT_RX_DONE | INT_RX_OVERRUN | INT_TX_DONE | INT_TX_FIN | INT_TX_ERR | INT_TX_UNDERRUN;

  // Reset all interrupts
  LPC_EMAC->IntClear  = 0xFFFF;

  NVIC_SetPriority(ENET_IRQn, EMAC_INTERRUPT_PRIORITY);

  // Enable the interrupt.
  NVIC_EnableIRQ(ENET_IRQn);
  // Associate the interrupt to this device
  interrupt_mbdev = (struct pico_device *)mbdev;
  
}


static int _emac_write_PHY (int PhyReg, int Value)
{
  unsigned int timeOut;

  LPC_EMAC->MADR = DP83848C_DEF_ADR | PhyReg;
  LPC_EMAC->MWTD = Value;

  // Wait until operation completed
  for (timeOut = 0; timeOut < MII_WR_TOUT; timeOut++) {
    if ((LPC_EMAC->MIND & MIND_BUSY) == 0) {
      return 0;
    }
  }
  return -1;
}

static int _emac_read_PHY (unsigned char PhyReg)
{
  unsigned int timeOut;

  LPC_EMAC->MADR = DP83848C_DEF_ADR | PhyReg;
  LPC_EMAC->MCMD = MCMD_READ;

  for(timeOut = 0; timeOut < MII_RD_TOUT; timeOut++) {   // Wait until operation completed
    if((LPC_EMAC->MIND & MIND_BUSY) == 0) {
      LPC_EMAC->MCMD = 0;
      return LPC_EMAC->MRDD;                             // Return a 16-bit value.
    }
  }
  
  return -1;
}


void rx_descr_init (void)
{
  unsigned int i;

  for (i = 0; i < NUM_RX_FRAG; i++) {
    RX_DESC_PACKET(i)  = RX_BUF(i);
    RX_DESC_CTRL(i)    = RCTRL_INT | (ETH_MAX_MTU-1);
    RX_STAT_INFO(i)    = 0;
    RX_STAT_HASHCRC(i) = 0;
  }

  // Set EMAC Receive Descriptor Registers.
  LPC_EMAC->RxDescriptor    =   RX_DESC_BASE;
  LPC_EMAC->RxStatus        =   RX_STAT_BASE;
  LPC_EMAC->RxDescriptorNumber= NUM_RX_FRAG-1;

  // Rx Descriptors Point to 0
  LPC_EMAC->RxConsumeIndex  = 0;
}


void tx_descr_init (void) {
  unsigned int i;

  for (i = 0; i < NUM_TX_FRAG; i++) {
    TX_DESC_PACKET(i) = TX_BUF(i);
    TX_DESC_CTRL(i)   = 0;
    TX_STAT_INFO(i)   = 0;
  }

  // Set EMAC Transmit Descriptor Registers.
  LPC_EMAC->TxDescriptor    = TX_DESC_BASE;
  LPC_EMAC->TxStatus        = TX_STAT_BASE;
  LPC_EMAC->TxDescriptorNumber = NUM_TX_FRAG-1;

  // Tx Descriptors Point to 0
  LPC_EMAC->TxProduceIndex  = 0;
}


static void _emac_rx_descr_init (struct pico_device_mbed_emac * mbdev)
{
  unsigned int i;

  for (i = 0; i < NUM_RX_FRAG; i++) {
    // Fill in pointers to ETH DMA AHB RAM
    mbdev->dma_data->p_rx_desc[i].Packet =  (uint32_t)&(mbdev->dma_data->rx_buf[i][0]);
    mbdev->dma_data->p_rx_desc[i].Ctrl = RCTRL_INT | (ETH_MAX_MTU-1);
    mbdev->dma_data->p_rx_stat[i].Info = 0;
    mbdev->dma_data->p_rx_stat[i].HashCRC = 0;
  }

  // Set EMAC Receive Descriptor Registers.
  LPC_EMAC->RxDescriptor        = (uint32_t)&(mbdev->dma_data->p_rx_desc[0]);
  LPC_EMAC->RxStatus            = (uint32_t)&(mbdev->dma_data->p_rx_stat[0]);
  LPC_EMAC->RxDescriptorNumber  = NUM_RX_FRAG-1;

  // Rx Descriptors Point to 0
  LPC_EMAC->RxConsumeIndex  = 0;
}

static void _emac_tx_descr_init (struct pico_device_mbed_emac * mbdev) {
  unsigned int i;

  for (i = 0; i < NUM_TX_FRAG; i++) {
    mbdev->dma_data->p_tx_desc[i].Packet =  (uint32_t)&(mbdev->dma_data->tx_buf[i][0]);
    mbdev->dma_data->p_tx_desc[i].Ctrl = 0;
    mbdev->dma_data->p_tx_stat[i].Info = 0;
  }
  
  // Set EMAC Transmit Descriptor Registers.
  LPC_EMAC->TxDescriptor    = (uint32_t)&(mbdev->dma_data->p_tx_desc[0]);
  LPC_EMAC->TxStatus        = (uint32_t)&(mbdev->dma_data->p_tx_stat[0]);
  LPC_EMAC->TxDescriptorNumber = NUM_TX_FRAG-1;

  // Tx Descriptors Point to index 0
  LPC_EMAC->TxProduceIndex  = 0;
}


// async send, returning amount of data sent, and 0 if the buffers are busy
static int _emac_send_frame(struct pico_device *dev, void * buf, int len)
{
  struct pico_device_mbed_emac *mbdev = (struct pico_device_mbed_emac *) dev;
  uint16_t size = len;
  uint32_t bufferIndex = LPC_EMAC->TxProduceIndex;
  uint16_t pSize = (size+1u)>>1u;
  uint16_t * data = (uint16_t *)buf;
  int data_sent = 0;

  // When TxProduceIndex == TxConsumeIndex, the transmit buffer is empty.
  // When TxProduceIndex == TxConsumeIndex -1 (taking wraparound into account), the transmit buffer is full!
  // We should check if there is still a tx_desc FREE!! Consume < Produce (wrapping!!)  
  int cons = (int)LPC_EMAC->TxConsumeIndex;
  int prod = (int)LPC_EMAC->TxProduceIndex;
  int txfree = 0;
  
  if (prod == cons)
    txfree = NUM_TX_FRAG;
  else if (prod > cons)
    txfree = NUM_TX_FRAG - prod + cons - 1;
  else if (prod < cons)
    txfree = cons - prod - 1; 
  
  if (txfree == 0)
  {
    int retries = 2;
    // block until an index gets free
    while((LPC_EMAC->TxConsumeIndex -1)== LPC_EMAC->TxProduceIndex && (retries--))
        Thread::wait(1);
    
    if((LPC_EMAC->TxConsumeIndex -1)== LPC_EMAC->TxProduceIndex)
    {
        printf("Failed to send frame !\n");
        return 0;
    }
    else
        bufferIndex = LPC_EMAC->TxProduceIndex;
    //printf("p%i c%i stat:%d\r\n",prod,cons,LPC_EMAC->TxStatus);
    //printf("Sending returned 0 : %x :%x\n",LPC_EMAC->IntStatus,LPC_EMAC->IntEnable);
    
  }
  
  // set tx descriptors for send
  tptr = (uint16_t *)(mbdev->dma_data->p_tx_desc[bufferIndex].Packet);
    
  // copy data to AHB RAM (16-bit copies)
  while(pSize)
  {
    *tptr++ = *data++;
    pSize--;
  }
  
  data_sent = size;

  // send data, no interrupt needed I see, for now
  mbdev->dma_data->p_tx_desc[bufferIndex].Ctrl &= ~0x7FF;
  mbdev->dma_data->p_tx_desc[bufferIndex].Ctrl = (((uint32_t)size -1) & 0x7FF) | TCTRL_INT | TCTRL_LAST;
  
  // advance the TxProduceIndex
  bufferIndex = (bufferIndex+1)%NUM_TX_FRAG;
  LPC_EMAC->TxProduceIndex = bufferIndex;
        
  // Toggle TX LED
  led_tx= !led_tx;

  return data_sent;
}


/* polls for new data
   returns amount of bytes received -- 0 when no new data arrived */
static int _emac_poll(struct pico_device_mbed_emac * mbdev)
{
    uint32_t index = LPC_EMAC->RxConsumeIndex;
    int retval = 0;

    // Consume all packets available
    while(index != LPC_EMAC->RxProduceIndex)
    {
      uint32_t info = mbdev->dma_data->p_rx_stat[index].Info;
      uint32_t RxLen = (info & RINFO_SIZE) - 3u;
      rptr = (uint16_t *)(mbdev->dma_data->p_rx_desc[index].Packet);

      if(!( (RxLen >= ETH_MAX_MTU) || (info & RINFO_ERR_MASK) ) ) {
        pico_stack_recv((struct pico_device *)mbdev,(uint8_t *)rptr,RxLen);
      } else {
        //printf("RxError?\r\n");
      }
      retval += RxLen;
      
      // Increment RxConsumeIndex
      index = (index+1)%NUM_RX_FRAG;
      LPC_EMAC->RxConsumeIndex = index;
                
    }
    
    return retval;
}

static int _pico_emac_poll(struct pico_device *dev, int loop_score)
{
  struct pico_device_mbed_emac *mb = (struct pico_device_mbed_emac *) dev;
  
  while(loop_score > 0)
  {
    // check for new frame(s) and send to pico stack
    // return loop_score if no frame has arrived
    if (!(_emac_poll(mb)))
        return loop_score;
    loop_score--;
  }
  return loop_score;
}


/* Read PHY status */
static uint8_t _emac_update_phy_status(uint32_t linksts)
{
  uint8_t changed = 0;
  static uint32_t oldlinksts = 0;
    
  if (oldlinksts != linksts)
    changed = 1;
        
  if (changed)
  {
    oldlinksts = linksts;
    
    // Update link active status
    if (linksts & DP8_VALID_LINK)
    {
      led_link = 1;
      //printf("PHY> Link ACTIVE!     --");
    }
    else
    {
      led_link = 0;
      //printf("PHY> Link inactive... --");
    }

    // Full or half duplex
    if (linksts & DP8_FULLDUPLEX)
      printf(" Full duplex mode ! --");
    else
      printf(" No full duplex...! --");

    // Configure 100MBit/10MBit mode.
    if (linksts & DP8_SPEED10MBPS)
      printf(" @  10 MBPS\r\n");
    else
      printf(" @ 100 MBPS\r\n");
  }

  return changed;
}

// Starts a read operation via the MII link (non-blocking)
static void _emac_mii_read_noblock(uint32_t PhyReg) 
{
  // Read value at PHY address and register
  LPC_EMAC->MADR = DP83848C_DEF_ADR | PhyReg;
  LPC_EMAC->MCMD = MCMD_READ;
}

/* Reads current MII link busy status */
static uint32_t _emac_mii_is_busy(void)
{
  return (uint32_t) (LPC_EMAC->MIND & MIND_BUSY);
}

// Starts a read operation via the MII link (non-blocking)
static uint32_t _emac_mii_read_data(void)
{
  uint32_t data = LPC_EMAC->MRDD;
  LPC_EMAC->MCMD = 0;

  return data;
}

// Phy status update state machine
static void _emac_phy_status(void const * dev)
{
  static uint8_t phyustate = 0;
  uint32_t changed = 0;

  switch (phyustate) {
    default:
    case 0:
      // Read BMSR to clear faults
      _emac_mii_read_noblock(PHY_REG_STS);
      phyustate = 1;
      break;

    case 1:
      // Wait for read status state
      if (!_emac_mii_is_busy()) {
        // Update PHY status
        changed = _emac_update_phy_status(_emac_mii_read_data());
        phyustate = 0;
      }
      break;
  }
}
