#!/usr/bin/env python3
"""
PressMatch Setup — Async D-Bus & Spatial Telemetry Bridge.

Connects cleanly to fprintd over the system D-Bus (zero pkexec popups for
normal enroll/verify), monitors /run/pressmatch/enroll-map.json for live
privacy-safe ElanpressPoseGraph spatial coverage maps, and supports both
single-reader and simultaneous multi-reader verification.
"""
from __future__ import annotations

import getpass
import json
import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from gi.repository import Gio, GLib
from PyQt6.QtCore import QObject, pyqtSignal

TELEMETRY_PATH = Path(
    os.environ.get("PRESSMATCH_ENROLL_MAP_PATH", "/run/pressmatch/enroll-map.json")
)

FINGER_NAMES = {
    "left-thumb": "Left Thumb",
    "left-index-finger": "Left Index",
    "left-middle-finger": "Left Middle",
    "left-ring-finger": "Left Ring",
    "left-little-finger": "Left Pinky",
    "right-thumb": "Right Thumb",
    "right-index-finger": "Right Index",
    "right-middle-finger": "Right Middle",
    "right-ring-finger": "Right Ring",
    "right-little-finger": "Right Pinky",
}


@dataclass
class SensorDevice:
    path: str
    name: str
    sensor_id: str  # "elan", "ft9201", or "generic"
    short_name: str
    subtitle: str
    width: int
    height: int
    shape: str  # "strip", "square", "generic"
    aspect_label: str
    num_enroll_stages: int
    fingers: List[str] = field(default_factory=list)


def read_lid_state() -> str:
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


def classify_device(path: str, name: str, stages: int, fingers: List[str]) -> SensorDevice:
    low = (name or "").lower()
    if "ft9201" in low or "focal" in low:
        return SensorDevice(
            path=path,
            name=name,
            sensor_id="ft9201",
            short_name="USB Reader",
            subtitle="External USB fingerprint reader",
            width=96,
            height=96,
            shape="square",
            aspect_label="External USB",
            num_enroll_stages=stages or 10,
            fingers=list(fingers),
        )
    if "elan" in low:
        return SensorDevice(
            path=path,
            name=name,
            sensor_id="elan",
            short_name="Power Button",
            subtitle="Built-in laptop sensor",
            width=150,
            height=52,
            shape="strip",
            aspect_label="Built-in",
            num_enroll_stages=stages or 12,
            fingers=list(fingers),
        )
    return SensorDevice(
        path=path,
        name=name or "Fingerprint Reader",
        sensor_id="generic",
        short_name="Fingerprint Sensor",
        subtitle="Connected fingerprint reader",
        width=100,
        height=100,
        shape="generic",
        aspect_label="Connected",
        num_enroll_stages=stages or 8,
        fingers=list(fingers),
    )


def get_system_integration_status() -> Dict[str, object]:
    """Read-only inspection of installed PressMatch v4.1 components."""
    lib_installed = Path("/usr/local/lib/pressmatch-v4.1/libfprint-2.so.2.0.0").exists()
    dropin = Path("/etc/systemd/system/fprintd.service.d/elanpress-v4.conf")
    dropin_txt = dropin.read_text() if dropin.exists() else ""
    telemetry_dir = TELEMETRY_PATH.parent.is_dir()
    dual_pam_so = Path("/usr/lib/x86_64-linux-gnu/security/pam_pressmatch_dual.so").exists()
    pam_active = False
    for pf in ("/etc/pam.d/common-auth", "/etc/pam.d/sddm", "/etc/pam.d/kde-fingerprint"):
        p = Path(pf)
        try:
            if p.exists() and "pam_pressmatch_dual.so" in p.read_text():
                pam_active = True
                break
        except OSError:
            pass
    return {
        "driver_active": lib_installed and "pressmatch-v4.1" in dropin_txt,
        "telemetry_ready": telemetry_dir and "RuntimeDirectory=pressmatch" in dropin_txt,
        "dual_pam_installed": dual_pam_so,
        "dual_pam_active": dual_pam_so and pam_active,
        "lid_state": read_lid_state(),
        "user": getpass.getuser(),
    }


