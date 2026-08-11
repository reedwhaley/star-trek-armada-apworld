from BaseClasses import ItemClassification, Region
from worlds.AutoWorld import World
import os
import traceback

from worlds.generic.Rules import set_rule
from worlds.LauncherComponents import Component, Type, components, icon_paths, launch_subprocess

from .items import (CAMPAIGN_MISSIONS, CAMPAIGN_MODULES, CAPABILITY_EFFECTS, FACTIONS,
                    FACTION_ACCESS_ITEMS, FINAL_ACCESS_ITEM, ArmadaItem, finale_mission_required_items,
                    item_name_to_id, item_table, TRAP_ITEMS)
from .locations import (ArmadaLocation, OBJECTIVE_TRANSITIONS, location_name_to_id,
                        location_table, mission_for_module, objective_location_by_event)
from .options import ArmadaOptions


def _run_client(*args: str) -> None:
    """Child-process wrapper that preserves a bootstrap failure for operators."""
    try:
        from .Client import launch
        launch(*args)
    except BaseException:
        log_path = os.path.join(os.environ.get("ProgramData", "."), "Archipelago", "logs",
                                "StarTrekArmadaClient-bootstrap-error.txt")
        with open(log_path, "w", encoding="utf-8") as stream:
            traceback.print_exc(file=stream)
        raise


def launch_client(*args: str) -> None:
    """Register the runtime bridge as a first-class Archipelago client."""
    launch_subprocess(_run_client, name="StarTrekArmadaClient", args=args)


components.append(Component(
    "Star Trek: Armada Client",
    func=launch_client,
    component_type=Type.CLIENT,
    game_name="Star Trek: Armada",
    supports_uri=True,
    description="Launch Armada, attach the observer, and connect its campaign checks to Archipelago.",
))
# Deliberately use Archipelago's standard icon until an original project icon is
# supplied. Do not package icons or other assets extracted from Armada.
_original_icon = os.path.join(os.path.dirname(__file__), "assets", "star_trek_armada.ico")
if os.path.isfile(_original_icon):
    icon_paths["star_trek_armada"] = f"ap:{__name__}/assets/star_trek_armada.ico"


