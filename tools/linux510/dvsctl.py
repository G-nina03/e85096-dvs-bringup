"""Display settings page for the S5KRC1S live view: ov5647-cap.py --live --raw8 --ctl PORT (11.28).

Runs in the capture process (libdvsview's settings live there), one daemon thread:
  GET  /           dvsctl.html
  GET  /params     settings with default and range, JSON (dvs_params())
  POST /params     {"name": int, ...}: apply (dvs_set) and save to SAVED for the next start
  GET  /stats      the renderer's last 2 s of stats + the CSIS CRC error count, JSON
  GET  /frame.png  the frame on HDMI now, 960x720 (before zoom)
  GET  /sensor     sensor settings: saved and in the CIS module now, with ranges (11.29)
  POST /sensor     {"margin": int, "flush_us": int}: save to SENSOR and restart (dvslive.service brings the
                   stream back in ~5 s with them; dvslive.sh writes them to the CIS module before stream on)
No login: it only changes how the view is drawn and the sensor's scan timing, on the board's own network.
"""
import ctypes
import http.server
import json
import os
import signal
import threading

SAVED = '/var/lib/dvsview/display.json'
SENSOR = '/var/lib/dvsview/sensor.json'
HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 960, 720
CIS = '/sys/module/is_cis_s5krc1s/parameters/'
CRC = '/sys/module/fimc_is/parameters/e850_csi_crc_count'
# name: (min, max, default) as in dvslive.sh: margin 17 = a 985 us scan (0 = the setfile's 2, 495 us), flush 50 us
SENSOR_RANGE = {'margin': (0, 140, 17), 'flush_us': (1, 1023, 50)}


def save(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.tmp', 'w') as f:
        json.dump(obj, f)
    os.replace(path + '.tmp', path)


def sensor():
    try:
        saved = json.load(open(SENSOR))
    except (OSError, ValueError):
        saved = {}
    out = {}
    for k, (lo, hi, d) in SENSOR_RANGE.items():
        try:
            now = int(open(CIS + k).read())
        except (OSError, ValueError):
            now = None
        out[k] = {'v': saved.get(k, d), 'now': now, 'min': lo, 'max': hi, 'def': d}
    return out


def start(lib, port):
    lib.dvs_set.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.dvs_params.restype = lib.dvs_stats.restype = ctypes.c_char_p
    lib.dvs_png.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    pngbuf = bytearray(W * H * 3 + H + 65536)   # level-1 deflate of the raw rows can come out a little larger
    lock = threading.Lock()                     # dvs_params() and dvs_png() are not reentrant
    try:
        for k, v in json.load(open(SAVED)).items():
            lib.dvs_set(k.encode(), int(v))
    except (OSError, ValueError, AttributeError):
        pass

    def params():
        with lock:
            return json.loads(lib.dvs_params())

    class Handler(http.server.BaseHTTPRequestHandler):
        def send(self, body, ctype='application/json', code=200):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split('?')[0]
            if path == '/':
                self.send(open(os.path.join(HERE, 'dvsctl.html'), 'rb').read(), 'text/html; charset=utf-8')
            elif path == '/params':
                self.send(json.dumps(params()).encode())
            elif path == '/stats':
                st = json.loads(lib.dvs_stats())
                try:
                    st['crc'] = int(open(CRC).read())
                except (OSError, ValueError):
                    pass
                self.send(json.dumps(st).encode())
            elif path == '/sensor':
                self.send(json.dumps(sensor()).encode())
            elif path == '/frame.png':
                with lock:
                    n = lib.dvs_png(ctypes.addressof(ctypes.c_char.from_buffer(pngbuf)), len(pngbuf))
                    body = bytes(pngbuf[:n]) if n > 0 else b''
                if not body:
                    return self.send(b'no frame yet', 'text/plain', 503)
                self.send(body, 'image/png')
            else:
                self.send(b'not found', 'text/plain', 404)

        def do_POST(self):
            path = self.path.split('?')[0]
            if path not in ('/params', '/sensor'):
                return self.send(b'not found', 'text/plain', 404)
            try:
                req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                if path == '/sensor':
                    cur = {k: p['v'] for k, p in sensor().items()}
                    for k, v in req.items():
                        lo, hi, _ = SENSOR_RANGE[k]
                        if not lo <= int(v) <= hi:
                            raise ValueError(k)
                        cur[k] = int(v)
                else:
                    rejected = [k for k, v in req.items() if lib.dvs_set(str(k).encode(), int(v)) < 0]
            except (ValueError, TypeError, AttributeError, KeyError):
                return self.send(b'{"error": "want {\\"name\\": integer in range, ...}"}', code=400)
            if path == '/sensor':
                save(SENSOR, cur)
                self.send(json.dumps({'restart': True, 'sensor': cur}).encode())
                # SIGTERM = the pkill path: stream off in ov5647-cap.py, then systemd starts dvslive.sh again
                threading.Timer(0.3, os.kill, (os.getpid(), signal.SIGTERM)).start()
                return
            cur = {k: p['v'] for k, p in params().items()}
            save(SAVED, cur)
            self.send(json.dumps({'rejected': rejected, 'params': cur}).encode())

        def log_message(self, *args):
            pass

    srv = http.server.ThreadingHTTPServer(('', port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv
