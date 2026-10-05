<div align="center">

<img src="icon.png" alt="SidecarTridge Multi-device Dragon's Lair" width="180" />

# Dragon's Lair for the SidecarTridge Multi-device

[![Build](https://github.com/sidecartridge/md-dragons-lair/actions/workflows/build.yml/badge.svg)](https://github.com/sidecartridge/md-dragons-lair/actions/workflows/build.yml)
[![Release](https://github.com/sidecartridge/md-dragons-lair/actions/workflows/release.yml/badge.svg)](https://github.com/sidecartridge/md-dragons-lair/actions/workflows/release.yml)
[![License: GPL v3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)

</div>

Dragon's Lair, the laserdisc game, on an Atari ST, STE, Mega ST or Mega STE with a
[SidecarTridge Multi-device](https://sidecartridge.com) in its cartridge slot. The Raspberry Pi
Pico (RP2040) in the cartridge reads the game's clips from the SD card, turns each picture into
320×200 in 16 colours of its own and serves it to the ST, which shows it at 25 frames a second;
the sound plays through the DMA sound chip on an STE or a Mega STE, through the YM2149 elsewhere.

**This is work in progress.** The firmware converts the game's clips once, for the machine it
is plugged into, and plays the game: the attract movie, a start menu with options, and the game
itself on the joystick or the keyboard, as the arcade plays it. The game's logic is
[DirkSimple](https://github.com/icculus/DirkSimple)'s. The bench, the test firmware it grew
from, is one key away in the menu.

## What you need

- Your own copy of the game's PC CD-ROM, as an ISO 9660 image: **Dragon's Lair CD-ROM
  (Version 3.1)** by Digital Leisure, the only one that works. None of the game's clips or sound
  is in this repository or in the firmware. If your own disc no longer reads after all these
  years, the Internet Archive keeps a copy for preservation: look for "Dragon's Lair CD-ROM
  (Version 3.1)" on archive.org.
- A microSD card in the Multi-device, with the image in the folder `/DLAIR` (the app creates the
  folder when it is missing). It looks for `DL_CDROM_V31.ISO`, then for any `.ISO` in the folder
  whose root holds `S01.MPG`.
- An Atari ST, STE, Mega ST or Mega STE with a colour monitor (low or medium resolution). In
  high resolution the app returns to GEM with a message.

## The first start: the game converted

At every start the app checks the converted clips on the card for the machine plugged in:
`/DLAIR/STE` for an STE or a Mega STE (4,096 colours), `/DLAIR/ST` for an ST or a Mega ST (512
colours). It converts those that are missing or were made by an older version. The first time that
is the whole game: 56 minutes on a Mega STE, 535 MB on the card (546 MB for an ST), so the card
needs that much free space. The screen shows the clip being converted, the whole game's progress
and the time left, and the ST can be left alone. Space stops it, and the next start carries on
where it stopped. When every clip is there, the game starts, and the set gets a list of its
clips, `SET.DLM`. A card with a complete set and its list needs no image: the app plays the
clips without it. With neither the image nor a set, the app says what to do, with a QR code that
opens the web page converting the image on a computer (not online yet).

## Playing

The attract movie plays first, its sound with it. Fire, Space or Return starts a game; M opens
the start menu, T the high scores, U / D change the volume. The menu also comes up by itself
after the movie.

In the game, Dirk follows the joystick in port 1 (port 0 is the mouse's) or the cursor keys,
and swings his sword with fire or Space. A move counts when it is pressed within its window: a
wrong one, or none, and Dirk dies; one pressed too early is ignored. P pauses (the scene, the
score and the lives), and Q in the pause leaves the game. U / D change the volume, Esc returns to
GEM.

The start menu, every option saved on the cartridge:

| Key | Option |
| --- | --- |
| H | oldies mode: the move that passes drawn over the picture, an outline before its window, filled while it can still be pressed |
| I | infinite lives |
| L | the lives at the start, 1 to 5 |
| O | the scenes' order: the arcade's (a scene of each row at random) or a fixed one |
| W | watch mode: the game plays itself (any key or fire: back to the menu) |
| C | a continue after game over, with a countdown |
| S | the input sounds: a blip for a move taken, a buzz for one with no window open |
| A | the timing: relaxed (a move pressed up to 250 ms after its window still passes, the default) or the arcade's |
| D | after a death: move on to another scene, as the arcade does (the scene comes back later), or retry it |
| P | the scene to start at |
| T | the high scores |
| B | the bench |

Fire, Space or Return in the menu starts a game. The arcade's own secret works too: up and left
held at the start give infinite lives. A game with a high score asks for its initials (up and
down change a letter, left and right move, fire or Return ends).

## The bench

B in the game's menu opens the bench. The screen shows the card, the folder, the machine and TOS the ST reported,
the image (its size, its fragments on the card, its volume), its root directory and the scene
clips in it. The keys:

| Key | What it does |
| --- | --- |
| Up / Down, Left / Right | scroll the listing, a line or a page |
| R | the read test: the card's speed at two SPI clocks and four read sizes, and a clip's CRC-32 |
| I | the intra pictures of the first scene clip, converted and shown (Left / Right: another clip; T: the timings; C: 512 or 4,096 colours; D: the dither; W: the palette's weighting; Space: back) |
| P | the first scene clip's I and P pictures decoded in place on the cartridge, timed and checked by CRC-32 (Space: back) |
| A | the first scene clip's sound, decoded and played (+ / -: the volume in 3 dB steps; Left / Right: another clip; Space: stop) |
| C | the game's clips checked and those not there converted, as at every start (Space: stop, or back) |
| V | the clips: the list of the converted clips of one set (S: the other set; Return: play one; L: play every clip from there to the last, then the run's results; Space: back). While a clip plays: Space pauses it, Left / Right the previous or next clip, S the same clip in the other set at the same frame, O the frame and the times over the picture, U / D the volume (-18 to +18 dB, saved for each sound output), Q stops it and shows the pictures shown and dropped and the times |
| T / Y | the palette test: two pictures with palettes of their own, sent with the frame (T: only red and green show) or before it, the old way (Y: blue and white flashes); Space: back |
| G | the game |
| X | back to Booster, the Multi-device's menu (the ST resets into it) |
| Esc | back to GEM |

The cartridge's SELECT button: a short press restarts the cartridge (then reset the ST); held for
10 s it is a factory reset.

## Build

```bash
# ./build.sh <board> <build_type> <app_uuid>
#   board:      pico_w
#   build_type: debug | release
#   app_uuid:   the app's UUID4 (desc/app.json); 44444444-4444-4444-8444-444444444444 for
#               local builds
./build.sh pico_w release 44444444-4444-4444-8444-444444444444
```

The firmware is `dist/<APP_UUID>-<VERSION>.uf2`. It needs the ARM GNU Toolchain 14.2
(`PICO_TOOLCHAIN_PATH`), and the `atarist-toolkit-docker` (`stcmd`) for the ST's side. The first
build clones the submodules (pico-sdk, pico-extras, fatfs-sdk). The app plays at 25 frames a
second; `APP_PROFILE=PROFILE_50FPS ./build.sh ...` builds the 50 fps profile.

- `make -C tests/host test` runs the host tests (the pure logic, on your computer, in a second
  or two); CI runs them and both builds on every pull request.
- `tools/dlconv/dlconv.c` is the cartridge's decoder and converter on a PC, for comparing its
  output with a reference decoder (its commands are at the top of the file). It builds with the
  host's compiler:

  ```bash
  mkdir -p build/dlconv
  cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unknown-pragmas -I rp/src/include \
     tools/dlconv/dlconv.c rp/src/mpeg1_video.c rp/src/mpeg_ps.c rp/src/crc32.c \
     rp/src/picture16.c rp/src/mp2_audio.c rp/src/cadence.c rp/src/convert.c \
     rp/src/clip.c -o build/dlconv/dlconv
  ```
- With a Raspberry Pi Debug Probe on the cartridge's SWD and debug UART, `tools/dev/` builds,
  flashes and checks the firmware, captures its console and drives the bench from the host:
  see [`tools/dev/README.md`](tools/dev/README.md).

## How it is made

The app is built on [md-framebuffer-template](https://github.com/sidecartridge/md-framebuffer-template)
v1.1.0, whose framework does the ST's side: the frame the RP draws, one byte per pixel, is
converted to the ST's planes and copied to the screen every frame, and the keyboard, the mouse,
the joysticks and the sound go through the cartridge. The template's README is the guide to that
framework's API. [`CLAUDE.md`](CLAUDE.md) describes this repository's code, the framework's
internals included. The Multi-device's programming documentation is at
<https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

## Acknowledgements

This project builds on the work of others, with thanks:

- **[DirkSimple](https://github.com/icculus/DirkSimple)** by Ryan C. Gordon (zlib licence): the
  game's logic, its scenes, sequences, move windows, deaths and points, comes from DirkSimple's
  `game.lua`, its re-creation of the arcade game from the original ROM's data (vendored unchanged
  in `third_party/dirksimple`).
- **[SNES Super Dragon's Lair Arcade](https://github.com/astrobleem/SNES-SuperDragonsLairArcade)**
  by Chad Doebelin (MIT licence): its laserdisc segment table (`segment_timing.json`) is how the
  laserdisc's frames, which DirkSimple's sequences start at, are found in the CD-ROM's clips
  (vendored unchanged in `third_party/snes-superdragonslairarcade`).
- **[pl_mpeg](https://github.com/phoboslab/pl_mpeg)** by Dominic Szablewski (MIT licence): the
  MPEG-1 video and MP2 audio decoding tables (`tools/gen_mpeg1_tables.py`,
  `tools/gen_mp2_tables.py`).
- **[QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)** by Project Nayuki
  (MIT licence): the QR code on the start screen (`rp/src/qrcodegen.c`, unchanged).
- **[md-framebuffer-template](https://github.com/sidecartridge/md-framebuffer-template)** and
  md-microfirmware-template by SidecarTridge: the framework this app is built on.

Dragon's Lair itself (Cinematronics, 1983, animated by Don Bluth) is not part of this project:
the app plays the clips of the player's own copy of the CD-ROM.

## License

GPL v3.0: see [LICENSE](LICENSE).
