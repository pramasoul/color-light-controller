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
#include "Socket/Endpoint.h"
#include "wrapper.h"
#include "proxy_endpoint.h"

extern "C"
{
#include "pico_ipv4.h"
}
#include <cstring>
#include <cstdio>

Endpoint::Endpoint()  {
    reset_address();
}
Endpoint::~Endpoint() {}

void Endpoint::reset_address(void) {
    memset(&_remoteHost,0,sizeof(_remoteHost));
}

#include "stdio.h"

int Endpoint::set_address(const char* host, const int port) {
    _remoteHost.sin_port = short_be(port);
    if(pico_string_to_ipv4(host,&_remoteHost.sin_addr.s_addr) < 0)
    {
        // execute a dns query because this is a name
        struct hostent * _host = gethostbyname(host);
        if(_host && _host->h_addr_list[0])
        {
            memcpy(_ipAddress,_host->h_addr_list[0],strlen(_host->h_addr_list[0])+1);
            mbed_dbg("Dns result : %s\n",_ipAddress);
            pico_string_to_ipv4(_ipAddress,&_remoteHost.sin_addr.s_addr);
            delete _host->h_name;
            delete _host->h_addr_list;
            delete _host;
        }
        else 
            return -1;
    }
    else
        memcpy(_ipAddress,host, strlen(host)+1);
        
    return 0;
}

char* Endpoint::get_address() {
    return _ipAddress;
}

int   Endpoint::get_port() {
    return short_be(_remoteHost.sin_port);
}
