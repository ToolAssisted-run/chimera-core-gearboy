# Design log

Dated entries: what was decided, why, and what was measured.

## 2026-11-14: Gearboy as a core (issue 581, user-decided)

The owner approved building Gearboy (https://github.com/drhelius/Gearboy)
as a core on 2026-11-14, with every system the emulator supports. It is one
of six cores from drhelius's Gear emulators built at the same time
(gearboy, gearsystem, gearcoleco, geargrafx, gearlynx, geartowns).

### Feasibility (Chimera's docs/new-core.md, section 1)

- **A new core?** Yes. Chimera has Gambatte and Mesen for the Game Boy;
  Gearboy is another emulator, asked for by name.
- **Which Gearboy.** The release tag 3.8.20 (commit a9ab9bdf, 2026-10-30),
  the newest release when the work started.
- **Licence.** At that commit `LICENSE` is the GNU GPL version 3, and every
  source file says "version 3 or any later version". Modified binaries may
  be passed on with their source. The sound chip, Shay Green's Gb_Snd_Emu
  (`src/audio`), is under the LGPL 2.1 or later; miniz is MIT. The package
  is under the GPL 3 or later.
- **Compiled-in firmware.** None. Gearboy reads the boot ROMs from files
  (`Memory::LoadBootromFromBuffer`) and the Super Game Boy's SNES cartridge
  from a file (`SGB::LoadBIOSFromBuffer`, which only knows four dumps by
  their CRC-32). Searched for byte tables: `Memory.h` holds the tiles and
  map the Game Boy's boot ROM leaves in video memory (the logo as the
  cartridge header gives it, 200 + 44 bytes) and the I/O registers' values
  after it, which is how every emulator starts a game without the boot ROM;
  it is not the boot ROM's code. The Super Game Boy's border is drawn from
  the SNES cartridge when there is one and is empty otherwise
  (`SGB::LoadDefaultBorder`): no border of Nintendo's is in the source.
- **One thread, one clock.** The emulator has no thread. It reads the
  computer's clock in one place, `Cartridge::UpdateCurrentRTC` (`time()`),
  for the clocks of MBC3, HuC3 and TAMA5 cartridges; that became a hook
  (below). Nothing is random.
- **The picture.** Gearboy's own framebuffer: 160x144, or 256x224 for the
  Super Game Boy with its border. No GPU.
- **No recompiler.**
- **What the user brings.** A game. Optionally the boot ROMs (`dmg_boot.bin`,
  `cgb_boot.bin`) and the Super Game Boy's cartridge (`SGB1.sfc`,
  `SGB2.sfc`); Gearboy starts every game without them.
- **What CI can run.** Test cartridges written by this repository's
  scripts (below). No download.
- **Size.** Small: a 3.4 MB `core.wbx`, states of 2.3 MB.

### What is built

The files upstream's libretro core builds
(`platforms/libretro/Makefile.common`), with its defines
(`GEARBOY_DISABLE_DISASSEMBLER`, `GEARBOY_DISABLE_VGMRECORDER`,
`MINIZ_NO_TIME`), without the libretro frontend. Nothing of the desktop
program, ImGui, the debugger or the MCP server; no network code.
`waterbox/gearboy.cpp` is the frontend. Gearboy's log goes nowhere
(`g_mcp_stdio_mode` is set, as when its MCP server owns standard output).

### The patches (hooks, no emulation changed)

1. `0001-cartridge-clock-time-source`: `Cartridge::SetRTCTimeSource`, where
   `UpdateCurrentRTC` reads the time. The core gives it the machine's time:
   the second the project's clock starts at (`time()` in the sandbox, which
   the engine sets from the project) plus the cycles the machine has run
   over 4,194,304. The MBC3's clock already counts cycles in Gearboy; the
   time is what saved data is stamped with, what moves the clock on when
   saved data is loaded, and what runs the HuC3 and TAMA5 clocks. The
   native reference answers `time()` with the sandbox's default start,
   1495889068.
2. `0002-input-read-hook`: `Input::SetReadHook`, called when the program
   reads the joypad register (FF00). A frame with no read is a lag frame,
   and each read calls Chimera's input-poll callback.
3. `0003-sgb-model`: `GearboyCore::SetSGBModel`. Gearboy allows the Super
   Game Boy mode only when the game is not forced to the Game Boy; a game
   that supports both the Game Boy Color and the Super Game Boy (Pokemon
   Gold) has to be forced to the Game Boy to run on a Super Game Boy. The
   hook keeps the mode for that case.

### The machines