def discover_devices(username: Optional[str] = None) -> List[SensorDevice]:
    """Synchronously query fprintd Manager for all connected readers."""
    user = username or getpass.getuser()
    devices: List[SensorDevice] = []
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        mgr = Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "net.reactivated.Fprint",
            "/net/reactivated/Fprint/Manager",
            "net.reactivated.Fprint.Manager",
            None,
        )
        paths = mgr.call_sync("GetDevices", None, Gio.DBusCallFlags.NONE, 4000, None).unpack()[0]
        for path in paths:
            proxy = Gio.DBusProxy.new_sync(
                bus,
                Gio.DBusProxyFlags.NONE,
                None,
                "net.reactivated.Fprint",
                path,
                "net.reactivated.Fprint.Device",
                None,
            )
            name_v = proxy.get_cached_property("name")
            name = name_v.unpack() if name_v else path
            stages_v = proxy.get_cached_property("num-enroll-stages")
            stages = int(stages_v.unpack()) if stages_v else 10
            try:
                fingers = list(
                    proxy.call_sync(
                        "ListEnrolledFingers",
                        GLib.Variant("(s)", (user,)),
                        Gio.DBusCallFlags.NONE,
                        4000,
                        None,
                    ).unpack()[0]
                )
            except Exception:
                fingers = []
            devices.append(classify_device(path, name, stages, fingers))
    except Exception:
        return []
    # Keep deterministic ordering: built-in ELAN first, then FT9201, then others
    order = {"elan": 0, "ft9201": 1, "generic": 2}
    devices.sort(key=lambda d: (order.get(d.sensor_id, 9), d.name))
    return devices


INTERACTIVE_FLAGS = Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION
INTERACTIVE_TIMEOUT_MS = GLib.MAXINT  # Never kill Polkit password prompts!


def ensure_enroll_authorization(
    bus: Gio.DBusConnection,
    cancellable: Optional[Gio.Cancellable] = None,
    on_prompt_needed=None,
) -> tuple[bool, bool]:
    """
    Ensure Polkit authorization for `net.reactivated.fprint.device.enroll`
    BEFORE claiming any fingerprint reader.

    Returns (authorized, user_cancelled).
    Because no sensor is claimed yet, pam_pressmatch_dual never collides with
    fprintd's Claim lock, and GLib.MAXINT ensures the password dialog never
    times out while the user is typing.
    """
    try:
        polkit = Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "org.freedesktop.PolicyKit1",
            "/org/freedesktop/PolicyKit1/Authority",
            "org.freedesktop.PolicyKit1.Authority",
            cancellable,
        )
        subj = ("system-bus-name", {"name": GLib.Variant("s", bus.get_unique_name())})

        # 1. Fast non-interactive check first (handles cached auth_self_keep)
        res = polkit.call_sync(
            "CheckAuthorization",
            GLib.Variant("((sa{sv})sa{ss}us)", (subj, "net.reactivated.fprint.device.enroll", {}, 0, "")),
            Gio.DBusCallFlags.NONE,
            5000,
            cancellable,
        )
        is_auth, is_challenge, _details = res.unpack()[0]
        if is_auth:
            return True, False
        if not is_challenge:
            return False, False

        # 2. Interactive authentication with NO timeout (GLib.MAXINT)
        if on_prompt_needed:
            on_prompt_needed()

        res2 = polkit.call_sync(
            "CheckAuthorization",
            GLib.Variant("((sa{sv})sa{ss}us)", (subj, "net.reactivated.fprint.device.enroll", {}, 1, "")),
            INTERACTIVE_FLAGS,
            INTERACTIVE_TIMEOUT_MS,
            cancellable,
        )
        is_auth2, _is_ch2, details2 = res2.unpack()[0]
        if is_auth2:
            return True, False
        dismissed = str(details2.get("polkit.dismissed", "")).lower() == "true"
        return False, dismissed or True
    except GLib.Error as exc:
        if cancellable and cancellable.is_cancelled():
            return False, True
        # If PolicyKit1 is unavailable for any reason, let fprintd handle auth directly
        if "Cancelled" in str(exc):
            return False, True
        return True, False
    except Exception:
        return True, False


