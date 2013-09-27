import socket
import struct
from collections import deque
#import fcntl, os
#import errno
#import random, string


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


class NeoString(object):
    def __init__(self, ip='192.168.32.139', port=2327, length=16):
	self.ip = ip
	self.port = port
	self.debug = False
	self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
	self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.neos = list(NeoPixel() for i in xrange(length))

    def __len__(self):
        return len(self.neos)

    def __getitem__(self, key):
        return self.neos.__getitem__(key)

    def __setitem__(self, key, value):
        return self.neos.__setitem__(key, value)

    def __iter__(self):
        return iter(self.neos)

    def __repr__(self):
        return object.__repr__(self) + "\n" \
            + "\n".join("(%3.3f, %3.3f, %3.3f)" % (n.red, n.green, n.blue) for n in self)

    def rotate(self, steps=1):
        n = int(steps) % 16
        self.neos = self.neos[n:] + self.neos[:n]

    def send(self):
        data = 'n' + ''.join(struct.pack('!BBB',
                                   int(neo.green * 255.5),
                                   int(neo.red   * 255.5),
                                   int(neo.blue  * 255.5)) for neo in self)
	if self.debug:
	    print data
	self.sock.sendto(data, (self.ip, self.port))

    def off(self):
        for neo in self:
            neo.off()

class NeoRing(NeoString):
    def __init__(self, ip='192.168.32.139', port=2327):
        NeoString.__init__(self, ip=ip, port=port, length=16)

class NeoPanel(NeoString):
    def __init__(self, ip='192.168.32.139', port=2327):
        NeoString.__init__(self, ip=ip, port=port, length=64)


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

def ringshow(ring, duration=10, speed=1.0, brightness=1.0):
    import time
    tag_green = tag_red = False
    ring.off()
    p = ring.neos[0]
    p.red = p.green = p.blue = brightness
    for i in xrange(16*duration):
        ring.send()
        time.sleep(1.0/speed)
        f = ring.neos[i%16]
        t = ring.neos[(i+1)%16]
        f.blue = 0
        t.blue = brightness
        if f.green and tag_green:
            f.green = 0
            t.green = brightness
            tag_green = False
            if f.red and tag_red:
                f.red = 0
                t.red = brightness
                tag_red = False
            else:
                tag_red = bool(t.red)
        else:
            tag_green = bool(t.green)
            
        
        


def main():
    light = ColoredLight()
    light.set(red=0.015, green=0.02, blue=0.03, white=0.05)

if __name__ == '__main__':
    main()
