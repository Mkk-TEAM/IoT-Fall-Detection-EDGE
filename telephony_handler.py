#!/usr/bin/env python3
"""SIM 4G A7680C Pro VoLTE telephony handler — SMS and missed-call via AT commands.

Hardware wiring (Raspberry Pi 5):
  Pi physical pin 8  (TXD0 / GPIO 14) -> SIM module RX
  Pi physical pin 10 (RXD0 / GPIO 15) -> SIM module TX
  Pi physical pin 6  (GND)            -> SIM module GND

Env overrides:
  SIM_PORT     serial port path   (default: /dev/serial0)
  SIM_BAUDRATE baud rate          (default: 115200)
  SIM_TIMEOUT  read timeout (sec) (default: 5)
"""

import logging
import os
import subprocess
import sys
import time
from pathlib import Path

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

# ── dependency bootstrap ───────────────────────────────────────────────────────

def _ensure_pyserial() -> None:
    try:
        import serial  # noqa: F401
    except ImportError:
        logging.info("[telephony] pyserial not found — installing via pip...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial"],
                              stdout=subprocess.DEVNULL)
        logging.info("[telephony] pyserial installed successfully.")

_ensure_pyserial()
import serial  # noqa: E402  (import after optional install)

# ── config ────────────────────────────────────────────────────────────────────

PORT     = os.environ.get("SIM_PORT",     "/dev/serial0")
BAUDRATE = int(os.environ.get("SIM_BAUDRATE", "115200"))
TIMEOUT  = float(os.environ.get("SIM_TIMEOUT", "5"))

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("telephony")

# ── core class ────────────────────────────────────────────────────────────────

