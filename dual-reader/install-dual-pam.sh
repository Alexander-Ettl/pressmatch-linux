#!/usr/bin/env bash
set -euo pipefail

# Installs pam_pressmatch_dual.so (lid-aware simultaneous ELAN + FT9201 unlock)
# Keeps password fallback intact everywhere; creates backups for 1-command rollback.

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Please run with sudo: sudo bash \"$0\""
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SO_SRC="$SCRIPT_DIR/pam_pressmatch_dual.so"
PAM_DST="/usr/lib/x86_64-linux-gnu/security/pam_pressmatch_dual.so"
BACKUP_DIR="/var/backups/pressmatch-dual-pam"

if [[ ! -f "$SO_SRC" ]]; then
  echo "ERROR: $SO_SRC not found."
  exit 1
fi

echo "=== [1/4] Verifying pam_pressmatch_dual.so symbols via dlopen(RTLD_NOW) ==="
python3 - "$SO_SRC" <<'EOF'
import ctypes, os, sys
lib = ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW)
for sym in ("pam_sm_authenticate", "pam_sm_setcred", "pam_sm_chauthtok"):
    assert hasattr(lib, sym), f"Missing symbol {sym}"
print("    dlopen(RTLD_NOW) + symbol check OK!")
EOF

echo "=== [2/4] Backing up current PAM files to $BACKUP_DIR ==="
mkdir -p "$BACKUP_DIR"
chmod 700 "$BACKUP_DIR"
for f in /etc/pam.d/common-auth /etc/pam.d/sddm /etc/pam.d/gdm-fingerprint /etc/pam.d/polkit-1 /etc/pam.d/sddm-autologin; do
  bn="$(basename "$f")"
  if [[ -f "$f" && ! -f "$BACKUP_DIR/$bn.pre-dual" ]]; then
    cp -a "$f" "$BACKUP_DIR/$bn.pre-dual"
  fi
done
if [[ -f /etc/pam.d/kde-fingerprint && ! -f "$BACKUP_DIR/kde-fingerprint.pre-dual" ]]; then
  cp -a /etc/pam.d/kde-fingerprint "$BACKUP_DIR/kde-fingerprint.pre-dual"
fi

echo "=== [3/5] Installing $PAM_DST & ELAN SSO helper ==="
install -m 644 "$SO_SRC" "$PAM_DST"
if [[ -x "$SCRIPT_DIR/pressmatch-elan-sso" ]]; then
  install -d -m 755 /usr/local/libexec
  install -m 755 "$SCRIPT_DIR/pressmatch-elan-sso" /usr/local/libexec/pressmatch-elan-sso
  install -d -m 755 /usr/lib/systemd/system-sleep
  cat > /usr/lib/systemd/system-sleep/pressmatch-elan-sso <<'SLEEP_EOF'
#!/usr/bin/env bash
# Arm ELAN EMSE1000 Software SSO before sleep (s2idle/suspend) so pressing the
# power button to wake caches the fingerprint; disarm SW SSO on hibernate so
# hardware power-on SSO takes over cleanly on cold boot.
case "$1/$2" in
  pre/suspend|pre/suspend-then-hibernate)
    /usr/local/libexec/pressmatch-elan-sso --swsso-on >/dev/null 2>&1 || true
    ;;
  pre/hibernate)
    /usr/local/libexec/pressmatch-elan-sso --swsso-off >/dev/null 2>&1 || true
    ;;
esac
SLEEP_EOF
  chmod 755 /usr/lib/systemd/system-sleep/pressmatch-elan-sso
fi

echo "=== [4/5] Wiring simultaneous dual-reader PAM into common-auth (sudo), SDDM/GDM, KDE lockscreen, and Polkit ==="
if [[ -f /etc/pam.d/common-auth ]]; then
  if ! grep -qE 'pam_(fprintd|pressmatch_dual)\.so' /etc/pam.d/common-auth; then
    if [[ -f /usr/share/pam-configs/fprintd ]] && command -v pam-auth-update >/dev/null 2>&1; then
      DEBIAN_FRONTEND=noninteractive pam-auth-update --enable fprintd || true
    fi
  fi
  if grep -q 'pam_fprintd\.so' /etc/pam.d/common-auth; then
    sed -i 's/pam_fprintd\.so/pam_pressmatch_dual.so/g' /etc/pam.d/common-auth
  elif ! grep -q 'pam_pressmatch_dual\.so' /etc/pam.d/common-auth; then
    # Safe jump-independent fallback when fprintd PAM profile isn't enabled yet:
    # insert 'sufficient' directly above pam_unix.so so sudo & login get fingerprint + intact password fallback.
    python3 - /etc/pam.d/common-auth <<'PY'
