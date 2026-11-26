# Parallel fingerprint and password for sudo-rs

On a local interactive terminal, the patched `sudo` shows the normal password prompt while PressMatch listens on both fingerprint readers. Either method can grant authentication. Each method has its own retry budget, so exhausting one leaves the other available. If a fingerprint matches while you are typing, sudo discards the partial password, restores the terminal, and continues. If the password wins, PressMatch stops verification and releases the readers. Ctrl-C cancels the prompt.

The fingerprint profile allows five attempts and has no elapsed-time limit. The password worker honors `passwd_tries` and `passwd_timeout` in sudoers. Set `passwd_timeout=0` for the users who should be able to leave both methods open indefinitely. The sudo credential cache has its own `timestamp_timeout`.

## Build and test

The patch targets [sudo-rs v0.2.13](https://github.com/trifectatechfoundation/sudo-rs/tree/v0.2.13) and Ubuntu's `rust-sudo-rs 0.2.13-0ubuntu1.2` source package. Build with Rust, Git, a C toolchain, and the PAM development headers. For the Ubuntu build, use the extracted source with Ubuntu patches applied and the supplied `rust-vendor` directory.

```sh
dual-reader/sudo-parallel/build.sh /path/to/sudo-rs-source /tmp/pressmatch-sudo
```

The build runs the sudo-rs library tests and writes a regular executable. Run the terminal integration tests from a machine that permits root mount and PID namespaces. The suite also needs Python 3.

```sh
sudo dual-reader/sudo-parallel/tests/isolated.sh /tmp/pressmatch-sudo
```

On the target machine with PressMatch readers already working in fprintd, the hardware test additionally requires `pkg-config` and GLib/GIO development headers. Follow the terminal prompts to authenticate first by finger and then by password.

```sh
sudo dual-reader/sudo-parallel/tests/hardware.sh /tmp/pressmatch-sudo
```

Both scripts stage the candidate and its PAM and sudoers configuration in temporary namespaces. The hardware test connects to the host's fingerprint daemon. Neither script replaces the system sudo binary or edits the host's PAM or sudoers files.

## Authentication policy

Parallel mode requires the root-owned PAM profiles in [pam/](pam/) and the matching PressMatch PAM module. The supplied password profiles are for Debian/Ubuntu local accounts using `pam_unix`. Review the site's existing authentication stack before enabling them. Sites with MFA, directory-backed passwords, or PAM modules that carry credentials from authentication into the session need a separate integration.

The normal sudoers command policy, PAM account and session checks, and credential cache still run in sudo-rs. Non-interactive authentication, askpass, password input from stdin, alternate-account password policies, and systems without both opt-in profiles use the normal sudo-rs path. This patch targets `sudo` on a foreground terminal. Desktop login and lock-screen policy are configured separately.
