"""Create a distributable Windows release archive for Star Trek: Armada."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from build_apworld import build_apworld


RELEASE_README = (
    "Star Trek: Armada Archipelago client\n\n"
    "Requirements\n------------\n"
    "- Archipelago 0.6.7 or newer\n"
    "- A legally obtained retail copy of Star Trek: Armada\n\n"
    "Installation\n------------\n"
    "1. Extract this archive to a permanent folder.\n"
    "2. Install star_trek_armada.apworld with the Archipelago Launcher.\n"
    "3. Set STAR_TREK_ARMADA_CLIENT_ROOT to this extracted folder.\n"
    "4. Set STAR_TREK_ARMADA_GAME_ROOT to the folder containing Armada.exe.\n"
    "5. Restart the Archipelago Launcher, then open Star Trek: Armada Client.\n\n"
    "This unofficial project contains no Armada game files or modified game executables.\n"
)


def is_x86_pe(path: Path) -> bool:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    return (len(data) >= pe_offset + 6 and data[pe_offset:pe_offset + 4] == b"PE\0\0"
            and struct.unpack_from("<H", data, pe_offset + 4)[0] == 0x014C)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, default=Path("out"))
    args = parser.parse_args()

    root = args.root.resolve()
    manifest = json.loads((root / "apworld/star_trek_armada/archipelago.json").read_text(encoding="utf-8"))
    version = manifest["world_version"]
    output = args.output.resolve()
    apworld = output / "star_trek_armada.apworld"
    build_apworld(root / "apworld/star_trek_armada", apworld)

    binaries = (root / "bin/armada_observer.dll", root / "bin/armada_injector.exe")
    missing = [str(path) for path in binaries if not path.is_file()]
    if missing:
        raise SystemExit("missing release binaries: " + ", ".join(missing))
    invalid = [str(path) for path in binaries if not is_x86_pe(path)]
    if invalid:
        raise SystemExit("release binaries must be 32-bit PE files: " + ", ".join(invalid))

    archive_path = output / f"StarTrekArmada-{version}-win32.zip"
    with ZipFile(archive_path, "w", ZIP_DEFLATED) as archive:
        archive.write(apworld, "star_trek_armada.apworld")
        for binary in binaries:
            archive.write(binary, f"bin/{binary.name}")
        archive.writestr("README.txt", RELEASE_README)
    print(archive_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
