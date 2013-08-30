/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

.

Authors: Daniele Lacamera
*********************************************************************/


#include "pico_config.h"
#include "pico_frame.h"
#include "pico_device.h"
#include "pico_protocol.h"
#include "pico_stack.h"
#include "pico_addressing.h"
#include "pico_dns_client.h"

#include "pico_eth.h"
#include "pico_arp.h"
#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_icmp4.h"
#include "pico_igmp.h"
#include "pico_udp.h"
#include "pico_tcp.h"
#include "pico_socket.h"
#include "heap.h"

#define IS_LIMITED_BCAST(f) ( ((struct pico_ipv4_hdr *) f->net_hdr)->dst.addr == PICO_IP4_BCAST )

#ifdef PICO_SUPPORT_MCAST
# define PICO_SIZE_MCAST 3
  const uint8_t PICO_ETHADDR_MCAST[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x00};
#endif

volatile unsigned long pico_tick;
volatile pico_err_t pico_err;

static uint32_t _rand_seed;

void pico_rand_feed(uint32_t feed)
{
  if (!feed)
    return;
  _rand_seed *= 1664525;
  _rand_seed += 1013904223;
  _rand_seed ^= ~(feed);
}

uint32_t pico_rand(void)
{
  pico_rand_feed(pico_tick);
  return _rand_seed;
}

/* NOTIFICATIONS: distributed notifications for stack internal errors.
 */

int pico_notify_socket_unreachable(struct pico_frame *f)
{
  if (0) {}
#ifdef PICO_SUPPORT_ICMP4 
  else if (IS_IPV4(f)) {
    pico_icmp4_port_unreachable(f);
  }
#endif
#ifdef PICO_SUPPORT_ICMP6 
  else if (IS_IPV6(f)) {
    pico_icmp6_port_unreachable(f);
  }
#endif

  return 0;
}

int pico_notify_proto_unreachable(struct pico_frame *f)
{
  if (0) {}
#ifdef PICO_SUPPORT_ICMP4 
  else if (IS_IPV4(f)) {
    pico_icmp4_proto_unreachable(f);
  }
#endif
#ifdef PICO_SUPPORT_ICMP6 
  else if (IS_IPV6(f)) {
    pico_icmp6_proto_unreachable(f);
  }
#endif
  return 0;
}

int pico_notify_dest_unreachable(struct pico_frame *f)
{
  if (0) {}
#ifdef PICO_SUPPORT_ICMP4 
  else if (IS_IPV4(f)) {
    pico_icmp4_dest_unreachable(f);
  }
#endif
#ifdef PICO_SUPPORT_ICMP6 
  else if (IS_IPV6(f)) {
    pico_icmp6_dest_unreachable(f);
  }
#endif
  return 0;
}

int pico_notify_ttl_expired(struct pico_frame *f)
{
  if (0) {}
#ifdef PICO_SUPPORT_ICMP4 
  else if (IS_IPV4(f)) {
    pico_icmp4_ttl_expired(f);
  }
#endif
#ifdef PICO_SUPPORT_ICMP6 
  else if (IS_IPV6(f)) {
    pico_icmp6_ttl_expired(f);
  }
#endif
  return 0;
}


/* Transport layer */
int pico_transport_receive(struct pico_frame *f, uint8_t proto)
{
  int ret = -1;
  switch (proto) {

#ifdef PICO_SUPPORT_ICMP4
  case PICO_PROTO_ICMP4:
    ret = pico_enqueue(pico_proto_icmp4.q_in, f);
    break;
#endif

#ifdef PICO_SUPPORT_IGMP
  case PICO_PROTO_IGMP:
    ret = pico_enqueue(pico_proto_igmp.q_in, f);
    break;
#endif

#ifdef PICO_SUPPORT_UDP
  case PICO_PROTO_UDP:
    ret = pico_enqueue(pico_proto_udp.q_in, f);
    break;
#endif

#ifdef PICO_SUPPORT_TCP
  case PICO_PROTO_TCP:
    ret = pico_enqueue(pico_proto_tcp.q_in, f);
    break;
#endif

  default:
    /* Protocol not available */
    dbg("pkt: no such protocol (%d)\n", proto);
    pico_notify_proto_unreachable(f);
    pico_frame_discard(f);
    ret = -1;
 }
 return ret;
}