def delete_enrolled_finger(device_path: str, finger: str, username: Optional[str] = None) -> tuple[bool, str]:
    user = username or getpass.getuser()
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        ok_auth, cancelled = ensure_enroll_authorization(bus)
        if not ok_auth:
            return False, "Cancelled" if cancelled else "Authentication required"

        proxy = Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "net.reactivated.Fprint",
            device_path,
            "net.reactivated.Fprint.Device",
            None,
        )
        proxy.call_sync("Claim", GLib.Variant("(s)", (user,)), Gio.DBusCallFlags.NONE, 8000, None)
        try:
            proxy.call_sync(
                "DeleteEnrolledFinger",
                GLib.Variant("(s)", (finger,)),
                INTERACTIVE_FLAGS,
                INTERACTIVE_TIMEOUT_MS,
                None,
            )
        finally:
            try:
                proxy.call_sync("Release", None, Gio.DBusCallFlags.NONE, 3000, None)
            except Exception:
                pass
        return True, "Removed"
    except Exception as exc:
        return False, str(exc)


def delete_all_fingers(device_path: str, username: Optional[str] = None) -> tuple[bool, str]:
    user = username or getpass.getuser()
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        ok_auth, cancelled = ensure_enroll_authorization(bus)
        if not ok_auth:
            return False, "Cancelled" if cancelled else "Authentication required"

        proxy = Gio.DBusProxy.new_sync(
            bus,
            Gio.DBusProxyFlags.NONE,
            None,
            "net.reactivated.Fprint",
            device_path,
            "net.reactivated.Fprint.Device",
            None,
        )
        proxy.call_sync("Claim", GLib.Variant("(s)", (user,)), Gio.DBusCallFlags.NONE, 8000, None)
        try:
            proxy.call_sync(
                "DeleteEnrolledFingers2",
                None,
                INTERACTIVE_FLAGS,
                INTERACTIVE_TIMEOUT_MS,
                None,
            )
        finally:
            try:
                proxy.call_sync("Release", None, Gio.DBusCallFlags.NONE, 3000, None)
            except Exception:
                pass
        return True, "Cleared"
    except Exception as exc:
        return False, str(exc)


def read_telemetry_snapshot() -> Optional[dict]:
    try:
        if not TELEMETRY_PATH.exists():
            return None
        raw = TELEMETRY_PATH.read_text(encoding="utf-8")
        if not raw.strip():
            return None
        return json.loads(raw)
    except Exception:
        return None


