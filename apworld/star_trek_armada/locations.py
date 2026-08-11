from typing import NamedTuple

from BaseClasses import Location

from .items import CAMPAIGN_MODULES, MISSION_COUNTS


class LocationData(NamedTuple):
    code: int
    mission: str
    kind: str
    module: str | None = None
    objective_file: str | None = None


class ArmadaLocation(Location):
    game = "Star Trek: Armada"


LOCATION_OFFSET = 7_861_000

# Static direct-call inventory, accepted for generation by user direction.
# Values omit the initial display state; each remaining text file is one check.
OBJECTIVE_TRANSITIONS = {
    "Borg1S.dsl": ("B1OB2.txt", "B1OB4.txt", "B1OB3.txt"), "Borg3S.dsl": ("b3ob2.txt",),
    "Federation1S.dsl": ("F1OB2.txt", "F1OB3.txt"), "Federation2S.dsl": tuple(f"F2OB{i}.txt" for i in range(2, 7)),
    "Federation3S.dsl": ("F3OB2.txt", "F3OB3.txt"), "Federation5S.dsl": tuple(f"F5OB{i}.txt" for i in range(2, 6)),
    "Finale1S.dsl": ("fi1ob2.txt",), "Finale4S.dsl": ("fi4ob2.txt",), "Finale5S.dsl": ("Fi5OB2.txt", "Fi5OB3.txt", "Fi5OB4.txt"),
    "Finale6S.dsl": ("fi6ob1.txt", "fi6ob2.txt", "fi6ob3.txt", "fi6ob4.txt", "fi6ob5.txt"),
    "Klingon1S.dsl": ("K1OB2.txt", "K1OB3.txt"),
    "Romulan2S.dsl": ("R2OB2.txt", "R2OB3.txt", "R2OB4.txt", "R2OB12.txt", "R2OB5.txt", "R2OB6.txt", "R2OB7.txt", "R2OB8.txt", "R2OB9.txt", "R2OB10.txt", "R2OB11.txt"),
    "Romulan3S.dsl": ("r3ob2.txt", "r3ob3.txt", "r3ob4.txt"), "Romulan4S.dsl": ("r4ob2.txt",),
    "Romulan5S.dsl": ("r5ob4.txt", "r5ob2.txt", "r5ob3.txt"),
}


def mission_for_module(module: str) -> str:
    for faction, modules in CAMPAIGN_MODULES.items():
        for number, candidate in enumerate(modules, 1):
            if candidate.casefold() == module.casefold():
                return f"{faction} Mission {number}"
    raise ValueError(f"unrecognized mission module {module}")


location_table: dict[str, LocationData] = {}
for faction, count in MISSION_COUNTS.items():
    for number in range(1, count + 1):
        mission = f"{faction} Mission {number}"
        location_table[f"{mission} Complete"] = LocationData(LOCATION_OFFSET + len(location_table), mission, "complete")

for module, files in OBJECTIVE_TRANSITIONS.items():
    mission = mission_for_module(module)
    for number, objective_file in enumerate(files, 1):
        name = f"{mission} Objective {number}"
        location_table[name] = LocationData(LOCATION_OFFSET + len(location_table), mission, "objective", module, objective_file)

location_name_to_id = {name: data.code for name, data in location_table.items()}
objective_location_by_event = {
    f"{data.module.lower()}|{data.objective_file.lower()}": name
    for name, data in location_table.items() if data.kind == "objective"
}
