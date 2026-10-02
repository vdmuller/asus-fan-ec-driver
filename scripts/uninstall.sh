#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

PACKAGE=asus-fan-ec-driver
VERSION=0.1.3
ROOT=/
die() { printf '%s\n' "$*" >&2; exit 1; }
if [[ $# -ne 0 ]]; then
    [[ $# -eq 2 && $1 == --root && $2 == /* && $2 != / ]] || die 'Usage: uninstall.sh [--root /staging/root]'
    ROOT="${2%/}/"
fi
[[ $(id -u) == 0 || $ROOT != / ]] || die 'Run as root (sudo make uninstall).'
DEST="${ROOT}usr/src/$PACKAGE-$VERSION"
AUTOLOAD="${ROOT}etc/modules-load.d/$PACKAGE.conf"
DKMS_ARGS=()
if [[ $ROOT != / ]]; then
    DKMS_ARGS=(--dkmstree "${ROOT}var/lib/dkms" --sourcetree "${ROOT}usr/src" --installtree "${ROOT}lib/modules")
fi
command -v dkms >/dev/null || die 'Missing dependency: dkms'
if [[ -e $DEST ]]; then
    [[ -f $DEST/.ec-owned && $(cat "$DEST/.ec-owned") == "$PACKAGE $VERSION" ]] || die "Refusing unmanaged source: $DEST"
    (cd "$DEST" && sha256sum --check --status .ec-manifest) || die 'Modified installed sources; preserve them before removal.'
fi
if [[ -e $AUTOLOAD ]]; then
    [[ $(cat "$AUTOLOAD") == asus_fan_ec ]] || die "Refusing unmanaged autoload file: $AUTOLOAD"
fi
if [[ $ROOT == / && -d /sys/module/asus_fan_ec ]]; then
    HWMON=
    for path in /sys/class/hwmon/hwmon*; do
        [[ -r $path/name ]] || continue
        if [[ $(cat "$path/name") == asus_fan_ec ]]; then
            [[ -z $HWMON ]] || die 'Multiple matching hwmon devices; stop and investigate.'
            HWMON=$path
        fi
    done
    [[ -n $HWMON ]] || die 'Loaded module has no accessible hwmon; refusing removal.'
    printf '2\n' > "$HWMON/pwm1_enable"
    modprobe -r asus_fan_ec
    [[ ! -d /sys/module/asus_fan_ec ]] || die 'Module still loaded; refusing removal.'
fi
STATUS="$(dkms "${DKMS_ARGS[@]}" status -m "$PACKAGE" -v "$VERSION")"
# A boot file belongs to the package, not one version. Keep it when another
# version remains installed (the removed module may be restored by DKMS).
OTHER="$(dkms "${DKMS_ARGS[@]}" status -m "$PACKAGE")"
KEEP_AUTOLOAD=false
while IFS= read -r line; do
    if [[ -n $line && $line != "$PACKAGE/$VERSION,"* && $line != "$PACKAGE/$VERSION:"* && $line == *': installed'* ]]; then
        KEEP_AUTOLOAD=true
    fi
done <<< "$OTHER"
if [[ -n $STATUS ]]; then
    dkms "${DKMS_ARGS[@]}" remove -m "$PACKAGE" -v "$VERSION" --all
fi
if [[ $KEEP_AUTOLOAD == false ]]; then
    rm -f -- "$AUTOLOAD"
fi
if [[ -e $DEST ]]; then
    # Delete only files owned by this project, never unknown contents.
    rm -f -- "$DEST/Kbuild" "$DEST/Makefile" "$DEST/dkms.conf" \
        "$DEST/LICENSE" "$DEST/COPYING" \
        "$DEST/src/asus_fan_ec.c" "$DEST/src/ec_core.c" "$DEST/src/ec_core.h" \
        "$DEST/.ec-manifest" "$DEST/.ec-owned"
    rmdir -- "$DEST/src" "$DEST" || die 'Extra files preserved in source directory; inspect them manually.'
fi
printf '%s\n' 'Removed this version. MOK keys, dependencies and other drivers were preserved.'
