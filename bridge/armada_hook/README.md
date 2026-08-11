# Armada observer

This x86 DLL is the local observer used by the **Star Trek: Armada Client**.
It observes supported mission-result and objective events from the player's
local Armada process, then relays them to the client over a named pipe.

The DLL is original project code. It does not contain or distribute any Armada
game asset, executable, or DLL, and it does not modify files in the game
installation.

## Build

From the repository root, build the observer and injector with a 32-bit Visual
Studio generator:

```powershell
$vsCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $vsCMake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DARMADA_BUILD_OBSERVER=ON -DARMADA_BUILD_INJECTOR=ON
& $vsCMake --build build --config Release
```

Armada is a 32-bit process, so both artifacts must be built for Win32. The
build writes them to the repository's `bin/` folder, where the Archipelago
client locates and loads the matching observer when launching an unlocked
mission.
