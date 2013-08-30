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

#include "Socket/UDPSocket.h"
#include "wrapper.h"
#include "proxy_endpoint.h"

#include <cstring>

using std::memset;

UDPSocket::UDPSocket() {
}

int UDPSocket::init(void) {
    return init_socket(SOCK_DGRAM);
}

// Server initialization
int UDPSocket::bind(int port) {
    if (init_socket(SOCK_DGRAM) < 0)
        return -1;

    struct sockaddr_in localHost; 

    std::memset(&localHost, 0, sizeof(localHost));
    
    localHost.sin_family = AF_INET;
    localHost.sin_port = short_be(port);
    localHost.sin_addr.s_addr = INADDR_ANY;
    
    if (picotcp_bind(_ep, (struct sockaddr *) &localHost, (socklen_t)sizeof(localHost)) < 0) {
        close();
        return -1;
    }
    
    return 0;
}

int UDPSocket::join_multicast_group(EthernetInterface& eth, const char* address) {
    
    return picotcp_join_multicast(_ep,address,eth.getIPAddress());
}

int UDPSocket::set_broadcasting(void) {
    int option = 1;
    return set_option(SOL_SOCKET, SO_BROADCAST, &option, sizeof(option));
}

// -1 if unsuccessful, else number of bytes written
int UDPSocket::sendTo(Endpoint &remote, char *packet, int length) {
    if (!_ep)
    {
        mbed_dbg("Error on socket descriptor \n");
        return -1;
    }
    
    return picotcp_sendto(_ep, packet, length, (struct sockaddr *) &remote._remoteHost, sizeof(struct sockaddr_in));
}

// -1 if unsuccessful, else number of bytes received
int UDPSocket::receiveFrom(Endpoint &remote, char *buffer, int length) {
    socklen_t remoteHostLen = sizeof(remote._remoteHost);  
    int ret;
    
    if (!_ep)
        return -1;
    
    if(is_readable())
    {
        ret = picotcp_recvfrom(_ep, buffer, length, (struct sockaddr*) &remote._remoteHost, &remoteHostLen);
        if(ret < length)
            _ep->revents &= (~PICO_SOCK_EV_RD);
        if(ret) // data was read or error was reported
            return ret;
    }   
        
    TimeInterval timeout(!_blocking ? _timeout : osWaitForever);
    if (wait_readable(timeout) != 0)
        return 0;


    ret = picotcp_recvfrom(_ep, buffer, length, (struct sockaddr*) &remote._remoteHost, &remoteHostLen);
    if(ret < length)
        _ep->revents &= (~PICO_SOCK_EV_RD);
    
    return ret;
}
