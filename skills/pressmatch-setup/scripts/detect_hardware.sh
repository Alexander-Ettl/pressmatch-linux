#!/usr/bin/env bash
# PressMatch Hardware & Linux Environment Probe (Read-Only, Zero Sudo)
# Outputs a clean diagnostic summary for automated setup by an AI agent or human.
set -euo pipefail

echo "=== PRESSMATCH ENVIRONMENT PROBE ==="

# 1. OS & Distro
if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  echo "DISTRO_ID=${ID:-unknown}"
  echo "DISTRO_LIKE=${ID_LIKE:-unknown}"
  echo "DISTRO_PRETTY=${PRETTY_NAME:-unknown}"
else
  echo "DISTRO_ID=unknown"
fi
echo "KERNEL=$(uname -r)"
echo "ARCH=$(uname -m)"

# 2. Desktop & Session
echo "DESKTOP=${XDG_CURRENT_DESKTOP:-unknown}"
echo "SESSION_TYPE=${XDG_SESSION_TYPE:-unknown}"
echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"

DM="unknown"
if [[ -L /etc/systemd/system/display-manager.service ]]; then
  DM="$(basename "$(readlink -f /etc/systemd/system/display-manager.service)" .service)"
fi
echo "DISPLAY_MANAGER=${DM}"

# 3. PAM layout & libpam location
PAM_LAYOUT="unknown"
if [[ -f /etc/pam.d/common-auth ]]; then
  PAM_LAYOUT="debian-common-auth"
elif [[ -f /etc/pam.d/system-auth ]]; then
  PAM_LAYOUT="rhel-arch-system-auth"
fi
echo "PAM_LAYOUT=${PAM_LAYOUT}"

PAM_SEC_DIR="unknown"
for d in /usr/lib/x86_64-linux-gnu/security /lib/x86_64-linux-gnu/security /usr/lib64/security /usr/lib/security; do
  if [[ -d "$d" ]]; then
    PAM_SEC_DIR="$d"
    break
  fi
done
echo "PAM_SECURITY_DIR=${PAM_SEC_DIR}"

LIBPAM_PATH="unknown"
for p in /usr/lib/x86_64-linux-gnu/libpam.so.0 /usr/lib64/libpam.so.0 /usr/lib/libpam.so.0 /lib/x86_64-linux-gnu/libpam.so.0; do
  if [[ -f "$p" ]]; then
    LIBPAM_PATH="$p"
    break
  fi
done
echo "LIBPAM_PATH=${LIBPAM_PATH}"

# 4. fprintd & Plasma 6 lockscreen availability
FPRINTD_BIN="$(command -v fprintd-list || echo none)"
echo "FPRINTD_CLI=${FPRINTD_BIN}"
FPRINTD_DAEMON="none"
for d in /usr/libexec/fprintd /usr/lib/fprintd/fprintd /usr/lib/x86_64-linux-gnu/fprintd/fprintd; do
  if [[ -x "$d" ]]; then
    FPRINTD_DAEMON="$d"
    break
  fi
done
echo "FPRINTD_DAEMON=${FPRINTD_DAEMON}"
echo "PLASMA6_LOCKSCREEN=$([[ -d /usr/share/plasma/shells/org.kde.plasma.desktop/contents/lockscreen ]] && echo yes || echo no)"
echo "LAYERSHELL_QT6=$(ldconfig -p 2>/dev/null | grep -q 'libLayerShellQtInterface\.so\.6' && echo yes || echo no)"

# 5. Laptop lid ACPI switch
LID_NODE="none"
for f in /proc/acpi/button/lid/*/state; do
  if [[ -f "$f" ]]; then
    LID_NODE="$f"
    break
  fi
done
echo "LID_ACPI_NODE=${LID_NODE}"

# 6. Build toolchain quick check
echo "TOOLS_PRESENT=$(for t in git gcc meson ninja pkg-config python3 lsusb; do command -v "$t" >/dev/null && printf '%s ' "$t"; done)"
echo "PYQT6_OK=$(python3 -c 'import PyQt6' 2>/dev/null && echo yes || echo no)"
echo "DBUS_PY_OK=$(python3 -c 'import dbus' 2>/dev/null && echo yes || echo no)"

# 7. USB Fingerprint Device Scan & Classification
echo "=== USB FINGERPRINT SENSORS ==="
FOUND=0
if command -v lsusb >/dev/null 2>&1; then
  while IFS= read -r line; do
    vidpid="$(echo "$line" | awk '{print $6}')"
    desc="$(echo "$line" | cut -d' ' -f7-)"
    vid="${vidpid%%:*}"
    pid="${vidpid##*:}"
    tier=""
    case "${vidpid,,}" in
      04f3:0c6e)
        tier="DIRECT_ELAN (150x52 Power-Button + One-Touch SRAM SSO)"
        ;;
      04f3:0c4f|04f3:0c63)
        tier="DIRECT_ELAN (ELAN Press Sensor)"
        ;;
      2808:93a9)
        tier="DIRECT_FT9201 (96x96 Native Open-Source; needs local ft9201_fw.h)"
        ;;
      04f3:0c*)
        if echo "$desc" | grep -qiE 'fingerprint|elan'; then
          tier="CANDIDATE_ELAN (Check MoC vs raw press sensor — see references/sensor-porting.md)"
        fi
        ;;
      2808:*)
        tier="CANDIDATE_FOCALTECH (Check raw-frame vs MoC — see references/sensor-porting.md)"
        ;;
      27c6:*|06cb:*|138a:*|1c7a:*|10a5:*|04e8:730b|147e:*|08ff:*)
        tier="CANDIDATE_OTHER (Verify MoC vs Match-on-Host in stock libfprint first)"
        ;;
      *)
        if echo "$desc" | grep -qiE 'fingerprint|biometric|touch.*sensor'; then
          tier="CANDIDATE_OTHER (Generic fingerprint USB descriptor)"
        fi
        ;;
    esac
    if [[ -n "$tier" ]]; then
      FOUND=$((FOUND + 1))
      echo "SENSOR_${FOUND}_ID=${vidpid,,}"
      echo "SENSOR_${FOUND}_TIER=${tier}"
      echo "SENSOR_${FOUND}_DESC=${desc}"
    fi
  done < <(lsusb)
fi

if [[ "$FOUND" -eq 0 ]]; then
  echo "SENSORS_FOUND=0"
  echo "NOTE=No USB fingerprint sensor detected via lsusb (check SPI devices under /sys/bus/spi/devices if internal)."
else
  echo "SENSORS_FOUND=${FOUND}"
fi
