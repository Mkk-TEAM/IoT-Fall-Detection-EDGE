#!/usr/bin/env python3
"""Live IMU monitor — connects BLE directly, shows raw data + fall detection state.

Usage:
    python3 imu_monitor.py [--no-service]  # --no-service: don't stop/restart edge-imu-ble

Requires: pip install bleak --break-system-packages
"""

import argparse
import asyncio
import json
import math
import os
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# ── deps ──────────────────────────────────────────────────────────────────────

def _ensure(pkg, import_name=None):
    try:
        __import__(import_name or pkg)
    except ImportError:
        print(f"[setup] installing {pkg}...")
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", pkg, "--break-system-packages"],
            stdout=subprocess.DEVNULL,
        )

_ensure("bleak")
from bleak import BleakClient, BleakScanner

# ── config from .env ──────────────────────────────────────────────────────────

def _load_dotenv(path=".env"):
    p = Path(path)
    if p.exists():
        for line in p.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            k = k.strip(); v = v.strip().strip('"').strip("'")
            if k and k not in os.environ:
                os.environ[k] = v

_load_dotenv()

DEVICE_NAME  = os.environ.get("BLE_DEVICE_NAME",      "FallDetect-IMU")
CHAR_UUID    = os.environ.get("BLE_NOTIFY_CHAR_UUID", "12345678-1234-1234-1234-123456789abc")
SERVICE_NAME = "edge-imu-ble"

# ── load fall config ──────────────────────────────────────────────────────────

def load_fall_config(path="fall_detector_config.json"):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return {}

CFG = load_fall_config(os.environ.get("FALL_DETECTOR_CONFIG", "fall_detector_config.json"))

IMPACT_G         = CFG.get("impact_g",               2.5)
FREEFALL_G       = CFG.get("freefall_g",             0.5)
FREEFALL_MIN_MS  = CFG.get("freefall_min_ms",        120)
FF_IMPACT_WIN_MS = CFG.get("freefall_impact_window_ms", 1000)
POSTURE_DELAY_MS = CFG.get("posture_eval_delay_ms",  500)
UPRIGHT_PITCH    = CFG.get("upright_pitch_deg",      -90.0)
TILT_CONFIRM_DEG = CFG.get("tilt_confirm_deg",       40.0)
POST_WIN_MS      = CFG.get("post_impact_window_ms",  10000)
COOLDOWN_MS      = CFG.get("cooldown_ms",            10000)
ACCEL_IS_MPS2    = CFG.get("accel_unit_mps2",        True)
G                = 9.80665

# ── ANSI colors ───────────────────────────────────────────────────────────────

RED  = "\033[91m"
YEL  = "\033[93m"
GRN  = "\033[92m"
CYN  = "\033[96m"
BLD  = "\033[1m"
DIM  = "\033[2m"
RST  = "\033[0m"
CLR  = "\033[2J\033[H"

STATE_COLOR = {
    "NORMAL":          GRN,
    "FREEFALL":        YEL,
    "IMPACT_DETECTED": RED,
    "POST_IMPACT":     YEL,
    "FALL_CONFIRMED":  f"{BLD}{RED}",
    "FALL_REJECTED":   DIM,
    "COOLDOWN":        CYN,
}

# ── packet parser ─────────────────────────────────────────────────────────────

PACKET_LEN = 61
MAGIC      = (0xAA, 0x55)

def parse_packet(data: bytes):
    if len(data) != PACKET_LEN:
        return None
    if data[0] != MAGIC[0] or data[1] != MAGIC[1]:
        return None
    csum = 0
    for b in data[:60]:
        csum ^= b
    if csum != data[60]:
        return None

    seq             = struct.unpack_from("<H",    data, 2)[0]
    ax, ay, az      = struct.unpack_from("<fff",  data, 4)
    gx, gy, gz      = struct.unpack_from("<fff",  data, 16)
    roll, pitch, yaw = struct.unpack_from("<fff", data, 28)
    q0, q1, q2, q3  = struct.unpack_from("<ffff", data, 40)
    esp_ms          = struct.unpack_from("<I",    data, 56)[0]

    amag = math.sqrt(ax*ax + ay*ay + az*az)
    if ACCEL_IS_MPS2:
        amag /= G
    gmag = math.sqrt(gx*gx + gy*gy + gz*gz)

    return {
        "seq": seq, "esp_ms": esp_ms,
        "ax": ax, "ay": ay, "az": az,
        "gx": gx, "gy": gy, "gz": gz,
        "roll": roll, "pitch": pitch, "yaw": yaw,
        "q0": q0, "q1": q1, "q2": q2, "q3": q3,
        "accel_mag_g": amag,
        "gyro_mag":    gmag,
    }

