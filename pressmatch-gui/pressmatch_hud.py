#!/usr/bin/env python3
"""
PressMatch OLED G3 Corner Fingerprint HUD for Linux (KDE Plasma 6 Wayland).

Listens on Unix DGRAM socket /run/user/<uid>/pressmatch-hud.sock for live PAM events
from pam_pressmatch_dual.so (armed, scanning, partial, retry, declined, match, ended)
and renders a pure-black (#000000) CAD G3 corner fillet anchored flush to the
bottom-right edge of the display via Wayland wlr-layer-shell (LayerOverlay),
complete with an Apple Dynamic Island-style adaptive specular rim & outer-only
OLED halo, animated Touch ID glyph, curved inner-fillet typography, and visual
pressure/posture animations.

Zero CPU when hidden; 60 FPS vector QPainter animation only while visible.

Usage:
  pressmatch_hud.py                        # Run background HUD daemon
  pressmatch_hud.py --demo                 # Cycle through all states live on screen
  pressmatch_hud.py --render-all /path/dir # Render PNG previews of every state
"""

from __future__ import annotations

import ctypes
import json
import math
import os
import signal
import socket
import sys
import time
from pathlib import Path

# Force Wayland layer-shell integration when running on Wayland
if os.environ.get("WAYLAND_DISPLAY") and "QT_WAYLAND_SHELL_INTEGRATION" not in os.environ:
    os.environ["QT_WAYLAND_SHELL_INTEGRATION"] = "layer-shell"

from PyQt6.QtCore import (
    QMargins,
    QPointF,
    QRectF,
    QSocketNotifier,
    Qt,
    QTimer,
    pyqtSlot,
)
from PyQt6.QtDBus import QDBusConnection, QDBusInterface
from PyQt6.QtGui import (
    QColor,
    QFont,
    QFontMetricsF,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
)
from PyQt6.QtWidgets import QApplication, QWidget
from PyQt6 import sip

from success_chime import SuccessChime


# ── Bilingual DE / EN labels (auto-detected from Linux locale) ────────────────

def _is_german() -> bool:
    for var in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        val = (os.environ.get(var) or "").lower()
        if val:
            return val.startswith("de")
    return False


IS_DE = _is_german()

TEXTS = {
    "armed": ("Zum Entsperren berühren", "Touch to unlock"),
    "scanning": ("Wird gelesen…", "Reading…"),
    "match": ("Bestätigt", "Verified"),
    "unlocked": ("Entsperrt", "Unlocked"),
    "retry": ("Nochmal versuchen", "Try again"),
    "partial": ("Fester drücken", "Press a little firmer"),
    "declined": ("Passwort verwenden", "Use your password"),
    "too_light": ("Fester drücken", "Press a little firmer"),
    "too_hard": ("Sanfter drücken", "Press more gently"),
    "off_center": ("Finger zentrieren", "Center your finger"),
    "moving": ("Ruhig halten", "Hold still until it reacts"),
    "dry": ("Finger & Sensor abwischen", "Wipe finger and sensor"),
    "wrong_area": ("Finger leicht bewegen", "Move finger slightly"),
}


def tr(key: str) -> str:
    de, en = TEXTS[key]
    return de if IS_DE else en


def classify_hint(hint: str, fallback_state: str = "") -> str:
    """Map PAM hint wording to a visual guidance animation kind."""
    low = (hint or "").lower()
    if "firmer" in low or "light" in low or "cover" in low or "fester" in low:
        return "too_light"
    if "gently" in low or "hard" in low or "sanfter" in low:
        return "too_hard"
    if "center" in low or "zentrier" in low:
        return "off_center"
    if "still" in low or "moving" in low or "ruhig" in low:
        return "moving"
    if "wipe" in low or "dry" in low or "wisch" in low:
        return "dry"
    if "setup" in low or "move" in low or "slightly" in low or "roll" in low:
        return "wrong_area"
    if fallback_state == "partial":
        return "too_light"
    return ""


# ── Exact CAD G3 Corner Fillet Math (degree-7 Bézier, K = 0.50) ───────────────

BINOM7 = (1, 7, 21, 35, 35, 21, 7, 1)
G3_K = 0.50


def g3_control_points(w: float, h: float, L: float) -> list[tuple[float, float]]:
    """P0..P3 on bottom edge y=h, P4..P7 on right edge x=w -> κ=0, dκ/ds=0 at ends."""
    step = (L - G3_K * L) / 3.0
    pts: list[tuple[float, float]] = []
    for i in range(4):
        pts.append((w - (L - i * step), h))
    for i in range(3, -1, -1):
        pts.append((w, h - (L - i * step)))
    return pts


def g3_eval(pts: list[tuple[float, float]], t: float) -> tuple[float, float]:
    mt = 1.0 - t
    x = 0.0
    y = 0.0
    for i in range(8):
        b = BINOM7[i] * (mt ** (7 - i)) * (t ** i)
        x += b * pts[i][0]
        y += b * pts[i][1]
    return (x, y)


def g3_sample(w: float, h: float, L: float, n: int = 120) -> list[tuple[float, float]]:
    pts = g3_control_points(w, h, L)
    return [g3_eval(pts, i / n) for i in range(n + 1)]


# ── Native Wayland Layer-Shell binding via libLayerShellQtInterface.so.6 ──────