The Model setting is GB, GBC, GBA or SGB, each one of the package's
machines (systems GB, GBC, GBC and SGB). GB forces the Game Boy; GBC lets
the game choose, which in Gearboy means a Game Boy game runs as on a Game
Boy (Gearboy has no Game Boy Color compatibility mode with its colouring);
GBA is the Game Boy Color telling the game it is a Game Boy Advance; SGB
forces the Game Boy with the Super Game Boy on. The test program reads the
registers each console starts it with (A/B/C): Game Boy 1/0/19, Game Boy
Color 17/0/0, Game Boy Advance 17/1/0, Super Game Boy 1/0/20.

- **Boot ROM** (off by default, the owner may choose otherwise): on runs
  `dmg_boot.bin` or `cgb_boot.bin`. Gearboy has no Game Boy Advance or Super
  Game Boy boot ROM, so the setting is shown for GB and GBC only.
- **Super Game Boy cartridge** (none, SGB1, SGB2): with one, the border and
  colours before a game sends its own, and Gearboy's "Super Game Boy for
  every game", so a game made only for the Game Boy runs in the border.
  Without one, a game made for the Super Game Boy draws its own border and
  colours, and any other game runs as on a Game Boy at 160x144. The firmware
  entries pin no SHA-1 (Gearboy accepts three dumps of the first cartridge);
  they give the size and name hints.
- **Game Boy colours** (green, Gearboy's own, or grey) and **Colour
  correction** change the picture only; the test `consoles:picture-settings`
  shows the memory is the same with them.
- **The controls**: four pads of eight buttons (P2 to P4 live on the Super
  Game Boy only), Power, and P1 Tilt X and Y (-32768 to 32767, live only
  with an MBC7 cartridge; 8,192 is one of Gearboy's units, which it clamps
  at 4). Power is Gearboy's reset that keeps the cartridge's memory.
- **A frame** is one `RunToVBlank`: to the start of the next vertical blank,
  or 87,780 cycles when the screen is off.
- **The sound**: Gearboy's 44,100 stereo samples a second. 442,971 samples
  in 600 frames (443,012 for 600 frames of 70,224 cycles).
- **The memory areas**: ROM, VRAM, VRAM1 (Game Boy Color), WRAM (8 KB, or
  32 KB on the Game Boy Color), CartRAM, OAM, HRAM, and the system bus,
  read without touching the machine (I/O registers as Gearboy last kept
  them in its memory map). Registers PC, SP, AF, BC, DE, HL.
- **Export Save Data** writes Gearboy's `.sav` (8,240 bytes for the test
  cartridge: 8 KB and the clock's 48 bytes). Writing it reads the clock; the
  core gives the cartridge its own cached time back, and the file is built
  in memory no state carries.

### What the tests compare

`waterbox/run-gate.sh`, 47 tests: 43 passed, 0 failed, 4 skipped (the boot
ROM and Super Game Boy cartridge tests: none of those files was on the
machine). With the owner's games in `tests/games-local`; without them the
game tests are one SKIP.

- The source is the pinned commit with the 3 patches; the package is not
  older than its sources; each machine declares the core's 33 buttons and 2
  axes; the default key bindings name keys Chimera knows.
- The test program (`tests/make-testprog.py`, from the Gambatte core): it
  runs; each of the 8 buttons is its own bit; lag frames (1 in 300 when it
  reads the buttons, 300 when it never does); the screen off; the clock (10
  seconds after 600 frames, 20 after 1,200); the sound; Power; the saved
  data out and back in; the clock's time stamp (4 to 5 seconds after the
  start at frame 300; saved data stamped an hour and 5 seconds before the
  start moves the clock on 5 seconds); exporting the saved data every frame
  changes nothing; the system bus.
- The consoles: with no Model a `.gbc` runs as GBC; the four tell the game
  who they are; the Super Game Boy picture is 256x224 with the screen 48 in
  and 40 down; the Super Game Boy variant of the program sends its palettes
  and MLT_REQ, and each of the four pads reaches it as its own (P1 Up, P2
  Up, P3 A, P4 Start read as 64 64 1 8); on a Game Boy P2 to P4 reach
  nothing.
- The tilt sensor (`tests/make-gb-test.py`, from the Mesen core): level
  81d0 81d0, tilted by 16384 and -8192 82b0 8160.
- Native against sandbox: the same 60 digests of memory, picture and sound
  with presses and Power; a state saved and loaded every frame (2,280,416
  bytes a state); the machine moved to a new sandbox half way; a state kept
  at frame 800 and loaded at 1200. The same on the Game Boy Color, the Game
  Boy Advance, the Super Game Boy (its four pads), the tilt cartridge (both
  axes, Power), with colour correction and with grey shades.
