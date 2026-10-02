# Assisted hardware validation

This checklist is executed with the laptop owner observing the fans. It is not
run by `make test`. Stop if mode/readback or channel selection is wrong. Never
use this driver to discover undocumented registers or bypass reserved ports.

## Preparation

1. Record `uname -r`, OS, DMI board/BIOS, `mokutil --sb-state`, source revision
   and module version. Check that expected board/BIOS match the decision record.
2. Keep the laptop idle on a suitable surface. Use the existing `coretemp` and
   ASUS monitoring interfaces if available; their values are not provided by
   this driver. Do not start thermal stress tests for protocol validation.
3. Inspect active controllers (`asus-fan-supervisor`, reference tool, fancontrol,
   asusd or custom services) and stop those actually present. Preserve their
   configuration so they can be restarted after unloading this module.
4. Release any existing manual mode through the existing controller before
   stopping it. Verify the fans remain under EC control.
5. Build/run automated tests. Install DKMS dependencies, review MOK/signing
   configuration, then run `sudo make install`. Installation does not load.
6. Keep a terminal ready to release with `pwm1_enable=2`. If communication fails,
   stop the test and inspect logs; do not assume unload guarantees recovery.

## First load and protocol gate

1. Run `sudo modprobe asus_fan_ec`; inspect `sudo journalctl -k -b`.
   Port/DMI/protocol errors are failures, not reasons to force loading.
2. Discover hwmon by `name=asus_fan_ec` as shown in README. Check only `pwm1`,
   `pwm2`, `pwm1_enable`, `name` and ordinary kernel infrastructure are present.
   Check root-only write permissions.
3. Read enable and both duties. Expected enable is 2. Duty values must be
   plausible integers in 0–255; successful count/mode validation alone is not
   enough to prove channel routing.
4. Write duty 128 to pwm1, then 192 to pwm2. Confirm mode 1, readbacks (record any EC quantization), and physical response of both fans. Record which
   physical fan corresponds to each channel without assuming CPU/GPU labels.
5. While manual, change only pwm1 and confirm pwm2's duty is preserved. Invert
   the values and confirm channel routing again.
6. Write enable 2 and verify it reads 2. Do not require automatic duty to retain
   the prior manual value: the EC owns its calculation again.

If either selected duty cannot be read/controlled independently, release and
stop. Do not publish this driver as validated or emulate successful reads from
the last request. Record the discrepancy for protocol investigation.

Invalid text, -1, 256 and enable 0/3 must fail without cooling writes. Duty zero
and 255 are already covered in simulation; a physical zero-duty test is optional,
brief, idle-only and explicitly supervised, with immediate release afterwards.

## Suspend and cancellation

- Start automatic, suspend and resume: confirm automatic and no error logs.
- Start manual with distinct duties, record actual readbacks, suspend and resume:
  confirm manual and both saved actual duties, recording any quantization.
- Repeat several cycles. Confirm userspace does not override the restored state.
- An optional supervised zero-duty cycle verifies restoration of zero; it is
  not necessary for the initial conservative smoke test.
- A cancelled PM transition is covered by simulation. For a physical cancellation,
  use an existing approved test setup; do not introduce a failing device module
  or system-wide debug settings just to provoke an abort.
- Hibernation callbacks are implemented and simulated. Only test physically if
  this laptop already has a supported hibernation setup; do not reconfigure swap
  or enable hibernation as part of driver validation. Record it as untested otherwise.

## Unload, reinstall and kernel targets

- Return automatic, unload with `sudo modprobe -r asus_fan_ec`, check hwmon
  disappears and fans remain EC-managed. Kernel removal failures are logged.
- Repeat loaded/unloaded installation lifecycle; `sudo make uninstall` must
  request automatic mode before removing DKMS files. A load/signature error must not lead
  to deleting keys or unrelated drivers.
- Confirm same-source installation is repeatable. Never overwrite modified
  source with the same version.
- Use DKMS autoinstall for each target with headers. Boot each target and repeat
  the conservative smoke test. Build success is not load success.
- Test autoload on reboot and confirm it starts automatic rather than restoring
  the previous boot's manual duty.

## Secure Boot acceptance

Follow README's enrollment procedure using the certificate actually reported by
DKMS. Firmware changes, MokManager confirmation and reboot are owner actions.

After boot, require all of:

- `mokutil --sb-state` reports enabled.
- The module's signing certificate is enrolled; `modinfo` reports a signer.
- The intended module loads under the running signed Ubuntu kernel.
- Minimal hwmon operations succeed without key rejection or lockdown errors.
- A second kernel's DKMS build/signature and trusted load succeed.

Record the signer/fingerprint (public information), kernel and results. Never
record private keys or enrollment passwords. If any step is pending, leave
Secure Boot marked unvalidated.

## Finish

Always release to automatic. Unload this module before restarting the old fan
controller. Update docs/decisions.md and the README hardware table with measured
evidence, exact board/BIOS/kernel and remaining untested cases.
