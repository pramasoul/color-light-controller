import socket
import fcntl, os
import errno
import random, string
from time import time, sleep
import struct

ECHO_SERVER_ADDRESS = "192.168.32.139"
NUMBER_OF_SECONDS = 20
LOCAL_SERVER_PORT = 2327
MEGA = 1024*1024.
LEN_PACKET = 1024
#data = struct.pack('!BIIII', 1, 1, 2, 3, 4)
data = "a 0.01 0.02 0.03 0.04"


s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM,socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', LOCAL_SERVER_PORT))

print "Started sending data"
print "Time : %d " % time()

while True:
	start = time()
	s.sendto(data, (ECHO_SERVER_ADDRESS, LOCAL_SERVER_PORT))
	sleep(0.2);