class EnrollSession(QObject):
    """Drives one fprintd EnrollStart session + streams live PoseGraph telemetry."""

    authorizing = pyqtSignal()              # Polkit prompt active (no timeout!)
    auth_cancelled = pyqtSignal()           # User cancelled Polkit prompt
    started = pyqtSignal(dict)
    progress = pyqtSignal(dict)             # merged D-Bus + spatial telemetry payload
    finger_present_changed = pyqtSignal(bool)
    completed = pyqtSignal(bool, str, str)  # success, code, message

    def __init__(self, device: SensorDevice, finger: str, username: Optional[str] = None):
        super().__init__()
        self.device = device
        self.finger = finger
        self.username = username or getpass.getuser()
        self._stop_evt = threading.Event()
        self._cancellable = Gio.Cancellable.new()
        self._thread: Optional[threading.Thread] = None
        self._loop: Optional[GLib.MainLoop] = None
        self._proxy: Optional[Gio.DBusProxy] = None

    def start(self) -> None:
        self._stop_evt.clear()
        self._cancellable = Gio.Cancellable.new()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def cancel(self) -> None:
        self._stop_evt.set()
        if self._cancellable:
            self._cancellable.cancel()
        if self._loop and self._loop.is_running():
            GLib.idle_add(self._abort_on_loop)

    def _abort_on_loop(self) -> bool:
        if self._proxy:
            try:
                self._proxy.call_sync("EnrollStop", None, Gio.DBusCallFlags.NONE, 1500, None)
            except Exception:
                pass
        if self._loop and self._loop.is_running():
            self._loop.quit()
        return False

    def _run(self) -> None:
        ctx = GLib.MainContext.new()
        ctx.push_thread_default()
        loop = GLib.MainLoop.new(ctx, False)
        self._loop = loop
        proxy = None
        claimed = False
        sig_id = 0
        prop_id = 0
        poll_src_id = 0
        stage = 0
        total_stages = max(self.device.num_enroll_stages, 1)
        last_seq = -1
        baseline = read_telemetry_snapshot()
        if baseline:
            last_seq = int(baseline.get("seq", -1))

        final_ok = False
        final_code = "failed"
        final_msg = "Enrollment cancelled"
        auth_was_cancelled = False

        try:
            bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, self._cancellable)

            # Pre-authorize with Polkit BEFORE claiming any sensor!
            # Prevents pam_pressmatch_dual from colliding with Claim AND gives the
            # user unlimited time (GLib.MAXINT) to enter their password or touch a sensor.
            ok_auth, was_cancelled = ensure_enroll_authorization(
                bus,
                cancellable=self._cancellable,
                on_prompt_needed=lambda: self.authorizing.emit(),
            )
            if self._stop_evt.is_set() or was_cancelled or not ok_auth:
                auth_was_cancelled = True
                return

            proxy = Gio.DBusProxy.new_sync(
                bus,
                Gio.DBusProxyFlags.NONE,
                None,
                "net.reactivated.Fprint",
                self.device.path,
                "net.reactivated.Fprint.Device",
                self._cancellable,
            )
            self._proxy = proxy

            # Claim device (brief retries if pam_pressmatch_dual just finished releasing)
            for attempt in range(8):
                try:
                    proxy.call_sync(
                        "Claim",
                        GLib.Variant("(s)", (self.username,)),
                        Gio.DBusCallFlags.NONE,
                        8000,
                        self._cancellable,
                    )
                    claimed = True
                    break
                except Exception as exc:
                    if attempt == 7 or self._stop_evt.is_set():
                        raise exc
                    time.sleep(0.35)

            def on_signal(_p, _sender, sig_name, params):
                nonlocal stage, final_ok, final_msg, last_seq
                if sig_name != "EnrollStatus":
                    return
                res, done = params.unpack()
                if res == "enroll-stage-passed":
                    stage = min(stage + 1, total_stages)
                elif res == "enroll-completed":
                    stage = total_stages

                # Read freshest telemetry written right before EnrollStatus was emitted
                telem = read_telemetry_snapshot()
                if telem and int(telem.get("seq", -1)) != last_seq:
                    last_seq = int(telem.get("seq", -1))
                elif telem and telem.get("sensor") != self.device.sensor_id:
                    telem = None

                payload = {
                    "dbus_result": res,
                    "done": bool(done),
                    "stage": stage,
                    "total_stages": total_stages,
                    "sensor": self.device.sensor_id,
                    "width": self.device.width,
                    "height": self.device.height,
                    "telemetry": telem,
                }
                self.progress.emit(payload)

                if done:
                    if res == "enroll-completed":
                        final_ok = True
                        final_code = "completed"
                        final_msg = "Fingerprint enrolled successfully!"
                    elif res == "enroll-duplicate":
                        final_ok = False
                        final_code = "duplicate"
                        final_msg = "This finger is already saved on this sensor."
                    elif res == "enroll-disconnected":
                        final_ok = False
                        final_code = "disconnected"
                        final_msg = "Sensor disconnected during enrollment."
                    elif res == "enroll-data-full":
                        final_ok = False
                        final_code = "data_full"
                        final_msg = "Device storage is full."
                    else:
                        final_ok = False
                        final_code = "failed"
                        final_msg = f"Enrollment stopped ({res})."
                    if not final_ok:
                        import sys
                        print(f"[pressmatch] enroll ended on {self.device.sensor_id}: {res}", file=sys.stderr, flush=True)
                    if loop.is_running():
                        loop.quit()

            def on_props(_p, changed_variant, _invalidated):
                if not changed_variant:
                    return
                changed = changed_variant.unpack()
                if "finger-present" in changed:
                    self.finger_present_changed.emit(bool(changed["finger-present"]))

            def poll_telemetry():
                nonlocal last_seq
                if self._stop_evt.is_set():
                    if loop.is_running():
                        loop.quit()
                    return False
                telem = read_telemetry_snapshot()
                if telem and int(telem.get("seq", -1)) > last_seq:
                    if telem.get("sensor") == self.device.sensor_id:
                        last_seq = int(telem.get("seq", -1))
                        if telem.get("status") == "started":
                            self.started.emit(telem)
                return True

            sig_id = proxy.connect("g-signal", on_signal)
            prop_id = proxy.connect("g-properties-changed", on_props)
            poll_src_id = GLib.timeout_add(60, poll_telemetry)

            proxy.call_sync(
                "EnrollStart",
                GLib.Variant("(s)", (self.finger,)),
                INTERACTIVE_FLAGS,
                INTERACTIVE_TIMEOUT_MS,
                self._cancellable,
            )
            self.started.emit(
                {
                    "sensor": self.device.sensor_id,
                    "width": self.device.width,
                    "height": self.device.height,
                    "stage": 0,
                    "total_stages": total_stages,
                }
            )
            loop.run()
        except Exception as exc:
            if self._stop_evt.is_set() or (self._cancellable and self._cancellable.is_cancelled()):
                auth_was_cancelled = True
            else:
                import sys
                print(f"[pressmatch] enroll exception on {self.device.sensor_id}: {exc}", file=sys.stderr, flush=True)
                final_ok = False
                low = str(exc).lower()
                if "alreadyinuse" in low or "already claimed" in low:
                    final_code = "busy"
                elif "notauthorized" in low or "not authorized" in low:
                    auth_was_cancelled = True
                else:
                    final_code = "failed"
                final_msg = f"Could not start enrollment: {exc}"
        finally:
            if poll_src_id:
                try:
                    GLib.source_remove(poll_src_id)
                except Exception:
                    pass
            if proxy:
                if sig_id:
                    try:
                        proxy.disconnect(sig_id)
                    except Exception:
                        pass
                if prop_id:
                    try:
                        proxy.disconnect(prop_id)
                    except Exception:
                        pass
                try:
                    proxy.call_sync("EnrollStop", None, Gio.DBusCallFlags.NONE, 1500, None)
                except Exception:
                    pass
                if claimed:
                    try:
                        proxy.call_sync("Release", None, Gio.DBusCallFlags.NONE, 2000, None)
                    except Exception:
                        pass
            self._proxy = None
            self._loop = None
            ctx.pop_thread_default()

        if not self._stop_evt.is_set():
            if auth_was_cancelled:
                self.auth_cancelled.emit()
            else:
                self.completed.emit(final_ok, final_code, final_msg)