- Refused, with the reason: no game, a file too short, Boot ROM on with no
  boot ROM, a Super Game Boy cartridge asked for and missing, and one Gearboy
  does not know.
- Chimera's headless runner plays projects of the test program (presses)
  and of the Super Game Boy program (P3's A) to the sandbox runner's memory;
  the reads of the buttons reach Chimera as they happen.

`tests/run-frontend.sh` in Chimera's window, headless: 4 passed, 1 skipped
(`window:refusal` needs `cgb_boot.bin`): the test program's memory against
the headless runner's, the package's keys, the Super Game Boy program at
256x224 with P3's pad, and NHL 2000 for 1,500 frames.

Chimera's package checks (`test-packages`, a folder with only this
package): 25 passed. `tools/check-core-texts.py`: 30 keys, all named.

### Broken on purpose

Each was a change to `waterbox/gearboy.cpp`, rebuilt, and the test script
run:

- every frame said to read the buttons: `prog:lag` failed;
- the machine's time not moving, and the computer's time: `prog:clock-time`;
- Power doing nothing: `prog:power`;
- the Super Game Boy hook off: `consoles:models`, `consoles:sgb-screen`,
  `consoles:sgb-pads`;
- the tilt scaled twice as much: `tilt:axes`;
- colour correction ignored: `consoles:picture-settings`;
- the bus reading zeros: `prog:bus`;
- A and B swapped: `prog:buttons`, `consoles:sgb-pads`, `engine:project`,
  `engine:sgb-project`;
- pads 2 to 4 not passed on: `consoles:sgb-pads`.

Not caught: exporting the saved data with the clock's cached time updated
(the change `prog:save-data-looks` is meant for). The cached time lives in
Gearboy's cartridge object, which no memory digest covers, and the test
cartridge's MBC3 clock does not read it while running; only HuC3 and TAMA5
cartridges would. So `prog:save-data-looks` has not been seen to fail.

### Games

From the owner's collection, copied into `tests/games-local` (SHA-1):
Bugs Bunny Crazy Castle II (1e4c8c8f), Kirby's Hoshinoka-Bi v1.1, Japan
(5fe35fab), NHL 2000 (8f7cb463), Pokemon Gold (d8b8a360), Prince of Persia
(ef2f6402), Super Mario Bros. Deluxe Rev A (07295cd6): each 2,000 frames
with no boot ROM, Start and A pressed, native and sandbox the same; NHL
2000 and Pokemon Gold also on the Super Game Boy, in their own borders.
Pokemon Gold's title screen was looked at on the Super Game Boy and the
Game Boy Color. None was played through. No boot ROM or Super Game Boy
cartridge was on the machine, so none was tried.

### Not done

- The link cable (Gearboy links two of itself), the Barcode Boy, the Game
  Boy Camera's picture, the infrared port and the Game Boy Printer.
- A Game Boy game on the Game Boy Color is not coloured (Gearboy's
  behaviour).
- Windows and the aarch64 build were not run here; CI is set up for both
  and was not run, as nothing is pushed.

## 2026-11-14: Boot ROM on by default (user-decided, 2026-11-14)

The owner chose the boot ROM on by default, as the Gambatte core has it,
over Gearboy's own default (off). So a Game Boy or Game Boy Color project
needs `dmg_boot.bin` or `cgb_boot.bin`, as the Gambatte core's does: the
New Project window asks for it on its firmware page, and a project without
it does not open; the core's message names the file and says the other
way, setting Boot ROM to off, which needs no file. The Game Boy Advance and
the Super Game Boy have no boot ROM in Gearboy and start without one, as
before. The test `refused:default-boot-rom` holds both: a Game Boy project
that does not name the setting and has no boot ROM is refused with
`dmg_boot.bin` named, and a Super Game Boy project starts. With the setting
off in the core's default, broken on purpose, it failed. The other tests
set Boot ROM off where they need no boot ROM.

## 2026-11-14: the Game Boy link cable, in the core (user-decided, 2026-11-14)

The owner's words: "No need to change this at a chimera level. Each core
should be able to do the replication at the core level and expose this as
a single machine to Chimera." So the link machine is two Gearboy machines
inside this one core, shown to Chimera as one machine, as the Gambatte core
does with its Game Boy Link (system GBL).

- **Model LINK**, the package's machine GBL ("Game Boy Link (two Game
  Boys)"). `linkModel1` and `linkModel2` say which console each is (GB, GBC
  or GBA; not the Super Game Boy). The slots `game2` and `savedata2` hold
  the second Game Boy's game and saved data; with no `game2` the first game
  is used for both. Boot ROM applies to each Game Boy that has one.
