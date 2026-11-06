# webprobe.py URL OUT [period_s]: Wi-Fi outage probe (docs 11.31). GET URL on a new connection
# every period, one line per try: "<unix time> ok <ms>" or "<unix time> err <reason>". Ctrl-C/SIGTERM stops.
import sys, time, urllib.request, signal
url, out = sys.argv[1], sys.argv[2]
period = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
with open(out, "a", buffering=1) as f:
    while True:
        t = time.time()
        try:
            urllib.request.urlopen(url, timeout=1.0).read()
            f.write(f"{t:.3f} ok {1000 * (time.time() - t):.0f}\n")
        except Exception as e:
            f.write(f"{t:.3f} err {type(e).__name__}\n")
        time.sleep(max(0, period - (time.time() - t)))
