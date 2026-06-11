#!/usr/bin/env python3
"""Continuous video recording from local MJPEG stream.

Saves 5-minute MP4 clips to:
  /media/usb/camera/YYYY-MM-DD/HH-MM.mp4

Auto-deletes clips older than CLIP_RETENTION_HOURS (default 72).
"""

import os
import subprocess
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path

STREAM_URL      = os.environ.get("EDGE_STREAM_URL", "http://localhost:8081/stream.mjpg")
CLIP_DIR        = Path(os.environ.get("CLIP_DIR", "/media/usb/camera"))
SEGMENT_SECONDS = int(os.environ.get("CLIP_SEGMENT_SECONDS", 300))   # 5 minutes
RETENTION_HOURS = int(os.environ.get("CLIP_RETENTION_HOURS", 72))


def _next_clip_path() -> Path:
    now = datetime.now()
    date_dir = CLIP_DIR / now.strftime("%Y-%m-%d")
    date_dir.mkdir(parents=True, exist_ok=True)
    return date_dir / now.strftime("%H-%M.mp4")


def latest_clip_relpath() -> str | None:
    """Return relative path of the most recently completed clip, e.g. '2026-06-11/10-05.mp4'."""
    clips = sorted(CLIP_DIR.rglob("*.mp4"), key=lambda p: p.stat().st_mtime, reverse=True)
    if clips:
        return str(clips[0].relative_to(CLIP_DIR))
    return None


def _cleanup_loop():
    while True:
        time.sleep(3600)   # run every hour
        cutoff = datetime.now() - timedelta(hours=RETENTION_HOURS)
        for f in list(CLIP_DIR.rglob("*.mp4")):
            try:
                if datetime.fromtimestamp(f.stat().st_mtime) < cutoff:
                    f.unlink()
                    print(f"[CLEANUP] Deleted: {f}", flush=True)
                    if not any(f.parent.iterdir()):
                        f.parent.rmdir()
            except Exception:
                pass


def _record_loop():
    """Main loop: record SEGMENT_SECONDS-long clips one after another."""
    while True:
        out = _next_clip_path()
        cmd = [
            "ffmpeg", "-y",
            "-i", STREAM_URL,
            "-t", str(SEGMENT_SECONDS),
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-crf", "28",
            str(out),
        ]
        print(f"[REC] → {out}", flush=True)
        try:
            result = subprocess.run(cmd, capture_output=True)
            if result.returncode != 0:
                err = result.stderr.decode(errors="replace")[-200:]
                print(f"[REC] ffmpeg error: {err}", flush=True)
                time.sleep(5)
        except Exception as exc:
            print(f"[REC] Error: {exc}", flush=True)
            time.sleep(5)


if __name__ == "__main__":
    CLIP_DIR.mkdir(parents=True, exist_ok=True)
    print(f"[INFO] Saving clips to {CLIP_DIR} | {SEGMENT_SECONDS}s/clip | retain {RETENTION_HOURS}h", flush=True)
    threading.Thread(target=_cleanup_loop, daemon=True).start()
    _record_loop()
