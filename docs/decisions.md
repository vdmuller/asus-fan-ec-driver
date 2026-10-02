# Decisions and maintenance memory

## Context and evidence (2026-10-01)

The owner requested a minimal Linux driver for an ASUS TUF F16 with an Intel
i7-14650HX, distributed as source using DKMS on Ubuntu 26.04, with Secure Boot
support and a comprehensive English README. The project directory was initially
empty. No neighboring project is part of this implementation.

Local read-only inspection recorded:

- DMI vendor: `ASUSTeK COMPUTER INC.`
- Product: `ASUS TUF Gaming F16 FX608JHR_FX608JHR`
- Board: `FX608JHR`; BIOS: `FX608JHR.309`
- OS: Ubuntu 26.04.1 LTS; running kernel: `7.0.0-34-generic`
- Headers also available for `7.0.0-31-generic`
- `mokutil --sb-state`: SecureBoot disabled; lockdown: none
- Existing hwmon devices include `asus`, `asus_custom_fan_curve` and `coretemp`.
- DKMS is not currently installed in the inspected host environment.

The owner reports that the reference's combined fan-duty/manual-mode operation
works. RPM/temperature telemetry does not work there and is explicitly out of
scope. This is user-reported evidence, not a test of this module. Independent
channel selection and mode/duty reads have not yet been physically confirmed.

The neighboring provisioning checkout includes an `asus-fan-supervisor` using
`asus-fan-control-ec`. Its active service state could not be inspected through
the sandbox system bus. Do not infer it is stopped. It must be checked during
assisted tests; no changes to it or existing ASUS modules are authorized here.

## Reference and licensing

