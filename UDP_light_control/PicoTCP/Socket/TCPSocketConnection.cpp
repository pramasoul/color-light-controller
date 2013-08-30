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
#include "TCPSocketConnection.h"
#include "wrapper.h"
#include <cstring>

using std::memset;
using std::memcpy;

TCPSocketConnection::TCPSocketConnection() :
        _is_connected(false) {
}

int TCPSocketConnection::connect(const char* host, const int port) {
    if (init_socket(SOCK_STREAM) < 0)
    {
        mbed_dbg("init_socket\n");
        return -1;
    }
   
    if (set_address(host, port) != 0)
    {
        mbed_dbg("set_address\n");
        return -1;
    }
    if (picotcp_connect(_ep, (struct sockaddr *) &_remoteHost, sizeof(_remoteHost)) < 0) {
        close();
        return -1;
    }
    _is_connected = true;
    
    return 0;
}

bool TCPSocketConnection::is_connected(void) {
    return _is_connected;
}

int TCPSocketConnection::send(char* data, int length) {
    int ret;
    if ((_ep == NULL) || !_is_connected)
        return -1;
    
    if(is_writable())
    {
        ret = picotcp_write(_ep, data, length);
        if(ret < length)
            _ep->revents &= (~PICO_SOCK_EV_WR);
        if(ret) // data was read or error was reported
            return ret;
    }
    
    TimeInterval timeout(!_blocking ? _timeout : osWaitForever);
    if (wait_writable(timeout) != 0)
    {
        return 0; /* Timeout */
    }
    
    ret = picotcp_write(_ep, data, length);
    if (ret < length) {
        _ep->revents &= (~PICO_SOCK_EV_WR);
        //mbed_dbg("Short write\n");
    }
    return ret;
}

// -1 if unsuccessful, else number of bytes written
int TCPSocketConnection::send_all(char* data, int length) {
    return send(data,length);
}

int TCPSocketConnection::receive(char* data, int length) {
    int ret;
    if ((_ep == NULL) || !_is_connected)
        return -1;
    
    if(is_readable())
    {
        ret = picotcp_read(_ep, data, length);
        if(ret<length)
            _ep->revents &= (~PICO_SOCK_EV_RD);
        if(ret) // data was read or error was reported
            return ret;
    }   
    
    TimeInterval timeout(!_blocking ? _timeout : osWaitForever);
    if (wait_readable(timeout) != 0)
    {
        return 0; /* Timeout */
    }

    ret = picotcp_read(_ep, data, length);
    if (ret < length) {
        _ep->revents &= (~PICO_SOCK_EV_RD);
        //mbed_dbg("Short read\n");
    }
    return ret;
}

// -1 if unsuccessful, else number of bytes received
int TCPSocketConnection::receive_all(char* data, int length) {
    return receive(data, length);
}
