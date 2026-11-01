# acm.py "<cmd>" [timeout s]: run a shell command on the board over the USB ACM console (root
# autologin) and print the output up to an __END__ marker. Do not end <cmd> with "&": wrap
# background jobs as "(setsid nohup CMD >/dev/null 2>&1 </dev/null &)".
import os, sys, termios, time, select
fd = os.open("/dev/cu.usbmodeme850_961", os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd); termios.cfmakeraw(a) if hasattr(termios, "cfmakeraw") else None
a[3] &= ~(termios.ECHO | termios.ICANON); termios.tcsetattr(fd, termios.TCSANOW, a)
cmd = sys.argv[1]; t = float(sys.argv[2]) if len(sys.argv) > 2 else 10
os.write(fd, b"\n" + f"{cmd}; echo __END__\n".encode())
out, end = b"", time.time() + t
while time.time() < end and b"__END__\n" not in out.split(b"echo __END__", 1)[-1]:
    if select.select([fd], [], [], 0.5)[0]: out += os.read(fd, 4096)
print(out.decode(errors="replace"))
