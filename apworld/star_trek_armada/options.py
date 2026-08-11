from dataclasses import dataclass

from Options import Choice, PerGameCommonOptions, Range, Toggle

from .items import CAMPAIGN_MODULES


class ObjectiveTransitionChecks(Toggle):
    """Include the full static catalog of objective-transition locations."""
    display_name = "Objective transition checks"
    default = 1


class FinaleMissionCompletionRequirement(Range):
    """Other campaign missions required before Alpha and the Omega, Part II unlocks."""
    display_name = "Finale mission completion requirement"
    range_start = 0
    range_end = sum(len(modules) for modules in CAMPAIGN_MODULES.values()) - 1
    default = 0


class StartingFaction(Choice):
    """Faction key precollected at game start."""
    display_name = "Starting faction"
    option_federation = 0
    option_klingon = 1
    option_romulan = 2
    option_borg = 3
    option_random = 4
    default = 4


class NebulaTrapAmount(Range):
    """Number of Nebula Anomaly traps placed in the item pool; 0 disables them."""
    display_name = "Nebula Trap Amount"
    range_start = 0
    range_end = 20
    default = 0


@dataclass
class ArmadaOptions(PerGameCommonOptions):
    objective_transition_checks: ObjectiveTransitionChecks
    finale_mission_completion_requirement: FinaleMissionCompletionRequirement
    starting_faction: StartingFaction
    nebula_trap_amount: NebulaTrapAmount
