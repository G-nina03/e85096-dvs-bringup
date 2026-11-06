"""Display settings page for the S5KRC1S live view: ov5647-cap.py --live --raw8 --ctl PORT (11.28).

Runs in the capture process (libdvsview's settings live there), one daemon thread:
  GET  /           dvsctl.html
  GET  /params     settings with default and range, JSON (dvs_params())
  POST /params     {"name": int, ...}: apply (dvs_set) and save to SAVED for the next start
  GET  /stats      the renderer's last 2 s of stats, JSON
  GET  /frame.png  the frame on HDMI now, 960x720 (before zoom)
No login: it only changes how the view is drawn, on the board's own network.
"""
import ctypes
import http.server
import json
import os
import struct
import threading
import zlib

import numpy as np

SAVED = '/var/lib/dvsview/display.json'
HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 960, 720


def png(rgb):
    h, w, _ = rgb.shape
    raw = np.hstack([np.zeros((h, 1), np.uint8), rgb.reshape(h, w * 3)]).tobytes()   # filter type 0 per row

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))


def start(lib, port):
    lib.dvs_set.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.dvs_params.restype = lib.dvs_stats.restype = ctypes.c_char_p
    lib.dvs_snapshot.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    snap = bytearray(W * H * 4)
    lock = threading.Lock()                     # dvs_params() and snap are not reentrant
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
                self.send(lib.dvs_stats())
            elif path == '/frame.png':
                with lock:
                    n = lib.dvs_snapshot(ctypes.addressof(ctypes.c_char.from_buffer(snap)), len(snap))
                    rgb = np.frombuffer(snap, np.uint8).reshape(H, W, 4)[:, :, 2::-1].copy()   # XRGB LE -> RGB
                if n <= 0:
                    return self.send(b'no frame yet', 'text/plain', 503)
                self.send(png(rgb), 'image/png')
            else:
                self.send(b'not found', 'text/plain', 404)

        def do_POST(self):
            if self.path != '/params':
                return self.send(b'not found', 'text/plain', 404)
            try:
                req = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                rejected = [k for k, v in req.items() if lib.dvs_set(str(k).encode(), int(v)) < 0]
            except (ValueError, TypeError, AttributeError):
                return self.send(b'{"error": "want {\\"name\\": integer, ...}"}', code=400)
            cur = {k: p['v'] for k, p in params().items()}
            os.makedirs(os.path.dirname(SAVED), exist_ok=True)
            with open(SAVED + '.tmp', 'w') as f:
                json.dump(cur, f)
            os.replace(SAVED + '.tmp', SAVED)
            self.send(json.dumps({'rejected': rejected, 'params': cur}).encode())

        def log_message(self, *args):
            pass

    srv = http.server.ThreadingHTTPServer(('', port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv
