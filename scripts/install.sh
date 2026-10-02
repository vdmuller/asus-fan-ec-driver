#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PACKAGE=asus-fan-ec-driver
VERSION=0.1.3
ROOT=/

die() { printf '%s\n' "$*" >&2; exit 1; }
if [[ $# -ne 0 ]]; then
    [[ $# -eq 2 && $1 == --root && $2 == /* && $2 != / ]] || die 'Usage: install.sh [--root /staging/root]'
    ROOT="${2%/}/"
fi
[[ $(id -u) == 0 || $ROOT != / ]] || die 'Run as root (sudo make install).'
for command in dkms make gcc sha256sum cmp install uname; do
    command -v "$command" >/dev/null || die "Missing dependency: $command"
done
KERNEL="$(uname -r)"
[[ -f /lib/modules/$KERNEL/build/Makefile ]] || die "Install linux-headers-$KERNEL first."
DEST="${ROOT}usr/src/$PACKAGE-$VERSION"
AUTOLOAD="${ROOT}etc/modules-load.d/$PACKAGE.conf"
DKMS_ARGS=()
if [[ $ROOT != / ]]; then
    mkdir -p "${ROOT}var/lib/dkms" "${ROOT}usr/src" "${ROOT}lib/modules"
    DKMS_ARGS=(--dkmstree "${ROOT}var/lib/dkms" --sourcetree "${ROOT}usr/src" --installtree "${ROOT}lib/modules")
fi
FILES=(Kbuild Makefile dkms.conf LICENSE COPYING src/asus_fan_ec.c src/ec_core.c src/ec_core.h)

# Refuse collisions before making any changes to an installed version.
if [[ -e $DEST ]]; then
    [[ -f $DEST/.ec-owned && $(cat "$DEST/.ec-owned") == "$PACKAGE $VERSION" ]] || die "Unmanaged destination: $DEST"
    for file in "${FILES[@]}"; do
        cmp -s "$SOURCE_DIR/$file" "$DEST/$file" || die "Different source already installed as $VERSION: $file. Bump version."
    done
    (cd "$DEST" && sha256sum --check --status .ec-manifest) || die 'Installed source manifest mismatch.'
fi
if [[ -e $AUTOLOAD ]]; then
    [[ $(cat "$AUTOLOAD") == asus_fan_ec ]] || die "Unmanaged autoload file: $AUTOLOAD"
fi
if [[ ! -e $DEST ]]; then
    install -d -m 0755 "$DEST/src"
    for file in "${FILES[@]}"; do
        install -m 0644 "$SOURCE_DIR/$file" "$DEST/$file"
    done
    (cd "$DEST" && sha256sum "${FILES[@]}" > .ec-manifest)
    printf '%s\n' "$PACKAGE $VERSION" > "$DEST/.ec-owned"
fi
STATUS="$(dkms "${DKMS_ARGS[@]}" status -m "$PACKAGE" -v "$VERSION")"
if [[ -z $STATUS ]]; then
    dkms "${DKMS_ARGS[@]}" add -m "$PACKAGE" -v "$VERSION"
fi
dkms "${DKMS_ARGS[@]}" install -m "$PACKAGE" -v "$VERSION" -k "$KERNEL"
install -d -m 0755 "${ROOT}etc/modules-load.d"
printf '%s\n' asus_fan_ec > "$AUTOLOAD"
chmod 0644 "$AUTOLOAD"
printf '%s\n' 'Installed. No module was loaded and no fan was changed.' \
    'Review competing controllers and Secure Boot enrollment before: sudo modprobe asus_fan_ec' \
    'Use dkms autoinstall -k KERNEL for other already-installed kernels.'
