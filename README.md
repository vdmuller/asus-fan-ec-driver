# asus-fan-ec-driver

A small, out-of-tree Linux hwmon driver for the proprietary ASUS FX608 embedded
controller (EC) fan-control protocol. It sets the duty of two fan channels and
switches their shared control mode between manual and the EC's automatic policy.
It is packaged as source for DKMS and designed for Ubuntu 26.04 amd64 with Secure
Boot enabled and an enrolled module-signing certificate.

**Status: initial implementation; first owner-assisted load and manual-duty smoke test passed on FX608JHR. Full hardware validation remains pending.**
Compilation and simulation do not establish hardware compatibility or successful
Secure Boot loading. See [validation status](docs/decisions.md) and the
[assisted hardware checklist](docs/hardware-validation.md).

## Hardware support and boundaries

The module accepts DMI system vendor `ASUSTeK COMPUTER INC.` and a board name
starting with `FX608`. It also requires two fans and a recognized EC mode. There
is no force option. This family match is a support policy, not evidence that all
FX608 variants use identical firmware.

| Hardware | Evidence |
| --- | --- |
| TUF F16 FX608JHR, i7-14650HX, BIOS FX608JHR.309 | Owner-assisted module 0.1.3 smoke test passed on kernel 7.0.0-34-generic with Secure Boot disabled: automatic mode at probe, manual mode with duties 128/192 and matching explicit reads, physical fan response, and explicit automatic-mode readback after release. Second-channel preservation passed in explicit readback. Certificate enrollment and autoload with Secure Boot enabled passed on this kernel; The owner also reports a successful manual-control/release smoke test under Secure Boot. One automatic and one manual suspend/resume cycle passed, including restoration of duties 128/192. Unload/reload also passed, with module/hwmon removal, no reported removal errors and automatic mode after reload. Repeated PM cycles and second-kernel loading remain pending. |
| Other FX608 models / BIOS versions | Accepted by DMI, not tested |
| Other ASUS models | Rejected before port access |

The reference's RPM and temperature telemetry does not work on the owner's
machine. This driver does not use that telemetry or map its MMIO aperture. It
does not supply fan curves, temperature sensors, RPM, a daemon, a watchdog or a
thermal protection policy. Existing kernel temperature/RPM drivers remain intact.

**Manual duty is held until changed**, including duty 0, which may stop a fan.
There is no timeout when the controlling process exits. Returning control to the
EC requires writing `2` to `pwm1_enable`; writing a zero duty does not release it.
Use one fan controller at a time. A module mutex cannot coordinate external port
access, firmware or another tool changing ASUS fan modes/profiles.

## How it works

The control channel uses data port `0x25c` and command/status port `0x25d`, separate
from the standard ACPI EC. Transactions drain stale output, wait for the input
buffer, send preamble `0xff` followed by `0xdd`, and access table 2 with direction
bytes `0x02` (read) or `0x82` (write).

| Register | Purpose |
| --- | --- |
| `0x30` | Fan count: must be 2 |
| `0x31` | Shared mode: EC 0 automatic, EC 1 manual |
| `0x32` | Fan selector: 0 or 1 |
| `0x35` | Selected fan duty: 0–255 |

The module reserves the two I/O ports. It exposes no arbitrary command/register
API. An entire selection/write/read or PM operation is protected by one
device mutex. Setting duty selects the fan, enters manual mode, reselects the
fan, and writes duty. Reselection accounts for a mode
change resetting the selection.

Register writes include a 20 ms sleeping gap. Status waits have a 100 ms bound;
mutations retry at most three times within a five-second operation budget. There is no post-write readback of duty or mode. Write success means the
protocol handshake completed, not that the EC applied the requested state. On a
transport failure, the driver attempts automatic mode with a separate two-second
budget and returns the original error. Recovery transport failures are logged; explicit sysfs reads still query
the hardware rather than a cached request. These are protocol budgets, not hard real
time guarantees: scheduling, mutex contention and a failing PM preparation that
also restores its snapshot can add elapsed time.

## hwmon interface

The device is identified by `name=asus_fan_ec`, not by a fixed `hwmonN` index.

| File | Access | Meaning |
| --- | --- | --- |
| `name` | Read only | `asus_fan_ec` |
| `pwm1` | Root write, public read | EC fan 0 duty, 0–255 |
| `pwm2` | Root write, public read | EC fan 1 duty, 0–255 |
| `pwm1_enable` | Root write, public read | Shared mode: 1 manual, 2 EC automatic |

The kernel also creates ordinary device infrastructure files. There is no
`pwm2_enable` or `control`. Mode 0 is intentionally rejected: the standard hwmon
meaning of enable 0 is full speed without speed control, not EC automatic mode.
The driver translates hwmon 2 into EC mode 0.

