from typing import NamedTuple

from BaseClasses import Item, ItemClassification


class ItemData(NamedTuple):
    code: int
    classification: ItemClassification


class ArmadaItem(Item):
    game = "Star Trek: Armada"


ITEM_OFFSET = 7_860_000
FACTIONS = ("Federation", "Klingon", "Romulan", "Borg")
FACTION_ACCESS_ITEMS = {
    "Federation": "U.S.S. Enterprise",
    "Klingon": "IKS Negh'Var",
    "Romulan": "IRW Valdore",
    "Borg": "Tactical Cube 138",
}
FINAL_ACCESS_ITEM = "Omega Particle"


def finale_mission_required_items(number: int) -> tuple[str, ...]:
    """Return the campaign-key and access-item gate for a Finale mission.

    ``Once and Again`` is the entry point to the finale arc, so it needs only
    the Omega Particle.  The next two missions retain their individual access
    items, while the victory mission requires every faction key.  It has no
    separate Finale Mission 4 Access item.
    """
    if number == 1:
        return (FINAL_ACCESS_ITEM,)
    if number in (2, 3):
        return (FINAL_ACCESS_ITEM, f"Finale Mission {number} Access")
    if number == 4:
        return (FINAL_ACCESS_ITEM, *FACTION_ACCESS_ITEMS.values())
    raise ValueError(f"Unknown Finale mission number: {number}")
# Campaign numbering is the order displayed in Armada's campaign shell, not
# the suffix embedded in its map/module filename.  This is the verified order
# in the installation's mshell.set.
CAMPAIGN_MISSIONS = {
    "Federation": (("Federation1S.dsl", "Premonitions"), ("Federation2S.dsl", "Paradise Revisited"),
                   ("Federation5S.dsl", "Vendetta"), ("Federation3S.dsl", "Dark Omens")),
    "Klingon": (("Klingon3S.dsl", "To the Gates of Sto'Vo'Kor"), ("Klingon1S.dsl", "The Enemy Within"),
                ("Klingon4S.dsl", "A Good Day to Die"), ("Klingon5S.dsl", "Gray Eminence")),
    "Romulan": (("Romulan2S.dsl", "Cloak and Dagger"), ("Romulan4S.dsl", "Call to Power"),
                ("Romulan3S.dsl", "The Gauntlet"), ("Romulan5S.dsl", "Unholy Alliances")),
    "Borg": (("Borg1S.dsl", "Resurrection"), ("Borg3S.dsl", "Assimilation"),
             ("Borg4S.dsl", "Extermination"), ("Borg5S.dsl", "The Twilight Hour")),
    "Finale": (("Finale4S.dsl", "Once and Again"), ("Finale1S.dsl", "A Line in the Sand"),
               ("Finale5S.dsl", "The Alpha and the Omega, Part I"),
               ("Finale6S.dsl", "The Alpha and the Omega, Part II")),
}
CAMPAIGN_MODULES = {faction: tuple(module for module, _ in missions)
                    for faction, missions in CAMPAIGN_MISSIONS.items()}
# Verified from mshell.set and runtime map-load traces.  These are the stock
# map identifiers consumed by Armada's campaign map loader, not filenames
# inferred from Archipelago's displayed mission numbers.
MISSION_MAPS = {
    "federation1s.dsl": "fed1.bzn", "federation2s.dsl": "fed2.bzn",
    "federation5s.dsl": "fed5.bzn", "federation3s.dsl": "fed3.bzn",
    "klingon3s.dsl": "kling3.bzn", "klingon1s.dsl": "kling1.bzn",
    "klingon4s.dsl": "kling4.bzn", "klingon5s.dsl": "kling5.bzn",
    "romulan2s.dsl": "rom2.bzn", "romulan4s.dsl": "rom4.bzn",
    "romulan3s.dsl": "rom3.bzn", "romulan5s.dsl": "rom5.bzn",
    "borg1s.dsl": "borg1.bzn", "borg3s.dsl": "borg3.bzn",
    "borg4s.dsl": "borg4.bzn", "borg5s.dsl": "borg5.bzn",
    "finale4s.dsl": "finale4.bzn", "finale1s.dsl": "finale1.bzn",
    "finale5s.dsl": "finale5.bzn", "finale6s.dsl": "finale6.bzn",
}
MISSION_COUNTS = {faction: len(modules) for faction, modules in CAMPAIGN_MODULES.items()}

item_table: dict[str, ItemData] = {}

for faction, count in MISSION_COUNTS.items():
    for number in range(1, count + 1):
        name = f"{faction} Mission {number} Access"
        item_table[name] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.progression)

# These keys are an additional campaign layer, rather than replacements for
# individual mission access.  A faction key permits its received mission
# access items to be used.  The Omega Particle opens Finale 1, while the
# faction keys are reserved for the final victory mission.
for name in (*FACTION_ACCESS_ITEMS.values(), FINAL_ACCESS_ITEM):
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.progression)

CAPABILITY_EFFECTS = {}
for faction, prefix, pods, super_nodes in (
    ("Federation", "f", "fedpod", ("fsuperbl",)), ("Klingon", "k", "klipod", ("ksuperbl", "ksuper")),
    ("Romulan", "r", "rompod", ("rsuperbl", "rsuper")), ("Borg", "b", "borpod", ("bsuperbl",)),
):
    CAPABILITY_EFFECTS.update({
        f"{faction} Economy and Construction": [f"{prefix}const", f"{prefix}freight"],
        f"{faction} Defenses and Sensors": [f"{prefix}sensor", f"{prefix}turret", f"{prefix}turret2"],
        f"{faction} Basic Fleet": [f"{prefix}yard", f"{prefix}scout", f"{prefix}destroy"],
        f"{faction} Research I Fleet": [f"{prefix}resear", f"{prefix}cruise1", f"{prefix}cruise2"],
        f"{faction} Advanced Shipyard": [f"{prefix}yard2"], f"{faction} Special Vessel": [f"{prefix}special"],
        f"{faction} Battleship": [f"{prefix}battle"], f"{faction} Advanced Research": [f"{prefix}resear2"],
        f"{faction} Research Pod 1": [f"{pods}1"], f"{faction} Research Pods 2-5": [f"{pods}{n}" for n in range(2, 6)],
        f"{faction} Research Pods 6-9": [f"{pods}{n}" for n in range(6, 10)], f"{faction} Superweapon Program": list(super_nodes),
    })

for name in CAPABILITY_EFFECTS:
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.useful)

# The observer resolves this generic item to stock mnebula ODFs using the
# deliberate Mutara/Cerulean/Metrion/Radioactive 50/30/12/8 weighting.
TRAP_ITEMS = {
    "Nebula Anomaly": {"command": "random"},
}
for name in TRAP_ITEMS:
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.trap)

item_table["Dilithium Supply Cache"] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.filler)
# The locked victory event is sent through a real network location.  It must
# have a numeric ID for WebHost's compiled LocationStore, unlike a local-only
# event location with no address.
item_table["Victory"] = ItemData(ITEM_OFFSET + len(item_table), ItemClassification.progression)

item_name_to_id = {name: data.code for name, data in item_table.items()}
