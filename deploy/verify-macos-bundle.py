# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Verify the deployed application, including actual dependency loading."""
import argparse
import json
import os
import plistlib
import subprocess
import tempfile
from pathlib import Path


def run(arguments, **kwargs):
    result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=30, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{arguments[0]} failed ({result.returncode}):\n{result.stdout}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app")
    parser.add_argument("--find-in")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.app:
        app = Path(args.app).resolve()
    else:
        candidates = list(Path(args.find_in).rglob("Deskflow.app"))
        if len(candidates) != 1:
            raise RuntimeError(f"Expected one staged application, found {len(candidates)}")
        app = candidates[0].resolve()
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    minimum = info.get("LSMinimumSystemVersion")
    expected_version = ".".join(info["CFBundleShortVersionString"].split(".")[:3])
    if not minimum:
        raise RuntimeError("The bundle must declare its minimum supported macOS version")
    run(["codesign", "--verify", "--deep", "--strict", str(app)])
    report = {"bundle_version": info["CFBundleShortVersionString"],
              "minimum_macos": minimum, "signature": "verified", "executables": {}}
    with tempfile.TemporaryDirectory(prefix="deskflow-deployed-runtime-") as temporary:
        environment = os.environ.copy()
        for key in list(environment):
            if key.startswith("DYLD_") or key in (
                    "QT_PLUGIN_PATH", "QML2_IMPORT_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                    "QT_ROOT_DIR", "QTDIR"):
                environment.pop(key, None)
        environment["XDG_CONFIG_HOME"] = temporary + "/config"
        environment["XDG_STATE_HOME"] = temporary + "/state"
        for name in ("Deskflow", "deskflow-core"):
            binary = app / "Contents/MacOS" / name
            libraries = run(["otool", "-L", str(binary)])
            for line in libraries.splitlines()[1:]:
                if "/Users/" in line or "/opt/homebrew/" in line:
                    raise RuntimeError(f"Build-machine dependency leaked into {name}: {line}")
            version = run([str(binary), "--version"], env=environment)
            if expected_version not in version:
                raise RuntimeError(f"Unexpected version from {name}: {version}")
            report["executables"][name] = {"launched": True,
                "architecture": run(["lipo", "-archs", str(binary)]).strip(),
                "version_output": version.strip()}
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Deployed application signature and clean runtime loading verified.")


if __name__ == "__main__":
    main()