# ── fall detection (mirrors FallDetector.cpp — new posture-based logic) ───────

def tilt_from_upright(pitch):
    d = abs(pitch - UPRIGHT_PITCH)
    if d > 180.0:
        d = 360.0 - d
    return d

class FallDetectorPy:
    def __init__(self):
        self.state       = "NORMAL"
        self.ff_start    = None
        self.ff_exit     = None
        self.impact_t    = None
        self.post_start  = None
        self.post_buf    = []       # (t_ms, sample) after impact
        self.peak_g      = 0.0
        self.min_g       = 99.0
        self.peak_gyro   = 0.0
        self.had_ff      = False
        self.cooldown_end = None
        self.events      = []
        self._history    = []

    def _now_ms(self):
        return int(time.monotonic() * 1000)

    def push(self, s):
        t = self._now_ms()
        self._history.append((t, s))
        if len(self._history) > 2000:
            self._history = self._history[-1000:]

        accel = s["accel_mag_g"]

        if self.state == "NORMAL":
            self._on_normal(t, s, accel)
        elif self.state == "FREEFALL":
            self._on_freefall(t, s, accel)
        elif self.state == "POST_IMPACT":
            self._on_post(t, s, accel)
            return self._try_eval(t, s)
        elif self.state == "COOLDOWN":
            if t >= self.cooldown_end:
                self._transition("NORMAL")
        return None

    def _on_normal(self, t, s, accel):
        if accel >= IMPACT_G:
            self.ff_start = None
            self._start_candidate(t, s, False)
            return
        if accel <= FREEFALL_G:
            if self.ff_start is None:
                self.ff_start = t
            if (t - self.ff_start) >= FREEFALL_MIN_MS:
                self._transition("FREEFALL")
        else:
            self.ff_start = None

    def _on_freefall(self, t, s, accel):
        if accel <= FREEFALL_G:
            return
        if self.ff_exit is None:
            self.ff_exit = t
        if accel >= IMPACT_G:
            self.ff_exit = None
            self._start_candidate(t, s, True)
            return
        if (t - self.ff_exit) > FF_IMPACT_WIN_MS:
            self.ff_exit = None
            self._transition("NORMAL")

    def _start_candidate(self, t, s, had_ff):
        self.had_ff     = had_ff
        self.impact_t   = t
        self.peak_g     = s["accel_mag_g"]
        self.min_g      = s["accel_mag_g"]
        self.peak_gyro  = s["gyro_mag"]
        self.post_start = t
        self.post_buf   = [(t, s)]
        self._transition("IMPACT_DETECTED")
        self._transition("POST_IMPACT")

    def _on_post(self, t, s, accel):
        self.post_buf.append((t, s))
        if accel > self.peak_g:        self.peak_g  = accel
        if accel < self.min_g:         self.min_g   = accel
        if s["gyro_mag"] > self.peak_gyro: self.peak_gyro = s["gyro_mag"]

    def _try_eval(self, t, s):
        elapsed = t - self.post_start
        if elapsed < POSTURE_DELAY_MS:
            return None   # still in bounce-settling window

        tilt = tilt_from_upright(s["pitch"])

        if tilt > TILT_CONFIRM_DEG:
            # Person is no longer upright → confirmed immediately
            return self._finalize(t, tilt, True)

        if elapsed >= POST_WIN_MS:
            # Timeout: person stayed upright → reject
            return self._finalize(t, tilt, False)

        return None

    def _finalize(self, t, tilt, confirmed):
        confidence = 0.30
        if self.had_ff: confidence += 0.20
        confidence += 0.50 * min(1.0, tilt / (TILT_CONFIRM_DEG * 2.0))

        ev = {
            "type":       "FALL_CONFIRMED" if confirmed else "FALL_REJECTED",
            "tilt":       tilt,
            "peak_g":     self.peak_g,
            "confidence": confidence,
            "had_ff":     self.had_ff,
            "confirmed":  confirmed,
        }
        self.events.append(ev)

        self._transition("FALL_CONFIRMED" if confirmed else "FALL_REJECTED")
        self.cooldown_end = t + COOLDOWN_MS
        self._transition("COOLDOWN")
        return ev

    def _transition(self, new_state):
        self.state = new_state

    def elapsed_post_ms(self):
        if self.state != "POST_IMPACT" or self.post_start is None:
            return 0
        return self._now_ms() - self.post_start

    def remaining_cooldown_ms(self):
        if self.state != "COOLDOWN" or self.cooldown_end is None:
            return 0
        return max(0, self.cooldown_end - self._now_ms())

