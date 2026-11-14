# Building

This core is built on Linux, for the CPU it is built on (x86_64 or aarch64).
The result, `core.wbx`, is one file that runs in Chimera on Linux, Windows
and macOS of that CPU.

## What is needed

- GCC 13 or newer (C and C++), meson, ninja, git, python3.
- A checkout of Chimera with miniBox built, with its C++ guest toolchain
  (Chimera's `AGENTS.md`, "Build"). Below, `<miniBox>` is its
  `extern/chimera-common-minibox` folder.

## Gearboy's source

Gearboy is a submodule, `extern/gearboy`, at its release tag 3.8.20
(commit a9ab9bdf):

```sh
git submodule update --init
```

The core compiles the files upstream's libretro core builds
(`platforms/libretro/Makefile.common`): the emulator in `src/` and miniz,
with the libretro build's own defines (no disassembler, no VGM recorder).
It does not compile the libretro frontend (`libretro*.cpp`), the desktop
program, its debugger or its MCP server. `waterbox/gearboy.cpp` is the
core's own frontend.

Three patches in `patches/` add hooks to Gearboy; `waterbox/apply-patches.sh`
lays them over the submodule (the build runs it). To change one: edit the
files in `extern/gearboy`, build and test, check the file is in the list in
`waterbox/make-patches.sh`, and run that script.

## The two flavors

- `native`: for the host. `run-native` is the reference the tests compare
  the sandbox with.
- `guest`: for miniBox's sandbox. This is `core.wbx`, and `run-wbx` runs it.

```sh
meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest
```

Both runners take a work folder that holds what the sandbox would see: the
game under its name, a boot ROM or Super Game Boy cartridge under its id
(`dmg_boot.bin`, `cgb_boot.bin`, `SGB1.sfc`, `SGB2.sfc`), a file `slots`
(`{"game":["game.gbc"]}`, with `"savedata":["game.sav"]` for saved data)
and a file `settings` (`{}`, or say `{"model":"SGB"}`). With no `model` the
file's extension chooses: `.gbc` the Game Boy Color, `.sgb` the Super Game
Boy, anything else the Game Boy. `waterbox/gate-harness.h` lists the
options they share (`--frames`, `--report`, `--press`, `--axis`,
`--inputs`, `--watch`, `--vid-out`, `--av-stream`, `--dump-domain`,
`--savedata-out`, `--export-every`, `--list-panel`). `run-wbx` adds
`--rerecord` (a state saved and loaded before every frame), `--session` (the
machine moved to a new sandbox half way) and `--rewind`.
`tests/bgra-to-png.py` turns a `--vid-out` picture into a PNG.

## The package

```sh
waterbox/build-package.sh -r <chimera checkout>
```

builds the sandbox core and writes `build/Cores/gearboy.chimeraCore` in
that checkout. `tests/make-project.py` writes a project for a game, which
Chimera's `chimera-run --project` then plays.

## The tests

```sh
CHIMERA_ROOT=<chimera checkout> bash waterbox/run-gate.sh
TMPDIR=$PWD/build/t bash tests/run-frontend.sh --chimera-root <chimera checkout>
```

The first runs every test and says, for each, what was compared. Most need
no file of anyone's:

- `tests/make-testprog.py` writes a test cartridge of this repository's
  own (from the Gambatte core), machine code written out in the script. It
  reads the buttons, counts frames, keeps a battery-backed memory, reads the
  cartridge's clock, keeps the registers the console started it with, makes
  a tone, and turns the screen off while Select is held. With `sgb` it also
  sends the Super Game Boy its palettes and asks it for four controllers.
  `tests/results.py` reads what it left in the work memory.
- `tests/make-gb-test.py` (from the Mesen core) writes `gb-tilt.gb`, a
  cartridge that reads an MBC7 tilt sensor.

The boot ROM tests need `dmg_boot.bin` and `cgb_boot.bin` in
`tests/roms-local/`, the Super Game Boy cartridge tests `SGB1.sfc` or
`SGB2.sfc` there, and the game tests any `.gb` or `.gbc` files in
`tests/games-local/` (`GEARBOY_GAMES=<folder>` names another). Both folders
are ignored by git; the tests say SKIP without them.

The second script runs the package in Chimera's window, run headless (no
display is needed); it needs Chimera built with its window
(`tools/ci-chimera.sh`). Its refusal test needs `cgb_boot.bin` in
`tests/roms-local/`, and its game test a `.gbc` game in `tests/games-local/`.

Chimera's own checks of a package, run with a folder that holds only it:

```sh
<chimera>/build/meson-linux/source/ui/test-packages <folder with the package>
```

## What CI does

`.github/workflows/chimera.yml` builds Chimera and miniBox, the two
flavors and the package, runs both test scripts and Chimera's checks, and
publishes the package as this repository's release. It does that once for
each CPU, on a runner of that CPU (x86_64 on ubuntu-24.04, aarch64 on
ubuntu-24.04-arm). The miniBox cache key carries the runner's Meson version.
A runner has no boot ROM and no game: those tests are skipped there, and
the workflow asks for the others by name.
