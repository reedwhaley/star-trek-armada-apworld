"""Archipelago Launcher client for the local Star Trek: Armada bridge.

The client starts Armada when necessary, ensures the version-pinned observer is
loaded once, and translates its named-pipe events into normal Archipelago
LocationChecks.  Received items are retained durably; effect application is
intentionally deferred until its game-state adapter has runtime evidence.
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import json
import os
import subprocess
import threading
import time
from ctypes import wintypes
from pathlib import Path

from CommonClient import (CommonContext, get_base_parser, gui_enabled, handle_url_arg,
                          logger, server_loop)
import Utils

try:
    from worlds.tracker.TrackerClient import TrackerGameContext as ArmadaContextBase
except ModuleNotFoundError:
    ArmadaContextBase = CommonContext
    UNIVERSAL_TRACKER_AVAILABLE = False
else:
    UNIVERSAL_TRACKER_AVAILABLE = True

from .items import CAMPAIGN_MODULES, MISSION_MAPS
from .locations import location_name_to_id


PIPE_NAME = r"\\.\pipe\armada_result_observer"
CONTROL_PIPE_NAME = r"\\.\pipe\armada_launcher_control"
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
ERROR_FILE_NOT_FOUND = 2
ERROR_PIPE_BUSY = 231
ERROR_NO_DATA = 232
ERROR_BROKEN_PIPE = 109
CREATE_NO_WINDOW = 0x08000000
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL

# Older generated rooms do not contain mission_locations in slot data.  These
# canonical names exist in their data packages, so retain this compatibility
# mapping while newer rooms provide the same mapping from their generated slot.
MISSION_LOCATIONS = {
    module.lower(): f"{faction} Mission {number} Complete"
    for faction, modules in CAMPAIGN_MODULES.items()
    for number, module in enumerate(modules, 1)
}


def run_hidden(command: list[str]) -> subprocess.CompletedProcess[str]:
    """Run a console helper without allowing it to take Armada's focus."""
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          startupinfo=startup, creationflags=CREATE_NO_WINDOW)


