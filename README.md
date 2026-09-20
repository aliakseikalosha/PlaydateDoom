# Doom for Playdate (doomgeneric port)

Playdate platform layer for [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric) (cloned in `doomgeneric/`).

## Build

Needs the Playdate SDK (`PLAYDATE_SDK_PATH`), CMake, and for the device `arm-none-eabi-gcc`.

    ./build.sh sim      # -> doom.pdx (simulator)
    ./build.sh device   # -> doom_DEVICE.pdx (hardware)

## WAD

Copy your IWAD to `Source/doom1.wad` **before building** (pdc bundles it into the .pdx).
The shareware `doom1.wad` works; the name is set in `src/main.c`.

## Controls

| Input | Gameplay | Menus / intermission |
|---|---|---|
| D-pad up/down | forward / back | move |
| D-pad left/right | turn (crank extended: strafe) | change value |
| Crank | turn | – |
| A | fire | Enter (Yes on prompts) |
| B | use / open (on release) | Back (No on prompts) |
| Hold B + left/right | previous / next weapon | – |

System menu: **Doom menu** (Esc), **Automap** (Tab), **Always run** toggle.

## Notes

- 320x200 frame is ordered-dithered to 1-bit and centred on the 400x240 screen.
- Zone heap is 4 MiB and low detail is the default (`CMakeLists.txt`) to fit/run on device.
- No sound. Config (`.cfg`) is not loaded/saved; savegames go to the game's Data folder.
- Patches to upstream doomgeneric are small: `i_system.c` (RAM sizes overridable),
  `m_menu.c` (default detail overridable), `g_game.c` (mouse motion accumulates so the
  crank isn't dropped between tics), `doomgeneric.c` (screen buffer sized by `pixel_t`).
