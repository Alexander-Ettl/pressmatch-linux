---
name: pressmatch-setup
description: >-
  Install, build, adapt, and verify the complete PressMatch v4.1 Linux fingerprint
  stack (pure-C RootSIFT + 2D Pose-Graph + Elastic ZNCC libfprint drivers,
  simultaneous lid-aware dual-reader PAM, optional ELAN One-Touch Power-On SSO,
  and Touch ID-style visual HUD & PyQt6 setup wizard) on a user's Linux machine.
  Use this skill when a user wants an AI agent to detect their fingerprint reader,
  wire up PressMatch from this repository, port a similar small-area capacitive
  sensor if needed, and verify login/lockscreen/Polkit unlock end-to-end.
---

# PressMatch Linux Setup & Sensor Integration Skill

This skill guides an AI agent through installing, compiling, wiring, and verifying the full **PressMatch v4.1** biometric stack on a user's Linux machine — safely preserving password fallback at every step and adapting to the user's exact sensor, distro, and desktop environment.

## Safety Rules (Non-Negotiable)

1. **Never overwrite distro `libfprint` binaries.** Always install side-by-side into `/usr/local/lib/pressmatch-v4.1` via a `systemd` drop-in for `fprintd.service` so uninstalling or rolling back is instantaneous.
2. **Never break password authentication.** Before modifying any `/etc/pam.d/*` file, back up all target files to `/var/backups/pressmatch-dual-pam`, verify `pam_pressmatch_dual.so` loads cleanly via `dlopen(RTLD_NOW)`, and keep `pam_unix.so` intact as fallback.
3. **Never commit or upload biometric data or vendor firmware.** Raw fingerprint frames, templates, and proprietary firmware headers (such as `ft9201_fw.h`) stay strictly local on the user's machine.
4. **Ask before privileged writes or SDDM autologin changes.** Confirm with the user before enabling PAM changes, One-Touch Power-On `sddm-autologin`, or modifying system files with `sudo`.

---

## Step 1: Detect Hardware & Linux Environment

Run the read-only hardware probe from the repository root:

```bash
bash skills/pressmatch-setup/scripts/detect_hardware.sh
```

Review its report to determine:
- **Distro & package manager** (`apt`, `dnf`, `pacman`, etc.)
- **Desktop & display stack** (KDE Plasma 6 / GNOME / wlroots, Wayland vs X11, SDDM / GDM / LightDM)
- **PAM layout** (`common-auth` on Debian/Ubuntu vs `system-auth` on Fedora/Arch)
- **Detected USB fingerprint sensor(s)** and their support tier:

| Classification | USB VID:PID | Path Forward |
|---|---|---|
| `DIRECT_ELAN` | `04f3:0c6e`, `04f3:0c4f`, `04f3:0c63` | Directly supported by `elanpress`. `04f3:0c6e` (`EMSE1000`) also supports One-Touch Power-On & `s2idle` Sleep SSO. |
| `DIRECT_FT9201` | `2808:93a9` | Directly supported by native `96×96` `ft9201.c`. Requires the user to provide/extract their local `ft9201_fw.h` (see Step 2B). |
| `CANDIDATE_ELAN` | Other `04f3:0cXX` non-MoC press sensor | Follow [Sensor Porting Guide](./references/sensor-porting.md) to verify bulk/command endpoints, probe frame dimensions, and register the PID in `elanpress_id_table`. |
| `MATCH_ON_CHIP` | Known MoC sensors (`elanmoc`, `goodixmoc`, `fpcmoc`, `synaptics`, `focaltech_moc`) | Inform the user: Match-on-Chip sensors run matching inside hardware firmware, not host `libfprint` — stock `libfprint` already handles them (or use `pam_pressmatch_dual.so` + Touch ID HUD only!). |
| `CANDIDATE_OTHER` | Other raw-frame small capacitive sensor | Inspect `lsusb -v` and consult [Sensor Porting Guide](./references/sensor-porting.md) before modifying drivers. |

---

## Step 2: Prepare Build Dependencies & Firmware (If Needed)

### 2A. Install Build & Runtime Dependencies
Install only what is missing for the user's distro:
- **Ubuntu / Debian:**
  ```bash
  sudo apt-get update && sudo apt-get install -y \
    git gcc g++ meson ninja-build pkg-config fprintd libfprint-2-2 \
    libglib2.0-dev libgusb-dev libpixman-1-dev libnss3-dev libgudev-1.0-dev \
    libpam0g-dev libusb-1.0-0-dev python3-pyqt6 python3-dbus
  ```
- **Fedora:**
  ```bash
  sudo dnf install -y \
    git gcc gcc-c++ meson ninja-build pkgconf-pkg-config fprintd \
    glib2-devel libgusb-devel pixman-devel nss-devel libgudev-devel \
    pam-devel libusb1-devel python3-pyqt6 python3-dbus
  ```
- **Arch Linux:**
  ```bash
  sudo pacman -S --needed \
    git base-devel meson ninja pkgconf fprintd \
    glib2 libgusb pixman nss libgudev pam libusb python-pyqt6 python-dbus
  ```
*(Optional for Wayland floating HUD pill on KDE Plasma 6: ensure `libLayerShellQtInterface.so.6` / `layer-shell-qt` is installed.)*

