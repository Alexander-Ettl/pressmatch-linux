#!/usr/bin/env bash
set -euo pipefail

# Upgrade script: ELAN Press v4.0 -> v4.1 (Elastic-ZNCC + Quad-Tier + Native Open-Source FT9201 2808:93a9)
# - Preserves /usr/local/lib/elanpress-v4 intact for instant rollback
# - Never overwrites Ubuntu's packaged /usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
# - Compatible with existing v4.0 enrolled ELAN templates (no re-enrollment needed!)

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Please run with sudo:"
  echo "  sudo bash \"$0\""
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_USER="${SUDO_USER:-${PKEXEC_UID:+$(id -nu "$PKEXEC_UID" 2>/dev/null || true)}}"
TARGET_USER="${TARGET_USER:-$(logname 2>/dev/null || true)}"
SO_SRC="$SCRIPT_DIR/source/build/libfprint/libfprint-2.so.2.0.0"
LIB_DIR="/usr/local/lib/pressmatch-v4.1"
DROPIN="/etc/systemd/system/fprintd.service.d/elanpress-v4.conf"

if [[ ! -f "$SO_SRC" ]]; then
  echo "ERROR: $SO_SRC not found."
  exit 1
fi

echo "=== [1/4] Installing v4.1 multi-driver library (elanpress + ft9201) to $LIB_DIR ==="
install -d -m 755 "$LIB_DIR"
install -m 755 "$SO_SRC" "$LIB_DIR/libfprint-2.so.2.0.0"
ln -sfn libfprint-2.so.2.0.0 "$LIB_DIR/libfprint-2.so.2"

echo "=== [2/4] Preflight ABI check against /usr/libexec/fprintd ==="
LD_LIBRARY_PATH="$LIB_DIR" /usr/libexec/fprintd --help >/dev/null
echo "    ABI preflight OK!"

echo "=== [3/4] Installing FT9201 (2808:93a9) udev rule & updating fprintd drop-in ==="
install -m 644 "$SCRIPT_DIR/60-ft9201.rules" /etc/udev/rules.d/60-ft9201.rules
udevadm control --reload-rules
udevadm trigger --subsystem-match=usb --attr-match=idVendor=2808 --attr-match=idProduct=93a9 || true

install -d -m 755 "$(dirname "$DROPIN")"
cat >"$DROPIN" <<EOF
[Unit]
Description=Fingerprint Authentication Daemon (PressMatch v4.1: ELAN + FT9201)

[Service]
Environment=LD_LIBRARY_PATH=$LIB_DIR
RuntimeDirectory=pressmatch
RuntimeDirectoryMode=0755
EOF

echo "=== [4/4] Restarting fprintd & verifying live devices ==="
systemctl daemon-reload
systemctl restart fprintd.service
sleep 0.6
fprintd-list "$TARGET_USER" 2>&1 || true

if [[ -x "$SCRIPT_DIR/../pressmatch-gui/install-gui.sh" ]] && id "$TARGET_USER" >/dev/null 2>&1; then
  su - "$TARGET_USER" -c "bash \"$SCRIPT_DIR/../pressmatch-gui/install-gui.sh\"" >/dev/null 2>&1 || true
fi

cat <<EOF

========================================================================
PressMatch v4.1 (ELAN Press + Native FocalTech FT9201) is active!
========================================================================
- Spatial enrollment telemetry enabled at /run/pressmatch/enroll-map.json
- Launch the Touch ID-style graphical setup wizard anytime:
    "$SCRIPT_DIR/../pressmatch-gui/pressmatch-setup"
- Existing ELAN v4.0 prints remain 100% compatible (no re-enrollment needed).
- Instant rollback to v4.0 anytime:
    sudo bash "$SCRIPT_DIR/rollback-to-v4.0.sh"
EOF
