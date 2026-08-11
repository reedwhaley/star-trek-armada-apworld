# Archipelago: Star Trek Armada

An in-development Archipelago integration for the retail PC game *Star Trek:
Armada*. It provides campaign checks, access-item logic, a launcher client, and
a local runtime observer that communicates mission results to Archipelago.

## Requirements and asset policy

- You must own a legally obtained retail copy of *Star Trek: Armada*.
- This repository and its releases do not include, download, or redistribute
  Armada executables, DLLs, maps, missions, archives, art, icons, music,
  video, or other original game assets.
- The client uses the player's local game installation and does not distribute
  a modified Armada executable.
- This is an unofficial fan project and is not made by, affiliated with, or
  supported by Activision, Paramount, or their licensors.

## Build

Build the APWorld archive:

```powershell
python tools/scripts/build_apworld.py
```

To build the x86 observer and injector, use a 32-bit Visual Studio CMake
generator:

```powershell
$vsCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $vsCMake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DARMADA_BUILD_OBSERVER=ON -DARMADA_BUILD_INJECTOR=ON
& $vsCMake --build build --config Release
```

The build writes `armada_observer.dll` and `armada_injector.exe` to `bin/`.

Create a distributable Windows release archive after building the binaries:

```powershell
python tools/scripts/build_release.py
```

Install the resulting `out/star_trek_armada.apworld` into Archipelago, restart
the Archipelago Launcher, and open **Star Trek: Armada Client**. See the
[setup guide](apworld/star_trek_armada/docs/setup_en.md) for client and game
configuration.