import re, sys
from pathlib import Path
p = Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
m = re.search(r'(?m)^auth[ \t]+.*pam_unix\.so.*$', src)
assert m, "pam_unix.so line not found in /etc/pam.d/common-auth"
line = "auth\tsufficient\tpam_pressmatch_dual.so max-tries=1 timeout=10\n"
p.write_text(src[:m.start()] + line + src[m.start():], encoding="utf-8")
PY
  fi
fi

if [[ -f /etc/pam.d/sddm ]]; then
  sed -i 's/pam_fprintd\.so/pam_pressmatch_dual.so/g' /etc/pam.d/sddm
fi
if [[ -f /etc/pam.d/gdm-fingerprint ]]; then
  sed -i 's/pam_fprintd\.so/pam_pressmatch_dual.so/g' /etc/pam.d/gdm-fingerprint
fi

if [[ -f /usr/lib/pam.d/kde-fingerprint ]]; then
  sed 's/pam_fprintd\.so.*/pam_pressmatch_dual.so max-tries=5 timeout=-1/' /usr/lib/pam.d/kde-fingerprint > /etc/pam.d/kde-fingerprint
  chmod 644 /etc/pam.d/kde-fingerprint
fi

# Polkit authentication popup (polkit-1): wait indefinitely for fingerprint (no 10s timer)
# while preserving instant password fallback when no reader is available or after 2 wrong touches.
cat > /etc/pam.d/polkit-1 <<'PAM_EOF'
#%PAM-1.0
# Polkit authentication popup — PressMatch dual-reader (no fingerprint timer)
# Password fallback remains active whenever no reader is usable or after 2 unmatched touches.

auth	[success=2 default=ignore]	pam_pressmatch_dual.so max-tries=2 timeout=-1
auth	[success=1 default=ignore]	pam_unix.so nullok try_first_pass
auth	requisite			pam_deny.so
auth	required			pam_permit.so
auth	optional			pam_cap.so

@include common-account
@include common-password
session       required   pam_env.so readenv=1 user_readenv=0
@include common-session-noninteractive
PAM_EOF
chmod 644 /etc/pam.d/polkit-1

echo "=== [5/5] Wiring One-Touch Power-On Login into SDDM (sddm-autologin) ==="
TARGET_USER="${SUDO_USER:-${PKEXEC_UID:+$(id -nu "$PKEXEC_UID" 2>/dev/null || true)}}"
TARGET_USER="${TARGET_USER:-$(logname 2>/dev/null || true)}"
if [[ -z "$TARGET_USER" || "$TARGET_USER" == "root" ]]; then
  echo "WARNING: Could not detect non-root desktop user; skipping SDDM [Autologin] User."
fi
if [[ -n "$TARGET_USER" && "$TARGET_USER" != "root" ]]; then
  if [[ -f /etc/pam.d/sddm-autologin ]]; then
    sed -E 's|^auth[[:space:]]+required[[:space:]]+pam_(permit\|pressmatch_dual)\.so.*|auth    required        pam_pressmatch_dual.so builtin-only quiet max-tries=1 timeout=2|' \
      "$BACKUP_DIR/sddm-autologin.pre-dual" > /etc/pam.d/sddm-autologin
    chmod 644 /etc/pam.d/sddm-autologin
  fi
  install -d -m 755 /etc/sddm.conf.d
  cat > /etc/sddm.conf.d/30-pressmatch-poweron.conf <<SDDM_EOF
# PressMatch One-Touch Power-On Login:
# Runs /etc/pam.d/sddm-autologin (gated strictly by pam_pressmatch_dual.so!) on cold boot.
# - Enrolled finger pressed power button -> logs straight into Plasma desktop!
# - Wrong finger / stranger / no cached print -> immediately falls back to normal SDDM login screen!
[Autologin]
User=${TARGET_USER}
Session=plasma
Relogin=false
SDDM_EOF
  chmod 644 /etc/sddm.conf.d/30-pressmatch-poweron.conf
fi

PAM_CHECK=()
for f in /etc/pam.d/common-auth /etc/pam.d/sddm /etc/pam.d/gdm-fingerprint /etc/pam.d/kde-fingerprint /etc/pam.d/polkit-1 /etc/pam.d/sddm-autologin; do
  [[ -f "$f" ]] && PAM_CHECK+=("$f")
done
grep -n "pam_pressmatch_dual" "${PAM_CHECK[@]}" || true

cat <<EOF

========================================================================
Simultaneous Dual-Reader Unlock (pam_pressmatch_dual.so) ACTIVE!
========================================================================
- Lid OPEN  -> Both ELAN (power button) and FT9201 (USB dongle) arm
               in parallel; whichever you touch unlocks immediately!
- Lid CLOSED -> Automatically skips built-in ELAN and listens on FT9201!
- Unplugged  -> Seamlessly uses whichever single reader is connected.
- Password fallback remains 100% intact everywhere.

Instant rollback to stock pam_fprintd.so anytime:
  sudo bash "$SCRIPT_DIR/rollback-dual-pam.sh"
EOF
