import socket
import fcntl, os
import errno
import random, string
from time import time, sleep

ECHO_SERVER_ADDRESS = "192.168.32.139"
NUMBER_OF_SECONDS = 20
LOCAL_SERVER_PORT = 2327

class coloredLight(object):
    def __init__(self, ip='192.168.32.139', port=2327):
	self.ip = ip
	self.port = port
	self.red = self.green = self.blue = self.white = 0;
	self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
	self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	self.sock.bind(('', self.port))

    def send(self):
	data = "a %f %f %f %f" % (self.red, self.green, self.blue, self.white)
	print data
	self.sock.sendto(data, (self.ip, self.port))

def main():
    light = coloredLight()
    light.red = 0.015
    light.green = 0.02
    light.blue = 0.03
    light.white = 0.05
    light.send()

if __name__ == '__main__':
    main()
