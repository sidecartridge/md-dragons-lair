# AGENTS.md — md-dragons-lair Playbook

Welcome to the md-dragons-lair workspace, created from md-framebuffer-template. This is the quick primer so any agent can get productive fast. `CLAUDE.md` is the full reference (architecture, shared-region map, pipelines, tests); where the two disagree, `CLAUDE.md` and the code win.

## 1. Environment setup (do this before touching the repo)
- **Host tooling**
  - ARM GNU Toolchain 14.2 (for example `/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin`); export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir.
  - Raspberry Pi Debug Probe / Picoprobe wired to the Multi-device header (TX, RX and both GND pins **must** be connected).
  - `atarist-toolkit-docker` installed and working (`stcmd` requires a PTY, so run with `pty=true`).
  - Git + GNU Make + VS Code with the C/C++ Extension Pack, CMake Tools and Cortex-Debug.
- **SDK environment variables** (set from the repo by the build when unset):
  ```bash
  export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
  export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
  export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
  ```
- **Optional debugger helpers**
  ```bash
  export ARM_GDB_PATH=/path/to/arm-none-eabi/bin
  export PICO_OPENOCD_PATH=/path/to/openocd/tcl
  ```

## 2. Common Commands
```bash
# List workspace via stcmd (requires PTY)
stcmd ls

# Host tests: the firmware's pure logic, in seconds
make -C tests/host test

# Build firmware (board, build type, the development UUID)
PICO_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin \
  ./build.sh pico_w release 44444444-4444-4444-8444-444444444444
```

With the Debug Probe attached (SWD and the debug UART), `tools/dev/` builds, flashes and verifies the RP, captures its console and reads or drives it from the host (see `tools/dev/README.md`):
```bash
python3 tools/dev/console.py watch                 # console capture, leave running
tools/dev/flash.sh debug                           # build out of tree, flash, verify over SWD
python3 tools/dev/swd.py counters --watch 2        # frames, blits, overruns per second
python3 tools/dev/swd.py fb screen.png             # what the ST shows, as a PNG
python3 tools/dev/tools_harness.py --build --flash --reset
```

## 3. Build Notes & Gotchas
- Expect harmless VASM warnings (`target data type overflow`, `trailing garbage after option -D`).
- The build script auto-copies `version.txt`, rebuilds the Atari target, then the RP target. A fresh clone has empty submodule folders: the first `./build.sh` clones and pins them.
- Successful builds drop the UF2 into `dist/` as `<UUID>-<version>.uf2` (`version.txt` already carries the `v`) and print the MD5 used in the generated JSON manifest.
- Use the development UUID `44444444-4444-4444-8444-444444444444` for local builds: any other UUID has no config sector and the app jumps to Booster.
- The Atari cartridge image (header + m68k code) must fit in 16 KB; `target/atarist/build.sh` enforces this against `BOOT.BIN` and aborts if exceeded. The 16 KB matches `CART_CARTRIDGE_CODE_SIZE` in `rp/src/include/cart_shared.h` and `CARTRIDGE_CODE_SIZE` in `target/atarist/src/inc/sidecart_layout.s`.
- After a change in `target/atarist/`, rebuild the m68k image (`./build.sh`, or `(cd target/atarist && ./build.sh "$PWD" release 0)`): it regenerates `rp/src/include/target_firmware.h`, which `tools/dev/flash.sh` does not.
- FatFs configuration lives at `rp/src/ff/ffconf.h`. `rp/src/CMakeLists.txt` puts that directory ahead of the `fatfs-sdk` include path with `target_include_directories(... BEFORE PRIVATE)`, so the override wins and the submodule stays clean. Do not edit the submodule's copy.

## 4. Troubleshooting
| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` when using `stcmd` | `target/atarist/build.sh` already sets `STCMD_NO_TTY=1` for every stcmd call. If you invoke `stcmd` directly from a non-TTY context, export `STCMD_NO_TTY=1` first. |
| `arm-none-eabi-gcc not found` | Ensure `PICO_TOOLCHAIN_PATH` points to the Arm GNU toolchain bin dir |
| `ERROR: cartridge code is N bytes; limit is 16384` | The m68k cartridge grew past 16 KB. Trim `target/atarist/src/userfw.s` / `main.s` (and their includes) or move data into `APP_FREE` / `SHARED_VARIABLES` rather than embedding it in the cartridge image. |
| `region RAM overflowed` at link time | Static data left less than the 32 KB heap the link guarantees (`PICO_HEAP_SIZE`). Shrink or share a static buffer. |
| The build stops before the UF2 is copied | An earlier step failed and the scripts stop at the first failure: scroll back to the first error. |
| The Atari ST display shows garbage but the RP runs | `target_firmware.h` does not match the m68k sources. Rebuild the m68k image (see the build notes) and flash again. |
| The app boots to Booster | The UUID the UF2 was built with has no config sector: build with the development UUID, or install the app through Booster. |

## 5. Editing Guardrails
- Agents are **not allowed** to modify code inside these directories under any circumstances:
  - `/fatfs-sdk`
  - `/pico-sdk`
  - `/pico-extras`
- To change FatFs configuration, edit `rp/src/ff/ffconf.h`, not the file inside `/fatfs-sdk`.
- **Never add AI-tool attribution** to commits, PR descriptions, code comments, docs, or any other artifact. No `Co-Authored-By: Claude …`, no "Generated with Claude Code / ChatGPT / etc.", no "AI-assisted" notes. Write everything as the human author.
- Release workflow: a new version starts with `release/vX.Y.Z` branched from `main` (the name is what `version.txt` will contain). Each epic gets its own branch cut from the release branch, `epic/NN-<slug>`, and its pull request targets the release branch, never `main`; it is merged after Diego verifies it on hardware. `main` receives the release branch once, when the version is done, and only then is it tagged. Commit, push and open PRs only when asked.
- Planning notes (iterations, epics, stories) live in `docs/epics/`, which is gitignored and machine-local. Never name an epic, story, iteration or task in anything committed or pushed — comments, docs, changelog, commit messages, PR descriptions (epic branch names, `epic/NN-<slug>`, are the one exception). Write the information itself, not a pointer to a document the reader cannot open. Release check: `git grep -nIiE "\b(epic|story|iteration)[ -]?[0-9]|docs/epics" -- ':!CLAUDE.md' ':!AGENTS.md' ':!.gitignore'` must come back empty.

Keep this file updated as the process evolves so every agent starts with the latest tribal knowledge.
