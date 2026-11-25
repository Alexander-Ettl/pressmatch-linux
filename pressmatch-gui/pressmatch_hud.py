#!/usr/bin/env python3
"""
PressMatch Touch ID-Style Floating Visual Feedback HUD for Linux (KDE Plasma 6 Wayland).

Listens on Unix DGRAM socket /run/user/<uid>/pressmatch-hud.sock for live PAM events
from pam_pressmatch_dual.so (armed, scanning, match, retry, ended) and renders a
sleek, click-through, non-focus-stealing frosted porcelain pill at top-center via
Wayland wlr-layer-shell (LayerOverlay).

Zero CPU when hidden; 60 FPS vector QPainter animation only while visible.

Usage:
  pressmatch_hud.py              # Run background HUD daemon
  pressmatch_hud.py --demo       # Cycle through all states visually (live preview)
  pressmatch_hud.py --render-all /path/dir  # Render PNG previews of every state
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
    QEasingCurve,
    QMargins,
    QPointF,
    QRectF,
    QSocketNotifier,
    Qt,
    QTimer,
    QVariantAnimation,
)
from PyQt6.QtGui import (
    QColor,
    QFont,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
    QRadialGradient,
)
from PyQt6.QtWidgets import QApplication, QWidget
from PyQt6 import sip


# ── Bilingual DE / EN labels (auto-detected from Linux locale) ────────────────

def _is_german() -> bool:
    for var in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        val = (os.environ.get(var) or "").lower()
        if val:
            return val.startswith("de")
    return False


IS_DE = _is_german()

TEXTS = {
    "armed": ("Fingerabdruck bereit", "Fingerprint Ready"),
    "scanning": ("Scanne Fingerabdruck…", "Scanning Fingerprint…"),
    "match": ("Bestätigt", "Verified"),
    "unlocked": ("Entsperrt", "Unlocked"),
    "retry": ("Nochmal versuchen", "Try Again"),
    "both": ("ELAN · FT9201", "ELAN · FT9201"),
}


def tr(key: str) -> str:
    de, en = TEXTS[key]
    return de if IS_DE else en


# ── Native Wayland Layer-Shell binding via libLayerShellQtInterface.so.6 ──────

class _LayerShellBridge:
    """Configure a PyQt6 QWindow as a click-through Wayland LayerOverlay pill."""

    LAYER_OVERLAY = 3
    ANCHOR_TOP = 1
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

    def configure(self, widget: QWidget, top_margin: int = 20) -> bool:
        if not self._lib or not os.environ.get("WAYLAND_DISPLAY"):
            return False
        widget.winId()  # ensure native QWindow exists
        qwin = widget.windowHandle()
        if qwin is None:
            return False
        qwin_ptr = sip.unwrapinstance(qwin)
        lwin = self._get(qwin_ptr)
        if not lwin:
            return False
        self._set_layer(lwin, self.LAYER_OVERLAY)
        self._set_anchors(lwin, self.ANCHOR_TOP)
        self._set_kb(lwin, self.KB_NONE)
        self._set_excl(lwin, 0)
        margins = QMargins(0, top_margin, 0, 0)
        self._set_margins(lwin, sip.unwrapinstance(margins))
        if self._set_activate:
            self._set_activate(lwin, False)
        return True


# ── Custom Vector Fingerprint Pill Widget ─────────────────────────────────────

class PressMatchHUD(QWidget):
    PILL_W = 324
    PILL_H = 68
    PAD = 18  # extra room for ambient shadow & horizontal shake

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
        self.setFixedSize(self.PILL_W + self.PAD * 2, self.PILL_H + self.PAD * 2)

        # Visual state
        self._state = "hidden"  # hidden | armed | scanning | match | retry
        self._title = tr("armed")
        self._subtitle = tr("both")
        self._readers = ["ELAN", "FT9201"]
        self._opacity = 0.0
        self._y_offset = -10.0
        self._shake_x = 0.0
        self._phase = 0.0
        self._state_t0 = time.monotonic()

        # 60 FPS animation loop (only runs while visible!)
        self._anim_timer = QTimer(self)
        self._anim_timer.setInterval(16)
        self._anim_timer.timeout.connect(self._on_tick)

        # Opacity/slide fade animation
        self._fade_anim = QVariantAnimation(self)
        self._fade_anim.setDuration(220)
        self._fade_anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._fade_anim.valueChanged.connect(self._on_fade_val)
        self._fade_anim.finished.connect(self._on_fade_done)
        self._hiding = False

        # Auto-dismiss / retry-revert timers
        self._dismiss_timer = QTimer(self)
        self._dismiss_timer.setSingleShot(True)
        self._dismiss_timer.timeout.connect(self.hide_smooth)

        self._revert_timer = QTimer(self)
        self._revert_timer.setSingleShot(True)
        self._revert_timer.timeout.connect(self._revert_to_armed)

        # Safety watchdog so HUD never stays stuck if a caller is SIGKILLed
        self._watchdog_timer = QTimer(self)
        self._watchdog_timer.setSingleShot(True)
        self._watchdog_timer.timeout.connect(self.hide_smooth)

    def _ensure_positioned(self) -> None:
        if not self._layer_configured:
            self._layer_configured = self._layer_bridge.configure(self, top_margin=14)
        if not self._layer_configured:
            # X11 / fallback positioning at top-center
            scr = QApplication.primaryScreen()
            if scr:
                g = scr.availableGeometry()
                self.move(g.x() + (g.width() - self.width()) // 2, g.y() + 14)

    def _on_fade_val(self, val: float) -> None:
        self._opacity = float(val)
        self._y_offset = (1.0 - self._opacity) * -8.0
        self.update()

    def _on_fade_done(self) -> None:
        if self._hiding and self._opacity <= 0.01:
            self._state = "hidden"
            self._anim_timer.stop()
            self.hide()

    def _on_tick(self) -> None:
        self._phase += 0.016
        dt = time.monotonic() - self._state_t0
        if self._state == "retry" and dt < 0.42:
            # Crisp damped horizontal shake
            decay = math.exp(-dt * 8.5)
            self._shake_x = math.sin(dt * 42.0) * 9.5 * decay
        else:
            self._shake_x = 0.0
        self.update()

    def show_smooth(self) -> None:
        self._hiding = False
        self._ensure_positioned()
        if not self.isVisible():
            self._opacity = 0.0
            self.show()
        if not self._anim_timer.isActive():
            self._anim_timer.start()
        self._fade_anim.stop()
        self._fade_anim.setDuration(200)
        self._fade_anim.setStartValue(self._opacity)
        self._fade_anim.setEndValue(1.0)
        self._fade_anim.start()

    def hide_smooth(self) -> None:
        self._dismiss_timer.stop()
        self._revert_timer.stop()
        self._watchdog_timer.stop()
        if not self.isVisible() or self._hiding:
            return
        self._hiding = True
        self._fade_anim.stop()
        self._fade_anim.setDuration(220)
        self._fade_anim.setStartValue(self._opacity)
        self._fade_anim.setEndValue(0.0)
        self._fade_anim.start()

    def _revert_to_armed(self) -> None:
        if self._state == "retry":
            self._state = "armed"
            self._state_t0 = time.monotonic()
            self._title = tr("armed")
            self._subtitle = " · ".join(self._readers) if self._readers else tr("both")
            self.update()

    def handle_event(self, ev: dict) -> None:
        state = ev.get("state", "")
        service = ev.get("service", "")

        # On lockscreen (kde-fingerprint), only show the celebratory green unlock confirmation
        # right as the desktop appears; skip armed/scanning while locked behind Wayland lock.
        if "fingerprint" in service and state != "match":
            return

        self._dismiss_timer.stop()
        self._revert_timer.stop()

        if state == "armed":
            readers = ev.get("readers") or ["ELAN", "FT9201"]
            self._readers = [str(r) for r in readers]
            self._state = "armed"
            self._state_t0 = time.monotonic()
            self._title = tr("armed")
            self._subtitle = " · ".join(self._readers)
            self._watchdog_timer.start(10 * 60 * 1000)  # 10 min safety cap
            self.show_smooth()

        elif state == "scanning":
            dev = ev.get("device") or "ELAN"
            self._state = "scanning"
            self._state_t0 = time.monotonic()
            self._title = tr("scanning")
            self._subtitle = str(dev)
            self._watchdog_timer.start(10000)
            self.show_smooth()

        elif state == "match":
            dev = ev.get("device") or "ELAN"
            ms = ev.get("ms")
            self._state = "match"
            self._state_t0 = time.monotonic()
            self._title = tr("unlocked") if "fingerprint" in service else tr("match")
            if isinstance(ms, (int, float)) and 0 < ms < 60000:
                self._subtitle = f"{dev} · {int(round(ms))} ms"
            else:
                self._subtitle = str(dev)
            self._watchdog_timer.stop()
            self.show_smooth()
            self._dismiss_timer.start(1050)

        elif state == "retry":
            dev = ev.get("device") or "ELAN"
            tries = int(ev.get("tries", 1))
            max_tries = int(ev.get("max_tries", 2))
            self._state = "retry"
            self._state_t0 = time.monotonic()
            self._title = f"{tr('retry')} ({tries}/{max_tries})"
            self._subtitle = str(dev)
            self.show_smooth()
            if tries < max_tries:
                self._revert_timer.start(1150)
            else:
                self._dismiss_timer.start(950)

        elif state == "ended":
            # Only dismiss immediately if we aren't showing a celebratory match or final retry shake
            if self._state not in ("match", "retry"):
                self.hide_smooth()

        self.update()

    # ── Vector Painting ───────────────────────────────────────────────────────

    def paintEvent(self, _event) -> None:
        if self._opacity <= 0.005:
            return

        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        p.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        p.setOpacity(self._opacity)

        px = self.PAD + self._shake_x
        py = self.PAD + self._y_offset
        pill = QRectF(px, py, self.PILL_W, self.PILL_H)
        radius = self.PILL_H / 2.0

        # Soft layered elevation shadow
        for i, alpha in ((6, 10), (4, 16), (2, 24)):
            sh = pill.adjusted(-i * 0.6, i * 0.7, i * 0.6, i * 1.15)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(15, 23, 42, alpha))
            p.drawRoundedRect(sh, radius + i * 0.4, radius + i * 0.4)

        # Frosted porcelain pill background
        bg_grad = QLinearGradient(pill.topLeft(), pill.bottomLeft())
        bg_grad.setColorAt(0.0, QColor(255, 255, 255, 246))
        bg_grad.setColorAt(1.0, QColor(246, 247, 250, 242))
        p.setBrush(bg_grad)

        # Subtle state-tinted border glow
        border_col = QColor(15, 23, 42, 24)
        if self._state == "scanning":
            border_col = QColor(0, 122, 255, 110)
        elif self._state == "match":
            border_col = QColor(40, 205, 65, 130)
        elif self._state == "retry":
            border_col = QColor(255, 107, 74, 135)
        p.setPen(QPen(border_col, 1.25))
        p.drawRoundedRect(pill, radius, radius)

        # Left animated orb center
        cx = px + 36.0
        cy = py + self.PILL_H / 2.0
        self._draw_orb(p, cx, cy)

        # Typography (clean SF/Inter/Noto sans hierarchy)
        tx = px + 72.0
        tw = self.PILL_W - 90.0

        title_font = QFont()
        title_font.setPointSizeF(11.0)
        title_font.setWeight(QFont.Weight.DemiBold)
        p.setFont(title_font)

        if self._state == "match":
            p.setPen(QColor(22, 138, 45))
        elif self._state == "retry":
            p.setPen(QColor(204, 62, 38))
        elif self._state == "scanning":
            p.setPen(QColor(0, 102, 224))
        else:
            p.setPen(QColor(28, 28, 30))

        p.drawText(
            QRectF(tx, py + 13.0, tw, 24.0),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            self._title,
        )

        sub_font = QFont()
        sub_font.setPointSizeF(9.0)
        sub_font.setWeight(QFont.Weight.Medium)
        p.setFont(sub_font)
        p.setPen(QColor(110, 115, 125))
        p.drawText(
            QRectF(tx, py + 36.0, tw, 19.0),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            self._subtitle,
        )

        # Right status dot (subtle live indicator)
        dot_x = px + self.PILL_W - 22.0
        dot_y = cy
        if self._state == "armed":
            pulse = 0.45 + 0.55 * (0.5 + 0.5 * math.sin(self._phase * 2.8))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 122, 255, int(65 * pulse)))
            p.drawEllipse(QPointF(dot_x, dot_y), 6.5, 6.5)
            p.setBrush(QColor(0, 122, 255, int(190 * pulse)))
            p.drawEllipse(QPointF(dot_x, dot_y), 3.3, 3.3)
        elif self._state == "scanning":
            pulse = 0.5 + 0.5 * math.sin(self._phase * 10.0)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 122, 255, int(120 * pulse + 80)))
            p.drawEllipse(QPointF(dot_x, dot_y), 5.0 + 2.0 * pulse, 5.0 + 2.0 * pulse)

        p.end()

    def _draw_orb(self, p: QPainter, cx: float, cy: float) -> None:
        dt = max(0.0, time.monotonic() - self._state_t0)
        r = 21.0

        if self._state == "armed":
            breath = 0.5 + 0.5 * math.sin(self._phase * 2.8)
            # Soft outer halo
            halo_r = r + 2.0 + 3.5 * breath
            p.setPen(QPen(QColor(0, 122, 255, int(45 + 45 * breath)), 1.6))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(QPointF(cx, cy), halo_r, halo_r)
            # Soft blue-tinted disc
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 122, 255, 22))
            p.drawEllipse(QPointF(cx, cy), r, r)
            # Fingerprint icon
            self._draw_fingerprint_glyph(p, cx, cy, QColor(0, 112, 243), 1.0)

        elif self._state == "scanning":
            # Fast expanding radar ripple
            rip = (dt * 1.8) % 1.0
            p.setPen(QPen(QColor(0, 122, 255, int(170 * (1.0 - rip))), 2.0))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(QPointF(cx, cy), r + rip * 9.0, r + rip * 9.0)
            # Rich electric-blue orb
            grad = QRadialGradient(cx, cy - 6, r * 1.4)
            grad.setColorAt(0.0, QColor(36, 150, 255))
            grad.setColorAt(1.0, QColor(0, 102, 235))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(grad)
            p.drawEllipse(QPointF(cx, cy), r, r)
            # Crisp white fingerprint ridges
            self._draw_fingerprint_glyph(p, cx, cy, QColor(255, 255, 255, 235), 1.0)
            # Sweeping horizontal scan beam clipped to orb
            p.save()
            clip = QPainterPath()
            clip.addEllipse(QPointF(cx, cy), r - 1.0, r - 1.0)
            p.setClipPath(clip)
            sweep = math.sin(self._phase * 6.5) * (r * 0.72)
            beam = QLinearGradient(cx, cy + sweep - 6, cx, cy + sweep + 6)
            beam.setColorAt(0.0, QColor(255, 255, 255, 0))
            beam.setColorAt(0.5, QColor(255, 255, 255, 165))
            beam.setColorAt(1.0, QColor(255, 255, 255, 0))
            p.fillRect(QRectF(cx - r, cy + sweep - 6, r * 2, 12), beam)
            p.restore()

        elif self._state == "match":
            # Celebratory ring burst
            burst = min(1.0, dt / 0.45)
            if burst < 1.0:
                p.setPen(QPen(QColor(40, 205, 65, int(190 * (1.0 - burst))), 2.2))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawEllipse(QPointF(cx, cy), r + burst * 11.0, r + burst * 11.0)
            # Emerald orb
            grad = QRadialGradient(cx, cy - 6, r * 1.4)
            grad.setColorAt(0.0, QColor(52, 218, 89))
            grad.setColorAt(1.0, QColor(30, 178, 60))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(grad)
            p.drawEllipse(QPointF(cx, cy), r, r)
            # Animated crisp checkmark stroke
            prog = min(1.0, dt / 0.24)
            self._draw_checkmark(p, cx, cy, prog)

        elif self._state == "retry":
            # Coral-amber orb
            grad = QRadialGradient(cx, cy - 6, r * 1.4)
            grad.setColorAt(0.0, QColor(255, 122, 89))
            grad.setColorAt(1.0, QColor(229, 72, 52))
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(grad)
            p.drawEllipse(QPointF(cx, cy), r, r)
            self._draw_fingerprint_glyph(p, cx, cy, QColor(255, 255, 255, 235), 1.0)

    def _draw_fingerprint_glyph(self, p: QPainter, cx: float, cy: float, color: QColor, scale: float) -> None:
        """Crisp concentric Touch ID-style fingerprint ridge arcs."""
        p.save()
        p.translate(cx, cy)
        p.scale(scale, scale)
        pen = QPen(color, 1.75, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)

        # 5 balanced concentric biometric arches
        arcs = [
            # (rx, ry, start_deg, span_deg, y_off)
            (11.5, 13.0, 28, 138, 1.0),
            (11.5, 13.0, 200, 42, 1.0),
            (8.5, 10.0, 12, 175, 1.2),
            (8.5, 10.0, 212, 95, 1.2),
            (5.5, 7.0, -5, 205, 1.4),
            (5.5, 7.0, 232, 85, 1.4),
            (2.6, 3.8, -10, 215, 1.6),
        ]
        for rx, ry, start_deg, span_deg, y_off in arcs:
            rect = QRectF(-rx, -ry + y_off, rx * 2, ry * 2)
            p.drawArc(rect, int(start_deg * 16), int(span_deg * 16))

        # Short center vertical core ridge
        p.drawLine(QPointF(0.0, 1.5), QPointF(0.0, 6.8))
        p.restore()

    def _draw_checkmark(self, p: QPainter, cx: float, cy: float, prog: float) -> None:
        p.save()
        pen = QPen(QColor(255, 255, 255), 2.8, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
        p.setPen(pen)
        p1 = QPointF(cx - 7.5, cy + 0.5)
        p2 = QPointF(cx - 2.0, cy + 6.0)
        p3 = QPointF(cx + 8.5, cy - 5.5)
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
    hud._opacity = 1.0
    hud._y_offset = 0.0
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    cases = [
        ("armed", {"state": "armed", "readers": ["ELAN", "FT9201"], "service": "polkit-1"}),
        ("scanning", {"state": "scanning", "device": "ELAN", "service": "polkit-1"}),
        ("match", {"state": "match", "device": "ELAN", "ms": 235, "service": "polkit-1"}),
        ("retry", {"state": "retry", "device": "FT9201", "tries": 1, "max_tries": 2, "service": "polkit-1"}),
    ]
    for name, ev in cases:
        hud.handle_event(ev)
        hud._opacity = 1.0
        hud._y_offset = 0.0
        hud._shake_x = 0.0
        hud._state_t0 = time.monotonic() - 0.35
        hud._phase = 0.5
        pix = QPixmap(hud.size())
        pix.fill(QColor(232, 236, 242))
        hud.render(pix)
        pix.save(str(Path(out_dir) / f"hud_{name}.png"))
    print(f"Rendered 4 HUD previews to {out_dir}")


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--render-all":
        render_all_previews(sys.argv[2])
        return 0

    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setApplicationName("PressMatch HUD")

    hud = PressMatchHUD()

    if "--demo" in sys.argv:
        # Live visual demo cycling through every state cleanly on screen
        steps = [
            (100, {"state": "armed", "readers": ["ELAN", "FT9201"], "service": "polkit-1"}),
            (1300, {"state": "scanning", "device": "ELAN", "service": "polkit-1"}),
            (2300, {"state": "retry", "device": "ELAN", "tries": 1, "max_tries": 2, "service": "polkit-1"}),
            (3900, {"state": "scanning", "device": "FT9201", "service": "polkit-1"}),
            (4700, {"state": "match", "device": "FT9201", "ms": 240, "service": "polkit-1"}),
            (6200, None),
        ]
        for delay_ms, ev in steps:
            if ev is None:
                QTimer.singleShot(delay_ms, app.quit)
            else:
                QTimer.singleShot(delay_ms, lambda e=ev: hud.handle_event(e))
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
    # Periodic wakeup so Python handles signals promptly
    sig_timer = QTimer()
    sig_timer.start(500)
    sig_timer.timeout.connect(lambda: None)

    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
