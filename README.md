# Doom for Playdate (doomgeneric port)

Playdate platform layer for [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) (cloned in `doomgeneric/`).

## Build

Needs the Playdate SDK (`PLAYDATE_SDK_PATH`), CMake, and for the device `arm-none-eabi-gcc`.

    ./build.sh sim      # -> doom.pdx (simulator)
    ./build.sh device   # -> doom_DEVICE.pdx (hardware)

## Release build

    ./scripts/build-release.sh [both|device|sim]   # -> release/doom.pdx

(VS Code task: *Release: shareware/free WADs only*.) Builds the game in Release and packages a
`.pdx` that bundles **only** the WADs listed in `scripts/release-wads.txt` (Doom shareware and
Freedoom by default; whichever of them are in `Source/wad/`), plus music rendered from just those
WADs, so none of the other WADs' content ends up in it. The default `both` makes one `.pdx` that
runs on the device and in the simulator. Edit `scripts/release-wads.txt` to change what is allowed.

## WAD

Copy one or more WADs into `Source/wad/` **before building** (pdc bundles them into the .pdx).
Any `*.wad` file works (standard names like `doom1.wad`/`doom2.wad` or your own); Doom detects E1M1 (Doom 1) vs MAP01 (Doom 2) games by contents. Heretic/Hexen/Strife/`voices.wad` are skipped as unsupported. Each WAD adds to the .pdx size, so bundle only what you want.
With more than one present, a Cover Flow "Select WAD" screen appears at start (D-pad or crank to browse, A to start; each cover is the WAD's own title picture); with one it boots straight in.

## Controls

| Input | Gameplay | Menus / intermission |
|---|---|---|
| D-pad up/down | forward / back | move |
| D-pad left/right | turn (crank extended: strafe) | change value |
| Crank | turn | – |
| A | fire | Enter (Yes on prompts) |
| B | use / open (on release) | Back (No on prompts) |
| Hold B + left/right | previous / next weapon | – |

System menu: **Doom menu** (Esc), **Automap** (Tab). Music/sfx volume are in Doom's own Options > Sound Volume. (Always-run is permanently on.)

## Notes

- 320x240 frame is ordered-dithered to 1-bit and centred on the 400x240 screen (fills the full height; only side bars remain).
- Zone heap is 4 MiB and low detail is the default (`CMakeLists.txt`) to fit/run on device.
- Sound effects are mixed in software (`src/i_playdate_sound.c`). Music is pre-rendered at build time: `tools/musrender.c` renders every music lump (MUS/MIDI) in `Source/wad/*.wad` with a small built-in synth to IMA-ADPCM WAVs in `Source/music/` (named by a hash of the lump, cached between builds), pdc converts them to `.pda`, and the game streams them with a `FilePlayer` (`src/i_playdate_sound.c`). Config (`.cfg`) is not loaded/saved; savegames go to the game's Data folder, one subfolder per WAD (`.savegame/doom1/`, `.savegame/doom2/`, ...).
- Patches to upstream doomgeneric are small: `i_system.c` (RAM sizes overridable), `m_config.c` (savegame folder per WAD),
  `m_menu.c` (default detail overridable), `g_game.c` (mouse motion accumulates so the
  crank isn't dropped between tics), `doomgeneric.c` (screen buffer sized by `pixel_t`),
  `i_video.h`/`r_main.c`/`d_main.c`/`st_stuff.c` (`SCREENHEIGHT` overridable to 240,
  with the status bar's absolute Y coordinates and the view-height formula made
  relative to it instead of hardcoded to the vanilla 200-tall screen),
  `v_video.c`/`wi_stuff.c`/`f_finale.c`/`d_main.c`/`m_menu.c` (the 320x200 title,
  credit/help, intermission and finale backdrops are stretched to `SCREENHEIGHT`,
  and map nodes/animations/cast sprite positioned on them are scaled to match).
