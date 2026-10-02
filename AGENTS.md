# Maintenance instructions

Read README.md, docs/decisions.md and docs/hardware-validation.md before changing
the EC protocol, hwmon ABI, power management or installation lifecycle.

- Preserve the agreed minimal ABI: pwm1, pwm2, global pwm1_enable (1/2).
- Do not copy GPLv3+ reference code into this independently written driver.
- Never introduce arbitrary EC commands, register/port overrides or a force flag.
- Do not add post-write confirmation; setters report transport completion only.
  Preserve explicit reads and pre-sleep snapshots.
- Keep one device mutex around whole operations, including PM and removal.
- Hardware writes, loading, sleep and Secure Boot tests are assisted by the owner.
  Do not run them merely because a build or simulation passes.
- Update docs/decisions.md when behavior, assumptions or validation evidence changes.
- Distinguish simulated behavior, compilation and actual laptop validation.
- Bump the version in dkms.conf, both lifecycle scripts and MODULE_VERSION when
  changing installed driver sources. Installation refuses same-version collisions.
- Run make test, make check, and W=1 builds for both documented kernel targets.
- Do not disable other ASUS drivers or change the neighboring provisioning repo.

The --root lifecycle option is for staging tests; tests substitute a fake DKMS.
It is not a complete sandbox for a real DKMS process or key management.
