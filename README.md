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

The Windows release archive contains `star_trek_armada.apworld`,
`armada_observer.dll`, and `armada_injector.exe`. Copy the two native files
beside the player's `Armada.exe`, install the APWorld, and open **Star Trek:
Armada Client**. It asks for the Armada installation folder on first launch and
saves that selection. See the [setup guide](apworld/star_trek_armada/docs/setup_en.md).

The client can skip Armada's startup intro without a key macro or a game-code
movie hook. Its saved Mission Launcher setting temporarily renames only
`animations\STIntro.bik` while a client-launched Armada process is running and
restores it when Armada exits. Campaign and ending cinematics remain untouched.
