# PressMatch for Linux

**High-Accuracy Small-Area Capacitive Fingerprint Drivers, Simultaneous Dual-Reader PAM, One-Touch Power-On Login, and Touch ID-Style Visual Feedback for Linux.**

PressMatch targets small-area capacitive press fingerprint sensors where traditional minutiae matchers (NBIS `mindtct`/`bozorth3`) collapse:
- **ELAN `04f3:0c6e`** (`150 × 52` px capacitive power-button strip; shared family with `04f3:0c4f`, `04f3:0c63`)
- **FocalTech FT9201 `2808:93a9`** (`96 × 96` px native full-frame USB dongle)

---

## Highlights

- **PressMatch v4.1 Matcher (`release-v4.1/source/libfprint/drivers/`):**
  - 4-bit packed coverage-normalized frames + compact `u16` PoseGraph + RootSIFT + elastic ZNCC verification (`±1 px` elastic window) + quad-tier decision thresholds.
  - **17× faster cold verification** (`~14–17 ms` CPU, `~240 ms` total touch-to-unlock) with **0 / 1,200,000+ false accepts** across 11 public 500 DPI fingerprint corpora (`paper-benchmark/PAPER_BENCHMARK_REPORT.md`).
- **One-Touch Power-On & Sleep Wake Login (ELAN `EMSE1000` SSO):**
  - Implements ELAN's hardware pre-captured power-button SRAM commands (`0x9a 0x10` check, `0x9a 0x18` pull frame, `0x9a 0x14` clear, `SoftwareSSO_On/Off` across `s2idle` sleep) wired into `elanpress.c`, `pressmatch-elan-sso`, and SDDM `sddm-autologin` (strictly gated by `pam_pressmatch_dual.so`, falling back cleanly to the normal login screen on mismatch).
- **Simultaneous Lid-Aware Dual-Reader PAM (`dual-reader/`):**
  - `pam_pressmatch_dual.c` arms both built-in ELAN and external FT9201 in parallel over a private per-thread `GMainContext` (avoiding Qt6/Plasma 6 `kscreenlocker_greet` GLib deadlocks), with automatic lid-closed detection, `DeviceAdded` hotplug recovery, timerless KDE lockscreen & Polkit support, and intact password fallback everywhere.
- **Native Touch ID-Style Setup App & Floating Visual Feedback HUD (`pressmatch-gui/`):**
  - Localized DE/EN PyQt6 setup wizard (`app.py`, `widgets.py`, `fprintd_bridge.py`) with live spatial fingertip coverage guidance and dual-reader Try Mode.
  - Wayland `wlr-layer-shell` floating porcelain HUD pill (`pressmatch_hud.py`) for Polkit & `sudo`, plus a matching native QML pill (`lockscreen/PressMatchPill.qml`) for the KDE Plasma 6 lockscreen.
- **Reproducible Benchmark Suite (`paper-benchmark/` & `dual-guide/`):**
  - Offline multi-algorithm C/Python benchmark harness (`NBIS`, `Upstream v0`, `PressMatch v1`, `v2`, `v4.0`, `v4.1`), 4 publication figures (`paper-benchmark/figures/`), and an accessible local-only guided capture studio (`dual-guide/`).

---

## Repository Structure

| Path | Description |
|---|---|
| `release-v4.1/` | PressMatch v4.1 `libfprint` drivers (`elanpress*`, `ft9201/ft9201.c`), udev rule, ASan test, and installer scripts |
| `dual-reader/` | `pam_pressmatch_dual.c`, `elan_sso_helper.c`, and PAM/SDDM/sleep install & rollback scripts |
| `pressmatch-gui/` | Native DE/EN PyQt6 setup app, Wayland floating HUD (`pressmatch_hud.py`), and Plasma 6 lockscreen pill (`lockscreen/PressMatchPill.qml`) |
| `paper-benchmark/` | Multi-algorithm benchmark harness, analysis/plotting scripts, aggregate public benchmark results, and figures |
| `dual-guide/` | Local-only HTML guided dual-sensor capture & replay studio |

---

## Building From Source

### 1. Dual-Reader PAM Module & ELAN SSO Helper (`dual-reader/`)
```bash
cd dual-reader
gcc -O2 -shared -fPIC -Wall -Wextra pam_pressmatch_dual.c \
    $(pkg-config --cflags --libs gio-2.0) /usr/lib/x86_64-linux-gnu/libpam.so.0 \
    -o pam_pressmatch_dual.so
gcc -O2 -Wall -Wextra elan_sso_helper.c \
    $(pkg-config --cflags --libs libusb-1.0) \
    -o pressmatch-elan-sso
sudo bash install-dual-pam.sh
```

### 2. PressMatch v4.1 `libfprint` Drivers (`release-v4.1/`)
Copy `release-v4.1/source/libfprint/drivers/elanpress*` and `ft9201/ft9201.c` (plus your locally generated `ft9201_fw.h`) into a `libfprint` source tree, build with Meson/Ninja (`meson setup build && ninja -C build`), and install via `sudo bash release-v4.1/install-v4.1.sh`.

### 3. Setup GUI, Floating Desktop HUD & Plasma 6 Lockscreen Pill (`pressmatch-gui/`)
```bash
bash pressmatch-gui/install-gui.sh
```

### 4. Benchmark Tools (`paper-benchmark/`)
```bash
cd paper-benchmark
gcc -O2 -Wall -Wextra raw_capture.c $(pkg-config --cflags --libs libusb-1.0) -o raw_capture -lm
```

---

## Privacy & Proprietary Blobs Note

- **Zero personal or biometric data** is stored in this repository (`paper-benchmark/profiles/`, raw `.bin` captures, and personal reports are strictly gitignored).
- **FT9201 firmware (`ft9201_fw.h`) is excluded** from this repository; generate it locally from the vendor firmware package before compiling the FT9201 driver.
- All PAM installers preserve full password fallback (`pam_unix.so`) and create automatic backups with one-command rollback (`dual-reader/rollback-dual-pam.sh`).
