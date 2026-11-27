#!/usr/bin/env bash
set -euo pipefail

# Installs the PressMatch Touch ID-style floating pill into the current user's
# KDE Plasma 6 lockscreen (~/.local/share/plasma/shells/org.kde.plasma.desktop).
# - Zero root required, zero system files modified.
# - Symlinks all non-lockscreen Plasma shell files to /usr/share so upgrades stay intact.
# - Instant 1-command revert: rm -rf ~/.local/share/plasma/shells/org.kde.plasma.desktop

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYS_SHELL="/usr/share/plasma/shells/org.kde.plasma.desktop"
USR_SHELL="$HOME/.local/share/plasma/shells/org.kde.plasma.desktop"

if [[ ! -d "$SYS_SHELL/contents/lockscreen" ]]; then
  echo "ERROR: $SYS_SHELL/contents/lockscreen not found."
  exit 1
fi

# Stage & validate the patched lockscreen in a temp directory first so any
# upstream Plasma QML anchor change never wipes an existing working override.
mkdir -p "$(dirname "$USR_SHELL")"
STAGE_DIR="$(mktemp -d "$(dirname "$USR_SHELL")/.org.kde.plasma.desktop.stage.XXXXXX")"
trap 'rm -rf "$STAGE_DIR"' EXIT
mkdir -p "$STAGE_DIR/contents/lockscreen"

# Symlink metadata + every non-lockscreen entry in contents/ so Plasma shell stays 100% stock
ln -s "$SYS_SHELL/metadata.json" "$STAGE_DIR/metadata.json"
for entry in "$SYS_SHELL/contents/"*; do
  bn="$(basename "$entry")"
  if [[ "$bn" != "lockscreen" ]]; then
    ln -s "$entry" "$STAGE_DIR/contents/$bn"
  fi
done

# Copy stock lockscreen files and patch LockScreenUi.qml cleanly + add PressMatchPill.qml
cp -a "$SYS_SHELL/contents/lockscreen/." "$STAGE_DIR/contents/lockscreen/"
cp -a "$SCRIPT_DIR/PressMatchPill.qml" "$STAGE_DIR/contents/lockscreen/PressMatchPill.qml"

python3 - "$STAGE_DIR/contents/lockscreen/LockScreenUi.qml" <<'PY'
import sys
from pathlib import Path

p = Path(sys.argv[1])
src = p.read_text(encoding="utf-8")

# 1. Route PAM fingerprint messages cleanly into pressMatchPill (and avoid cluttering root.notification)
old_conn = """        function onFailed(kind) {
            if (kind != 0) { // if this is coming from the noninteractive authenticators
                return;
            }
            const msg = i18ndc("plasma_shell_org.kde.plasma.desktop", "@info:status", "Unlocking failed");
            lockScreenUi.handleMessage(msg);
            graceLockTimer.restart();
            notificationRemoveTimer.restart();
            rejectPasswordAnimation.start();
        }

        function onSucceeded() {
            if (authenticator.hadPrompt) {
                Qt.quit();
            } else {
                mainStack.replace(null, Qt.resolvedUrl("NoPasswordUnlock.qml"),
                    {
                        userListModel: users
                    },
                    StackView.Immediate,
                );
                mainStack.forceActiveFocus();
            }
        }

        function onInfoMessageChanged() {
            lockScreenUi.handleMessage(authenticator.infoMessage);
        }

        function onErrorMessageChanged() {
            lockScreenUi.handleMessage(authenticator.errorMessage);
        }"""

