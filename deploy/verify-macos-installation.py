# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Exercise the offline installer on a disposable macOS CI runner, never a user Mac."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def run(*arguments, timeout=180):
    result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed ({result.returncode}):\n{result.stdout}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("Installation checks are restricted to disposable GitHub Actions runners")
    report = {"passed": False, "system_extension_activation_requested": False}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        before = run("/usr/bin/systemextensionsctl", "list")
        report["installer_output"] = run("sudo", "-n", "/usr/sbin/installer", "-pkg", str(Path(args.package).resolve()),
                                         "-target", "/", timeout=240)
        application = Path("/Applications/Deskflow.app")
        helper_root = Path("/Library/Application Support/Deskflow/NativeKeyboard")
        core = application / "Contents/MacOS/deskflow-core"
        manifest = dict(line.split("=", 1) for line in (helper_root / "allowed-binaries.conf").read_text().splitlines()
                        if "=" in line)
        if manifest.get("core") != hashlib.sha256(core.read_bytes()).hexdigest():
            raise RuntimeError("The installed native helper manifest does not pin the installed core")
        signature = run("/usr/bin/codesign", "-dv", "--verbose=4", str(core))
        if manifest.get("core-cdhash") != re.search(r"^CDHash=(\w+)$", signature, re.MULTILINE).group(1):
            raise RuntimeError("The installed core CodeDirectory identity does not match the helper")
        roots = [application, application / "Contents", application / "Contents/MacOS",
                 application / "Contents/Resources", helper_root.parent, helper_root]
        roots += list((application / "Contents/Frameworks").rglob("*"))
        roots += list((application / "Contents/PlugIns").rglob("*"))
        for path in roots:
            status = path.lstat()
            if status.st_uid != 0 or (not path.is_symlink() and status.st_mode & 0o022):
                raise RuntimeError(f"Installed native input dependency is mutable by a non-root user: {path}")
        service = ""
        for _ in range(30):
            service = run("sudo", "-n", "/bin/launchctl", "print", "system/org.deskflow.native-keyboard", timeout=10)
            if "state = running" in service:
                break
            time.sleep(0.2)
        if "state = running" not in service:
            raise RuntimeError("The installed native keyboard bridge did not start")
        report["native_helper_running"] = True
        report["root_owned_dependencies"] = True
        report["manifest_identity"] = "verified"
        validation = output.with_name("installed-macos-bundle-validation.json")
        run("python3", "deploy/verify-macos-bundle.py", "--app", str(application), "--output", str(validation))
        after = run("/usr/bin/systemextensionsctl", "list")
        if before != after:
            raise RuntimeError("The installer unexpectedly activated or changed a system extension")
        report["system_extension_state_unchanged"] = True
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        # No driver was activated. Stop our two CI-only jobs before runner disposal.
        for label in ("org.deskflow.native-keyboard", "org.deskflow.virtual-hid-service"):
            subprocess.run(["sudo", "-n", "/bin/launchctl", "bootout", "system/" + label],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)


if __name__ == "__main__":
    main()
