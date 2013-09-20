import socket
import struct
#import fcntl, os
#import errno
#import random, string
#from time import time, sleep

class ColoredLight(object):
    def __init__(self, ip='192.168.32.139', port=2327):
	self.ip = ip
	self.port = port
	self.debug = False
	self.red = self.green = self.blue = self.white = 0;
	self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
	self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    def set(self, **args):
	self.__dict__.update(args)
	data = "a %f %f %f %f" % (self.red, self.green, self.blue, self.white)
	if self.debug:
	    print data
	self.sock.sendto(data, (self.ip, self.port))

    def off(self):
        self.set(red=0.0, green=0.0, blue=0.0, white=0.0)

class NeoPixel(object):
    def __init__(self, **args):
        self.off()
        self.__dict__.update(args)

    def __repr__(self):
        return object.__repr__(self) + ": (%3.3f, %3.3f, %3.3f)" % (self.red, self.green, self.blue)

    def off(self):
        self.red = self.green = self.blue = 0.0;


class NeoRing(object):
    def __init__(self, ip='192.168.32.139', port=2327):
	self.ip = ip
	self.port = port
	self.debug = False
	self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
	self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.neos = [ NeoPixel() for i in xrange(16) ]

    def rotate(self, steps=1):
        n = int(steps) % 16
        self.neos = self.neos[n:] + self.neos[:n]

    def send(self):
        data = 'n' + ''.join(struct.pack('!BBB',
                                   int(neo.green * 255.5),
                                   int(neo.red   * 255.5),
                                   int(neo.blue  * 255.5)) for neo in self.neos)
	if self.debug:
	    print data
	self.sock.sendto(data, (self.ip, self.port))

    def off(self):
        for neo in self.neos:
            neo.off()

################################################################
# demonstration functions

def lightshow(light):
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
	light.set(red   = v(wr),
		  green = v(wg),
		  blue  = v(wb),
		  white = v(wk))
	time.sleep(0.01)
	t += 0.01

def flashlight(light, f=5, duration=10):
    import time
    t0 = now = time.time()
    t_finish = t0 + duration
    t = lambda p,r: 0.5 * (((now % p) / p) > r)
    while now < t_finish:
        ft = f * now
        light.set(
            red=t(1, 0.5),
            green=t(0.5, 0.5),
            blue=t(0.25, 0.5),
            white=0 )
        time.sleep(0.01)
        now = time.time()
    light.off()

def copcar(light, duration=10):
    import time
    light.off()
    t0 = now = time.time()
    t_finish = t0 + duration
    while time.time() < t_finish:
        for i in range(5):
            light.set(red=1)
            time.sleep(0.005)
            light.off()
            time.sleep(0.1)
        for i in range(5):
            light.set(blue=1)
            time.sleep(0.005)
            light.off()
            time.sleep(0.1)

def randlight(light, f=5, duration=10):
    import random
    import time
    t0 = now = time.time()
    t_finish = t0 + duration
    p = 1.0 / f
    d = {}
    while now < t_finish:
        d[random.choice(['red', 'green', 'blue'])] = random.choice([0, 0.5])
        light.set(**d)
        time.sleep(p)
        now = time.time()
    light.off()

def ringshow(ring, duration=10):
    ring.off()
    red_pos = green_pos = blue_pos = 0
    bump_red = bump_green = False
    for i in xrange(16 * duration):
        r.send()
        time.sleep(1.0/16)
        b = ring.neos[blue_pos]
        b.blue = 0
        blue_pos = (blue_pos + 1) % 16
        ring.neos[blue_pos].blue = 1
        if b.green:
            pass                # ***
        
        


def main():
    light = ColoredLight()
    light.set(red=0.015, green=0.02, blue=0.03, white=0.05)

if __name__ == '__main__':
    main()
