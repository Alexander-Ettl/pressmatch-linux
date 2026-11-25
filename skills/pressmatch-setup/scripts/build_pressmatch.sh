#!/usr/bin/env bash
# Build PressMatch v4.1 libfprint drivers, ASan test suite, dual-reader PAM module,
# and ELAN EMSE1000 SSO helper cleanly from source.
#
# Usage:
#   bash skills/pressmatch-setup/scripts/build_pressmatch.sh
#   bash skills/pressmatch-setup/scripts/build_pressmatch.sh --with-ft9201 /path/to/ft9201_fw.h
#   bash skills/pressmatch-setup/scripts/build_pressmatch.sh --libfprint-dir /existing/libfprint
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIBFPRINT_DIR="$REPO_ROOT/release-v4.1/source"
FT9201_FW=""
SKIP_CLONE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-ft9201)
      FT9201_FW="${2:?Missing path to ft9201_fw.h}"
      shift 2
      ;;
    --libfprint-dir)
      LIBFPRINT_DIR="${2:?Missing libfprint source path}"
      SKIP_CLONE=1
      shift 2
      ;;
    -h|--help)
      sed -n '2,10p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

DRIVERS_SRC="$REPO_ROOT/release-v4.1/source/libfprint/drivers"

echo "=== [1/5] Preparing upstream libfprint source tree ==="
if [[ ! -f "$LIBFPRINT_DIR/meson.build" ]]; then
  if [[ "$SKIP_CLONE" -eq 1 ]]; then
    echo "ERROR: $LIBFPRINT_DIR/meson.build not found." >&2
    exit 1
  fi
  TMP_CLONE="$(mktemp -d)"
  trap 'rm -rf "$TMP_CLONE"' EXIT
  echo "Cloning upstream libfprint (shallow)..."
  git clone --depth 1 https://gitlab.freedesktop.org/libfprint/libfprint.git "$TMP_CLONE/libfprint"
  # Copy upstream files into release-v4.1/source WITHOUT clobbering PressMatch driver sources
  cp -an "$TMP_CLONE/libfprint/." "$LIBFPRINT_DIR/"
fi

echo "=== [2/5] Syncing PressMatch v4.1 driver sources & patching Meson ==="
mkdir -p "$LIBFPRINT_DIR/libfprint/drivers/ft9201"
if [[ "$LIBFPRINT_DIR" != "$REPO_ROOT/release-v4.1/source" ]]; then
  cp -a "$DRIVERS_SRC"/elanpress*.c "$DRIVERS_SRC"/elanpress*.h "$LIBFPRINT_DIR/libfprint/drivers/"
  cp -a "$DRIVERS_SRC/ft9201/ft9201.c" "$LIBFPRINT_DIR/libfprint/drivers/ft9201/"
fi

ENABLE_FT9201=0
if [[ -n "$FT9201_FW" ]]; then
  if [[ ! -f "$FT9201_FW" ]]; then
    echo "ERROR: FT9201 firmware header not found: $FT9201_FW" >&2
    exit 1
  fi
  cp -a "$FT9201_FW" "$LIBFPRINT_DIR/libfprint/drivers/ft9201/ft9201_fw.h"
  ENABLE_FT9201=1
elif [[ -f "$LIBFPRINT_DIR/libfprint/drivers/ft9201/ft9201_fw.h" ]]; then
  ENABLE_FT9201=1
fi
echo "FT9201 enabled: $ENABLE_FT9201"

python3 - "$LIBFPRINT_DIR" "$ENABLE_FT9201" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
enable_ft = sys.argv[2] == "1"

top_meson = root / "meson.build"
lib_meson = root / "libfprint" / "meson.build"

src = top_meson.read_text(encoding="utf-8")
if "'elanpress'" not in src:
    assert "'elan'," in src, "Expected 'elan', in top-level meson.build"
    src = src.replace("'elan',", "'elan',\n    'elanpress',", 1)
if enable_ft and "'ft9201'" not in src:
    assert "'focaltech_moc'," in src, "Expected 'focaltech_moc', in top-level meson.build"
    src = src.replace("'focaltech_moc',", "'focaltech_moc',\n    'ft9201',", 1)
if "'elanpress' : [ 'pressmatch' ]" not in src:
    assert "'uru4000' : [ 'openssl' ]," in src, "Expected driver_helpers anchor in top-level meson.build"
    ins = "'uru4000' : [ 'openssl' ],\n    'elanpress' : [ 'pressmatch' ],"
    if enable_ft:
        ins += "\n    'ft9201' : [ 'pressmatch' ],"
    src = src.replace("'uru4000' : [ 'openssl' ],", ins, 1)
