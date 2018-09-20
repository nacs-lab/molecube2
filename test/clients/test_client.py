#!/usr/bin/python

import zmq

import sys
import struct

assert len(sys.argv) >= 3

ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect(sys.argv[1])

cmd = sys.argv[2]

if cmd == 'set_startup':
    with open(sys.argv[3]) as fh:
        cmdlist = fh.read().encode()
    sock.send_string("set_startup", zmq.SNDMORE)
    sock.send(cmdlist + b'\0')

    msg = sock.recv()
    print(msg)
elif cmd == 'get_startup':
    sock.send_string("get_startup")
    msg = sock.recv()
    print(msg.decode())
elif cmd == 'set_ttl_names':
    sock.send_string("set_ttl_names", zmq.SNDMORE)
    msg = b''
    for i in range(3, len(sys.argv), 2):
        msg += int(sys.argv[i]).to_bytes(1, byteorder=sys.byteorder,
                                         signed=False) + sys.argv[i + 1].encode() + b'\0'
    print(msg)
    sock.send(msg)
    msg = sock.recv()
    print(msg)
elif cmd == 'get_ttl_names':
    sock.send_string("get_ttl_names")
    msg = sock.recv()
    print(msg)
elif cmd == 'set_dds_names':
    sock.send_string("set_dds_names", zmq.SNDMORE)
    msg = b''
    for i in range(3, len(sys.argv), 2):
        msg += int(sys.argv[i]).to_bytes(1, byteorder=sys.byteorder,
                                         signed=False) + sys.argv[i + 1].encode() + b'\0'
    print(msg)
    sock.send(msg)
    msg = sock.recv()
    print(msg)
elif cmd == 'get_dds_names':
    sock.send_string("get_dds_names")
    msg = sock.recv()
    print(msg)
elif cmd == 'override_ttl':
    sock.send_string("override_ttl", zmq.SNDMORE)
    lo = int(sys.argv[3], 16)
    hi = int(sys.argv[4], 16)
    normal = int(sys.argv[5], 16)
    sock.send(struct.pack('III', lo, hi, normal))
    lo, hi = struct.unpack('II', sock.recv())
    print("lo: {0:#0{1}x}; hi: {2:#0{1}x}".format(lo, 10, hi))
elif cmd == 'set_ttl':
    sock.send_string("set_ttl", zmq.SNDMORE)
    lo = int(sys.argv[3], 16)
    hi = int(sys.argv[4], 16)
    sock.send(struct.pack('II', lo, hi))
    print("ttl: {0:#0{1}x}".format(struct.unpack('I', sock.recv())[0], 10))