# ── service management ────────────────────────────────────────────────────────

def service_stop():
    r = subprocess.run(["sudo", "systemctl", "stop", SERVICE_NAME], capture_output=True)
    return r.returncode == 0

def service_start():
    subprocess.run(["sudo", "systemctl", "start", SERVICE_NAME], capture_output=True)

# ── terminal UI ───────────────────────────────────────────────────────────────

def bar(value, vmin, vmax, width=20, fill="█", empty="░"):
    frac = max(0.0, min(1.0, (value - vmin) / (vmax - vmin)))
    return fill * int(frac * width) + empty * (width - int(frac * width))

def accel_bar(g):
    color = RED if g >= IMPACT_G else (YEL if g >= 1.5 else GRN)
    return f"{color}{bar(g, 0, 4)}{RST} {g:5.2f}g"

def tilt_bar(deg):
    color = RED if deg > TILT_CONFIRM_DEG else (YEL if deg > TILT_CONFIRM_DEG * 0.5 else GRN)
    return f"{color}{bar(deg, 0, 180, 30)}{RST} {deg:5.1f}°"

def render(s, det, pkt_count, hz, events_log):
    sc    = STATE_COLOR.get(det.state, "")
    lines = []

    lines.append(f"{BLD}━━━ IMU Monitor ━━━  {datetime.now().strftime('%H:%M:%S')}"
                 f"  pkts={pkt_count}  {hz:.1f}Hz{RST}")
    lines.append("")

    # ── Raw BLE data ─────────────────────────────────────────────────────────
    lines.append(f"{BLD}── Raw IMU data (BLE) ──────────────────────────────────────────{RST}")
    lines.append(f"  seq={s['seq']:<6}  esp_ms={s['esp_ms']}")
    lines.append(f"  Accel (m/s²):  ax={s['ax']:+7.3f}  ay={s['ay']:+7.3f}  az={s['az']:+7.3f}")
    lines.append(f"  Gyro  (°/s):   gx={s['gx']:+7.2f}  gy={s['gy']:+7.2f}  gz={s['gz']:+7.2f}")
    lines.append(f"  Euler (°):   roll={s['roll']:+7.2f}  pitch={s['pitch']:+7.2f}  yaw={s['yaw']:+7.2f}")
    lines.append(f"  Quat:  q0={s['q0']:+.3f} q1={s['q1']:+.3f} q2={s['q2']:+.3f} q3={s['q3']:+.3f}")
    lines.append("")

    # ── Derived metrics ───────────────────────────────────────────────────────
    lines.append(f"{BLD}── Phân tích ngã ───────────────────────────────────────────────{RST}")
    lines.append(f"  Accel mag:      {accel_bar(s['accel_mag_g'])}"
                 f"  {DIM}freefall<{FREEFALL_G}g  impact>{IMPACT_G}g{RST}")

    tilt = tilt_from_upright(s["pitch"])
    lines.append(f"  Tilt from upright: {tilt_bar(tilt)}"
                 f"  {DIM}(upright={UPRIGHT_PITCH}°  confirm>{TILT_CONFIRM_DEG}°){RST}")

    upright_marker = f"{GRN}UPRIGHT{RST}" if tilt <= TILT_CONFIRM_DEG else f"{RED}NOT UPRIGHT{RST}"
    lines.append(f"  Tư thế:         pitch={s['pitch']:+.1f}°  → {upright_marker}")
    lines.append("")

    # ── State machine ──────────────────────────────────────────────────────────
    lines.append(f"{BLD}── Fall detector state ─────────────────────────────────────────{RST}")
    lines.append(f"  State: {sc}{BLD}{det.state}{RST}")

    if det.state == "FREEFALL" and det.ff_start is not None:
        elapsed = det._now_ms() - det.ff_start
        lines.append(f"  Freefall: {elapsed}ms / {FREEFALL_MIN_MS}ms  [{bar(elapsed, 0, FREEFALL_MIN_MS)}]")

    if det.state == "POST_IMPACT":
        elapsed = det.elapsed_post_ms()
        pct = int(elapsed / POST_WIN_MS * 100)
        lines.append(f"  Bounce settling:   {min(elapsed, POSTURE_DELAY_MS)}ms / {POSTURE_DELAY_MS}ms"
                     f"  {'✓ evaluating' if elapsed >= POSTURE_DELAY_MS else '⏳ waiting'}")
        lines.append(f"  Reject countdown:  {elapsed}ms / {POST_WIN_MS}ms"
                     f"  [{bar(elapsed, 0, POST_WIN_MS, 30)}] {pct}%")
        lines.append(f"  Peak accel:        {det.peak_g:.2f}g  had_freefall={det.had_ff}")
        lines.append(f"  Tilt from upright: {tilt:.1f}°  (>{TILT_CONFIRM_DEG}° → CONFIRM ngay)")

    if det.state == "COOLDOWN":
        rem = det.remaining_cooldown_ms()
        lines.append(f"  Cooldown còn: {rem}ms")
    lines.append("")

    # ── Quick reference ────────────────────────────────────────────────────────
    lines.append(f"{BLD}── Ngưỡng ──────────────────────────────────────────────────────{RST}")
    lines.append(f"  Impact>{IMPACT_G}g  Freefall<{FREEFALL_G}g≥{FREEFALL_MIN_MS}ms  "
                 f"Tilt từ upright>{TILT_CONFIRM_DEG}° → ngay lập tức xác nhận")
    lines.append(f"  Nếu upright sau {POST_WIN_MS//1000}s → bỏ qua (stumble/false positive)")
    lines.append("")

    # ── Recent events ──────────────────────────────────────────────────────────
    lines.append(f"{BLD}── Sự kiện ─────────────────────────────────────────────────────{RST}")
    if events_log:
        for ev in events_log[-5:]:
            color = f"{BLD}{RED}" if ev["type"] == "FALL_CONFIRMED" else DIM
            ff = " [có freefall]" if ev["had_ff"] else ""
            lines.append(f"  {color}{ev['type']}{RST}  "
                         f"confidence={ev['confidence']:.0%}  "
                         f"tilt={ev['tilt']:.1f}°  peak={ev['peak_g']:.1f}g"
                         f"{ff}")
    else:
        lines.append(f"  {DIM}(chưa có sự kiện){RST}")
    lines.append("")
    lines.append(f"{DIM}  Ctrl+C để thoát và khởi động lại edge-imu-ble{RST}")

    return "\n".join(lines)

