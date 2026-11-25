#!/usr/bin/env python3
"""
PressMatch Setup — Automatic Linux Locale Detection & Natural DE/EN Copy.

Automatically selects German ("de") or English ("en") based on the user's
Linux locale environment (LC_ALL -> LC_MESSAGES -> LANGUAGE -> LANG -> QLocale),
with an optional CLI override (`--lang de|en`) for testing or demos.
"""
from __future__ import annotations

import math
import os
from typing import Dict, Optional

from PyQt6.QtCore import QLocale


def detect_system_lang(override: Optional[str] = None) -> str:
    """Detect 'de' or 'en' from override, Linux locale env vars, or QLocale."""
    if override:
        low = override.strip().lower()
        if low.startswith("de"):
            return "de"
        if low.startswith("en"):
            return "en"

    for env_key in ("LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"):
        val = os.environ.get(env_key, "").strip()
        if not val or val.upper() in ("C", "POSIX"):
            continue
        # LANGUAGE can be colon-separated, e.g. "de_DE:en_US"
        first = val.split(":")[0].strip().lower()
        if first.startswith("de"):
            return "de"
        if first.startswith("en"):
            return "en"

    qname = QLocale.system().name().strip().lower()
    if qname.startswith("de"):
        return "de"
    return "en"


STRINGS: Dict[str, Dict[str, str]] = {
    "en": {
        "app_title": "PressMatch Fingerprint Setup",
        "cancel": "Cancel",
        "back": "‹ Back",
        "done": "Done",
        "continue": "Continue",
        "settings_tip": "Settings & Saved Fingerprints",
        "sensor_elan": "Power Button",
        "sensor_ft": "USB Reader",
        "sensor_generic": "Fingerprint Sensor",
        "sensor_elan_phrase": "the power button",
        "sensor_ft_phrase": "your USB reader",
        "sensor_generic_phrase": "the fingerprint sensor",
        "welcome_no_dev_title": "Connect a fingerprint reader",
        "welcome_no_dev_sub": "Plug in your fingerprint reader, then click Check Again.",
        "welcome_check_again": "Check Again",
        "welcome_title": "Unlock with your fingerprint",
        "welcome_sub_dual": "Use your power button or USB reader to unlock your laptop, sign in, and approve apps in a single touch.",
        "welcome_sub_single": "Use your fingerprint to unlock your laptop, sign in, and approve apps in a single touch.",
        "welcome_setup": "Set Up Fingerprint",
        "welcome_add": "Add a Fingerprint",
        "welcome_try": "Try Unlocking",
        "more_fingers": "+{n} more",
        "finger_n": "Finger {n}",
        "finger_generic": "this fingerprint",
        "sensor_step": "Sensor {idx} of {total}",
        "prep_title": "Preparing Sensor…",
        "prep_sub": "Getting {sensor} ready.",
        "auth_title": "Confirm Your Identity",
        "auth_sub_saved": "Enter your password or touch a saved finger in the system prompt to continue.",
        "auth_sub_pass": "Enter your password in the system prompt to begin — take your time.",
        "same_finger_title": "Use the Same Finger",
        "same_finger_sub": "Rest that same finger gently on {sensor}, then lift.",
        "place_title": "Place Your Finger",
        "place_sub": "Use whichever finger feels most natural on {sensor} — rest gently, then lift.",
        "menu_both": "Both Sensors (Recommended)",
        "menu_only": "{name} Only",
        "hold_still": "Hold Still…",
        "lighter_title": "Touch a Little Lighter",
        "lighter_sub": "A gentle rest works best — no need to press down.",
        "lighter_sub_lift": "Lift your finger and rest it gently on the sensor.",
        "different_spot_title": "Try a Different Spot",
        "flat_title": "Rest Your Finger Flat",
        "flat_sub": "Cover the sensor for a moment, then lift.",
        "adjust_title": "Adjust Your Grip",
        "tilt_sub": "Tilt your finger slightly to the {side} to capture the outer edges.",
        "side_left": "left",
        "side_right": "right",
        "almost_title": "Almost Done",
        "almost_sub_fallback": "One or two more gentle touches around the edges.",
        "lift_rest_title": "Lift and Rest Your Finger",
        "lift_rest_fallback": "Shift slightly each time to map your fingertip.",
        "not_centered_sub": "Shift slightly to scan a fresh part of your fingertip.",
        "retry_title": "Try That Again",
        "retry_sub": "Rest your finger steadily for a moment, then lift.",
        "adjust_fallback": "Capture the edges and try tilting slightly left or right.",
        "sensor_ready_title": "{name} Ready!",
        "sensor_ready_sub": "Next, scan the same finger on {next_phrase} so both unlock seamlessly.",
        "setup_next": "Set Up {name}",
        "skip_next": "Skip {name}",
        "finger_added_title": "Fingerprint Added",
        "finger_added_sub": "Your fingerprint is saved and ready to use.",
        "add_another": "Add Another Finger",
        "dup_second_title": "Already Saved on {name}",
        "dup_second_sub": "This finger already unlocks your {name}. Click Done, or rest a different finger.",
        "dup_title": "Already Saved",
        "dup_sub": "That finger is already set up — try a different finger.",
        "full_title": "Storage Full",
        "full_sub": "Remove a saved finger in Settings to add a new one.",
        "full_dialog_title": "Fingerprint Storage Full",
        "full_dialog_body": "You've already saved the maximum of 10 fingerprints.\nRemove one in Settings (⚙) if you'd like to add a new one.",
        "disc_title": "Sensor Disconnected",
        "disc_sub": "Reconnect your reader — setup will resume automatically.",
        "rearm_title": "Lift and Touch Again",
        "rearm_sub": "Rest your finger flat and gently for a moment, then lift.",
        "rearm_stuck": "Having trouble reading — try cleaning the sensor or click Cancel.",
        "all_set_title": "You're All Set",
        "all_set_sub": "Touch your sensor anytime right now to try it out.",
        "try_title": "Try Your Fingerprint",
        "try_sub_dual": "Touch your power button or USB reader to test unlocking.",
        "try_sub_single": "Rest your finger on {sensor} to test unlocking.",
        "try_mode_both": "Both Readers",
        "try_scanning_on": "Scanning on {name}…",
        "unlocked_title": "Unlocked!",
        "unlocked_sub": "Recognized on {name}. Touch again anytime to test.",
        "nomatch_title": "Not Recognized",
        "nomatch_sub": "Not recognized on {name} — try again with a saved finger.",
        "verify_err_sub": "Your fingerprint is saved and ready to unlock your laptop.",
        "settings_title": "Fingerprint Settings",
        "sec_saved": "SAVED FINGERPRINTS",
        "no_saved": "No fingerprints saved yet.",
        "remove": "Remove",
        "remove_title": "Remove Fingerprint",
        "remove_confirm": "Remove {name}?",
        "add_row": "+  Add a Fingerprint",
        "sec_sys": "SYSTEM UNLOCK",
        "unlock_title": "Unlock Everywhere",
        "unlock_sub": "Use any connected sensor on the lock screen, login, and system prompts.",
        "status_active": "●  Active",
        "enable": "Enable",
        "drv_title": "Enhanced Sensor Driver",
        "drv_sub": "Powers live fingertip guidance and fast, accurate matching.",
        "status_uptodate": "●  Up to Date",
        "status_update_avail": "Update available",
        "update": "Update",
        "restore_btn": "Restore Default System Login Settings",
        "restore_title": "Restore Default Settings",
        "restore_confirm": "Restore default system login settings?\nYour saved fingerprints and password will not be removed.",
        "drv_update_title": "Update Fingerprint Driver",
        "unlock_enable_title": "Enable Fingerprint Unlock Everywhere",
        "helper_missing": "Setup helper could not be found.",
        "helper_ok": "All set! Changes are active now.",
        "helper_err": "Could not complete update:\n\n{err}",
        "helper_exc": "Could not run setup helper:\n{exc}",
        "unknown_err": "Unknown error",
        "dir_edges": "Roll gently around the edges of your fingertip.",
        "dir_both": "Shift slightly {vert} and to the {horiz}.",
        "dir_vert": "Move slightly {vert}, then rest again.",
        "dir_horiz": "Move slightly to the {horiz}, then rest again.",
        "dir_fallback": "Shift your finger slightly to scan a new spot.",
        "dir_tip": "toward the tip",
        "dir_base": "toward the lower pad",
        "dir_left": "left",
        "dir_right": "right",
    },
    "de": {
        "app_title": "PressMatch Fingerabdruck-Einrichtung",
        "cancel": "Abbrechen",
        "back": "‹ Zurück",
        "done": "Fertig",
        "continue": "Weiter",
        "settings_tip": "Einstellungen & gespeicherte Fingerabdrücke",
        "sensor_elan": "Power-Taste",
        "sensor_ft": "USB-Sensor",
        "sensor_generic": "Fingerabdruck-Sensor",
        "sensor_elan_phrase": "die Power-Taste",
        "sensor_ft_phrase": "den USB-Sensor",
        "sensor_generic_phrase": "den Fingerabdruck-Sensor",
        "welcome_no_dev_title": "Sensor anschließen",
        "welcome_no_dev_sub": "Schließe deinen Fingerabdruck-Sensor an und klicke auf Erneut prüfen.",
        "welcome_check_again": "Erneut prüfen",
        "welcome_title": "Mit dem Finger entsperren",
        "welcome_sub_dual": "Nutze die Power-Taste oder den USB-Sensor zum Entsperren, Anmelden und Bestätigen mit einer Berührung.",
        "welcome_sub_single": "Nutze deinen Fingerabdruck zum Entsperren, Anmelden und Bestätigen mit einer Berührung.",
        "welcome_setup": "Fingerabdruck einrichten",
        "welcome_add": "Fingerabdruck hinzufügen",
        "welcome_try": "Entsperren testen",
        "more_fingers": "+{n} weitere",
        "finger_n": "Finger {n}",
        "finger_generic": "diesen Fingerabdruck",
        "sensor_step": "Sensor {idx} von {total}",
        "prep_title": "Sensor wird vorbereitet…",
        "prep_sub": "Einen Moment bitte — gleich geht es los.",
        "auth_title": "Kurz bestätigen",
        "auth_sub_saved": "Gib dein Passwort ein oder lege einen gespeicherten Finger auf, um fortzufahren.",
        "auth_sub_pass": "Gib im Systemfenster dein Passwort ein — lass dir dabei ruhig Zeit.",
        "same_finger_title": "Denselben Finger nutzen",
        "same_finger_sub": "Lege genau denselben Finger sanft auf {sensor} und hebe ihn kurz an.",
        "place_title": "Finger auflegen",
        "place_sub": "Nimm einfach deinen bequemsten Finger, lege ihn sanft auf {sensor} und hebe ihn kurz an.",
        "menu_both": "Beide Sensoren (Empfohlen)",
        "menu_only": "Nur {name}",
        "hold_still": "Kurz ruhig halten…",
        "lighter_title": "Etwas sanfter auflegen",
        "lighter_sub": "Sanftes Auflegen reicht völlig aus — du musst nicht drücken.",
        "lighter_sub_lift": "Hebe den Finger kurz an und lege ihn sanft wieder auf.",
        "different_spot_title": "Leicht versetzen",
        "flat_title": "Finger flach auflegen",
        "flat_sub": "Bedecke den Sensor kurz ruhig und hebe den Finger dann an.",
        "adjust_title": "Position leicht anpassen",
        "tilt_sub": "Kippe den Finger leicht nach {side}, um auch die Ränder zu erfassen.",
        "side_left": "links",
        "side_right": "rechts",
        "almost_title": "Fast geschafft",
        "almost_sub_fallback": "Noch ein oder zwei sanfte Berührungen an den Rändern.",
        "lift_rest_title": "Anheben und erneut auflegen",
        "lift_rest_fallback": "Versetze den Finger jedes Mal ein kleines Stück.",
        "not_centered_sub": "Versetze den Finger leicht, um einen neuen Bereich zu scannen.",
        "retry_title": "Noch einmal kurz",
        "retry_sub": "Lege den Finger einen kurzen Moment ruhig auf und hebe ihn dann an.",
        "adjust_fallback": "Erfasse die Ränder und kippe den Finger leicht nach links oder rechts.",
        "sensor_ready_title": "{name} fertig!",
        "sensor_ready_sub": "Scanne jetzt denselben Finger auf {next_phrase}, damit beide nahtlos entsperren.",
        "setup_next": "{name} einrichten",
        "skip_next": "{name} überspringen",
        "finger_added_title": "Fingerabdruck gespeichert",
        "finger_added_sub": "Dein Fingerabdruck ist gespeichert und sofort einsatzbereit.",
        "add_another": "Weiteren Finger hinzufügen",
        "dup_second_title": "Schon auf {name} gespeichert",
        "dup_second_sub": "Dieser Finger entsperrt {name} bereits. Klicke auf Fertig oder nutze einen anderen Finger.",
        "dup_title": "Bereits gespeichert",
        "dup_sub": "Dieser Finger ist schon eingerichtet — nimm einfach einen anderen Finger.",
        "full_title": "Speicher voll",
        "full_sub": "Entferne zuerst einen Finger in den Einstellungen, um einen neuen hinzuzufügen.",
        "full_dialog_title": "Speicher voll",
        "full_dialog_body": "Es sind bereits die maximal 10 Fingerabdrücke gespeichert.\nEntferne einen unter Einstellungen (⚙), um einen neuen hinzuzufügen.",
        "disc_title": "Sensor getrennt",
        "disc_sub": "Schließe den Sensor wieder an — die Einrichtung geht automatisch weiter.",
        "rearm_title": "Anheben und erneut auflegen",
        "rearm_sub": "Lege den Finger einen kurzen Moment flach und sanft auf.",
        "rearm_stuck": "Lesefehler — reinige kurz den Sensor oder klicke auf Abbrechen.",
        "all_set_title": "Alles bereit",
        "all_set_sub": "Berühre jetzt jederzeit den Sensor, um es direkt auszuprobieren.",
        "try_title": "Fingerabdruck testen",
        "try_sub_dual": "Berühre die Power-Taste oder den USB-Sensor zum Testen.",
        "try_sub_single": "Lege deinen Finger kurz auf {sensor} zum Testen.",
        "try_mode_both": "Beide Sensoren",
        "try_scanning_on": "Scanne auf {name}…",
        "unlocked_title": "Entsperrt!",
        "unlocked_sub": "Erkannt über {name}. Zum Testen jederzeit erneut auflegen.",
        "nomatch_title": "Nicht erkannt",
        "nomatch_sub": "Auf {name} nicht erkannt — versuche es noch einmal mit einem gespeicherten Finger.",
        "verify_err_sub": "Dein Fingerabdruck ist gespeichert und einsatzbereit.",
        "settings_title": "Fingerabdruck-Einstellungen",
        "sec_saved": "GESPEICHERTE FINGERABDRÜCKE",
        "no_saved": "Noch keine Fingerabdrücke gespeichert.",
        "remove": "Entfernen",
        "remove_title": "Fingerabdruck entfernen",
        "remove_confirm": "{name} wirklich entfernen?",
        "add_row": "+  Fingerabdruck hinzufügen",
        "sec_sys": "SYSTEM-ENTSPERRUNG",
        "unlock_title": "Überall entsperren",
        "unlock_sub": "Jeden verbundenen Sensor für Sperrbildschirm, Anmeldung und Passwort-Fenster nutzen.",
        "status_active": "●  Aktiv",
        "enable": "Aktivieren",
        "drv_title": "Verbesserter Sensor-Treiber",
        "drv_sub": "Sorgt für Live-Führung und schnelle, präzise Erkennung.",
        "status_uptodate": "●  Aktuell",
        "status_update_avail": "Update verfügbar",
        "update": "Aktualisieren",
        "restore_btn": "Standard-Anmeldung wiederherstellen",
        "restore_title": "Standard wiederherstellen",
        "restore_confirm": "Standard-Anmeldeeinstellungen wiederherstellen?\nDeine gespeicherten Fingerabdrücke und dein Passwort bleiben erhalten.",
        "drv_update_title": "Sensor-Treiber aktualisieren",
        "unlock_enable_title": "Fingerabdruck-Entsperrung aktivieren",
        "helper_missing": "Das Installations-Hilfsskript wurde nicht gefunden.",
        "helper_ok": "Fertig! Die Änderungen sind ab sofort aktiv.",
        "helper_err": "Aktualisierung fehlgeschlagen:\n\n{err}",
        "helper_exc": "Hilfsskript konnte nicht gestartet werden:\n{exc}",
        "unknown_err": "Unbekannter Fehler",
        "dir_edges": "Rolle die Fingerkuppe sanft leicht über die Ränder.",
        "dir_both": "Versetze den Finger leicht {vert} und nach {horiz}.",
        "dir_vert": "Bewege den Finger leicht {vert} und lege ihn wieder auf.",
        "dir_horiz": "Bewege den Finger leicht nach {horiz} und lege ihn wieder auf.",
        "dir_fallback": "Versetze den Finger leicht für einen neuen Bereich.",
        "dir_tip": "zur Fingerspitze",
        "dir_base": "nach unten",
        "dir_left": "links",
        "dir_right": "rechts",
    },
}