Writing either PWM enters **global manual mode** and adjusts only that channel.
The other fan freezes at its existing duty. Writing enable 1 alone freezes both
without setting new duties. Enable 2 changes only mode, leaving duty calculation
to the EC. Reads return hardware values, which can differ from requests due to
quantization and change while in automatic mode. Invalid ranges return EINVAL.
Channel numbers are not assumed to identify CPU/GPU fans.

After the assisted first load, discover the path:

```bash
HWMON=
for path in /sys/class/hwmon/hwmon*; do
    if [ "$(cat "$path/name" 2>/dev/null)" = asus_fan_ec ]; then
        HWMON=$path
        break
    fi
done
test -n "$HWMON" || { echo 'Driver not loaded'; exit 1; }
cat "$HWMON/pwm1_enable" "$HWMON/pwm1" "$HWMON/pwm2"
```

Example manual control, followed by release:

```bash
printf '128\n' | sudo tee "$HWMON/pwm1"
printf '192\n' | sudo tee "$HWMON/pwm2"
printf '2\n' | sudo tee "$HWMON/pwm1_enable"
```

These are separate sysfs operations, not an atomic two-fan update. Generic
applications that require `pwm2_enable`, RPM or temperature inputs may not work
with this deliberately minimal interface.

## Install on Ubuntu 26.04

Run from this checkout. Do not have another controller manage the fans during
the first load or tests. In particular, stop the reference program, fancontrol,
and any installed `asus-fan-supervisor` while validating. The installer does not
disable them or blacklist ASUS modules.

```bash
sudo apt update
sudo apt install build-essential dkms linux-headers-$(uname -r) mokutil shim-signed
make
make test
make check
sudo make install
dkms status -m asus-fan-ec-driver
```

`make check` needs `shellcheck` and the kernel headers' `scripts/checkpatch.pl`:
install ShellCheck with `sudo apt install shellcheck` if needed. Build warnings
about BTF/vmlinux or an equivalent compiler executable name are environmental;
investigate actual compiler diagnostics separately.

Installation copies the driver sources to
`/usr/src/asus-fan-ec-driver-0.1.3`, registers/builds/installs DKMS for the running
kernel, and creates `/etc/modules-load.d/asus-fan-ec-driver.conf`. **It does not
load the module or change a fan.** Autoload is active on the next boot, so arrange
controller coexistence and signing before that boot. The first explicit load,
after reviewing the hardware checklist and signing status, is:

```bash
sudo modprobe asus_fan_ec
```

Probe validates the EC and requests automatic mode before publishing hwmon. No
manual setting is restored across a fresh boot. Unsupported DMI or a busy port
fails loading; the driver never bypasses a resource reservation.

### Reinstallation and kernel updates

Repeating installation with identical source is supported. Different sources
under the same version are refused; maintainers must bump the version. The owned
source files have a checksum manifest. Version 0.1.3 uses `.ec-owned` and
`.ec-manifest` in its version-specific source directory. Earlier installations
are not migrated; remove version 0.1.1 using its original checkout and scripts.
Modified/unmanaged files are not silently overwritten or removed.

DKMS autoinstall rebuilds for future kernels through the distribution's kernel
installation hooks. Install headers for every target. To cover other kernels
already present, or diagnose a failed rebuild:

```bash
sudo dkms autoinstall -k 7.0.0-31-generic
sudo dkms autoinstall -k 7.0.0-34-generic
dkms status -m asus-fan-ec-driver
```

Inspect `/var/lib/dkms/asus-fan-ec-driver/<version>/` for build logs. Before
changing driver versions, release and unload the running module; replacing a
file on disk does not replace code already loaded in the kernel. Recheck signing
after upgrades. This out-of-tree driver does not promise compatibility with
future kernel APIs without a successful build and validation.

## Secure Boot

**DKMS normally signs this module automatically, but this project does not
guarantee signing on every system.** Current upstream DKMS signs the modules it
builds by default, even when Secure Boot is disabled. The installed DKMS version,
distribution configuration, signing tool and available key material determine
what actually happens; check the build output rather than assuming success.

`sudo make install` delegates the build/install to DKMS. Neither the installer
nor `dkms.conf` supplies a signing key, invokes `sign-file` itself or enrolls a
certificate. A plain `make` only compiles a local module and does not sign it.
DKMS rebuilds for new kernels normally sign each new module with the configured
key; an already enrolled certificate can be reused while that key is unchanged.

Signing and trust are separate steps: DKMS uses the private key to sign the
module; the matching public certificate must be enrolled through MOK for the
Ubuntu kernel to accept it under Secure Boot. Key generation or successful DKMS
installation does not establish enrollment. `modinfo -F signer` shows signature
metadata, not proof that the running kernel trusts or can load the module.

