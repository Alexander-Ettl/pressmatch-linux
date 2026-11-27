#!/usr/bin/env python3
"""
PressMatch — Calm, Warm Apple Touch ID-Style Fingerprint Setup.

Designed for effortless, zero-jargon setup out of the box, automatically
localized to German or English from the user's Linux locale settings:
  - Welcome:    One iconic breathing fingerprint, one clear button.
  - Enroll:     Instant live guided scan with real spatial ridge lighting,
                human coaching ("Place Your Finger" / "Finger auflegen"),
                and automatic back-to-back setup when two sensors are connected.
  - All Set:    Immediate playful live try-it unlock (with spring checkmark
                on match and playful head-shake on unrecognized fingers).
  - Settings:   Quietly tucked behind the top-right gear icon for managing
                saved fingers or refreshing system unlock integration.
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys
import threading
from pathlib import Path
from typing import Dict, List, Optional

from PyQt6.QtCore import Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QAction, QIcon
from PyQt6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from fprintd_bridge import (
    EnrollSession,
    SensorDevice,
    SimultaneousVerifySession,
    delete_enrolled_finger,
    discover_devices,
    get_system_integration_status,
)
from i18n import (
    detect_system_lang,
    friendly_direction,
    sensor_name,
    sensor_phrase,
    tr,
)
from success_chime import SuccessChime
from widgets import GRID_COLS, GRID_ROWS, SensorHintIcon, TouchIdFingerprintWidget

GUI_DIR = Path(__file__).resolve().parent
REPO_ROOT = GUI_DIR.parent

# Internal fprintd storage slots (never forced on the user — displayed as Finger 1, Finger 2, ...)
STORAGE_SLOTS = [
    "right-index-finger",
    "left-index-finger",
    "right-thumb",
    "left-thumb",
    "right-middle-finger",
    "left-middle-finger",
    "right-ring-finger",
    "left-ring-finger",
    "right-little-finger",
    "left-little-finger",
]

LIGHT_STYLESHEET = """
QMainWindow, QWidget#root {
    background-color: #F5F5F7;
    color: #1D1D1F;
    font-family: "SF Pro Display", "Inter", "Noto Sans", "Helvetica Neue", sans-serif;
}
QPushButton {
    outline: none;
}
QLabel {
    color: #1D1D1F;
    background: transparent;
}
QLabel#heroTitle {
    font-size: 26px;
    font-weight: 700;
    color: #1D1D1F;
    letter-spacing: -0.4px;
}
QLabel#coachTitle {
    font-size: 23px;
    font-weight: 700;
    color: #1D1D1F;
    letter-spacing: -0.35px;
}
QLabel#settingsTitle {
    font-size: 21px;
    font-weight: 700;
    color: #1D1D1F;
}
QLabel#subtitle {
    font-size: 14.5px;
    color: #6E6E73;
    line-height: 1.45;
}
QLabel#sectionHeader {
    font-size: 11.5px;
    font-weight: 700;
    color: #6E6E73;
    letter-spacing: 0.45px;
}
QLabel#rowTitle {
    font-size: 14px;
    font-weight: 600;
    color: #1D1D1F;
}
QLabel#rowSub {
    font-size: 12.5px;
    color: #6E6E73;
}
QLabel#quietNote {
    font-size: 12.5px;
    font-weight: 600;
    color: #6E6E73;
}
QFrame#glassGroup {
    background-color: #FFFFFF;
    border: 1px solid rgba(15, 23, 42, 0.09);
    border-radius: 18px;
}
QFrame#hintPill {
    background-color: #FFFFFF;
    border: 1px solid rgba(15, 23, 42, 0.11);
    border-radius: 18px;
}
QFrame#hairline {
    background-color: rgba(15, 23, 42, 0.08);
    max-height: 1px;
    min-height: 1px;
}
QLabel#fingerChip {
    background-color: rgba(52, 199, 89, 0.13);
    border: 1px solid rgba(36, 138, 61, 0.28);
    border-radius: 13px;
    padding: 5px 13px;
    font-size: 12.5px;
    font-weight: 600;
    color: #248A3D;
}
QPushButton#trySensorChip {
    background-color: #FFFFFF;
    border: 1px solid rgba(15, 23, 42, 0.12);
    border-radius: 16px;
    padding: 6px 14px;
    font-size: 12.5px;
    font-weight: 600;
    color: #3A3A3C;
}
QPushButton#trySensorChip:hover {
    background-color: #F2F2F7;
}
QPushButton#trySensorChip[state="armed"] {
    background-color: rgba(0, 113, 227, 0.08);
    border: 1px solid rgba(0, 113, 227, 0.30);
    color: #0071E3;
}
QPushButton#trySensorChip[state="scanning"] {
    background-color: #0071E3;
    border: 1px solid #0071E3;
    color: #FFFFFF;
}
QPushButton#trySensorChip[state="match"] {
    background-color: rgba(52, 199, 89, 0.16);
    border: 1px solid rgba(36, 138, 61, 0.45);
    color: #248A3D;
}
QPushButton#trySensorChip[state="nomatch"] {
    background-color: rgba(255, 59, 48, 0.12);
    border: 1px solid rgba(215, 0, 21, 0.35);
    color: #D70015;
}
QLabel#statusGood {
    color: #248A3D;
    font-size: 12.5px;
    font-weight: 600;
}
QLabel#statusWarn {
    color: #B45309;
    font-size: 12.5px;
    font-weight: 600;
}
QPushButton#primaryPill {
    background: #0071E3;
    color: #FFFFFF;
    border: none;
    border-radius: 24px;
    padding: 13px 34px;
    min-width: 230px;
    min-height: 22px;
    font-size: 15px;
    font-weight: 600;
}
QPushButton#primaryPill:hover {
    background: #0077ED;
}
QPushButton#primaryPill:pressed {
    background: #0062C4;
}
QPushButton#primaryPill:disabled {
    background: #D1D5DB;
    color: #6E6E73;
}
QPushButton#sensorChooser {
    background: transparent;
    border: none;
    color: #1D1D1F;
    font-size: 13.5px;
    font-weight: 600;
    padding: 3px 12px 3px 6px;
}
QPushButton#sensorChooser:hover {
    color: #0071E3;
}
QPushButton#iconBtn {
    background-color: #FFFFFF;
    color: #3A3A3C;
    border: 1px solid rgba(15, 23, 42, 0.11);
    border-radius: 18px;
    min-width: 36px;
    max-width: 36px;
    min-height: 36px;
    max-height: 36px;
    font-size: 16px;
}
QPushButton#iconBtn:hover {
    background-color: #EAECEF;
    color: #1D1D1F;
}
QPushButton#textLink {
    background: transparent;
    color: #0071E3;
    border: none;
    padding: 8px 14px;
    font-size: 14px;
    font-weight: 600;
}
QPushButton#textLink:hover {
    color: #005BB5;
}
QPushButton#addRowLink {
    text-align: left;
    padding: 7px 0px;
    font-size: 13.5px;
    font-weight: 600;
    color: #0071E3;
    background: transparent;
    border: none;
}
QPushButton#addRowLink:hover {
    color: #005BB5;
}
QPushButton#mutedLink {
    background: transparent;
    color: #6E6E73;
    border: none;
    padding: 6px 8px;
    font-size: 14px;
    font-weight: 600;
}
QPushButton#mutedLink:hover {
    color: #1D1D1F;
}
QPushButton#smallAction {
    background-color: #F2F2F7;
    color: #1D1D1F;
    border: 1px solid rgba(15, 23, 42, 0.12);
    border-radius: 12px;
    padding: 7px 14px;
    font-size: 12.5px;
    font-weight: 600;
}
QPushButton#smallAction:hover {
    background-color: #E5E5EA;
}
QPushButton#smallDanger {
    background-color: transparent;
    color: #D70015;
    border: none;
    padding: 5px 10px;
    font-size: 12.5px;
    font-weight: 600;
}
QPushButton#smallDanger:hover {
    color: #A50010;
}
QMenu {
    background-color: #FFFFFF;
    border: 1px solid rgba(15, 23, 42, 0.14);
    border-radius: 12px;
    padding: 6px;
    color: #1D1D1F;
}
QMenu::item {
    padding: 7px 18px;
    border-radius: 8px;
    font-size: 13px;
}
QMenu::item:selected {
    background-color: #0071E3;
    color: #FFFFFF;
}
QMenu::separator {
    height: 1px;
    background: rgba(15, 23, 42, 0.08);
    margin: 5px 10px;
}
QScrollArea {
    background: transparent;
    border: none;
}
QScrollArea > QWidget > QWidget {
    background: transparent;
}
"""

DARK_STYLESHEET = """
QMainWindow, QWidget#root {
    background-color: #101218;
    color: #F5F7FA;
    font-family: "SF Pro Display", "Inter", "Noto Sans", "Helvetica Neue", sans-serif;
}
QPushButton { outline: none; }
QLabel { color: #F5F7FA; background: transparent; }
QLabel#heroTitle { font-size: 26px; font-weight: 700; color: #FFFFFF; letter-spacing: -0.4px; }
QLabel#coachTitle { font-size: 23px; font-weight: 700; color: #FFFFFF; letter-spacing: -0.35px; }
QLabel#settingsTitle { font-size: 21px; font-weight: 700; color: #FFFFFF; }
QLabel#subtitle { font-size: 14.5px; color: #98A2B3; line-height: 1.45; }
QLabel#sectionHeader { font-size: 11.5px; font-weight: 700; color: #8E98A8; letter-spacing: 0.45px; }
QLabel#rowTitle { font-size: 14px; font-weight: 600; color: #F5F7FA; }
QLabel#rowSub { font-size: 12.5px; color: #8E98A8; }
QLabel#quietNote { font-size: 12.5px; font-weight: 600; color: #8E98A8; }
QFrame#glassGroup { background-color: rgba(255, 255, 255, 0.048); border: 1px solid rgba(255, 255, 255, 0.095); border-radius: 18px; }
QFrame#hintPill { background-color: rgba(255, 255, 255, 0.055); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 18px; }
QFrame#hairline { background-color: rgba(255, 255, 255, 0.08); max-height: 1px; min-height: 1px; }
QLabel#fingerChip { background-color: rgba(48, 209, 88, 0.12); border: 1px solid rgba(48, 209, 88, 0.28); border-radius: 13px; padding: 5px 13px; font-size: 12.5px; font-weight: 600; color: #7CE89A; }
QPushButton#trySensorChip { background-color: rgba(255, 255, 255, 0.06); border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 16px; padding: 6px 14px; font-size: 12.5px; font-weight: 600; color: #C7CFD9; }
QPushButton#trySensorChip:hover { background-color: rgba(255, 255, 255, 0.12); }
QPushButton#trySensorChip[state="armed"] { background-color: rgba(10, 132, 255, 0.16); border: 1px solid rgba(10, 132, 255, 0.45); color: #64B5FF; }
QPushButton#trySensorChip[state="scanning"] { background-color: #0A84FF; border: 1px solid #0A84FF; color: #FFFFFF; }
QPushButton#trySensorChip[state="match"] { background-color: rgba(48, 209, 88, 0.18); border: 1px solid rgba(48, 209, 88, 0.50); color: #7CE89A; }
QPushButton#trySensorChip[state="nomatch"] { background-color: rgba(255, 105, 97, 0.16); border: 1px solid rgba(255, 105, 97, 0.45); color: #FF8A84; }
QLabel#statusGood { color: #30D158; font-size: 12.5px; font-weight: 600; }
QLabel#statusWarn { color: #FF9F0A; font-size: 12.5px; font-weight: 600; }
QPushButton#primaryPill { background: #0A84FF; color: #FFFFFF; border: none; border-radius: 24px; padding: 13px 34px; min-width: 230px; min-height: 22px; font-size: 15px; font-weight: 600; }
QPushButton#primaryPill:hover { background: #2693FF; }
QPushButton#primaryPill:pressed { background: #0071E3; }
QPushButton#primaryPill:disabled { background: rgba(255, 255, 255, 0.08); color: #636C7A; }
QPushButton#sensorChooser { background: transparent; border: none; color: #E5EAF2; font-size: 13.5px; font-weight: 600; padding: 3px 12px 3px 6px; }
QPushButton#sensorChooser:hover { color: #FFFFFF; }
QPushButton#iconBtn { background-color: rgba(255, 255, 255, 0.06); color: #C7CFD9; border: 1px solid rgba(255, 255, 255, 0.10); border-radius: 18px; min-width: 36px; max-width: 36px; min-height: 36px; max-height: 36px; font-size: 16px; }
QPushButton#iconBtn:hover { background-color: rgba(255, 255, 255, 0.13); color: #FFFFFF; }
QPushButton#textLink { background: transparent; color: #4DA6FF; border: none; padding: 8px 14px; font-size: 14px; font-weight: 600; }
QPushButton#textLink:hover { color: #7CC0FF; }
QPushButton#addRowLink { text-align: left; padding: 7px 0px; font-size: 13.5px; font-weight: 600; color: #4DA6FF; background: transparent; border: none; }
QPushButton#mutedLink { background: transparent; color: #8E98A8; border: none; padding: 6px 8px; font-size: 14px; font-weight: 600; }
QPushButton#mutedLink:hover { color: #E2E8F0; }
QPushButton#smallAction { background-color: rgba(255, 255, 255, 0.08); color: #F5F7FA; border: 1px solid rgba(255, 255, 255, 0.13); border-radius: 12px; padding: 7px 14px; font-size: 12.5px; font-weight: 600; }
QPushButton#smallAction:hover { background-color: rgba(255, 255, 255, 0.14); }
QPushButton#smallDanger { background-color: transparent; color: #FF6961; border: none; padding: 5px 10px; font-size: 12.5px; font-weight: 600; }
QPushButton#smallDanger:hover { color: #FF8A84; }
QMenu { background-color: #1C202B; border: 1px solid rgba(255, 255, 255, 0.14); border-radius: 12px; padding: 6px; color: #F5F7FA; }
QMenu::item { padding: 7px 18px; border-radius: 8px; font-size: 13px; }
QMenu::item:selected { background-color: #0A84FF; color: #FFFFFF; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
"""


class PressMatchWindow(QMainWindow):
    PAGE_WELCOME = 0
    PAGE_ENROLL = 1
    PAGE_DONE = 2
    PAGE_SETTINGS = 3
    _delete_done = pyqtSignal()

    def __init__(self, demo_mode: bool = False, lang: str = "en", theme: str = "light"):
        super().__init__()
        self._success_chime = SuccessChime(self)
        self.demo_mode = demo_mode
        self.lang = lang
        self.theme = theme
        self.setWindowTitle(self._t("app_title"))
        self.resize(560, 640)
        self.setMinimumSize(500, 580)

        icon_path = GUI_DIR / "pressmatch-setup.svg"
        if icon_path.exists():
            self.setWindowIcon(QIcon(str(icon_path)))

        self.devices: List[SensorDevice] = []
        self.sys_status: Dict[str, object] = {}

        # Enrollment & verify state
        self.enroll_session: Optional[EnrollSession] = None
        self.verify_session: Optional[SimultaneousVerifySession] = None
        self.enroll_queue: List[SensorDevice] = []
        self.enroll_total_sensors: int = 1
        self.enroll_sensor_index: int = 1
        self.selected_sensor_scope: str = "__both__"
        self.try_sensor_filter: Optional[str] = None  # None = both, or "elan" / "ft9201"
        self.try_sensor_chips: Dict[str, QPushButton] = {}
        self.current_enroll_finger: str = "right-index-finger"
        self.current_enroll_device: Optional[SensorDevice] = None
        self._skip_action_is_add_another: bool = False
        self._last_verify_settle_timer: Optional[QTimer] = None
        self._auto_rearm_timer: Optional[QTimer] = None
        self._silent_rearm: bool = False
        self._auto_retry_count: int = 0
        self._demo_timer: Optional[QTimer] = None
        self._demo_step = 0

        root = QWidget(self)
        root.setObjectName("root")
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(24, 16, 24, 24)
        outer.setSpacing(6)

        # ---- Minimal Top Chrome (Zero Tabs, Symmetric & Wide Enough for DE) ----
        top_bar = QHBoxLayout()
        top_bar.setContentsMargins(0, 0, 0, 0)

        left_slot = QWidget()
        left_slot.setFixedWidth(116)
        ls_lay = QHBoxLayout(left_slot)
        ls_lay.setContentsMargins(0, 0, 0, 0)
        self.btn_top_left = QPushButton(self._t("cancel"))
        self.btn_top_left.setObjectName("mutedLink")
        self.btn_top_left.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.btn_top_left.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_top_left.clicked.connect(self._on_top_left_clicked)
        ls_lay.addWidget(self.btn_top_left, 0, Qt.AlignmentFlag.AlignLeft)
        top_bar.addWidget(left_slot)

        top_bar.addStretch()

        self.lbl_top_center = QLabel("")
        self.lbl_top_center.setObjectName("quietNote")
        self.lbl_top_center.setAlignment(Qt.AlignmentFlag.AlignCenter)
        top_bar.addWidget(self.lbl_top_center)

        top_bar.addStretch()

        right_slot = QWidget()
        right_slot.setFixedWidth(116)
        rs_lay = QHBoxLayout(right_slot)
        rs_lay.setContentsMargins(0, 0, 0, 0)
        self.btn_settings = QPushButton("⚙")
        self.btn_settings.setObjectName("iconBtn")
        self.btn_settings.setToolTip(self._t("settings_tip"))
        self.btn_settings.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_settings.clicked.connect(lambda: self._go_to(self.PAGE_SETTINGS))
        rs_lay.addWidget(self.btn_settings, 0, Qt.AlignmentFlag.AlignRight)
        top_bar.addWidget(right_slot)

        outer.addLayout(top_bar)

        # ---- Single-Focus Stacked Pages ----
        self.stack = QStackedWidget()
        outer.addWidget(self.stack, 1)

        self.stack.addWidget(self._build_welcome_page())
        self.stack.addWidget(self._build_enroll_page())
        self.stack.addWidget(self._build_done_page())
        self.stack.addWidget(self._build_settings_page())

        self.refresh_state()
        self._delete_done.connect(self.refresh_state)
        self._go_to(self.PAGE_WELCOME)

    def _t(self, key: str, **kwargs) -> str:
        return tr(self.lang, key, **kwargs)

    def _dev_name(self, dev: Optional[SensorDevice]) -> str:
        return sensor_name(self.lang, dev.sensor_id if dev else "generic")

    def _dev_phrase(self, dev: Optional[SensorDevice]) -> str:
        return sensor_phrase(self.lang, dev.sensor_id if dev else "generic")

    # =========================================================================
    # Page 0: Welcome (Calm, Iconic, Single Primary Choice)
    # =========================================================================

    def _build_welcome_page(self) -> QWidget:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(16, 14, 16, 8)
        lay.setSpacing(0)

        lay.addStretch(2)

        hero_row = QHBoxLayout()
        hero_row.addStretch()
        self.hero_fp = TouchIdFingerprintWidget(mode="ambient", theme=self.theme)
        hero_row.addWidget(self.hero_fp)
        hero_row.addStretch()
        lay.addLayout(hero_row)

        lay.addSpacing(24)

        self.lbl_welcome_title = QLabel(self._t("welcome_title"))
        self.lbl_welcome_title.setObjectName("heroTitle")
        self.lbl_welcome_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_welcome_title.setWordWrap(True)
        lay.addWidget(self.lbl_welcome_title)

        lay.addSpacing(10)

        self.lbl_welcome_sub = QLabel(self._t("welcome_sub_single"))
        self.lbl_welcome_sub.setObjectName("subtitle")
        self.lbl_welcome_sub.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_welcome_sub.setWordWrap(True)
        self.lbl_welcome_sub.setMaximumWidth(430)
        sub_wrap = QHBoxLayout()
        sub_wrap.addStretch()
        sub_wrap.addWidget(self.lbl_welcome_sub)
        sub_wrap.addStretch()
        lay.addLayout(sub_wrap)

        lay.addSpacing(22)

        self.welcome_chips_wrap = QWidget()
        self.welcome_chips_layout = QHBoxLayout(self.welcome_chips_wrap)
        self.welcome_chips_layout.setContentsMargins(0, 0, 0, 0)
        self.welcome_chips_layout.setSpacing(8)
        lay.addWidget(self.welcome_chips_wrap)

        lay.addStretch(3)

        btn_row = QHBoxLayout()
        btn_row.addStretch()
        self.btn_welcome_primary = QPushButton(self._t("welcome_setup"))
        self.btn_welcome_primary.setObjectName("primaryPill")
        self.btn_welcome_primary.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_welcome_primary.clicked.connect(self._begin_quick_enroll)
        btn_row.addWidget(self.btn_welcome_primary)
        btn_row.addStretch()
        lay.addLayout(btn_row)

        lay.addSpacing(6)

        sec_row = QHBoxLayout()
        sec_row.addStretch()
        self.btn_welcome_try = QPushButton(self._t("welcome_try"))
        self.btn_welcome_try.setObjectName("textLink")
        self.btn_welcome_try.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_welcome_try.clicked.connect(lambda: self._open_done_try_mode(just_enrolled=False))
        sec_row.addWidget(self.btn_welcome_try)
        sec_row.addStretch()
        lay.addLayout(sec_row)

        return page

    # =========================================================================
    # Page 1: Guided Touch ID Enrollment (Pure, Human, Finger-Agnostic)
    # =========================================================================

    def _build_enroll_page(self) -> QWidget:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(12, 4, 12, 8)
        lay.setSpacing(0)

        pill_row = QHBoxLayout()
        pill_row.addStretch()

        self.enroll_hint_pill = QFrame()
        self.enroll_hint_pill.setObjectName("hintPill")
        hp_lay = QHBoxLayout(self.enroll_hint_pill)
        hp_lay.setContentsMargins(12, 5, 10, 5)
        hp_lay.setSpacing(6)
        self.enroll_sensor_icon = SensorHintIcon("strip", theme=self.theme)
        hp_lay.addWidget(self.enroll_sensor_icon)

        self.btn_finger_chooser = QPushButton(f"{self._t('sensor_elan')} ▾")
        self.btn_finger_chooser.setObjectName("sensorChooser")
        self.btn_finger_chooser.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_finger_chooser.clicked.connect(self._open_sensor_menu)
        hp_lay.addWidget(self.btn_finger_chooser)

        pill_row.addWidget(self.enroll_hint_pill)
        pill_row.addStretch()
        lay.addLayout(pill_row)

        lay.addStretch(1)

        fp_row = QHBoxLayout()
        fp_row.addStretch()
        self.fp_enroll = TouchIdFingerprintWidget(mode="enroll", theme=self.theme)
        fp_row.addWidget(self.fp_enroll)
        fp_row.addStretch()
        lay.addLayout(fp_row)

        lay.addSpacing(22)

        self.lbl_coach_title = QLabel(self._t("place_title"))
        self.lbl_coach_title.setObjectName("coachTitle")
        self.lbl_coach_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_coach_title.setWordWrap(True)
        lay.addWidget(self.lbl_coach_title)

        lay.addSpacing(8)

        self.lbl_coach_sub = QLabel(self._t("place_sub", sensor=self._t("sensor_generic_phrase")))
        self.lbl_coach_sub.setObjectName("subtitle")
        self.lbl_coach_sub.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_coach_sub.setWordWrap(True)
        self.lbl_coach_sub.setMaximumWidth(420)
        cs_wrap = QHBoxLayout()
        cs_wrap.addStretch()
        cs_wrap.addWidget(self.lbl_coach_sub)
        cs_wrap.addStretch()
        lay.addLayout(cs_wrap)

        lay.addStretch(2)

        cta_row = QHBoxLayout()
        cta_row.addStretch()
        self.btn_enroll_continue = QPushButton(self._t("continue"))
        self.btn_enroll_continue.setObjectName("primaryPill")
        self.btn_enroll_continue.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_enroll_continue.clicked.connect(self._on_enroll_continue)
        self.btn_enroll_continue.setVisible(False)
        cta_row.addWidget(self.btn_enroll_continue)
        cta_row.addStretch()
        lay.addLayout(cta_row)

        lay.addSpacing(6)

        skip_row = QHBoxLayout()
        skip_row.addStretch()
        self.btn_enroll_skip = QPushButton("")
        self.btn_enroll_skip.setObjectName("textLink")
        self.btn_enroll_skip.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_enroll_skip.clicked.connect(self._on_enroll_skip_rest)
        self.btn_enroll_skip.setVisible(False)
        skip_row.addWidget(self.btn_enroll_skip)
        skip_row.addStretch()
        lay.addLayout(skip_row)

        return page

    # =========================================================================
    # Page 2: All Set + Playful Live Try-It Unlock
    # =========================================================================

    def _build_done_page(self) -> QWidget:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(16, 4, 16, 8)
        lay.setSpacing(0)

        # Live armed reader pills (Power Button + USB Reader — clickable to test both or single)
        self.try_chips_wrap = QWidget()
        self.try_chips_lay = QHBoxLayout(self.try_chips_wrap)
        self.try_chips_lay.setContentsMargins(0, 0, 0, 0)
        self.try_chips_lay.setSpacing(8)
        lay.addWidget(self.try_chips_wrap)

        lay.addStretch(1)

        fp_row = QHBoxLayout()
        fp_row.addStretch()
        self.fp_verify = TouchIdFingerprintWidget(mode="verify", theme=self.theme)
        fp_row.addWidget(self.fp_verify)
        fp_row.addStretch()
        lay.addLayout(fp_row)

        lay.addSpacing(22)

        self.lbl_done_title = QLabel(self._t("all_set_title"))
        self.lbl_done_title.setObjectName("heroTitle")
        self.lbl_done_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        lay.addWidget(self.lbl_done_title)

        lay.addSpacing(8)

        self.lbl_done_sub = QLabel(self._t("all_set_sub"))
        self.lbl_done_sub.setObjectName("subtitle")
        self.lbl_done_sub.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_done_sub.setWordWrap(True)
        self.lbl_done_sub.setMaximumWidth(420)
        ds_wrap = QHBoxLayout()
        ds_wrap.addStretch()
        ds_wrap.addWidget(self.lbl_done_sub)
        ds_wrap.addStretch()
        lay.addLayout(ds_wrap)

        lay.addStretch(2)

        btn_row = QHBoxLayout()
        btn_row.addStretch()
        self.btn_done_finish = QPushButton(self._t("done"))
        self.btn_done_finish.setObjectName("primaryPill")
        self.btn_done_finish.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_done_finish.clicked.connect(lambda: self._go_to(self.PAGE_WELCOME))
        btn_row.addWidget(self.btn_done_finish)
        btn_row.addStretch()
        lay.addLayout(btn_row)

        lay.addSpacing(6)

        add_row = QHBoxLayout()
        add_row.addStretch()
        btn_add_more = QPushButton(self._t("add_another"))
        btn_add_more.setObjectName("textLink")
        btn_add_more.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_add_more.clicked.connect(self._begin_quick_enroll)
        add_row.addWidget(btn_add_more)
        add_row.addStretch()
        lay.addLayout(add_row)

        return page

    # =========================================================================
    # Page 3: Quiet Settings Sheet (Apple Grouped List, Plain DE/EN)
    # =========================================================================

    def _build_settings_page(self) -> QWidget:
        page = QWidget()
        outer = QVBoxLayout(page)
        outer.setContentsMargins(6, 4, 6, 4)
        outer.setSpacing(12)

        title = QLabel(self._t("settings_title"))
        title.setObjectName("settingsTitle")
        outer.addWidget(title)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        lay = QVBoxLayout(content)
        lay.setContentsMargins(0, 4, 4, 8)
        lay.setSpacing(14)

        h1 = QLabel(self._t("sec_saved"))
        h1.setObjectName("sectionHeader")
        lay.addWidget(h1)

        self.settings_fingers_group = QFrame()
        self.settings_fingers_group.setObjectName("glassGroup")
        self.settings_fingers_lay = QVBoxLayout(self.settings_fingers_group)
        self.settings_fingers_lay.setContentsMargins(16, 12, 16, 12)
        self.settings_fingers_lay.setSpacing(8)
        lay.addWidget(self.settings_fingers_group)

        h2 = QLabel(self._t("sec_sys"))
        h2.setObjectName("sectionHeader")
        lay.addWidget(h2)

        sys_group = QFrame()
        sys_group.setObjectName("glassGroup")
        sg_lay = QVBoxLayout(sys_group)
        sg_lay.setContentsMargins(16, 14, 16, 14)
        sg_lay.setSpacing(12)

        r1 = QHBoxLayout()
        r1_txt = QVBoxLayout()
        r1_txt.setSpacing(2)
        r1_t = QLabel(self._t("unlock_title"))
        r1_t.setObjectName("rowTitle")
        r1_s = QLabel(self._t("unlock_sub"))
        r1_s.setObjectName("rowSub")
        r1_s.setWordWrap(True)
        r1_txt.addWidget(r1_t)
        r1_txt.addWidget(r1_s)
        r1.addLayout(r1_txt, 1)
        self.lbl_set_unlock_status = QLabel(self._t("status_active"))
        self.lbl_set_unlock_status.setObjectName("statusGood")
        r1.addWidget(self.lbl_set_unlock_status)
        self.btn_set_unlock = QPushButton(self._t("enable"))
        self.btn_set_unlock.setObjectName("smallAction")
        self.btn_set_unlock.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_set_unlock.clicked.connect(self._run_dual_pam_install)
        r1.addWidget(self.btn_set_unlock)
        sg_lay.addLayout(r1)

        sep = QFrame()
        sep.setObjectName("hairline")
        sg_lay.addWidget(sep)

        r2 = QHBoxLayout()
        r2_txt = QVBoxLayout()
        r2_txt.setSpacing(2)
        r2_t = QLabel(self._t("drv_title"))
        r2_t.setObjectName("rowTitle")
        r2_s = QLabel(self._t("drv_sub"))
        r2_s.setObjectName("rowSub")
        r2_s.setWordWrap(True)
        r2_txt.addWidget(r2_t)
        r2_txt.addWidget(r2_s)
        r2.addLayout(r2_txt, 1)
        self.lbl_set_drv_status = QLabel(self._t("status_uptodate"))
        self.lbl_set_drv_status.setObjectName("statusGood")
        r2.addWidget(self.lbl_set_drv_status)
        self.btn_set_drv = QPushButton(self._t("update"))
        self.btn_set_drv.setObjectName("smallAction")
        self.btn_set_drv.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_set_drv.clicked.connect(self._run_driver_update)
        r2.addWidget(self.btn_set_drv)
        sg_lay.addLayout(r2)

        lay.addWidget(sys_group)

        self.btn_restore_defaults = QPushButton(self._t("restore_btn"))
        self.btn_restore_defaults.setObjectName("smallDanger")
        self.btn_restore_defaults.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_restore_defaults.clicked.connect(self._run_dual_pam_rollback)
        lay.addWidget(self.btn_restore_defaults, 0, Qt.AlignmentFlag.AlignLeft)

        lay.addStretch()
        scroll.setWidget(content)
        outer.addWidget(scroll, 1)
        return page

    # =========================================================================
    # State & Navigation
    # =========================================================================

    def _stop_sessions(self) -> None:
        if self._demo_timer and self._demo_timer.isActive():
            self._demo_timer.stop()
        if self._last_verify_settle_timer and self._last_verify_settle_timer.isActive():
            self._last_verify_settle_timer.stop()
        if self._auto_rearm_timer and self._auto_rearm_timer.isActive():
            self._auto_rearm_timer.stop()
        if self.enroll_session:
            self.enroll_session.cancel()
            self.enroll_session = None
        if self.verify_session:
            self.verify_session.stop(wait=True)
            self.verify_session = None

    def _on_top_left_clicked(self) -> None:
        self._stop_sessions()
        self.enroll_queue.clear()
        self._go_to(self.PAGE_WELCOME)

    def _go_to(self, page_idx: int) -> None:
        if page_idx != self.PAGE_ENROLL and page_idx != self.PAGE_DONE:
            self._stop_sessions()
        self.stack.setCurrentIndex(page_idx)

        if page_idx == self.PAGE_WELCOME:
            self.btn_top_left.setVisible(False)
            self.lbl_top_center.setText("")
            self.btn_settings.setVisible(True)
            self.refresh_state()
        elif page_idx == self.PAGE_ENROLL:
            self.btn_top_left.setText(self._t("cancel"))
            self.btn_top_left.setVisible(True)
            self.btn_settings.setVisible(False)
        elif page_idx == self.PAGE_DONE:
            self.btn_top_left.setText(self._t("back"))
            self.btn_top_left.setVisible(True)
            self.lbl_top_center.setText("")
            self.btn_settings.setVisible(True)
        elif page_idx == self.PAGE_SETTINGS:
            self.btn_top_left.setText(self._t("back"))
            self.btn_top_left.setVisible(True)
            self.lbl_top_center.setText("")
            self.btn_settings.setVisible(False)
            self.refresh_state()

    def _enrolled_finger_map(self) -> Dict[str, List[SensorDevice]]:
        raw: Dict[str, List[SensorDevice]] = {}
        for dev in self.devices:
            for f in dev.fingers:
                raw.setdefault(f, []).append(dev)
        ordered: Dict[str, List[SensorDevice]] = {}
        for slot in STORAGE_SLOTS:
            if slot in raw:
                ordered[slot] = raw[slot]
        for slot, devs in raw.items():
            if slot not in ordered:
                ordered[slot] = devs
        return ordered

    def _slot_display_names(self) -> Dict[str, str]:
        fmap = self._enrolled_finger_map()
        return {slot: self._t("finger_n", n=idx + 1) for idx, slot in enumerate(fmap.keys())}

    def _pick_next_free_slot(self, target_devs: List[SensorDevice]) -> Optional[str]:
        used = set()
        for dev in target_devs:
            used.update(dev.fingers)
        for slot in STORAGE_SLOTS:
            if slot not in used:
                return slot
        return None

    def refresh_state(self) -> None:
        self.devices = discover_devices()
        self.sys_status = get_system_integration_status()
        self._update_welcome_ui()
        self._update_settings_ui()

    def _update_welcome_ui(self) -> None:
        fmap = self._enrolled_finger_map()
        has_devices = bool(self.devices)
        has_fingers = bool(fmap)

        if not has_devices:
            self.lbl_welcome_title.setText(self._t("welcome_no_dev_title"))
            self.lbl_welcome_sub.setText(self._t("welcome_no_dev_sub"))
            self.btn_welcome_primary.setText(self._t("welcome_check_again"))
            self.btn_welcome_primary.setEnabled(True)
            self.btn_welcome_try.setVisible(False)
        else:
            self.lbl_welcome_title.setText(self._t("welcome_title"))
            if len(self.devices) >= 2:
                self.lbl_welcome_sub.setText(self._t("welcome_sub_dual"))
            else:
                self.lbl_welcome_sub.setText(self._t("welcome_sub_single"))
            self.btn_welcome_primary.setText(
                self._t("welcome_add") if has_fingers else self._t("welcome_setup")
            )
            self.btn_welcome_primary.setEnabled(True)
            self.btn_welcome_try.setVisible(has_fingers)

        self._clear_layout(self.welcome_chips_layout)

        if has_fingers:
            names = self._slot_display_names()
            self.welcome_chips_wrap.setVisible(True)
            self.welcome_chips_layout.addStretch()
            shown = list(fmap.keys())[:3]
            for f in shown:
                chip = QLabel(f"✓  {names.get(f, self._t('finger_n', n=1))}")
                chip.setObjectName("fingerChip")
                self.welcome_chips_layout.addWidget(chip)
            if len(fmap) > 3:
                more = QLabel(self._t("more_fingers", n=len(fmap) - 3))
                more.setObjectName("quietNote")
                self.welcome_chips_layout.addWidget(more)
            self.welcome_chips_layout.addStretch()
        else:
            self.welcome_chips_wrap.setVisible(False)

    def _update_settings_ui(self) -> None:
        self._clear_layout(self.settings_fingers_lay)

        fmap = self._enrolled_finger_map()
        names = self._slot_display_names()
        if not fmap:
            empty = QLabel(self._t("no_saved"))
            empty.setObjectName("rowSub")
            empty.setContentsMargins(0, 6, 0, 6)
            self.settings_fingers_lay.addWidget(empty)
        else:
            items = list(fmap.items())
            for idx, (finger, devs) in enumerate(items):
                row = QHBoxLayout()
                col = QVBoxLayout()
                col.setSpacing(1)
                t_lbl = QLabel(names.get(finger, self._t("finger_n", n=idx + 1)))
                t_lbl.setObjectName("rowTitle")
                sub = QLabel(" & ".join(self._dev_name(d) for d in devs))
                sub.setObjectName("rowSub")
                col.addWidget(t_lbl)
                col.addWidget(sub)
                row.addLayout(col, 1)

                btn_rm = QPushButton(self._t("remove"))
                btn_rm.setObjectName("smallDanger")
                btn_rm.setCursor(Qt.CursorShape.PointingHandCursor)
                btn_rm.clicked.connect(lambda _c, f=finger: self._remove_finger(f))
                row.addWidget(btn_rm)
                self.settings_fingers_lay.addLayout(row)

                if idx < len(items) - 1:
                    sep = QFrame()
                    sep.setObjectName("hairline")
                    self.settings_fingers_lay.addWidget(sep)

        sep_add = QFrame()
        sep_add.setObjectName("hairline")
        self.settings_fingers_lay.addWidget(sep_add)
        btn_add = QPushButton(self._t("add_row"))
        btn_add.setObjectName("addRowLink")
        btn_add.setCursor(Qt.CursorShape.PointingHandCursor)
        btn_add.clicked.connect(self._begin_quick_enroll)
        self.settings_fingers_lay.addWidget(btn_add)

        pam_ok = bool(self.sys_status.get("dual_pam_active"))
        drv_ok = bool(self.sys_status.get("driver_active"))
        tel_ok = bool(self.sys_status.get("telemetry_ready"))

        if pam_ok:
            self.lbl_set_unlock_status.setText(self._t("status_active"))
            self.lbl_set_unlock_status.setObjectName("statusGood")
            self.btn_set_unlock.setVisible(False)
            self.btn_restore_defaults.setVisible(True)
        else:
            self.lbl_set_unlock_status.setText("")
            self.btn_set_unlock.setText(self._t("enable"))
            self.btn_set_unlock.setVisible(True)
            self.btn_restore_defaults.setVisible(False)

        if drv_ok and tel_ok:
            self.lbl_set_drv_status.setText(self._t("status_uptodate"))
            self.lbl_set_drv_status.setObjectName("statusGood")
            self.btn_set_drv.setVisible(False)
        else:
            self.lbl_set_drv_status.setText(self._t("status_update_avail"))
            self.lbl_set_drv_status.setObjectName("statusWarn")
            self.btn_set_drv.setText(self._t("update"))
            self.btn_set_drv.setVisible(True)

        for lbl in (self.lbl_set_unlock_status, self.lbl_set_drv_status):
            lbl.style().unpolish(lbl)
            lbl.style().polish(lbl)

    @staticmethod
    def _clear_layout(lay) -> None:
        while lay.count():
            it = lay.takeAt(0)
            w = it.widget()
            if w is not None:
                w.hide()
                w.setParent(None)
                w.deleteLater()
            elif it.layout() is not None:
                PressMatchWindow._clear_layout(it.layout())

    def _remove_finger(self, finger: str) -> None:
        nice = self._slot_display_names().get(finger, self._t("finger_generic"))
        targets = [d for d in self.devices if finger in d.fingers]
        if not targets:
            return
        reply = QMessageBox.question(
            self,
            self._t("remove_title"),
            self._t("remove_confirm", name=nice),
        )
        if reply != QMessageBox.StandardButton.Yes:
            return

        def _worker():
            for d in targets:
                delete_enrolled_finger(d.path, finger)
            self._delete_done.emit()

        threading.Thread(target=_worker, daemon=True).start()

    # =========================================================================
    # Seamless Touch ID Enrollment Flow (100% Finger-Agnostic!)
    # =========================================================================

    def _begin_quick_enroll(self) -> None:
        self.refresh_state()
        if not self.devices:
            return
        self.selected_sensor_scope = "__both__"
        self._start_enrollment_queue()

    def _start_enrollment_queue(self) -> None:
        if not self.devices:
            return
        if self.selected_sensor_scope == "__both__":
            targets = list(self.devices)
        else:
            targets = [d for d in self.devices if d.path == self.selected_sensor_scope]
            if not targets:
                targets = [self.devices[0]]

        slot = self._pick_next_free_slot(targets)
        if slot is None:
            QMessageBox.information(
                self,
                self._t("full_dialog_title"),
                self._t("full_dialog_body"),
            )
            return

        self.current_enroll_finger = slot
        self.enroll_queue = list(targets)
        self.enroll_total_sensors = len(self.enroll_queue)
        self.enroll_sensor_index = 0
        self._advance_enroll_queue()

    def _advance_enroll_queue(self, silent_restart: bool = False) -> None:
        self._stop_sessions()
        if not silent_restart:
            self._auto_retry_count = 0
            if not self.enroll_queue:
                self._open_done_try_mode(just_enrolled=True)
                return
            dev = self.enroll_queue.pop(0)
            self.enroll_sensor_index += 1
            self.current_enroll_device = dev
        else:
            dev = self.current_enroll_device
            if dev is None:
                return

        self._silent_rearm = silent_restart
        self._go_to(self.PAGE_ENROLL)

        self.enroll_sensor_icon.set_shape(dev.shape)
        d_name = self._dev_name(dev)
        if len(self.devices) >= 2:
            self.btn_finger_chooser.setText(f"{d_name} ▾")
        else:
            self.btn_finger_chooser.setText(d_name)
        self.btn_finger_chooser.adjustSize()
        self.enroll_hint_pill.adjustSize()

        if self.enroll_total_sensors > 1:
            self.lbl_top_center.setText(
                self._t("sensor_step", idx=self.enroll_sensor_index, total=self.enroll_total_sensors)
            )
        else:
            self.lbl_top_center.setText("")

        if not silent_restart:
            self.fp_enroll.reset_enroll()
            self.lbl_coach_title.setText(self._t("prep_title"))
            self.lbl_coach_sub.setText(self._t("prep_sub", sensor=self._dev_phrase(dev)))
            self.btn_enroll_continue.setVisible(False)
            self.btn_enroll_skip.setVisible(False)
        else:
            self.fp_enroll.reset_enroll()
        self.btn_top_left.setText(self._t("cancel"))

        sess = EnrollSession(dev, self.current_enroll_finger)
        sess.authorizing.connect(self._on_enroll_authorizing)
        sess.auth_cancelled.connect(self._on_enroll_auth_cancelled)
        sess.started.connect(self._on_enroll_ready)
        sess.finger_present_changed.connect(self._on_enroll_finger_present)
        sess.progress.connect(self._on_enroll_progress)
        sess.completed.connect(self._on_enroll_completed)
        self.enroll_session = sess
        sess.start()

    def _on_enroll_authorizing(self) -> None:
        self.lbl_coach_title.setText(self._t("auth_title"))
        has_saved = any(d.fingers for d in self.devices)
        self.lbl_coach_sub.setText(
            self._t("auth_sub_saved") if has_saved else self._t("auth_sub_pass")
        )

    def _on_enroll_auth_cancelled(self) -> None:
        self.enroll_session = None
        self.enroll_queue.clear()
        self._go_to(self.PAGE_WELCOME)

    def _on_enroll_ready(self, _info: dict) -> None:
        if self._silent_rearm:
            self._silent_rearm = False
            return
        phrase = self._dev_phrase(self.current_enroll_device)
        if self.enroll_sensor_index > 1:
            self.lbl_coach_title.setText(self._t("same_finger_title"))
            self.lbl_coach_sub.setText(self._t("same_finger_sub", sensor=phrase))
        else:
            self.lbl_coach_title.setText(self._t("place_title"))
            self.lbl_coach_sub.setText(self._t("place_sub", sensor=phrase))

    def _open_sensor_menu(self) -> None:
        if len(self.devices) < 2:
            return
        menu = QMenu(self)
        act_both = QAction(self._t("menu_both"), menu)
        act_both.setCheckable(True)
        act_both.setChecked(self.selected_sensor_scope == "__both__")
        act_both.triggered.connect(lambda: self._switch_sensor_scope("__both__"))
        menu.addAction(act_both)

        for d in self.devices:
            act_d = QAction(self._t("menu_only", name=self._dev_name(d)), menu)
            act_d.setCheckable(True)
            act_d.setChecked(self.selected_sensor_scope == d.path)
            act_d.triggered.connect(lambda _c, p=d.path: self._switch_sensor_scope(p))
            menu.addAction(act_d)

        pos = self.enroll_hint_pill.mapToGlobal(self.enroll_hint_pill.rect().bottomLeft())
        menu.exec(pos)

    def _switch_sensor_scope(self, scope: str) -> None:
        self.selected_sensor_scope = scope
        self._start_enrollment_queue()

    def _on_enroll_finger_present(self, present: bool) -> None:
        self.fp_enroll.set_finger_present(present)
        if present and self.enroll_session is not None:
            self.btn_enroll_continue.setVisible(False)
            self.lbl_coach_title.setText(self._t("hold_still"))

    def _schedule_silent_rearm(self, delay_ms: int = 900) -> None:
        if self._auto_rearm_timer and self._auto_rearm_timer.isActive():
            self._auto_rearm_timer.stop()
        self._auto_rearm_timer = QTimer(self)
        self._auto_rearm_timer.setSingleShot(True)
        self._auto_rearm_timer.timeout.connect(self._do_silent_rearm)
        self._auto_rearm_timer.start(delay_ms)

    def _do_silent_rearm(self) -> None:
        if self.stack.currentIndex() != self.PAGE_ENROLL:
            return
        self._advance_enroll_queue(silent_restart=True)

    def _on_enroll_progress(self, payload: dict) -> None:
        self.fp_enroll.apply_telemetry(payload)
        telem = payload.get("telemetry")
        dbus_res = payload.get("dbus_result", "")
        stage = int(payload.get("stage", 0))
        total = max(int(payload.get("total_stages", 10)), 1)

        if telem and isinstance(telem, dict):
            pct = int(telem.get("progress_pct", int(100 * stage / total)))
            st = str(telem.get("status", ""))
            strips = int(telem.get("strips", stage))
            hdx = float(telem.get("hint_dx", 0.0))
            hdy = float(telem.get("hint_dy", 0.0))
            poses = telem.get("poses") or []
            degs = [float(p.get("deg", 0.0)) for p in poses]
            ang_span = (max(degs) - min(degs)) if len(degs) >= 2 else 0.0

            if st == "complete" or dbus_res == "enroll-completed":
                self._show_step_complete_copy()
            elif st == "saturated":
                self.lbl_coach_title.setText(self._t("lighter_title"))
                self.lbl_coach_sub.setText(self._t("lighter_sub"))
            elif st == "redundant":
                self.lbl_coach_title.setText(self._t("different_spot_title"))
                self.lbl_coach_sub.setText(friendly_direction(self.lang, hdx, hdy))
            elif st in ("low_quality", "low_coverage"):
                self.lbl_coach_title.setText(self._t("flat_title"))
                self.lbl_coach_sub.setText(self._t("flat_sub"))
            else:
                if pct >= 55 and ang_span < 14.0 and strips >= 4:
                    side = self._t("side_left") if (strips % 2 == 0) else self._t("side_right")
                    self.lbl_coach_title.setText(self._t("adjust_title"))
                    self.lbl_coach_sub.setText(self._t("tilt_sub", side=side))
                elif pct >= 82:
                    self.lbl_coach_title.setText(self._t("almost_title"))
                    self.lbl_coach_sub.setText(friendly_direction(self.lang, hdx, hdy))
                elif pct >= 55:
                    self.lbl_coach_title.setText(self._t("adjust_title"))
                    self.lbl_coach_sub.setText(friendly_direction(self.lang, hdx, hdy))
                else:
                    self.lbl_coach_title.setText(self._t("lift_rest_title"))
                    self.lbl_coach_sub.setText(friendly_direction(self.lang, hdx, hdy))
        else:
            pct = int(100 * stage / total)
            if dbus_res == "enroll-completed":
                self._show_step_complete_copy()
            elif dbus_res == "enroll-remove-and-retry":
                self.lbl_coach_title.setText(self._t("lighter_title"))
                self.lbl_coach_sub.setText(self._t("lighter_sub_lift"))
            elif dbus_res == "enroll-finger-not-centered":
                self.lbl_coach_title.setText(self._t("adjust_title"))
                self.lbl_coach_sub.setText(self._t("not_centered_sub"))
            elif dbus_res == "enroll-retry-scan":
                self.lbl_coach_title.setText(self._t("retry_title"))
                self.lbl_coach_sub.setText(self._t("retry_sub"))
            else:
                if pct >= 82:
                    self.lbl_coach_title.setText(self._t("almost_title"))
                    self.lbl_coach_sub.setText(self._t("almost_sub_fallback"))
                elif pct >= 55:
                    self.lbl_coach_title.setText(self._t("adjust_title"))
                    self.lbl_coach_sub.setText(self._t("adjust_fallback"))
                else:
                    self.lbl_coach_title.setText(self._t("lift_rest_title"))
                    self.lbl_coach_sub.setText(self._t("lift_rest_fallback"))

    def _show_step_complete_copy(self) -> None:
        if self.enroll_queue:
            nxt = self.enroll_queue[0]
            cur_name = self._dev_name(self.current_enroll_device)
            nxt_name = self._dev_name(nxt)
            self._skip_action_is_add_another = False
            self.lbl_coach_title.setText(self._t("sensor_ready_title", name=cur_name))
            self.lbl_coach_sub.setText(
                self._t("sensor_ready_sub", next_phrase=self._dev_phrase(nxt))
            )
            self.btn_enroll_continue.setText(self._t("setup_next", name=nxt_name))
            self.btn_enroll_skip.setText(self._t("skip_next", name=nxt_name))
            self.btn_enroll_skip.setVisible(True)
        else:
            self._skip_action_is_add_another = True
            self.lbl_coach_title.setText(self._t("finger_added_title"))
            self.lbl_coach_sub.setText(self._t("finger_added_sub"))
            self.btn_enroll_continue.setText(self._t("done"))
            self.btn_enroll_skip.setText(self._t("add_another"))
            self.btn_enroll_skip.setVisible(True)

    def _on_enroll_completed(self, success: bool, code: str, _message: str) -> None:
        self.enroll_session = None
        if success:
            self._auto_retry_count = 0
            self.refresh_state()
            self._show_step_complete_copy()
            self.btn_enroll_continue.setVisible(True)
            self.btn_top_left.setText(self._t("cancel"))
            return

        self.fp_enroll.trigger_shake()
        dev_name = self._dev_name(self.current_enroll_device)

        if code == "duplicate":
            if self.enroll_sensor_index > 1:
                self.lbl_coach_title.setText(self._t("dup_second_title", name=dev_name))
                self.lbl_coach_sub.setText(self._t("dup_second_sub", name=dev_name))
                self.btn_enroll_continue.setText(self._t("done"))
                self.btn_enroll_continue.setVisible(True)
                self.btn_enroll_skip.setVisible(False)
                self._schedule_silent_rearm(900)
            else:
                self.lbl_coach_title.setText(self._t("dup_title"))
                self.lbl_coach_sub.setText(self._t("dup_sub"))
                self.btn_enroll_continue.setVisible(False)
                self.btn_enroll_skip.setVisible(False)
                self._schedule_silent_rearm(900)
        elif code == "data_full":
            self.lbl_coach_title.setText(self._t("full_title"))
            self.lbl_coach_sub.setText(self._t("full_sub"))
            self.btn_enroll_continue.setText(self._t("done"))
            self.btn_enroll_continue.setVisible(True)
            self.btn_enroll_skip.setVisible(False)
        elif code == "disconnected":
            self.lbl_coach_title.setText(self._t("disc_title"))
            self.lbl_coach_sub.setText(self._t("disc_sub"))
            self.btn_enroll_continue.setVisible(False)
            self.btn_enroll_skip.setVisible(False)
            self._schedule_silent_rearm(1800)
        else:
            self._auto_retry_count += 1
            self.lbl_coach_title.setText(self._t("rearm_title"))
            self.lbl_coach_sub.setText(self._t("rearm_sub"))
            self.btn_enroll_continue.setVisible(False)
            self.btn_enroll_skip.setVisible(False)
            if self._auto_retry_count <= 8:
                self._schedule_silent_rearm(1000)
            else:
                self.lbl_coach_sub.setText(self._t("rearm_stuck"))

    def _on_enroll_continue(self) -> None:
        self._stop_sessions()
        self._advance_enroll_queue()

    def _on_enroll_skip_rest(self) -> None:
        if self._skip_action_is_add_another:
            self._begin_quick_enroll()
        else:
            self.enroll_queue.clear()
            self._open_done_try_mode(just_enrolled=True)

    # =========================================================================
    # Page 2: All Set + Playful Interactive Try-It (Power Button + USB Reader)
    # =========================================================================

    def _rebuild_try_sensor_chips(self, armed_ids: Optional[set] = None) -> None:
        self._clear_layout(self.try_chips_lay)
        self.try_sensor_chips.clear()
        enrolled_devs = [d for d in self.devices if d.fingers]
        if not enrolled_devs:
            self.try_chips_wrap.setVisible(False)
            return

        self.try_chips_wrap.setVisible(True)
        self.try_chips_lay.addStretch()

        if len(enrolled_devs) >= 2:
            btn_both = QPushButton(self._t("try_mode_both"))
            btn_both.setObjectName("trySensorChip")
            btn_both.setCursor(Qt.CursorShape.PointingHandCursor)
            btn_both.setProperty("state", "armed" if self.try_sensor_filter is None else "idle")
            btn_both.clicked.connect(lambda: self._select_try_filter(None))
            self.try_chips_lay.addWidget(btn_both)
            self.try_sensor_chips["__both__"] = btn_both

        for dev in enrolled_devs:
            name = self._dev_name(dev)
            is_armed = armed_ids is not None and dev.sensor_id in armed_ids
            dot = "●  " if is_armed else "○  "
            btn = QPushButton(f"{dot}{name}")
            btn.setObjectName("trySensorChip")
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            state = "armed" if is_armed else "idle"
            btn.setProperty("state", state)
            btn.clicked.connect(lambda _c, sid=dev.sensor_id: self._select_try_filter(sid))
            self.try_chips_lay.addWidget(btn)
            self.try_sensor_chips[dev.sensor_id] = btn

        self.try_chips_lay.addStretch()

    def _set_try_chip_state(self, sensor_id: str, state: str, prefix: str = "●  ") -> None:
        btn = self.try_sensor_chips.get(sensor_id)
        if not btn:
            return
        dev = next((d for d in self.devices if d.sensor_id == sensor_id), None)
        if dev:
            btn.setText(f"{prefix}{self._dev_name(dev)}")
        btn.setProperty("state", state)
        btn.style().unpolish(btn)
        btn.style().polish(btn)

    def _select_try_filter(self, sensor_id: Optional[str]) -> None:
        self.try_sensor_filter = sensor_id
        self._stop_sessions()
        self.lbl_done_title.setText(self._t("try_title"))
        self._update_try_subtitle()
        self._arm_live_try()

    def _update_try_subtitle(self, armed_devs: Optional[List[SensorDevice]] = None) -> None:
        devs = armed_devs if armed_devs is not None else [
            d for d in self.devices if d.fingers and (self.try_sensor_filter is None or d.sensor_id == self.try_sensor_filter)
        ]
        if len(devs) >= 2:
            self.lbl_done_sub.setText(self._t("try_sub_dual"))
        elif len(devs) == 1:
            self.lbl_done_sub.setText(self._t("try_sub_single", sensor=self._dev_phrase(devs[0])))
        else:
            self.lbl_done_sub.setText(self._t("try_sub_single", sensor=self._t("sensor_generic_phrase")))

    def _open_done_try_mode(self, just_enrolled: bool) -> None:
        self._stop_sessions()
        self.try_sensor_filter = None
        self.refresh_state()
        self._go_to(self.PAGE_DONE)
        self._rebuild_try_sensor_chips(armed_ids=set())

        if just_enrolled:
            self.fp_verify.set_verify_state("ready")
            self.lbl_done_title.setText(self._t("all_set_title"))
            self.lbl_done_sub.setText(self._t("all_set_sub"))
            self._last_verify_settle_timer = QTimer(self)
            self._last_verify_settle_timer.setSingleShot(True)
            self._last_verify_settle_timer.timeout.connect(self._arm_live_try)
            self._last_verify_settle_timer.start(1300)
        else:
            self.lbl_done_title.setText(self._t("try_title"))
            self._update_try_subtitle()
            self._arm_live_try()

    def _arm_live_try(self) -> None:
        if self.stack.currentIndex() != self.PAGE_DONE:
            return
        if self.verify_session:
            self.verify_session.stop(wait=True)
            self.verify_session = None
        # Refresh devices so re-enumerated Power Button (/Device/N) is always present!
        self.devices = discover_devices()
        enrolled_devs = [d for d in self.devices if d.fingers]
        if not enrolled_devs:
            return
        self._rebuild_try_sensor_chips(armed_ids=set())
        self.fp_verify.set_verify_state("listening")
        vs = SimultaneousVerifySession(enrolled_devs, sensor_filter=self.try_sensor_filter)
        vs.armed.connect(self._on_verify_armed)
        vs.finger_present.connect(self._on_verify_finger_present)
        vs.result.connect(self._on_verify_result)
        vs.error.connect(self._on_verify_error)
        self.verify_session = vs
        vs.start()

    def _on_verify_armed(self, armed_devs: List[SensorDevice]) -> None:
        if self.stack.currentIndex() != self.PAGE_DONE:
            return
        armed_ids = {d.sensor_id for d in armed_devs}
        self._rebuild_try_sensor_chips(armed_ids=armed_ids)
        if self.fp_verify._verify_state == "listening":
            self._update_try_subtitle(armed_devs)

    def _on_verify_finger_present(self, sensor_id: str, present: bool) -> None:
        self.fp_verify.set_finger_present(present)
        if self.stack.currentIndex() != self.PAGE_DONE:
            return
        if present:
            self._set_try_chip_state(sensor_id, "scanning", prefix="◉  ")
            dev = next((d for d in self.devices if d.sensor_id == sensor_id), None)
            if dev:
                self.lbl_done_sub.setText(self._t("try_scanning_on", name=self._dev_name(dev)))
        else:
            self._set_try_chip_state(sensor_id, "armed", prefix="●  ")

    def _on_verify_result(self, res: dict) -> None:
        matched = bool(res.get("matched"))
        dev: SensorDevice = res["device"]
        if matched:
            self._success_chime.play()
            self.fp_verify.set_verify_state("match")
            self._set_try_chip_state(dev.sensor_id, "match", prefix="✓  ")
            self.lbl_done_title.setText(self._t("unlocked_title"))
            self.lbl_done_sub.setText(self._t("unlocked_sub", name=self._dev_name(dev)))
        else:
            self.fp_verify.set_verify_state("nomatch")
            self._set_try_chip_state(dev.sensor_id, "nomatch", prefix="✕  ")
            self.lbl_done_title.setText(self._t("nomatch_title"))
            self.lbl_done_sub.setText(self._t("nomatch_sub", name=self._dev_name(dev)))

        self._last_verify_settle_timer = QTimer(self)
        self._last_verify_settle_timer.setSingleShot(True)
        self._last_verify_settle_timer.timeout.connect(self._arm_live_try)
        self._last_verify_settle_timer.start(1600)

    def _on_verify_error(self, _msg: str) -> None:
        self.lbl_done_sub.setText(self._t("verify_err_sub"))

    # =========================================================================
    # Demo Preview (--demo CLI flag for smooth animation showcase)
    # =========================================================================

    def _start_demo_enrollment(self) -> None:
        self._stop_sessions()
        self._go_to(self.PAGE_ENROLL)
        self.enroll_sensor_icon.set_shape("square")
        self.btn_finger_chooser.setText(f"{self._t('sensor_ft')} ▾")
        self.lbl_top_center.setText("")
        self.fp_enroll.reset_enroll()
        self.btn_enroll_continue.setVisible(False)
        self.btn_enroll_skip.setVisible(False)
        self._demo_step = 0

        demo_touches = [
            (0.0, 0.0, 0.0, 0.15, -0.55),
            (10.0, -24.0, 2.5, -0.45, -0.25),
            (-26.0, -14.0, -4.0, 0.55, 0.30),
            (28.0, 16.0, 5.0, -0.35, 0.55),
            (-18.0, 28.0, -6.5, 0.35, -0.45),
            (22.0, -28.0, 14.5, -0.45, 0.40),
            (-28.0, 18.0, -15.5, 0.25, 0.45),
            (4.0, 32.0, 8.0, 0.0, 0.0),
        ]
        grid = [0] * (GRID_COLS * GRID_ROWS)
        poses: List[dict] = []

        def step():
            if self._demo_step >= len(demo_touches):
                if self._demo_timer:
                    self._demo_timer.stop()
                self.btn_enroll_continue.setText(self._t("continue"))
                self.btn_enroll_continue.setVisible(True)
                return
            x, y, deg, hdx, hdy = demo_touches[self._demo_step]
            self._demo_step += 1
            poses.append({"x": x, "y": y, "deg": deg, "comp": 0})
            last_mask = [0] * (GRID_COLS * GRID_ROWS)
            rad = math.radians(deg)
            ca, sa = math.cos(rad), math.sin(rad)
            for r in range(GRID_ROWS):
                wy = ((r + 0.5) - GRID_ROWS * 0.5) * 10.0 - y
                for c in range(GRID_COLS):
                    wx = ((c + 0.5) - GRID_COLS * 0.5) * 10.0 - x
                    lx = ca * wx + sa * wy
                    ly = -sa * wx + ca * wy
                    if abs(lx) <= 44.0 and abs(ly) <= 44.0:
                        idx = r * GRID_COLS + c
                        grid[idx] = min(3, grid[idx] + 1)
                        last_mask[idx] = 1
            done = self._demo_step >= len(demo_touches)
            pct = 100 if done else int(100 * self._demo_step / len(demo_touches))
            self._on_enroll_progress(
                {
                    "dbus_result": "enroll-completed" if done else "enroll-stage-passed",
                    "done": done,
                    "stage": self._demo_step,
                    "total_stages": len(demo_touches),
                    "telemetry": {
                        "status": "complete" if done else "accepted",
                        "progress_pct": pct,
                        "complete": done,
                        "strips": self._demo_step,
                        "hint_dx": hdx,
                        "hint_dy": hdy,
                        "grid": list(grid),
                        "last_mask": last_mask,
                        "poses": list(poses),
                    },
                }
            )

        self._demo_timer = QTimer(self)
        self._demo_timer.setInterval(850)
        self._demo_timer.timeout.connect(step)
        self._demo_timer.start()
        QTimer.singleShot(200, step)

    # =========================================================================
    # Quiet Polkit System Integration Helpers (Settings Only)
    # =========================================================================

    def _run_pkexec_script(self, script_path: Path, title: str) -> None:
        if not script_path.exists():
            QMessageBox.warning(self, title, self._t("helper_missing"))
            return
        self._stop_sessions()
        try:
            proc = subprocess.run(
                ["pkexec", "bash", str(script_path)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if proc.returncode == 0:
                QMessageBox.information(self, title, self._t("helper_ok"))
            elif proc.returncode == 126:
                return
            else:
                err = (proc.stderr or proc.stdout or self._t("unknown_err")).strip()[-400:]
                QMessageBox.warning(self, title, self._t("helper_err", err=err))
        except Exception as exc:
            QMessageBox.warning(self, title, self._t("helper_exc", exc=exc))
        finally:
            self.refresh_state()

    def _run_driver_update(self) -> None:
        self._run_pkexec_script(
            REPO_ROOT / "release-v4.1" / "install-v4.1.sh",
            self._t("drv_update_title"),
        )

    def _run_dual_pam_install(self) -> None:
        self._run_pkexec_script(
            REPO_ROOT / "dual-reader" / "install-dual-pam.sh",
            self._t("unlock_enable_title"),
        )

    def _run_dual_pam_rollback(self) -> None:
        reply = QMessageBox.question(
            self,
            self._t("restore_title"),
            self._t("restore_confirm"),
        )
        if reply == QMessageBox.StandardButton.Yes:
            self._run_pkexec_script(
                REPO_ROOT / "dual-reader" / "rollback-dual-pam.sh",
                self._t("restore_title"),
            )

    def closeEvent(self, event) -> None:
        self._stop_sessions()
        super().closeEvent(event)


def main() -> int:
    parser = argparse.ArgumentParser(description="PressMatch — Fingerprint Setup")
    parser.add_argument("--demo", action="store_true", help="Preview the enrollment animation")
    parser.add_argument("--lang", choices=["auto", "de", "en"], default="auto", help="UI language (default: auto from Linux locale)")
    parser.add_argument("--theme", choices=["light", "dark"], default="light", help="UI theme (default: warm daylight light)")
    args = parser.parse_args()

    app = QApplication(sys.argv)
    lang = detect_system_lang(None if args.lang == "auto" else args.lang)
    app.setApplicationName(tr(lang, "app_title"))
    app.setDesktopFileName("pressmatch-setup")
    app.setStyleSheet(DARK_STYLESHEET if args.theme == "dark" else LIGHT_STYLESHEET)

    win = PressMatchWindow(demo_mode=args.demo, lang=lang, theme=args.theme)
    win.show()
    if args.demo:
        QTimer.singleShot(180, win._start_demo_enrollment)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
