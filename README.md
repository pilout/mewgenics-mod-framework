# Mewgenics Mod Framework - loader

Source of `version.dll`, the mod loader of the [Mewgenics Mod Framework](https://www.nexusmods.com/mewgenics/mods/183) (game build 1.1.21239, Steam and repack).

It is a `version.dll` proxy: the game loads it at start, it forwards the real `version.dll` calls to the system, hooks a few game functions, loads the mods and shows the F10 mod menu.

The SDK used to write mods is not in this repository; it is downloaded from the [Nexus page](https://www.nexusmods.com/mewgenics/mods/183).

## Download
Get the compiled `version.dll` from the [latest release](../../releases/latest). The SHA-256 is in the release notes.

## Install
1. Copy `version.dll` next to `Mewgenics.exe`.
2. Create a `Mods` folder there and drop mod DLLs in it.
3. Start the game and press **F10**.

## What the loader does (1.3.0)
- **SDK mods**: `Mods\*.dll` built with the SDK (they export `MewMod_Init`), with the F10 menu (enable/disable, settings) and `Mods\config\core.cfg`.
- **Community mods**: mods written for [Mewjector](https://github.com/githubuser508/mewjector) run unchanged. The loader exposes Mewjector's API v3
  (`MJ_InstallHook`, `MJ_QueryHook`, `MJ_AllocTypeIdPair`, `MJ_RegisterName`, `MJ_LookupName`, `MJ_GetGameBase`, `MJ_Log`, `MJ_VerifyHooks`,
  `MJ_GetVersion`) from `version.dll`, reads `chainloader.ini` and the Mewtator manifest, and shares one hook chain per game function between
  the loader, SDK mods and community mods. Install it **instead of** Mewjector, not beside it (both are a `version.dll`).
- **Resource mods**: a folder `Mods\<id>\` with `data\`, `swfs\`, `textures\` or `sprites\` is added to the game's own mod overlay (the
  mechanism Mewtator uses), so data files (`*.gon.patch`, `*.gon.append`, `combined.csv.append`, ...) and SWF files load without replacing
  `resources.gpak`.
- **Pictures from PNG files**: `Mods\<id>\sprites\<Clip>\<Label>.png` is compiled at launch into an SWF that extends the game's clip
  `<Clip>` (`AbilityIcon`, `PassiveIcon`, `Furniture` are addressed by label). `@fpsN` in a name makes an animation from a folder of frames,
  `@center` moves the picture's origin. Item pictures go in `sprites\Items\<kind>\<Label>.png` (kind: `trinket`, `weapon`, `head`, `face`,
  `neck`); the game addresses them by frame number, so the loader assigns the next free numbers and replaces `@frame(kind/Label)` in
  `*.tpl` data files. Generated files are cached in `Mods\.cache\`.

`chainloader.ini` is created with defaults on first launch (`ScanPath`, `ScanGameDir`, `MewtatorManifest`, `Enabled`, `[LoadOrder] Mod1..`).
`Mods\config\core.cfg` has `agent=1` (dev console on 127.0.0.1:47800), `menu_key=F10`, `disabled=` and `resources=0` (no resource folders).
Set `MEW_TRACE_LOAD=1` in the environment to log every file the game reads while diagnosing a mod.

Not covered yet: pictures the cats wear, cat body parts and appearance, particles and projectiles, new in-game screens.

## Build
Needs Visual Studio 2022 (MSVC, x64).

```powershell
.\core\build.ps1      # produces core\build\version.dll
```

- `core/core.c`, `core/menu.c`: loader, hooks, event dispatch, mod menu.
- `core/assets.c`: PNG to SWF compiler for the `sprites` folders (uses `core/stb/stb_image.h`, public domain).
- `core/mj.c`: Mewjector API v3, hook chain, chainloader.ini and Mewtator manifest.
- `core/minhook/`: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause, see its `LICENSE.txt`).
- `agent/agent.c`: in-process helper compiled into the loader.
