# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Verify the deployed application, including actual dependency loading."""
import argparse
import json
import os
import plistlib
import re
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
    helper = app / "Contents/Resources/native-keyboard/deskflow-hid-bridge"
    if not helper.exists():
        raise RuntimeError("Native keyboard bridge is missing")
    helper_libraries = run(["otool", "-L", str(helper)])
    for line in helper_libraries.splitlines()[1:]:
        dependency = line.strip().split(" (", 1)[0]
        if not dependency.startswith(("/System/Library/", "/usr/lib/")):
            raise RuntimeError(f"Privileged bridge depends on a non-system library: {dependency}")
    helper_signature = run(["codesign", "-dv", "--verbose=4", str(helper)])
    helper_cdhash = re.search(r"^CDHash=(\w+)$", helper_signature, re.MULTILINE).group(1)
    run([str(helper), "--self-test"])
    run([str(helper), "--identity-self-test", helper_cdhash])
    wrong_identity = subprocess.run([str(helper), "--identity-self-test", "0" * 40],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
    if wrong_identity.returncode == 0:
        raise RuntimeError("Native bridge accepted an incorrect running-code identity")
    core_signature = run(["codesign", "-dv", "--verbose=4", str(app / "Contents/MacOS/deskflow-core")])
    signature_flags = re.search(r"flags=0x([0-9a-fA-F]+)", core_signature)
    if not signature_flags or not (int(signature_flags.group(1), 16) & 0x10000):
        raise RuntimeError("The native input core must use hardened runtime")
    if not (app / "Contents/Resources/Deskflow-NativeKeyboard.pkg").is_file():
        raise RuntimeError("Offline native keyboard component installer is missing")
    report["native_keyboard"] = {"bridge_self_test": "passed", "kernel_identity": "verified",
                                  "core_hardened_runtime": True, "system_libraries_only": True,
                                  "system_extension_activated": False}
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
            load_commands = run(["otool", "-l", str(binary)])
            for rpath in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", load_commands):
                if rpath.startswith(("/Users/", "/opt/", "/usr/local/")):
                    raise RuntimeError(f"Build-machine runtime search path leaked into {name}: {rpath}")
            version = run([str(binary), "--version"], env=environment)
            if expected_version not in version:
                raise RuntimeError(f"Unexpected version from {name}: {version}")
            report["executables"][name] = {"launched": True,
                "architecture": run(["lipo", "-archs", str(binary)]).strip(),
                "version_output": version.strip()}
        environment["QT_PLUGIN_PATH"] = temporary + "/untrusted-plugins"
        environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = temporary + "/untrusted-platforms"
        environment["QT_QPA_PLATFORM"] = "untrusted-platform-must-not-load"
        isolated = run([str(app / "Contents/MacOS/deskflow-core"), "--version"], env=environment)
        if expected_version not in isolated:
            raise RuntimeError("The hardened core failed its plugin-environment isolation check")
        report["native_keyboard"]["plugin_environment_isolated"] = True
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("Deployed application signature and clean runtime loading verified.")


if __name__ == "__main__":
    main()
