# AGENTS.md - Gearboy core for Chimera

This repository builds Gearboy (https://github.com/drhelius/Gearboy) as a
Game Boy, Game Boy Color and Super Game Boy core for Chimera
(https://github.com/ToolAssisted-run/chimera), a frontend for tool-assisted
speedruns. It produces one file, `gearboy.chimeraCore`: Gearboy's emulator
compiled to run inside Chimera's sandbox (miniBox), a small program around
it, and the declarations Chimera reads (buttons, settings, machines, file
types, firmware). Its machines are the Game Boy, the Game Boy Color, the
Game Boy Advance (Game Boy Color games), the Super Game Boy, and the Game
Boy Link machine: two Gearboy machines in the core, joined by a link cable.

Read [docs/BUILDING.md](docs/BUILDING.md) before building and
[docs/PLAN.md](docs/PLAN.md) before changing how the core works: it is the
design log, with what was decided, why, and what was measured.

## Layout

- `extern/gearboy` - Gearboy at its release tag 3.8.20, a submodule. Only
  the emulator in `src/` (the files upstream's libretro core builds) and
  `platforms/shared/dependencies/miniz` are compiled; the desktop program,
  its debugger, its MCP server and the libretro frontend are not. Never
  edited in place: every change is a numbered patch in `patches/`.
- `patches/` - three small hooks: where the cartridge clocks read the time
  (0001), a call at each read of the joypad register (0002), and a Super
  Game Boy that keeps its Super Game Boy mode for a game it runs as a Game
  Boy game (0003).
- `meson.build` - the build of both flavors.
- `waterbox/gearboy.cpp` - the program around Gearboy: it loads the game,
  the boot ROM, the Super Game Boy cartridge and the saved data, sets the
  controls, runs the machine to the end of a picture, gives the cartridge
  clocks the machine's own time, and is what Chimera's engine calls. For
  the link machine it runs two Gearboy machines a few cycles at a time and
  matches their serial transfers (`link_frame`, `link_resolve`, carried
  over from upstream's libretro link).
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json`, `languages/` - what the package declares, and
  its texts in Chimera's 26 languages. `licenses/` holds the licence texts.
- `waterbox/run-native.c`, `waterbox/run-wbx.c` - the native runner and the
  sandbox runner. They take the same options and print the same lines
  (`waterbox/gate-harness.h` is the part they share).
- `waterbox/*.sh` - `apply-patches.sh`, `make-patches.sh`, `setup-guest.sh`
  (configures the sandbox build), `build-package.sh`, `run-gate.sh` (the
  test script).
- `tests/` - `make-testprog.py` (the test cartridge, with a Super Game Boy
  variant and link modes) and `results.py` (reads what it left), `make-gb-test.py` (a
  tilt-sensor cartridge), `sgb-screen.py`, `check-key-names.py`,
  `run-frontend.sh` (the window test, with its helpers), `make-project.py`
  (writes a project). `roms-local/` and `games-local/` hold boot ROMs and
  games of yours and are ignored by git.
- `build/` - every build output, ignored by git.

## Build and test

```sh
git submodule update --init
meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest
waterbox/build-package.sh -r <chimera checkout>
CHIMERA_ROOT=<chimera checkout> bash waterbox/run-gate.sh
TMPDIR=$PWD/build/t bash tests/run-frontend.sh --chimera-root <chimera checkout>
```

Run both test scripts before every commit that touches what they cover, and
say in the commit what ran, with the numbers. A new test is broken on
purpose once and seen to fail.

## Rules of the repository

- **Gearboy's machine, as upstream has it.** The patches are hooks; none
  changes the emulation.
- **The machine's own time.** A cartridge's clock reads the time from the
  core: the second the project's clock starts at, plus the time the machine
  has run in its own cycles. Never the computer's clock.
- **The machine's state is its memory.** A savestate is the sandbox's copy
  of the core's memory. What a tool asks for (the bus, the saved data)
  is answered from memory no state carries (`ECL_INVISIBLE`), and asking
  must leave the machine as it was; the test `prog:save-data-looks` holds
  that for the saved data.
- **The controls are in two places that must agree**: the tables in
  `gearboy.cpp` and the `input` of each machine in `waterbox.config`. The
  test `package:controls` compares them; `package:key-names` checks the
  default bindings name keys Chimera knows.
- **Every text a person reads is a key** of `waterbox/languages/*.json`, in
  all 26 languages, changed in the same commit.
- **No boot ROM, Super Game Boy cartridge or game is committed**, uploaded
  to CI or attached to an issue.
- **Nothing from BizHawk**: no code, movie, save or firmware pack, not even
  for a test.
- **Build only what the libretro core builds.** No desktop program, ImGui,
  MCP server or network code.
- **Say what was not tried.** A game that was not run has not been shown to
  work.
- **Write in plain language**: commit messages, documents, comments and
  setting descriptions are read by people who have never seen this
  repository. `.md` files are ASCII.
- **Commits**: `type(scope): what changed`; a short body with what changed,
  why, and how it was tested, with the numbers. Stage explicit paths. No
  amending, no force push.

## What is the owner's to decide

Publishing, the roster row in Chimera, moving to another Gearboy version,
and anything a licence does not plainly allow.
