#!/usr/bin/env python3
"""Simulate BLE IMU wearable sending data to the backend.

- POST /api/v1/internal/health-logs every HEALTH_LOG_INTERVAL_SECONDS (default 5)
- Randomly trigger FALL events (FALL_PROBABILITY_PER_MINUTE, default 0.2/min ≈ once every 5 min)
- On FALL: attach latest video clip URL + snapshot URL to the event payload
"""

import glob
import json
import math
import os
import random
import time
import urllib.error
import urllib.request
from datetime import datetime

BE_URL          = os.environ.get("BE_BASE_URL",         "http://localhost:3000/api/v1")
EDGE_SECRET     = os.environ.get("EDGE_SECRET",         "")
DEVICE_ID       = os.environ.get("IMU_DEVICE_ID",       "imu_001")
GATEWAY_ID      = os.environ.get("GATEWAY_ID",          "gw_001")
STREAM_HOST     = os.environ.get("EDGE_PUBLIC_HOST",    "192.168.2.14")
STREAM_PORT     = int(os.environ.get("EDGE_STREAM_PORT", 8081))
CLIP_DIR        = os.environ.get("CLIP_DIR",            "/media/usb/camera")
LOG_INTERVAL    = int(os.environ.get("HEALTH_LOG_INTERVAL_SECONDS", 5))
FALL_PROB_MIN   = float(os.environ.get("FALL_PROBABILITY_PER_MINUTE", 0.2))


# ── HTTP helpers ──────────────────────────────────────────────────────────────

def _post(path: str, body: dict) -> dict | None:
    url = f"{BE_URL}{path}"
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "X-Edge-Secret": EDGE_SECRET},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        print(f"[IMU] HTTP {e.code} {path}: {e.read().decode(errors='replace')[:120]}", flush=True)
    except Exception as exc:
        print(f"[IMU] Error {path}: {exc}", flush=True)
    return None


# ── Data generators ───────────────────────────────────────────────────────────

def _normal_imu(t: float) -> dict:
    """Smooth sinusoidal data — person at rest / slow movement."""
    return {
        "deviceId":      DEVICE_ID,
        "accelX":        round(0.08 * math.sin(t * 0.3)  + random.gauss(0, 0.02), 3),
        "accelY":        round(0.06 * math.cos(t * 0.25) + random.gauss(0, 0.02), 3),
        "accelZ":        round(9.81 + 0.04 * math.sin(t * 0.5) + random.gauss(0, 0.01), 3),
        "gyroX":         round(random.gauss(0, 0.8), 3),
        "gyroY":         round(random.gauss(0, 0.8), 3),
        "gyroZ":         round(random.gauss(0, 0.4), 3),
        "tiltAngle":     round(5 + 2 * math.sin(t * 0.1), 2),
        "movementLevel": round(max(0, 0.10 + 0.04 * abs(math.sin(t * 0.4)) + random.gauss(0, 0.01)), 3),
        "batteryLevel":  max(10, 85 - int(t / 3600)),
        "timestamp":     datetime.utcnow().isoformat() + "Z",
    }


def _fall_imu() -> dict:
    """Spike data simulating a fall — large acceleration + rapid rotation."""
    return {
        "deviceId":      DEVICE_ID,
        "accelX":        round(random.uniform(3.5, 5.5), 3),
        "accelY":        round(random.uniform(2.8, 4.5), 3),
        "accelZ":        round(random.uniform(0.3, 1.8), 3),
        "gyroX":         round(random.uniform(55, 130), 3),
        "gyroY":         round(random.uniform(45, 110), 3),
        "gyroZ":         round(random.uniform(20, 60),  3),
        "tiltAngle":     round(random.uniform(62, 88),  2),
        "movementLevel": round(random.uniform(0.82, 1.0), 3),
        "batteryLevel":  80,
        "timestamp":     datetime.utcnow().isoformat() + "Z",
    }


# ── URL helpers ───────────────────────────────────────────────────────────────

def _latest_clip_url() -> str | None:
    files = glob.glob(os.path.join(CLIP_DIR, "**", "*.mp4"), recursive=True)
    if not files:
        return None
    latest = max(files, key=os.path.getmtime)
    rel = os.path.relpath(latest, CLIP_DIR).replace(os.sep, "/")
    return f"http://{STREAM_HOST}:{STREAM_PORT}/clips/{rel}"


def _snapshot_url() -> str:
    return f"http://{STREAM_HOST}:{STREAM_PORT}/snapshot.jpg"


# ── Fall trigger ──────────────────────────────────────────────────────────────

def _trigger_fall():
    fall_data = _fall_imu()
    confidence = round(random.uniform(0.76, 0.97), 2)
    video_url  = _latest_clip_url()

    print(f"[IMU] ⚠️  FALL  confidence={confidence}  video={video_url}", flush=True)

    _post("/internal/health-logs", fall_data)
    result = _post("/internal/events", {
        "eventType":      "FALL",
        "priority":       "CRITICAL",
        "confidence":     confidence,
        "deviceId":       DEVICE_ID,
        "gatewayId":      GATEWAY_ID,
        "message":        f"Phát hiện té ngã (confidence: {confidence:.0%})",
        "snapshotUrl":    _snapshot_url(),
        "relatedVideoUrl": video_url,
    })
    if result and result.get("success"):
        eid = result["data"]["eventId"]
        print(f"[IMU] Event created: {eid}", flush=True)


# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    fall_prob_per_tick = FALL_PROB_MIN / (60 / LOG_INTERVAL)
    print(
        f"[IMU] Simulator started | BE={BE_URL} | device={DEVICE_ID} | "
        f"interval={LOG_INTERVAL}s | fall_prob={FALL_PROB_MIN:.2f}/min",
        flush=True,
    )

    t = 0.0
    while True:
        imu = _normal_imu(t)
        _post("/internal/health-logs", imu)

        if random.random() < fall_prob_per_tick:
            _trigger_fall()

        t += LOG_INTERVAL
        time.sleep(LOG_INTERVAL)


if __name__ == "__main__":
    main()
