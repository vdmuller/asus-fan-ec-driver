#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run real lifecycle scripts with isolated trees and a fake DKMS command.

This verifies packaging behavior, not a real DKMS build or module signature.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
MOCK = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
log = Path(os.environ['EC_TEST_LOG'])
with log.open('a') as f: f.write(json.dumps(args) + '\\n')
assert '--dkmstree' in args and '--sourcetree' in args and '--installtree' in args
tree = Path(args[args.index('--dkmstree') + 1])
state = tree / 'state.json'
data = json.loads(state.read_text()) if state.exists() else {}
action = next(x for x in args if x in ('status', 'add', 'install', 'remove'))
if os.environ.get('EC_TEST_FAIL') == action: sys.exit(1)
name = args[args.index('-m') + 1]
version = args[args.index('-v') + 1] if '-v' in args else None
key = name + '/' + version if version else None
if action == 'status':
    for k, status in data.items():
        if k == key or (key is None and k.startswith(name + '/')):
            print(k + ', test-kernel, x86_64: ' + status)
elif action == 'add': data[key] = 'added'
elif action == 'install': data[key] = 'installed'
elif action == 'remove': data.pop(key, None)
state.write_text(json.dumps(data))
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ec-install-") as directory:
        base = Path(directory)
        root = base / "root"
        mockbin = base / "bin"
        mockbin.mkdir()
        dkms = mockbin / "dkms"
        dkms.write_text(MOCK)
        dkms.chmod(0o755)
        log = base / "dkms.log"
        env = dict(os.environ, PATH=f"{mockbin}:{os.environ['PATH']}",
                   EC_TEST_LOG=str(log))

        def run(script, success=True, **extra):
            result = subprocess.run(
                ["bash", str(PROJECT / "scripts" / script), "--root", str(root)],
                env=dict(env, **extra), text=True, capture_output=True)
            assert (result.returncode == 0) == success, result.stdout + result.stderr
            return result

        dest = root / "usr/src/asus-fan-ec-driver-0.1.3"
        autoload = root / "etc/modules-load.d/asus-fan-ec-driver.conf"
        state = root / "var/lib/dkms/state.json"
        run("install.sh", False, EC_TEST_FAIL="install")
        assert dest.exists() and not autoload.exists()
        run("install.sh")
        assert autoload.read_text() == "asus_fan_ec\n"
        assert (dest / ".ec-owned").read_text() == "asus-fan-ec-driver 0.1.3\n"
        assert (dest / ".ec-manifest").is_file()
        contents = (dest / "src/ec_core.c").read_bytes()
        run("install.sh")
        assert (dest / "src/ec_core.c").read_bytes() == contents
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        assert sum("add" in call for call in calls) == 1
        (dest / "src/ec_core.c").write_bytes(contents + b"\n/* local edit */\n")
        run("install.sh", False)
        run("uninstall.sh", False)
        assert autoload.exists() and state.exists()
        (dest / "src/ec_core.c").write_bytes(contents)
        run("uninstall.sh", False, EC_TEST_FAIL="remove")
        assert dest.exists() and autoload.exists()
        run("uninstall.sh")
        assert not dest.exists() and not autoload.exists()
        run("uninstall.sh")
        run("install.sh")
        data = json.loads(state.read_text())
        data["asus-fan-ec-driver/0.2.0"] = "installed"
        state.write_text(json.dumps(data))
        run("uninstall.sh")
        assert autoload.exists() and not dest.exists()
        data = json.loads(state.read_text())
        assert data == {"asus-fan-ec-driver/0.2.0": "installed"}
        autoload.write_text("other_module\n")
        run("install.sh", False)
        print("PASS: staged install/reinstall, collisions, failures and removal")


if __name__ == "__main__":
    main()