class SimultaneousVerifySession(QObject):
    """Arms one OR ALL enrolled sensors simultaneously for instant unlock testing."""

    armed = pyqtSignal(list)             # list of SensorDevice armed
    finger_present = pyqtSignal(str, bool)
    result = pyqtSignal(dict)            # {matched, raw_result, device, elapsed_ms}
    error = pyqtSignal(str)

    def __init__(
        self,
        devices: List[SensorDevice],
        username: Optional[str] = None,
        sensor_filter: Optional[str] = None,  # None = all enrolled, or "elan" / "ft9201"
    ):
        super().__init__()
        self.devices = [d for d in devices if d.fingers]
        self.sensor_filter = sensor_filter
        self.username = username or getpass.getuser()
        self._stop_evt = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._loop: Optional[GLib.MainLoop] = None
        self._ctx: Optional[GLib.MainContext] = None

    def start(self) -> None:
        self._stop_evt.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self, wait: bool = False) -> None:
        self._stop_evt.set()
        loop = self._loop
        ctx = self._ctx
        if loop and loop.is_running():
            if ctx is not None:
                def _quit():
                    if loop.is_running():
                        loop.quit()
                    return False
                src = GLib.Idle()
                src.set_callback(_quit)
                src.attach(ctx)
            else:
                GLib.idle_add(loop.quit)
        if wait and self._thread and self._thread.is_alive() and self._thread is not threading.current_thread():
            self._thread.join(timeout=2.0)

    def _run(self) -> None:
        ctx = GLib.MainContext.new()
        ctx.push_thread_default()
        loop = GLib.MainLoop.new(ctx, False)
        self._ctx = ctx
        self._loop = loop

        handles = []
        t0 = time.monotonic()
        done_flag = {"done": False}
        watch_id = 0
        mgr_proxy = None
        mgr_sig_id = 0

        try:
            # Always query fresh live devices so USB re-enumeration (e.g. ELAN /Device/N)
            # never leaves a stale object path behind!
            fresh_devs = [d for d in discover_devices(self.username) if d.fingers]
            if self.sensor_filter:
                targets = [d for d in fresh_devs if d.sensor_id == self.sensor_filter]
            else:
                targets = list(fresh_devs)

            bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            mgr_proxy = Gio.DBusProxy.new_sync(
                bus,
                Gio.DBusProxyFlags.NONE,
                None,
                "net.reactivated.Fprint",
                "/net/reactivated/Fprint/Manager",
                "net.reactivated.Fprint.Manager",
                None,
            )

            lid = read_lid_state()
            if not self.sensor_filter and lid == "closed" and len(targets) > 1:
                non_elan = [d for d in targets if d.sensor_id != "elan"]
                if non_elan:
                    targets = non_elan

            def arm_one(dev: SensorDevice) -> bool:
                if any(h["dev"].path == dev.path for h in handles):
                    return False
                proxy = None
                claimed = False
                try:
                    proxy = Gio.DBusProxy.new_sync(
                        bus,
                        Gio.DBusProxyFlags.NONE,
                        None,
                        "net.reactivated.Fprint",
                        dev.path,
                        "net.reactivated.Fprint.Device",
                        None,
                    )
                    # Brief retries in case the previous verify/enroll just released ELAN
                    for attempt in range(5):
                        if self._stop_evt.is_set():
                            return False
                        try:
                            proxy.call_sync(
                                "Claim",
                                GLib.Variant("(s)", (self.username,)),
                                Gio.DBusCallFlags.NONE,
                                3500,
                                None,
                            )
                            claimed = True
                            break
                        except Exception:
                            if attempt == 4:
                                return False
                            time.sleep(0.22)

                    entry = {"dev": dev, "proxy": proxy, "sig_id": 0, "prop_id": 0}

                    def make_sig_cb(d_obj):
                        def _cb(_p, _s, sig_name, params):
                            if sig_name != "VerifyStatus" or done_flag["done"]:
                                return
                            res, is_done = params.unpack()
                            if is_done:
                                done_flag["done"] = True
                                elapsed_ms = int((time.monotonic() - t0) * 1000)
                                self.result.emit(
                                    {
                                        "matched": res == "verify-match",
                                        "raw_result": res,
                                        "device": d_obj,
                                        "elapsed_ms": elapsed_ms,
                                    }
                                )
                                if loop.is_running():
                                    loop.quit()
                        return _cb

                    def make_prop_cb(d_obj):
                        def _pcb(_p, changed_v, _inv):
                            if not changed_v:
                                return
                            ch = changed_v.unpack()
                            if "finger-present" in ch:
                                self.finger_present.emit(d_obj.sensor_id, bool(ch["finger-present"]))
                        return _pcb

                    entry["sig_id"] = proxy.connect("g-signal", make_sig_cb(dev))
                    entry["prop_id"] = proxy.connect("g-properties-changed", make_prop_cb(dev))
                    proxy.call_sync(
                        "VerifyStart",
                        GLib.Variant("(s)", ("any",)),
                        Gio.DBusCallFlags.NONE,
                        4000,
                        None,
                    )
                    handles.append(entry)
                    return True
                except Exception:
                    if proxy and claimed:
                        try:
                            proxy.call_sync("Release", None, Gio.DBusCallFlags.NONE, 1500, None)
                        except Exception:
                            pass
                    return False

            for dev in targets:
                arm_one(dev)

            # Hotplug listener: if ELAN finishes USB re-enumeration 1s after Try Mode opens,
            # automatically claim & arm it live and update the UI badges!
            def on_mgr_signal(_p, _sender, sig_name, params):
                if sig_name != "DeviceAdded" or done_flag["done"] or self._stop_evt.is_set():
                    return
                try:
                    new_path = params.unpack()[0]
                    for d in discover_devices(self.username):
                        if d.path == new_path and d.fingers:
                            if self.sensor_filter and d.sensor_id != self.sensor_filter:
                                continue
                            if not self.sensor_filter and read_lid_state() == "closed" and d.sensor_id == "elan":
                                continue
                            if arm_one(d):
                                self.armed.emit([h["dev"] for h in handles])
                except Exception:
                    pass

            mgr_sig_id = mgr_proxy.connect("g-signal", on_mgr_signal)

            if not handles:
                # Wait up to 3.5 s in small steps in case ELAN is mid-USB-re-enumeration
                # right after a previous VerifyStop/Release!
                for _ in range(7):
                    if self._stop_evt.is_set() or handles:
                        break
                    time.sleep(0.5)
                    for d in [x for x in discover_devices(self.username) if x.fingers]:
                        if self.sensor_filter and d.sensor_id != self.sensor_filter:
                            continue
                        arm_one(d)

            if not handles:
                self.error.emit("No enrolled fingerprints found or sensor busy.")
                return

            self.armed.emit([h["dev"] for h in handles])

            def check_stop():
                if self._stop_evt.is_set() and loop.is_running():
                    loop.quit()
                    return False
                return True

            src = GLib.timeout_source_new(100)
            src.set_callback(check_stop)
            watch_id = src.attach(ctx)
            loop.run()
        except Exception as exc:
            if not self._stop_evt.is_set():
                self.error.emit(str(exc))
        finally:
            if watch_id:
                try:
                    s = ctx.find_source_by_id(watch_id)
                    if s:
                        s.destroy()
                except Exception:
                    pass
            if mgr_proxy and mgr_sig_id:
                try:
                    mgr_proxy.disconnect(mgr_sig_id)
                except Exception:
                    pass
            for h in handles:
                p = h["proxy"]
                for sid in (h["sig_id"], h["prop_id"]):
                    if sid:
                        try:
                            p.disconnect(sid)
                        except Exception:
                            pass
                try:
                    p.call_sync("VerifyStop", None, Gio.DBusCallFlags.NONE, 1500, None)
                except Exception:
                    pass
                try:
                    p.call_sync("Release", None, Gio.DBusCallFlags.NONE, 1500, None)
                except Exception:
                    pass
            self._loop = None
            self._ctx = None
            ctx.pop_thread_default()
