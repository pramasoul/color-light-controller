/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

Authors: Daniele Lacamera
*********************************************************************/

#include "wrapper.h"
#include "rtos.h"
#include "cmsis_os.h"
#include "mbed.h"
#include "Socket.h"
#include "Mutex.h"

extern "C"
{
    #include "pico_dns_client.h"
}

//#define ptsock_dbg mbed_dbg
#define ptsock_dbg(...)

int in_the_stack = 0;
Mutex *PicoTcpLock;
Queue<void,32> *PicoTcpEvents;

static struct stack_endpoint *ep_accepting;
static Thread * serverThread = NULL;

/* Testing ng blocking mechanism */

/* 
 * backend of select function, used in blocking (like picotcp_read()...) 
 * calls. Sleeps on the message queue 
 * 
 *
 * WARNING: PicoTcpLock (big stack lock) must be acquired before entering this.
 */

static inline int __critical_select(struct stack_endpoint *ep, uint32_t time, uint8_t lock)
{
  int retval = 0;
  uint16_t ev = ep->revents;
  uint32_t in_time = PICO_TIME_MS();
  
  if(lock) PicoTcpLock->unlock();
  while ((ep->events & ep->revents) == 0) {
    ep->queue->get(time);
    if ((time != osWaitForever) && (PICO_TIME_MS() > in_time + time)) {
        ptsock_dbg("TIMEOUT in critical select... (ev:%04x rev:%04x \n", ep->events, ep->revents);
        if(lock) PicoTcpLock->lock();
        return 0;
    }
  }
  if(lock) PicoTcpLock->lock();
  return 1;
}

static void wakeup(uint16_t ev, struct pico_socket *s)
{
  struct stack_endpoint *ep = (struct stack_endpoint *)s->priv;
  if (!ep) {
    if (ep_accepting != NULL) {
        ptsock_dbg("Delivering %02x to accepting socket...\n", ev);
        ep = ep_accepting;
    } else {
        ptsock_dbg("WAKEUP: socket not found! ev=%04x\n", ev);
        return;
    }
  }
  //if (((ep->revents & PICO_SOCK_EV_RD) == 0) && (ev & PICO_SOCK_EV_RD))
  //  printf("Activating RD\n");
  ep->revents |= ev;

  if(ev & PICO_SOCK_EV_ERR)
  {
    if(pico_err == PICO_ERR_ECONNRESET)
    {   
      ptsock_dbg("Connection reset by peer...\n");
      ep->state = SOCK_RESET_BY_PEER;
      pico_socket_close(ep->s);
      ep->s->priv = NULL;
    }
  }
  if ((ev & PICO_SOCK_EV_CLOSE) || (ev & PICO_SOCK_EV_FIN)) {
    ep->connected = 0;
    pico_socket_close(ep->s);
    ep->state = SOCK_CLOSED;
  }
  if ((ev & PICO_SOCK_EV_CONN) || (ev & PICO_SOCK_EV_RD)) {
      ep->connected = 1;
      ep->state = SOCK_CONNECTED;
  }
  ep->queue->put((void *)0);
}


struct stack_endpoint *picotcp_socket(uint16_t net, uint16_t proto, uint16_t timeout)
{
  struct stack_endpoint *ep = (struct stack_endpoint *)pico_zalloc(sizeof(struct stack_endpoint));
  uint16_t p_net = ((net == AF_INET6)?PICO_PROTO_IPV6:PICO_PROTO_IPV4);
  uint16_t p_proto = ((proto == SOCK_DGRAM)?PICO_PROTO_UDP:PICO_PROTO_TCP);
  PicoTcpLock->lock();
  ep->s = pico_socket_open( p_net, p_proto, &wakeup );
  if (ep->s == NULL) {
    delete(ep->queue);
    pico_free(ep);
    ep = NULL;
    ptsock_dbg("Error opening socket!\n");
  } else {
    ep->s->priv = ep;
    ptsock_dbg("Added socket (open)\n");
    ep->state = SOCK_OPEN;
    ep->queue = new Queue<void,1>();
  }
  PicoTcpLock->unlock();
  return ep;
}