int pico_transport_send(struct pico_frame *f)
{
  if (!f || !f->sock || !f->sock->proto) {
    pico_frame_discard(f);
    return -1;
  }
  return f->sock->proto->push(f->sock->net, f);
}

int pico_network_receive(struct pico_frame *f)
{
  if (0) {}
#ifdef PICO_SUPPORT_IPV4
  else if (IS_IPV4(f)) {
    pico_enqueue(pico_proto_ipv4.q_in, f);
  }
#endif
#ifdef PICO_SUPPORT_IPV6
  else if (IS_IPV6(f)) {
    pico_enqueue(pico_proto_ipv6.q_in, f);
  }
#endif
  else {
    dbg("Network not found.\n");
    pico_frame_discard(f);
    return -1;
  }
  return f->buffer_len;
}


/* Network layer: interface towards socket for frame sending */
int pico_network_send(struct pico_frame *f)
{
  if (!f || !f->sock || !f->sock->net) {
    pico_frame_discard(f);
    return -1;
  }
  return f->sock->net->push(f->sock->net, f);
}

int pico_destination_is_local(struct pico_frame *f)
{
  if (0) { }
#ifdef PICO_SUPPORT_IPV4
  else if (IS_IPV4(f)) {
    struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *)f->net_hdr;
    if (pico_ipv4_link_find(&hdr->dst))
      return 1;
  }
#endif
#ifdef PICO_SUPPORT_IPV6
  else if (IS_IPV6(f)) {
  }
#endif
  return 0;
}

int pico_source_is_local(struct pico_frame *f)
{
  if (0) { }
#ifdef PICO_SUPPORT_IPV4
  else if (IS_IPV4(f)) {
    struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *)f->net_hdr;
    if (hdr->src.addr == PICO_IPV4_INADDR_ANY)
      return 1;
    if (pico_ipv4_link_find(&hdr->src))
      return 1;
  }
#endif
#ifdef PICO_SUPPORT_IPV6
  else if (IS_IPV6(f)) {
  /* XXX */
  }
#endif
  return 0;


}


/* DATALINK LEVEL: interface from network to the device
 * and vice versa.
 */

/* The pico_ethernet_receive() function is used by 
 * those devices supporting ETH in order to push packets up 
 * into the stack. 
 */
int pico_ethernet_receive(struct pico_frame *f)
{
  struct pico_eth_hdr *hdr;
  if (!f || !f->dev || !f->datalink_hdr)
    goto discard;
  hdr = (struct pico_eth_hdr *) f->datalink_hdr;
  if ( (memcmp(hdr->daddr, f->dev->eth->mac.addr, PICO_SIZE_ETH) != 0) && 
#ifdef PICO_SUPPORT_MCAST
    (memcmp(hdr->daddr, PICO_ETHADDR_MCAST, PICO_SIZE_MCAST) != 0) &&
#endif
    (memcmp(hdr->daddr, PICO_ETHADDR_ALL, PICO_SIZE_ETH) != 0) ) 
    goto discard;

  f->net_hdr = f->datalink_hdr + sizeof(struct pico_eth_hdr);
  if (hdr->proto == PICO_IDETH_ARP)
    return pico_arp_receive(f);
  if ((hdr->proto == PICO_IDETH_IPV4) || (hdr->proto == PICO_IDETH_IPV6))
    return pico_network_receive(f);
discard:
  pico_frame_discard(f);
  return -1;
}

static int destination_is_bcast(struct pico_frame *f)
{
  if (!f)
    return 0;

  if (IS_IPV6(f))
    return 0;
#ifdef PICO_SUPPORT_IPV4
  else {
    struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *) f->net_hdr;
    return pico_ipv4_is_broadcast(hdr->dst.addr);
  }
#endif
  return 0;
}

#ifdef PICO_SUPPORT_MCAST
static int destination_is_mcast(struct pico_frame *f)
{
  if (!f)
    return 0;

  if (IS_IPV6(f))
    return 0;
#ifdef PICO_SUPPORT_IPV4
  else {
    struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *) f->net_hdr;
    return pico_ipv4_is_multicast(hdr->dst.addr);
  }