class StarTrekArmadaWorld(World):
    """Campaign mission, unit, and technology randomization for Star Trek: Armada."""

    game = "Star Trek: Armada"
    options_dataclass = ArmadaOptions
    item_name_to_id = item_name_to_id
    location_name_to_id = location_name_to_id
    origin_region_name = "Menu"
    victory_location_name = "Finale Mission 4 Complete"
    # Universal Tracker can recreate this world from the generated room data,
    # so players do not need to keep a copy of the original YAML beside it.
    ut_can_gen_without_yaml = True

    @staticmethod
    def interpret_slot_data(slot_data: dict) -> dict:
        """Give Universal Tracker the real generation inputs on connection."""
        return slot_data

    def generate_early(self) -> None:
        # UT performs a second, slot-specific generation after it connects.
        # Apply the original non-deterministic outcomes before any regions or
        # pool contents are created, rather than rolling a new faction/trap
        # amount from a local YAML.
        tracker_data = getattr(self.multiworld, "re_gen_passthrough", {}).get(self.game, {})
        if tracker_data:
            slot_options = tracker_data.get("generation_options", {})
            for name, value in slot_options.items():
                option = getattr(self.options, name, None)
                if option is not None:
                    setattr(self.options, name, option.from_any(value))
        self.active_locations = [
            (name, data) for name, data in location_table.items()
            if data.kind == "complete" or self.options.objective_transition_checks.value
        ]
        self.starting_faction = tracker_data.get("starting_faction") if tracker_data else None
        if self.starting_faction not in FACTIONS:
            self.starting_faction = (
                self.random.choice(FACTIONS)
                if self.options.starting_faction.value == 4
                else FACTIONS[self.options.starting_faction.value]
            )
        self.requested_nebula_trap_amount = int(
            tracker_data.get("requested_nebula_trap_amount", self.options.nebula_trap_amount.value)
            if tracker_data else self.options.nebula_trap_amount.value
        )
        self.generated_nebula_trap_amount = 0

    def create_regions(self) -> None:
        menu = Region(self.origin_region_name, self.player, self.multiworld)
        campaign = Region("Campaign", self.player, self.multiworld)
        menu.connect(campaign)
        for name, data in self.active_locations:
            campaign.locations.append(ArmadaLocation(self.player, name, data.code, campaign))
        self.multiworld.regions.extend([menu, campaign])
        self.multiworld.get_location(self.victory_location_name, self.player).place_locked_item(
            self.create_item("Victory")
        )

    def create_items(self) -> None:
        # Each faction starts at its first mission. Finale 1 and 4 have no
        # individual access item: Omega opens 1, and the five campaign keys
        # open the victory mission.
        starting_items = [
            *(f"{faction} Mission 1 Access" for faction in FACTIONS),
            FACTION_ACCESS_ITEMS[self.starting_faction],
        ]
        for name in starting_items:
            self.multiworld.push_precollected(self.create_item(name))

        pool = [name for name in item_table
                if name not in starting_items and name not in (
                    "Finale Mission 1 Access", "Finale Mission 4 Access",
                    "Dilithium Supply Cache", "Victory", *TRAP_ITEMS
                )]
        capacity = len(self.active_locations) - 1  # The final campaign check holds the locked Victory event.
        # Mission access and campaign keys are never removed.  Requested traps
        # take the next available slots, then capability items fill what
        # remains. This keeps a trap-free YAML at exactly zero traps.
        access = [name for name in pool if name.endswith(" Access")]
        keys = [name for name in pool if name in (*FACTION_ACCESS_ITEMS.values(), FINAL_ACCESS_ITEM)]
        capabilities = [name for name in pool if name not in access and name not in keys]
        available = max(0, capacity - len(access) - len(keys))
        self.generated_nebula_trap_amount = min(self.requested_nebula_trap_amount, available)
        pool = access + keys + ["Nebula Anomaly"] * self.generated_nebula_trap_amount
        pool.extend(capabilities[: max(0, capacity - len(pool))])
        pool.extend(["Dilithium Supply Cache"] * (capacity - len(pool)))
        self.multiworld.itempool.extend(self.create_item(name) for name in pool)

    def set_rules(self) -> None:
        for name, data in self.active_locations:
            access_item = f"{data.mission} Access"
            faction = data.mission.rsplit(" Mission", 1)[0]
            if faction == "Finale":
                mission_number = int(data.mission.rsplit(" ", 1)[1])
                requirements = finale_mission_required_items(mission_number)
                # The completed-mission threshold is enforced by the connected
                # client because it depends on real observer results, not
                # pre-collected Archipelago items.
                set_rule(self.multiworld.get_location(name, self.player),
                         lambda state, requirements=requirements: all(
                             state.has(item, self.player) for item in requirements
                         ))
            else:
                faction_key = FACTION_ACCESS_ITEMS[faction]
                set_rule(self.multiworld.get_location(name, self.player),
                         lambda state, item=access_item, key=faction_key: (
                             state.has(item, self.player) and state.has(key, self.player)
                         ))
        self.multiworld.completion_condition[self.player] = (
            lambda state: state.has("Victory", self.player)
        )

    def create_item(self, name: str) -> ArmadaItem:
        data = item_table[name]
        return ArmadaItem(name, data.classification, data.code, self.player)

    def get_filler_item_name(self) -> str:
        return "Dilithium Supply Cache"

    def fill_slot_data(self) -> dict:
        return {
            # These are every option that changes world construction.  UT
            # uses them when it regenerates from this room's slot data.
            "generation_options": self.options.as_dict(
                "objective_transition_checks",
                "finale_mission_completion_requirement",
                "starting_faction",
                "nebula_trap_amount",
            ),
            "objective_transition_checks": bool(self.options.objective_transition_checks.value),
            "finale_mission_completion_requirement": self.options.finale_mission_completion_requirement.value,
            "starting_faction": self.starting_faction,
            "faction_access_items": FACTION_ACCESS_ITEMS,
            "final_access_item": FINAL_ACCESS_ITEM,
            "finale_required_items": [*FACTION_ACCESS_ITEMS.values(), FINAL_ACCESS_ITEM],
            "finale_mission_requirements": {
                str(number): list(finale_mission_required_items(number))
                for number in range(1, len(CAMPAIGN_MISSIONS["Finale"]) + 1)
            },
            "item_effects": CAPABILITY_EFFECTS,
            "trap_items": TRAP_ITEMS,
            "nebula_trap_amount": self.generated_nebula_trap_amount,
            "requested_nebula_trap_amount": self.requested_nebula_trap_amount,
            "objective_transitions": OBJECTIVE_TRANSITIONS,
            "objective_locations": objective_location_by_event,
            "mission_locations": {
                module.lower(): f"{faction} Mission {number} Complete"
                for faction, modules in CAMPAIGN_MODULES.items()
                for number, module in enumerate(modules, 1)
            },
            "missions": {
                module.lower(): {"faction": faction, "number": number, "title": title}
                for faction, missions in CAMPAIGN_MISSIONS.items()
                for number, (module, title) in enumerate(missions, 1)
            },
            "goal_location": self.victory_location_name,
            "terminal_location_modules": {
                f"{faction} Mission {number} Complete": module
                for faction, modules in CAMPAIGN_MODULES.items()
                for number, module in enumerate(modules, 1)
            },
        }
