#!/usr/bin/env python3
"""
PressMatch — Apple Liquid Glass-Inspired Native PyQt6 Widgets.

Minimal, tactile, human-first 60 fps custom widgets:
  1. TouchIdFingerprintWidget:
       Iconic Touch ID-style fingerprint inside a Liquid Glass lens.
       - Supports 'ambient' (Welcome hero), 'enroll' (live spatial ridge fill),
         and 'verify' (interactive Try-It unlock + playful head-shake).
       - Driven seamlessly under the hood by live 24x16 ElanpressPoseGraph maps
         with zero debug clutter — just luminous ridges, tactile touch squish,
         organic light ripples, subtle unscanned-region guidance, and springy
         completion/shake animations.
  2. SensorHintPill:
       Tiny, friendly visual cue showing whether to touch the Power Button or
       the USB Reader.
"""
from __future__ import annotations

import math
from typing import List, Optional, Tuple

from PyQt6.QtCore import QPointF, QRectF, Qt, QTimer
from PyQt6.QtGui import (
    QBrush,
    QColor,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QRadialGradient,
)
from PyQt6.QtWidgets import QSizePolicy, QWidget

GRID_COLS = 24
GRID_ROWS = 16


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def _clamp(v: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, v))


def _lerp_color(c1: QColor, c2: QColor, t: float) -> QColor:
    t = _clamp(t)
    return QColor(
        int(_lerp(c1.red(), c2.red(), t)),
        int(_lerp(c1.green(), c2.green(), t)),
        int(_lerp(c1.blue(), c2.blue(), t)),
        int(_lerp(c1.alpha(), c2.alpha(), t)),
    )


