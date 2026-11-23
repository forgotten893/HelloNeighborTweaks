# Hello Neighbor Tweaks

A graphics mod for **Hello Neighbor** (PC, Unreal Engine 4.20, 64-bit Windows). It adds real-time lighting and
shadows, reflections, ambient occlusion, volumetric fog and an in-game settings menu, without changing any game files.

Version 1.0.0 by forgotten893. MIT License. The full source code is included.

**AI-assisted coding.** The code was written with an AI assistant (Claude by Anthropic). All in-game testing was
done by forgotten893.

> Not affiliated with or endorsed by tinyBuild or Dynamic Pixels. Hello Neighbor is a trademark of its owners.

---

## What it does

- **Real-time lighting.** The game's baked (static and stationary) lamps become real-time lights that cast
  real-time shadows. Part of the original baked light is kept as fill (60% by default), so dark rooms and basement
  doors stay visible before you have the flashlight.
- **Reflections.** Screen-space reflections are switched off in the shipped maps. The mod turns them back on, at
  higher quality.
- **Ambient occlusion.** The maps ask for almost none. The mod uses Unreal's normal values.
- **Sun shadows.** 4 shadow cascades instead of 1, a longer shadow distance, sharper shadows and contact shadows.
- **Lamp shadows.** Higher resolution, and they reach further.
- **Volumetric fog** in the outdoor maps that have height fog.
- **Eye adaptation.** You can tune how dark and how bright the auto-exposure goes, and how fast.
- **Quality presets.** Low, Medium, High and Ultra. Real-time lighting stays on in every preset. Each preset sets
  render resolution, anti-aliasing, view distance, shadow and texture quality. The default is **High**.
- **Display options.** Windowed, borderless or exclusive fullscreen, V-Sync, a frame rate limiter and field of view.

Everything is changed in game with the **F1** menu. Most settings take effect immediately.

<!-- installer-only -->
## Downloads