class _LayerShellBridge:
    """Configure a PyQt6 QWindow as a click-through bottom-right Wayland LayerOverlay."""

    LAYER_OVERLAY = 3
    ANCHOR_TOP = 1
    ANCHOR_BOTTOM = 2
    ANCHOR_LEFT = 4
    ANCHOR_RIGHT = 8
    KB_NONE = 0

    def __init__(self) -> None:
        self._lib = None
        try:
            self._lib = ctypes.CDLL("libLayerShellQtInterface.so.6")
            self._get = self._lib._ZN12LayerShellQt6Window3getEP7QWindow
            self._get.argtypes = [ctypes.c_void_p]
            self._get.restype = ctypes.c_void_p

            self._set_layer = self._lib._ZN12LayerShellQt6Window8setLayerENS0_5LayerE
            self._set_layer.argtypes = [ctypes.c_void_p, ctypes.c_int]

            self._set_anchors = self._lib._ZN12LayerShellQt6Window10setAnchorsE6QFlagsINS0_6AnchorEE
            self._set_anchors.argtypes = [ctypes.c_void_p, ctypes.c_int]

            self._set_kb = self._lib._ZN12LayerShellQt6Window24setKeyboardInteractivityENS0_21KeyboardInteractivityE
            self._set_kb.argtypes = [ctypes.c_void_p, ctypes.c_int]

            self._set_excl = self._lib._ZN12LayerShellQt6Window16setExclusiveZoneEi
            self._set_excl.argtypes = [ctypes.c_void_p, ctypes.c_int]

            self._set_margins = self._lib._ZN12LayerShellQt6Window10setMarginsERK8QMargins
            self._set_margins.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

            try:
                self._set_activate = self._lib._ZN12LayerShellQt6Window17setActivateOnShowEb
                self._set_activate.argtypes = [ctypes.c_void_p, ctypes.c_bool]
            except AttributeError:
                self._set_activate = None
        except Exception:
            self._lib = None

    def configure_bottom_right(self, widget: QWidget) -> bool:
        if not self._lib or not os.environ.get("WAYLAND_DISPLAY"):
            return False
        widget.winId()
        qwin = widget.windowHandle()
        if qwin is None:
            return False
        qwin_ptr = sip.unwrapinstance(qwin)
        lwin = self._get(qwin_ptr)
        if not lwin:
            return False
        self._set_layer(lwin, self.LAYER_OVERLAY)
        self._set_anchors(lwin, self.ANCHOR_BOTTOM | self.ANCHOR_RIGHT)
        self._set_kb(lwin, self.KB_NONE)
        self._set_excl(lwin, -1)  # ignore panels/taskbars so corner sits truly flush on bezel
        margins = QMargins(0, 0, 0, 0)
        self._set_margins(lwin, sip.unwrapinstance(margins))
        if self._set_activate:
            self._set_activate(lwin, False)
        return True


# ── Palette per state (OLED rim keyline, outer bloom, label & glyph tint) ─────

PALETTES = {
    "armed": {
        "rim": QColor("#dce6f7"),
        "rim_peak": QColor("#ffffff"),
        "bloom": QColor("#8fbaff"),
        "bloom_peak": QColor("#d8e9ff"),
        "label": QColor(235, 235, 245, 185),
        "glyph": QColor(255, 255, 255, 228),
    },
    "scanning": {
        "rim": QColor("#5ac8fa"),
        "rim_peak": QColor("#d8f4ff"),
        "bloom": QColor("#0a84ff"),
        "bloom_peak": QColor("#64d2ff"),
        "label": QColor("#7cc8ff"),
        "glyph": QColor("#0a84ff"),
    },
    "wrong_area": {
        "rim": QColor("#64d2ff"),
        "rim_peak": QColor("#e6f9ff"),
        "bloom": QColor("#0a84ff"),
        "bloom_peak": QColor("#64d2ff"),
        "label": QColor("#64d2ff"),
        "glyph": QColor("#64d2ff"),
    },
    "match": {
        "rim": QColor("#4cd964"),
        "rim_peak": QColor("#d9ffe2"),
        "bloom": QColor("#30d158"),
        "bloom_peak": QColor("#7dff9b"),
        "label": QColor("#30d158"),
        "glyph": QColor("#30d158"),
    },
    "amber": {
        "rim": QColor("#ffb340"),
        "rim_peak": QColor("#ffe8bf"),
        "bloom": QColor("#ff9f0a"),
        "bloom_peak": QColor("#ffd080"),
        "label": QColor("#ffb340"),
        "glyph": QColor("#ff9f0a"),
    },
    "coral": {
        "rim": QColor("#ff6961"),
        "rim_peak": QColor("#ffd4d1"),
        "bloom": QColor("#ff453a"),
        "bloom_peak": QColor("#ff8c85"),
        "label": QColor("#ff6961"),
        "glyph": QColor("#ff5e4d"),
    },
}


# ── Custom OLED G3 Corner Sensor Widget ───────────────────────────────────────

