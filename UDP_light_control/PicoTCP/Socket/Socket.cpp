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
#include "Socket/Socket.h"
#include "wrapper.h"
#include "proxy_endpoint.h"
#include <cstring>

using std::memset;

Socket::Socket() : _ep(NULL), _blocking(true)
{
    //set_blocking(true,1500); // ?
}

void Socket::set_blocking(bool blocking, unsigned int timeout) {
    _blocking = blocking;
    _timeout = timeout;
}

int Socket::init_socket(int type) {
    if (_ep != NULL)
    {
        mbed_dbg("Sock open already...\n");
        return -1;
    }
    struct stack_endpoint *ep  = picotcp_socket(AF_INET, type, 0);
    if (!ep)
    {
        mbed_dbg("Error opening socket...\n");
        return -1;
    }
    _ep = ep;
    return 0;
}

int Socket::set_option(int level, int optname, const void *optval, socklen_t optlen) {
    (void)level;
    return picotcp_setsockopt(_ep, optname, (void *)optval);
}

int Socket::get_option(int level, int optname, void *optval, socklen_t *optlen) {
    (void)level;
    return picotcp_getsockopt(_ep, optname, optval);
}

int Socket::select(struct timeval *timeout, bool read, bool write) {
    return picotcp_select(_ep, timeout, read, write);
}

int Socket::wait_readable(TimeInterval& timeout) {
    return (select(&timeout._time, true, false) == 0 ? -1 : 0);
}

int Socket::wait_writable(TimeInterval& timeout) {
    return (select(&timeout._time, false, true) == 0 ? -1 : 0);
}

int Socket::is_readable(void) {
    return (this->_ep->revents & PICO_SOCK_EV_RD);
}

int Socket::is_writable(void) {
    return (this->_ep->revents & PICO_SOCK_EV_WR);
}

int Socket::close() {
    if (_ep == NULL)
        return -1;
    picotcp_close(_ep);
    _ep = NULL;
    return 0;
}

Socket::~Socket() {
    if(_ep)
    {
        picotcp_close(_ep);
        _ep = NULL;
    }
}

TimeInterval::TimeInterval(unsigned int ms) {
    _time.tv_sec = ms / 1000;
    _time.tv_usec = (ms - (_time.tv_sec * 1000)) * 1000;
}
