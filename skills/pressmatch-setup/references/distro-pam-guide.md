# Distro PAM, Lockscreen & One-Touch SSO Wiring Guide

Before modifying any file under `/etc/pam.d/`, **always**:
1. Keep an active root terminal open (`sudo -i`) until authentication is verified in a *second* terminal.
2. Verify `pam_pressmatch_dual.so` resolves every symbol cleanly:
   ```bash
   python3 -c 'import ctypes, os; lib=ctypes.CDLL("./dual-reader/pam_pressmatch_dual.so", mode=os.RTLD_NOW); assert hasattr(lib, "pam_sm_authenticate"); print("OK")'
   ```
3. Back up every `/etc/pam.d/*` file you plan to touch into `/var/backups/pressmatch-dual-pam/`.

---

## 1. Ubuntu / Debian (KDE Plasma 6 + SDDM)

Run the included safe installer directly:
```bash
sudo bash dual-reader/install-dual-pam.sh
```
This installs `pam_pressmatch_dual.so` to `/usr/lib/x86_64-linux-gnu/security/`, automatically enables fingerprint auth in `/etc/pam.d/common-auth` (via `pam-auth-update --enable fprintd` if not yet enabled, with a safe `sufficient` fallback above `pam_unix.so`) so **`sudo`** and login work out-of-the-box on fresh installs, and wires `sddm`/`gdm-fingerprint`, `kde-fingerprint`, `polkit-1`, and (when ELAN `04f3:0c6e` is present) One-Touch Power-On `sddm-autologin` + `systemd-sleep` Software SSO.

Rollback anytime:
```bash
sudo bash dual-reader/rollback-dual-pam.sh
```

---

## 2. Fedora / RHEL (authselect + `system-auth`)

1. Install the module to `/usr/lib64/security/pam_pressmatch_dual.so` (`chmod 644`).
2. Create a custom `authselect` profile based on `sssd` + `with-fingerprint` (never hand-edit generated `/etc/pam.d/system-auth` without a custom profile, or `authselect apply-changes` will revert it):
   ```bash
   sudo authselect create-profile pressmatch --base-on=sssd
   sudo sed -i 's/pam_fprintd\.so/pam_pressmatch_dual.so/g' /etc/authselect/custom/pressmatch/system-auth
   sudo sed -i 's/pam_fprintd\.so/pam_pressmatch_dual.so/g' /etc/authselect/custom/pressmatch/fingerprint-auth
   sudo authselect select custom/pressmatch with-fingerprint --force
   ```
3. For KDE Plasma 6 lockscreen without timeout, copy `/usr/lib/pam.d/kde-fingerprint` to `/etc/pam.d/kde-fingerprint` and replace `pam_fprintd.so` with `pam_pressmatch_dual.so max-tries=5 timeout=-1`.
4. **SELinux note:** Restore file context after installing `.so` files:
   ```bash
   sudo restorecon -v /usr/lib64/security/pam_pressmatch_dual.so /usr/local/lib/pressmatch-v4.1/libfprint-2.so.2.0.0
   ```

---

## 3. Arch Linux (`system-auth` / `polkit-1` / `kde`)

1. Install the module to `/usr/lib/security/pam_pressmatch_dual.so` (`chmod 644`).
2. Add the sufficient fingerprint line **above** `pam_unix.so` in `/etc/pam.d/system-local-login`, `/etc/pam.d/sudo`, `/etc/pam.d/polkit-1`, and `/etc/pam.d/kde` (or `/etc/pam.d/kde-fingerprint` on Plasma 6):
   ```pam
   auth    sufficient    pam_pressmatch_dual.so max-tries=3 timeout=-1
   ```
   Ensure `pam_unix.so` remains directly below it so password fallback always works.

---

## 4. GNOME (GDM) Notes

- Replace `pam_fprintd.so` with `pam_pressmatch_dual.so` in `/etc/pam.d/gdm-fingerprint` (and `common-auth` / `polkit-1`).
- Skip `/etc/sddm.conf.d/30-pressmatch-poweron.conf` and `pressmatch-gui/lockscreen/install-lockscreen-hud.sh` (those target SDDM & KDE Plasma 6).
- The PyQt6 Setup Wizard (`pressmatch-gui/pressmatch-setup`) works natively on GNOME Wayland & X11.

---

## 5. Emergency Recovery (If Locked Out of PAM)

If a PAM misconfiguration ever blocks login:
1. Boot to single-user / recovery shell (or use the open root terminal).
2. Restore backups:
   ```bash
   sudo bash dual-reader/rollback-dual-pam.sh
   ```
3. Disable side-by-side `libfprint` v4.1 if needed:
   ```bash
   sudo rm -f /etc/systemd/system/fprintd.service.d/elanpress-v4.conf
   sudo systemctl daemon-reload && sudo systemctl restart fprintd.service
   ```