class TouchIdFingerprintWidget(QWidget):
    """
    Calm, iconic Touch ID fingerprint inside a Liquid Glass lens.

    Modes:
      - "ambient": Living hero emblem on the Welcome screen with gentle specular sheen.
      - "enroll":  Live spatial coverage lighting driven invisibly by ElanpressPoseGraph.
      - "verify":  Interactive live unlock lens (listening / match / nomatch + head-shake).
    """

    def __init__(self, mode: str = "enroll", theme: str = "light", parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._mode = mode
        self._theme = theme
        if mode == "ambient":
            self.setFixedSize(168, 168)
        else:
            self.setMinimumSize(250, 250)
            self.setMaximumSize(320, 320)
            self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        # Smoothly animated 24x16 coverage depth [0.0 .. 2.5]
        self._target_grid = [0.0] * (GRID_COLS * GRID_ROWS)
        self._display_grid = [0.0] * (GRID_COLS * GRID_ROWS)

        self._progress = 0.0
        self._display_progress = 0.0
        self._hint_dx = 0.0
        self._hint_dy = 0.0
        self._has_spatial = False
        self._finger_present = False
        self._completed = False

        # Verify mode state: "ready" | "listening" | "match" | "nomatch"
        self._verify_state = "listening"
        self._verify_flash = 0.0

        # Tactile & playful micro-animations
        self._phase = 0.0
        self._scale = 1.0
        self._target_scale = 1.0
        self._pulse_strength = 0.0
        self._pulse_center = QPointF(0.0, 0.0)
        self._warn_flash = 0.0
        self._shake_t = 0.0  # > 0 runs damped horizontal head-shake
        self._complete_anim = 0.0
        self._celebrate_ring = 0.0

        self._ridges: List[List[Tuple[float, float]]] = self._build_organic_ridges()

        self._timer = QTimer(self)
        self._timer.setInterval(33)  # calm 30 fps idle, boosted to 60 fps during active motion
        self._timer.timeout.connect(self._tick)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._timer.isActive():
            self._timer.start()

    def hideEvent(self, event) -> None:
        if self._timer.isActive():
            self._timer.stop()
        super().hideEvent(event)

    def _is_animating_actively(self) -> bool:
        if (
            self._finger_present
            or self._shake_t > 0.005
            or self._pulse_strength > 0.005
            or self._warn_flash > 0.005
            or self._celebrate_ring > 0.005
            or self._verify_flash > 0.005
            or abs(self._target_scale - self._scale) > 0.001
            or (self._completed and self._complete_anim < 0.995)
        ):
            return True
        if self._mode == "enroll":
            if abs(self._progress - self._display_progress) > 0.003:
                return True
            for i in range(GRID_COLS * GRID_ROWS):
                tgt = 2.4 if self._completed else self._target_grid[i]
                if abs(tgt - self._display_grid[i]) > 0.005:
                    return True
        return False

    @staticmethod
    def _build_organic_ridges() -> List[List[Tuple[float, float]]]:
        """Build crisp, Apple Touch ID-inspired flowing fingerprint ridges."""
        ridges: List[List[Tuple[float, float]]] = []

        def inside_pad(nx: float, ny: float) -> bool:
            # Smooth anatomical fingertip contour (gracefully rounded top, softer base)
            rx = 0.78 - 0.07 * max(0.0, -ny)
            ry = 0.96
            return (nx / rx) ** 2 + (ny / ry) ** 2 <= 1.0

        core_y = -0.04
        num_rings = 11  # Iconic, clean, bold Touch ID ridge density (never busy!)
        for i in range(num_rings):
            t = (i + 0.55) / num_rings
            rx = 0.065 + t * 0.77
            ry = 0.095 + t * 0.95
            segs: List[Tuple[float, float]] = []
            steps = 88
            for s in range(steps + 1):
                ang = (s / steps) * 2.0 * math.pi
                x = rx * math.cos(ang)
                y = core_y + ry * math.sin(ang)
                # Subtle organic fingerprint drift
                x += 0.015 * math.sin(y * 2.5)
                if y > 0.34:
                    dy = y - 0.34
                    y = 0.34 + dy * 0.88
                    x *= 1.0 + dy * 0.12

                # Clean Apple-like open breaks so ridges feel natural & breathable
                gap = False
                if i == 0:
                    if 0.65 < ang < 1.55:
                        gap = True
                elif i >= 1:
                    g1 = (i * 2.25 + 0.5) % (2.0 * math.pi)
                    if abs(((ang - g1 + math.pi) % (2.0 * math.pi)) - math.pi) < 0.13:
                        gap = True
                if i >= 4:
                    g2 = (i * 3.85 + 2.1) % (2.0 * math.pi)
                    if abs(((ang - g2 + math.pi) % (2.0 * math.pi)) - math.pi) < 0.12:
                        gap = True
                if i >= 8:
                    g3 = (i * 1.45 + 4.3) % (2.0 * math.pi)
                    if abs(((ang - g3 + math.pi) % (2.0 * math.pi)) - math.pi) < 0.11:
                        gap = True

                if inside_pad(x, y) and not gap:
                    segs.append((x, y))
                else:
                    if len(segs) >= 3:
                        ridges.append(segs)
                    segs = []
            if len(segs) >= 3:
                ridges.append(segs)

        return ridges

    # ------------------------------------------------------------------
    # Public Control API
    # ------------------------------------------------------------------

    def reset_enroll(self) -> None:
        self._mode = "enroll"
        self._target_grid = [0.0] * (GRID_COLS * GRID_ROWS)
        self._display_grid = [0.0] * (GRID_COLS * GRID_ROWS)
        self._progress = 0.0
        self._display_progress = 0.0
        self._hint_dx = 0.0
        self._hint_dy = 0.0
        self._has_spatial = False
        self._finger_present = False
        self._completed = False
        self._pulse_strength = 0.0
        self._warn_flash = 0.0
        self._shake_t = 0.0
        self._complete_anim = 0.0
        self._celebrate_ring = 0.0
        self._scale = 1.0
        self._target_scale = 1.0
        self.update()

    def set_verify_state(self, state: str) -> None:
        """state: 'ready' | 'listening' | 'match' | 'nomatch'"""
        self._mode = "verify"
        self._verify_state = state
        if state == "match":
            self._completed = True
            self._complete_anim = 0.0
            self._celebrate_ring = 1.0
            self._scale = 1.05
            self._target_scale = 1.0
            self._verify_flash = 1.0
        elif state == "nomatch":
            self._completed = False
            self._warn_flash = 1.0
            self._shake_t = 1.0
        elif state == "ready":
            self._completed = True
            self._complete_anim = 1.0
        else:
            self._completed = False
            self._complete_anim = 0.0
        self.update()

    def set_finger_present(self, present: bool) -> None:
        self._finger_present = present
        self._target_scale = 0.955 if present else 1.0
        self.update()

    def trigger_shake(self) -> None:
        self._shake_t = 1.0
        self._warn_flash = 0.9
        self.update()

    def apply_telemetry(self, payload: dict) -> None:
        """Update live spatial coverage from merged D-Bus + PoseGraph payload."""
        telem = payload.get("telemetry")
        stage = int(payload.get("stage", 0))
        total = max(int(payload.get("total_stages", 10)), 1)
        dbus_res = payload.get("dbus_result", "")

        if telem and isinstance(telem.get("grid"), list) and len(telem["grid"]) == GRID_COLS * GRID_ROWS:
            self._has_spatial = True
            self._target_grid = [float(v) for v in telem["grid"]]
            last_mask = telem.get("last_mask") or []
            self._hint_dx = float(telem.get("hint_dx", 0.0))
            self._hint_dy = float(telem.get("hint_dy", 0.0))
            pct = float(telem.get("progress_pct", 0.0)) / 100.0
            self._progress = max(pct, stage / total)
            st = str(telem.get("status", ""))

            if st in ("accepted", "complete"):
                sx, sy, cnt = 0.0, 0.0, 0
                if len(last_mask) == GRID_COLS * GRID_ROWS:
                    for r in range(GRID_ROWS):
                        for c in range(GRID_COLS):
                            if last_mask[r * GRID_COLS + c] > 0:
                                sx += ((c + 0.5) / GRID_COLS) * 2.0 - 1.0
                                sy += ((r + 0.5) / GRID_ROWS) * 2.0 - 1.0
                                cnt += 1
                self._pulse_center = QPointF(sx / cnt, sy / cnt) if cnt > 0 else QPointF(0.0, 0.0)
                self._pulse_strength = 1.0
                self._scale = 1.045  # Delightful gentle spring pop on accepted touch!
                self._target_scale = 1.0
            elif st in ("redundant", "saturated", "low_quality", "low_coverage"):
                self.trigger_shake()

            if st == "complete" or telem.get("complete") or dbus_res == "enroll-completed":
                self._completed = True
                self._progress = 1.0
                self._celebrate_ring = 1.0
                self._scale = 1.06
                self._target_scale = 1.0
        else:
            # Graceful progressive concentric fill when spatial map isn't present
            frac = min(1.0, stage / total)
            self._progress = frac
            if dbus_res in ("enroll-stage-passed", "enroll-completed"):
                self._pulse_center = QPointF(0.0, 0.0)
                self._pulse_strength = 1.0
                self._scale = 1.045
                self._target_scale = 1.0
                rad = 0.24 + 0.96 * frac
                for r in range(GRID_ROWS):
                    ny = ((r + 0.5) / GRID_ROWS) * 2.0 - 1.0
                    for c in range(GRID_COLS):
                        nx = ((c + 0.5) / GRID_COLS) * 2.0 - 1.0
                        d = math.hypot(nx / 0.85, ny / 0.92)
                        if d <= rad:
                            depth = 2.0 if d <= rad * 0.65 else 1.0
                            self._target_grid[r * GRID_COLS + c] = max(
                                self._target_grid[r * GRID_COLS + c], depth
                            )
            elif "retry" in dbus_res or "not-centered" in dbus_res:
                self.trigger_shake()

            if dbus_res == "enroll-completed":
                self._completed = True
                self._progress = 1.0
                self._celebrate_ring = 1.0
                self._scale = 1.06
                self._target_scale = 1.0

        self.update()

    # ------------------------------------------------------------------
    # Animation Loop
    # ------------------------------------------------------------------

    def _tick(self) -> None:
        if not self.isVisible():
            self._timer.stop()
            return
        active = self._is_animating_actively()
        desired_ms = 16 if active else 40  # 60 fps active transitions, 25 fps calm breathing
        if self._timer.interval() != desired_ms:
            self._timer.setInterval(desired_ms)
        self._phase += 0.038 * (desired_ms / 16.0)

        # Smooth spring scale
        if abs(self._target_scale - self._scale) > 0.0005:
            self._scale = _lerp(self._scale, self._target_scale, 0.20)

        # Smooth spatial coverage interpolation
        if self._mode == "enroll":
            for i in range(GRID_COLS * GRID_ROWS):
                tgt = 2.4 if self._completed else self._target_grid[i]
                cur = self._display_grid[i]
                if abs(tgt - cur) > 0.004:
                    self._display_grid[i] = _lerp(cur, tgt, 0.15)
            if abs(self._progress - self._display_progress) > 0.002:
                self._display_progress = _lerp(self._display_progress, self._progress, 0.14)

        if self._pulse_strength > 0.004:
            self._pulse_strength *= 0.91
        else:
            self._pulse_strength = 0.0

        if self._warn_flash > 0.004:
            self._warn_flash *= 0.91
        else:
            self._warn_flash = 0.0

        if self._shake_t > 0.004:
            self._shake_t = max(0.0, self._shake_t - 0.036)
        else:
            self._shake_t = 0.0

        if self._celebrate_ring > 0.004:
            self._celebrate_ring *= 0.93
        else:
            self._celebrate_ring = 0.0

        if self._completed and self._complete_anim < 1.0:
            self._complete_anim = min(1.0, self._complete_anim + 0.055)

        if self._verify_flash > 0.004:
            self._verify_flash *= 0.92
        else:
            self._verify_flash = 0.0

        self.update()

    def _sample_depth(self, nx: float, ny: float) -> float:
        u = ((nx / 0.85) * 0.5 + 0.5) * (GRID_COLS - 1)
        v = ((ny / 0.98) * 0.5 + 0.5) * (GRID_ROWS - 1)
        u = _clamp(u, 0.0, GRID_COLS - 1.001)
        v = _clamp(v, 0.0, GRID_ROWS - 1.001)
        c0, r0 = int(u), int(v)
        c1, r1 = min(c0 + 1, GRID_COLS - 1), min(r0 + 1, GRID_ROWS - 1)
        fx, fy = u - c0, v - r0
        d00 = self._display_grid[r0 * GRID_COLS + c0]
        d10 = self._display_grid[r0 * GRID_COLS + c1]
        d01 = self._display_grid[r1 * GRID_COLS + c0]
        d11 = self._display_grid[r1 * GRID_COLS + c1]
        return _lerp(_lerp(d00, d10, fx), _lerp(d01, d11, fx), fy)

    # ------------------------------------------------------------------
    # Painting (Liquid Glass Medallion + Living Ridges)
    # ------------------------------------------------------------------

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        w, h = self.width(), self.height()
        cx = w * 0.5
        cy = h * 0.5

        # Playful damped horizontal head-shake offset
        if self._shake_t > 0.0:
            elapsed = 1.0 - self._shake_t
            shake_x = math.sin(elapsed * math.pi * 6.0) * (self._shake_t ** 1.3) * 14.0
            cx += shake_x

        base_r = min(w, h) * (0.42 if self._mode != "ambient" else 0.44)

        p.save()
        p.translate(cx, cy)
        p.scale(self._scale, self._scale)

        # 1. Soft Apple Porcelain / Liquid Glass ambient refraction aura
        light = self._theme == "light"
        breathe = 0.5 + 0.5 * math.sin(self._phase * 1.4)
        aura_r = base_r * (1.28 + 0.04 * breathe)
        aura = QRadialGradient(QPointF(0, 0), aura_r)
        if self._completed or (self._mode == "verify" and self._verify_state in ("match", "ready")):
            aura.setColorAt(0.0, QColor(52, 199, 89, 54 if light else 68))
            aura.setColorAt(0.60, QColor(52, 199, 89, 16 if light else 18))
            aura.setColorAt(1.0, QColor(52, 199, 89, 0))
        elif self._warn_flash > 0.05:
            aura.setColorAt(0.0, QColor(255, 159, 10, int((58 if light else 74) * self._warn_flash)))
            aura.setColorAt(0.65, QColor(255, 69, 58, int(20 * self._warn_flash)))
            aura.setColorAt(1.0, QColor(255, 69, 58, 0))
        else:
            alpha = (46 if light else 62) if self._finger_present else int((22 if light else 30) + 14 * breathe)
            aura.setColorAt(0.0, QColor(0, 113, 227, alpha))
            aura.setColorAt(0.62, QColor(56, 189, 248, alpha // 3))
            aura.setColorAt(1.0, QColor(0, 113, 227, 0))
        p.setBrush(QBrush(aura))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(QPointF(0, 0), aura_r, aura_r)

        # 2. Porcelain / Translucent Liquid Glass Lens Disc
        lens_r = base_r * 1.04
        if light:
            # Subtle soft drop halo under white porcelain medallion
            sh = QRadialGradient(QPointF(0, lens_r * 0.08), lens_r * 1.12)
            sh.setColorAt(0.75, QColor(15, 23, 42, 22))
            sh.setColorAt(1.0, QColor(15, 23, 42, 0))
            p.setBrush(QBrush(sh))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(QPointF(0, lens_r * 0.05), lens_r * 1.08, lens_r * 1.08)

            disc_grad = QRadialGradient(QPointF(-lens_r * 0.22, -lens_r * 0.35), lens_r * 1.55)
            disc_grad.setColorAt(0.0, QColor("#FFFFFF"))
            disc_grad.setColorAt(0.65, QColor("#FAFAFC"))
            disc_grad.setColorAt(1.0, QColor("#EEF2F7"))
            p.setBrush(QBrush(disc_grad))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(QPointF(0, 0), lens_r, lens_r)

            rim_col = QColor(0, 113, 227, 52) if self._finger_present else QColor(15, 23, 42, 24)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(rim_col, 1.5))
            p.drawEllipse(QPointF(0, 0), lens_r, lens_r)
        else:
            disc_grad = QRadialGradient(QPointF(-lens_r * 0.22, -lens_r * 0.35), lens_r * 1.5)
            disc_grad.setColorAt(0.0, QColor(255, 255, 255, 26))
            disc_grad.setColorAt(0.45, QColor(34, 40, 54, 215))
            disc_grad.setColorAt(1.0, QColor(18, 21, 29, 238))
            p.setBrush(QBrush(disc_grad))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(QPointF(0, 0), lens_r, lens_r)

            rim_grad = QLinearGradient(QPointF(0, -lens_r), QPointF(0, lens_r))
            rim_alpha = 95 if self._finger_present else 62
            rim_grad.setColorAt(0.0, QColor(255, 255, 255, rim_alpha))
            rim_grad.setColorAt(0.35, QColor(255, 255, 255, 22))
            rim_grad.setColorAt(1.0, QColor(255, 255, 255, 36))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QBrush(rim_grad), 1.4))
            p.drawEllipse(QPointF(0, 0), lens_r, lens_r)

        # 3. Delicate circular progress ring (enroll mode only)
        if self._mode == "enroll":
            ring_r = lens_r + 1.5
            ring_rect = QRectF(-ring_r, -ring_r, ring_r * 2, ring_r * 2)
            track_col = QColor(15, 23, 42, 22) if light else QColor(255, 255, 255, 18)
            p.setPen(QPen(track_col, 4.2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawEllipse(ring_rect)

            if self._display_progress > 0.005:
                prog_grad = QLinearGradient(ring_rect.topLeft(), ring_rect.bottomRight())
                if self._completed:
                    prog_grad.setColorAt(0.0, QColor("#34C759"))
                    prog_grad.setColorAt(1.0, QColor("#248A3D"))
                elif self._warn_flash > 0.25:
                    prog_grad.setColorAt(0.0, QColor("#FF9F0A"))
                    prog_grad.setColorAt(1.0, QColor("#F59E0B"))
                else:
                    prog_grad.setColorAt(0.0, QColor("#38BDF8"))
                    prog_grad.setColorAt(1.0, QColor("#0071E3"))
                p.setPen(QPen(QBrush(prog_grad), 4.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
                span = int(-self._display_progress * 360.0 * 16)
                p.drawArc(ring_rect, 90 * 16, span)

        # Verify mode gentle listening arc
        if self._mode == "verify" and self._verify_state == "listening":
            ring_r = lens_r + 1.5
            ring_rect = QRectF(-ring_r, -ring_r, ring_r * 2, ring_r * 2)
            arc_col = QColor(0, 113, 227, 155) if light else QColor(90, 200, 250, 140)
            p.setPen(QPen(arc_col, 3.2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            start = int((-self._phase * 85) % 360) * 16
            p.drawArc(ring_rect, start, 68 * 16)

        fp_rx = base_r * 0.66
        fp_ry = base_r * 0.74

        def to_px(nx: float, ny: float) -> QPointF:
            return QPointF(nx * fp_rx, ny * fp_ry)

        # 4. Subtle unscanned-region directional glow (only in enroll mode when spatial hint exists)
        if (
            self._mode == "enroll"
            and self._has_spatial
            and not self._completed
            and self._progress > 0.05
            and (abs(self._hint_dx) > 0.08 or abs(self._hint_dy) > 0.08)
        ):
            mag = math.hypot(self._hint_dx, self._hint_dy)
            hx = _clamp((self._hint_dx / max(mag, 0.25)) * 0.44, -0.58, 0.58)
            hy = _clamp((self._hint_dy / max(mag, 0.25)) * 0.48, -0.62, 0.62)
            bpt = to_px(hx, hy)
            pulse = 0.5 + 0.5 * math.sin(self._phase * 2.2)
            br = base_r * (0.24 + 0.06 * pulse)
            bg = QRadialGradient(bpt, br)
            bg.setColorAt(0.0, QColor(0, 113, 227, int(48 + 36 * pulse)) if light else QColor(100, 210, 255, int(60 + 45 * pulse)))
            bg.setColorAt(0.6, QColor(0, 113, 227, int(18 + 14 * pulse)))
            bg.setColorAt(1.0, QColor(0, 113, 227, 0))
            p.setBrush(QBrush(bg))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(bpt, br, br)

        # 5. The Fingerprint Ridges (butter-smooth vector paths with zero dot artifacts!)
        unscanned = QColor(15, 23, 42, 42) if light else QColor(255, 255, 255, 42)
        unscanned_opaque = QColor(203, 213, 225, 255) if light else QColor(42, 48, 60, 255)
        apple_blue = QColor("#0071E3") if light else QColor("#0A84FF")
        apple_cyan = QColor("#0284C7") if light else QColor("#5AC8FA")
        apple_mint = QColor("#248A3D") if light else QColor("#30D158")
        apple_amber = QColor("#D97706") if light else QColor("#FF9F0A")
        shimmer_white = QColor(125, 211, 252, 255) if light else QColor(225, 245, 255, 255)

        base_w = max(2.4, base_r * 0.032)

        def build_path(pts: List[Tuple[float, float]]) -> QPainterPath:
            path = QPainterPath()
            p0 = to_px(pts[0][0], pts[0][1])
            path.moveTo(p0)
            for rx_p, ry_p in pts[1:]:
                path.lineTo(to_px(rx_p, ry_p))
            return path

        p.setBrush(Qt.BrushStyle.NoBrush)

        if self._mode == "ambient":
            grad = QLinearGradient(QPointF(-fp_rx, -fp_ry), QPointF(fp_rx, fp_ry))
            shift = 0.5 + 0.35 * math.sin(self._phase * 1.3)
            grad.setColorAt(0.0, apple_blue)
            grad.setColorAt(_clamp(shift, 0.15, 0.85), QColor("#38BDF8") if light else QColor("#5AC8FA"))
            grad.setColorAt(1.0, apple_blue)
            p.setPen(QPen(QBrush(grad), base_w * 0.98, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            for ridge in self._ridges:
                p.drawPath(build_path(ridge))
        elif self._mode == "verify":
            if self._verify_state in ("match", "ready"):
                col = _lerp_color(apple_cyan, apple_mint, 0.85)
                col.setAlpha(int(215 - 85 * self._complete_anim))
                pen = QPen(col, base_w * 1.02, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
            elif self._verify_state == "nomatch":
                col = _lerp_color(apple_blue, apple_amber, max(0.35, self._warn_flash))
                pen = QPen(col, base_w, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
            else:
                grad = QLinearGradient(QPointF(0, -fp_ry), QPointF(0, fp_ry))
                pos = 0.5 + 0.42 * math.sin(self._phase * 1.8)
                grad.setColorAt(0.0, QColor(15, 23, 42, 48) if light else QColor(255, 255, 255, 68))
                grad.setColorAt(_clamp(pos, 0.1, 0.9), QColor(0, 113, 227, 230) if light else QColor(90, 200, 250, 235))
                grad.setColorAt(1.0, QColor(15, 23, 42, 48) if light else QColor(255, 255, 255, 68))
                pen = QPen(QBrush(grad), base_w, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
            p.setPen(pen)
            for ridge in self._ridges:
                p.drawPath(build_path(ridge))
        elif self._completed:
            col = _lerp_color(apple_cyan, apple_mint, self._complete_anim)
            p.setPen(QPen(col, base_w * 1.08, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            for ridge in self._ridges:
                p.drawPath(build_path(ridge))
        else:
            # LIVE SPATIAL ENROLLMENT:
            # 1) Paint smooth base unscanned ridges once as continuous vector paths
            p.setPen(QPen(unscanned, base_w * 0.88, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            for ridge in self._ridges:
                p.drawPath(build_path(ridge))

            # 2) Paint opaque spatially captured ridge segments cleanly on top
            for ridge in self._ridges:
                for k in range(len(ridge) - 1):
                    x0, y0 = ridge[k]
                    x1, y1 = ridge[k + 1]
                    mx = (x0 + x1) * 0.5
                    my = (y0 + y1) * 0.5
                    depth = self._sample_depth(mx, my)
                    if depth <= 0.10:
                        continue
                    if depth < 1.30:
                        t = _clamp((depth - 0.10) / 1.05)
                        col = _lerp_color(unscanned_opaque, apple_blue, t)
                        pen_w = base_w * (0.92 + 0.16 * t)
                    else:
                        t = _clamp((depth - 1.30) / 1.0)
                        col = _lerp_color(apple_blue, apple_cyan, t)
                        pen_w = base_w * 1.10

                    if self._pulse_strength > 0.04:
                        dist = math.hypot(mx - self._pulse_center.x(), my - self._pulse_center.y())
                        if dist < 0.60:
                            boost = self._pulse_strength * (1.0 - dist / 0.60)
                            col = _lerp_color(col, shimmer_white, boost * 0.72)

                    p.setPen(QPen(col, pen_w, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
                    p.drawLine(to_px(x0, y0), to_px(x1, y1))

        # 6. Soft ripple ring on fresh tap
        if self._pulse_strength > 0.04 and self._mode == "enroll":
            rpt = to_px(self._pulse_center.x() * 0.6, self._pulse_center.y() * 0.65)
            rr = base_r * (0.14 + (1.0 - self._pulse_strength) * 0.46)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor(160, 230, 255, int(170 * self._pulse_strength)), 2.0))
            p.drawEllipse(rpt, rr, rr)

        # 7. Celebratory expanding glow ring
        if self._celebrate_ring > 0.04:
            cr = lens_r * (1.0 + (1.0 - self._celebrate_ring) * 0.30)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor(48, 209, 88, int(190 * self._celebrate_ring)), 2.6))
            p.drawEllipse(QPointF(0, 0), cr, cr)

        # 8. Crisp animated completion checkmark (enroll complete or verify match/ready)
        show_check = (
            (self._mode == "enroll" and self._completed and self._complete_anim > 0.25)
            or (self._mode == "verify" and self._verify_state in ("match", "ready") and self._complete_anim > 0.2)
        )
        if show_check:
            t = _clamp((self._complete_anim - 0.2) / 0.8)
            # Subtle circular mint glass pill centered in verify mode, corner badge in enroll mode
            if self._mode == "verify":
                br = base_r * 0.44 * (0.7 + 0.3 * t)
                bx, by = 0.0, 0.0
                p.setBrush(QBrush(QColor(48, 209, 88, int(235 * t))))
                p.setPen(QPen(QColor(255, 255, 255, int(90 * t)), 1.6))
                p.drawEllipse(QPointF(bx, by), br, br)
            else:
                br = base_r * 0.26 * t
                bx, by = base_r * 0.68, base_r * 0.68
                p.setBrush(QBrush(QColor("#34C759" if light else "#30D158")))
                p.setPen(QPen(QColor("#F5F5F7" if light else "#14171F"), 3.0))
                p.drawEllipse(QPointF(bx, by), br, br)

            if t > 0.35:
                ck = QPainterPath()
                ck.moveTo(bx - br * 0.38, by + br * 0.03)
                ck.lineTo(bx - br * 0.08, by + br * 0.33)
                ck.lineTo(bx + br * 0.42, by - br * 0.28)
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.setPen(
                    QPen(
                        QColor("#FFFFFF"),
                        3.4 if self._mode == "enroll" else 4.5,
                        Qt.PenStyle.SolidLine,
                        Qt.PenCapStyle.RoundCap,
                        Qt.PenJoinStyle.RoundJoin,
                    )
                )
                p.drawPath(ck)

        p.restore()


class SensorHintIcon(QWidget):
    """Minimalist Apple-style tiny hardware cue (Power Button vs USB Reader)."""

    def __init__(self, shape: str = "strip", theme: str = "light", parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.shape = shape
        self.theme = theme
        self.setFixedSize(36, 24)

    def set_shape(self, shape: str) -> None:
        self.shape = shape
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        w, h = self.width(), self.height()
        cx, cy = w * 0.5, h * 0.5

        light = self.theme == "light"
        accent = QColor("#0071E3") if light else QColor("#5AC8FA")
        fill = QColor(0, 113, 227, 34) if light else QColor(10, 132, 255, 48)
        border = QColor(15, 23, 42, 62) if light else QColor(255, 255, 255, 55)
        body = QColor(15, 23, 42, 12) if light else QColor(255, 255, 255, 16)
        plug = QColor(15, 23, 42, 26) if light else QColor(255, 255, 255, 32)

        if self.shape == "strip":
            p.setBrush(QBrush(body))
            p.setPen(QPen(border, 1.1))
            p.drawRoundedRect(QRectF(2, 4, 32, 16), 5, 5)
            p.setBrush(QBrush(fill))
            p.setPen(QPen(accent, 1.3))
            p.drawRoundedRect(QRectF(6, 7.5, 24, 9), 3, 3)
        elif self.shape == "square":
            p.setBrush(QBrush(plug))
            p.setPen(QPen(border, 1.0))
            p.drawRoundedRect(QRectF(2, 7.5, 8, 9), 2, 2)
            p.setBrush(QBrush(body))
            p.setPen(QPen(border, 1.1))
            p.drawRoundedRect(QRectF(10, 3, 22, 18), 5, 5)
            p.setBrush(QBrush(fill))
            p.setPen(QPen(accent, 1.3))
            p.drawRoundedRect(QRectF(14.5, 6.5, 13, 11), 3, 3)
        else:
            p.setBrush(QBrush(fill))
            p.setPen(QPen(accent, 1.3))
            p.drawRoundedRect(QRectF(cx - 8, cy - 8, 16, 16), 4, 4)
