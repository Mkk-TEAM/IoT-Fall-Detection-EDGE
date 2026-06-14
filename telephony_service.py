#!/usr/bin/env python3
"""HTTP microservice wrapping TelephonyHandler for SMS/missed-call alerts.

Listens on SMS_SERVICE_PORT (default 8090).
Protected by X-Edge-Secret header.

Endpoints:
  POST /sms          {"to": "0909...", "message": "..."}
  POST /call         {"to": "0909...", "duration": 5}   (optional)
  GET  /health       {"status":"ok","sim_ready":true/false}
"""

import json
import logging
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

# ── .env loader ────────────────────────────────────────────────────────────────

def _load_dotenv(path: str = ".env") -> None:
    p = Path(path)
    if not p.exists():
        return
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip(); val = val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val

_load_dotenv()

# ── config ─────────────────────────────────────────────────────────────────────

EDGE_SECRET  = os.environ.get("EDGE_SECRET", "")
SERVICE_PORT = int(os.environ.get("SMS_SERVICE_PORT", "8090"))
SIM_PORT     = os.environ.get("SIM_PORT",     "/dev/ttyAMA0")
SIM_BAUDRATE = int(os.environ.get("SIM_BAUDRATE", "115200"))
SIM_TIMEOUT  = float(os.environ.get("SIM_TIMEOUT", "5"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("telephony_service")

# ── SIM driver (imported from telephony_handler.py) ───────────────────────────

from telephony_handler import TelephonyHandler

# Open a fresh connection for every operation — the RP1 UART on Pi 5 leaves
# the port in an inconsistent state if it's held open between AT sequences.
_sim_lock  = threading.Lock()
_sim_ready = False  # updated after first successful health-check at startup

def _with_sim(fn):
    """Open, run fn(handler), close — ensures clean serial state each time."""
    h = TelephonyHandler(port=SIM_PORT, baudrate=SIM_BAUDRATE, timeout=SIM_TIMEOUT)
    h.open()
    try:
        return fn(h)
    finally:
        h.close()

def _try_init_sim() -> None:
    """Background thread: probe SIM at startup, retry every 30s on failure."""
    global _sim_ready
    while True:
        try:
            with _sim_lock:
                _with_sim(lambda h: h.initialize())
            _sim_ready = True
            log.info("SIM module ready on %s", SIM_PORT)
            break
        except Exception as e:
            log.warning("SIM probe failed (%s) — retrying in 30s", e)
            time.sleep(30)

threading.Thread(target=_try_init_sim, daemon=True).start()

# ── HTTP handler ───────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.info(fmt, *args)

    def _auth(self) -> bool:
        if not EDGE_SECRET:
            return True
        return self.headers.get("X-Edge-Secret") == EDGE_SECRET

    def _json_response(self, code: int, body: dict) -> None:
        payload = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _read_body(self) -> dict:
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        return json.loads(raw)

    def do_GET(self):
        if self.path == "/health":
            self._json_response(200, {
                "status": "ok",
                "sim_ready": _sim_ready,
                "sim_port": SIM_PORT,
            })
        else:
            self._json_response(404, {"error": "not found"})

    def do_POST(self):
        if not self._auth():
            self._json_response(401, {"error": "unauthorized"})
            return

        body = self._read_body()

        if self.path == "/sms":
            to  = body.get("to", "").strip()
            msg = body.get("message", "").strip()
            if not to or not msg:
                self._json_response(400, {"error": "to and message are required"})
                return
            try:
                with _sim_lock:
                    _with_sim(lambda h: h.send_sms(to, msg))
                log.info("SMS sent to %s", to)
                self._json_response(200, {"status": "sent", "to": to})
            except Exception as e:
                log.error("SMS failed to %s: %s", to, e)
                self._json_response(500, {"error": str(e)})

        elif self.path == "/call":
            to       = body.get("to", "").strip()
            duration = float(body.get("duration", 5))
            if not to:
                self._json_response(400, {"error": "to is required"})
                return
            def _call():
                try:
                    with _sim_lock:
                        _with_sim(lambda h: h.make_missed_call(to, duration))
                    log.info("Missed-call delivered to %s", to)
                except Exception as e:
                    log.error("Call failed to %s: %s", to, e)
            threading.Thread(target=_call, daemon=True).start()
            self._json_response(202, {"status": "calling", "to": to})

        else:
            self._json_response(404, {"error": "not found"})

# ── main ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", SERVICE_PORT), Handler)
    log.info("Telephony service listening on port %d", SERVICE_PORT)
    log.info("SIM port: %s @ %d baud", SIM_PORT, SIM_BAUDRATE)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutting down.")
        server.shutdown()
        if _sim_handler:
            _sim_handler.close()
