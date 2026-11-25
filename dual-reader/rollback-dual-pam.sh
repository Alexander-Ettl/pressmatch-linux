#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Please run with sudo: sudo bash \"$0\""
  exit 1
fi

BACKUP_DIR="/var/backups/pressmatch-dual-pam"
for f in common-auth sddm gdm-fingerprint polkit-1 sddm-autologin; do
  if [[ -f "$BACKUP_DIR/$f.pre-dual" ]]; then
    cp -a "$BACKUP_DIR/$f.pre-dual" "/etc/pam.d/$f"
  elif [[ "$f" != "sddm-autologin" && -f "/etc/pam.d/$f" ]]; then
    sed -i 's/pam_pressmatch_dual\.so/pam_fprintd.so/g' "/etc/pam.d/$f"
  fi
done
rm -f /etc/sddm.conf.d/30-pressmatch-poweron.conf \
      /usr/lib/systemd/system-sleep/pressmatch-elan-sso \
      /usr/local/libexec/pressmatch-elan-sso
if [[ -f "$BACKUP_DIR/kde-fingerprint.pre-dual" ]]; then
  cp -a "$BACKUP_DIR/kde-fingerprint.pre-dual" /etc/pam.d/kde-fingerprint
else
  rm -f /etc/pam.d/kde-fingerprint
fi
rm -f /usr/lib/x86_64-linux-gnu/security/pam_pressmatch_dual.so
echo "Rolled back cleanly to pam_fprintd.so."