| File | For |
|------|-----|
| `HelloNeighborTweaks-1.0.0-Installer.zip` | Most people. Contains `HelloNeighborTweaks-1.0.0-Setup.exe`, which finds the game and installs the mod for you. |
| `HelloNeighborTweaks-1.0.0-Portable.zip`  | Manual install. Contains `XINPUT1_3.dll`, which you copy into the game folder yourself. |
| `SHA256SUMS.txt` | Checksums of everything above (see [Verifying a download](#verifying-a-download)). |

Both zips contain this readme, the license, `CHECKSUMS.txt`, `BUILD-INFO.txt` and the complete source code
(`source\`). The source covers the mod and the installer, and is the same in both zips.

<!-- /installer-only -->
<!-- portable-only
This is the portable version: `XINPUT1_3.dll`, this readme, the license, `CHECKSUMS.txt`, `BUILD-INFO.txt` and
the complete source code of the DLL (`source\`).

-->
## Install

<!-- installer-only -->
### Installer

1. Close the game.
2. Extract `HelloNeighborTweaks-1.0.0-Installer.zip` and run `HelloNeighborTweaks-1.0.0-Setup.exe`.
3. It finds Hello Neighbor automatically: Steam libraries, Epic Games, GOG and common folders. If it doesn't, click
   **Browse...** and pick the game folder. On Steam you can find it with right-click Hello Neighbor > Manage >
   Browse local files.
4. Click **Install**, then start the game and press **F1**.

The installer only runs with normal user rights and never asks for administrator rights. If the game is in a
folder your user account can't write to (for example under `C:\Program Files`), it tells you so and recommends
the portable zip instead.

The installer is not code-signed, because signing certificates cost money, so Windows SmartScreen may warn you.
Click *More info* > *Run anyway*. You can check exactly what it does below, or build it yourself from the source.

<!-- /installer-only -->
### Portable (manual)

Copy `XINPUT1_3.dll` from the portable zip into the game's Win64 folder, next to
`HelloNeighbor-Win64-Shipping.exe`:

```
...\steamapps\common\Hello Neighbor\HelloNeighbor\Binaries\Win64\
```

That is all you need. The settings file is created automatically on the first start.

## Uninstall

<!-- installer-only -->
- **Installer:** run it again and click **Uninstall**. You can choose to keep your settings or remove them.
<!-- /installer-only -->
- **Manual:** delete `XINPUT1_3.dll` from the Win64 folder. You can also delete `HelloNeighborTweaks.ini` and
  `HelloNeighborTweaks.log`.

## Using it

Press **F1** in game for the settings menu. It is the only key the mod uses.

- Up/Down: select.
- Right or Enter: open a category.
- Left or Enter: close a category.
- Left/Right on a setting: change it. Hold Shift for bigger steps.
- F1: close the menu.

While the menu is open, the arrow keys and Enter go to the menu, not the game. The key can be changed in the
settings file (`[Hotkeys] ToggleMenu`).

**The menu can't be drawn over exclusive fullscreen.** To leave exclusive fullscreen, press **Alt+Enter**, or set
`Mode=Borderless` in `HelloNeighborTweaks.ini`.

Some settings apply on the next level load rather than instantly: the lighting conversion, sun shadow cascades
and turning volumetric fog on. The menu says which. Anisotropic filtering and mouse smoothing need a game restart.

### Settings file

`HelloNeighborTweaks.ini` is created next to the DLL the first time the game starts. Every key is commented in the
file. The game re-reads it every 5 seconds, so you can also edit it while playing. `-1` means "keep the game's or
the map's own value". To get the defaults back, delete the file.

Defaults: High preset, real-time lighting with 60% baked fill, reflections and AO on, volumetric fog at double
density, and motion blur, depth of field and vignette off. Display mode, V-Sync, frame rate limit, FOV and mouse
smoothing are left at the game's own settings until you change them.

`HelloNeighborTweaks.log` (rewritten every 5 seconds) shows which hooks were installed and the values currently
applied. It's useful when reporting a problem. It contains only game and mod settings, nothing personal.

## Exactly what it does on your PC

Nothing is hidden. Everything below can be checked in the source code.

### The mod (`XINPUT1_3.dll`)

- **How it loads.** The game loads this file from its own folder instead of the system's XInput 1.3 (controller
  support). Every controller call is passed on, unchanged, to the real `C:\Windows\System32\XInput1_3.dll`.
- **Where it runs.** It only does something inside `HelloNeighbor-Win64-Shipping.exe`. If any other program loads
  it, it writes one line to the log and does nothing else.
- **What it changes in memory.** The game's files are never modified. In the running game's memory it changes:
  - one 5-byte instruction, which unlocks the reflection quality limit;
  - six small code hooks: post-process settings, sun shadow cascades, the AO pass check, camera FOV, fog setup, and
    the light setup that makes baked lamps real-time;
  - engine console variables, for the quality and display settings.

  All of it is gone when the game closes.
- **To find the frame counter,** it creates a hidden 64x64 Direct3D 11 window once at startup. It also draws the
  menu in its own click-through window over the game.
- **Keyboard.** It reads the key presses sent to the game window, so it can react to F1 and, while the menu is
  open, the arrow keys and Enter. Nothing is recorded or stored.
- **Files it writes:**
  - `HelloNeighborTweaks.ini` and `HelloNeighborTweaks.log`, next to the DLL.
  - Only if you change those settings: `r.MaxAnisotropy` in `Engine.ini` and `bEnableMouseSmoothing` in
    `Input.ini`, both in `%LOCALAPPDATA%\HelloNeighbor\Saved\Config\WindowsNoEditor\`. The old value is remembered
    (`[State]` in the settings file) and put back when you return the setting to "game setting".
- **Files it reads:** its own settings file, plus `Engine.ini` and `GameUserSettings.ini` from the folder above.
- **Display mode.** If you choose a display mode in the menu, the game itself may save the new resolution and
  window mode in its `GameUserSettings.ini`, the same as when you change it in the game's own options.
- **No network access.** The DLL imports only `kernel32`, `user32` and `gdi32`, and loads `d3d11.dll` once (see
  above). It contains no networking code, collects nothing, sends nothing and never updates itself.
- **No administrator rights.**

<!-- installer-only -->
### The installer (`HelloNeighborTweaks-1.0.0-Setup.exe`)

- **To find the game, it only reads (never writes):**
  - these registry values:
    - `HKCU\Software\Valve\Steam\SteamPath`
    - `HKLM\SOFTWARE\Valve\Steam\InstallPath`
    - `HKLM\...\Uninstall\Steam App 521890\InstallLocation`
    - `HKLM\SOFTWARE\GOG.com\Games\*\path`
    - the `InstallLocation` of installed programs (`HKLM` and `HKCU` `...\CurrentVersion\Uninstall`)
  - Steam's `libraryfolders.vdf` and Epic's `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item`;
  - a short list of common game folders on each fixed drive.

  A folder only counts if it contains `HelloNeighbor\Binaries\Win64\HelloNeighbor-Win64-Shipping.exe`.
- **It checks the running processes** so it never replaces the DLL while the game is open.
- **Install writes into the game's Win64 folder only:**
  - `XINPUT1_3.dll`
  - `HelloNeighborTweaks_README.txt`
  - `HelloNeighborTweaks_LICENSE.txt`
  - `HelloNeighborTweaks_Source.zip`

  Before that, it writes a temporary file and deletes it at once, to test that it may write there. If another
  `XINPUT1_3.dll` is already there, it is renamed to `XINPUT1_3.dll.tweaks-backup`, not deleted.
- **Uninstall** removes those files, puts the backup back, reverts the two config lines described above and, if
  you choose, deletes the settings file.
- **"Read me"** writes the readme to your `%TEMP%` folder and opens it.
- **No network access, no registry writes, no administrator rights.** It also installs nothing else: no Start menu
  entry, no shortcut and no "Programs and Features" entry.

<!-- /installer-only -->
### Verifying a download

<!-- installer-only -->
`SHA256SUMS.txt` on the release page lists the SHA-256 of both zips and of the DLL and Setup.exe inside them.
Inside each zip, `CHECKSUMS.txt` lists the same hashes for the DLL and Setup.exe. To check a file, open
PowerShell and run:
<!-- /installer-only -->
<!-- portable-only
`CHECKSUMS.txt` lists the SHA-256 of `XINPUT1_3.dll`; `SHA256SUMS.txt` on the release page lists it too.
To check it, open PowerShell in the folder and run:
-->

```
Get-FileHash .\XINPUT1_3.dll
```

<!-- installer-only -->
The installer copies exactly the same DLL as the portable zip, so an installed `XINPUT1_3.dll` has the same hash.

<!-- /installer-only -->
**Build it yourself and compare.** `XINPUT1_3.dll` is built deterministically (`/Brepro`). If you run
`build.bat` with the compiler version listed in `BUILD-INFO.txt`, you get a byte-identical file with the same
SHA-256. You don't have to trust the download: you can check that it is exactly this source code.

## Compatibility

- Made for and tested on the **Steam** version of Hello Neighbor (the full game, Unreal Engine 4.20, 64-bit).
- The Epic Games and GOG versions should work if they are the same build. The mod looks for code patterns
  instead of fixed addresses, and anything it can't find is skipped and listed in the log. They have not been
  tested.
- Not for Hello Neighbor 2, Hide and Seek, the Alpha builds or the Xbox / Microsoft Store version.
- Windows 10 or 11, 64-bit. The game needs `XInput1_3.dll` in `C:\Windows\System32`, which comes with the DirectX
<!-- installer-only -->
  End-User Runtime (June 2010). Steam installs it with the game. The installer warns you if it's missing.
- If another mod already uses `XINPUT1_3.dll`, the installer keeps it as `XINPUT1_3.dll.tweaks-backup` and puts it
  back when you uninstall. The two mods can't run at the same time.
<!-- /installer-only -->
<!-- portable-only
  End-User Runtime (June 2010). Steam installs it with the game.
- If another mod already uses `XINPUT1_3.dll`, keep a copy of it before replacing it. The two mods can't run at
  the same time.
-->

## Troubleshooting

- **Nothing changes in game.** Check that `XINPUT1_3.dll` is in the same folder as
  `HelloNeighbor-Win64-Shipping.exe`, not the top-level game folder. Then look at `HelloNeighborTweaks.log`.
- **The F1 menu doesn't show.** You're probably in exclusive fullscreen: press Alt+Enter.
- **Low frame rate.** Pick a lower preset in F1 > QUALITY. Ultra renders at 111% resolution with an 8 GB texture
  pool. On a GPU with less video memory, use High or lower.
- **My antivirus removed the DLL.** DLLs that load into a game like this one are often flagged by heuristics.
  Compare its SHA-256 with `SHA256SUMS.txt`, or build it yourself (above).

When reporting a problem, please attach `HelloNeighborTweaks.log` and `HelloNeighborTweaks.ini`.

## Building from source

You need Visual Studio 2019 or later, or the free *Build Tools for Visual Studio*, with the **Desktop development
with C++** workload.

<!-- installer-only -->
```
build.bat           -> build\XINPUT1_3.dll
build_release.bat   -> dist\HelloNeighborTweaks-<version>-Installer.zip, -Portable.zip and SHA256SUMS.txt
```
<!-- /installer-only -->
<!-- portable-only
```
build.bat           -> build\XINPUT1_3.dll
```
-->

The version number is in `src/version.h`.

| Path | Contents |
|------|----------|
| `src/ssr_unlock.c`  | The mod (the DLL): one C file with no dependencies. |
| `src/version.h`, `src/version.rc` | Version number and the DLL's version resource. |
<!-- installer-only -->
| `installer/setup.c` | The installer: game detection, install and uninstall. It also has a command line: `/detect`, `/install "<folder>"`, `/uninstall "<folder>" [/removesettings]`, `/quiet`. |
| `installer/setup.rc` | The installer's dialog. It embeds the DLL, this readme, the license and the source code. |
| `tools/make_readme.ps1` | Makes the two readme versions for the zips (the portable one leaves out the installer). |
<!-- /installer-only -->

For developers, there are test hotkeys used while making the mod (F5 compare with the original game, F6 to F11
live switches). They are left out of release builds. To build them in, add `/DHNT_DEBUG_HOTKEYS=1` to the `cl`
line in `build.bat`.

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 forgotten893.
