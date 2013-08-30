/*********************************************************************
PicoTCP. Copyright (c) 2013 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

Author: Daniele Lacamera <daniele.lacamera@tass.be>
*********************************************************************/

#ifndef __PICOTCP_BSD_LAYER
#define __PICOTCP_BSD_LAYER

extern "C"  {
#include "pico_stack.h"
#include "pico_tree.h"
#include "pico_dns_client.h"
#include "pico_socket.h"
#include "pico_addressing.h"
#include "pico_protocol.h"
#include "pico_config.h"
#include "pico_ipv4.h"
#include "pico_device.h"
};
#include "mbed.h"
#include "rtos.h"
#include "Queue.h"
#include "PicoCondition.h"

struct sockaddr;

struct ip_addr_s {
    uint32_t s_addr;
};

typedef struct ip_addr_s ip_addr_t;

struct sockaddr_in 
{
    ip_addr_t sin_addr;
    uint16_t sin_family;
    uint16_t sin_port;
};

struct timeval {
    uint32_t tv_sec;
    uint32_t tv_usec;
};

/* Description of data base entry for a single host.  */
struct hostent
{
  char *h_name;     /* Official name of host.  */
  char **h_aliases;   /* Alias list.  */
  int h_addrtype;   /* Host address type.  */
  int h_length;     /* Length of address.  */
  char **h_addr_list;   /* List of addresses from name server.  */
# define  h_addr  h_addr_list[0] /* Address, for backward compatibility.*/
};

#ifndef PF_INET
#define PF_INET     2
#define PF_INET6    10
#define AF_INET     PF_INET
#define AF_INET6    PF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET 1
#define SO_BROADCAST  6

#define INADDR_ANY 0u

#endif

//#define mbed_dbg printf
#define mbed_dbg(...)

enum socket_state_e {
    SOCK_OPEN,
    SOCK_BOUND,
    SOCK_LISTEN,
    SOCK_CONNECTED,
    SOCK_RESET_BY_PEER,
    SOCK_CLOSED
};

typedef int socklen_t;

void picotcp_init(void);

struct stack_endpoint * picotcp_socket(uint16_t net, uint16_t proto, uint16_t flags);
int picotcp_state(struct stack_endpoint *);
int picotcp_bind(struct stack_endpoint *, struct sockaddr *local_addr, socklen_t len);

int picotcp_listen(struct stack_endpoint *, int queue);
int picotcp_connect(struct stack_endpoint *, struct sockaddr *srv_addr, socklen_t len);
struct stack_endpoint * picotcp_accept(struct stack_endpoint *, struct sockaddr *orig, socklen_t *);
int picotcp_select(struct stack_endpoint *, struct timeval *timeout, int read, int write);

int picotcp_send(struct stack_endpoint *,void * buff, int len, int flags);
int picotcp_recv(struct stack_endpoint *,void * buff, int len, int flags);
int picotcp_sendto(struct stack_endpoint *,void * buff, int len, struct sockaddr*,socklen_t);
int picotcp_recvfrom(struct stack_endpoint *,void * buff, int len, struct sockaddr *, socklen_t *);
int picotcp_read(struct stack_endpoint *,void *buf, int len);
int picotcp_write(struct stack_endpoint *,void *buf, int len);
int picotcp_setsockopt(struct stack_endpoint *, int option, void *value);
int picotcp_getsockopt(struct stack_endpoint *, int option, void *value);
struct hostent * picotcp_gethostbyname(const char *url);
char * picotcp_gethostbyaddr(const char *ip);
int picotcp_close(struct stack_endpoint *);
// set blocking
int picotcp_setblocking(struct stack_endpoint *,int blocking);
int picotcp_join_multicast(struct stack_endpoint *,const char* address,const char* local);

int picotcp_async_interrupt(void *);
struct hostent *picotcp_gethostbyname(const char *name);

#endif