#endif
  return 0;
}

static struct pico_eth *pico_ethernet_mcast_translate(struct pico_frame *f, uint8_t *pico_mcast_mac)
{
  struct pico_ipv4_hdr *hdr = (struct pico_ipv4_hdr *) f->net_hdr;

  /* place 23 lower bits of IP in lower 23 bits of MAC */
  pico_mcast_mac[5] = (long_be(hdr->dst.addr) & 0x000000FF);
  pico_mcast_mac[4] = (long_be(hdr->dst.addr) & 0x0000FF00) >> 8; 
  pico_mcast_mac[3] = (long_be(hdr->dst.addr) & 0x007F0000) >> 16;

  return (struct pico_eth *)pico_mcast_mac;
}


#endif /* PICO_SUPPORT_MCAST */

/* This is called by dev loop in order to ensure correct ethernet addressing.
 * Returns 0 if the destination is unknown, and -1 if the packet is not deliverable
 * due to ethernet addressing (i.e., no arp association was possible. 
 *
 * Only IP packets must pass by this. ARP will always use direct dev->send() function, so
 * we assume IP is used.
 */
int pico_ethernet_send(struct pico_frame *f)
{
  struct pico_eth *dstmac = NULL;
  int ret = -1;

  if (IS_IPV6(f)) {
    /*TODO: Neighbor solicitation */
    dstmac = NULL;
  }

  else if (IS_IPV4(f)) {
    if (IS_BCAST(f) || destination_is_bcast(f)) {
      dstmac = (struct pico_eth *) PICO_ETHADDR_ALL;
    } 
#ifdef PICO_SUPPORT_MCAST
    else if (destination_is_mcast(f)) {
      uint8_t pico_mcast_mac[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x00};
      dstmac = pico_ethernet_mcast_translate(f, pico_mcast_mac);
    } 
#endif
    else {
      dstmac = pico_arp_get(f);
      if (!dstmac)
        return 0;
    }
    /* This sets destination and source address, then pushes the packet to the device. */
    if (dstmac && (f->start > f->buffer) && ((f->start - f->buffer) >= PICO_SIZE_ETHHDR)) {
      struct pico_eth_hdr *hdr;
      f->start -= PICO_SIZE_ETHHDR;
      f->len += PICO_SIZE_ETHHDR;
      f->datalink_hdr = f->start;
      hdr = (struct pico_eth_hdr *) f->datalink_hdr;
      memcpy(hdr->saddr, f->dev->eth->mac.addr, PICO_SIZE_ETH);
      memcpy(hdr->daddr, dstmac, PICO_SIZE_ETH);
      hdr->proto = PICO_IDETH_IPV4;
      if(!memcmp(hdr->daddr, hdr->saddr, PICO_SIZE_ETH)){
        dbg("sending out packet destined for our own mac\n");
      return pico_ethernet_receive(f);
      }else if(IS_LIMITED_BCAST(f)){
        ret = pico_device_broadcast(f);
      }else {
        ret = f->dev->send(f->dev, f->start, f->len);
        /* Frame is discarded after this return by the caller */
      }

      if(!ret) pico_frame_discard(f);
        return ret;
    } else {
      return -1;
    }
  } /* End IPV4 ethernet addressing */
  return -1;

}

void pico_store_network_origin(void *src, struct pico_frame *f)
{
  #ifdef PICO_SUPPORT_IPV4
  struct pico_ip4 *ip4;
  #endif

  #ifdef PICO_SUPPORT_IPV6
  struct pico_ip6 *ip6;
  #endif

  #ifdef PICO_SUPPORT_IPV4
  if (IS_IPV4(f)) {
    struct pico_ipv4_hdr *hdr;
    hdr = (struct pico_ipv4_hdr *) f->net_hdr;
    ip4 = (struct pico_ip4 *) src;
    ip4->addr = hdr->src.addr;
  }
  #endif
  #ifdef PICO_SUPPORT_IPV6
  if (IS_IPV6(f)) {
    struct pico_ipv6_hdr *hdr;
    hdr = (struct pico_ipv6_hdr *) f->net_hdr;
    ip6 = (struct pico_ip6 *) src;
    memcpy(ip6->addr, hdr->src.addr, PICO_SIZE_IP6);
  }
  #endif
}


