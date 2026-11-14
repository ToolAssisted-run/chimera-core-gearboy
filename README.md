# chimera-core-gearboy

A core for [Chimera](https://github.com/ToolAssisted-run/chimera), the
frontend for tool-assisted speedruns, for the **Game Boy**, the **Game Boy
Color** and the **Super Game Boy**. It is built on
[Gearboy](https://github.com/drhelius/Gearboy) by Ignacio Sanchez, and
produces one file, `gearboy.chimeraCore`, that goes in Chimera's `cores`
folder.

## Status

New, and not published yet.

- Four consoles: the Game Boy, the Game Boy Color, a Game Boy Advance
  running Game Boy Color games, and the Super Game Boy (its border, its
  colours and its four controllers, as Gearboy works them out).
- The tilt sensor of an MBC7 cartridge (Kirby Tilt 'n' Tumble), on two axes.
- Tried for 2,000 frames each with no boot ROM, in the sandbox and natively:
  Bugs Bunny Crazy Castle II, Kirby's Star Stacker (Japan), NHL 2000,
  Pokemon Gold, Prince of Persia and Super Mario Bros. Deluxe; NHL 2000 and
  Pokemon Gold on the Super Game Boy too. No game was played through.
- Game Boy Link: two Game Boys joined by a link cable, in the one core,
  each with its own game, their screens side by side. Two Super Mario Bros.
  Deluxe were linked and raced in its Vs. mode; no Pokemon trade was tried.
- Not there: the Barcode Boy, the Game Boy Camera's picture and the Game
  Boy Printer.

## What you bring

- A game: a `.gb`, `.gbc` or `.sgb` file; for the link machine, one for
  each Game Boy (or one, used for both).
- The boot ROM of the Game Boy (`dmg_boot.bin`) or the Game Boy Color
  (`cgb_boot.bin`), which is Nintendo's. Boot ROM is on by default; set it
  off and none is needed: the game starts at once, in the state the boot ROM
  would have left. The Game Boy Advance and the Super Game Boy need none.
- Only if you want it: the Super Game Boy's own cartridge (`SGB1.sfc` or
  `SGB2.sfc`), for its default border and colours.

## Buttons and settings

- **Buttons**: Up, Down, Left, Right, Start, Select, B and A. The Super Game
  Boy has four controllers, P1 to P4. **Power** turns the console off and on
  again. **P1 Tilt X** and **P1 Tilt Y** are a tilt cartridge's sensor. On
  the link machine P1's and P2's pads are the two Game Boys', and **Toggle
  Link Connection** pulls the cable out and puts it back.
- **Model**: GB, GBC, GBA, SGB or LINK, chosen with the machine in the New
  Project window. **P1 Game Boy** and **P2 Game Boy**: which console each
  linked Game Boy is.
- **Boot ROM**: on (the default, the console's own start-up) or off.
- **Super Game Boy cartridge**: none, SGB1 or SGB2.
- **Game Boy colours** (green or grey) and **Colour correction** change the
  picture only.

A cartridge's clock (Pokemon Gold has one) counts the console's own time
from the second the project's clock starts at, so a movie replays the same.
Emulator > Export Save Data writes the cartridge's battery memory as Gearboy
writes it; a project can start from one.

RAM Watch and RAM Search read the work memory (WRAM), the video memory
(VRAM, and VRAM1 on the Game Boy Color), the cartridge's memory, the sprite
table (OAM), the high memory (HRAM), the cartridge's ROM, and the 64 KB the
processor sees (the system bus); on the link machine, each Game Boy's
("P2 WRAM").

## Building and testing

[docs/BUILDING.md](docs/BUILDING.md) has every step. [docs/PLAN.md](docs/PLAN.md)
is the design log: what was decided, why, and what was measured.

## Licence

Gearboy and this repository's own files are under the GNU GPL version 3 or
later, and so is the package. See [LICENSE](LICENSE).