### 2B. FT9201 Firmware (Only if `2808:93a9` is present)
`ft9201_fw.h` is proprietary vendor firmware and intentionally excluded from this repository. Ask the user to supply the path to their locally obtained `ft9201_fw.h` (or extract it from their own FocalTech Windows driver package using the community FT9201 firmware header layout). If the user does **not** have an FT9201 dongle, build with ELAN only — no firmware header is needed.

---

## Step 3: Build PressMatch v4.1 Core, PAM Module & SSO Helper

Run the automated build helper from the repository root:

```bash
# ELAN-only build:
bash skills/pressmatch-setup/scripts/build_pressmatch.sh

# ELAN + FocalTech FT9201 build (with user's local firmware header):
bash skills/pressmatch-setup/scripts/build_pressmatch.sh --with-ft9201 /path/to/ft9201_fw.h
```

What this script does cleanly and reproducibly:
1. Clones upstream `libfprint` into `release-v4.1/source` (reuses it if already present).
2. Copies PressMatch v4.1 driver sources (`elanpress*`, and `ft9201/ft9201.c` + `ft9201_fw.h` when enabled).
3. Idempotently registers `elanpress`, `ft9201` (if enabled), and the `pressmatch` helper library in `meson.build` and `libfprint/meson.build`.
4. Compiles `libfprint-2.so.2.0.0` via Meson/Ninja and runs the AddressSanitizer safety suite (`release-v4.1/test_v41_asan.c`) to verify zero memory errors.
5. Compiles `dual-reader/pam_pressmatch_dual.so` (auto-detecting the distro's `libpam.so` path) and `dual-reader/pressmatch-elan-sso`.

### Install Side-by-Side `libfprint` v4.1
```bash
sudo bash release-v4.1/install-v4.1.sh
```

**Verify Step 3:**
```bash
systemctl status fprintd.service --no-pager
fprintd-list "$USER"
```
Confirm `fprintd-list` reports the user's reader(s) with no crash or ABI error in `journalctl -u fprintd -n 30`.

---

## Step 4: Wire Simultaneous Dual-Reader PAM & Optional One-Touch SSO

Before editing PAM, consult [Distro PAM & SSO Guide](./references/distro-pam-guide.md) to match the user's distribution and display manager:
- **Ubuntu / Debian with KDE Plasma 6 + SDDM:** `sudo bash dual-reader/install-dual-pam.sh` works out of the box (backs up PAM configs, verifies `dlopen(RTLD_NOW)`, wires `common-auth`, `sddm`, `kde-fingerprint`, `polkit-1`, and optionally `sddm-autologin` + `systemd-sleep` for ELAN `04f3:0c6e`).
- **Fedora / Arch / GNOME / LightDM / Custom setups:** Adapt the exact PAM lines in [Distro PAM & SSO Guide](./references/distro-pam-guide.md) instead of blindly running the Ubuntu-targeted `install-dual-pam.sh`.
- **Single-reader or non-`04f3:0c6e` systems:** Skip SDDM `sddm-autologin` / `pressmatch-elan-sso` sleep hooks if the hardware is not ELAN `EMSE1000` (`04f3:0c6e`), unless hardware probe (`pressmatch-elan-sso --check`) confirms SRAM SSO support (`40 9a 10 ...`).

**Verify Step 4:**
1. Confirm `pam_pressmatch_dual.so` is installed in the distro's PAM security directory and passes `ldd` with zero missing libraries.
2. Keep a root shell open and test `sudo -k true` in a **new** terminal — verify fingerprint prompt activates AND pressing Ctrl+C / waiting / wrong finger falls back cleanly to password authentication.

---

## Step 5: Install Native Touch ID Visual Feedback HUD & Setup Wizard

Run as the normal desktop user (no `sudo` required):

```bash
bash pressmatch-gui/install-gui.sh
```

This installs:
- **PressMatch Fingerprint Setup** desktop launcher (`~/.local/share/applications/pressmatch-setup.desktop`)
- **Floating Wayland Touch ID HUD** systemd user service (`pressmatch-hud.service`) for Polkit & `sudo`
- **KDE Plasma 6 Lockscreen Touch ID Pill** user-local QML override (`~/.local/share/plasma/shells/org.kde.plasma.desktop`, automatically installed only if Plasma 6 lockscreen is present; zero root required, 1-command revert)

**Verify Step 5:**
```bash
systemctl --user status pressmatch-hud.service --no-pager
```

---

## Step 6: Enroll Fingers & End-to-End Hardware Verification

1. Launch the localized Touch ID-style spatial enrollment wizard:
   ```bash
   ./pressmatch-gui/pressmatch-setup
   ```
   *(Or headless CLI enrollment: `fprintd-enroll -f right-index-finger`.)*
2. Guide the user to enroll covering center, edges, and slight tilts until spatial coverage reaches 100% (12 strips on ELAN, 10 stages on FT9201).
3. Test live verification:
   - Inside **Setup Wizard → Try Mode** (tests Both Readers / Power Button / USB Reader simultaneously)
   - Terminal: `sudo -k true` (check floating HUD pill + instant unlock)
   - Lockscreen: lock session (`Super+L`) and unlock by touch (check QML pill + instant unlock)
   - *(Optional, ELAN `04f3:0c6e` only)* Test sleep/wake or cold power-on SSO.
4. Report a crisp summary to the user: active sensor(s), installed components, live verification results, and the exact rollback commands (`sudo bash dual-reader/rollback-dual-pam.sh` and removing `/etc/systemd/system/fprintd.service.d/elanpress-v4.conf`).