class TelephonyHandler:
    """Low-level AT-command driver for A7680C Pro over UART."""

    def __init__(self, port: str = PORT, baudrate: int = BAUDRATE,
                 timeout: float = TIMEOUT) -> None:
        self.port     = port
        self.baudrate = baudrate
        self.timeout  = timeout
        self._ser: serial.Serial | None = None

    # ── connection lifecycle ───────────────────────────────────────────────────

    def open(self) -> None:
        log.info("Opening serial port %s @ %d baud ...", self.port, self.baudrate)
        self._ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=self.timeout,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
        )
        log.info("Serial port opened: %s", self._ser.name)

    def close(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()
            log.info("Serial port closed.")

    def __enter__(self) -> "TelephonyHandler":
        self.open()
        return self

    def __exit__(self, *_) -> None:
        self.close()

    # ── low-level AT helpers ───────────────────────────────────────────────────

    def _send_raw(self, data: bytes) -> None:
        if not (self._ser and self._ser.is_open):
            raise RuntimeError("Serial port is not open.")
        try:
            self._ser.reset_input_buffer()
        except Exception:
            pass  # RP1 UART on Pi 5 may return EIO for tcflush — safe to ignore
        self._ser.write(data)
        self._ser.flush()

    def _read_response(self, wait: float | None = None) -> str:
        """Accumulate lines until OK/ERROR/> or timeout."""
        deadline = time.monotonic() + (wait or self.timeout)
        lines: list[str] = []
        while time.monotonic() < deadline:
            raw = self._ser.readline()
            if raw:
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    log.debug("  << %s", line)
                    lines.append(line)
                    if line in ("OK", "ERROR", ">") or line.startswith("+CME ERROR"):
                        break
        return "\n".join(lines)

    def send_at(self, cmd: str, wait: float | None = None) -> str:
        """Send a single AT command and return the full response string."""
        log.debug(">> %s", cmd)
        self._send_raw((cmd + "\r\n").encode())
        return self._read_response(wait)

    def _assert_ok(self, cmd: str, response: str) -> None:
        if "OK" not in response:
            raise RuntimeError(f"Command '{cmd}' failed. Response: {response!r}")

    # ── initialization / self-check ───────────────────────────────────────────

    def initialize(self) -> None:
        """Run basic AT health-checks: connectivity, SIM status, signal quality."""
        log.info("=== SIM module initialization ===")

        log.info("--- [1/3] AT connectivity check ---")
        resp = self.send_at("AT")
        self._assert_ok("AT", resp)
        log.info("Module is responsive.")

        log.info("--- [2/3] SIM card status (AT+CPIN?) ---")
        # Retry up to 3 times — module sometimes needs a moment after power-on.
        for attempt in range(3):
            resp = self.send_at("AT+CPIN?", wait=self.timeout + 2)
            if "+CPIN:" in resp:
                break
            log.debug("AT+CPIN? attempt %d got %r — retrying", attempt + 1, resp)
            time.sleep(1)
        if "+CPIN: READY" in resp:
            log.info("SIM card: READY")
        elif "+CPIN:" in resp:
            log.warning("SIM card status: %s", resp)
        else:
            raise RuntimeError(f"Could not read SIM status: {resp!r}")

        log.info("--- [3/3] Signal quality (AT+CSQ) ---")
        resp = self.send_at("AT+CSQ")
        self._assert_ok("AT+CSQ", resp)
        # +CSQ: <rssi>,<ber>   rssi 0-31, 99=unknown
        for part in resp.splitlines():
            if part.startswith("+CSQ:"):
                rssi_str = part.split(":")[1].strip().split(",")[0]
                rssi = int(rssi_str)
                if rssi == 99:
                    log.warning("Signal quality: UNKNOWN (99) — SIM may not be registered")
                else:
                    dbm = -113 + rssi * 2
                    log.info("Signal quality: rssi=%d (%d dBm)", rssi, dbm)
        log.info("=== Initialization complete ===")

    # ── SMS ───────────────────────────────────────────────────────────────────

    def send_sms(self, phone_number: str, message: str) -> None:
        """Send an SMS text message to phone_number."""
        log.info("Sending SMS to %s ...", phone_number)

        log.debug("Setting text mode (AT+CMGF=1)")
        resp = self.send_at("AT+CMGF=1")
        self._assert_ok("AT+CMGF=1", resp)

        cmd = f'AT+CMGS="{phone_number}"'
        log.debug(">> %s", cmd)
        self._send_raw((cmd + "\r\n").encode())

        # wait for '>' prompt
        deadline = time.monotonic() + self.timeout
        prompt_seen = False
        while time.monotonic() < deadline:
            raw = self._ser.read(self._ser.in_waiting or 1)
            if b">" in raw:
                prompt_seen = True
                log.debug("  << > (ready for message body)")
                break
            time.sleep(0.05)

        if not prompt_seen:
            raise RuntimeError("Did not receive '>' prompt for SMS body.")

        log.debug("Sending message body + Ctrl+Z")
        self._send_raw((message + "\x1a").encode("utf-8"))

        # wait for +CMGS / OK (up to 30 s — network round-trip)
        resp = self._read_response(wait=30.0)
        if "OK" in resp or "+CMGS:" in resp:
            log.info("SMS sent successfully. Response: %s", resp.replace("\n", " | "))
        else:
            raise RuntimeError(f"SMS send failed. Response: {resp!r}")

    # ── voice call / missed call ──────────────────────────────────────────────

    def make_missed_call(self, phone_number: str, duration_seconds: float = 5.0) -> None:
        """Dial phone_number, let it ring for duration_seconds, then hang up.

        The trailing semicolon in 'ATD<num>;' tells the modem to place a voice
        call (not a data call) and return control immediately with OK so we can
        hang up programmatically.
        """
        log.info("Initiating missed call to %s (ring for %.1fs) ...",
                 phone_number, duration_seconds)

        dial_cmd = f"ATD{phone_number};"
        resp = self.send_at(dial_cmd, wait=10.0)
        if "OK" not in resp and "NO CARRIER" not in resp:
            raise RuntimeError(f"Dial command failed. Response: {resp!r}")
        log.info("Call placed. Letting it ring for %.1f seconds ...", duration_seconds)

        time.sleep(duration_seconds)

        log.debug("Hanging up (ATH)")
        resp = self.send_at("ATH", wait=5.0)
        if "OK" in resp:
            log.info("Call ended. Missed-call delivered to %s.", phone_number)
        else:
            log.warning("ATH response unexpected: %s", resp)
