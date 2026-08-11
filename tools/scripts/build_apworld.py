"""Package the local Star Trek Armada custom world as a .apworld archive."""

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def build_apworld(source: Path, output: Path) -> None:
    """Build an APWorld archive from its source directory."""
    source = source.resolve()
    if not (source / "archipelago.json").is_file():
        raise ValueError(f"not an apworld source directory: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob("*")):
            relative_path = path.relative_to(source.parent)
            if path.is_file() and "__pycache__" not in path.parts:
                archive.write(path, relative_path.as_posix())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("apworld/star_trek_armada"))
    parser.add_argument("--output", type=Path, default=Path("out/star_trek_armada.apworld"))
    args = parser.parse_args()

    try:
        build_apworld(args.source, args.output)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