Use the Ubuntu shim/MOK trust path and native DKMS signing. No disabling Secure
Boot, shim validation, lockdown or `iomem` restrictions is required. Enrollment
requires interaction in MokManager at reboot; the project cannot perform that
firmware confirmation for you.

1. Check `mokutil --sb-state`. Inspect the DKMS build/install output: it identifies
   the signing key and public certificate actually used. Confirm the installed
   module with `modinfo -F signer asus_fan_ec`.
2. Reuse the configured DKMS key. Typical Ubuntu installations use
   `/var/lib/shim-signed/mok/MOK.priv` and `MOK.der`; upstream DKMS defaults may use
   `/var/lib/dkms/mok.key` and `mok.pub`. Inspect `/etc/dkms/framework.conf` and
   its configuration fragments, and trust the signing output rather than guessing.
3. Check and, if necessary, import **that public certificate**:

   ```bash
   sudo mokutil --test-key /actual/path/to/certificate.der
   sudo mokutil --import /actual/path/to/certificate.der
   ```

   Replace the placeholder with the certificate DKMS reported (`mok.pub` may
   also be a DER certificate). Choose an enrollment password, reboot, select
   **Enroll MOK**, inspect/confirm the certificate, enter the password and reboot.
4. Enable Secure Boot in firmware if currently disabled, keeping the Ubuntu
   signed shim boot entry. After boot, confirm:

   ```bash
   mokutil --sb-state
   sudo mokutil --test-key /actual/path/to/certificate.der
   modinfo -F signer asus_fan_ec
   sudo modprobe asus_fan_ec
   lsmod | rg asus_fan_ec
   sudo journalctl -k -b --no-pager
   ```

   `lsmod | grep asus_fan_ec` also works if ripgrep is not installed.
5. Test hwmon operations and a DKMS rebuild for another kernel with Secure Boot
   still enabled. A signer string alone is not proof of trusted loading.

Current DKMS generates/signs with a configured key; Ubuntu may integrate key
generation with `update-secureboot-policy`. If signing is missing, inspect the
installed DKMS configuration and logs; `sudo dkms generate_mok` can generate its
configured key material, then rebuild/install the module and enroll the matching
certificate. Do not replace or delete existing signing keys casually: other
drivers, including NVIDIA, may use them. Private keys never belong in this repo.

If loading fails with `Key was rejected by service`, check certificate enrollment,
the running kernel's module path (`modinfo -n asus_fan_ec`), and its signature.
If loading reports protocol/resource errors instead, inspect the kernel log and
follow the hardware checklist. Do not remove resource owners to force access.

