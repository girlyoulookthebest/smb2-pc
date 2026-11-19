# SMB2: a native PC port of Super Monkey Ball 2

SMB2 is an unofficial, non-commercial native Windows port of
*Super Monkey Ball 2* (Nintendo GameCube, 2002). It is built from the
[matching decompilation](https://github.com/girlyoulookthebest/smb2-decomp) of the game:
the decompiled C code is compiled for x86 / x86_64 and runs on a PC
platform layer (SDL2, OpenGL 3.3 or Direct3D 12, miniaudio) that stands in for
the GameCube hardware and SDK. It is **not an emulator**.

**You need your own, legally obtained copy of the game**: a disc image
(`.iso` / `.gcm`) of the USA release, disc ID **GM2E8P**, made from a disc you
own. This repository contains **no game assets** (no textures, models, audio
or disc files). The game data is read from your disc image at run time.

Modded disc images work if the mod replaces game files (stages, textures,
sounds); code mods (DOL edits, Gecko/AR codes) don't apply, because the port
runs its own compiled code instead of the disc's `main.dol`.

## Features

- Story Mode, Challenge Mode, Practice, options, memory-card saves and
  replays, and the party games whose code is decompiled: Monkey Race,
  Target, Bowling, Boat, Dogfight, Baseball, Golf and Monkey Fight.
- Not playable yet (not decompiled): Billiards, Shot, Soccer and Tennis. Party Game Select shows them dimmed with a "NOT YET" tag and
  says so in the description box; they can still be unlocked with Play
  Points.
- 32-bit (i686) and 64-bit (x86_64) Windows builds; saves are compatible
  between them.
- Renderers: OpenGL 3.3 (default) and Direct3D 12 (`--renderer d3d12` or the
  settings).
- Any resolution, windowed or borderless fullscreen, original or stretched
  aspect, MSAA, vsync.
- High frame rates: 120/144/165/240 fps, the display's refresh rate or
  uncapped. The game still runs at 60 ticks per second and the frames in
  between are interpolated ([docs/hfr.md](docs/hfr.md)). 60 fps renders
  exactly like the original.
- Controllers: Xbox, PlayStation, Switch and generic pads through SDL2, with
  hot-plugging for up to 4 players, GameCube-style analog triggers and stick
  emulation ([docs/stick.md](docs/stick.md)), plus keyboard. On-screen button
  prompts match the controller in use.
- Languages: English and Japanese (complete on the US disc), German, French,
  Spanish and Italian as far as the US disc has them
  ([docs/language.md](docs/language.md)).
- Deterministic game math: every build produces the same results
  ([docs/detmath.md](docs/detmath.md)).
- A Windows installer that extracts the game data from your disc image once,
  or a portable build that reads the image directly.
- Update check: once a day at most, the game looks for a newer release on
  GitHub and shows its release notes at the title screen; you choose Update
  now (installed version: download, SHA-256 check, Setup updates in place and
  keeps your saves), Later or Skip this version. Portable copies get a link to
  the download page. Off in Options > Game Screen > Updates
  ([docs/updater.md](docs/updater.md)).

## Installing and playing

Download a release from the
[Releases page](https://github.com/girlyoulookthebest/smb2-pc/releases):

- **Installer** (`SMB2_PC_Setup_<version>.exe`): installs the 64-bit build
  (32-bit on 32-bit Windows), asks for your disc image and extracts the game
  data from it. Start the game from the "SMB2" shortcut. Saves and settings go
  to `%APPDATA%\SMB2\installed\`. Details: [installer/README.txt](installer/README.txt).
- **Portable zip**: unpack it anywhere and run `smb2_pc_port.exe`. On the first
  start the game asks for your disc image in a file dialog and remembers it
  (`--iso <path>` works too and is remembered the same way). If the saved image
  can't be opened later, it asks again; cancelling quits the game.
  The memory card is stored next to the exe, settings in
  `%APPDATA%\SMB2\smb2_pc_port\`.

A console window next to the game shows the log, which is useful for bug
reports.

### Controls

Xbox and PlayStation pads are mapped by button position (the bottom face button
is GameCube A); Nintendo pads are mapped by label, as in Nintendo's own ports.

| GameCube | Xbox | PlayStation | Switch | Keyboard |
|---|---|---|---|---|
| A / B / X / Y | A / B / X / Y | Cross / Circle / Square / Triangle | A / B / X / Y | Z / X / C / V |
| L / R (analog + click) | LT / RT | L2 / R2 | ZL / ZR | Q / E (full pull) |
| Z | RB | R1 | R | Space |
| START/PAUSE | Menu | Options | + | Enter |
| Control Stick | left stick | left stick | left stick | W A S D |
| C Stick | right stick | right stick | right stick | - |
| +Control Pad | d-pad | d-pad | d-pad | arrow keys |

- **Alt+Enter** toggles fullscreen, **F1** opens the PC settings overlay.
- Quit to Desktop: B (or Esc) on the title screen, X (or Esc) on Mode Select,
  the pause menu, Options > Game Screen or the F1 overlay.
- The triggers give the analog L/R value and, past 85 % of their travel, the
  digital click, like a GameCube trigger. LB / L1 / L is unused.
- Pads can be plugged in and out at any time; a new pad takes the first free
  port. The keyboard always drives port 1 too. A `gamecontrollerdb.txt` next to
  the exe adds SDL mappings for unknown controllers.

### Settings

In game: Options > Game Screen (resolution, fullscreen, aspect, frame rate,
vsync, MSAA, renderer, language, button prompts, volume, controllers,
updates, memory card) or the F1 overlay, which also shows the version. The Updates row
opens a screen with "Check automatically" (On by default; at most once a
day, only the version number is sent) and "Check now".
They are stored in `settings.dat`. Command-line options override them for one
run; `smb2_pc_port.exe --help` lists them all, the main ones:

```
--iso <file>          disc image (GM2E8P .iso/.gcm); saved for later starts
--gamedata <dir>      extracted game data folder instead of an image
--renderer gl|d3d12   renderer
--fps <n>             60 (default), 120, 144, 240, ..., display or uncapped
--width <w> --height <h> --fullscreen --windowed --no-vsync --msaa <n>
--language auto|en|de|fr|es|it|ja
--button-style auto|gc|ps|ps4|ps5|xbox|switch
--save-dir <dir>      folder for settings.dat and the memory card
--import-save <file>  import a .gci save or a .raw memory card image, then exit
--export-save <dir>   write the saves as .gci files, then exit
```

### Saves: importing from Dolphin or a GameCube memory card

The memory card is a folder: `memcard\` next to the exe (portable zip) or
`%APPDATA%\SMB2\installed\memcard\` (installer). The port reads standard
GameCube save files (`.gci`) and whole memory card images (`.raw`), and
imports the Super Monkey Ball 2 (USA, GM2E8P) game data save, converted to the
port's byte order; saves of other games on a card are listed as skipped.
Replays are not converted. Saves from the European or Japanese version can't
be used.

1. Get the save:
   - **Dolphin**: Tools > Memory Card Manager, select the Super Monkey Ball 2
     save, Export GCI. Or use the whole card file instead (Dolphin's
     `User\GC\USA\Card A\` folder in GCI folder mode already holds
     `.gci` files; a raw card is `MemoryCardA.USA.raw`).
   - **A real memory card**: dump it with the GCMM homebrew on a softmodded Wii
     or GameCube, either the whole card (`.raw`) or the single save (`.gci`).
2. Import it, any of:
   - In game: Options > Game Screen > Memory card (or F1 > Memory card) >
     Import a save..., pick the file. Allowed on the title screen and in the
     menus. If you already have a save, the game asks before replacing it and
     keeps a copy in `memcard\backup\`. The game then closes; start it again
     to play with the imported save.
   - Drop-in: put the `.gci` / `.raw` in `memcard\import\` and start the game.
     It is imported before the game reads the card (a save it replaces is
     copied to `memcard\backup\`), the title screen says what happened, and
     the file moves to `import\done\` (or `import\failed\`).
   - Command line: `smb2_pc_port.exe --import-save <file.gci|card.raw>`
     (prints the result and exits).

To go the other way (back to Dolphin or a real card): Memory card > Export
saves..., or `smb2_pc_port.exe --export-save <folder>`. This writes `.gci`
files: import them in Dolphin's Memory Card Manager (or drop them into its GCI
folder), or copy them to a real card with GCMM.

## Building from source

The game is cross-compiled from Linux (or WSL2 on Windows). The port compiles
the decompilation's C sources directly, so you need both repositories side by
side. Only Windows builds are supported at the moment.

### Requirements

- Ubuntu 22.04/24.04 or similar (WSL2 works):
  ```sh
  sudo apt install git cmake ninja-build python3 make mingw-w64
  ```
  (`mingw-w64` provides `i686-w64-mingw32-gcc` for the 32-bit build.)
- For the 64-bit build: [llvm-mingw 20241217](https://github.com/mstorsjo/llvm-mingw/releases/tag/20241217),
  `llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64.tar.xz` (clang 19). Clang is
  required because the 64-bit build uses `__ptr32` pointer fields
  ([docs/x64.md](docs/x64.md)).
- SDL2 2.30.9 for MinGW (static libraries, see below).
- Your disc image, for the decompilation build and to run the game.

### Directory layout

```
smb2/
├── decomp/        git clone https://github.com/girlyoulookthebest/smb2-decomp decomp
├── pc_port/       git clone https://github.com/girlyoulookthebest/smb2-pc pc_port
│   └── mingw_deps/
│       ├── i686-w64-mingw32/      SDL2 for the 32-bit build
│       └── x86_64-w64-mingw32/    SDL2 for the 64-bit build
└── toolchains/
    └── llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64/
```

These are the defaults. Other locations work with `-DSMB2_DECOMP_DIR=<path>`
and `-DLLVM_MINGW_ROOT=<path>` (or the `LLVM_MINGW_ROOT` environment
variable).

### 1. Decompilation

Build the decompilation once; it verifies your disc image and is the source
of the game code. See its
[README](https://github.com/girlyoulookthebest/smb2-decomp#building):

```sh
cd smb2/decomp
cp /path/to/your/game.iso orig/GM2E8P/
python3 configure.py
ninja
```

### 2. SDL2

```sh
cd smb2/pc_port
mkdir -p mingw_deps && cd mingw_deps

# 32-bit: static SDL2 built with the distribution's MinGW GCC
wget https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
tar xf SDL2-2.30.9.tar.gz
mkdir SDL2-2.30.9/build32 && cd SDL2-2.30.9/build32
../configure --host=i686-w64-mingw32 --prefix="$(realpath ../..)/i686-w64-mingw32" \
    --disable-shared --enable-static
make -j"$(nproc)" && make install
cd ../..

# 64-bit: the x86_64 part of the official MinGW development package
wget https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz
tar xf SDL2-devel-2.30.9-mingw.tar.gz
cp -r SDL2-2.30.9/x86_64-w64-mingw32 .
```

(The devel package unpacks into the same `SDL2-2.30.9/` folder as the source;
only its `x86_64-w64-mingw32/` is used.) SDL2 is linked statically, so the
exes need no `SDL2.dll`.

### 3. The game

```sh
cd smb2/pc_port

# 32-bit (i686, MinGW GCC)
cmake -S . -B build32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake -DSMB2_GAME=ON
ninja -C build32

# 64-bit (x86_64, llvm-mingw clang)
cmake -S . -B build64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw64.cmake -DSMB2_GAME=ON
ninja -C build64
```

The exe and its `assets/` folder are in `build32/bin/` and `build64/bin/`.
Run it with `--iso <your image>` (a Windows path when started from WSL,
e.g. `--iso 'D:\games\smb2.iso'`).

CMake options:

| Option | Default | |
|---|---|---|
| `SMB2_GAME` | `OFF` | link the decompiled game (without it only the platform layer is built) |
| `SMB2_DECOMP_DIR` | `../decomp` | the decompilation checkout |
| `LLVM_MINGW_ROOT` | `../toolchains/llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64` | llvm-mingw for `cmake/llvm-mingw64.cmake` (also read from the environment) |
| `SMB2_VERSION` | `0.0.0-dev` | version in the exe's version resource, the log, the F1 overlay; compared with GitHub releases by the updater |
| `SMB2_UPDATE_REPO` | empty | GitHub `owner/name` whose releases the in-game updater checks; empty = updater compiled in but disabled ([docs/updater.md](docs/updater.md)) |
| `SMB2_PTR_WARNINGS` | `OFF` | report pointer/int size-mismatch casts in game code instead of `-w` |
| `CMAKE_BUILD_TYPE` | `Release` | |

### 4. Installer (optional)

```sh
installer/build.sh --version 1.2.3
```

builds both exes (in `build32/` and `build64/`) and
`installer/out/SMB2_PC_Setup_1.2.3.exe` with NSIS. It uses the system
`makensis` (`sudo apt install nsis`); without one it unpacks the Ubuntu `nsis`
packages into `../tools_nsis` (no root needed), or set `MAKENSIS` and
`NSISDIR`. Other options are listed in the script header: `--exe32` /
`--exe64` to package existing exes, `--no-build`, and `--test-root` for a test
build that only writes below a test folder ([docs/installer.md](docs/installer.md)).

## Documentation

- [docs/x64.md](docs/x64.md): the 64-bit build (`__ptr32` fields, low-memory
  layout, debugging, checklist for pointer-size changes and the test harness)
- [docs/d3d12.md](docs/d3d12.md): the renderers (GX core, OpenGL, Direct3D 12)
- [docs/endian.md](docs/endian.md): byte-swapping the big-endian disc data
- [docs/hfr.md](docs/hfr.md): high frame rates (interpolation)
- [docs/detmath.md](docs/detmath.md): deterministic math
- [docs/stick.md](docs/stick.md): analog stick emulation
- [docs/audio.md](docs/audio.md): audio (MusyX synth, aux effects, output)
- [docs/language.md](docs/language.md): languages and text encoding
- [docs/installer.md](docs/installer.md): installer, game data extraction,
  installed/portable layout
- [docs/updater.md](docs/updater.md): in-game update check and the release
  procedure (`tools/make_release.sh`, SHA256SUMS.txt)

## Credits and licenses

The PC port and the decompilation it builds on are both released under the
[CC0 1.0 Universal](LICENSE) public domain dedication.

- The game code comes from the SMB2 decompilation
  ([girlyoulookthebest/smb2-decomp](https://github.com/girlyoulookthebest/smb2-decomp)), set up with
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) and
  [dtk-template](https://github.com/encounter/dtk-template) (CC0 1.0).
- [SDL2](https://libsdl.org) 2.30.9 (zlib license), linked statically.
- [miniaudio](https://miniaud.io) by David Reid (public domain or MIT-0),
  `third_party/miniaudio/`.
- [stb_image](https://github.com/nothings/stb) by Sean Barrett (public domain
  or MIT), `third_party/stb/`.
- fdlibm 5.3, Copyright (C) 1993 Sun Microsystems, Inc. (permissive notice in
  each file), `src/detmath/fdlibm/`.
- Controller button prompt icons: "Free Input Pack" by Julio Cacko
  (<https://juliocacko.gumroad.com/l/free-input-pack>), CC0 1.0,
  `assets/prompts/`.
- The installer is built with [NSIS](https://nsis.sourceforge.io) (zlib license).

The third-party license texts shipped with the game are in
[installer/LICENSE.txt](installer/LICENSE.txt).

## Legal

SMB2 is an unofficial fan project. It is not affiliated with, endorsed by or
sponsored by SEGA or Nintendo. *Super Monkey Ball* is a trademark of SEGA.
This repository contains only source code; no game assets are included or
distributed. To play, you must provide a disc image of a copy of the game
that you own.