int picotcp_state(struct stack_endpoint *ep)
{
    return ep->state;
}

int picotcp_bind(struct stack_endpoint *ep, struct sockaddr *_local_addr, socklen_t len)
{
  int ret;
  struct sockaddr_in *local_addr;
  local_addr = (struct sockaddr_in *)_local_addr;
  
  PicoTcpLock->lock();
  ret = pico_socket_bind(ep->s, (struct pico_ip4 *)(&local_addr->sin_addr.s_addr), &local_addr->sin_port);
  if(ret == 0)
    ep->state = SOCK_BOUND;
  PicoTcpLock->unlock();
  return ret;
}

int picotcp_listen(struct stack_endpoint *ep, int queue)
{
  int ret;
  PicoTcpLock->lock();
  ret = pico_socket_listen(ep->s, queue);
  ep_accepting = (struct stack_endpoint *) pico_zalloc(sizeof(struct stack_endpoint));
  ep_accepting->queue = new Queue<void,1>();
  if (!ep_accepting)
    ret = -1;
  if(ret == 0)
    ep->state = SOCK_LISTEN;
  PicoTcpLock->unlock();
  return ret;
}

int picotcp_connect(struct stack_endpoint *ep, struct sockaddr *_srv_addr, socklen_t len)
{
  int retval;
  struct sockaddr_in *srv_addr;
  srv_addr = (struct sockaddr_in *)_srv_addr;
  PicoTcpLock->lock();
  pico_socket_connect(ep->s, (struct pico_ip4 *)(&srv_addr->sin_addr.s_addr), srv_addr->sin_port);
  ep->events = PICO_SOCK_EV_CONN | PICO_SOCK_EV_ERR;
  __critical_select(ep, osWaitForever, 1);
  if ((ep->revents & PICO_SOCK_EV_CONN) && ep->connected) {
    ep->revents &= (~PICO_SOCK_EV_CONN);
    ep->revents |= PICO_SOCK_EV_WR;
    ptsock_dbg("Established. sock state: %x\n", ep->s->state);
    ep->state = SOCK_CONNECTED;
    retval = 0;
  } else {
    retval = -1;
  }
  PicoTcpLock->unlock();
  return retval;
}

struct stack_endpoint *picotcp_accept(struct stack_endpoint *ep, struct sockaddr *_cli_addr, socklen_t *len)
{
  int retval;
  struct stack_endpoint *aep = ep_accepting;
  struct sockaddr_in *cli_addr = (struct sockaddr_in *)_cli_addr;
  ep_accepting = (struct stack_endpoint *) pico_zalloc(sizeof(struct stack_endpoint));
  if (ep_accepting)
    ep_accepting->queue = new Queue<void,1>();
  
  
  if (!aep)
    return aep;
  
  PicoTcpLock->lock();
  ep->events = PICO_SOCK_EV_CONN | PICO_SOCK_EV_ERR;
  __critical_select(ep, osWaitForever, 1);
  if (ep->revents & PICO_SOCK_EV_CONN) {
    ptsock_dbg("Calling Accept\n");
    aep->s = pico_socket_accept(ep->s, (struct pico_ip4 *)(&cli_addr->sin_addr.s_addr), &cli_addr->sin_port);
    ptsock_dbg("Accept returned\n");
    aep->s->priv = aep;
    ep->revents &= (~PICO_SOCK_EV_CONN);
    aep->revents = 0; // set this to 0 to allow seq connections
    aep->revents |= PICO_SOCK_EV_WR;
    aep->state = SOCK_CONNECTED;
    ptsock_dbg("Added socket (accept)\n");
    
    *len = sizeof(struct sockaddr_in);
    ptsock_dbg("Established. sock state: %x\n", aep->s->state);
  } else {
    pico_free(aep);
    aep = NULL;
  }
  PicoTcpLock->unlock();
  return aep;
}