/* LOWEST LEVEL: interface towards devices. */
/* Device driver will call this function which returns immediately.
 * Incoming packet will be processed later on in the dev loop.
 */
int pico_stack_recv(struct pico_device *dev, uint8_t *buffer, int len)
{
  struct pico_frame *f;
  int ret;
  if (len <= 0)
    return -1;
  f = pico_frame_alloc(len);
  if (!f)
    return -1;

  /* Association to the device that just received the frame. */
  f->dev = dev;

  /* Setup the start pointer, lenght. */
  f->start = f->buffer;
  f->len = f->buffer_len;
  if (f->len > 8) {
    int mid_frame = (f->buffer_len >> 2)<<1;
    mid_frame -= (mid_frame % 4);
    pico_rand_feed(*(uint32_t*)(f->buffer + mid_frame));
  }
  memcpy(f->buffer, buffer, len);
  ret = pico_enqueue(dev->q_in, f);
  if (ret <= 0) {
    pico_frame_discard(f);
  }
  return ret;
}

int pico_sendto_dev(struct pico_frame *f)
{
  if (!f->dev) {
    pico_frame_discard(f);
    return -1;
  } else {
    if (f->len > 8) {
      int mid_frame = (f->buffer_len >> 2)<<1;
      mid_frame -= (mid_frame % 4);
      pico_rand_feed(*(uint32_t*)(f->buffer + mid_frame));
    }
    return pico_enqueue(f->dev->q_out, f);
  }
}

struct pico_timer
{
  unsigned long expire;
  void *arg;
  void (*timer)(unsigned long timestamp, void *arg);
};

typedef struct pico_timer pico_timer;

DECLARE_HEAP(pico_timer, expire);

static heap_pico_timer *Timers;

void pico_check_timers(void)
{
  struct pico_timer timer;
  struct pico_timer *t = heap_first(Timers);
  pico_tick = PICO_TIME_MS();
  while((t) && (t->expire < pico_tick)) {
    t->timer(pico_tick, t->arg);
    heap_peek(Timers, &timer);
    t = heap_first(Timers);
  }
}


#define PROTO_DEF_NR      11
#define PROTO_DEF_AVG_NR  4
#define PROTO_DEF_SCORE   32
#define PROTO_MIN_SCORE   32
#define PROTO_MAX_SCORE   128
#define PROTO_LAT_IND     3   /* latecy indication 0-3 (lower is better latency performance), x1, x2, x4, x8 */
#define PROTO_MAX_LOOP    (PROTO_MAX_SCORE<<PROTO_LAT_IND) /* max global loop score, so per tick */

