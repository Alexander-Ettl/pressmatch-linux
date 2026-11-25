#!/usr/bin/env python3
"""
PressMatch v4.1 Dual-Sensor Ultimate Stress Test & Visual Guide Server.

Drives BOTH live fprintd devices (ELAN 04f3:0c6e 150x52 strip + FocalTech FT9201
2808:93a9 96x96 square) directly over system D-Bus (zero pkexec popups!), supports:
  - Phase 1: ELAN 150x52 stress test (18 checks: genuine angles/shifts/light + wrong fingers)
  - Phase 2: FT9201 96x96 stress test (18 checks: genuine angles/shifts/light + wrong fingers)
  - Phase 3: SIMULTANEOUS Dual-Reader Race (both sensors armed in parallel + lid state)
  - One-click FT9201 finger enrollment (e.g. left-index-finger) right inside the guide

Saves every stress test run to ./results/run-*/ (results.log + summary.json) for paper analysis.
"""
import argparse
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parent
PAPER_DIR = REPO_ROOT / "paper-benchmark"
PROFILES_DIR = PAPER_DIR / "profiles"
PAPER_RESULTS_DIR = PAPER_DIR / "results"
PAPER_FIGURES_DIR = PAPER_DIR / "figures"
WORKER_SCRIPT = PAPER_DIR / "web_capture_worker.py"
REPLAY_SCRIPT = PAPER_DIR / "replay_profiles.py"
RESULTS_DIR = ROOT / "results"
RESULTS_DIR.mkdir(parents=True, exist_ok=True)

ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/guide.css": ("guide.css", "text/css; charset=utf-8"),
    "/guide.js": ("guide.js", "text/javascript; charset=utf-8"),
}

# 18-check stress protocol (12 genuine across varied postures + 6 wrong fingers)
STRESS_CHECKS_2F = (
    ("right-index",  True,  "flat",          "Natural flat touch — right index."),
    ("left-index",   True,  "flat",          "Natural flat touch — left index."),
    ("right-middle", False, "flat",          "Wrong-finger security check — right middle."),
    ("right-index",  True,  "shift-forward", "Slightly forward (~2 mm toward tip) — right index."),
    ("left-index",   True,  "shift-forward", "Slightly forward (~2 mm toward tip) — left index."),
    ("left-middle",  False, "flat",          "Wrong-finger security check — left middle."),
    ("right-index",  True,  "tilt-left",     "Casual angled tap (slight left tilt) — right index."),
    ("left-index",   True,  "tilt-right",    "Casual angled tap (slight right tilt) — left index."),
    ("right-ring",   False, "flat",          "Wrong-finger security check — right ring."),
    ("right-index",  True,  "shift-back",    "Slightly back (~2 mm lower pad) — right index."),
    ("left-index",   True,  "shift-back",    "Slightly back (~2 mm lower pad) — left index."),
    ("right-thumb",  False, "flat",          "Wrong-finger security check — right thumb."),
    ("right-index",  True,  "tilt-right",    "Quick light tap (slight right tilt) — right index."),
    ("left-index",   True,  "tilt-left",     "Quick light tap (slight left tilt) — left index."),
    ("right-middle", False, "flat",          "Wrong-finger security check — right middle."),
    ("right-index",  True,  "flat",          "Relaxed final unlock touch — right index."),
    ("left-index",   True,  "flat",          "Relaxed final unlock touch — left index."),
    ("left-middle",  False, "flat",          "Final wrong-finger security check — left middle."),
)

STRESS_CHECKS_1F = (
    ("right-index",  True,  "flat",          "Natural flat touch — right index."),
    ("right-middle", False, "flat",          "Wrong-finger security check — right middle."),
    ("right-index",  True,  "shift-forward", "Slightly forward (~2 mm toward tip) — right index."),
    ("left-index",   False, "flat",          "Wrong-finger security check — left index (not enrolled on FT9201 yet)."),
    ("right-index",  True,  "tilt-left",     "Casual angled tap (slight left tilt) — right index."),
    ("left-middle",  False, "flat",          "Wrong-finger security check — left middle."),
    ("right-index",  True,  "shift-back",    "Slightly back (~2 mm lower pad) — right index."),
    ("right-ring",   False, "flat",          "Wrong-finger security check — right ring."),
    ("right-index",  True,  "tilt-right",    "Quick light tap (slight right tilt) — right index."),
    ("right-thumb",  False, "flat",          "Wrong-finger security check — right thumb."),
    ("right-index",  True,  "flat",          "Light quick touch — right index."),
    ("right-middle", False, "flat",          "Wrong-finger security check — right middle."),
    ("right-index",  True,  "tilt-left",     "Natural tilted unlock — right index."),
    ("right-index",  True,  "flat",          "Final relaxed unlock — right index."),
)

DUAL_RACE_CHECKS = (
    ("right-index",  True,  "flat", "Touch EITHER reader (ELAN power button OR FT9201 dongle) — right index."),
    ("right-index",  True,  "flat", "Now try the OTHER reader — right index (both are armed simultaneously!)."),
    ("right-middle", False, "flat", "Wrong-finger check on EITHER reader — right middle (must reject!)."),
    ("right-index",  True,  "flat", "Quick ergonomic tap on whichever reader feels natural — right index."),
    ("right-index",  True,  "flat", "Switch readers again — right index."),
    ("left-middle",  False, "flat", "Wrong-finger check on EITHER reader — left middle (must reject!)."),
    ("right-index",  True,  "tilt-left", "Casual angled tap on EITHER reader — right index."),
    ("right-index",  True,  "flat", "Fast tap on your preferred reader — right index."),
)


