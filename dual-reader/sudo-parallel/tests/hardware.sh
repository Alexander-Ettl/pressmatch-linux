#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || $EUID -ne 0 || -z "${SUDO_USER:-}" || "$SUDO_USER" == root ]]; then
    printf 'Usage: sudo %s /path/to/test-build/sudo\n' "$0" >&2
    exit 2
fi

project="$(cd "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(realpath -- "$1")"
terminal="$(tty)" || { printf 'Run this test from a terminal.\n' >&2; exit 2; }
host_namespace="$(readlink /proc/self/ns/mnt)"
exec unshare --mount --propagation private \
    /usr/bin/bash -se -- "$host_namespace" "$project" "$binary" "$SUDO_USER" "$terminal" <<'NAMESPACE'
set -euo pipefail
[[ "$(readlink /proc/self/ns/mnt)" != "$1" ]]
project="$2"
binary="$3"
test_user="$4"
terminal="$5"
uid="$(id -u "$test_user")"
gid="$(id -g "$test_user")"
[[ "$uid" != 0 ]]

mount -t tmpfs -o mode=755,suid,exec tmpfs /mnt
root=/mnt/pressmatch-test
mkdir -p "$root"/{pam,dbus,journal,hud}
mount --bind /run/dbus "$root/dbus"
if [[ -d /run/systemd/journal ]]; then
    mount --bind /run/systemd/journal "$root/journal"
fi
if [[ -d "/run/user/$uid" ]]; then
    mount --bind "/run/user/$uid" "$root/hud"
fi
mount -t tmpfs -o mode=755,nosuid,nodev tmpfs /run
mkdir -p /run/dbus /run/systemd/journal "/run/user/$uid"
mount --bind "$root/dbus" /run/dbus
mount --bind "$root/journal" /run/systemd/journal
mount --bind "$root/hud" "/run/user/$uid"

install -o root -g root -m 4755 "$binary" "$root/sudo"
read -ra gio_flags <<< "$(pkg-config --cflags --libs gio-2.0)"
cc -O2 -shared -fPIC -Wall -Wextra "$project/dual-reader/pam_pressmatch_dual.c" \
    "${gio_flags[@]}" -l:libpam.so.0 -o "$root/fingerprint.so"
cat > "$root/record-auth" <<'RECORD'
#!/usr/bin/bash
set -eu
printf '%s\n' "$1" >> /mnt/pressmatch-test/auth-results
RECORD
chmod 0755 "$root/record-auth"
install -o root -g root -m 0600 /dev/null "$root/auth-results"
cp -a /etc/pam.d/. "$root/pam"
for profile in sudo-pressmatch-password sudo-pressmatch-password-i; do
    install -o root -g root -m 0644 "$project/dual-reader/sudo-parallel/pam/$profile" "$root/pam/$profile"
    printf 'auth required pam_exec.so seteuid %s/record-auth password\n' "$root" >> "$root/pam/$profile"
done
{
    printf 'auth required pam_exec.so seteuid %s/record-auth fingerprint-started\n' "$root"
    printf 'auth requisite %s/fingerprint.so max-tries=5 timeout=-1\n' "$root"
    printf 'auth required pam_exec.so seteuid %s/record-auth fingerprint\n' "$root"
} > "$root/pam/sudo-pressmatch-fingerprint"
mount --bind "$root/pam" /etc/pam.d
{
    printf 'Defaults passwd_tries=3, passwd_timeout=0, timestamp_timeout=0\n'
    printf '%s ALL=(root) PASSWD: /usr/bin/id -u\n' "$test_user"
} > "$root/sudoers"
chmod 0440 "$root/sudoers"
sudoers=/etc/sudoers
if [[ -e /etc/sudoers-rs ]]; then sudoers=/etc/sudoers-rs; fi
mount --bind "$root/sudoers" "$sudoers"

attempt() {
    local method="$1"
    : > "$root/auth-results"
    /usr/bin/setpriv --reuid="$uid" --regid="$gid" --init-groups \
        "$root/sudo" -k /usr/bin/id -u <"$terminal" >"$terminal" 2>"$terminal"
    if ! grep -Fxq fingerprint-started "$root/auth-results" || ! grep -Fxq "$method" "$root/auth-results"; then
        printf 'The expected parallel authentication path did not complete: %s\n' "$method" >&2
        exit 1
    fi
    printf 'Verified %s authentication with the fingerprint worker active.\n' "$method"
}

printf '\nTest 1: Scan your enrolled fingerprint at the password prompt.\n' >/dev/tty
attempt fingerprint
printf '\nTest 2: Enter your Linux password while the readers are listening.\n' >/dev/tty
attempt password
printf 'Both isolated hardware commands completed.\n'
NAMESPACE
