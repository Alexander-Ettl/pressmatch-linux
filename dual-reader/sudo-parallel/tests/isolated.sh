#!/usr/bin/env bash
set -euo pipefail

if (( $# < 1 || $# > 2 )) || [[ $EUID != 0 ]]; then
    printf 'Usage: sudo %s /path/to/test-build/sudo [non-root-test-user]\n' "$0" >&2
    exit 2
fi

test_dir="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
sudo_binary="$(realpath -- "$1")"
test_user="${2:-${SUDO_USER:-}}"
[[ -f "$sudo_binary" && -n "$test_user" && "$(id -u "$test_user")" != 0 ]]
host_namespace="$(readlink /proc/self/ns/mnt)"

exec unshare --mount --propagation private --pid --fork --mount-proc \
    /usr/bin/bash -se -- "$host_namespace" "$test_dir" "$sudo_binary" "$test_user" "${PAM_INCLUDE_DIR:-/usr/include}" <<'NAMESPACE'
set -euo pipefail
[[ "$(readlink /proc/self/ns/mnt)" != "$1" && "$$" -eq 1 ]]
test_dir="$2"
sudo_binary="$3"
test_user="$4"
pam_include="$5"
mount -t tmpfs -o mode=755,suid,exec tmpfs /mnt
mount -t tmpfs -o mode=755,nosuid,nodev tmpfs /run
root=/mnt/pressmatch-test
mkdir -p "$root"/{pam,case}
install -o root -g root -m 4755 "$sudo_binary" "$root/sudo"
cc -shared -fPIC -Wall -Wextra -Werror -I"$pam_include" "$test_dir/pam_fixture.c" \
    -o "$root/fixture.so" -l:libpam.so.0
{
    printf 'Defaults passwd_tries=2, passwd_timeout=0, timestamp_timeout=0, pwfeedback\n'
    printf '%s ALL=(root) PASSWD: /usr/bin/id -u\n' "$test_user"
} > "$root/sudoers"
chmod 0440 "$root/sudoers"
sudoers=/etc/sudoers
if [[ -e /etc/sudoers-rs ]]; then sudoers=/etc/sudoers-rs; fi
mount --bind "$root/sudoers" "$sudoers"
mount --bind "$root/pam" /etc/pam.d
exec /usr/bin/python3 "$test_dir/pty_cases.py" "$test_user"
NAMESPACE