def read_lid_state():
    lid_root = Path("/proc/acpi/button/lid")
    if lid_root.is_dir():
        for p in lid_root.iterdir():
            sf = p / "state"
            try:
                txt = sf.read_text().strip().lower()
                if "closed" in txt:
                    return "closed"
                if "open" in txt:
                    return "open"
            except OSError:
                pass
    return "open"


def classify_sensor(name):
    low = (name or "").lower()
    if "ft9201" in low or "focal" in low:
        return {
            "id": "ft9201",
            "short": "FT9201",
            "title": "FocalTech FT9201 (2808:93a9)",
            "width": 96,
            "height": 96,
            "pixels": 9216,
            "shape": "square",
            "aspect": "1.00 : 1 Square",
        }
    return {
        "id": "elan",
        "short": "ELAN",
        "title": "ELAN Press (04f3:0c6e)",
        "width": 150,
        "height": 52,
        "pixels": 7800,
        "shape": "strip",
        "aspect": "2.88 : 1 Strip",
    }


def discover_fprintd_devices(username):
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    mgr = Gio.DBusProxy.new_sync(
        bus, Gio.DBusProxyFlags.NONE, None,
        "net.reactivated.Fprint", "/net/reactivated/Fprint/Manager",
        "net.reactivated.Fprint.Manager", None,
    )
    paths = mgr.call_sync("GetDevices", None, Gio.DBusCallFlags.NONE, 4000, None).unpack()[0]
    found = {}
    for path in paths:
        proxy = Gio.DBusProxy.new_sync(
            bus, Gio.DBusProxyFlags.NONE, None,
            "net.reactivated.Fprint", path,
            "net.reactivated.Fprint.Device", None,
        )
        name_v = proxy.get_cached_property("name")
        name = name_v.unpack() if name_v else path
        try:
            fingers = list(proxy.call_sync(
                "ListEnrolledFingers", GLib.Variant("(s)", (username,)),
                Gio.DBusCallFlags.NONE, 3000, None,
            ).unpack()[0])
        except Exception:
            fingers = []
        info = classify_sensor(name)
        info["path"] = path
        info["name"] = name
        info["fingers"] = fingers
        found[info["id"]] = info
    return found


def scan_profiles():
    """Inspect paper-benchmark/profiles/ for all saved local participant profiles."""
    out = []
    if not PROFILES_DIR.exists():
        return {"profiles": [], "permissionIssue": False}
    try:
        entries = sorted(PROFILES_DIR.iterdir())
    except PermissionError:
        return {"profiles": [], "permissionIssue": True}

    for pdir in entries:
        if not pdir.is_dir():
            continue
        info = {
            "name": pdir.name,
            "elanClean": 0, "elanDirty": 0,
            "ftClean": 0, "ftDirty": 0,
            "readable": True,
        }
        try:
            for skey, c_key, d_key in (("elan", "elanClean", "elanDirty"), ("ft9201", "ftClean", "ftDirty")):
                sdir = pdir / skey
                if sdir.is_dir():
                    for f in sdir.glob("*.u8"):
                        if "_dirty_" in f.name:
                            info[d_key] += 1
                        else:
                            info[c_key] += 1
        except PermissionError:
            info["readable"] = False
        out.append(info)
    return {"profiles": out, "permissionIssue": any(not p["readable"] for p in out)}


def load_paper_summaries():
    live_p = PAPER_RESULTS_DIR / "live_profiles_summary.json"
    pub_p = PAPER_RESULTS_DIR / "master_paper_summary.json"
    figs = sorted(f.name for f in PAPER_FIGURES_DIR.glob("*.png")) if PAPER_FIGURES_DIR.is_dir() else []
    return {
        "live": json.loads(live_p.read_text()) if live_p.exists() else None,
        "public": json.loads(pub_p.read_text()) if pub_p.exists() else None,
        "figures": figs,
    }


class Stopped(Exception):
    pass