static int calc_score(int *score, int *index, int avg[][PROTO_DEF_AVG_NR], int *ret)
{
  int temp, i, j, sum;
  int max_total = PROTO_MAX_LOOP, total = 0;

  //dbg("USED SCORES> "); 

  for (i = 0; i < PROTO_DEF_NR; i++) {

    /* if used looped score */
    if (ret[i] < score[i]) {
      temp = score[i] - ret[i]; /* remaining loop score */
      
      //dbg("%3d - ",temp);

      if (index[i] >= PROTO_DEF_AVG_NR)
        index[i] = 0;           /* reset index */
      j = index[i];
      avg[i][j] = temp;

      index[i]++; 

      if (ret[i] == 0 && (score[i]<<1 <= PROTO_MAX_SCORE) && ((total+(score[i]<<1)) < max_total) ) {        /* used all loop score -> increase next score directly */
        score[i] <<= 1;
        total += score[i];
        continue;
      }

      sum = 0;
      for (j = 0; j < PROTO_DEF_AVG_NR; j++)
        sum += avg[i][j];       /* calculate sum */

      sum >>= 2;                /* divide by 4 to get average used score */

      /* criterion to increase next loop score */
      if (sum > (score[i] - (score[i]>>2))  && (score[i]<<1 <= PROTO_MAX_SCORE) && ((total+(score[i]<<1)) < max_total)) { /* > 3/4 */
        score[i] <<= 1;         /* double loop score */
        total += score[i];
        continue;
      }

      /* criterion to decrease next loop score */
      if (sum < (score[i]>>2) && (score[i]>>1 >= PROTO_MIN_SCORE)) { /* < 1/4 */
        score[i] >>= 1;         /* half loop score */
        total += score[i];
        continue;
      }

      /* also add non-changed scores */
      total += score[i];
    }
    else if (ret[i] == score[i]) {
      /* no used loop score - gradually decrease */
      
    //  dbg("%3d - ",0);

      if (index[i] >= PROTO_DEF_AVG_NR)
        index[i] = 0;           /* reset index */
      j = index[i];
      avg[i][j] = 0;

      index[i]++; 

      sum = 0;
      for (j = 0; j < PROTO_DEF_AVG_NR; j++)
        sum += avg[i][j];       /* calculate sum */

      sum >>= 2;                /* divide by 4 to get average used score */

      if ((sum == 0) && (score[i]>>1 >= PROTO_MIN_SCORE)) {
        score[i] >>= 1;         /* half loop score */
        total += score[i];
        for (j = 0; j < PROTO_DEF_AVG_NR; j++)
          avg[i][j] = score[i];
      }
      
    }
  }

  //dbg("\n");

  return 0;
}



/* 

         .                                                               
       .vS.                                                              
     <aoSo.                                                              
    .XoS22.                                                              
    .S2S22.             ._...              ......            ..._.       
  :=|2S2X2|=++;      <vSX2XX2z+          |vSSSXSSs>.      :iXXZUZXXe=    
  )2SS2SS2S2S2I    =oS2S2S2S2X22;.    _vuXS22S2S2S22i  ._wZZXZZZXZZXZX=  
  )22S2S2S2S2Sl    |S2S2S22S2SSSXc:  .S2SS2S2S22S2SS= .]#XZZZXZXZZZZZZ:  
  )oSS2SS2S2Sol     |2}!"""!32S22S(. uS2S2Se**12oS2e  ]dXZZXX2?YYXXXZ*   
   .:2S2So:..-.      .      :]S2S2e;=X2SS2o     .)oc  ]XZZXZ(     =nX:   
    .S2S22.          ___s_i,.)oS2So(;2SS2So,       `  3XZZZZc,      -    
    .S2SSo.        =oXXXSSS2XoS2S2o( XS2S2XSos;.      ]ZZZZXXXX|=        
    .S2S22.      .)S2S2S22S2S2S2S2o( "X2SS2S2S2Sus,,  +3XZZZZZXZZoos_    
    .S2S22.     .]2S2SS22S222S2SS2o(  ]S22S2S2S222So   :3XXZZZZZZZZXXv   
    .S2S22.     =u2SS2e"~---"{2S2So(   -"12S2S2SSS2Su.   "?SXXXZXZZZZXo  
    .S2SSo.     )SS22z;      :S2S2o(       ={vS2S2S22v      .<vXZZZZZZZ; 
    .S2S2S:     ]oSS2c;      =22S2o(          -"S2SS2n          ~4XXZXZ( 
    .2S2S2i     )2S2S2[.    .)XS2So(  <;.      .2S2S2o :<.       ]XZZZX( 
     nX2S2S,,_s_=3oSS2SoaasuXXS2S2o( .oXoasi_aioSSS22l.]dZoaas_aadXZZXZ' 
     vS2SSSXXX2; )S2S2S2SoS2S2S2S2o( iS2S222XSoSS22So.)nXZZXXXZZXXZZXZo  
     +32S22S2Sn  -+S2S2S2S2So22S2So( 12S2SS2S2SS22S}- )SXXZZZZZZZZZXX!-  
      .)S22222i    .i2S2S2o>;:S2S2o(  .<vSoSoSo2S(;     :nXXXXXZXXX(     
       .-~~~~-        --- .   - -        --~~~--           --^^~~-       
                                  .                                      
                                                                         

 ... curious about our source code? We are hiring! mailto:<recruiting@tass.be>


*/

