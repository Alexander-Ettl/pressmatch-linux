#!/usr/bin/env bash
set -euo pipefail

# Installs the native PressMatch Setup desktop launcher & icon for the current user
# (zero sudo required!) so it appears in KDE Application Launcher / GNOME Overview.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"

chmod +x "$SCRIPT_DIR/pressmatch-setup" "$SCRIPT_DIR/app.py" "$SCRIPT_DIR/pressmatch_hud.py"
mkdir -p "$APP_DIR" "$ICON_DIR"

cp "$SCRIPT_DIR/pressmatch-setup.svg" "$ICON_DIR/pressmatch-setup.svg"

cat >"$APP_DIR/pressmatch-setup.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=PressMatch Fingerprint Setup
Name[de]=PressMatch Fingerabdruck-Einrichtung
GenericName=Fingerprint Setup Wizard
GenericName[de]=Fingerabdruck-Assistent
Comment=Touch ID-style spatial fingerprint enrollment & dual-reader setup
Comment[de]=Touch-ID-ähnliche Fingerabdruck-Einrichtung für beide Sensoren
Exec="$SCRIPT_DIR/pressmatch-setup"
Icon=pressmatch-setup
Terminal=false
Categories=Settings;System;Security;
Keywords=fingerprint;biometric;touchid;fprintd;pressmatch;elan;ft9201;
StartupWMClass=pressmatch-setup
EOF

update-desktop-database "$APP_DIR" 2>/dev/null || true

# Install & enable floating Touch ID desktop HUD service
USER_UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
mkdir -p "$USER_UNIT_DIR"
cat >"$USER_UNIT_DIR/pressmatch-hud.service" <<EOF
[Unit]
Description=PressMatch Touch ID-Style Floating Fingerprint HUD
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
Environment=QT_WAYLAND_SHELL_INTEGRATION=layer-shell
ExecStart=/usr/bin/python3 "$SCRIPT_DIR/pressmatch_hud.py"
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF
systemctl --user daemon-reload 2>/dev/null || true
systemctl --user enable --now pressmatch-hud.service 2>/dev/null || true

# Install Plasma 6 lockscreen Touch ID pill override (user-local, zero root)
if [[ -x "$SCRIPT_DIR/lockscreen/install-lockscreen-hud.sh" ]] && [[ -d /usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen ]]; then
  bash "$SCRIPT_DIR/lockscreen/install-lockscreen-hud.sh" >/dev/null 2>&1 || true
fi

echo "Installed PressMatch Setup desktop entry & Touch ID HUD:"
echo "  $APP_DIR/pressmatch-setup.desktop"
echo "  $ICON_DIR/pressmatch-setup.svg"
echo "  $USER_UNIT_DIR/pressmatch-hud.service"
echo "Launch anytime from your app menu or run:"
echo "  \"$SCRIPT_DIR/pressmatch-setup\""