class PressMatchHUD(QWidget):
    L_MAX = 360.0       # G3 fillet extent along right & bottom edges (px)
    BLOOM_PAD = 28      # Extra transparent margin on top/left for outer OLED bloom

    def __init__(self) -> None:
        super().__init__(None)
        self._layer_bridge = _LayerShellBridge()
        self._layer_configured = False

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowDoesNotAcceptFocus
            | Qt.WindowType.WindowTransparentForInput
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

        size = int(self.L_MAX) + self.BLOOM_PAD
        self.setFixedSize(size, size)

        # Visual state
        self._state = "hidden"  # hidden | armed | scanning | partial | retry | declined | match
        self._hint_kind = ""    # "" | too_light | too_hard | off_center | moving | dry | wrong_area
        self._label = tr("armed")
        self._readers = ["ELAN", "FT9201"]

        # Mechanical drive state (L in [0..L_MAX])
        self._L = 0.0
        self._drive_from = 0.0
        self._drive_to = 0.0
        self._drive_t0 = 0.0
        self._drive_dur = 0.72
        self._driving = False
        self._hiding = False
        self._content_alpha = 0.0

        # Lockscreen shadow pre-staging: while KWin's ext-session-lock-v1 sits
        # above our LayerOverlay, we keep our surface pre-mapped & committed at
        # L_MAX (0% CPU, timer paused) so the compositor has our buffer ready on
        # the exact frame the lockscreen closes — zero blank frames on unlock.
        self._screen_locked = False
        self._shadow_locked = False
        self._awaiting_unlock_dwell = False
        self._connect_screensaver_dbus()

        # Specular glint sweep state (0..1)
        self._glint_t0 = -10.0
        self._glint_dur = 0.92

        # Animation clock
        self._phase = 0.0
        self._state_t0 = time.monotonic()

        self._anim_timer = QTimer(self)
        self._anim_timer.setInterval(16)
        self._anim_timer.timeout.connect(self._on_tick)

        # Auto-dismiss / revert / watchdog timers
        self._dismiss_timer = QTimer(self)
        self._dismiss_timer.setSingleShot(True)
        self._dismiss_timer.timeout.connect(self.hide_smooth)

        self._revert_timer = QTimer(self)
        self._revert_timer.setSingleShot(True)
        self._revert_timer.timeout.connect(self._revert_to_armed)

        self._watchdog_timer = QTimer(self)
        self._watchdog_timer.setSingleShot(True)
        self._watchdog_timer.timeout.connect(self.hide_smooth)

        # Permanently map the click-through Wayland LayerOverlay surface once at
        # startup (painting 100% transparent when idle, timer stopped -> 0% CPU).
        # Never calling hide() avoids destroying the zwlr_layer_surface_v1 and
        # completely eliminates the Wayland configure / KWin open-fade gap on
        # lockscreen -> desktop unlock transitions.
        self._ensure_positioned()
        self.show()

    def _ensure_positioned(self) -> None:
        if not self._layer_configured:
            self._layer_configured = self._layer_bridge.configure_bottom_right(self)
        if not self._layer_configured:
            scr = QApplication.primaryScreen()
            if scr:
                g = scr.geometry()
                self.move(g.x() + g.width() - self.width(), g.y() + g.height() - self.height())

    def _start_drive(self, target: float, duration_s: float) -> None:
        self._drive_from = self._L
        self._drive_to = target
        self._drive_t0 = time.monotonic()
        self._drive_dur = duration_s
        self._driving = True
        if not self._anim_timer.isActive():
            self._anim_timer.start()

    def _trigger_glint(self) -> None:
        self._glint_t0 = time.monotonic()

    def _on_tick(self) -> None:
        now = time.monotonic()
        self._phase += 0.016

        if self._driving:
            t = min(1.0, (now - self._drive_t0) / self._drive_dur)
            extending = self._drive_to > self._drive_from
            if extending:
                # Under-damped mechanical spring (~3% overshoot, settles cleanly at t=1)
                if t >= 1.0:
                    e = 1.0
                else:
                    w = 11.2
                    z = 0.64
                    wd = w * math.sqrt(1.0 - z * z)
                    e = 1.0 - math.exp(-z * w * t) * (
                        math.cos(wd * t) + (z / math.sqrt(1.0 - z * z)) * math.sin(wd * t)
                    )
                self._L = self._drive_from + (self._drive_to - self._drive_from) * e
                # Fade glyph & text in once the pocket opens past 65%
                self._content_alpha = max(0.0, min(1.0, (t - 0.35) / 0.45))
            else:
                e = t * t * t
                self._L = self._drive_from + (self._drive_to - self._drive_from) * e
                self._content_alpha = max(0.0, min(1.0, 1.0 - t * 2.2))

            if t >= 1.0:
                self._driving = False
                self._L = self._drive_to
                if extending:
                    self._content_alpha = 1.0
                    self._trigger_glint()
                elif self._hiding and self._L <= 0.5:
                    self._state = "hidden"
                    self._hiding = False
                    self._L = 0.0
                    self._content_alpha = 0.0
                    self._anim_timer.stop()
                    # Keep LayerOverlay surface permanently mapped (transparent, 0% CPU)
                    # so KWin never has to re-create/configure it on the next transition.
                    self.repaint()
                    return
        elif not self._hiding and self._L >= self.L_MAX * 0.95:
            self._content_alpha = min(1.0, self._content_alpha + 0.08)

        self.update()

    def _connect_screensaver_dbus(self) -> None:
        try:
            bus = QDBusConnection.sessionBus()
            if not bus.isConnected():
                return
            bus.connect(
                "org.freedesktop.ScreenSaver",
                "/ScreenSaver",
                "org.freedesktop.ScreenSaver",
                "ActiveChanged",
                self._on_screensaver_active_changed,
            )
            iface = QDBusInterface(
                "org.freedesktop.ScreenSaver",
                "/ScreenSaver",
                "org.freedesktop.ScreenSaver",
                bus,
            )
            if iface.isValid():
                reply = iface.call("GetActive")
                if reply.arguments():
                    self._screen_locked = bool(reply.arguments()[0])
        except Exception:
            pass

    @pyqtSlot(bool)
    def _on_screensaver_active_changed(self, active: bool) -> None:
        self._screen_locked = bool(active)
        exp = bool(self.windowHandle() and self.windowHandle().isExposed())
        print(f"[hud] screensaver active={active} shadow={self._shadow_locked} state={self._state} L={self._L:.0f} exposed={exp}", flush=True)
        if not active:
            if self._shadow_locked:
                # User unlocked via password (not fingerprint match): smoothly retract
                # the pre-staged shadow corner so it never lingers on the desktop.
                self._shadow_locked = False
                self._awaiting_unlock_dwell = False
                self.hide_smooth()
            elif self._awaiting_unlock_dwell and self._state == "match":
                # Desktop is now visible: start the normal post-unlock dwell so
                # swap pressure behind the greeter never eats the confirmation.
                self._awaiting_unlock_dwell = False
                self._dismiss_timer.start(950)

    def _stage_behind_lockscreen(self) -> None:
        """Pre-map and commit our Wayland LayerOverlay at L_MAX behind the active
        lockscreen with the animation timer stopped (zero CPU, zero blank frames)."""
        self._shadow_locked = True
        self._hiding = False
        self._driving = False
        self._L = self.L_MAX
        self._content_alpha = 1.0
        self._ensure_positioned()
        if not self.isVisible():
            self.show()
        # Keep timer stopped while hidden behind lockscreen -> 0% CPU while locked
        if self._anim_timer.isActive():
            self._anim_timer.stop()
        # Synchronous paint forces an immediate Wayland buffer commit into KWin
        self.repaint()

    def show_smooth(self, instant: bool = False) -> None:
        self._shadow_locked = False
        self._hiding = False
        self._ensure_positioned()
        already_opening = self._driving and self._drive_to >= self.L_MAX * 0.9
        already_open = self._L >= self.L_MAX * 0.95 and not self._driving
        if instant:
            # Seamless handoff (e.g. lockscreen -> desktop): surface is already
            # mapped & configured in KWin, so paint immediately at L_MAX.
            self._driving = False
            self._L = self.L_MAX
            self._content_alpha = 1.0
            if not self.isVisible():
                self.show()
            if not self._anim_timer.isActive():
                self._anim_timer.start()
            self.repaint()
            return
        if not self.isVisible():
            self.show()
        if not self._anim_timer.isActive():
            self._anim_timer.start()
        if not already_open and not already_opening:
            self._start_drive(self.L_MAX, 0.72)

    def hide_smooth(self) -> None:
        self._shadow_locked = False
        self._dismiss_timer.stop()
        self._revert_timer.stop()
        self._watchdog_timer.stop()
        if self._L <= 0.5 or self._hiding:
            return
        self._hiding = True
        self._start_drive(0.0, 0.42)

    def _revert_to_armed(self) -> None:
        if self._state in ("retry", "partial"):
            self._state = "armed"
            self._hint_kind = ""
            self._state_t0 = time.monotonic()
            self._label = tr("armed")
            self.update()

    def handle_event(self, ev: dict) -> None:
        state = ev.get("state", "")
        service = ev.get("service", "")
        # Match KDE's lockscreen PAM service explicitly so desktop services like
        # sudo-pressmatch-fingerprint never enter the lockscreen shadow path.
        is_lockscreen = (service == "kde-fingerprint")
        exp = bool(self.windowHandle() and self.windowHandle().isExposed())
        print(f"[hud] event state={state} service={service} L={self._L:.0f} shadow={self._shadow_locked} exposed={exp}", flush=True)

        self._awaiting_unlock_dwell = False
        self._dismiss_timer.stop()
        self._revert_timer.stop()

        # While locked (kde-fingerprint), pre-stage and mirror our Wayland buffer
        # behind KWin's lock surface so it is ALREADY mapped, configured and
        # painted at L_MAX on the exact compositor frame the lockscreen exits.
        if is_lockscreen and state != "match":
            if state == "armed":
                readers = ev.get("readers") or ["ELAN", "FT9201"]
                self._readers = [str(r) for r in readers]
                self._state = "armed"
                self._hint_kind = ""
                self._state_t0 = time.monotonic()
                self._label = tr("armed")
                self._watchdog_timer.stop()
                self._stage_behind_lockscreen()
            elif state == "scanning":
                self._state = "scanning"
                self._hint_kind = ""
                self._state_t0 = time.monotonic()
                self._label = tr("scanning")
                self._stage_behind_lockscreen()
            elif state == "partial":
                hint = str(ev.get("hint") or "")
                self._state = "partial"
                self._hint_kind = classify_hint(hint, "partial")
                self._state_t0 = time.monotonic()
                self._label = tr(self._hint_kind) if self._hint_kind in TEXTS else (hint or tr("partial"))
                self._stage_behind_lockscreen()
            elif state == "retry":
                hint = str(ev.get("hint") or "")
                self._state = "retry"
                self._hint_kind = classify_hint(hint, "retry")
                self._state_t0 = time.monotonic()
                self._label = tr(self._hint_kind) if self._hint_kind in TEXTS else (hint or tr("retry"))
                self._stage_behind_lockscreen()
            elif state == "declined":
                self._state = "declined"
                self._hint_kind = ""
                self._state_t0 = time.monotonic()
                self._label = tr("declined")
                self._stage_behind_lockscreen()
            elif state == "ended":
                if not self._screen_locked and self._shadow_locked:
                    self._shadow_locked = False
                    self.hide_smooth()
            return

        if state == "armed":
            readers = ev.get("readers") or ["ELAN", "FT9201"]
            self._readers = [str(r) for r in readers]
            self._state = "armed"
            self._hint_kind = ""
            self._state_t0 = time.monotonic()
            self._label = tr("armed")
            self._watchdog_timer.start(10 * 60 * 1000)
            self.show_smooth()

        elif state == "scanning":
            self._state = "scanning"
            self._hint_kind = ""
            self._state_t0 = time.monotonic()
            self._label = tr("scanning")
            self._watchdog_timer.start(10000)
            self.show_smooth()

        elif state == "match":
            is_lock_handoff = is_lockscreen
            self._state = "match"
            self._hint_kind = ""
            self._state_t0 = time.monotonic()
            self._label = tr("unlocked") if is_lock_handoff else tr("match")
            self._watchdog_timer.stop()
            # On lockscreen unlock handoff, PressMatchPill.qml is already extended
            # at L_MAX in the exact same corner — stay static instead of re-entering.
            self.show_smooth(instant=is_lock_handoff)
            self._trigger_glint()
            if is_lock_handoff and self._screen_locked:
                # Hold until ActiveChanged(false) confirms desktop reveal, with a
                # safety cap in case the D-Bus signal is missed.
                self._awaiting_unlock_dwell = True
                self._dismiss_timer.start(4000)
            else:
                self._dismiss_timer.start(950 if is_lock_handoff else 1100)

        elif state == "partial":
            hint = str(ev.get("hint") or "")
            self._state = "partial"
            self._hint_kind = classify_hint(hint, "partial")
            self._state_t0 = time.monotonic()
            self._label = tr(self._hint_kind) if self._hint_kind in TEXTS else (hint or tr("partial"))
            self.show_smooth()
            self._revert_timer.start(3200)

        elif state == "retry":
            tries = int(ev.get("tries", 1))
            max_tries = int(ev.get("max_tries", 5))
            hint = str(ev.get("hint") or "")
            self._state = "retry"
            self._hint_kind = classify_hint(hint, "retry")
            self._state_t0 = time.monotonic()
            if self._hint_kind in TEXTS:
                self._label = tr(self._hint_kind)
            elif hint:
                self._label = hint
            else:
                self._label = f"{tr('retry')} ({tries}/{max_tries})"
            self.show_smooth()
            if tries < max_tries:
                self._revert_timer.start(3200 if hint else 1500)
            else:
                self._dismiss_timer.start(1100)

        elif state == "declined":
            self._state = "declined"
            self._hint_kind = ""
            self._state_t0 = time.monotonic()
            self._label = tr("declined")
            self.show_smooth()
            self._dismiss_timer.start(2600)

        elif state == "ended":
            if self._state not in ("match", "retry", "declined"):
                self.hide_smooth()

        self.update()

    # ── Vector Painting ───────────────────────────────────────────────────────

    def _active_palette(self) -> dict[str, QColor]:
        if self._state == "match":
            return PALETTES["match"]
        if self._state == "scanning":
            return PALETTES["scanning"]
        if self._state in ("partial", "retry") and self._hint_kind in ("too_light", "too_hard", "dry"):
            return PALETTES["amber"]
        if self._state == "retry" and self._hint_kind == "wrong_area":
            return PALETTES["wrong_area"]
        if self._state == "retry" and self._hint_kind in ("off_center", "moving"):
            return PALETTES["armed"]
        if self._state in ("retry", "declined"):
            return PALETTES["coral"]
        return PALETTES["armed"]

    def paintEvent(self, _event) -> None:
        L = self._L
        p = QPainter(self)
        if L < 0.8:
            # Explicit clear to 100% transparent so the permanently-mapped
            # LayerOverlay surface is completely invisible while idle.
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
            p.fillRect(self.rect(), QColor(0, 0, 0, 0))
            p.end()
            return

        w = float(self.width())
        h = float(self.height())
        now = time.monotonic()
        dt = max(0.0, now - self._state_t0)
        pal = self._active_palette()

        curve = g3_sample(w, h, L, 110)
        curve_path = QPainterPath(QPointF(curve[0][0], curve[0][1]))
        for x, y in curve[1:]:
            curve_path.lineTo(x, y)

        shape_path = QPainterPath(curve_path)
        shape_path.lineTo(w + 4.0, h - L)
        shape_path.lineTo(w + 4.0, h + 4.0)
        shape_path.lineTo(w - L, h + 4.0)
        shape_path.closeSubpath()

        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        p.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)

        # 1. Outer-only OLED ambient bloom drawn BEHIND the pure #000000 shape
        #    so the corner pocket interior stays 100% pure OLED black.
        breath = 0.5 + 0.5 * math.sin(self._phase * 2.1)
        if self._state == "armed":
            bloom_mult = 0.42 + 0.24 * breath
        elif self._state == "match":
            bloom_mult = 0.95
        elif self._state in ("scanning", "partial") or self._hint_kind:
            bloom_mult = 0.65 + 0.20 * math.sin(self._phase * 3.5)
        elif self._state == "declined":
            bloom_mult = 0.34
        else:
            bloom_mult = 0.72

        for width_px, alpha_base in ((26.0, 10), (18.0, 18), (12.0, 30), (7.0, 48), (3.8, 76)):
            grad = self._edge_fade_gradient(
                w - L, h, w, h - L,
                pal["bloom"], pal["bloom_peak"],
                alpha_base * bloom_mult,
            )
            pen = QPen(grad, width_px, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
            p.setPen(pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(curve_path)

        # 2. Pure #000000 OLED hardware cutout
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(0, 0, 0, 255))
        p.drawPath(shape_path)

        # 3. Precision specular keyline along the G3 contour (fades to 0 at edges)
        rim_alpha = 230 if self._state in ("match", "scanning") else 205
        rim_grad = self._edge_fade_gradient(
            w - L, h, w, h - L,
            pal["rim"], pal["rim_peak"],
            rim_alpha,
        )
        p.setPen(QPen(rim_grad, 1.35, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(curve_path)

        # 4. Traveling soft specular glint on extend / match
        glint_age = now - self._glint_t0
        if 0.0 <= glint_age <= self._glint_dur:
            gt = glint_age / self._glint_dur
            ge = 2.0 * gt * gt if gt < 0.5 else 1.0 - ((-2.0 * gt + 2.0) ** 2) / 2.0
            center = 0.12 + ge * 0.76
            env = math.sin(gt * math.pi)
            ggrad = QLinearGradient(w - L, h, w, h - L)
            c0 = QColor(255, 255, 255, 0)
            c1 = QColor(255, 255, 255, int(215 * env))
            ggrad.setColorAt(max(0.0, center - 0.16), c0)
            ggrad.setColorAt(center, c1)
            ggrad.setColorAt(min(1.0, center + 0.16), c0)
            p.setPen(QPen(ggrad, 2.2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawPath(curve_path)

        # 5. Inside the clipped pure-black G3 pocket: glyph, guidance & curved text
        if self._content_alpha > 0.01:
            p.save()
            p.setClipPath(shape_path)
            p.setOpacity(self._content_alpha)

            # Inscribed circle inside G3 corner pocket
            mid = g3_eval(g3_control_points(w, h, L), 0.5)
            D = math.hypot(w - mid[0], h - mid[1])
            r = D / (1.0 + math.sqrt(2.0))
            cx = w - r * 0.86
            cy = h - r * 0.86
            glyph_scale = (r * 0.43) / 22.8

            self._draw_sensor_content(p, cx, cy, r, glyph_scale, dt, pal)
            self._draw_curved_label(p, curve, w, h, r, pal["label"])
            p.restore()

        p.end()

    def _edge_fade_gradient(
        self, x1: float, y1: float, x2: float, y2: float,
        edge_col: QColor, peak_col: QColor, max_alpha: float,
    ) -> QLinearGradient:
        grad = QLinearGradient(x1, y1, x2, y2)
        c_zero = QColor(edge_col.red(), edge_col.green(), edge_col.blue(), 0)
        c_mid = QColor(edge_col.red(), edge_col.green(), edge_col.blue(), int(min(255, max_alpha * 0.36)))
        c_peak = QColor(peak_col.red(), peak_col.green(), peak_col.blue(), int(min(255, max_alpha)))
        grad.setColorAt(0.0, c_zero)
        grad.setColorAt(0.16, c_mid)
        grad.setColorAt(0.50, c_peak)
        grad.setColorAt(0.84, c_mid)
        grad.setColorAt(1.0, c_zero)
        return grad

    def _draw_sensor_content(
        self, p: QPainter, cx: float, cy: float, r: float, scale: float, dt: float, pal: dict[str, QColor]
    ) -> None:
        if self._state == "match":
            # Fade green ridges out quickly while drawing crisp checkmark
            ridge_alpha = max(0.0, 1.0 - max(0.0, dt - 0.12) / 0.22)
            if ridge_alpha > 0.01:
                col = QColor(pal["glyph"])
                col.setAlphaF(ridge_alpha)
                self._draw_fingerprint_glyph(p, cx, cy, col, scale)
            check_prog = max(0.0, min(1.0, (dt - 0.16) / 0.32))
            if check_prog > 0.0:
                self._draw_checkmark(p, cx, cy, scale * 1.12, check_prog, pal["glyph"])
            return

        # Guidance animations
        if self._hint_kind in ("too_light", "too_hard"):
            inward = self._hint_kind == "too_light"
            period = 1.2 if inward else 1.3
            for i in range(3):
                t = ((self._phase / period) + i / 3.0) % 1.0
                if inward:
                    sc = 1.42 - 0.60 * t
                    alpha = math.sin(t * math.pi) * 0.82
                else:
                    sc = 0.84 + 0.58 * t
                    alpha = (1.0 - t) * 0.85
                col = QColor(pal["glyph"])
                col.setAlphaF(max(0.0, min(1.0, alpha)))
                p.setPen(QPen(col, 1.5 * scale))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawEllipse(QPointF(cx, cy), 25.0 * scale * sc, 25.0 * scale * sc)
            pulse = 0.5 - 0.5 * math.cos(self._phase * (2.0 * math.pi / period))
            gscale = scale * (1.0 - 0.12 * pulse if inward else 0.90 + 0.14 * pulse)
            self._draw_fingerprint_glyph(p, cx, cy, pal["glyph"], gscale)
            return

        if self._hint_kind == "wrong_area":
            # Gentle micro-orbit over dashed sensor track (prompts rolling/shifting slightly)
            track_pen = QPen(QColor(100, 210, 255, 72), 1.4 * scale, Qt.PenStyle.DashLine)
            p.setPen(track_pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(QPointF(cx, cy), 27.0 * scale, 27.0 * scale)
            ang = self._phase * 2.8
            ox = math.sin(ang) * 5.2 * scale
            oy = math.sin(ang * 2.0) * 3.2 * scale
            self._draw_fingerprint_glyph(p, cx + ox, cy + oy, pal["glyph"], scale)
            return

        if self._hint_kind == "off_center":
            # Ghost fingerprint gliding gently toward center + subtle pulsing target ring
            p.setPen(QPen(QColor(255, 255, 255, 65), 1.3 * scale))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(QPointF(cx, cy), 26.0 * scale, 26.0 * scale)
            t = (self._phase / 1.3) % 1.0
            ease = 1.0 - (1.0 - t) ** 3
            gx = (1.0 - ease) * (-12.0 * scale)
            dim = QColor(255, 255, 255, 95)
            self._draw_fingerprint_glyph(p, cx, cy, dim, scale)
            ghost = QColor(255, 255, 255, int(215 * math.sin(t * math.pi)))
            self._draw_fingerprint_glyph(p, cx + gx, cy, ghost, scale)
            return

        if self._state == "scanning":
            # Base dim ridges + blue ridges filling from center outward
            self._draw_fingerprint_glyph(p, cx, cy, QColor(255, 255, 255, 72), scale)
            fill_prog = min(1.0, (dt % 0.95) / 0.62)
            self._draw_fingerprint_glyph(p, cx, cy, QColor("#0a84ff"), scale, fill_frac=fill_prog)
            return

        if self._state == "retry":
            shake_x = 0.0
            if dt < 0.48:
                shake_x = math.sin(dt * 42.0) * 6.5 * math.exp(-dt * 7.5)
            self._draw_fingerprint_glyph(p, cx + shake_x, cy, pal["glyph"], scale)
            return

        if self._state == "declined":
            col = QColor(pal["glyph"])
            col.setAlpha(120)
            self._draw_fingerprint_glyph(p, cx, cy, col, scale)
            return

        # Armed (ready): calm breathing white fingerprint
        breath = 0.74 + 0.26 * (0.5 + 0.5 * math.sin(self._phase * 2.1))
        col = QColor(255, 255, 255, int(240 * breath))
        self._draw_fingerprint_glyph(p, cx, cy, col, scale)

    def _draw_fingerprint_glyph(
        self, p: QPainter, cx: float, cy: float, color: QColor, scale: float, fill_frac: float = 1.0
    ) -> None:
        """Touch ID-style vertical oval whorl (matches prototype & QML)."""
        p.save()
        p.translate(cx, cy)
        p.scale(scale, scale)
        pen = QPen(color, 2.0, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)

        arcs = (
            (5.0,  7.2, -10, 215, 1.8),
            (10.0, 12.6,  -5, 198, 1.2),
            (10.0, 12.6, 232, 108, 1.2),
            (15.0, 17.6,  12, 172, 0.6),
            (15.0, 17.6, 212, 128, 0.6),
            (20.0, 22.8,  28, 138, 0.0),
            (20.0, 22.8, 200, 148, 0.0),
        )
        # Center vertical ridge
        if fill_frac > 0.05:
            p.drawLine(QPointF(0.0, 2.0), QPointF(0.0, 2.0 + 9.6 * min(1.0, fill_frac * 3.0)))

        total = len(arcs)
        for idx, (rx, ry, start_deg, span_deg, y_off) in enumerate(arcs):
            if fill_frac < 1.0:
                # Illuminate progressively from center arcs outward
                local = max(0.0, min(1.0, (fill_frac * total - idx * 0.55) / 1.4))
                if local <= 0.01:
                    continue
                span = span_deg * local
            else:
                span = span_deg
            rect = QRectF(-rx, -ry + y_off, rx * 2.0, ry * 2.0)
            p.drawArc(rect, int(start_deg * 16), int(span * 16))
        p.restore()

    def _draw_checkmark(
        self, p: QPainter, cx: float, cy: float, scale: float, prog: float, color: QColor
    ) -> None:
        p.save()
        pen = QPen(color, 3.2 * scale, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        p.setPen(pen)
        p1 = QPointF(cx - 11.0 * scale, cy + 1.0 * scale)
        p2 = QPointF(cx - 3.0 * scale, cy + 9.0 * scale)
        p3 = QPointF(cx + 13.0 * scale, cy - 9.0 * scale)
        path = QPainterPath(p1)
        if prog <= 0.42:
            t = prog / 0.42
            path.lineTo(p1 + (p2 - p1) * t)
        else:
            path.lineTo(p2)
            t = (prog - 0.42) / 0.58
            path.lineTo(p2 + (p3 - p2) * t)
        p.drawPath(path)
        p.restore()

    def _draw_curved_label(
        self, p: QPainter, curve: list[tuple[float, float]], w: float, h: float, r: float, color: QColor
    ) -> None:
        text = self._label
        if not text:
            return

        # Compute inward-normal offset polyline hugging the G3 fillet
        off = max(18.0, r * 0.30)
        inner: list[tuple[float, float]] = []
        n_pts = len(curve)
        for i, (px, py) in enumerate(curve):
            ax, ay = curve[max(0, i - 1)]
            bx, by = curve[min(n_pts - 1, i + 1)]
            tx, ty = bx - ax, by - ay
            norm = math.hypot(tx, ty) or 1.0
            nx, ny = -ty / norm, tx / norm
            if (w - px) * nx + (h - py) * ny < 0.0:
                nx, ny = -nx, -ny
            inner.append((px + nx * off, py + ny * off))

        # Cumulative arc lengths along inner curve
        cum = [0.0]
        for i in range(1, len(inner)):
            cum.append(cum[-1] + math.hypot(inner[i][0] - inner[i - 1][0], inner[i][1] - inner[i - 1][1]))
        total_len = cum[-1]
        if total_len < 40.0:
            return

        font = QFont()
        font.setPixelSize(int(round(max(12.0, min(14.5, r * 0.21)))))
        font.setWeight(QFont.Weight.DemiBold)
        font.setStyleStrategy(QFont.StyleStrategy.NoSubpixelAntialias)
        p.setFont(font)
        p.setPen(color)

        fm = QFontMetricsF(font)
        advances = [fm.horizontalAdvance(ch) for ch in text]
        text_w = sum(advances)
        s = (total_len - text_w) * 0.5

        def sample_at(dist: float) -> tuple[float, float, float]:
            dist = max(0.0, min(total_len, dist))
            lo, hi = 0, len(cum) - 1
            while lo < hi - 1:
                mid = (lo + hi) // 2
                if cum[mid] <= dist:
                    lo = mid
                else:
                    hi = mid
            seg = cum[hi] - cum[lo] or 1.0
            f = (dist - cum[lo]) / seg
            x = inner[lo][0] + (inner[hi][0] - inner[lo][0]) * f
            y = inner[lo][1] + (inner[hi][1] - inner[lo][1]) * f
            ang = math.degrees(math.atan2(inner[hi][1] - inner[lo][1], inner[hi][0] - inner[lo][0]))
            return x, y, ang

        for ch, adv in zip(text, advances):
            mid_s = s + adv * 0.5
            x, y, deg = sample_at(mid_s)
            p.save()
            p.translate(x, y)
            p.rotate(deg)
            p.drawText(QPointF(-adv * 0.5, 0.0), ch)
            p.restore()
            s += adv


# ── Unix DGRAM Socket Listener ────────────────────────────────────────────────

def bind_hud_socket() -> tuple[socket.socket, Path]:
    uid = os.getuid()
    run_dir = Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{uid}")
    sock_path = run_dir / "pressmatch-hud.sock"
    try:
        sock_path.unlink()
    except FileNotFoundError:
        pass
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind(str(sock_path))
    os.chmod(sock_path, 0o622)  # allow root PAM helpers (polkit/sudo) to sendto cleanly
    sock.setblocking(False)
    return sock, sock_path


def render_all_previews(out_dir: str) -> None:
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    app = QApplication(sys.argv[:1])
    hud = PressMatchHUD()
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    cases = [
        ("armed_oled", QColor(0, 0, 0), {"state": "armed", "readers": ["ELAN", "FT9201"], "service": "polkit-1"}),
        ("armed_light", QColor(232, 238, 248), {"state": "armed", "readers": ["ELAN", "FT9201"], "service": "polkit-1"}),
        ("scanning_oled", QColor(0, 0, 0), {"state": "scanning", "device": "ELAN", "service": "polkit-1"}),
        ("light_oled", QColor(0, 0, 0), {"state": "partial", "device": "ELAN", "hint": "Press firmer, cover the sensor", "service": "polkit-1"}),
        ("wrong_area_oled", QColor(0, 0, 0), {"state": "retry", "device": "ELAN", "tries": 1, "max_tries": 5, "hint": "Place it like during setup", "service": "polkit-1"}),
        ("match_oled", QColor(0, 0, 0), {"state": "match", "device": "ELAN", "ms": 235, "service": "polkit-1"}),
    ]
    for name, bg, ev in cases:
        hud.handle_event(ev)
        hud._L = hud.L_MAX
        hud._driving = False
        hud._content_alpha = 1.0
        hud._state_t0 = time.monotonic() - 0.55
        hud._glint_t0 = time.monotonic() - 0.38 if "match" in name else -10.0
        hud._phase = 0.8
        pix = QPixmap(hud.size())
        pix.fill(bg)
        hud.render(pix)
        pix.save(str(Path(out_dir) / f"hud_{name}.png"))
    print(f"Rendered {len(cases)} OLED G3 HUD previews to {out_dir}")


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--render-all":
        render_all_previews(sys.argv[2])
        return 0

    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setApplicationName("PressMatch HUD")

    hud = PressMatchHUD()
    chime = SuccessChime(hud)

    if "--demo" in sys.argv:
        steps = [
            (100, {"state": "armed", "readers": ["ELAN", "FT9201"], "service": "polkit-1"}),
            (1500, {"state": "partial", "device": "ELAN", "hint": "Press firmer, cover the sensor", "service": "polkit-1"}),
            (3200, {"state": "retry", "device": "ELAN", "tries": 1, "max_tries": 5, "hint": "Place it like during setup", "service": "polkit-1"}),
            (5000, {"state": "scanning", "device": "ELAN", "service": "polkit-1"}),
            (5900, {"state": "match", "device": "ELAN", "ms": 235, "service": "polkit-1"}),
            (7600, None),
        ]
        for delay_ms, ev in steps:
            if ev is None:
                QTimer.singleShot(delay_ms, app.quit)
            else:
                def fire(e=ev):
                    hud.handle_event(e)
                    if e.get("state") == "match":
                        chime.play()
                QTimer.singleShot(delay_ms, fire)
        return app.exec()

    sock, sock_path = bind_hud_socket()

    def on_readable() -> None:
        while True:
            try:
                raw = sock.recv(4096)
            except BlockingIOError:
                break
            except OSError:
                break
            if not raw:
                break
            try:
                ev = json.loads(raw.decode("utf-8", errors="replace"))
                if isinstance(ev, dict):
                    hud.handle_event(ev)
                    if ev.get("state") == "match":
                        chime.play()
            except Exception:
                pass

    notifier = QSocketNotifier(sock.fileno(), QSocketNotifier.Type.Read, hud)
    notifier.activated.connect(on_readable)

    def cleanup(*_args) -> None:
        try:
            sock.close()
        except Exception:
            pass
        try:
            sock_path.unlink()
        except FileNotFoundError:
            pass
        app.quit()

    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)
    sig_timer = QTimer()
    sig_timer.start(500)
    sig_timer.timeout.connect(lambda: None)

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
