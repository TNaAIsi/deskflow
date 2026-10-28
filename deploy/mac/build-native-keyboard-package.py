# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Compose offline native-keyboard and full installers without activating a driver."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
from xml.sax.saxutils import escape

DRIVER_NAME = "Karabiner-DriverKit-VirtualHIDDevice-8.7.0.pkg"
DRIVER_SHA256 = "3364294fca2629d4d8968bbc434537f4093b8ba620a211ec192b3d2e71e42dfb"
HELPER_ROOT = "Library/Application Support/Deskflow/NativeKeyboard"
VENDOR_DAEMON = "/Library/Application Support/org.pqrs/Karabiner-DriverKit-VirtualHIDDevice/Applications/Karabiner-VirtualHIDDevice-Daemon.app/Contents/MacOS/Karabiner-VirtualHIDDevice-Daemon"


def run(*args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}):\n{result.stdout}")
    return result.stdout


def write(path, content, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755 if executable else 0o644)


def distribution(packages, output, title):
    choices = []
    references = []
    for index, (identifier, filename) in enumerate(packages):
        choice = f"component{index}"
        choices.append(f'<line choice="{choice}"/>')
        references.append(f'<choice id="{choice}" visible="false"><pkg-ref id="{identifier}"/></choice>'
                          f'<pkg-ref id="{identifier}">{escape(filename)}</pkg-ref>')
    write(output, '<?xml version="1.0" encoding="utf-8"?>\n'
          '<installer-gui-script minSpecVersion="2">'
          f'<title>{escape(title)}</title>'
          '<options customize="never" require-scripts="false" hostArchitectures="arm64"/>'
          '<domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>'
          '<choices-outline>' + ''.join(choices) + '</choices-outline>' + ''.join(references) +
          '</installer-gui-script>')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--output-directory", required=True)
    parser.add_argument("--identity", default="-")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        raise RuntimeError("Installer version must have three numeric components")
    bundle = Path(args.bundle).resolve()
    source = Path(args.source_root).resolve()
    output = Path(args.output_directory).resolve()
    output.mkdir(parents=True, exist_ok=True)
    vendor = source / "third_party/native-hid" / DRIVER_NAME
    if hashlib.sha256(vendor.read_bytes()).hexdigest() != DRIVER_SHA256:
        raise RuntimeError("The upstream virtual HID installer checksum is incorrect")
    signature = run("/usr/sbin/pkgutil", "--check-signature", str(vendor))
    if "G43BCU2T37" not in signature or "signed by a developer certificate issued by Apple" not in signature:
        raise RuntimeError("The upstream virtual HID installer has no expected Apple Developer ID signature")

    helper = bundle / "Contents/Resources/native-keyboard/deskflow-hid-bridge"
    if not helper.is_file():
        raise RuntimeError("The native keyboard bridge is missing from the staged application")
    sign_args = ["/usr/bin/codesign", "--force", "--sign", args.identity]
    sign_args += ["--options", "0"] if args.identity == "-" else ["--options", "runtime", "--timestamp"]
    helper_sign = ["/usr/bin/codesign", "--force", "--sign", args.identity, "--options", "runtime"]
    if args.identity != "-":
        helper_sign.append("--timestamp")
    run(*helper_sign, str(helper))
    core_sign = helper_sign + ["--entitlements", str(source / "deploy/mac/native-input.entitlements")]
    run(*core_sign, str(bundle / "Contents/MacOS/deskflow-core"))
    expected_core = hashlib.sha256((bundle / "Contents/MacOS/deskflow-core").read_bytes()).hexdigest()
    core_signature = run("/usr/bin/codesign", "-dv", "--verbose=4", str(bundle / "Contents/MacOS/deskflow-core"))
    expected_cdhash = next(line.split("=", 1)[1] for line in core_signature.splitlines() if line.startswith("CDHash="))
    helper_signature = run("/usr/bin/codesign", "-dv", "--verbose=4", str(helper))
    helper_cdhash = next(line.split("=", 1)[1] for line in helper_signature.splitlines() if line.startswith("CDHash="))
    run(str(helper), "--self-test")
    run(str(helper), "--identity-self-test", helper_cdhash)
    with tempfile.TemporaryDirectory(prefix="deskflow-native-packaging-") as temporary:
        stage = Path(temporary)
        payload = stage / "payload"
        library = payload / HELPER_ROOT
        library.mkdir(parents=True)
        shutil.copy2(helper, library / "deskflow-hid-bridge")
        shutil.copy2(source / "third_party/native-hid/README.md", library / "Third-party-notices.md")
        shutil.copytree(source / "LICENSES", library / "LICENSES")
        script = f'''#!/bin/sh
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
while /usr/bin/pgrep -u 0 -f '^{VENDOR_DAEMON}$' >/dev/null; do
  /bin/sleep 2
done
exec '{VENDOR_DAEMON}'
'''
        write(library / "start-virtual-hid-service", script, True)
        daemons = payload / "Library/LaunchDaemons"
        daemons.mkdir(parents=True)
        jobs = {
            "org.deskflow.native-keyboard": ["/" + HELPER_ROOT + "/deskflow-hid-bridge"],
            "org.deskflow.virtual-hid-service": ["/" + HELPER_ROOT + "/start-virtual-hid-service"],
        }
        for label, command in jobs.items():
            data = {"Label": label, "ProgramArguments": command, "RunAtLoad": True, "KeepAlive": True,
                    "ThrottleInterval": 10, "ProcessType": "Interactive", "WorkingDirectory": "/var/empty",
                    "StandardOutPath": "/var/log/deskflow-native-keyboard.log",
                    "StandardErrorPath": "/var/log/deskflow-native-keyboard.log"}
            (daemons / (label + ".plist")).write_bytes(plistlib.dumps(data))
        scripts = stage / "scripts"
        scripts.mkdir()
        labels = "org.deskflow.native-keyboard org.deskflow.virtual-hid-service"
        write(scripts / "preinstall", f'''#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
for directory in '/Library/Application Support/Deskflow' '/{HELPER_ROOT}'; do
  if [ -L "$directory" ] || {{ [ -e "$directory" ] && [ "$(/usr/bin/stat -f '%u' "$directory")" != "0" ]; }}; then
    echo "Native keyboard installation directory is not trusted: $directory" >&2
    exit 1
  fi
  if [ -e "$directory" ]; then
    permissions=$(/usr/bin/stat -f '%Lp' "$directory")
    if [ $((0$permissions & 022)) -ne 0 ]; then
      echo "Native keyboard installation directory is writable by another user: $directory" >&2
      exit 1
    fi
  fi
done
/bin/mkdir -p '/Library/Application Support/Deskflow' '/{HELPER_ROOT}'
/usr/sbin/chown root:wheel '/Library/Application Support/Deskflow'
/bin/chmod 755 '/Library/Application Support/Deskflow'
/usr/sbin/chown root:wheel '/{HELPER_ROOT}'
/bin/chmod 755 '/{HELPER_ROOT}'
for label in {labels}; do
  /bin/launchctl bootout "system/$label" >/dev/null 2>&1 || true
done
exit 0
''', True)
        write(scripts / "postinstall", f'''#!/bin/sh
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
if [ "$3" != "/" ]; then
  echo "Native keyboard components must be installed on the startup volume." >&2
  exit 1
fi
root='/{HELPER_ROOT}'
core='/Applications/Deskflow.app/Contents/MacOS/deskflow-core'
if [ ! -f "$core" ] || [ "$(/usr/bin/stat -f '%u' '/Applications/Deskflow.app')" != "0" ]; then
  echo "Use the full macOS Deskflow installer to install its native keyboard component." >&2
  exit 1
fi
actual=$(/usr/bin/shasum -a 256 "$core" | /usr/bin/awk '{{print $1}}')
cdhash=$(/usr/bin/codesign -dv --verbose=4 "$core" 2>&1 | /usr/bin/awk -F= '/^CDHash=/{{print $2}}')
if [ "$actual" != '{expected_core}' ] || [ "$cdhash" != '{expected_cdhash}' ]; then
  echo "The installed Deskflow version does not match this native keyboard component." >&2
  exit 1
fi
/usr/bin/codesign --verify --strict "$core"
umask 077
manifest=$(/usr/bin/mktemp "$root/allowed-binaries.XXXXXX")
cat > "$manifest" <<'DESKFLOW_NATIVE_MANIFEST'
core={expected_core}
core-cdhash={expected_cdhash}
version={args.version}
DESKFLOW_NATIVE_MANIFEST
/usr/sbin/chown root:wheel "$manifest"
/bin/chmod 644 "$manifest"
/bin/mv -f "$manifest" "$root/allowed-binaries.conf"
/usr/sbin/chown -R root:wheel "$root"
/bin/chmod 755 "$root" "$root/deskflow-hid-bridge" "$root/start-virtual-hid-service"
for label in {labels}; do
  /usr/sbin/chown root:wheel "/Library/LaunchDaemons/$label.plist"
  /bin/chmod 644 "/Library/LaunchDaemons/$label.plist"
  /bin/launchctl bootstrap system "/Library/LaunchDaemons/$label.plist"
done
exit 0
''', True)
        helper_pkg = stage / "native-helper.pkg"
        run("/usr/bin/pkgbuild", "--root", str(payload), "--install-location", "/", "--ownership", "recommended",
            "--scripts", str(scripts), "--identifier", "org.deskflow.native-keyboard", "--version", args.version,
            str(helper_pkg))
        shutil.copy2(vendor, stage / DRIVER_NAME)
        # Preserve the original vendor PKG bytes and signing identity.
        packages = [("org.pqrs.Karabiner-DriverKit-VirtualHIDDevice", DRIVER_NAME),
                    ("org.deskflow.native-keyboard", helper_pkg.name)]
        native_xml = stage / "native.xml"
        distribution(packages, native_xml, "Deskflow Native Keyboard")
        resources = bundle / "Contents/Resources"
        native_pkg = resources / "Deskflow-NativeKeyboard.pkg"
        run("/usr/bin/productbuild", "--distribution", str(native_xml), "--package-path", str(stage), str(native_pkg))
        run(*sign_args, str(bundle))
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(bundle))

        # A complete offline installer installs the app first, then its signed
        # vendor component and narrowly scoped bridge. No driver is activated
        # during packaging or by these scripts; activation is a user action.
        app_pkg = stage / "deskflow-app.pkg"
        app_payload = stage / "app-payload"
        shutil.copytree(bundle, app_payload / "Applications/Deskflow.app", symlinks=True)
        components = stage / "app-components.plist"
        run("/usr/bin/pkgbuild", "--analyze", "--root", str(app_payload), str(components))
        component_info = plistlib.loads(components.read_bytes())
        for component in component_info:
            component["BundleIsRelocatable"] = False
            component["BundleOverwriteAction"] = "upgrade"
            component["BundleIsVersionChecked"] = False
        components.write_bytes(plistlib.dumps(component_info))
        run("/usr/bin/pkgbuild", "--root", str(app_payload), "--component-plist", str(components),
            "--install-location", "/", "--ownership", "recommended", "--identifier", "org.deskflow.application",
            "--version", args.version, str(app_pkg))
        full_xml = stage / "full.xml"
        distribution([("org.deskflow.application", app_pkg.name)] + packages, full_xml, "Deskflow")
        full_pkg = output / f"deskflow-{args.version}-macos-arm64-full.pkg"
        run("/usr/bin/productbuild", "--distribution", str(full_xml), "--package-path", str(stage), str(full_pkg))
        (output / "native-keyboard-component.txt").write_text(
            f"Upstream component: {DRIVER_NAME}\nSHA256: {DRIVER_SHA256}\n"
            "Identity: Fumihiko Takayama (G43BCU2T37), original signed and notarized installer preserved.\n"
            "Native helper self-test and final application signature verification passed.\n", encoding="utf-8")
        print(full_pkg)


if __name__ == "__main__":
    main()
