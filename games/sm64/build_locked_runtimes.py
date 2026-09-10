#!/usr/bin/env python3
"""Create redistributable, ROM-locked native SM64 runtimes for FrameTee.

Run this only as a release-maintenance step with legally acquired, unmodified
ROMs. The output contains encrypted native libraries, their SHA-256 manifest,
and the precompiled unlock tool. It never copies a ROM into the output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
ROOT = SCRIPT_DIRECTORY.parents[1]
BUILD_LIBRARY = SCRIPT_DIRECTORY / "build_full_libsm64.py"
SUPPORTED_VERSIONS = ("us", "jp", "eu")
ROM_VERSIONS = {
    ("635a2bff", ord("E")): "us",
    ("4eaa3d0e", ord("J")): "jp",
    ("a03cf036", ord("P")): "eu",
}


def normalize_rom(data: bytes) -> bytes:
    if len(data) != 8 * 1024 * 1024:
        raise ValueError("expected an 8 MiB vanilla SM64 ROM")
    if data[:4] == b"\x80\x37\x12\x40":
        return data
    output = bytearray(data)
    if data[:4] == b"\x37\x80\x40\x12":
        for index in range(0, len(output), 2):
            output[index], output[index + 1] = output[index + 1], output[index]
        return bytes(output)
    if data[:4] == b"\x40\x12\x37\x80":
        for index in range(0, len(output), 4):
            output[index : index + 4] = reversed(output[index : index + 4])
        return bytes(output)
    raise ValueError("unrecognized N64 ROM byte order")


def version_for_rom(path: Path) -> str | None:
    try:
        rom = normalize_rom(path.read_bytes())
    except ValueError:
        return None
    return ROM_VERSIONS.get((rom[0x10:0x14].hex(), rom[0x3E]))


def discover_roms(directory: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    for path in sorted(directory.iterdir()):
        if not path.is_file() or path.suffix.lower() not in {".z64", ".n64", ".v64"}:
            continue
        version = version_for_rom(path)
        if version and version not in found:
            found[version] = path
    return found


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_revision(source: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True, capture_output=True
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom-dir", required=True, type=Path, help="directory containing vanilla SM64 ROMs")
    parser.add_argument("--output", required=True, type=Path, help="directory that receives linux/ and windows/ runtime bundles")
    parser.add_argument("--lock-tool", required=True, type=Path, help="native sm64_lock executable")
    parser.add_argument("--unlocker-linux", type=Path, default=ROOT / "tools/sm64_lock/target/release/sm64_lock")
    parser.add_argument("--unlocker-windows", type=Path, default=ROOT / "tools/sm64_lock/target/x86_64-pc-windows-gnu/release/sm64_lock.exe")
    parser.add_argument("--source", type=Path, default=ROOT / "libs/sm64ex", help="pinned SM64EX source checkout")
    parser.add_argument("--targets", choices=("linux", "windows"), nargs="+", default=("linux", "windows"))
    parser.add_argument("--versions", choices=("us", "jp", "eu"), nargs="+", default=SUPPORTED_VERSIONS)
    parser.add_argument("--jobs", type=int, default=None)
    args = parser.parse_args()

    if not args.rom_dir.is_dir():
        raise SystemExit(f"ROM directory does not exist: {args.rom_dir}")
    if not args.lock_tool.is_file():
        raise SystemExit(f"lock tool does not exist: {args.lock_tool}")
    unlockers = {"linux": args.unlocker_linux, "windows": args.unlocker_windows}
    for target in args.targets:
        if not unlockers[target].is_file():
            raise SystemExit(f"{target} unlocker does not exist: {unlockers[target]}")

    roms = discover_roms(args.rom_dir)
    versions = [version for version in args.versions if version in roms]
    if not versions:
        raise SystemExit("no supported US, JP, or EU vanilla SM64 ROM was found")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    revision = source_revision(args.source)
    with tempfile.TemporaryDirectory(prefix="frametee-sm64-runtime-") as temporary:
        temporary_directory = Path(temporary)
        for target in args.targets:
            extension = ".dll" if target == "windows" else ".so"
            bundle = output / target
            bundle.mkdir(parents=True, exist_ok=True)
            # A release can be regenerated with a subset of supported ROMs;
            # remove only files this script owns so stale locked revisions do
            # not slip into that package.
            for existing in bundle.iterdir():
                if existing.name in {"manifest.json", "sm64_lock", "sm64_lock.exe"} or (
                    existing.name.startswith("frametee_sm64_") and existing.name.endswith(".locked")
                ):
                    existing.unlink()
            unlocker_name = "sm64_lock.exe" if target == "windows" else "sm64_lock"
            bundled_unlocker = bundle / unlocker_name
            shutil.copy2(unlockers[target], bundled_unlocker)
            manifest_entries = []
            for version in versions:
                plain = temporary_directory / f"frametee_sm64_{version}{extension}"
                metadata_path = temporary_directory / f"frametee_sm64_{version}-{target}.json"
                command = [
                    "python3", str(BUILD_LIBRARY), "--rom", str(roms[version]), "--output", str(plain),
                    "--target", target, "--metadata-output", str(metadata_path), "--source", str(args.source),
                ]
                if args.jobs:
                    command.extend(["--jobs", str(args.jobs)])
                subprocess.run(command, check=True)
                locked_name = plain.name + ".locked"
                locked = bundle / locked_name
                subprocess.run(
                    [str(args.lock_tool), "--lock", "--input", str(plain), "--output", str(locked), "--rom", str(roms[version])],
                    check=True,
                )
                metadata = json.loads(metadata_path.read_text())
                manifest_entries.append(
                    {
                        "version": version,
                        "target": target,
                        "rom_sha256": metadata["rom_sha256"],
                        "library": plain.name,
                        "library_sha256": metadata["library_sha256"],
                        "locked_library": locked_name,
                        "locked_library_sha256": sha256(locked),
                    }
                )
            manifest = {
                "format": 1,
                "lock_format": "pwbox-sodium-compatible-with-wafel",
                "sm64ex_revision": revision,
                "unlocker": unlocker_name,
                "unlocker_sha256": sha256(bundled_unlocker),
                "runtimes": manifest_entries,
            }
            (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