Protocol source: [Keyitdev/asus-fan-control-ec](https://github.com/Keyitdev/asus-fan-control-ec).
Pinned reference revision: **c3a8ba77bcc27e645946b770a56ee15abbbdfcee**, obtained
with `git ls-remote` for master on 2026-10-01. The README, HOW-IT-WORKS.md and
transport/fan source were inspected through GitHub during planning. Preserve this
revision when comparing future upstream changes.

This driver was written independently from the documented protocol. The owner
chose GPL-2.0-or-later; no GPLv3+ source from the reference was copied. Keep the
reference credits and distinguish its documented ASUS Windows reverse engineering
from the unverified hypothesis about GHelper.

## Agreed behavior

| Decision | Rationale / constraint |
| --- | --- |
| `pwm1` and `pwm2`, each 0–255 | Reference supports independent selection; local validation is pending. |
| Only `pwm1_enable`, controlling both fans | EC mode is global; owner selected one mode attribute. |
| Enable 1 manual, 2 automatic; reject 0 | Owner initially suggested 0 automatic, then accepted standard hwmon semantics: 0 means full speed without speed control. Translate hwmon 2 to EC 0. |
| Writing duty automatically enters manual | Explicit owner choice; document that the other fan freezes. |
| Allow duty zero | Explicit owner choice; zero is not automatic. |
| No watchdog or thermal policy | Manual continues until explicitly changed; kernel only supplies controls. |
| Boot in automatic | No persistence of manual duties across fresh boots. |
| Restore mode and both hardware duties after sleep | Owner revised the original automatic-only resume policy. Include cancellation and hibernation paths. |
| Accept ASUS boards prefixed FX608 | Owner requested the family generally, rather than FX608JHR-only. Accepted does not mean physically validated. |
| Source + DKMS, no .deb | Owner preference; Ubuntu native signing/key trust integration. |
| Assisted physical tests | Do not load, change cooling, reboot or alter firmware without owner participation. |
| English artifacts | README, comments and maintenance records in English; discussion can be Portuguese. |

## Implementation invariants

- Only ports 0x25c/0x25d, table-2 registers 0x30/0x31/0x32/0x35; no raw API.
- A real kernel mutex protects complete operations, PM state and teardown. It
  does not lock external tools or firmware. hwmon core locking alone does not
  coordinate PM callbacks.
- DMI is checked before port access. Ports are reserved; no bypass on conflict.
- Read values come from the EC. No fake fallback when readback is unavailable.
- Count must equal two and mode must be recognized before modifying cooling.
- Reselect after mode writes; mode may reset the selector.
- Sleep after register writes for 20 ms; status waits at most 100 ms; at most
  three operation attempts under a five-second budget. Recovery has two seconds.
- As of 0.1.1, no post-write mode/duty confirmation or tolerance checks. Success
  means transport completion only; silent ignored writes are not detected. Explicit
  sysfs reads and pre-sleep hardware snapshots remain.
- PM prepare blocks access and snapshots before requesting release to EC. PM complete
  restores after all resume variants, including aborts before suspend. A transport-failed
  prepare restores internally because that device will not receive complete.
- Failed restore tries automatic, invalidates the snapshot and reports an error.
  complete has no return value, so recovery failures must be visible in logs.
- After remove starts, callers see ENODEV; suspended callers see EBUSY.
- Install never loads the module; the first load remains assisted. The installed
  autoload file takes effect at the next boot.
- Scripts refuse same-version source collisions and preserve keys, unrelated
  files and other driver versions. `--root` is a staging aid, not a security
  sandbox for real DKMS signing/hooks; tests use fake DKMS.

## Validation ledger

| Check | Result |
| --- | --- |
| Production protocol core with fake EC | Passed: packet encoding, independent writes, selector reset, 0/255, invalid numeric values, hardware readback and quantization. |
| Fault injection | Passed: busy IBF, missing OBF, stale output, wrong fan count/mode, transient retry, three transport-failed write attempts, failed recovery, undetected silent ignored writes and truthful subsequent reads. |
| Power management simulation | Passed: auto/manual/zero snapshots, complete-only cancellation, snapshot failure, failed release rollback, failed second-channel restoration and idempotence. |
| Concurrency simulation | Passed with callers sharing a pthread mutex; production kernel mutex integration still needs physical/runtime checks. |
| ASan/UBSan | Passed outside the sandbox; LeakSanitizer initially failed under sandbox ptrace. No driver hardware was accessed. |
| Lifecycle scripts | Passed with fake DKMS and temporary staging roots: repeated install/remove, source collisions, install/remove failures, another version retained. |
| W=1 builds: 7.0.0-31 and 7.0.0-34 generic | Passed. Environment notices: identical compiler version under different executable names; unavailable pahole/vmlinux prevents BTF generation. |
| Bash syntax, ShellCheck, strict checkpatch | Passed with no findings. |
| Real DKMS install/signing and kernel update hooks | Pending; fake DKMS is not equivalent. |
| Module load and real fan readback/selection | Passed initial owner-assisted 0.1.3 smoke test on FX608JHR / 7.0.0-34: physical fan response, manual 128/192 reads, second-channel readback preservation and automatic release. Physical channel mapping remains pending. |
| Real suspend/resume, hibernation and unload | One automatic and one manual suspend/resume cycle and unload passed on 7.0.0-34 with Secure Boot enabled. Repeated cycles and hibernation remain unvalidated. |
| Secure Boot enabled + enrolled certificate + trusted module load | Passed enrollment and autoload on 7.0.0-34; owner reports manual control/release passed. Second-kernel trusted load remains pending. |

Initial owner-assisted hardware smoke tests passed on the recorded FX608JHR;
full hardware validation remains incomplete. If readback or independent selection fails, stop cooling tests and
investigate the protocol before claiming the ABI works on this machine.

## Maintenance history

- 2026-10-01: Initial implementation and documentation. Recorded the final
  hwmon 1/2 mode decision and the revision to restore pre-sleep state. No module
  was installed/loaded and no EC ports or firmware settings were changed.

Future records must include date, source version, board/BIOS/kernel, signing state,
test evidence, changed behavior and remaining uncertainty. Update this file and
the README together when validation status changes.

- 2026-10-01, 0.1.1: Owner requested removal of post-write confirmation. Removed
  duty/mode readback, the extra selector write and tolerance-based verification
  from all setters (including PM/probe/recovery), plus uninstall post-write
  readback. Kept preflight count/mode validation, explicit sysfs reads, snapshots,
  handshake waits and recovery on transport errors. Tests assert no post-write
  transactions and demonstrate silent ignored writes are accepted. A normal duty
  attempt now has four 20 ms write gaps instead of five. No physical tests run.

- 2026-10-01, 0.1.2: Adopted `EC_` constants and `ec_` functions/types throughout
  the independent implementation and tests. Port names end in `_PORT`, register
  names in `_REG`; named constants identify buffer status bits, table-2 command
  bytes, EC modes, fan count, polling interval, deadlines and retry limits. The
  neighboring driver2 was inspected for naming clarity only; no implementation
  was copied. Protocol bytes, timing values, transaction order, hwmon ABI, device
  mutex coverage, recovery and PM behavior remain unchanged. Setters still report
  transport completion without post-write confirmation.
  Version 0.1.2 uses `.ec-owned`/`.ec-manifest` in its own source directory; older
  installations are not migrated and require their original lifecycle scripts.
  README examples, DKMS, both lifecycle scripts and MODULE_VERSION now use 0.1.2.
  `make test` passed with ASan/UBSan outside the ptrace sandbox, including numeric
  read/write packet expectations, faults, snapshots, cancellation, concurrent
  callers and staged lifecycle tests with fake DKMS. The initial sandbox run
  failed because LeakSanitizer cannot run under ptrace. `make check` passed with
  no findings. W=1 builds passed for 7.0.0-31-generic and 7.0.0-34-generic; only
  environmental compiler-name/pahole notices and skipped BTF due to missing
  vmlinux were reported. No legacy naming remains in maintained source, scripts
  or tests. Laptop context remains FX608JHR / BIOS FX608JHR.309; Secure Boot was
  not rechecked. No real installation, module load, EC access, sleep or signing
  test was performed; physical validation remains pending.

- 2026-10-02, 0.1.3: Renamed `EC_TABLE2_CMD`, `EC_TABLE2_READ` and
  `EC_TABLE2_WRITE` to `EC_CMD`, `EC_READ` and `EC_WRITE` at the owner's request.
  Values and protocol behavior remain unchanged. Bumped DKMS, both lifecycle
  scripts, MODULE_VERSION, README examples and staged lifecycle expectations.
  `make test` passed with ASan/UBSan outside the ptrace sandbox after the initial
  sandbox run hit LeakSanitizer's ptrace limitation. `make check` passed with no
  findings. W=1 builds passed for 7.0.0-31-generic and 7.0.0-34-generic, with only
  environmental compiler-name/pahole notices and skipped BTF due to missing
  vmlinux. Recorded laptop context remains FX608JHR / BIOS FX608JHR.309; board,
  BIOS and Secure Boot state were not rechecked. No real installation, module
  load, EC access, sleep or signing test was performed; hardware validation
  remains pending.

- 2026-10-02, 0.1.3 documentation: Added a manual uninstall procedure without
  the original checkout: assisted automatic-mode release/unload, all-kernel DKMS
  removal, conditional autoload cleanup and checksum/ownership-checked deletion
  of known source files while preserving extra files and shared MOK material.
  Clarified that current upstream DKMS signs builds by default, subject to the
  installed version/configuration and signing prerequisites; the installer
  delegates to DKMS and plain `make` does not sign. Certificate enrollment is a
  separate owner action and signature metadata does not prove trusted loading.
  Checked the linked upstream DKMS and Ubuntu Secure Boot documentation.
  Driver sources, lifecycle scripts and version were not changed.
  `make test` passed with ASan/UBSan outside the sandbox after the known
  LeakSanitizer/ptrace failure; lifecycle tests used fake DKMS. `make check` and
  W=1 builds for 7.0.0-31-generic and 7.0.0-34-generic passed, with the existing
  environmental compiler-name/pahole notices and skipped BTF generation.
  Recorded laptop context remains FX608JHR / BIOS FX608JHR.309; board, BIOS and
  Secure Boot state were not rechecked. Manual uninstall and real DKMS signing
  were documented, not executed; physical validation remains pending.

- 2026-10-02, 0.1.3 assisted smoke test: FX608JHR / BIOS FX608JHR.309,
  Ubuntu 26.04.1, kernel 7.0.0-34-generic, Secure Boot disabled. Read-only
  inspection confirmed real DKMS installation for the running kernel, version
  0.1.3 and signer metadata `ubuntu Secure Boot Module Signature key`. The owner
  loaded the module and supplied the kernel ready-in-automatic log; a signature
  verification warning tainted the kernel but did not prevent loading. No trusted
  Secure Boot loading is established. Initial explicit reads were mode 2 and
  duties 0/0; zero automatic readbacks do not establish stopped fans. The owner
  supplied subsequent explicit reads of manual mode 1 and duties 128/192 and
  reported immediate physical fan response when the values were written. An
  explicit read after automatic release returned mode 2. Setter behavior remains
  transport-only; these were separately requested reads, not driver confirmation.
  Independent channel preservation, physical channel mapping, PM, unload, second
  kernel loading and Secure Boot acceptance remain untested. Source and installed
  version are unchanged.

- 2026-10-02, 0.1.3 follow-up: Owner supplied explicit reads after changing
  only pwm1: mode 1, pwm1 160, pwm2 192, then mode 2 after release. This
  establishes second-channel preservation in readback; isolated physical
  response was not separately confirmed. After the owner enabled Secure Boot
  and rebooted, inspection confirmed SecureBoot enabled on 7.0.0-34-generic.
  Module autoload failed with `Key was rejected by service`; no asus_fan_ec
  module or hwmon device was present. The installed module remains version
  0.1.3 with signer `ubuntu Secure Boot Module Signature key`. The public
  certificate /var/lib/shim-signed/mok/MOK.der is not enrolled; its SHA1
  fingerprint is 8A:0A:07:27:E4:5D:C7:F7:AB:E5:3D:DC:55:50:B4:50:D7:93:C7:EA.
  asusd restarted on reboot and is active; it must be paused again before
  subsequent assisted fan tests. Certificate enrollment and trusted loading
  remain pending. No keys, firmware, service state or hardware were changed
  by these inspections.

- 2026-10-02, 0.1.3 Secure Boot loading: After owner-assisted MokManager
  enrollment and reboot, inspection confirmed SecureBoot enabled on
  7.0.0-34-generic, the same MOK.der reported already enrolled, module
  asus_fan_ec loaded automatically, and hwmon name asus_fan_ec present. Kernel
  logs report ready in automatic mode without a module signature verification
  failure. The ordinary out-of-tree module taint remains. The unprivileged
  mokutil query also reported inability to access the kernel trusted keyring;
  enrolled-certificate output plus actual Secure Boot loading establish this
  kernel's loading result. Hwmon operations under Secure Boot and second-kernel
  trusted loading remain pending, as do PM and unload. asusd is active after
  reboot and must be paused before assisted control tests.

- 2026-10-02, 0.1.3 Secure Boot hwmon smoke test: On the enrolled,
  Secure Boot enabled 7.0.0-34-generic boot, the owner supplied explicit initial
  reads mode 2, pwm1 0, pwm2 0. After receiving the assisted 128/192 manual
  write/read and automatic-release procedure, the owner reported that it worked.
  This is owner-reported completion; exact post-write readings and individual
  physical responses were not supplied for this repetition. Basic hwmon
  operation under Secure Boot is reported successful. PM, unload and trusted
  second-kernel loading remain pending. No sleep was initiated by the agent.

- 2026-10-02, 0.1.3 assisted suspend/resume: On FX608JHR / BIOS
  FX608JHR.309, kernel 7.0.0-34-generic with Secure Boot enabled, the owner
  supplied post-resume automatic-state reads mode 2, duties 0/0 and no
  asus_fan_ec errors in the filtered kernel log. A subsequent manual-state
  suspend/resume returned mode 1 and duties 128/192; explicit automatic release
  then read mode 2. The supplied filtered kernel log again contained only the
  out-of-tree notice and initial automatic-mode ready message. One automatic
  and one manual cycle are evidenced; repeated cycles, cancellation, hibernation,
  unload and second-kernel trusted loading remain unvalidated. No PM operations
  were initiated by the agent.

- 2026-10-02, 0.1.3 assisted unload: After explicit automatic-mode release,
  the owner ran modprobe -r asus_fan_ec successfully. Their checks reported
  module absent and no matching hwmon device, with no removal/recovery errors
  in the supplied filtered kernel log. Physical fan behavior after unload was
  not separately reported. Reload, repeated PM cycles, real reinstall/uninstall
  and second-kernel trusted loading remain pending.

- 2026-10-02, 0.1.3 assisted reload: The owner ran modprobe asus_fan_ec
  again in the same Secure Boot enabled 7.0.0-34-generic session, rediscovered
  hwmon by name and supplied explicit mode readback 2. Unload/reload therefore
  passed and fresh probe returned automatic mode. No manual duty was persisted
  across reload. Physical fan behavior after unload was not separately reported.

- 2026-10-02, 0.1.3 publication preparation: Re-ran `make test` with
  ASan/UBSan outside the ptrace sandbox after its known LeakSanitizer failure;
  protocol simulation and staged lifecycle tests with fake DKMS passed.
  `make check` passed with no findings. W=1 builds passed for
  7.0.0-31-generic and 7.0.0-34-generic, with the existing compiler-name/pahole
  notices and skipped BTF due to unavailable vmlinux. Prepared the initial Git
  history containing maintained sources, tests, licensing and documentation;
  generated build files are ignored. Driver sources and version are unchanged.
  No real installation, module load, EC access, sleep or signing test was run.
