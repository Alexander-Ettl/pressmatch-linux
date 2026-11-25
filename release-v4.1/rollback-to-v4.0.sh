#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Please run with sudo: sudo bash \"$0\""
  exit 1
fi

DROPIN="/etc/systemd/system/fprintd.service.d/elanpress-v4.conf"
cat >"$DROPIN" <<EOF
[Unit]
Description=Fingerprint Authentication Daemon (ELAN Press v4.0 driver)

[Service]
Environment=LD_LIBRARY_PATH=/usr/local/lib/elanpress-v4
EOF

rm -f /etc/udev/rules.d/60-ft9201.rules
udevadm control --reload-rules
systemctl daemon-reload
systemctl restart fprintd.service
echo "Rolled back cleanly to ELAN Press v4.0."