int picotcp_select(struct stack_endpoint *ep, struct timeval *timeout, int read, int write)
{
  int ret;
  ep->timeout |= timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
  ep->events = PICO_SOCK_EV_ERR;
  ep->events |= PICO_SOCK_EV_FIN;
  ep->events |= PICO_SOCK_EV_CLOSE;
  ep->events |= PICO_SOCK_EV_CONN;
  if (read) {
    ep->events |= PICO_SOCK_EV_RD;
  }
  if (write)
    ep->events |= PICO_SOCK_EV_WR;
  ret = __critical_select(ep, ep->timeout, 0);
  return ret;
}

int picotcp_send(struct stack_endpoint *ep,void * buf, int len, int flags)
{
  int retval = 0;
  int tot_len = 0;
  if (!buf || (len <= 0))
     return  0;
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_send(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len);
    if (retval == 0) {
        if (tot_len < len)
            ep->revents &= ~PICO_SOCK_EV_WR;
        break;
    }
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len; 
}

int picotcp_recv(struct stack_endpoint *ep,void * buf, int len, int flags)
{
  int retval = 0;
  int tot_len = 0;
  if (!buf || (len <= 0))
     return  0;
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_recv(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len);
    if (retval == 0) {
        if (tot_len < len)
            ep->revents &= ~PICO_SOCK_EV_RD;
        break;
    }
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len;
}

int picotcp_sendto(struct stack_endpoint * ep,void * buf, int len, struct sockaddr* a,socklen_t size)
{
  int retval = 0;
  int tot_len = 0;
  struct sockaddr_in * in = (struct sockaddr_in *)a;
  if (!buf || (len <= 0))
     return  0;
  if(!ep->broadcast && pico_ipv4_is_broadcast(in->sin_addr.s_addr))
     return -1;
     
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_sendto(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len,(struct pico_ip4 *)&in->sin_addr.s_addr, in->sin_port);
    
    if (retval == 0)
        break;
    
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len; 
}

int picotcp_recvfrom(struct stack_endpoint *ep,void * buf, int len, struct sockaddr *a, socklen_t * size)
{
  (void)len;
  int retval = 0;
  int tot_len = 0;
  struct sockaddr_in * in = (struct sockaddr_in *)a;
  if (!buf || (len <= 0))
     return  0;
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_recvfrom(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len,(struct pico_ip4 *)&(in->sin_addr.s_addr),&in->sin_port);
    if (retval == 0) {
        if (tot_len < len)
            ep->revents &= ~PICO_SOCK_EV_RD;
        break;
    }
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len;
}

int picotcp_read(struct stack_endpoint *ep,void *buf, int len)
{
  int retval = 0;
  int tot_len = 0;
  if (!buf || (len <= 0))
     return  0;
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_read(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len);
    if (retval == 0) {
        if (tot_len < len)
            ep->revents &= ~PICO_SOCK_EV_RD;
        break;
    }
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len;
}

int picotcp_write(struct stack_endpoint *ep,void *buf, int len)
{
  int retval = 0;
  int tot_len = 0;
  if (!buf || (len <= 0))
     return  0;
  PicoTcpLock->lock();
  while (tot_len < len) {
    retval = pico_socket_write(ep->s, ((uint8_t *)buf) + tot_len ,  len - tot_len);
    
    if (retval == 0) {
        if (tot_len < len)
            ep->revents &= ~PICO_SOCK_EV_WR;
        break;
    }
    if (retval < 0) {
       if(tot_len == 0) tot_len = -1;
       break;
    }
    tot_len += retval;
  }
  PicoTcpLock->unlock();
  picotcp_async_interrupt(NULL);
  return tot_len; 
}