class DualStressGuide:
    def __init__(self, username):
        self.username = username
        self.condition = threading.Condition()
        self.sequence = 0
        self.running = False
        self.stop_evt = threading.Event()
        self.active_loop = None
        self.active_proc = None
        self.active_stop_file = None
        self.active_redo_file = None
        self.study_meta = {
            "active": False,
            "profile": "",
            "mode": "clean",
            "doneTouches": 0,
            "totalTouches": 0,
            "remainingSec": 600,
            "fingerLabel": "",
            "replaying": False,
            "redoSeq": 0,
        }
        self.fb_seq = 0
        self.last_fb = None
        self.stats = self._empty_stats()
        self.devices = self._safe_discover()
        self.show(
            "idle",
            "Ready for Research Study & Stress Test",
            "Enter a profile name (e.g. p01, p02, p03) and click Start Guided Capture — zero terminal required.",
            sensor="dual",
        )

    def _empty_stats(self):
        return {
            "elan":   {"gen_ok": 0, "gen_n": 0, "imp_acc": 0, "imp_n": 0, "lat_ms": []},
            "ft9201": {"gen_ok": 0, "gen_n": 0, "imp_acc": 0, "imp_n": 0, "lat_ms": []},
            "dual":   {"gen_ok": 0, "gen_n": 0, "imp_acc": 0, "imp_n": 0, "elan_wins": 0, "ft_wins": 0, "lat_ms": []},
        }

    def _stats_summary(self):
        out = {}
        for k, s in self.stats.items():
            med = round(statistics.median(s["lat_ms"]), 1) if s["lat_ms"] else None
            out[k] = {**{kk: vv for kk, vv in s.items() if kk != "lat_ms"}, "med_ms": med}
        return out

    def _safe_discover(self):
        try:
            return discover_fprintd_devices(self.username)
        except Exception as e:
            print(f"discover_fprintd_devices error: {e}", flush=True)
            return {}

    def _feedback(self, text, kind="ok"):
        self.fb_seq += 1
        self.last_fb = {"text": text, "kind": kind, "seq": self.fb_seq}

    def show(self, phase, action, instruction="", *, finger=None, progress="",
             motion="flat", sensor="dual", winner_sensor=None, sensor_label="",
             enroll_pct=None):
        with self.condition:
            self.state = {
                "phase": phase,
                "action": action,
                "instruction": instruction,
                "finger": finger,
                "progress": progress,
                "motion": motion,
                "running": self.running,
                "sensor": sensor,               # "elan" | "ft9201" | "dual"
                "winnerSensor": winner_sensor,  # "elan" | "ft9201" | None
                "sensorLabel": sensor_label,
                "feedback": self.last_fb,
                "lidState": read_lid_state(),
                "devices": self.devices,
                "stats": self._stats_summary(),
                "enrollPct": enroll_pct,
                "study": dict(self.study_meta),
            }
            self.sequence += 1
            self.condition.notify_all()

    def pause(self, seconds):
        if self.stop_evt.wait(seconds):
            raise Stopped

    def start(self, mode):
        with self.condition:
            if self.running:
                return False
            self.running = True
            self.stop_evt.clear()
            self.study_meta["active"] = False
            self.devices = self._safe_discover()
            if mode in ("ultimate", "elan", "ft9201", "dual"):
                self.stats = self._empty_stats()
            self.last_fb = None
            self.show("prepare", "Initializing sensors…", "Keep both readers clear.", sensor="dual")
        threading.Thread(target=self._run_thread, args=(mode,), daemon=True).start()
        return True

    def start_study_capture(self, profile, mode="clean", sensors="elan,ft9201"):
        if mode not in ("clean", "dirty", "practice"):
            mode = "clean"
        clean_name = "".join(c for c in (profile or "").strip().lower() if c.isalnum() or c in "-_").lstrip("-")
        if not clean_name:
            if mode == "practice":
                clean_name = "practice"
            else:
                return False, "Please enter a profile name (letters/numbers)."
        with self.condition:
            if self.running:
                return False, "A session is already running."
            self.running = True
            self.stop_evt.clear()
            self.last_fb = None
            self.study_meta = {
                "active": True,
                "profile": clean_name,
                "mode": mode,
                "doneTouches": 0,
                "totalTouches": 0,
                "remainingSec": 600,
                "fingerLabel": "",
                "replaying": False,
                "redoSeq": 0,
            }
            self.show(
                "prepare",
                f"Approve System Prompt for [{clean_name}]",
                "Enter your password once (or touch a saved finger) to allow lossless raw sensor capture.",
                sensor="dual",
                sensor_label="Waiting for one-time capture permission…",
            )
        threading.Thread(
            target=self._run_study_capture_thread,
            args=(clean_name, mode, sensors),
            daemon=True,
        ).start()
        return True, "Started"

    def start_study_replay(self):
        with self.condition:
            if self.running:
                return False, "A session is already running."
            self.running = True
            self.study_meta["replaying"] = True
            self.show(
                "prepare",
                "Replaying All Profiles Across 6 Algorithms…",
                "Scoring NBIS Bozorth3, Upstream v0, PressMatch v1, v2, v4.0, and v4.1 on identical raw frames.",
                sensor="dual",
                sensor_label="Running local 16-core C replay…",
            )
        threading.Thread(target=self._run_study_replay_thread, daemon=True).start()
        return True, "Started"

    def _run_study_capture_thread(self, profile, mode, sensors):
        phase, action, desc = "complete", f"Profile [{profile}] Saved!", "Click 'Run 6-Algorithm Replay' below to score all captured profiles."
        uid = os.getuid()
        gid = os.getgid()
        ts_tag = f"{os.getpid()}_{int(time.time() * 1000)}"
        stop_file = Path(tempfile.gettempdir()) / f"pressmatch_stop_{ts_tag}.flag"
        redo_file = Path(tempfile.gettempdir()) / f"pressmatch_redo_{ts_tag}.flag"
        for f in (stop_file, redo_file):
            try:
                if f.exists():
                    f.unlink()
            except OSError:
                pass
        self.active_stop_file = stop_file
        self.active_redo_file = redo_file
        try:
            cmd = [
                "pkexec", sys.executable, str(WORKER_SCRIPT),
                "--profile", profile,
                "--mode", mode,
                "--sensors", sensors,
                "--owner-uid", str(uid),
                "--owner-gid", str(gid),
                "--stop-file", str(stop_file),
                "--redo-file", str(redo_file),
            ]
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            self.active_proc = proc
            if self.stop_evt.is_set():
                self.stop()
            last_done = None
            assert proc.stdout is not None
            for raw_line in proc.stdout:
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    evt = json.loads(line)
                except json.JSONDecodeError:
                    continue
                etype = evt.get("type", "")
                if "doneTouches" in evt:
                    self.study_meta["doneTouches"] = int(evt["doneTouches"])
                if "totalTouches" in evt:
                    self.study_meta["totalTouches"] = int(evt["totalTouches"])
                if "remainingSec" in evt:
                    self.study_meta["remainingSec"] = int(evt["remainingSec"])
                if "fingerLabel" in evt:
                    self.study_meta["fingerLabel"] = str(evt["fingerLabel"])

                sensor = evt.get("sensor", "dual")
                pct = None
                if self.study_meta["totalTouches"] > 0:
                    pct = int(round(100 * self.study_meta["doneTouches"] / self.study_meta["totalTouches"]))

                if etype == "prepare":
                    self.show("prepare", evt.get("headline", "Preparing…"), evt.get("instruction", ""),
                              sensor=sensor, sensor_label="Initializing sensor…", enroll_pct=pct)
                elif etype == "transition":
                    self._feedback("Power Button complete ✓", "ok")
                    self.show("prepare", evt.get("headline", "Switch Sensor"), evt.get("instruction", ""),
                              sensor=sensor, sensor_label=f"Switching to {sensor.upper()}…", enroll_pct=pct)
                elif etype == "warn":
                    self._feedback("Lift finger briefly", "warn")
                    self.show("lift", evt.get("headline", "Lift Your Finger"), evt.get("instruction", ""),
                              sensor=sensor, sensor_label="Calibrating empty sensor…", enroll_pct=pct)
                elif etype in ("block", "touch_wait"):
                    t_i = evt.get("touchIdx", 1)
                    t_n = evt.get("touchTotal", 4)
                    flbl = evt.get("fingerLabel", "")
                    s_pretty = "Power Button (150×52)" if sensor == "elan" else "USB Reader (96×96)"
                    self.show(
                        "touch",
                        f"{evt.get('headline', 'Touch Sensor')}  ({t_i}/{t_n})",
                        evt.get("instruction", ""),
                        progress=f"Profile: {profile} · {flbl} · {self.study_meta['doneTouches']}/{self.study_meta['totalTouches']} total",
                        motion=evt.get("motion", "flat"),
                        sensor=sensor,
                        sensor_label=f"{s_pretty} · Waiting for touch {t_i} of {t_n}…",
                        enroll_pct=pct,
                    )
                elif etype == "captured":
                    fb = evt.get("feedback", "Captured ✓")
                    t_i = evt.get("touchIdx", 1)
                    t_n = evt.get("touchTotal", 4)
                    self._feedback(f"Captured ✓ ({fb})", "ok")
                    self.show(
                        "lift",
                        f"Captured ✓ — Lift Your Finger  ({t_i}/{t_n})",
                        "Lift off cleanly for the next touch…",
                        progress=f"Profile: {profile} · {self.study_meta['doneTouches']}/{self.study_meta['totalTouches']} captured",
                        motion="flat",
                        sensor=sensor,
                        winner_sensor=sensor,
                        sensor_label=f"Saved lossless frame ({fb})",
                        enroll_pct=pct,
                    )
                elif etype == "redo":
                    self.study_meta["redoSeq"] = self.study_meta.get("redoSeq", 0) + 1
                    self._feedback("Repeating last step ↶", "warn")
                    self.show(
                        "prepare",
                        "Redo Last Step",
                        "Previous touch discarded — repeating that step cleanly.",
                        sensor=sensor,
                        sensor_label="Repeating last touch…",
                        enroll_pct=pct,
                    )
                elif etype == "done":
                    last_done = evt

            rc = proc.wait()
            self.active_proc = None
            if self.stop_evt.is_set() or (last_done and last_done.get("status") == "stopped"):
                phase, action, desc = "stopped", "Capture Stopped", f"Saved {self.study_meta['doneTouches']} captured frames for [{profile}]. fprintd restored."
            elif rc == 126 or rc == 127:
                phase, action, desc = "stopped", "Permission Cancelled", "No changes made — click Start Guided Capture whenever you're ready."
            elif last_done and last_done.get("status") == "error":
                phase, action, desc = "error", "Capture Error", last_done.get("error") or "Sensor read failed."
            elif last_done and last_done.get("status") == "timeout":
                phase, action, desc = "complete", f"10:00 Cap Reached — [{profile}] Saved!", f"Saved {self.study_meta['doneTouches']} frames cleanly. Ready to replay!"
            elif rc != 0:
                phase, action, desc = "error", "Capture Interrupted", f"Capture helper exited with code {rc}."
            else:
                el = last_done.get("elapsedSec", 0) if last_done else 0
                phase, action = "complete", f"Profile [{profile}] Complete!"
                desc = f"Captured {self.study_meta['doneTouches']} lossless raw frames in {int(el//60)}:{int(el%60):02d}. Ready for 6-algorithm replay!"
        except Exception as err:
            phase, action, desc = "error", "Capture Error", str(err)
        finally:
            self.active_proc = None
            self.active_stop_file = None
            self.active_redo_file = None
            for f in (stop_file, redo_file):
                try:
                    if f.exists():
                        f.unlink()
                except OSError:
                    pass
            time.sleep(0.4)
            self.devices = self._safe_discover()
            with self.condition:
                self.running = False
                self.study_meta["active"] = False
                self.show(phase, action, desc, sensor="dual")

    def _run_study_replay_thread(self):
        phase, action, desc = "complete", "6-Algorithm Replay Complete!", "Live multi-profile benchmark tables updated below."
        try:
            proc = subprocess.run(
                [sys.executable, str(REPLAY_SCRIPT)],
                capture_output=True,
                text=True,
                timeout=300,
            )
            if proc.returncode != 0:
                phase = "error"
                action = "Replay Error"
                desc = (proc.stderr or proc.stdout or "Replay failed").strip()[-300:]
            else:
                self._feedback("All 6 algorithms scored ✓", "ok")
        except Exception as err:
            phase, action, desc = "error", "Replay Error", str(err)
        finally:
            with self.condition:
                self.running = False
                self.study_meta["replaying"] = False
                self.show(phase, action, desc, sensor="dual")

    def redo_last_touch(self):
        with self.condition:
            if not self.running or not self.study_meta.get("active"):
                return False, "No active capture session."
            if self.study_meta.get("doneTouches", 0) <= 0:
                return False, "No previous touch to redo yet."
        rf = self.active_redo_file
        if rf is not None:
            try:
                rf.write_text(f"{time.time_ns()}\n")
                return True, "Redoing last touch"
            except OSError as err:
                return False, str(err)
        return False, "Redo unavailable."

    def delete_profile(self, profile):
        clean_name = "".join(c for c in (profile or "").strip().lower() if c.isalnum() or c in "-_").lstrip("-")
        if not clean_name:
            return False, "Invalid profile name."
        with self.condition:
            if self.running:
                return False, "Stop the active session before deleting a profile."
        target = (PROFILES_DIR / clean_name).resolve()
        try:
            target.relative_to(PROFILES_DIR.resolve())
        except ValueError:
            return False, "Invalid profile path."
        if not target.exists():
            return False, f"Profile '{clean_name}' not found."
        try:
            shutil.rmtree(target)
        except Exception as err:
            return False, f"Could not delete '{clean_name}': {err}"
        return True, f"Deleted '{clean_name}'."

    def stop(self):
        self.stop_evt.set()
        loop = self.active_loop
        if loop and loop.is_running():
            GLib.idle_add(loop.quit)
        sf = self.active_stop_file
        if sf is not None:
            try:
                sf.write_text("STOP\n")
            except OSError:
                pass
        proc = self.active_proc
        if proc and proc.poll() is None:
            if proc.stdin is not None:
                try:
                    proc.stdin.write("STOP\n")
                    proc.stdin.flush()
                    proc.stdin.close()
                except Exception:
                    pass
            try:
                proc.send_signal(signal.SIGTERM)
            except Exception:
                try:
                    proc.terminate()
                except Exception:
                    pass

    def _run_thread(self, mode):
        phase, action, desc = "complete", "Stress Test Complete!", "Review the live side-by-side results below."
        run_dir = Path(tempfile.mkdtemp(prefix=f"dual-{mode}-", dir=RESULTS_DIR))
        log_path = run_dir / "results.log"
        try:
            with log_path.open("w", encoding="utf-8", buffering=1) as log:
                log.write(f"MODE={mode} USER={self.username} LID={read_lid_state()}\n")
                if mode == "enroll-ft-left":
                    self._enroll_single("ft9201", "left-index-finger", log)
                    self.devices = self._safe_discover()
                    action = "FT9201 Left Index Enrolled!"
                    desc = "Both ELAN and FT9201 now have 2 fingers enrolled for full symmetric testing."
                elif mode == "elan":
                    self._run_single_sensor_phase("elan", log)
                elif mode == "ft9201":
                    self._run_single_sensor_phase("ft9201", log)
                elif mode == "dual":
                    self._run_dual_race_phase(log)
                elif mode == "ultimate":
                    self._run_single_sensor_phase("elan", log)
                    self.pause(1.2)
                    self._run_single_sensor_phase("ft9201", log)
                    self.pause(1.2)
                    self._run_dual_race_phase(log)
                else:
                    raise RuntimeError(f"Unknown mode: {mode}")

            summary = {
                "mode": mode,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "lid": read_lid_state(),
                "devices": self.devices,
                "stats": self._stats_summary(),
            }
            (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))
            log_text = f"Saved to {run_dir.name}."
            if mode != "enroll-ft-left":
                desc = f"All phases finished! {log_text}"
        except Stopped:
            phase, action, desc = "stopped", "Stopped", "Both readers released cleanly."
        except Exception as err:
            print(f"DualStressGuide error: {err}", flush=True)
            phase, action, desc = "error", "Test Paused", str(err)
        finally:
            self.devices = self._safe_discover()
            with self.condition:
                self.running = False
                self.show(phase, action, desc, sensor="dual")

    def _enroll_single(self, sensor_id, finger_dbus, log):
        dev_info = self.devices.get(sensor_id)
        if not dev_info:
            raise RuntimeError(f"Sensor '{sensor_id}' not detected by fprintd.")
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        proxy = Gio.DBusProxy.new_sync(
            bus, Gio.DBusProxyFlags.NONE, None,
            "net.reactivated.Fprint", dev_info["path"],
            "net.reactivated.Fprint.Device", None,
        )
        stages = int(proxy.get_cached_property("num-enroll-stages").unpack())
        proxy.call_sync("Claim", GLib.Variant("(s)", (self.username,)), Gio.DBusCallFlags.NONE, 4000, None)
        loop = GLib.MainLoop()
        self.active_loop = loop
        state = {"done": False, "stage": 0, "err": None}

        def on_sig(_p, _s, sig_name, params):
            if sig_name != "EnrollStatus":
                return
            res, done = params.unpack()
            log.write(f"ENROLL {sensor_id} {finger_dbus}: {res} done={done}\n")
            if res == "enroll-stage-passed":
                state["stage"] += 1
                pct = min(99, int(round(100 * state["stage"] / max(stages, 1))))
                self._feedback(f"Tap {state['stage']}/{stages} saved", "ok")
                self.show("touch", f"FT9201 · Tap {state['stage']}/{stages}",
                          "Shift slightly (~2 mm) and tap your left index on the square FT9201 dongle.",
                          finger="left-index", progress=f"FT9201 Enroll · {pct}%",
                          sensor=sensor_id, sensor_label=f"FT9201 96×96 · Tap {state['stage']}/{stages}",
                          enroll_pct=pct)
            elif res == "enroll-completed":
                state["done"] = True
                self._feedback("Enrollment complete!", "ok")
                loop.quit()
            elif res.startswith("enroll-"):
                self._feedback("Shift slightly & retry", "warn")
                self.show("touch", "Shift slightly",
                          "Move your left index slightly on the FT9201 pad and tap again.",
                          finger="left-index", progress=f"FT9201 Enroll · {state['stage']}/{stages}",
                          sensor=sensor_id, sensor_label="FT9201 96×96 · Retry",
                          enroll_pct=min(99, int(round(100 * state["stage"] / max(stages, 1)))))
                if done:
                    state["err"] = res
                    loop.quit()

        sig_id = proxy.connect("g-signal", on_sig)
        try:
            self.show("touch", "Enroll Left Index on FT9201",
                      f"Tap your left index finger on the square FT9201 dongle ({stages} stages).",
                      finger="left-index", progress=f"FT9201 Enroll · 0/{stages}",
                      sensor=sensor_id, sensor_label="FT9201 96×96 · Armed for enrollment", enroll_pct=0)
            proxy.call_sync("EnrollStart", GLib.Variant("(s)", (finger_dbus,)), Gio.DBusCallFlags.NONE, 4000, None)
            loop.run()
            if self.stop_evt.is_set():
                raise Stopped
            if state["err"]:
                raise RuntimeError(f"Enroll failed: {state['err']}")
        finally:
            self.active_loop = None
            proxy.disconnect(sig_id)
            try:
                proxy.call_sync("EnrollStop", None, Gio.DBusCallFlags.NONE, 2000, None)
            except Exception:
                pass
            try:
                proxy.call_sync("Release", None, Gio.DBusCallFlags.NONE, 2000, None)
            except Exception:
                pass

    def _verify_once(self, dev_ids, timeout_s=25):
        """Claim and arm 1 or 2 fprintd readers simultaneously; return (winner_id, matched, elapsed_ms, raw_status)."""
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        proxies = []
        loop = GLib.MainLoop()
        self.active_loop = loop
        res = {"winner": None, "status": "timeout", "matched": False, "ms": 0.0}
        t0 = None

        def make_handler(sid):
            def _cb(_p, _s, sig_name, params):
                if sig_name != "VerifyStatus" or res["winner"] is not None:
                    return
                status, done = params.unpack()
                if status in ("verify-match", "verify-no-match") or done:
                    res["winner"] = sid
                    res["status"] = status
                    res["matched"] = (status == "verify-match")
                    res["ms"] = (time.monotonic() - t0) * 1000.0 if t0 else 0.0
                    if loop.is_running():
                        loop.quit()
            return _cb

        claimed = []
        started = []
        sigs = []
        try:
            for sid in dev_ids:
                info = self.devices.get(sid)
                if not info or not info["fingers"]:
                    continue
                p = Gio.DBusProxy.new_sync(
                    bus, Gio.DBusProxyFlags.NONE, None,
                    "net.reactivated.Fprint", info["path"],
                    "net.reactivated.Fprint.Device", None,
                )
                p.call_sync("Claim", GLib.Variant("(s)", (self.username,)), Gio.DBusCallFlags.NONE, 4000, None)
                claimed.append((sid, p))
                sigs.append((p, p.connect("g-signal", make_handler(sid))))

            if not claimed:
                raise RuntimeError(f"No enrolled readers available for {dev_ids}.")

            t0 = time.monotonic()
            for sid, p in claimed:
                p.call_sync("VerifyStart", GLib.Variant("(s)", ("any",)), Gio.DBusCallFlags.NONE, 4000, None)
                started.append((sid, p))

            def on_to():
                if loop.is_running():
                    loop.quit()
                return False

            tid = GLib.timeout_add_seconds(timeout_s, on_to)
            loop.run()
            GLib.source_remove(tid)

            if self.stop_evt.is_set():
                raise Stopped
            return res
        finally:
            self.active_loop = None
            for p, sid_sig in sigs:
                try:
                    p.disconnect(sid_sig)
                except Exception:
                    pass
            for _, p in started:
                try:
                    p.call_sync("VerifyStop", None, Gio.DBusCallFlags.NONE, 2000, None)
                except Exception:
                    pass
            for _, p in claimed:
                try:
                    p.call_sync("Release", None, Gio.DBusCallFlags.NONE, 2000, None)
                except Exception:
                    pass

    def _run_single_sensor_phase(self, sensor_id, log):
        info = self.devices.get(sensor_id)
        if not info:
            raise RuntimeError(f"Reader '{sensor_id}' is not connected.")
        if not info["fingers"]:
            raise RuntimeError(f"Reader '{info['title']}' has no enrolled fingers.")

        has_left = "left-index-finger" in info["fingers"]
        checks = STRESS_CHECKS_2F if has_left else STRESS_CHECKS_1F
        s_tag = info["short"]
        dims = f"{info['width']}×{info['height']} {info['shape'].title()}"

        self.show("prepare", f"Phase: {s_tag} ({dims})",
                  f"Testing {info['title']} — {len(checks)} checks. Leave the sensor clear.",
                  sensor=sensor_id, sensor_label=f"{s_tag} {dims} · Starting")
        self.pause(1.4)

        for idx, (finger, expect_match, motion, cue) in enumerate(checks, 1):
            fname = finger.replace("-", " ").title()
            prog = f"{s_tag} Stress · {idx} / {len(checks)}"
            self.show("touch", f"{s_tag}: {fname}", cue,
                      finger=finger, progress=prog, motion=motion,
                      sensor=sensor_id,
                      sensor_label=f"{s_tag} ({dims}) · Armed — touch now")
            r = self._verify_once([sensor_id], timeout_s=30)
            ok_outcome = (r["matched"] == expect_match)

            st = self.stats[sensor_id]
            if expect_match:
                st["gen_n"] += 1
                if r["matched"]:
                    st["gen_ok"] += 1
                    st["lat_ms"].append(r["ms"])
            else:
                st["imp_n"] += 1
                if r["matched"]:
                    st["imp_acc"] += 1

            log.write(f"CHECK sensor={sensor_id} idx={idx}/{len(checks)} finger={finger} "
                      f"expect={'MATCH' if expect_match else 'REJECT'} got={r['status']} "
                      f"ok={int(ok_outcome)} ms={r['ms']:.0f}\n")

            if expect_match and r["matched"]:
                self._feedback(f"{s_tag} MATCH · {r['ms']:.0f} ms", "ok")
            elif (not expect_match) and (not r["matched"]):
                self._feedback(f"{s_tag} Rejected wrong finger ✓", "ok")
            elif expect_match and not r["matched"]:
                self._feedback(f"{s_tag} Missed genuine tap", "warn")
            else:
                self._feedback(f"{s_tag} FALSE ACCEPT!", "error")

            self.show("lift", "Lift finger", "Brief pause before the next check…",
                      finger=finger, progress=prog, motion="flat",
                      sensor=sensor_id, winner_sensor=sensor_id if r["matched"] else None,
                      sensor_label=f"{s_tag} ({dims}) · {r['status']}")
            self.pause(0.75)

    def _run_dual_race_phase(self, log):
        available = [sid for sid in ("elan", "ft9201")
                     if sid in self.devices and self.devices[sid]["fingers"]]
        if len(available) < 2:
            raise RuntimeError("Dual Race requires BOTH ELAN and FT9201 connected with enrolled prints.")

        self.show("prepare", "Phase: Simultaneous Dual-Reader Race",
                  "BOTH readers (ELAN 150×52 + FT9201 96×96) will arm in parallel — touch WHICHEVER you like!",
                  sensor="dual", sensor_label="ELAN + FT9201 · Preparing simultaneous arm")
        self.pause(1.8)

        for idx, (finger, expect_match, motion, cue) in enumerate(DUAL_RACE_CHECKS, 1):
            fname = finger.replace("-", " ").title()
            prog = f"Dual Simultaneous Race · {idx} / {len(DUAL_RACE_CHECKS)}"
            self.show("touch", f"Touch EITHER · {fname}", cue,
                      finger=finger, progress=prog, motion=motion,
                      sensor="dual",
                      sensor_label="BOTH ARMED: ELAN (150×52) + FT9201 (96×96)")
            r = self._verify_once(available, timeout_s=30)
            winner = r["winner"]
            w_name = self.devices[winner]["short"] if winner in self.devices else "?"

            st = self.stats["dual"]
            if expect_match:
                st["gen_n"] += 1
                if r["matched"]:
                    st["gen_ok"] += 1
                    st["lat_ms"].append(r["ms"])
                    if winner == "elan":
                        st["elan_wins"] += 1
                    elif winner == "ft9201":
                        st["ft_wins"] += 1
            else:
                st["imp_n"] += 1
                if r["matched"]:
                    st["imp_acc"] += 1

            log.write(f"DUAL idx={idx}/{len(DUAL_RACE_CHECKS)} finger={finger} "
                      f"expect={'MATCH' if expect_match else 'REJECT'} winner={winner} "
                      f"got={r['status']} ms={r['ms']:.0f} lid={read_lid_state()}\n")

            if expect_match and r["matched"]:
                self._feedback(f"{w_name} WON · {r['ms']:.0f} ms", "ok")
            elif (not expect_match) and (not r["matched"]):
                self._feedback(f"{w_name} rejected wrong finger ✓", "ok")
            elif expect_match and not r["matched"]:
                self._feedback(f"{w_name} missed tap", "warn")
            else:
                self._feedback(f"{w_name} FALSE ACCEPT!", "error")

            self.show("lift", f"{w_name}: {'Matched ✓' if r['matched'] else 'No match'}",
                      "Lift your finger for the next simultaneous check…",
                      finger=finger, progress=prog, motion="flat",
                      sensor="dual", winner_sensor=winner,
                      sensor_label=f"Winner: {w_name} · {r['status']} ({r['ms']:.0f} ms)")
            self.pause(0.85)


