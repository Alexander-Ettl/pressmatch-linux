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

rm -rf "$USR_SHELL"
mkdir -p "$USR_SHELL/contents/lockscreen"

# Symlink metadata + every non-lockscreen entry in contents/ so Plasma shell stays 100% stock
ln -s "$SYS_SHELL/metadata.json" "$USR_SHELL/metadata.json"
for entry in "$SYS_SHELL/contents/"*; do
  bn="$(basename "$entry")"
  if [[ "$bn" != "lockscreen" ]]; then
    ln -s "$entry" "$USR_SHELL/contents/$bn"
  fi
done

# Copy stock lockscreen files and patch LockScreenUi.qml cleanly + add PressMatchPill.qml
cp -a "$SYS_SHELL/contents/lockscreen/." "$USR_SHELL/contents/lockscreen/"
cp -a "$SCRIPT_DIR/PressMatchPill.qml" "$USR_SHELL/contents/lockscreen/PressMatchPill.qml"

python3 - "$USR_SHELL/contents/lockscreen/LockScreenUi.qml" <<'PY'
import sys
from pathlib import Path

p = Path(sys.argv[1])
src = p.read_text(encoding="utf-8")

# 1. Route PAM fingerprint messages cleanly into pressMatchPill (and avoid cluttering root.notification)
old_conn = """        function onSucceeded() {
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

new_conn = """        function onSucceeded() {
            pressMatchPill.setMatch(pressMatchPill.activeDevice);
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
                horizontalCenter: parent.horizontalCenter
                top: parent.top
                topMargin: 14
            }
            Component.onCompleted: {
                if (authenticator.authenticatorTypes & ScreenLocker.Authenticator.Fingerprint) {
                    setArmed("ELAN · FT9201");
                }
            }
        }"""

assert old_osd in src, "Expected LockOsd Loader block not found in LockScreenUi.qml"
src = src.replace(old_osd, new_osd, 1)

p.write_text(src, encoding="utf-8")
print("Patched", p)
PY

echo "Installed PressMatch Lockscreen HUD to $USR_SHELL/contents/lockscreen"
echo "To revert anytime: rm -rf \"$USR_SHELL\""
