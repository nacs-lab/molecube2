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
