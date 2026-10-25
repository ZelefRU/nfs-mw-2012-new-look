# NewLook

by **Zelef** · https://github.com/ZelefRU/nfs-mw-2012-new-look

A lighting and atmosphere overhaul for **Need for Speed: Most Wanted (2012)**, PC, built as a native ASI plugin
(D3D11 hooks), with no ReShade. It re-lights the game's own frame: sun shadows, ambient occlusion, bounce light,
reflections, wet streets, volumetric clouds, street-lamp lights and haze, all tunable live from an in-game menu.

[Русская версия](README.ru.md) · [Changelog](CHANGELOG.md) · [Known issues / TODO](TODO.md) · [Dev log](docs/DEVLOG.md)

> Version **1.96h** (formerly **NFSLight**). Work in progress: it runs and looks good on the author's PC; expect rough edges (see
> [Known issues](#known-issues)).

## Features

**Sun and shadows**
- Its own sun shadow maps in 4 cascades instead of the game's: soft shadows whose penumbra grows with the distance
  to the caster (sharp at a wall's foot, soft at the end of a tower's shadow).
- Far cascades are kept and follow the sun; a height map of the city keeps buildings casting even when the game
  stops drawing them for its own shadows (behind the camera, far away).
- The sun and moon follow a clock: sunrise and sunset times, the sun's arc, a soft dawn and dusk, moon shadows.

**Light**
- Screen-space ambient occlusion and sky occlusion, smoothed over frames.
- Contact shadows and small "relief" shadows from the materials' normal maps.
- One-bounce indirect light: lit surfaces light their surroundings, from the previous frame's image.
- **Street lamps as real lights**: a map of about 4000 lamps over the city, with shadows. Lamps go out when a post is
  knocked down (most of them; see Known issues). Lamp groups and light zones are editable in game.
- Car lights: the player's headlight beams in the air, with shadows; traffic headlights and tail lights light the
  street.

**Reflections and wet streets**
- Screen-space reflections with temporal anti-aliasing, the sky (and the clouds) in them when the ray leaves the
  screen.
- A clear coat on car paint.
- Wet ground from a rain map seen from above (dry under roofs and bridges), small mirror puddles, big matte puddles,
  a "wetness" control from damp patches to standing water.

**Sky and air**
- Volumetric clouds (after Guerrilla's Nubis) with their shadows on the city; presets from clear to storm.
- Haze that follows the time of day, sun shafts, halos round lamps.

**Look**
- Colour grading presets, sharpening (CAS-like), a "photo" look.
- Parallax on roads, pavements and building walls (heights made from the normal maps), texture replacement.

**Rain, weather, wind** (experimental)
- Rain drops on the player's car, simulated on its shape: they land, merge, run down and blow back with speed.
- Wind in the trees' leaves.
- Weather (rain by hand or by itself, weather kinds, falling rain, splashes, ripples, rain on the lens): in the menu's
  WEATHER section, **off** in the release while it is being finished.

**Tools**
- An in-game menu: a simple view (14 sections, each on / off with its main settings) and an advanced one (every
  setting, live, saved to `NewLook.ini`); quality presets Low / Medium / High / Ultra; 30+ debug views.
- A built-in benchmark (Ctrl+B, 2-3 min, day and night): each section's cost in ms and its share of the mod's cost,
  in the menu and in `scripts\NewLook_bench_report.txt`.
- **World editor** (Ctrl+J): hide, move, turn, scale and copy the game's own objects; props can be baked into the
  game's track files (`worldtools/`), and the city can be edited in Blender (see `worldtools/README.md`).
- A shader cache: the mod switches on at once after the first start.

## Requirements
- Need for Speed: Most Wanted (2012), PC (tested: the Steam version, `NFS13.exe`).
- An ASI loader: [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (included in the release
  as `dinput8.dll`; many NFS MW 2012 mods ship the same one).
- A D3D11 GPU. Measured on an RTX 3080 at 2560x1440: the mod costs about 8 ms of GPU time a frame by day, ~15 ms at
  night with many lamps. Every section can be switched off in the menu; Ctrl+B shows what costs what on your PC.
- Keep the game's own anti-aliasing off: it renders the frame at 2x, so every effect costs twice as much.

## Install
1. Copy the release's contents (`dinput8.dll` and the `scripts` folder) into the game folder, next to `NFS13.exe`.
   If a `dinput8.dll` is already there from another mod, it is most likely the same loader: copy only `scripts`.
2. Start the game. The first start compiles the shaders in the background (~10 s, during loading).
3. **P** shows / hides the menu, **Y** switches the mod on / off (to compare with the original).

Updating from NFSLight (1.63 or older): copy the new files. At its first start NewLook renames your settings, lamp
map and edits (`scripts\NFSLight*`) to `NewLook*`. If the old `NFSLight.asi` is still there, it is renamed `.old` and
this start does nothing: restart the game once.

Uninstall: delete `scripts\NewLook*` (and `dinput8.dll` if nothing else uses it).

### With ReShade
Install ReShade for "DirectX 10/11/12" (it takes `dxgi.dll`); do not give it the name `dinput8.dll`. NewLook hooks
the real system D3D11 / DXGI, not ReShade's proxy.

## Keys
| Key | |
|---|---|
| Y | mod on / off |
| P / Shift+P | menu show / hide / reload the shaders (`NewLook.hlsl`) |
| I / O, Shift+I / O | select a setting / a section |
| K / L | change the value; J: back to default; H: save all |
| U | the list of debug views (I / O choose) |
| Ctrl+B / Shift+B / B | benchmark (every section, day / night) / the selected setting / stop |
| Ctrl+J | world editor (Ctrl+D copy, Del delete, Ctrl+Z / Ctrl+Y undo / redo) |

## Files (in `scripts`)
- `NewLook.asi`, `NewLook.hlsl`: the mod and its shaders (the HLSL is compiled at run time; edit and Shift+P).
- `NewLook.ini`: the settings.
- `NewLook_lamps.txt`, `_zones`, `_lamp_presets`, `_lamp_templates`: the lamp map and zones.
- `NewLook_carlights.txt`: light positions for traffic car models.
- Written by the mod: `NewLook.log` (send it with a bug report), `NewLook_bench_report.txt` (the benchmark),
  `NewLook_cache\` (compiled shaders), `NewLook_world.txt` (world editor edits).

## Known issues
See [TODO.md](TODO.md). The main ones: far buildings' shadows can still pop or come in ragged where the shadow
cascades meet; night cost with many lamps; the weather is not finished (off by default).

## Building from source
- Visual Studio 2022 Build Tools (x86, C++). `src\build.bat` builds `build\NewLook.asi` (one translation unit:
  `newlook.cpp` includes the `*.inc` files).
- `src\NewLook.hlsl` goes next to the ASI; it is compiled at run time with `d3dcompiler_47.dll`.
- `worldtools\` are Python 3 scripts (no extra packages) for the game's Bundle 2 track files.

## How it works (short)
The plugin hooks the game's D3D11 device and context: it records the game's draws (G-buffer, its shadow passes, the
sky, the deferred sun pass), replaces the sun pass with its own lighting, re-draws the game's shadow casters into its
own cascades, and adds its passes (half-resolution AO / bounce / shafts, reflections, clouds, air) before the game's
post effects. Details, every finding about the renderer and every mistake on the way: [docs/DEVLOG.md](docs/DEVLOG.md).

## Credits
- Ultimate ASI Loader by ThirteenAG (MIT).
- Track-file formats: the Burnout / NFS community's documentation (burnout.wiki); the Blender workflow uses DGIorio's
  NFS Most Wanted 2012 import / export add-ons.
- Techniques after published work: Guerrilla Games' Nubis clouds, AMD FidelityFX CAS (sharpening), PCSS-style soft
  shadows, screen-space reflections / temporal accumulation.
- **AI disclosure:** the code was written by Claude Code (Anthropic, Claude Opus 5.5) working with the author (Zelef), who set
  the goals, tested every version in game and judged the results.

## License
MIT (see [LICENSE](LICENSE)). No game files are included; Need for Speed is a trademark of Electronic Arts.
