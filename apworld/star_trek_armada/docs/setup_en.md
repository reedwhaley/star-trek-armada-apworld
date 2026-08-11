# Star Trek: Armada setup

## Important

This is an unofficial fan integration, not made by, affiliated with, or
supported by Activision, Paramount, or their licensors. It requires a legally
obtained retail copy of *Star Trek: Armada*. The APWorld and client do not
include, download, or redistribute Armada executables, DLLs, maps, missions,
archives, art, icons, music, video, or other original game assets.

The client launches the user's local installation and injects an original
observer DLL at runtime. It does not distribute a modified Armada executable.

This world is an early integration package for the local Armada bridge. It
defines campaign completion checks, statically inventoried objective-transition
checks, mission access items, campaign keys, and faction capability items.

## Client

Copy `armada_observer.dll` and `armada_injector.exe` from the release into the
folder containing the retail `Armada.exe`, then install
`star_trek_armada.apworld` into Archipelago and restart the Archipelago
Launcher. Its **Client** category contains **Star Trek: Armada Client**. On its
first launch, the client opens the standard Windows folder picker; select the
folder containing `Armada.exe`. The selection is saved per user. Enter the room
address, slot name, and optional password in the normal Archipelago client. The
Mission Launcher shows received items and campaign missions that are currently
unlocked; starting Armada attaches the matching observer automatically. No
separate pipe reader, injector command, bridge command, or environment variable
is required.

When launched from a terminal, `--game-root` can override the saved retail
Armada installation folder.

`objective_transition_checks` is a checkbox. Disable it for completion-only
generation; enable it (the default) for the full static objective-transition
catalog (47 non-terminal checks in this build). The bridge deduplicates a
transition by mission module and objective text filename. Capability items are
delivered and persisted, but no game effect is applied until unit/research
state tracing validates a version-specific effect adapter.

Mission access remains a separate item layer. The four primary Mission 1
access items are precollected, but a player has only one faction key at the
start. This makes exactly the selected (or random) starting faction playable;
the other faction keys and later mission-access items are randomized.

Each seed also has five campaign-key items: **U.S.S. Enterprise** (Federation),
**IKS Negh'Var** (Klingon), **IRW Valdore** (Romulan), **Tactical Cube 138**
(Borg), and **Omega Particle** (Finale). Every faction mission requires both
its normal mission-access item and that faction's key. Finale 1 requires only
the Omega Particle; Finale 2 and 3 require Omega plus their own access item;
and Finale 4 requires Omega plus all four faction keys. `starting_faction`
can be `federation`, `klingon`, `romulan`,
`borg`, or `random` (the default); its faction key begins precollected and is
therefore omitted from the item pool.

The campaign order and mission names are taken from Armada's `mshell.set`.
Filename suffixes are not campaign numbers; for example, Federation Mission 3
is **Vendetta** and loads `Federation5S.dsl`. The goal is **The Alpha and the
Omega, Part II** (Finale Mission 4); checking that completion location awards
the internal Victory event.

`finale_mission_completion_requirement` controls how many other campaign
mission completions are required before **The Alpha and the Omega, Part II**
unlocks. It ranges from `0` to `19` and defaults to `0`. The client uses its
durable mission-result ledger to keep Finale 4 locked until the threshold is
actually met; clearing it while locked is ignored rather than saved for later.

`nebula_trap_amount` controls how many **Nebula Anomaly** trap items are added
to the seed. It ranges from `0` (the default, no traps) to `20`; YAML also
accepts `random`. Each received anomaly chooses Mutara, Cerulean, Metrion, or
Radioactive using the 50/30/12/8 weighting. Mission access and campaign keys
always take priority, so a completion-only seed can reduce an excessively high
requested amount when it does not have enough locations.

## Universal Tracker

Install the **Universal Tracker** APWorld alongside `star_trek_armada.apworld`.
The Armada client adds its normal Tracker tab automatically when Universal
Tracker is present; the standalone **Universal Tracker** launcher entry also
works. It regenerates from the connected room's slot data, so it uses the
actual rolled starting faction, objective-check setting, and Nebula Trap Amount
instead of rerolling local `random` YAML values. The tracker reports normal
Archipelago item-access logic. The client-only Finale 4 mission-completion
threshold remains a launcher restriction, so it is shown in the Mission
Launcher rather than modeled as an in-logic tracker check.