- **How the two run.** Gearboy has its own link cable for two of itself
  (`platforms/libretro/libretro_link.cpp`): each Game Boy reports its serial
  port's state and the start of each transfer through callbacks
  (`SetLinkCableCallbacks`), and the frontend steps the two a few cycles at
  a time, always the one behind, matching each transfer with what the other
  had in its port at that cycle. `gearboy.cpp` carries that loop
  (`link_frame`, `link_resolve`) over without its libretro parts or its own
  savestates: the sandbox keeps the matching state with the rest of the
  machine. So a link frame is always 70,224 cycles of each Game Boy.
- **Controls.** One list for every machine, 34 buttons: four pads, Power and
  Toggle Link Connection, and the two tilt axes. On the link machine P1's
  pad is the first Game Boy's, P2's the second's; Power restarts both and
  starts the cable's matching afresh (as upstream's reset does); Toggle Link
  Connection pulls the cable out (a Game Boy that sends on its own clock
  then gets 255) and puts it back. A frame is a lag frame when neither Game
  Boy read its buttons.
- **Picture and sound.** The screens side by side, 320x144; the two sounds
  mixed, each at half, as upstream mixes them.
- **Memory.** Each Game Boy's areas and bus, named with its player
  ("P2 WRAM", "P2 System Bus"); Export Save Data writes "P2 <game>.sav" for
  the second. The registers are P1's.

### Tests

The test program's link modes (from the Gambatte core): a master sends its
frame count every frame on its own clock; a slave answers on the master's
clock with its own count, the top bit set. In Gearboy the slave is ready
for a transfer about every second frame, so it finishes about 150 in 300
frames.

- `link:exchange`: 150 transfers in 300 frames, each Game Boy getting the
  other's count; 320x144.
- `link:unplugged` and its control: the cable pulled at frame 100 stops
  the slave's transfers (50, as at frame 100 with the cable in) and the
  master gets 255; put back at 200, they go on (100 by frame 300).
- `link:controls`: each pad reaches its own Game Boy; Power restarts both.
- `link:models`: GBC and GBA as the two Game Boys tell the game so (A/B
  17/0 and 17/1); with no second game, the second Game Boy runs the first's.
- `link:save-data`, `link:sound` (442,971 samples in 600 frames, and P2's
  sound in the mix), `link:lag` (300 in 300 when neither reads, 1 when P2
  does).
- `link:sandbox`: native, sandbox, a state saved and loaded every frame
  (4,463,682 bytes, both Game Boys) and the machine moved to a new sandbox
  half way: the same 60 digests, with both pads, the cable pulled and put
  back, and Power. `link:state-rewind`.
- `engine:link`: a link project in Chimera's headless runner gives P2's
  memory as the sandbox runner does. `window:link` (tests/run-frontend.sh):
  the same in Chimera's window, at 320x144.
- `link-game:smb-deluxe` (needs the game): two Super Mario Bros. Deluxe
  (USA, Europe) (Rev A), both as the Game Boy Color, go from the title to
  VS GAME, reach the Vs. screen, where each shows itself as YOU and the
  other as RIVAL, and race on course 1-1 (2,100 frames; both screens were
  looked at). Native and sandbox the same.

Not tried: a Pokemon trade or battle (Pokemon Gold needs a saved game with
two Pokemon, and the Gambatte core's input file for one is timed for
Gambatte), Rampart's and NHL 2000's link modes; Tetris was not in the
collection yet. No link game was played to its end.

### Broken on purpose, and the counts

Each was a change to `waterbox/gearboy.cpp`, rebuilt, and the test script
run: the link matching only transfers P1 starts (`link:exchange`, once the
test also ran with P2 sending; before that it passed, as only P1 started
transfers), the starting Game Boy never given the other's byte
(`link:exchange`, once the test refused 255; before that it passed, since
255 has the top bit too), the toggle doing nothing (`link:unplugged`, its
control), P2's pad sent to P1 (`link:controls`), `linkModel` ignored
(`link:models`), P2's saved data from P1's slot (`link:save-data`), only
P1's sound (`link:sound`), Power restarting P1 only (`link:controls`), P2's
pad given to the wrong player (`link:controls`, `link:sound`,
`engine:link`), Boot ROM off by default (`refused:default-boot-rom`).

`waterbox/run-gate.sh`: 56 passed, 0 failed, 4 skipped (boot ROMs and
Super Game Boy cartridges absent). `tests/run-frontend.sh`: 5 passed, 1
skipped. Chimera's package checks: 25 passed. All 38 texts in 26
languages.
