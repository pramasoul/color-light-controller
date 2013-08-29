import socket
#import fcntl, os
#import errno
#import random, string
#from time import time, sleep

class coloredLight(object):
    def __init__(self, ip='192.168.32.139', port=2327):
	self.ip = ip
	self.port = port
	self.debug = False
	self.red = self.green = self.blue = self.white = 0;
	self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
	self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	self.sock.bind(('', self.port))

    def set(self, **args):
	self.__dict__.update(args)
	data = "a %f %f %f %f" % (self.red, self.green, self.blue, self.white)
	if self.debug:
	    print data
	self.sock.sendto(data, (self.ip, self.port))

    def off(self):
        self.set(red=0.0, green=0.0, blue=0.0, white=0.0)

def lightShow(light):
    import math
    import time
    k = 0.05
    wr = math.sqrt(7.0)
    wg = math.sqrt(5.0)
    wb = math.sqrt(3.0)
    wk = math.sqrt(2.0)
    t = 0
    v = lambda w: k*(math.sin(t*w) + 1.0)
    while True:
#	light.set(red   = k*math.sin(t*wr),
#		  green = k*math.sin(t*wg),
#		  blue  = k*math.sin(t*wb),
#		  white = k*math.sin(t*wk))
	light.set(red   = v(wr),
		  green = v(wg),
		  blue  = v(wb),
		  white = v(wk))
	time.sleep(0.01)
	t += 0.01

def main():
    light = coloredLight()
#    light.red = 0.015
#    light.green = 0.02
#    light.blue = 0.03
#    light.white = 0.05
#    light.set()
    light.set(red=0.015, green=0.02, blue=0.03, white=0.05)

if __name__ == '__main__':
    main()
