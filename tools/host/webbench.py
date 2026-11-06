#!/usr/bin/env python3
"""webbench.py URL [N] [FPS] [keep]: N sequential GETs (paced to FPS if > 0, else back to back), latency stats.
keep: one HTTP/1.1 connection for all (as a browser does when the server allows it), else a new one each time."""
import http.client
import statistics
import sys
import time
import urllib.parse
import urllib.request

url = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 0
keep = len(sys.argv) > 4 and sys.argv[4] == 'keep'
u = urllib.parse.urlsplit(url)
conn = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=10) if keep else None
ts, size, closed = [], 0, 0
t0 = time.perf_counter()
for i in range(n):
    a = time.perf_counter()
    if keep:
        conn.request('GET', u.path)
        r = conn.getresponse()
        body = r.read()
        if r.will_close:                 # the server ended the connection: count it, reconnect next time
            closed += 1
            conn.close()
    else:
        body = urllib.request.urlopen(url, timeout=10).read()
    b = time.perf_counter()
    ts.append((b - a) * 1e3)
    size += len(body)
    if fps:
        time.sleep(max(0, t0 + (i + 1) / fps - b))
el = time.perf_counter() - t0
s = sorted(ts)
print('n %d%s, %.1f req/s, latency median %.1f p90 %.1f max %.1f ms, %.0f kB each%s'
      % (n, ' keep-alive' if keep else '', n / el, statistics.median(s), s[int(n * 0.9)], s[-1], size / n / 1024,
         ', server closed %d' % closed if closed else ''))
