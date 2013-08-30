#ifndef ____WRAPPER_H
#define ____WRAPPER_H

#ifdef __cplusplus
extern "C"{
#endif

#include "pico_ipv4.h"
#include "pico_ipv6.h"
#include "pico_stack.h"
#include "pico_socket.h"

#ifdef __cplusplus
}
#endif

#include "mbed.h"
#include "rtos.h"

struct stack_endpoint {
  uint16_t sock_fd;
  struct pico_socket *s;
  int connected;
  int events;
  int revents;
  Queue<void,1> *queue;//receive queue of 1 element of type 
  uint32_t timeout; // this is used for timeout sockets
  int state; // for pico_state
  uint8_t broadcast;
};

void picotcp_start(void);
struct stack_endpoint *pico_get_socket(uint16_t sockfd);
#endif