void pico_stack_tick(void)
{
    static int score[PROTO_DEF_NR] = {PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE, PROTO_DEF_SCORE};
    static int index[PROTO_DEF_NR] = {0,0,0,0,0,0};
    static int avg[PROTO_DEF_NR][PROTO_DEF_AVG_NR];
    static int ret[PROTO_DEF_NR] = {0};

    pico_check_timers();

    //dbg("LOOP_SCORES> %3d - %3d - %3d - %3d - %3d - %3d - %3d - %3d - %3d - %3d - %3d\n",score[0],score[1],score[2],score[3],score[4],score[5],score[6],score[7],score[8],score[9],score[10]);

    //score = pico_protocols_loop(100);

    ret[0] = pico_devices_loop(score[0],PICO_LOOP_DIR_IN);
    pico_rand_feed(ret[0]);

    ret[1] = pico_protocol_datalink_loop(score[1], PICO_LOOP_DIR_IN);
    pico_rand_feed(ret[1]);

    ret[2] = pico_protocol_network_loop(score[2], PICO_LOOP_DIR_IN);
    pico_rand_feed(ret[2]);

    ret[3] = pico_protocol_transport_loop(score[3], PICO_LOOP_DIR_IN);
    pico_rand_feed(ret[3]);


    ret[5] = score[5];
#if defined (PICO_SUPPORT_IPV4) || defined (PICO_SUPPORT_IPV6)
#if defined (PICO_SUPPORT_TCP) || defined (PICO_SUPPORT_UDP)
    ret[5] = pico_sockets_loop(score[5]); // swapped
    pico_rand_feed(ret[5]);
#endif
#endif

    ret[4] = pico_protocol_socket_loop(score[4], PICO_LOOP_DIR_IN);
    pico_rand_feed(ret[4]);


    ret[6] = pico_protocol_socket_loop(score[6], PICO_LOOP_DIR_OUT);
    pico_rand_feed(ret[6]);

    ret[7] = pico_protocol_transport_loop(score[7], PICO_LOOP_DIR_OUT);
    pico_rand_feed(ret[7]);

    ret[8] = pico_protocol_network_loop(score[8], PICO_LOOP_DIR_OUT);
    pico_rand_feed(ret[8]);

    ret[9] = pico_protocol_datalink_loop(score[9], PICO_LOOP_DIR_OUT);
    pico_rand_feed(ret[9]);

    ret[10] = pico_devices_loop(score[10],PICO_LOOP_DIR_OUT);
    pico_rand_feed(ret[10]);

    /* calculate new loop scores for next iteration */
    calc_score(score, index,(int (*)[]) avg, ret);
}

void pico_stack_loop(void)
{
  while(1) {
    pico_stack_tick();
    PICO_IDLE();
  }
}

void pico_timer_add(unsigned long expire, void (*timer)(unsigned long, void *), void *arg)
{
  pico_timer t;
  t.expire = PICO_TIME_MS() + expire;
  t.arg = arg;
  t.timer = timer;
  heap_insert(Timers, &t);
  if (Timers->n > PICO_MAX_TIMERS) {
    dbg("Warning: I have %d timers\n", Timers->n);
  }
}

void pico_stack_init(void)
{

#ifdef PICO_SUPPORT_IPV4
  pico_protocol_init(&pico_proto_ipv4);
#endif

#ifdef PICO_SUPPORT_IPV6
  pico_protocol_init(&pico_proto_ipv6);
#endif

#ifdef PICO_SUPPORT_ICMP4
  pico_protocol_init(&pico_proto_icmp4);
#endif

#ifdef PICO_SUPPORT_IGMP
  pico_protocol_init(&pico_proto_igmp);
#endif

#ifdef PICO_SUPPORT_UDP
  pico_protocol_init(&pico_proto_udp);
#endif

#ifdef PICO_SUPPORT_TCP
  pico_protocol_init(&pico_proto_tcp);
#endif

#ifdef PICO_SUPPORT_DNS_CLIENT
  pico_dns_client_init();
#endif

  pico_rand_feed(123456);

  /* Initialize timer heap */
  Timers = heap_init();
  pico_stack_tick();
  pico_stack_tick();
  pico_stack_tick();
}

