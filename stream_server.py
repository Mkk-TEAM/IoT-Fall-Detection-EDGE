#!/usr/bin/env python3
"""MJPEG HTTP stream server — wraps rpicam-vid, no extra packages needed."""

import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HOST    = os.environ.get("EDGE_STREAM_HOST", "0.0.0.0")
PORT    = int(os.environ.get("EDGE_STREAM_PORT", 8081))
W       = int(os.environ.get("CAMERA_WIDTH",  640))
H       = int(os.environ.get("CAMERA_HEIGHT", 480))
FPS     = int(os.environ.get("CAMERA_FPS",    15))
QUALITY = int(os.environ.get("JPEG_QUALITY",  80))

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


class FrameBuffer:
    def __init__(self):
        self._frame = None
        self._cond  = threading.Condition()

    def put(self, frame: bytes) -> None:
        with self._cond:
            self._frame = frame
            self._cond.notify_all()

    def get(self, timeout: float = 5.0) -> bytes | None:
        """Block until a NEW frame arrives (or timeout)."""
        with self._cond:
            self._cond.wait(timeout)
            return self._frame

    @property
    def ready(self) -> bool:
        return self._frame is not None


_buf = FrameBuffer()
_camera_ok = False


def capture_loop() -> None:
    global _camera_ok
    cmd = [
        "rpicam-vid",
        "-t", "0",
        "--width",     str(W),
        "--height",    str(H),
        "--framerate", str(FPS),
        "--quality",   str(QUALITY),
        "--codec",     "mjpeg",
        "--nopreview",
        "-o", "-",
    ]
    print(f"[INFO] Starting: {' '.join(cmd)}", flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    _camera_ok = True
    data = b""
    while True:
        chunk = proc.stdout.read(65536)
        if not chunk:
            _camera_ok = False
            print("[ERROR] rpicam-vid exited unexpectedly.", flush=True)
            break
        data += chunk
        # Parse individual JPEG frames from the concatenated MJPEG stream
        while True:
            s = data.find(SOI)
            if s == -1:
                data = b""
                break
            e = data.find(EOI, s + 2)
            if e == -1:
                data = data[s:]   # keep partial frame for next read
                break
            _buf.put(data[s : e + 2])
            data = data[e + 2 :]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # suppress per-request logs
        pass

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/health":
            self._json(200, {
                "status": "ok",
                "cameraOpened": _camera_ok and _buf.ready,
                "width": W, "height": H, "fps": FPS,
            })

        elif path == "/snapshot.jpg":
            frame = _buf.get(timeout=5)
            if frame:
                self._raw(200, "image/jpeg", frame)
            else:
                self.send_error(503, "No frame available yet")

        elif path == "/stream.mjpg":
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frame")
            self._cors()
            self.end_headers()
            try:
                while True:
                    frame = _buf.get(timeout=5)
                    if frame is None:
                        continue
                    self.wfile.write(
                        b"--frame\r\n"
                        b"Content-Type: image/jpeg\r\n"
                        b"Content-Length: " + str(len(frame)).encode() +
                        b"\r\n\r\n" + frame + b"\r\n"
                    )
            except Exception:
                pass  # client disconnected — normal

        else:
            self.send_error(404)

    def _json(self, code: int, obj: dict) -> None:
        self._raw(code, "application/json", json.dumps(obj).encode())

    def _raw(self, code: int, ctype: str, body: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", len(body))
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")


try:
    from http.server import ThreadingHTTPServer as _BaseServer  # Python ≥ 3.7
except ImportError:
    _BaseServer = HTTPServer


def main() -> None:
    threading.Thread(target=capture_loop, daemon=True).start()

    server = _BaseServer((HOST, PORT), Handler)
    print(f"[INFO] Edge stream server  http://{HOST}:{PORT}", flush=True)
    print(f"[INFO] Endpoints: /health  /snapshot.jpg  /stream.mjpg", flush=True)
    print(f"[INFO] Camera:    {W}x{H} @ {FPS}fps  quality={QUALITY}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[INFO] Stopped.")


if __name__ == "__main__":
    main()