class Ledger:
    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        try:
            self.state = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            self.state = {}
        except json.JSONDecodeError:
            corrupt = path.with_suffix(path.suffix + ".corrupt")
            path.replace(corrupt)
            logger.warning("Moved malformed Armada client ledger to %s", corrupt)
            self.state = {}
        self.state.setdefault("checks", {})
        self.state.setdefault("received_items", {})
        self.state.setdefault("traps", {})
        for entry in self.state["checks"].values():
            entry.setdefault("kind", "mission" if entry.get("location_name", "").endswith(" Complete") else "objective")
    def save(self) -> None:
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(json.dumps(self.state, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.path)

    def record_check(self, event: dict, location_name: str | None, kind: str) -> bool:
        if not location_name:
            return False
        event_key = f"{event.get('pid')}:{event.get('sequence')}"
        if event_key in self.state["checks"]:
            return False
        self.state["checks"][event_key] = {
            "location_name": location_name, "kind": kind, "status": "pending",
            "payload": event, "recorded_at": int(time.time()),
        }
        self.save()
        return True

    def has_location(self, location_name: str) -> bool:
        """Return whether this local slot already recorded a location."""
        return any(entry.get("location_name") == location_name for entry in self.state["checks"].values())

    def record_reconciled_objective(self, mission_event: dict, module: str, location_name: str,
                                    objective_file: str) -> bool:
        """Record a completion-confirmed objective missing from live display events.

        Some campaign objectives are satisfied without causing their text file
        to be displayed again (for example, when the required Cube is already
        at its destination).  A verified mission success is authoritative that
        every configured objective in that mission was completed.
        """
        if self.has_location(location_name):
            return False
        event = dict(mission_event)
        event.update({
            "type": "mission_success_objective_reconcile",
            "caller_module": module,
            "objective_file": objective_file,
            "reconciled_from_success": True,
            "reconciled_location": location_name,
        })
        event_key = f"success-reconcile:{mission_event.get('pid')}:{mission_event.get('sequence')}:{location_name}"
        self.state["checks"][event_key] = {
            "location_name": location_name, "kind": "objective", "status": "pending",
            "payload": event, "recorded_at": int(time.time()),
        }
        self.save()
        return True

    def pending(self) -> list[tuple[str, str]]:
        return [(event_key, entry["location_name"])
                for event_key, entry in sorted(self.state["checks"].items(), key=lambda pair: pair[1]["recorded_at"])
                if entry["status"] == "pending"]

    def mark_submitted(self, event_keys: list[str]) -> None:
        for event_key in event_keys:
            self.state["checks"][event_key]["status"] = "submitted"
        self.save()

    def completed_mission_count(self, goal_location: str) -> int:
        return sum(
            entry.get("kind") == "mission" and entry["location_name"] != goal_location
            for entry in self.state["checks"].values()
        )

    def record_received(self, start_index: int, items: list[object]) -> list[int]:
        received: list[int] = []
        for offset, item in enumerate(items):
            if hasattr(item, "item"):
                item_id = int(item.item)
                location_id = int(item.location)
                player_id = int(item.player)
                flags = int(item.flags)
            else:
                item_id, location_id, player_id, flags = map(int, item)
            index = start_index + offset
            if str(index) in self.state["received_items"]:
                continue
            self.state["received_items"][str(index)] = {
                "item_id": item_id, "location_id": location_id, "player_id": player_id,
                "flags": flags, "received_at": int(time.time()),
            }
            received.append(index)
        if received:
            self.save()
        return received

    def record_trap(self, item_index: int, item_name: str, command: str) -> bool:
        key = str(item_index)
        if key in self.state["traps"]:
            return False
        self.state["traps"][key] = {
            "item_name": item_name, "command": command, "status": "pending",
            "received_at": int(time.time()),
        }
        self.save()
        return True

    def pending_traps(self) -> list[tuple[str, dict]]:
        return [(index, entry) for index, entry in sorted(self.state["traps"].items(), key=lambda pair: int(pair[0]))
                if entry.get("status") == "pending"]

    def mark_trap_dispatched(self, item_index: str) -> None:
        self.state["traps"][item_index]["status"] = "dispatched"
        self.state["traps"][item_index]["dispatched_at"] = int(time.time())
        self.save()

    def close(self) -> None:
        self.save()


def connect_pipe() -> wintypes.HANDLE:
    while True:
        handle = kernel32.CreateFileW(PIPE_NAME, GENERIC_READ, 0, None, OPEN_EXISTING, 0, None)
        if handle != INVALID_HANDLE_VALUE:
            return handle
        error = ctypes.get_last_error()
        if error in (ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY):
            time.sleep(1)
            continue
        raise ctypes.WinError(error)


def pipe_events():
    buffer = bytearray()
    while True:
        handle = connect_pipe()
        logger.info("Connected to Armada observer pipe.")
        try:
            while True:
                chunk = ctypes.create_string_buffer(4096)
                read = wintypes.DWORD()
                if kernel32.ReadFile(handle, chunk, len(chunk), ctypes.byref(read), None):
                    buffer.extend(chunk.raw[:read.value])
                    while b"\n" in buffer:
                        raw, _, remainder = buffer.partition(b"\n")
                        buffer[:] = remainder
                        try:
                            yield json.loads(raw.decode("utf-8"))
                        except json.JSONDecodeError:
                            logger.warning("Ignoring malformed observer JSON")
                    continue
                error = ctypes.get_last_error()
                if error == ERROR_NO_DATA:
                    time.sleep(0.1)
                elif error == ERROR_BROKEN_PIPE:
                    break
                else:
                    raise ctypes.WinError(error)
        finally:
            kernel32.CloseHandle(handle)


def send_control_request(command: str, timeout: float = 15) -> str:
    """Send one allowlisted observer-control command without stealing Armada focus."""
    deadline = time.monotonic() + timeout
    while True:
        handle = kernel32.CreateFileW(CONTROL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if handle != INVALID_HANDLE_VALUE:
            break
        error = ctypes.get_last_error()
        if error in (ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY) and time.monotonic() < deadline:
            time.sleep(0.25)
            continue
        raise ctypes.WinError(error)
    try:
        payload = (command.rstrip() + "\n").encode("ascii")
        written = wintypes.DWORD()
        if not kernel32.WriteFile(handle, payload, len(payload), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if written.value != len(payload):
            raise RuntimeError("Armada launcher control pipe accepted only a partial request.")
        response = ctypes.create_string_buffer(256)
        received = wintypes.DWORD()
        if not kernel32.ReadFile(handle, response, len(response) - 1, ctypes.byref(received), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return response.raw[:received.value].decode("ascii", errors="replace").strip()
    finally:
        kernel32.CloseHandle(handle)


def send_map_launch_request(map_name: str) -> str:
    """Ask the injected bridge to dispatch a validated stock campaign map and await its result."""
    if map_name not in set(MISSION_MAPS.values()):
        raise ValueError(f"Refusing unknown Armada campaign map: {map_name}")
    acknowledgement = send_control_request(f"launch_map {map_name}")
    if not acknowledgement.startswith("queued native campaign controller route"):
        raise RuntimeError(f"Armada did not dispatch {map_name}: {acknowledgement or 'no acknowledgement'}")
    return acknowledgement


class ArmadaContext(ArmadaContextBase):
    game = "Star Trek: Armada"
    items_handling = 0b111
    # TrackerGameContext adds its own connection tag; Armada is still an AP
    # game client and must advertise the normal AP tag to the server.
    tags = {"AP"}

    def __init__(self, server_address: str | None, password: str | None, ledger: Ledger) -> None:
        super().__init__(server_address, password)
        self.ledger = ledger
        self.mission_locations = dict(MISSION_LOCATIONS)
        self.objective_locations: dict[str, str] = {}
        self.goal_location = ""
        self.finale_mission_completion_requirement = 0
        self.finale_required_items: list[str] = []
        self.finale_mission_requirements: dict[int, list[str]] = {}
        self.missions: dict[str, dict] = {}
        self.faction_access_items: dict[str, str] = {}
        self.item_effects: dict[str, list[str]] = {}
        self.trap_items: dict[str, dict[str, object]] = {}
        self.nebula_trap_amount = 0
        self._trap_retry_scheduled = False

    async def server_auth(self, password_requested: bool = False) -> None:
        """Use Archipelago's normal client prompt for the player's slot name."""
        if password_requested and not self.password:
            await super().server_auth(password_requested)
        await self.get_username()
        await self.send_connect()

    def run_gui(self) -> None:
        """Embed the campaign picker in the standard Archipelago Kivy client."""
        from kivy.clock import Clock
        from kivy.metrics import dp
        from kivy.uix.button import Button
        from kivy.uix.gridlayout import GridLayout
        from kivy.uix.label import Label
        from kivy.uix.scrollview import ScrollView
        ctx = self

        class MissionPanel(ScrollView):
            def __init__(self) -> None:
                super().__init__()
                self.layout = GridLayout(cols=1, spacing=dp(6), padding=dp(10), size_hint_y=None)
                self.layout.bind(minimum_height=self.layout.setter("height"))
                self.add_widget(self.layout)
                Clock.schedule_interval(self.refresh, 0.5)

            def refresh(self, _dt: float) -> None:
                self.layout.clear_widgets()
                snapshot = ctx.mission_snapshot()
                self.layout.add_widget(Label(text=snapshot["status"], size_hint_y=None, height=dp(32)))
                if not snapshot["missions"]:
                    self.layout.add_widget(Label(
                        text="Use the normal Connect bar, then enter your slot name in the command field when prompted.",
                        size_hint_y=None, height=dp(48), halign="center", valign="middle"
                    ))
                    return
                for mission in snapshot["missions"]:
                    text = f"{mission['label']} — {mission['status']} — {mission['requires']}"
                    button = Button(text=text, size_hint_y=None, height=dp(42), disabled=not mission["available"])
                    if mission["available"]:
                        button.bind(on_release=lambda _button, module=mission["module"]: self.start_armada(module))
                    self.layout.add_widget(button)

            @staticmethod
            def start_armada(module: str) -> None:
                mission = ctx.missions.get(module)
                if not mission:
                    logger.error("Unknown mission selected by launcher: %s", module)
                    return
                map_name = MISSION_MAPS.get(module)
                if not map_name:
                    logger.error("No verified stock map is configured for %s.", module)
                    return

                async def launch() -> None:
                    logger.info("Launching %s Mission %s (%s) through %s.",
                                mission["faction"], mission["number"], mission["title"], map_name)
                    if ctx.game_root is None:
                        ctx.game_root = await asyncio.to_thread(prompt_for_game_root)
                    if ctx.game_root is None:
                        logger.error("Armada launch cancelled: select the folder containing Armada.exe to continue.")
                        return
                    # For a fresh process, the injector provides the selected
                    # map before resuming Armada. A running process uses the
                    # established control pipe instead.
                    started = await asyncio.to_thread(
                        start_armada_and_observer, ctx.game_root, map_name
                    )
                    if started:
                        logger.info("Armada prequeued %s for its native startup campaign controller route.", map_name)
                        # The inherited map is consumed by the full native
                        # picker during startup. Sending a second request here
                        # re-enters that picker while its first route is still
                        # completing and can crash Armada.
                        return
                    # Armada was already running, so this is the single
                    # supported way to queue a new native campaign route.
                    acknowledgement = await asyncio.to_thread(send_map_launch_request, map_name)
                    logger.info("Armada queued native campaign controller route: %s", acknowledgement)

                asyncio.create_task(launch(), name="launch selected Armada mission")

        # make_gui returns a manager *class*, not an instance. Universal
        # Tracker wraps that class to add its own tab, so subclass the returned
        # manager and add Armada's launcher during the normal build phase.
        # This works with both the stock GameManager and TrackerManager.
        manager_base = super().make_gui()

        class ArmadaManager(manager_base):
            base_title = "Archipelago Star Trek: Armada Client"

            def build(self):
                container = super().build()
                self.add_client_tab("Mission Launcher", MissionPanel())
                return container

        self.ui = ArmadaManager(self)
        self.ui_task = asyncio.create_task(self.ui.async_run(), name="UI")

    def on_package(self, cmd: str, args: dict) -> None:
        super().on_package(cmd, args)
        if cmd == "Connected":
            slot_data = args.get("slot_data", {})
            self.mission_locations.update(
                {key.lower(): value for key, value in slot_data.get("mission_locations", {}).items()}
            )
            self.objective_locations = {key.lower(): value for key, value in slot_data.get("objective_locations", {}).items()}
            self.goal_location = str(slot_data.get("goal_location", ""))
            self.finale_mission_completion_requirement = int(
                slot_data.get("finale_mission_completion_requirement", 0)
            )
            self.finale_required_items = [str(name) for name in slot_data.get("finale_required_items", [])]
            self.finale_mission_requirements = {
                int(number): [str(name) for name in requirements]
                for number, requirements in slot_data.get("finale_mission_requirements", {}).items()
            }
            self.missions = {key.lower(): value for key, value in slot_data.get("missions", {}).items()}
            self.faction_access_items = {str(key): str(value)
                                         for key, value in slot_data.get("faction_access_items", {}).items()}
            self.item_effects = {
                str(item): [str(node) for node in nodes]
                for item, nodes in slot_data.get("item_effects", {}).items()
            }
            self.trap_items = {str(name): dict(data) for name, data in slot_data.get("trap_items", {}).items()}
            self.nebula_trap_amount = int(slot_data.get("nebula_trap_amount", 0))
            logger.info("This Armada seed contains %s Nebula Anomaly trap item(s).", self.nebula_trap_amount)
            asyncio.create_task(self.flush_pending())
            asyncio.create_task(self.flush_traps())
        elif cmd == "ReceivedItems":
            indexes = self.ledger.record_received(int(args["index"]), args["items"])
            for offset, item in enumerate(args["items"]):
                index = int(args["index"]) + offset
                if index not in indexes:
                    continue
                item_id = int(item.item) if hasattr(item, "item") else int(item[0])
                try:
                    item_name = self.item_names.lookup_in_game(item_id, self.game)
                except KeyError:
                    item_name = ""
                trap = self.trap_items.get(item_name)
                if trap and self.ledger.record_trap(index, item_name, str(trap["command"])):
                    logger.info("Queued received Armada trap index=%s: %s", index, item_name)
                else:
                    logger.info("Received Armada item index=%s.", index)
            asyncio.create_task(self.flush_pending())
            asyncio.create_task(self.flush_traps())

    def _schedule_trap_retry(self) -> None:
        if self._trap_retry_scheduled:
            return
        self._trap_retry_scheduled = True

        async def retry() -> None:
            await asyncio.sleep(5)
            self._trap_retry_scheduled = False
            await self.flush_traps()

        asyncio.create_task(retry(), name="Armada trap retry")

    async def flush_traps(self) -> None:
        """Apply one durable received trap when the confirmed Fed1 adapter is live."""
        pending = self.ledger.pending_traps()
        if not pending:
            return
        item_index, entry = pending[0]
        try:
            acknowledgement = await asyncio.to_thread(
                send_control_request, f"apply_trap {entry['command']}", 1.0
            )
        except (OSError, TimeoutError):
            # No live observer is normal while a player is in the launcher.
            return
        if acknowledgement.startswith("dispatched nebula trap"):
            self.ledger.mark_trap_dispatched(item_index)
            logger.info("Applied received Armada trap index=%s: %s", item_index, entry["item_name"])
            if self.ledger.pending_traps():
                self._schedule_trap_retry()
        elif acknowledgement.startswith("busy nebula trap"):
            self._schedule_trap_retry()
        else:
            # Unsupported maps and pre-objective startup remain pending.  The
            # observer event loop retries after the next live objective.
            logger.info("Holding Armada trap index=%s: %s", item_index, acknowledgement or "no acknowledgement")

    def missing_finale_items(self) -> list[str]:
        if not self.finale_required_items:
            return []
        received_names = set()
        for item in self.items_received:
            try:
                received_names.add(self.item_names.lookup_in_game(item.item, self.game))
            except KeyError:
                continue
        return [name for name in self.finale_required_items if name not in received_names]

    def received_item_names(self) -> set[str]:
        names = set()
        for item in self.items_received:
            try:
                names.add(self.item_names.lookup_in_game(item.item, self.game))
            except KeyError:
                continue
        return names

    def mission_lock_reasons(self, module: str, received: set[str] | None = None) -> list[str]:
        """Return the missing access items for an observer module.

        This is deliberately shared by the launcher and observer paths.  The
        launcher is a convenience layer; an externally launched locked mission
        must never be able to create a deferred Archipelago check.
        """
        data = self.missions.get(module.lower())
        if not data:
            return ["mission is not available in this connected slot"]
        received = self.received_item_names() if received is None else received
        faction, number = str(data["faction"]), int(data["number"])
        access = f"{faction} Mission {number} Access"
        if faction == "Finale":
            required = self.finale_mission_requirements.get(number, [access, *self.finale_required_items])
        else:
            required = [access, self.faction_access_items.get(faction, "")]
        missing = [name for name in required if name and name not in received]
        if faction == "Finale" and number == 4:
            completed = self.ledger.completed_mission_count(self.goal_location)
            remaining = self.finale_mission_completion_requirement - completed
            if remaining > 0:
                missing.append(f"{remaining} more campaign mission completion{'s' if remaining != 1 else ''}")
        return missing

    def mission_snapshot(self) -> dict:
        if not self.server or not self.slot:
            return {"status": "Not connected.", "missions": []}
        rows = []
        faction_order = {"Federation": 0, "Klingon": 1, "Romulan": 2, "Borg": 3, "Finale": 4}
        for module, data in sorted(self.missions.items(), key=lambda pair: (faction_order.get(pair[1]["faction"], 9), pair[1]["number"])):
            faction, number = str(data["faction"]), int(data["number"])
            missing = self.mission_lock_reasons(module)
            location = f"{faction} Mission {number} Complete"
            rows.append({"module": module, "label": f"{faction} {number}: {data['title']}",
                         "available": not missing, "status": "Available" if not missing else "Locked",
                         "requires": "Ready" if not missing else ", ".join(missing), "location": location})
        return {"status": f"Connected as {self.auth or self.username or 'slot'}.", "missions": rows}

    async def flush_pending(self) -> None:
        if not self.server:
            return
        pending = self.ledger.pending()
        if not pending:
            return
        event_keys: list[str] = []
        location_ids: list[int] = []
        for event_key, location_name in pending:
            if location_name == self.goal_location:
                missing_items = self.missing_finale_items()
                if missing_items:
                    logger.info("Holding final mission check: waiting for %s.", ", ".join(missing_items))
                    continue
            if (location_name == self.goal_location and
                    self.ledger.completed_mission_count(self.goal_location) < self.finale_mission_completion_requirement):
                logger.info("Holding final mission check: %s/%s other campaign missions complete.",
                            self.ledger.completed_mission_count(self.goal_location),
                            self.finale_mission_completion_requirement)
                continue
            try:
                # Keep submission independent of the optional Universal
                # Tracker NameLookupDict wrapper, which only supports ID to
                # name lookup. Armada location IDs are versioned constants in
                # this APWorld's data package.
                location_ids.append(location_name_to_id[location_name])
                event_keys.append(event_key)
            except KeyError:
                logger.error("Server does not expose configured Armada location: %s", location_name)
        if location_ids:
            await self.send_msgs([{"cmd": "LocationChecks", "locations": location_ids}])
            self.ledger.mark_submitted(event_keys)
            logger.info("Submitted Archipelago checks: %s", ", ".join(event_keys))


async def observe(ctx: ArmadaContext) -> None:
    events: asyncio.Queue[dict] = asyncio.Queue()
    loop = asyncio.get_running_loop()

    def read_pipe() -> None:
        try:
            for event in pipe_events():
                loop.call_soon_threadsafe(events.put_nowait, event)
        except Exception:
            logger.exception("Armada observer pipe reader stopped unexpectedly.")

    threading.Thread(target=read_pipe, name="Armada observer pipe", daemon=True).start()
    while True:
        event = await events.get()
        module = Path(str(event.get("caller_module", ""))).name.lower()
        if event.get("type") == "objective_display":
            # A live objective establishes the observer's script-thread
            # dispatcher. This is a client retry signal only; the observer
            # executes a received trap immediately when its control message
            # arrives, rather than tying it to an objective transition.
            await ctx.flush_traps()
            missing = ctx.mission_lock_reasons(module)
            if missing:
                logger.info("Ignored objective transition from locked %s: missing %s.", module, ", ".join(missing))
                continue
            objective = Path(str(event.get("objective_file", ""))).name.lower()
            location = ctx.objective_locations.get(f"{module}|{objective}")
            kind = "objective"
        elif event.get("type") == "mission_result":
            if event.get("result") == "success":
                missing = ctx.mission_lock_reasons(module)
                if missing:
                    logger.info("Ignored success from locked %s: missing %s.", module, ", ".join(missing))
                else:
                    # A campaign success proves that its configured objectives
                    # have been met, even if an objective's display transition
                    # was skipped by the mission state.  Reconcile those
                    # missing checks first, so a later mission completion can
                    # never strand an objective check locally.
                    mission_objectives = sorted(
                        ((event_key, objective_location)
                         for event_key, objective_location in ctx.objective_locations.items()
                         if event_key.startswith(f"{module}|")),
                        key=lambda pair: location_name_to_id.get(pair[1], -1),
                    )
                    for event_key, objective_location in mission_objectives:
                        objective_file = event_key.partition("|")[2]
                        if ctx.ledger.record_reconciled_objective(
                                event, module, objective_location, objective_file):
                            logger.info("Reconciled Armada objective check from mission success: %s", objective_location)
                    location = ctx.mission_locations.get(module)
                    kind = "mission"
                    if ctx.ledger.record_check(event, location, kind):
                        logger.info("Recorded Armada check: %s", location)
                    await ctx.flush_pending()
            else:
                logger.info("Observed Armada mission failure; no Archipelago check sent.")
            await asyncio.to_thread(stop_armada_process, event.get("pid"))
            continue
        else:
            continue
        if not ctx.ledger.record_check(event, location, kind):
            continue
        logger.info("Recorded Armada check: %s", location)
        await ctx.flush_pending()


def find_armada_pid() -> int:
    result = run_hidden(["tasklist", "/fi", "IMAGENAME eq Armada.exe", "/fo", "csv", "/nh"])
    for row in result.stdout.splitlines():
        columns = [value.strip('"') for value in row.split('","')]
        if len(columns) > 1 and columns[0].casefold() == "armada.exe":
            return int(columns[1])
    return 0


def stop_armada_process(pid: object) -> None:
    """End only the completed mission's Armada process before the next selection."""
    try:
        target = int(pid)
    except (TypeError, ValueError):
        target = find_armada_pid()
    if not target:
        return
    result = run_hidden(["taskkill", "/PID", str(target), "/T", "/F"])
    if result.returncode == 0:
        logger.info("Closed Armada after mission result (PID %s).", target)
    else:
        logger.warning("Could not close Armada PID %s after mission result: %s", target, result.stderr.strip())


def start_armada_and_observer(game_root: Path, launch_map: str | None = None) -> bool:
    game = game_root / "Armada.exe"
    injector = game_root / "armada_injector.exe"
    observer = game_root / "armada_observer.dll"
    for path in (game, injector, observer):
        if not path.is_file():
            raise RuntimeError(
                f"Required Armada file is missing: {path}. Copy armada_observer.dll and "
                "armada_injector.exe from the Armada APWorld release into the folder containing Armada.exe."
            )
    pid = find_armada_pid()
    started = False
    if not pid:
        started = True
        logger.info("Starting Armada with the observer preloaded...")
        command = [str(injector), str(observer), "--launch", str(game)]
        if launch_map:
            command.extend(["--map", launch_map])
        result = run_hidden(command)
        if result.returncode:
            detail = result.stderr.strip() or result.stdout.strip()
            raise RuntimeError(f"Armada startup/injection failed with exit code {result.returncode}: {detail}")
        deadline = time.monotonic() + 30
        while not pid and time.monotonic() < deadline:
            time.sleep(0.25)
            pid = find_armada_pid()
    if not pid:
        raise RuntimeError("Armada.exe did not start within 30 seconds.")
    if find_armada_pid() != pid:
        raise RuntimeError("Armada.exe exited before the observer could be used.")
    # --launch injects before ResumeThread. Do not snapshot/inject again while
    # Armada is still executing the startup campaign picker; the second pass
    # provides no coverage and races its UI-thread handoff.
    if started:
        return True
    result = run_hidden([str(injector), str(observer), "--pid", str(pid), "--if-needed"])
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"Observer injection failed with exit code {result.returncode}: {detail}")
    return False


def default_ledger_path() -> Path:
    """Return the per-user persistent state path for the packaged client."""
    local_app_data = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return local_app_data / "Archipelago" / "StarTrekArmada" / "armada-client.json"


def client_data_directory() -> Path:
    """Return the private writable directory used by the packaged client."""
    local_app_data = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return local_app_data / "Archipelago" / "StarTrekArmada"


def client_settings_path() -> Path:
    return client_data_directory() / "settings.json"


def load_client_settings() -> dict[str, object]:
    try:
        settings = json.loads(client_settings_path().read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return {}
    return settings if isinstance(settings, dict) else {}


def save_client_settings(settings: dict[str, object]) -> None:
    path = client_settings_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(settings, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def configured_game_root(explicit: Path | None = None) -> Path | None:
    """Return a valid configured Armada installation, if one is known."""
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    legacy_environment = os.environ.get("STAR_TREK_ARMADA_GAME_ROOT")
    if legacy_environment:
        candidates.append(Path(legacy_environment))
    saved = load_client_settings().get("game_root")
    if isinstance(saved, str):
        candidates.append(Path(saved))
    for candidate in candidates:
        candidate = candidate.expanduser()
        if (candidate / "Armada.exe").is_file():
            return candidate
    return None


def prompt_for_game_root() -> Path | None:
    """Ask once for the retail install folder using Windows' standard picker."""
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox
    except ImportError:
        logger.error("Unable to open the Armada folder picker; start with --game-root instead.")
        return None
    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    try:
        while True:
            selection = filedialog.askdirectory(
                parent=root,
                title="Select the Star Trek: Armada installation folder",
            )
            if not selection:
                return None
            game_root = Path(selection)
            if (game_root / "Armada.exe").is_file():
                settings = load_client_settings()
                settings["game_root"] = str(game_root)
                save_client_settings(settings)
                logger.info("Saved Star Trek: Armada installation folder: %s", game_root)
                return game_root
            messagebox.showerror(
                "Armada.exe not found",
                "Select the folder that contains Armada.exe.",
                parent=root,
            )
    finally:
        root.destroy()


async def run(connect: str | None, password: str | None, name: str | None,
              game_root: Path | None) -> None:
    """Run as a normal Archipelago client with an embedded mission-launcher tab."""
    ledger = Ledger(default_ledger_path())
    try:
        ctx = ArmadaContext(connect, password, ledger)
        ctx.username = name
        ctx.game_root = game_root
        ctx.server_task = asyncio.create_task(server_loop(ctx), name="server loop")
        if UNIVERSAL_TRACKER_AVAILABLE:
            ctx.run_generator()
        if gui_enabled:
            ctx.run_gui()
        ctx.run_cli()
        observer = asyncio.create_task(observe(ctx), name="Armada observer")
        await ctx.exit_event.wait()
        observer.cancel()
        await asyncio.gather(observer, return_exceptions=True)
        await ctx.shutdown()
    finally:
        ledger.close()


def launch(*args: str) -> None:
    Utils.init_logging("StarTrekArmadaClient")
    parser = get_base_parser()
    parser.add_argument("--name", default=None, help="Slot name to connect as.")
    parser.add_argument("--game-root", type=Path, default=None,
                        help="override the saved folder that contains Armada.exe")
    parser.add_argument("--diagnose", action="store_true", help="validate client startup paths without launching Armada")
    parser.add_argument("url", nargs="?", help="Archipelago connection URL")
    parsed = handle_url_arg(parser.parse_args(args), parser=parser)
    game_root = configured_game_root(parsed.game_root)
    if parsed.diagnose:
        logger.info("Armada client game_root=%s", game_root)
        return
    if game_root is None and gui_enabled:
        game_root = prompt_for_game_root()
    asyncio.run(run(parsed.connect, parsed.password, parsed.name, game_root))
