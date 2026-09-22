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

System menu: **Doom menu** (Esc), **Automap** (Tab), **Music** toggle. (Always-run is permanently on - Playdate caps the system menu at 3 items.)

## Notes

- 320x200 frame is ordered-dithered to 1-bit and centred on the 400x240 screen.
- Zone heap is 4 MiB and low detail is the default (`CMakeLists.txt`) to fit/run on device.
- Sound effects are mixed in software (`src/i_playdate_sound.c`). Music is converted MUS->MIDI (`mus2mid`), parsed into a flat note list, and scheduled directly onto plain `PDSynth` voices (percussion dropped; toggle in the system menu) - `PDSynthInstrument`/`SoundSequence`/`loadMIDIFile()` all turned out to be non-functional on-device (SDK/firmware 3.1.2), so music bypasses them entirely; see the comment above `PD_RegisterSong` in `src/i_playdate_sound.c`. Config (`.cfg`) is not loaded/saved; savegames go to the game's Data folder.
- Patches to upstream doomgeneric are small: `i_system.c` (RAM sizes overridable),
  `m_menu.c` (default detail overridable), `g_game.c` (mouse motion accumulates so the
  crank isn't dropped between tics), `doomgeneric.c` (screen buffer sized by `pixel_t`).
