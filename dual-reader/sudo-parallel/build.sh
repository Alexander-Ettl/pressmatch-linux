#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'Usage: %s /path/to/sudo-rs-source /path/to/output-binary\n' "$0" >&2
    exit 2
fi
here="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_tree="$(realpath -- "$1")"
output="$(realpath -m -- "$2")"
[[ -f "$source_tree/Cargo.toml" && -f "$source_tree/Cargo.lock" && ! -d "$output" ]]
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
tar -C "$source_tree" --exclude=./.git --exclude=./target -cf - . | tar -C "$work" -xf -
cd "$work"
git apply "$here/sudo-rs-0.2.13.patch"

features=pam-login
cargo_options=()
if [[ -d rust-vendor && -f debian/changelog ]]; then
    upstream_version="$(dpkg-parsechangelog -S Version)"
    export SUDO_RS_VERSION="${upstream_version}+pressmatch"
    features=pam-login,apparmor,sudoedit,gettext
    cargo_options=(
        --offline
        --config 'source.crates-io.replace-with="ubuntu-vendor"'
        --config 'source.ubuntu-vendor.directory="rust-vendor"'
    )
fi
export CARGO_TARGET_DIR="$work/target"
cargo build --locked --release --bin sudo --features "$features" "${cargo_options[@]}"
cargo test --locked --lib --features "$features" "${cargo_options[@]}"
install -m 0755 "$CARGO_TARGET_DIR/release/sudo" "$output"
printf 'Built standalone candidate at %s\n' "$output"
