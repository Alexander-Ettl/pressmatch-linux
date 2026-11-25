#!/usr/bin/env python3
"""
PressMatch Research Paper — Local Multi-Profile 10-Minute Capture Wizard.

Strictly local (zero cloud / zero Cloudflare). Captures lossless raw frames
from BOTH ELAN (150x52) and FT9201 (96x96 + 64x80) ONCE per person so the
exact same physical touches can be replayed across ALL algorithms:
  NBIS Bozorth3 | Upstream v0 | PressMatch v1 | v2 | v4.0 | v4.1

Hard time budget: <= 10 minutes per profile (typically 7-9 minutes).
Supports:
  - Multiple named profiles (--profile p01, --profile p02, --profile p03)
  - Clean posture battery (flat, shift-forward, shift-back, tilt +/-15 deg)
  - Optional intentional contamination/grime block (--dirty)
  - Automatic cross-person & cross-finger physical impostor matrix on replay!

Usage:
  sudo python3 capture_profile.py --profile p01
  sudo python3 capture_profile.py --profile p01 --dirty   # add oily/grimy probes (~2 min)
  sudo python3 capture_profile.py --profile p02
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
RAW_CAPTURE = BENCH_DIR / "raw_capture"
PROFILES_DIR = BENCH_DIR / "profiles"

HARD_LIMIT_SEC = 600  # Strict 10-minute session cap!

CLEAN_PROTOCOL = [
    # (prefix, count, human instruction)
    ("f1_enr_flat",  4, "Finger 1 ENROLL (flat natural posture — 4x)"),
    ("f1_enr_shift", 4, "Finger 1 ENROLL (slight up/down/left/right shifts — 4x)"),
    ("f1_enr_tilt",  4, "Finger 1 ENROLL (slight +/-15 deg tilts — 4x)"),
    ("f1_prb_flat",  4, "Finger 1 PROBE CLEAN (natural flat — 4x)"),
    ("f1_prb_shift", 4, "Finger 1 PROBE SHIFTED (2mm forward/back/side — 4x)"),
    ("f1_prb_tilt",  4, "Finger 1 PROBE TILTED (+/-15 deg casual angle — 4x)"),
    ("f2_enr_flat",  4, "Finger 2 ENROLL (different finger, flat — 4x)"),
    ("f2_enr_shift", 4, "Finger 2 ENROLL (slight shifts — 4x)"),
    ("f2_enr_tilt",  4, "Finger 2 ENROLL (slight +/-15 deg tilts — 4x)"),
    ("f2_prb_flat",  4, "Finger 2 PROBE CLEAN (natural flat — 4x)"),
    ("f2_prb_shift", 4, "Finger 2 PROBE SHIFTED (2mm shifts — 4x)"),
    ("f2_prb_tilt",  4, "Finger 2 PROBE TILTED (+/-15 deg angle — 4x)"),
]

DIRTY_PROTOCOL = [
    ("f1_prb_dirty_oil",   4, "Finger 1 DIRTY/OILY (light lotion/skin oil — 4x)"),
    ("f1_prb_dirty_grime", 4, "Finger 1 DIRTY/GRIME (chalk/dust/grime — 4x)"),
    ("f2_prb_dirty_oil",   4, "Finger 2 DIRTY/OILY (light lotion/skin oil — 4x)"),
    ("f2_prb_dirty_grime", 4, "Finger 2 DIRTY/GRIME (chalk/dust/grime — 4x)"),
]


def fmt_elapsed(t0: float) -> str:
    sec = int(time.monotonic() - t0)
    rem = max(0, HARD_LIMIT_SEC - sec)
    return f"elapsed {sec // 60}:{sec % 60:02d} | remaining {rem // 60}:{rem % 60:02d} (10:00 max)"


def main() -> int:
    ap = argparse.ArgumentParser(description="PressMatch Local 10-Minute Multi-Profile Capture")
    ap.add_argument("--profile", required=True, help="Participant profile name (e.g. p01, p02, p03)")
    ap.add_argument("--sensors", default="elan,ft9201", help="Comma-separated sensors (default: elan,ft9201)")
    ap.add_argument("--dirty", action="store_true", help="Run the optional dirty/grime contamination battery")
    ap.add_argument("--notes", default="", help="Optional skin/environment notes (e.g. 'cool dry hands')")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("Please run with sudo so raw_capture can temporarily pause fprintd and read USB frames:")
        print(f"  sudo python3 \"{__file__}\" --profile {args.profile}" + (" --dirty" if args.dirty else ""))
        return 1

    if not RAW_CAPTURE.exists():
        print(f"ERROR: {RAW_CAPTURE} not built yet.")
        return 1

    profile_name = "".join(c for c in args.profile.strip().lower() if c.isalnum() or c in "-_")
    if not profile_name:
        print("Invalid profile name.")
        return 1

    prof_dir = PROFILES_DIR / profile_name
    prof_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(PROFILES_DIR, 0o700)
    os.chmod(prof_dir, 0o700)

    meta_path = prof_dir / "profile.json"
    meta = json.loads(meta_path.read_text()) if meta_path.exists() else {
        "profile": profile_name,
        "created": time.strftime("%Y-%m-%d %H:%M:%S"),
        "sessions": [],
    }

    sensors = [s.strip() for s in args.sensors.split(",") if s.strip() in ("elan", "ft9201")]
    steps = DIRTY_PROTOCOL if args.dirty else CLEAN_PROTOCOL
    mode_tag = "dirty" if args.dirty else "clean"

    print("=" * 72)
    print(f" PressMatch Scientific Study — Local Profile Capture: [{profile_name}] ({mode_tag.upper()})")
    print(f" Sensors: {', '.join(sensors)} | Hard session cap: 10 minutes")
    print("=" * 72)
    if args.dirty:
        print("Contamination test: apply a light reproducible contaminant (e.g., hand lotion/oil")
        print("for OIL blocks, and light dust/flour/chalk for GRIME blocks). Keep both sensors dry")
        print("between sessions and wipe them clean after finishing!")
    else:
        print("Protocol: pick ANY two comfortable fingers (Finger 1 & Finger 2).")
        print("Use the EXACT SAME Finger 1 and Finger 2 on both ELAN and FT9201.")
        print("Total: 48 touches per sensor (~3.5-4.5 min per sensor, <= 9 min total).")
    print()

    # Temporarily stop fprintd so libusb can claim sensors cleanly, ALWAYS restart in finally!
    print("Pausing fprintd temporarily for lossless raw capture...")
    subprocess.run(["systemctl", "stop", "fprintd"], check=False)
    time.sleep(0.5)

    t0 = time.monotonic()
    completed_blocks = []
    try:
        for sensor in sensors:
            s_dir = prof_dir / sensor
            s_dir.mkdir(parents=True, exist_ok=True)
            print("\n" + "-" * 72)
            print(f" SENSOR: {sensor.upper()}  ({fmt_elapsed(t0)})")
            print("-" * 72)
            input(f"Keep finger OFF the {sensor.upper()} sensor for 1 second, then press Enter to begin...")

            for prefix, count, desc in steps:
                if time.monotonic() - t0 >= HARD_LIMIT_SEC:
                    print("\n[10:00 HARD CAP REACHED] Ending session cleanly — all captured blocks saved!")
                    break
                print(f"\n[{fmt_elapsed(t0)}] -> {desc}")
                rc = subprocess.run(
                    [str(RAW_CAPTURE), sensor, str(s_dir), prefix, str(count), desc]
                ).returncode
                if rc != 0:
                    print(f"  ! Sensor {sensor} returned code {rc}. Stopping this sensor cleanly.")
                    break
                completed_blocks.append(f"{sensor}:{prefix}")
    finally:
        print("\nRestoring fprintd service...")
        subprocess.run(["systemctl", "start", "fprintd"], check=False)

    meta["sessions"].append({
        "mode": mode_tag,
        "sensors": sensors,
        "notes": args.notes,
        "elapsed_sec": round(time.monotonic() - t0, 1),
        "blocks": completed_blocks,
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
    })
    meta_path.write_text(json.dumps(meta, indent=2))

    # Give ownership back to normal user if invoked under sudo
    sudo_uid = os.environ.get("SUDO_UID")
    sudo_gid = os.environ.get("SUDO_GID")
    if sudo_uid and sudo_gid:
        subprocess.run(["chown", "-R", f"{sudo_uid}:{sudo_gid}", str(PROFILES_DIR)], check=False)

    print("\n" + "=" * 72)
    print(f" Saved profile [{profile_name}] locally in: {prof_dir}")
    print(f" Total elapsed: {fmt_elapsed(t0)}")
    print(" Next: replay ALL profiles across ALL 6 algorithms with:")
    print(f"   python3 \"{BENCH_DIR / 'replay_profiles.py'}\"")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