int picotcp_setsockopt(struct stack_endpoint *ep, int option, void *value)
{
  int ret;
  
  PicoTcpLock->lock();
  if(option == SO_BROADCAST)
  {
    ep->broadcast = *(int *)value;
    ret = 0;
  }
  else
    ret = pico_socket_setoption(ep->s,option,value);
  PicoTcpLock->unlock();
  
  return ret; 
}

int picotcp_getsockopt(struct stack_endpoint *ep, int option, void *value)
{
  int ret;
  
  PicoTcpLock->lock();
  ret = pico_socket_getoption(ep->s,option,value);
  PicoTcpLock->unlock();
  
  return ret;
}

int picotcp_close(struct stack_endpoint *ep)
{
  PicoTcpLock->lock();
  if(ep->state != SOCK_RESET_BY_PEER)
  {
    pico_socket_close(ep->s);
    ep->s->priv = NULL;
  }
  ptsock_dbg("Socket closed!\n");
  delete(ep->queue);
  pico_free(ep);
  PicoTcpLock->unlock();
}

int picotcp_join_multicast(struct stack_endpoint *ep,const char* address,const char* local)
{
  int ret;
  struct pico_ip_mreq mreq={};
  
  PicoTcpLock->lock();
  pico_string_to_ipv4(address,&mreq.mcast_group_addr.addr);
  pico_string_to_ipv4(local,&mreq.mcast_link_addr.addr);
  ret = pico_socket_setoption(ep->s, PICO_IP_ADD_MEMBERSHIP, &mreq);
  PicoTcpLock->unlock();
  
  return ret; 
}



void pico_wrapper_loop(const void * arg)
{
  (void)arg;
  int ret = 0;
  struct pico_device *dev;
  while(1) {
    
    osEvent evt = PicoTcpEvents->get(5);
    
    if (evt.status == osEventMessage) {
        dev = (struct pico_device *)evt.value.p;
    } else {
        dev = NULL;
    }
    PicoTcpLock->lock();
    if (dev && dev->dsr)
      dev->dsr(dev, 5);
    pico_stack_tick();
    pico_stack_tick();
    PicoTcpLock->unlock();
  }
}

void picotcp_start(void)
{
  if (serverThread == NULL) {
    PicoTcpLock = new Mutex();
    PicoTcpEvents = new Queue<void,32>();
    ptsock_dbg (" *** PicoTCP initialized *** \n");
    serverThread = new Thread(pico_wrapper_loop);
    serverThread->set_priority(osPriorityIdle);
  }
}

void picotcp_init(void)
{
  picotcp_start(); 
}

int picotcp_async_interrupt(void *arg)
{
    PicoTcpEvents->put(arg);
}

// *************** DNS part *************** 
void dns_cb(char *ip,void *arg)
{
  if(!arg)
    goto fail;
  // send the result back  
  ((Queue<void,1> *)arg)->put((void *)ip);
fail:
  if(ip) free(ip);    
}

// dns get host
struct hostent *picotcp_gethostbyname(const char *name)
{
    Queue<void,1> *dnsResult = new Queue<void,1>();
    struct hostent * hostdata = NULL;
    char *ip;
    
    if(!dnsResult)
        return NULL;
    
    pico_dns_client_getaddr(name,dns_cb,dnsResult);
 
    osEvent evt = dnsResult->get(7000);// 7 seconds timeout
    if (evt.status == osEventMessage){
        ip = (char *)evt.value.p;
        hostdata = new struct hostent;
        hostdata->h_name = new char[strlen(name)+1];
        strcpy(hostdata->h_name,name);
        hostdata->h_aliases=NULL;
        hostdata->h_addrtype = AF_INET;
        hostdata->h_length = strlen(ip);
        hostdata->h_addr_list = new char*[2];
        hostdata->h_addr_list[0] = ip;
        hostdata->h_addr_list[1] = NULL;
    }
    
    free(dnsResult);
    return hostdata;
}