def tr(lang: str, key: str, **kwargs) -> str:
    table = STRINGS.get(lang, STRINGS["en"])
    text = table.get(key, STRINGS["en"].get(key, key))
    if kwargs:
        return text.format(**kwargs)
    return text


def friendly_direction(lang: str, dx: float, dy: float) -> str:
    """Turn spatial map nudge vector into warm, natural human guidance (DE or EN)."""
    if math.hypot(dx, dy) < 0.16:
        return tr(lang, "dir_edges")
    horiz = tr(lang, "dir_right") if dx > 0.22 else (tr(lang, "dir_left") if dx < -0.22 else "")
    vert = tr(lang, "dir_tip") if dy < -0.22 else (tr(lang, "dir_base") if dy > 0.22 else "")
    if vert and horiz:
        return tr(lang, "dir_both", vert=vert, horiz=horiz)
    if vert:
        return tr(lang, "dir_vert", vert=vert)
    if horiz:
        return tr(lang, "dir_horiz", horiz=horiz)
    return tr(lang, "dir_fallback")


def sensor_name(lang: str, sensor_id: str) -> str:
    if sensor_id == "elan":
        return tr(lang, "sensor_elan")
    if sensor_id == "ft9201":
        return tr(lang, "sensor_ft")
    return tr(lang, "sensor_generic")


def sensor_phrase(lang: str, sensor_id: str) -> str:
    if sensor_id == "elan":
        return tr(lang, "sensor_elan_phrase")
    if sensor_id == "ft9201":
        return tr(lang, "sensor_ft_phrase")
    return tr(lang, "sensor_generic_phrase")