elif enable_ft and "'ft9201' : [ 'pressmatch' ]" not in src:
    src = src.replace(
        "'elanpress' : [ 'pressmatch' ],",
        "'elanpress' : [ 'pressmatch' ],\n    'ft9201' : [ 'pressmatch' ],",
        1,
    )
top_meson.write_text(src, encoding="utf-8")

lsrc = lib_meson.read_text(encoding="utf-8")
if "'elanpress' :" not in lsrc:
    anchor = "'elan' :\n        [ 'drivers/elan.c' ],"
    assert anchor in lsrc, "Expected 'elan' driver source anchor in libfprint/meson.build"
    lsrc = lsrc.replace(
        anchor,
        anchor + "\n    'elanpress' :\n        [ 'drivers/elanpress.c' ],",
        1,
    )
if enable_ft and "'ft9201' :" not in lsrc:
    anchor = "'focaltech_moc' :\n        [ 'drivers/focaltech_moc/focaltech_moc.c' ],"
    assert anchor in lsrc, "Expected 'focaltech_moc' driver source anchor in libfprint/meson.build"
    lsrc = lsrc.replace(
        anchor,
        anchor + "\n    'ft9201' :\n        [ 'drivers/ft9201/ft9201.c' ],",
        1,
    )
if "'pressmatch' :" not in lsrc:
    anchor = "'aes3k' :\n        [ 'drivers/aes3k.c' ],"
    assert anchor in lsrc, "Expected 'aes3k' helper_sources anchor in libfprint/meson.build"
    lsrc = lsrc.replace(
        anchor,
        anchor + "\n    'pressmatch' :\n        [ 'drivers/elanpress-match.c', 'drivers/elanpress-sift.c' ],",
        1,
    )
lib_meson.write_text(lsrc, encoding="utf-8")
print("Meson registration verified.")
PY

echo "=== [3/5] Compiling libfprint with PressMatch v4.1 ==="
BUILD_DIR="$LIBFPRINT_DIR/build"
if [[ ! -d "$BUILD_DIR" ]]; then
  meson setup "$BUILD_DIR" "$LIBFPRINT_DIR" \
    -Ddoc=false -Dgtk-examples=false -Dintrospection=false -Dinstalled-tests=false
fi
ninja -C "$BUILD_DIR" libfprint/libfprint-2.so.2.0.0

echo "=== [4/5] Running PressMatch v4.1 memory-safety check (ASan/UBSan) ==="
if [[ -f "$REPO_ROOT/release-v4.1/test_v41_asan.c" ]]; then
  gcc -O1 -g -fsanitize=address,undefined \
    -I"$LIBFPRINT_DIR/libfprint/drivers" \
    "$REPO_ROOT/release-v4.1/test_v41_asan.c" \
    "$LIBFPRINT_DIR/libfprint/drivers/elanpress-match.c" \
    "$LIBFPRINT_DIR/libfprint/drivers/elanpress-sift.c" \
    $(pkg-config --cflags --libs glib-2.0) -lm \
    -o "$REPO_ROOT/release-v4.1/test_v41_asan"
  "$REPO_ROOT/release-v4.1/test_v41_asan"
fi

echo "=== [5/5] Compiling pam_pressmatch_dual.so & pressmatch-elan-sso ==="
LIBPAM_LINK="-lpam"
if ! ldconfig -p 2>/dev/null | grep -q 'libpam\.so '; then
  for p in /usr/lib/x86_64-linux-gnu/libpam.so.0 /usr/lib64/libpam.so.0 /usr/lib/libpam.so.0; do
    if [[ -f "$p" ]]; then
      LIBPAM_LINK="$p"
      break
    fi
  done
fi

gcc -O2 -shared -fPIC -Wall -Wextra \
  "$REPO_ROOT/dual-reader/pam_pressmatch_dual.c" \
  $(pkg-config --cflags --libs gio-2.0) "$LIBPAM_LINK" \
  -o "$REPO_ROOT/dual-reader/pam_pressmatch_dual.so"

gcc -O2 -Wall -Wextra \
  "$REPO_ROOT/dual-reader/elan_sso_helper.c" \
  $(pkg-config --cflags --libs libusb-1.0) \
  -o "$REPO_ROOT/dual-reader/pressmatch-elan-sso"

echo ""
echo "Build complete!"
echo "  libfprint: $BUILD_DIR/libfprint/libfprint-2.so.2.0.0"
echo "  PAM:       $REPO_ROOT/dual-reader/pam_pressmatch_dual.so"
echo "  SSO:       $REPO_ROOT/dual-reader/pressmatch-elan-sso"
echo ""
echo "Next: install side-by-side with:"
echo "  sudo bash \"$REPO_ROOT/release-v4.1/install-v4.1.sh\""