# ── main BLE loop ─────────────────────────────────────────────────────────────

async def run(manage_service: bool):
    detector   = FallDetectorPy()
    events_log = []
    pkt_count  = 0
    last_render = 0.0
    hz_counter  = 0
    hz_window   = time.monotonic()
    current_hz  = 0.0
    last_sample = None

    def on_notify(_, data: bytearray):
        nonlocal pkt_count, hz_counter, current_hz, last_sample
        s = parse_packet(bytes(data))
        if s is None:
            return
        pkt_count  += 1
        hz_counter += 1
        last_sample = s

        ev = detector.push(s)
        if ev:
            events_log.append(ev)

    print(f"Đang scan BLE cho thiết bị '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
    if device is None:
        print(f"{RED}Không tìm thấy '{DEVICE_NAME}'. Đảm bảo ESP32 đang bật.{RST}")
        return

    print(f"{GRN}Tìm thấy: {device.address}{RST}  Đang kết nối...")
    async with BleakClient(device) as client:
        await client.start_notify(CHAR_UUID, on_notify)
        print(f"{GRN}Đã kết nối. Nhận IMU notifications...{RST}")
        await asyncio.sleep(0.5)

        try:
            while True:
                now = time.monotonic()
                if now - hz_window >= 1.0:
                    current_hz = hz_counter / (now - hz_window)
                    hz_counter = 0
                    hz_window  = now

                if last_sample and (now - last_render) >= 0.1:
                    last_render = now
                    print(CLR, end="")
                    print(render(last_sample, detector, pkt_count, current_hz, events_log))

                await asyncio.sleep(0.05)

        except asyncio.CancelledError:
            pass
        finally:
            await client.stop_notify(CHAR_UUID)

async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-service", action="store_true",
                        help="Don't stop/restart edge-imu-ble service")
    args = parser.parse_args()
    manage = not args.no_service

    if manage:
        print(f"Đang dừng {SERVICE_NAME}...")
        service_stop()
        await asyncio.sleep(1)

    try:
        await run(manage)
    except KeyboardInterrupt:
        print(f"\n{YEL}Ctrl+C — đang thoát...{RST}")
    finally:
        if manage:
            print(f"Khởi động lại {SERVICE_NAME}...")
            service_start()
            print(f"{GRN}edge-imu-ble đã được khởi động lại.{RST}")

if __name__ == "__main__":
    asyncio.run(main())
