# uartlog.py PORT LOGFILE: append the raw 115200 baud debug UART (kernel log) to LOGFILE.
#   nohup python3 tools/host/uartlog.py /dev/cu.usbserial-AK0O198I ~/e850-logs/uart.log &
import os, sys, termios
fd = os.open(sys.argv[1], os.O_RDONLY | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = a[5] = termios.B115200; a[6][termios.VMIN] = 1; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
with open(sys.argv[2], "ab", buffering=0) as f:
    while True: f.write(os.read(fd, 4096))
