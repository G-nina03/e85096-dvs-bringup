# fbhist.py: colour histogram of the visible fb0 (checks what the HDMI console really draws)
import numpy as np, os, fcntl, struct, mmap
fd = os.open('/dev/fb0', os.O_RDWR); v = bytearray(160); fcntl.ioctl(fd, 0x4600, v); yo = struct.unpack_from('I', v, 20)[0]
st = int(open('/sys/class/graphics/fb0/stride').read()); m = mmap.mmap(fd, st * 2160)
a = np.frombuffer(m, np.uint32).reshape(2160, st // 4)[yo:yo + 1080, :1920]
u, c = np.unique(a, return_counts=True); o = np.argsort(-c)[:8]
print('HIST', [(hex(u[i]), int(c[i])) for i in o])
