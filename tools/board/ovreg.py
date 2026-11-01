# ovreg.py BUS reg[=val] ...  : OV5647 (0x36) 16-bit-address register read/write via I2C_RDWR
import sys, os, fcntl, ctypes
class Msg(ctypes.Structure):
    _fields_ = [('addr', ctypes.c_uint16), ('flags', ctypes.c_uint16), ('len', ctypes.c_uint16), ('buf', ctypes.c_void_p)]
class Data(ctypes.Structure):
    _fields_ = [('msgs', ctypes.c_void_p), ('n', ctypes.c_uint32)]
fd = os.open('/dev/i2c-%s' % sys.argv[1], os.O_RDWR)
def xfer(*msgs):
    arr = (Msg * len(msgs))(*msgs)
    fcntl.ioctl(fd, 0x0707, Data(ctypes.addressof(arr), len(msgs)))
for a in sys.argv[2:]:
    r, _, v = a.partition('=')
    r = int(r, 16)
    if v:
        b = ctypes.create_string_buffer(bytes([r >> 8, r & 0xff, int(v, 16)]), 3)
        xfer(Msg(0x36, 0, 3, ctypes.addressof(b)))
        print('%04x <- %s' % (r, v))
    else:
        w = ctypes.create_string_buffer(bytes([r >> 8, r & 0xff]), 2); rd = ctypes.create_string_buffer(1)
        xfer(Msg(0x36, 0, 2, ctypes.addressof(w)), Msg(0x36, 1, 1, ctypes.addressof(rd)))
        print('%04x = %02x' % (r, rd.raw[0]))