Sources: [Ubuntu Secure Boot documentation](https://documentation.ubuntu.com/security/security-features/platform-protections/secure-boot/)
and [DKMS module signing](https://github.com/dkms-project/dkms#module-signing).

## Suspend, resume and shutdown

During PM `prepare`, the driver blocks hwmon access, snapshots mode and both
hardware duties when manual, and requests EC automatic mode. Failed snapshots
abort sleep without changing the cooling mode. If switching to automatic fails,
the driver attempts to restore the snapshot and aborts sleep.

PM `complete` restores the snapshot after resume, thaw or hibernation restore,
and also after a cancelled sleep transition. Automatic snapshots restore
automatic mode; manual snapshots restore both saved duties, including zero.
Restoration is idempotent. If restoration fails, the driver attempts automatic
mode, logs the failure and discards the snapshot; subsequent reads report actual
hardware state. No timer retries or cached success are exposed.

Access during sleep returns EBUSY; access during teardown returns ENODEV.
Shutdown and unload attempt automatic mode before releasing resources. A failed
EC, kernel panic or abrupt power loss can prevent recovery. Kernel remove and PM
complete callbacks cannot promise to stop removal/resume on an error; inspect
logs. No watchdog catches a controlling process that crashes while manual.

## Uninstall

Stop userspace fan controllers first, then from the checkout:

```bash
sudo make uninstall
```

If loaded, the script writes enable 2 and unloads the
module before removing DKMS registrations for version 0.1.3 on all kernels. It
stops on communication or unload failure. It removes owned source/autoload files;
unknown files are preserved. An autoload file remains if another version is
installed. Repeated removal is supported. MOK certificates/private keys,
dependencies and unrelated ASUS drivers are preserved.

### Manual uninstall (without the original checkout)

Use the following steps for version **0.1.3**. Inspect
`dkms status -m asus-fan-ec-driver` first and substitute the installed version if
different. Stop userspace fan controllers before proceeding. Execute each step
only after the previous one succeeds; stop and inspect errors on a failed
release or unload. These commands are owner-assisted hardware operations when
the module is loaded.

1. If `/sys/module/asus_fan_ec` exists, find its hwmon device, request automatic
   mode and unload. Run this block in Bash; it skips hardware access if unloaded
   and refuses a missing or ambiguous hwmon device:

   ```bash
   (
       set -euo pipefail
       if [ -d /sys/module/asus_fan_ec ]; then
           HWMON=
           for path in /sys/class/hwmon/hwmon*; do
               [ "$(cat "$path/name" 2>/dev/null)" = asus_fan_ec ] || continue
               test -z "$HWMON" || { echo 'Multiple matching hwmon devices'; exit 1; }
               HWMON=$path
           done
           test -n "$HWMON" || { echo 'Loaded module has no accessible hwmon'; exit 1; }
           sudo sh -c 'printf "2\n" > "$1/pwm1_enable"' sh "$HWMON"
           sudo modprobe -r asus_fan_ec
       fi
       test ! -d /sys/module/asus_fan_ec
   )
   ```

   Release success means transport completion, without post-write confirmation.
   Inspect `sudo journalctl -k -b --no-pager` for removal/recovery errors.

2. Remove this version from DKMS for every kernel:

   ```bash
   sudo dkms remove -m asus-fan-ec-driver -v 0.1.3 --all
   dkms status -m asus-fan-ec-driver
   ```

   DKMS manages the installed module files and module dependency updates. This
   command does not remove `/usr/src/asus-fan-ec-driver-0.1.3` or the project's
   autoload file. If this version is already absent from DKMS, skip the remove
   command. DKMS may restore another installed version; retain the autoload file
   if another version remains installed.

3. If no other version remains installed, inspect the autoload file:

   ```bash
   cat /etc/modules-load.d/asus-fan-ec-driver.conf
   ```

   Only if its content is the project's single `asus_fan_ec` entry, remove it:

   ```bash
   sudo rm -- /etc/modules-load.d/asus-fan-ec-driver.conf
   ```

   Skip an absent file. Preserve a customized file for manual review.

4. Remove only the owned source files. This Bash block checks the ownership
   marker and checksums first. If sources were modified or the marker/manifest
   is missing, preserve them and investigate before removing anything:

   ```bash
   (
       set -euo pipefail
       cd /usr/src/asus-fan-ec-driver-0.1.3
       test "$(cat .ec-owned)" = 'asus-fan-ec-driver 0.1.3'
       sha256sum --check .ec-manifest
       sudo rm -- Kbuild Makefile dkms.conf LICENSE COPYING \
           src/asus_fan_ec.c src/ec_core.c src/ec_core.h .ec-manifest .ec-owned
       sudo rmdir -- src
       sudo rmdir -- /usr/src/asus-fan-ec-driver-0.1.3
   )
   ```

   Skip this step if the source directory is absent. `rmdir` refuses nonempty
   directories, preserving extra files for inspection; do not replace it with
   recursive deletion.

Finally, check `dkms status -m asus-fan-ec-driver` and confirm
`/sys/module/asus_fan_ec` is absent. Keep MOK certificates, private keys and
dependencies: other modules may rely on them. Do not delete a shared MOK or
remove unrelated ASUS drivers as part of uninstalling this driver.

## Tests and maintenance

`make test` compiles the production protocol core against a simulated I/O backend
with AddressSanitizer/UndefinedBehaviorSanitizer and runs lifecycle scripts with
fake DKMS in temporary staging trees. No EC ports are accessed. It covers protocol
encoding, reselecting channels, values, quantization, failures, recovery, snapshots,
cancelled sleep and callers serialized by a pthread mutex. Kernel integration,
real DKMS hooks, actual mutex/PM scheduling and trusted module loading require
the assisted tests. LeakSanitizer may require execution outside a ptrace sandbox.

See [decisions](docs/decisions.md), [hardware checklist](docs/hardware-validation.md)
and [maintenance instructions](AGENTS.md). Report board/BIOS, kernel, Secure Boot
state, exact operation/error and relevant kernel log; never publish private keys.

## Credits and license

Thank you to **Keyitdev** for publishing
[asus-fan-control-ec](https://github.com/Keyitdev/asus-fan-control-ec) and its
[reverse-engineering findings](https://github.com/Keyitdev/asus-fan-control-ec/blob/c3a8ba77bcc27e645946b770a56ee15abbbdfcee/HOW-IT-WORKS.md).
Its documented investigation of the proprietary ASUS Windows path
(`AsusWinIO64.dll` / `AsusSAIO.sys`) made this implementation possible. A connection
to GHelper's manual Windows fan control is plausible but not verified here.

This is an independent implementation of the documented protocol, not a port of
the reference's GPLv3+ source. No proprietary code is included. Licensed under
**GPL-2.0-or-later**; see [LICENSE](LICENSE) and [COPYING](COPYING). The project is
independent of ASUS and comes without warranty.
