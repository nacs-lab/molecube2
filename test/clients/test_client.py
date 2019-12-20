#!/usr/bin/python

import zmq

import sys
import struct

assert len(sys.argv) >= 3

ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.connect(sys.argv[1])

cmd = sys.argv[2]

def parse_ddschn(chn):
    if chn.startswith('freq'):
        typ = 0
        chn = chn[4:]
    elif chn.startswith('amp'):
        typ = 1
        chn = chn[3:]
    elif chn.startswith('phase'):
        typ = 2
        chn = chn[5:]
    else:
        raise ValueError("Invalid DDS channel name: " + chn)
    chn = int(chn)
    return typ << 6 | chn

def print_ddschn(chn):
    typ = chn >> 6
    chn = chn & ((1 << 6) - 1)
    if typ == 0:
        name = 'freq'
    elif typ == 1:
        name = 'amp'
    elif typ == 2:
        name = 'phase'
    else:
        raise ValueError("Invalid DDS channel type: %d" % typ);
    return "%s(%d)" % (name, chn)

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
elif cmd == 'set_clock':
    sock.send_string("set_clock", zmq.SNDMORE)
    sock.send(struct.pack('B', int(sys.argv[3])))
    print(sock.recv())
elif cmd == 'get_clock':
    sock.send_string("get_clock")
    print("clock: {0}".format(struct.unpack('B', sock.recv())[0]))
elif cmd == 'override_dds':
    sock.send_string("override_dds", zmq.SNDMORE)
    msg = b''
    for i in range(3, len(sys.argv), 2):
        chn = parse_ddschn(sys.argv[i]).to_bytes(1, byteorder=sys.byteorder, signed=False)
        val = int(sys.argv[i + 1], 16).to_bytes(4, byteorder=sys.byteorder, signed=False)
        msg += chn + val
    sock.send(msg)
    msg = sock.recv()
    print(msg)
elif cmd == 'get_override_dds':
    sock.send_string("get_override_dds")
    msg = sock.recv()
    assert len(msg) % 5 == 0
    print("%d DDS overrides" % (len(msg) / 5))
    for i in range(0, len(msg), 5):
        print("  {0} = {1:#0{2}x}".format(print_ddschn(msg[i]),
                                          struct.unpack('I', msg[i + 1:i + 5])[0], 10))
elif cmd == 'set_dds':
    sock.send_string("set_dds", zmq.SNDMORE)
    msg = b''
    for i in range(3, len(sys.argv), 2):
        chn = parse_ddschn(sys.argv[i]).to_bytes(1, byteorder=sys.byteorder, signed=False)
        val = int(sys.argv[i + 1], 16).to_bytes(4, byteorder=sys.byteorder, signed=False)
        msg += chn + val
    sock.send(msg)
    msg = sock.recv()
    print(msg)
elif cmd == 'get_dds':
    if len(sys.argv) == 3:
        sock.send_string("get_dds")
    else:
        sock.send_string("get_dds", zmq.SNDMORE)
        msg = b''
        for i in range(3, len(sys.argv)):
            msg += parse_ddschn(sys.argv[i]).to_bytes(1, byteorder=sys.byteorder, signed=False)
        sock.send(msg)
    msg = sock.recv()
    if len(sys.argv) == 3:
        assert len(msg) % 15 == 0
        print("%d x 3 channels" % (len(msg) / 15))
    else:
        print("%d channels" % (len(msg) / 5))
    for i in range(0, len(msg), 5):
        print("  {0} = {1:#0{2}x}".format(print_ddschn(msg[i]),
                                          struct.unpack('I', msg[i + 1:i + 5])[0], 10))
elif cmd == 'reset_dds':
    sock.send_string("reset_dds", zmq.SNDMORE)
    sock.send(int(sys.argv[3]).to_bytes(1, byteorder=sys.byteorder, signed=False))
    msg = sock.recv()
    print(msg)
