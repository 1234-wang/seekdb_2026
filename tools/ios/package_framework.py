#!/usr/bin/env python3
"""Write the public header and Info.plist alongside the linked framework."""

import argparse
from pathlib import Path
import plistlib
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "config", "header", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    # Use the linker's deployment target, including command-line overrides.
    build = subprocess.check_output(["xcrun", "vtool", "-show-build", str(args.binary)], text=True)
    minimum_os = re.search(r"minos\s+(\S+)", build).group(1)
    platform = re.search(r"platform\s+(\S+)", build).group(1)
    revision = re.search(r'#define SEEKDB_IOS_BUILD_ID "([^"]+)"', args.config.read_text()).group(1)
    destination = args.output
    (destination / "Headers").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.header, destination / "Headers/seekdb.h")
    info = {
        "CFBundleExecutable": "SeekDB", "CFBundleIdentifier": "org.oceanbase.seekdb",
        "CFBundleName": "SeekDB", "CFBundlePackageType": "FMWK",
        "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
        "CFBundleSupportedPlatforms": [{"IOS": "iPhoneOS", "IOSSIMULATOR": "iPhoneSimulator"}[platform]],
        "MinimumOSVersion": minimum_os, "SeekDBSourceRevision": revision,
        "SeekDBTestHooks": "disabled",
    }
    (destination / "Info.plist").write_bytes(plistlib.dumps(info))


if __name__ == "__main__":
    main()
