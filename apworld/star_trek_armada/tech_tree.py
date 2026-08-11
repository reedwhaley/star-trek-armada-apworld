"""Pure campaign tech-tree overlay generation for received capability items.

Campaign ``.tt`` files carry availability for every faction.  The adapter must
leave all existing lines intact (including AI and story support) and only add
stock multiplayer entries for the active player faction.  Serving the result
to Armada is intentionally a separate, version-pinned runtime concern.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
import struct


FACTION_PREFIX = {
    "Federation": "f",
    "Klingon": "k",
    "Romulan": "r",
    "Borg": "b",
}
ZFS_HEADER = struct.Struct("<4s6I")


@dataclass(frozen=True)
class TechEntry:
    node: str
    requirements: tuple[str, ...]
    raw: str


def parse_tech_tree(text: str) -> list[TechEntry]:
    """Parse Armada's simple ``name.odf prerequisite-count prerequisites`` format."""
    entries: list[TechEntry] = []
    for raw in text.splitlines():
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) < 2 or not fields[1].isdigit():
            raise ValueError(f"unrecognized tech-tree line: {raw!r}")
        count = int(fields[1])
        requirements = tuple(Path(name).stem for name in fields[2:])
        if len(requirements) != count:
            raise ValueError(f"incorrect prerequisite count: {raw!r}")
        entries.append(TechEntry(Path(fields[0]).stem, requirements, raw.rstrip()))
    return entries


def capability_nodes(effects: Mapping[str, Iterable[str]], received_names: Iterable[str]) -> set[str]:
    """Resolve received Archipelago capability names to stock tech-tree nodes."""
    received = set(received_names)
    return {node for item, nodes in effects.items() if item in received for node in nodes}


def read_archive_text(archive: Path, name: str) -> str:
    """Read one stock text resource from an Armada version-1 ZFS archive."""
    data = archive.read_bytes()
    magic, version, name_length, entries_per_block, _file_count, key, block_offset = ZFS_HEADER.unpack_from(data)
    if magic != b"ZFSF" or version != 1:
        raise ValueError(f"{archive} is not a version 1 Armada ZFS archive")
    entry_size = name_length + 20
    wanted = name.casefold()
    while block_offset:
        (next_block,) = struct.unpack_from("<I", data, block_offset)
        entry_offset = block_offset + 4
        for _ in range(entries_per_block):
            raw_name = data[entry_offset: entry_offset + name_length]
            if not raw_name.strip(b"\0"):
                break
            entry_name = raw_name.split(b"\0", 1)[0].decode("ascii")
            offset, _file_id, length, _checksum, _unused = struct.unpack_from("<5I", data, entry_offset + name_length)
            if entry_name.casefold() == wanted:
                payload = bytearray(data[offset: offset + length])
                key_bytes = key.to_bytes(4, "little")
                for index in range(0, len(payload) - len(payload) % 4, 4):
                    for byte_index, key_byte in enumerate(key_bytes):
                        payload[index + byte_index] ^= key_byte
                return bytes(payload).decode("cp1252")
            entry_offset += entry_size
        block_offset = next_block
    raise FileNotFoundError(f"{name} is not present in {archive}")


def build_mission_overlay(archive: Path, map_name: str, faction: str,
                          effects: Mapping[str, Iterable[str]], received_names: Iterable[str]) -> str:
    """Build a standard faction mission overlay from unmodified stock resources."""
    return build_additive_overlay(
        read_archive_text(archive, f"{Path(map_name).stem}.tt"),
        read_archive_text(archive, "tech1.tt"),
        faction,
        capability_nodes(effects, received_names),
    )


def build_additive_overlay(mission_tree: str, multiplayer_tree: str, faction: str,
                           enabled_nodes: Iterable[str]) -> str:
    """Add received multiplayer capabilities without taking campaign availability away.

    A new entry is added only after its prerequisites are already available in
    the mission tree or were granted earlier in the same overlay. Descendant
    tech effects are included when their prerequisite chain originates in a
    granted capability node.
    """
    try:
        prefix = FACTION_PREFIX[faction]
    except KeyError as exc:
        raise ValueError(f"unsupported Armada faction: {faction}") from exc
    mission_entries = parse_tech_tree(mission_tree)
    multiplayer_entries = parse_tech_tree(multiplayer_tree)
    existing = {entry.node for entry in mission_entries}
    available = set(existing)
    requested = {node for node in enabled_nodes if node.startswith(prefix)}
    granted: set[str] = set()
    additions: list[TechEntry] = []

    # The stock multiplayer order is dependency-first. Repeat only to support
    # unusual campaign trees whose baseline supplies a prerequisite later.
    #
    # A faction-prefixed node is allowed only when it was explicitly granted.
    # Global ``g...`` entries represent research effects.  They are admitted
    # only when they are a fully-satisfied descendant of a granted node; this
    # preserves unrelated stock global technology and every other faction.
    while True:
        progress = False
        for entry in multiplayer_entries:
            if entry.node in available:
                continue
            is_player_node = entry.node.startswith(prefix)
            inherited = bool(set(entry.requirements) & granted)
            wanted = entry.node in requested if is_player_node else inherited
            if wanted and set(entry.requirements).issubset(available):
                additions.append(entry)
                available.add(entry.node)
                granted.add(entry.node)
                progress = True
        if not progress:
            break

    if not additions:
        return mission_tree if mission_tree.endswith("\n") else mission_tree + "\n"
    suffix = "\n// Archipelago received capability overlay (player faction only)\n"
    suffix += "\n".join(entry.raw for entry in additions) + "\n"
    return mission_tree.rstrip("\n") + suffix
