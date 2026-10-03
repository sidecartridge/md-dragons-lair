# Dragon's Lair for the SidecarTridge Multi-device

Dragon's Lair, the laserdisc game, on an Atari ST, STE, Mega ST or Mega STE with a
[SidecarTridge Multi-device](https://sidecartridge.com) in its cartridge slot. The Raspberry Pi
Pico (RP2040) in the cartridge reads the game's clips from the SD card, turns each picture into
320×200 in 16 colours of its own and serves it to the ST, which shows it at 25 frames a second;
the sound plays through the DMA sound chip on an STE or a Mega STE, through the YM2149 elsewhere.

**This is work in progress, and it is not the game yet.** The current firmware is a bench: it
finds the CD-ROM image on the card, lists it, measures how fast the card reads, decodes a clip's
pictures and sound on the cartridge and shows or plays them. The next versions convert the clips
once into a format of the app's own on the card, then play them and the game.

## What you need

- Your own copy of the game's PC CD-ROM, as an ISO 9660 image. Nothing of the game is in this
  repository or in the firmware.
- A microSD card in the Multi-device, with the image in the folder `/DLAIR` (the app creates the
  folder when it is missing). It looks for `DL_CDROM_V31.ISO`, then for any `.ISO` in the folder
  whose root holds `S01.MPG`.
- An Atari ST, STE, Mega ST or Mega STE with a colour monitor (low or medium resolution). In
  high resolution the app returns to GEM with a message.

## The bench

After the ST boots, the screen shows the card, the folder, the machine and TOS the ST reported,
the image (its size, its fragments on the card, its volume), its root directory and the scene
clips in it. The keys:

| Key | What it does |
| --- | --- |
| Up / Down, Left / Right | scroll the listing, a line or a page |
| R | the read test: the card's speed at two SPI clocks and four read sizes, and a clip's CRC-32 |
| I | the intra pictures of the first scene clip, converted and shown (Left / Right: another clip; T: the timings; C: 512 or 4,096 colours; D: the dither; W: the palette's weighting; Space: back) |
| P | the first scene clip's I and P pictures decoded in place on the cartridge, timed and checked by CRC-32 (Space: back) |
| A | the first scene clip's sound, decoded and played (+ / -: the volume in 3 dB steps; Left / Right: another clip; Space: stop) |
| Esc | back to GEM |

The cartridge's SELECT button works as in every Multi-device app: a short press restarts the
cartridge (then reset the ST), held at power-on it starts Booster, the Multi-device's menu.

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
     rp/src/picture16.c rp/src/mp2_audio.c -o build/dlconv/dlconv
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

## License

GPL v3.0: see [LICENSE](LICENSE).