def make_handler(guide):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _send_json(self, obj, code=200):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path in ASSETS:
                fname, ctype = ASSETS[path]
                data = (ROOT / fname).read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            if path == "/state":
                self._send_json(guide.state)
                return
            if path == "/profiles":
                self._send_json(scan_profiles())
                return
            if path == "/paper-summary":
                self._send_json(load_paper_summaries())
                return
            if path.startswith("/figures/"):
                fname = Path(path.split("/figures/", 1)[1]).name
                fpath = PAPER_FIGURES_DIR / fname
                if fpath.is_file() and fpath.suffix.lower() == ".png":
                    data = fpath.read_bytes()
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                self.send_error(404)
                return
            if path == "/events":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                seq = -1
                try:
                    while True:
                        with guide.condition:
                            if guide.sequence == seq:
                                guide.condition.wait(timeout=15)
                            seq = guide.sequence
                            payload = json.dumps(guide.state)
                        self.wfile.write(f"data: {payload}\n\n".encode("utf-8"))
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    return
            self.send_error(404)

        def _origin_is_local(self) -> bool:
            sec_fetch_site = (self.headers.get("Sec-Fetch-Site") or "").strip().lower()
            if sec_fetch_site != "same-origin":
                return False
            origin = (self.headers.get("Origin") or "").strip()
            candidate = origin or (self.headers.get("Referer") or "").strip()
            if not candidate:
                return False
            try:
                parsed = urlsplit(candidate)
                port = parsed.port
            except ValueError:
                return False
            return (
                parsed.scheme == "http"
                and parsed.username is None
                and parsed.password is None
                and parsed.hostname in ("127.0.0.1", "localhost")
                and port == self.server.server_port
            )

        def do_POST(self):
            if not self._origin_is_local():
                self._send_json({"ok": False, "message": "Forbidden cross-origin request"}, 403)
                return
            path = urlsplit(self.path).path
            if path == "/stop":
                guide.stop()
                self._send_json({"ok": True})
                return
            if path == "/study/start":
                n = int(self.headers.get("Content-Length", "0") or 0)
                body = json.loads(self.rfile.read(n).decode("utf-8")) if n > 0 else {}
                ok, msg = guide.start_study_capture(
                    body.get("profile", ""),
                    body.get("mode", "clean"),
                    body.get("sensors", "elan,ft9201"),
                )
                self._send_json({"ok": ok, "message": msg}, 200 if ok else 409)
                return
            if path == "/study/replay":
                ok, msg = guide.start_study_replay()
                self._send_json({"ok": ok, "message": msg}, 200 if ok else 409)
                return
            if path == "/study/redo":
                ok, msg = guide.redo_last_touch()
                self._send_json({"ok": ok, "message": msg}, 200 if ok else 409)
                return
            if path == "/profiles/delete":
                n = int(self.headers.get("Content-Length", "0") or 0)
                body = json.loads(self.rfile.read(n).decode("utf-8")) if n > 0 else {}
                ok, msg = guide.delete_profile(body.get("profile", ""))
                self._send_json({"ok": ok, "message": msg, **scan_profiles()}, 200 if ok else 400)
                return
            if path.startswith("/start/"):
                mode = path.split("/start/", 1)[1]
                ok = guide.start(mode)
                self._send_json({"ok": ok}, 200 if ok else 409)
                return
            self.send_error(404)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--user", default=os.environ.get("USER") or os.getlogin())
    args = ap.parse_args()

    guide = DualStressGuide(args.user)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(guide))
    print(f"Dual-Sensor Stress Test Guide live at http://127.0.0.1:{args.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
