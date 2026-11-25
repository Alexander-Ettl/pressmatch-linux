#!/usr/bin/env python3
"""
PressMatch Research Study — Streaming Single-Session Raw Capture Worker.

Launched once per capture session via pkexec from dual_guide.py so the user
authenticates AT MOST ONCE for the entire <=10-minute session. Streams real-time
JSON-line events to stdout so the HTML guide animates every single touch, lift,
posture shift/tilt, finger swap, sensor switch, and redo live in the browser!

Features:
  - Practice mode (--mode practice): Short 4-posture/sensor warm-up (~45 s) in a
    disposable temp directory so family members can practice stress-free with
    ZERO pollution of paper-benchmark/profiles/.
  - Instant supervisor Redo (--redo-file): Flags the last captured touch as wrong,
    deletes its saved raw/u8/jsonl entry, jumps back 1 step (even across block or
    sensor boundaries!), and overwrites it cleanly without restarting the run.
  - Instant Stop (--stop-file + stdin EOF): Cleanly terminates raw_capture and
    always restores fprintd + profile ownership in finally:.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
RAW_CAPTURE = BENCH_DIR / "raw_capture"
PROFILES_DIR = BENCH_DIR / "profiles"

HARD_LIMIT_SEC = 600

# (prefix, count, finger_label, motion_css, headline, instruction)
CLEAN_BLOCKS = [
    ("f1_enr_flat",  4, "Finger 1 (Your Choice)", "flat",          "Finger 1 · Natural Flat",      "Use any comfortable finger — rest flat on the sensor, then lift (4×)."),
    ("f1_enr_shift", 4, "Finger 1 (Same Finger)", "shift-forward", "Finger 1 · Slight Shifts",     "Same finger — shift slightly (~2 mm) up, down, left, and right (4×)."),
    ("f1_enr_tilt",  4, "Finger 1 (Same Finger)", "tilt-left",     "Finger 1 · Gentle Tilts",      "Same finger — tilt slightly left and right (~15°) to cover edges (4×)."),
    ("f1_prb_flat",  4, "Finger 1 (Test · Clean)","flat",          "Finger 1 · Clean Test (Flat)", "Same finger — 4 relaxed, everyday unlock touches."),
    ("f1_prb_shift", 4, "Finger 1 (Test · Shift)","shift-back",    "Finger 1 · Shifted Test",      "Same finger — slightly off-center (~2 mm forward/back/side, 4×)."),
    ("f1_prb_tilt",  4, "Finger 1 (Test · Tilt)", "tilt-right",    "Finger 1 · Tilted Test",       "Same finger — casual angled touches (~15° left/right, 4×)."),
    ("f2_enr_flat",  4, "Finger 2 (New Finger)",  "flat",          "Finger 2 · Natural Flat",      "Switch to a SECOND finger — rest flat on the sensor, then lift (4×)."),
    ("f2_enr_shift", 4, "Finger 2 (Same Finger)", "shift-forward", "Finger 2 · Slight Shifts",     "Same second finger — shift slightly (~2 mm) around the pad (4×)."),
    ("f2_enr_tilt",  4, "Finger 2 (Same Finger)", "tilt-left",     "Finger 2 · Gentle Tilts",      "Same second finger — tilt slightly left and right (~15°, 4×)."),
    ("f2_prb_flat",  4, "Finger 2 (Test · Clean)","flat",          "Finger 2 · Clean Test (Flat)", "Same second finger — 4 relaxed, everyday unlock touches."),
    ("f2_prb_shift", 4, "Finger 2 (Test · Shift)","shift-back",    "Finger 2 · Shifted Test",      "Same second finger — slightly off-center (~2 mm shifts, 4×)."),
    ("f2_prb_tilt",  4, "Finger 2 (Test · Tilt)", "tilt-right",    "Finger 2 · Tilted Test",       "Same second finger — casual angled touches (~15° left/right, 4×)."),
]

DIRTY_BLOCKS = [
    ("f1_prb_dirty_oil",   4, "Finger 1 · Oily/Lotion", "flat",         "Finger 1 · Oily / Lotion Test", "Apply light lotion or skin oil to Finger 1 — 4 natural touches."),
    ("f1_prb_dirty_grime", 4, "Finger 1 · Grime/Dust",  "shift-forward","Finger 1 · Grime / Dust Test",  "Lightly dab dust/chalk/grime on Finger 1 — 4 natural touches."),
    ("f2_prb_dirty_oil",   4, "Finger 2 · Oily/Lotion", "flat",         "Finger 2 · Oily / Lotion Test", "Apply light lotion or skin oil to Finger 2 — 4 natural touches."),
    ("f2_prb_dirty_grime", 4, "Finger 2 · Grime/Dust",  "shift-forward","Finger 2 · Grime / Dust Test",  "Lightly dab dust/chalk/grime on Finger 2 — 4 natural touches."),
]

# Short, stress-free practice warm-up covering every cue (flat, shift, tilt, finger swap, sensor swap)
PRACTICE_BLOCKS = [
    ("p_f1_flat",  1, "Finger 1 (Practice)", "flat",          "Finger 1 · Natural Flat",  "Practice: rest any comfortable finger gently, then lift."),
    ("p_f1_shift", 1, "Finger 1 (Practice)", "shift-forward", "Finger 1 · Slight Shifts", "Practice: shift the same finger slightly off-center."),
    ("p_f1_tilt",  1, "Finger 1 (Practice)", "tilt-left",     "Finger 1 · Gentle Tilts",  "Practice: gently tilt the same finger slightly."),
    ("p_f2_flat",  1, "Finger 2 (Practice)", "flat",          "Finger 2 · Natural Flat",  "Practice: switch to your second finger, rest gently, then lift."),
]

STOP_FLAG = False
REDO_EVENT = threading.Event()
ACTIVE_PROC: subprocess.Popen | None = None
PROC_LOCK = threading.Lock()


def emit(obj: dict) -> None:
    print(json.dumps(obj), flush=True)


def _kill_active_proc() -> None:
    with PROC_LOCK:
        proc = ACTIVE_PROC
    if proc and proc.poll() is None:
        try:
            proc.terminate()
        except Exception:
            pass

        def _force_kill():
            time.sleep(0.55)
            if proc.poll() is None:
                try:
                    proc.kill()
                except Exception:
                    pass

        threading.Thread(target=_force_kill, daemon=True).start()


def trigger_stop() -> None:
    global STOP_FLAG
    STOP_FLAG = True
    _kill_active_proc()


def trigger_redo() -> None:
    REDO_EVENT.set()
    _kill_active_proc()


def handle_sig(_sig, _frame) -> None:
    trigger_stop()


def start_watchers(stop_file: Path | None, redo_file: Path | None) -> None:
    """Allow unprivileged dual_guide.py to stop or redo instantaneously without root signals."""
    if stop_file is not None:
        def _watch_stop():
            while not STOP_FLAG:
                if stop_file.exists():
                    trigger_stop()
                    return
                time.sleep(0.08)
        threading.Thread(target=_watch_stop, daemon=True).start()

    if redo_file is not None:
        def _watch_redo():
            last_nonce = ""
            while not STOP_FLAG:
                try:
                    if redo_file.exists():
                        nonce = redo_file.read_text(errors="ignore").strip()
                        if nonce and nonce != last_nonce:
                            last_nonce = nonce
                            trigger_redo()
                except OSError:
                    pass
                time.sleep(0.08)
        threading.Thread(target=_watch_redo, daemon=True).start()

    if not sys.stdin.isatty():
        def _watch_stdin():
            try:
                for line in sys.stdin:
                    cmd = line.strip().upper()
                    if cmd == "REDO":
                        trigger_redo()
                    elif "STOP" in cmd:
                        trigger_stop()
                        return
                # Only treat EOF as stop if no --stop-file was provided (pkexec may close stdin!)
                if stop_file is None and redo_file is None:
                    trigger_stop()
            except Exception:
                pass
        threading.Thread(target=_watch_stdin, daemon=True).start()


def remove_captured_touch(s_dir: Path, prefix: str, idx: int) -> None:
    """Remove all saved raw/normalized files and the matching captures.jsonl line for one touch."""
    for ext in ("raw16", "raw8", "u8", "u8_64x80"):
        p = s_dir / f"{prefix}_{idx:02d}.{ext}"
        try:
            if p.exists():
                p.unlink()
        except OSError:
            pass

    log_path = s_dir / "captures.jsonl"
    if log_path.exists():
        try:
            lines = log_path.read_text(encoding="utf-8", errors="ignore").splitlines()
            drop_i = -1
            for i in range(len(lines) - 1, -1, -1):
                try:
                    obj = json.loads(lines[i])
                    if obj.get("prefix") == prefix and int(obj.get("idx", -1)) == idx:
                        drop_i = i
                        break
                except Exception:
                    continue
            if drop_i >= 0:
                lines.pop(drop_i)
                log_path.write_text(("\n".join(lines) + "\n") if lines else "", encoding="utf-8")
        except OSError:
            pass


def fix_ownership(owner_uid: int, owner_gid: int) -> None:
    if owner_uid > 0 and PROFILES_DIR.exists():
        subprocess.run(["chown", "-R", f"{owner_uid}:{owner_gid}", str(PROFILES_DIR)], check=False)
        subprocess.run(["chmod", "700", str(PROFILES_DIR)], check=False)


def main() -> int:
    global ACTIVE_PROC
    signal.signal(signal.SIGTERM, handle_sig)
    signal.signal(signal.SIGINT, handle_sig)

    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--mode", choices=("clean", "dirty", "practice"), default="clean")
    ap.add_argument("--sensors", default="elan,ft9201")
    ap.add_argument("--owner-uid", type=int, default=1000)
    ap.add_argument("--owner-gid", type=int, default=1000)
    ap.add_argument("--stop-file", default="")
    ap.add_argument("--redo-file", default="")
    args = ap.parse_args()

    is_practice = args.mode == "practice"
    profile = "".join(c for c in args.profile.strip().lower() if c.isalnum() or c in "-_").lstrip("-")
    if not profile:
        if is_practice:
            profile = "practice"
        else:
            emit({"type": "error", "message": "Invalid profile name."})
            return 1

    stop_file = Path(args.stop_file) if args.stop_file else None
    redo_file = Path(args.redo_file) if args.redo_file else None
    start_watchers(stop_file, redo_file)

    sensors = [s.strip() for s in args.sensors.split(",") if s.strip() in ("elan", "ft9201")]
    if is_practice:
        blocks_template = PRACTICE_BLOCKS
    elif args.mode == "dirty":
        blocks_template = DIRTY_BLOCKS
    else:
        blocks_template = CLEAN_BLOCKS

    # Build flat indexed schedule across all selected sensors so Redo can step back across any boundary!
    schedule = []
    for s_idx, sensor in enumerate(sensors):
        for b_idx, (prefix, count, finger_lbl, motion, headline, instr) in enumerate(blocks_template):
            schedule.append({
                "s_idx": s_idx,
                "sensor": sensor,
                "is_sensor_start": (s_idx > 0 and b_idx == 0),
                "prefix": prefix,
                "count": count,
                "fingerLabel": finger_lbl,
                "motion": motion,
                "headline": headline,
                "instruction": instr,
            })

    tmp_practice_dir: Path | None = None
    if is_practice:
        tmp_practice_dir = Path(tempfile.mkdtemp(prefix="pressmatch_practice_"))
        prof_dir = tmp_practice_dir
        meta_path = None
        meta = {}
    else:
        prof_dir = PROFILES_DIR / profile
        prof_dir.mkdir(parents=True, exist_ok=True)
        fix_ownership(args.owner_uid, args.owner_gid)
        meta_path = prof_dir / "profile.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {
            "profile": profile,
            "created": time.strftime("%Y-%m-%d %H:%M:%S"),
            "sessions": [],
        }

    # Clear stale ELAN bg ref on fresh session start
    for sensor in sensors:
        s_dir = prof_dir / sensor
        s_dir.mkdir(parents=True, exist_ok=True)
        bg_ref = s_dir / "elan_bg_ref.raw16"
        if bg_ref.exists():
            try:
                bg_ref.unlink()
            except OSError:
                pass

    total_touches = sum(item["count"] for item in schedule)
    done_touches = 0
    history: list[tuple[int, int, Path, str]] = []  # (sched_idx, touch_0idx, s_dir, prefix)
    completed_blocks: list[str] = []
    shown_sensor_transitions: set[int] = set()

    emit({
        "type": "prepare",
        "sensor": sensors[0] if sensors else "dual",
        "headline": "Preparing Sensors…",
        "instruction": "Pausing background lock service temporarily. Keep both fingers off the sensors for 1 second.",
        "totalTouches": total_touches,
        "doneTouches": 0,
        "remainingSec": HARD_LIMIT_SEC,
    })

    subprocess.run(["systemctl", "stop", "fprintd"], check=False)
    time.sleep(0.4)

    t0 = time.monotonic()
    status_out = "complete"
    err_msg = ""

    sched_idx = 0
    start_touch = 0

    try:
        while sched_idx < len(schedule):
            if STOP_FLAG:
                status_out = "stopped"
                break

            rem = int(HARD_LIMIT_SEC - (time.monotonic() - t0))
            if rem <= 0 and not is_practice:
                status_out = "timeout"
                emit({
                    "type": "info",
                    "headline": "10-Minute Cap Reached",
                    "instruction": "All captured touches have been saved safely!",
                    "remainingSec": 0,
                })
                break

            blk = schedule[sched_idx]
            sensor = blk["sensor"]
            s_dir = prof_dir / sensor
            s_dir.mkdir(parents=True, exist_ok=True)

            # Halfway sensor-switch cue (shown once on normal forward entry)
            if blk["is_sensor_start"] and start_touch == 0 and sched_idx not in shown_sensor_transitions:
                shown_sensor_transitions.add(sched_idx)
                sname = "USB Reader (FT9201)" if sensor == "ft9201" else "Power Button (ELAN)"
                for sec in (3, 2, 1):
                    if STOP_FLAG or REDO_EVENT.is_set():
                        break
                    emit({
                        "type": "transition",
                        "sensor": sensor,
                        "headline": f"Switch to {sname}",
                        "instruction": f"Use the EXACT SAME Finger 1 and Finger 2 on your {sname}. Starting in {sec}s…",
                        "doneTouches": done_touches,
                        "totalTouches": total_touches,
                        "remainingSec": max(0, int(HARD_LIMIT_SEC - (time.monotonic() - t0))),
                    })
                    time.sleep(1.0)
                if STOP_FLAG:
                    status_out = "stopped"
                    break
                if REDO_EVENT.is_set():
                    REDO_EVENT.clear()
                    if history:
                        prev_si, prev_ti, prev_sdir, prev_pref = history.pop()
                        remove_captured_touch(prev_sdir, prev_pref, prev_ti)
                        done_touches = max(0, done_touches - 1)
                        sched_idx, start_touch = prev_si, prev_ti
                        emit({
                            "type": "redo",
                            "sensor": schedule[sched_idx]["sensor"],
                            "doneTouches": done_touches,
                            "totalTouches": total_touches,
                            "remainingSec": max(0, int(HARD_LIMIT_SEC - (time.monotonic() - t0))),
                        })
                        time.sleep(0.35)
                        continue

            REDO_EVENT.clear()
            prefix = blk["prefix"]
            count = blk["count"]
            finger_lbl = blk["fingerLabel"]
            motion = blk["motion"]
            headline = blk["headline"]
            instr = blk["instruction"]

            emit({
                "type": "block",
                "sensor": sensor,
                "prefix": prefix,
                "blockIdx": sched_idx + 1,
                "blockTotal": len(schedule),
                "touchIdx": start_touch + 1,
                "touchTotal": count,
                "fingerLabel": finger_lbl,
                "motion": motion,
                "headline": headline,
                "instruction": instr,
                "doneTouches": done_touches,
                "totalTouches": total_touches,
                "remainingSec": max(0, rem),
            })

            cur_touch_0idx = start_touch
            proc = subprocess.Popen(
                [str(RAW_CAPTURE), sensor, str(s_dir), prefix, str(count), headline, str(start_touch)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            with PROC_LOCK:
                ACTIVE_PROC = proc
            assert proc.stdout is not None

            for raw_line in proc.stdout:
                line = raw_line.strip()
                rem_now = max(0, int(HARD_LIMIT_SEC - (time.monotonic() - t0)))
                if "lift finger off power button" in line.lower():
                    emit({
                        "type": "warn",
                        "sensor": sensor,
                        "headline": "Lift Your Finger",
                        "instruction": "Take your finger off the power button for a moment so we can calibrate it cleanly.",
                        "doneTouches": done_touches,
                        "totalTouches": total_touches,
                        "remainingSec": rem_now,
                    })
                elif line.startswith("OK") or "  OK  " in raw_line:
                    history.append((sched_idx, cur_touch_0idx, s_dir, prefix))
                    cur_touch_0idx += 1
                    done_touches += 1
                    emit({
                        "type": "captured",
                        "sensor": sensor,
                        "prefix": prefix,
                        "touchIdx": cur_touch_0idx,
                        "touchTotal": count,
                        "feedback": line.replace("(lift finger)", "").strip(),
                        "doneTouches": done_touches,
                        "totalTouches": total_touches,
                        "remainingSec": rem_now,
                    })
                elif m := re.search(r"\[(ELAN|FT9201)\s+(\d+)/(\d+)\]", line):
                    t_i, t_n = int(m.group(2)), int(m.group(3))
                    cur_touch_0idx = t_i - 1
                    emit({
                        "type": "touch_wait",
                        "sensor": sensor,
                        "prefix": prefix,
                        "touchIdx": t_i,
                        "touchTotal": t_n,
                        "fingerLabel": finger_lbl,
                        "motion": motion,
                        "headline": headline,
                        "instruction": instr,
                        "doneTouches": done_touches,
                        "totalTouches": total_touches,
                        "remainingSec": rem_now,
                    })
                elif line.startswith("ERROR:"):
                    err_msg = line

            rc = proc.wait()
            with PROC_LOCK:
                ACTIVE_PROC = None

            if STOP_FLAG:
                status_out = "stopped"
                break

            if REDO_EVENT.is_set():
                REDO_EVENT.clear()
                if history:
                    prev_si, prev_ti, prev_sdir, prev_pref = history.pop()
                    remove_captured_touch(prev_sdir, prev_pref, prev_ti)
                    done_touches = max(0, done_touches - 1)
                    sched_idx = prev_si
                    start_touch = prev_ti
                    emit({
                        "type": "redo",
                        "sensor": schedule[sched_idx]["sensor"],
                        "doneTouches": done_touches,
                        "totalTouches": total_touches,
                        "remainingSec": max(0, int(HARD_LIMIT_SEC - (time.monotonic() - t0))),
                    })
                    time.sleep(0.35)
                continue

            if rc != 0:
                status_out = "error"
                if not err_msg:
                    err_msg = f"Sensor {sensor} exited with code {rc}."
                break

            tag = f"{sensor}:{prefix}"
            if tag not in completed_blocks:
                completed_blocks.append(tag)
            sched_idx += 1
            start_touch = 0
    finally:
        _kill_active_proc()
        subprocess.run(["systemctl", "start", "fprintd"], check=False)
        if is_practice and tmp_practice_dir and tmp_practice_dir.exists():
            shutil.rmtree(tmp_practice_dir, ignore_errors=True)
        elif meta_path is not None:
            meta["sessions"].append({
                "mode": args.mode,
                "sensors": sensors,
                "elapsed_sec": round(time.monotonic() - t0, 1),
                "status": status_out,
                "blocks": completed_blocks,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            })
            meta_path.write_text(json.dumps(meta, indent=2))
            fix_ownership(args.owner_uid, args.owner_gid)

    emit({
        "type": "done",
        "status": status_out,
        "mode": args.mode,
        "error": err_msg,
        "profile": profile,
        "doneTouches": done_touches,
        "totalTouches": total_touches,
        "elapsedSec": round(time.monotonic() - t0, 1),
    })
    return 0 if status_out in ("complete", "timeout", "stopped") else 1


if __name__ == "__main__":
    raise SystemExit(main())