new_conn = """        function onFailed(kind) {
            if (kind != 0) { // if this is coming from the noninteractive authenticators
                // PressMatch: the fingerprint worker gave up (all tries used) and the
                // readers are off until the next lock: say so instead of "ready"
                if (kind & ScreenLocker.Authenticator.Fingerprint) {
                    lockScreenUi.fingerprintDeclined = true;
                    pressMatchPill.setDeclined();
                }
                return;
            }
            const msg = i18ndc("plasma_shell_org.kde.plasma.desktop", "@info:status", "Unlocking failed");
            lockScreenUi.handleMessage(msg);
            graceLockTimer.restart();
            notificationRemoveTimer.restart();
            rejectPasswordAnimation.start();
        }

        function onSucceeded() {
            // the pill is cosmetic: it must never stand between a match and the unlock
            try {
                pressMatchPill.setMatch(pressMatchPill.activeDevice);
            } catch (e) {
                console.warn("PressMatch pill:", e);
            }
            if (authenticator.hadPrompt) {
                Qt.quit();
            } else {
                mainStack.replace(null, Qt.resolvedUrl("NoPasswordUnlock.qml"),
                    {
                        userListModel: users
                    },
                    StackView.Immediate,
                );
                mainStack.forceActiveFocus();
            }
        }

        function onNoninteractiveInfo(kind, auth) {
            if (kind & ScreenLocker.Authenticator.Fingerprint) {
                if (String(auth.infoMessage).toLowerCase().indexOf("place your finger") !== -1) {
                    lockScreenUi.fingerprintDeclined = false; // readers re-armed
                }
                if (pressMatchPill.parsePamMessage(auth.infoMessage)) {
                    return;
                }
            }
        }

        function onNoninteractiveError(kind, auth) {
            if (kind & ScreenLocker.Authenticator.Fingerprint) {
                pressMatchPill.parsePamMessage(auth.errorMessage);
            }
        }

        function onInfoMessageChanged() {
            if (pressMatchPill.parsePamMessage(authenticator.infoMessage)) {
                return;
            }
            lockScreenUi.handleMessage(authenticator.infoMessage);
        }

        function onErrorMessageChanged() {
            if (pressMatchPill.parsePamMessage(authenticator.errorMessage)) {
                return;
            }
            lockScreenUi.handleMessage(authenticator.errorMessage);
        }

        function onAuthenticatorTypesChanged() {
            if ((authenticator.authenticatorTypes & ScreenLocker.Authenticator.Fingerprint) &&
                pressMatchPill.hudState === "hidden") {
                pressMatchPill.setArmed(pressMatchPill.readersLabel);
            }
        }"""

# 0. Flag shared with MainBlock's "(or scan your fingerprint)" hint
old_root = "    id: lockScreenUi\n"
assert old_root in src, "lockScreenUi id not found"
src = src.replace(old_root, old_root + "    // PressMatch: set when the fingerprint worker has used up its tries\n"
                  "    property bool fingerprintDeclined: false\n", 1)

assert old_conn in src, "Expected Connections block not found in LockScreenUi.qml"
src = src.replace(old_conn, new_conn, 1)

# 2. Insert PressMatchPill at top-center (z: 3) right next to LockOsd.qml loader!
old_osd = """        Loader {
            z: 2
            active: root.viewVisible
            source: "LockOsd.qml"
            anchors {
                horizontalCenter: parent.horizontalCenter
                bottom: parent.bottom
                bottomMargin: Kirigami.Units.gridUnit
            }
        }"""

new_osd = """        Loader {
            z: 2
            active: root.viewVisible
            source: "LockOsd.qml"
            anchors {
                horizontalCenter: parent.horizontalCenter
                bottom: parent.bottom
                bottomMargin: Kirigami.Units.gridUnit
            }
        }

        PressMatchPill {
            id: pressMatchPill
            z: 3
            anchors {
                right: parent.right
                bottom: parent.bottom
            }
            Component.onCompleted: {
                if (authenticator.authenticatorTypes & ScreenLocker.Authenticator.Fingerprint) {
                    setArmed("ELAN · FT9201");
                }
            }
        }

        // PressMatch: arm the readers as soon as the screen locks. Plasma starts
        // authenticating only when the UI first appears (click, key or mouse move),
        // so a touch on a hidden lock screen did nothing. startAuthenticating() is a
        // no-op while authentication runs and during the post-lock grace period,
        // hence the retry while the UI is hidden.
        Timer {
            interval: 1000
            repeat: true
            triggeredOnStart: true
            running: !lockScreenRoot.uiVisible
            onTriggered: authenticator.startAuthenticating()
        }"""

assert old_osd in src, "Expected LockOsd Loader block not found in LockScreenUi.qml"
src = src.replace(old_osd, new_osd, 1)

p.write_text(src, encoding="utf-8")
print("Patched", p)

# 3. Hide Plasma's "(or scan your fingerprint on the reader)" hint once declined
mb = p.with_name("MainBlock.qml")
msrc = mb.read_text(encoding="utf-8")
old_vis = "        visible: authenticator.authenticatorTypes & kind\n"
assert old_vis in msrc, "FailableLabel visibility not found in MainBlock.qml"
msrc = msrc.replace(old_vis,
    "        visible: (authenticator.authenticatorTypes & kind)\n"
    "                 && !((kind & ScreenLocker.Authenticator.Fingerprint) && lockScreenUi.fingerprintDeclined)\n", 1)
mb.write_text(msrc, encoding="utf-8")
print("Patched", mb)
PY

# All patch assertions succeeded: swap staged tree cleanly into place
rm -rf "$USR_SHELL"
mv "$STAGE_DIR" "$USR_SHELL"
trap - EXIT

echo "Installed PressMatch Lockscreen HUD to $USR_SHELL/contents/lockscreen"
echo "To revert anytime: rm -rf \"$USR_SHELL\""
