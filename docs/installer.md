# Installer support (port side)

This page describes the port side of the interface with the NSIS installer
(`installer/smb2.nsi`): the command line Setup runs, the files it writes and
how the game finds its data.

## Command line

### `--extract-iso "<iso>" --gamedata "<dir>" [--progress]`

Runs headless. It opens no window, starts no audio device and doesn't run the
game: it returns before SDL, the watchdog or the renderer start. It writes to
stdout only. Each line is flushed as soon as it's printed.

```
PROGRESS <done_bytes> <total_bytes>     (only with --progress; first line is "PROGRESS 0 <total>")
INFO extracted <n> files, <bytes> bytes (success only)
RESULT OK
RESULT ERROR <code> <message>           (one line)
```

- Ignore any line that isn't `PROGRESS` or `RESULT`. `INFO` lines may be added later.
- `total_bytes` doesn't change during a run. It's **twice the data size**,
  because each byte is counted once when written and once when read back to
  verify it. For a progress bar, use `done/total`.
- A `PROGRESS` line is printed every 16 MiB and at the end.

Exit codes are the same as the `RESULT` code:

| Code | Meaning | Examples |
|---|---|---|
| 0 | OK | |
| 2 | Not a GameCube disc image | Random file, Wii disc, file < 0x440 bytes |
| 3 | Image unreadable, corrupt or unsupported | Missing file, truncated image (an FST file lies beyond EOF), unreadable FST, RVZ/WIA/CISO/GCZ/WBFS/NKit (the message says to convert it to ISO with Dolphin) |
| 4 | Write error or disk full | Not enough free space (checked up front, message gives MB needed/free), ENOSPC or other write failure, a file that doesn't verify after writing, the final rename fails |
| 5 | Other | Usage error; the target exists but isn't a game data folder (non-empty, no `manifest.txt`) — refused and left untouched |

**Accepted images:** any plain `.iso`/`.gcm` GameCube disc image. The disc ID,
revision and contents are not checked, so asset mods (which often change the
ID) work; the ID is only logged (`[iso] <path>: disc ID ...`).
Compressed formats (RVZ, WIA, CISO, GCZ, WBFS, NKit) are detected and rejected
with exit code 3.

**Extraction steps:**
1. Write everything to `<dir>.partial` (a sibling of `<dir>`). It contains the
   marker file `.smb2-extract-partial`. A stale `.partial` from an interrupted
   run is deleted first, but only if it has the marker or is empty.
2. Write `manifest.txt`.
3. Read every file back and compare it with the CRC32 taken from the image.
4. Rename the old `<dir>` to `<dir>.old`, rename `.partial` to `<dir>`, then
   delete `<dir>.old`. A previous `<dir>` is replaced only if it has a
   `manifest.txt` or is empty.

**On failure:** the `.partial` folder is deleted. If the process is killed,
only `<dir>.partial` is left, and the next extraction cleans it up. A
previously valid `<dir>` is never touched before step 4.

### `--verify-gamedata "<dir>" [--progress]`

- Same output format as `--extract-iso`. Here `total_bytes` is the data size
  itself (not doubled).
- `RESULT OK`, or `RESULT ERROR 6 <message>`.
- Exit code 6 means: `manifest.txt` is missing or incomplete, a file is
  missing, or a file has the wrong size or CRC32. It also covers
  `sys/fst.bin` disagreeing with the manifest. The CRCs come from the image
  that was extracted, so extracted mods verify like the original.
- Exit code 5 is a usage error.

### `--gamedata "<dir>"` (normal runtime)

Loads the game from an extracted folder instead of an ISO. The folder must
pass the quick check: the manifest parses (any disc ID), every
listed file exists with the listed size, and the FST is consistent with the
manifest. The CRCs are not checked at startup.

The folder backend uses the disc's own FST (`sys/fst.bin`). DVD entry
numbers, names, sizes, directory order and reads past EOF (zero-filled) are
therefore identical to running from the ISO.

## Folder layout

```
<dir>/manifest.txt
<dir>/sys/boot.bin      disc 0x0000-0x0440 (header)
<dir>/sys/bi2.bin       disc 0x0440-0x2440
<dir>/sys/fst.bin       the disc FST
<dir>/files/...         all 3009 FST files, same paths as on the disc
<dir>/icon.ico          generated: 16/32/48/64/128 (32-bit BMP) + 256 (PNG)
<dir>/icon_256.png      generated
```

- **apploader / main.dol:** not extracted. The port never reads them:
  `main.dol`'s code and data are compiled in, and the disc ID is fixed.
- **Size:** retail GM2E8P is 3014 manifest entries, 1,037,446,520 bytes
  (about 0.97 GiB). `manifest.txt` is about 140 KB.

`manifest.txt` is UTF-8/ASCII, one `key=value` per line:

```
SMB2PC-GAMEDATA 1
disc_id=GM2E8P
rev=0
game_name=SUPER MONKEY BALL 2
fst_crc32=76b4d36b
files=3014
total_bytes=1037446520
icon=icon.ico                 (only when the icon was generated)
icon_png=icon_256.png
file=<crc32 hex> <size> <path relative to dir, '/' separators>
...
end
```

## Game icon

The icon is AiAi from the disc's memory-card save icon: `/memcard/sys.bin`,
offset 0x1800, 32x32 RGB5A3. It is generated from the user's own disc and is
never shipped.

| Size | How it's made |
|---|---|
| 16 | 2x2 box filter of the original |
| 32 | Original |
| 48 | Area downsample of the 64 |
| 64 / 128 / 256 | Scale2x applied 1 / 2 / 3 times |

- All filtering is alpha-premultiplied.
- At runtime, the window and taskbar icons (`WM_SETICON` big/small at the
  system metrics sizes) are rendered the same way from the disc or folder in
  use, in every mode including portable `--iso`. On a non-Windows build, SDL
  gets the 64 px version.

## install.ini (installed mode)

The port reads `install.ini` from the exe's folder:

- It's UTF-8 (a BOM is OK) with `key=value` lines. Lines starting with
  `#`/`;`/`[` are ignored, as are surrounding quotes.
- Values are converted to the ANSI code page for the CRT file APIs.
- Only `mode=installed` turns installed mode on.
- `%APPDATA%`, `%LOCALAPPDATA%` and `%PROGRAMDATA%` are expanded with
  `SHGetKnownFolderPath`. Any other `%VAR%` is expanded with `getenv`.
- A relative path is taken as relative to the exe folder.
- If `userdata` is missing, it defaults to `%APPDATA%\SMB2\installed`.

What Setup writes (since 1.0.4; 1.0.0-1.0.3 wrote `userdata=%APPDATA%\SMB2 PC Port`):

```
mode=installed
gamedata=gamedata                     (= <install folder>\gamedata)
userdata=%APPDATA%\SMB2\installed
version=1.0.4
```

**Why `%APPDATA%\SMB2\installed` and not `%APPDATA%\SMB2`:** the portable
build keeps settings.dat and the shader cache in SDL's pref path
`%APPDATA%\smb2\smb2_pc_port\`, and Windows paths are case-insensitive, so
`%APPDATA%\SMB2` is that folder's parent. Putting the installed game's files
in their own subfolder means the two never share a file (both write
`settings.dat`), and the uninstaller can delete the installed game's saves
with one recursive delete of `installed\` without touching the portable
settings. `%APPDATA%\SMB2` itself is removed only when empty.

`gamedata` is relative, so it always means the `gamedata` folder next to the
exe, wherever the install folder is. 1.0.0 wrote the absolute path
`C:\ProgramData\SMB2 PC Port\gamedata`; the port accepts both.

What changes in installed mode:

| Item | Portable (no install.ini) | Installed | Installed + `--save-dir D` |
|---|---|---|---|
| settings.dat, d3d12_shader_cache.bin | SDL pref path (`%APPDATA%\smb2\smb2_pc_port\`) or `--save-dir` | userdata (`%APPDATA%\SMB2\installed\`) | D |
| memcard\slotA, memcard\backup | `<exe dir>\memcard\` (`--save-dir` doesn't move it) | `<userdata>\memcard\` | `D\memcard\` |
| logs | `<exe dir>\logs\smb2_pc_port.log` (1.1.14+; console if that folder isn't writable) | `<userdata>\logs\smb2_pc_port.log` (previous run in `smb2_pc_port.old.log`), only when stdout is a console. Redirected output is left alone. | `D\logs\` |

The userdata folder is created with all its parents. It isn't created when
`--save-dir` is given.

### Migration from the old name (port side, 1.0.4)

Up to 1.0.3 the app was called "SMB2 PC Port". On start in installed mode
(`install_mode.c`, `migrate_old_names`; not with `--save-dir`, never in
portable mode), and only when install.ini's `userdata` is exactly the default
`%APPDATA%\SMB2\installed` (so a custom or test `userdata` never pulls in
the real profile folders):

| Old | New | When |
|---|---|---|
| `%APPDATA%\SMB2 PC Port\` | the userdata folder | the new folder has no `settings.dat` and no `memcard\` |
| `%LOCALAPPDATA%\SMB2 PC Port\gamedata\` | `%LOCALAPPDATA%\SMB2\gamedata\` | the old one has our manifest and the new one doesn't; the empty old parent is removed |

- A move is one rename. If that fails (e.g. the new folder exists but has no
  saves, only `logs\`), the files are copied one by one, never overwriting a
  file in the new folder, and each copied file is deleted from the old
  folder. Files that could not be moved stay in the old folder.
- Every step is printed as an `[install] migrated ...` / `[install] not
  migrating ...` line on stdout and repeated in the log file once logging
  to `<userdata>\logs` starts.
- Tests: `SMB2_PC_TEST_OLD_USERDATA` / `SMB2_PC_TEST_OLD_LOCAL_GAMEDATA` name
  the old folders; when either is set, the real ones are never used
  (`installer/test/port_migration_test.sh`).

## Windows resources (exe)

`src/res/smb2_pc_port.rc.in` (configured by CMake, compiled by windres for
i686 and llvm-mingw's windres for x86_64): VERSIONINFO with FileDescription and
ProductName "SMB2", CompanyName "unofficial PC port", FileVersion/ProductVersion
from `-DSMB2_VERSION` (installer/build.sh passes its `--version`; default
`0.0.0-dev`), and the generic `installer/smb2.ico` as the exe icon for
Explorer. The window/taskbar icon is made from the disc at runtime. The
exe file name stays `smb2_pc_port.exe`. The window title is "SMB2".

## Installed layout (Windows installer, `installer/smb2.nsi`)

| What | Where | Written by |
|---|---|---|
| Program, `assets\`, `install.ini`, README, uninstaller | `%ProgramFiles%\SMB2\` | Setup (elevated) |
| Extracted game data + disc icon | `%ProgramFiles%\SMB2\gamedata\` | Setup (elevated) runs `--extract-iso`; read-only at runtime |
| Saves, settings, shader cache, logs | `%APPDATA%\SMB2\installed\` | the game |
| Recovery copy of the game data | `%LOCALAPPDATA%\SMB2\gamedata\` | the game (recovery flow, no admin) |
| Start Menu / desktop shortcut | `SMB2.lnk` in the all-users Programs folder / desktop | Setup |
| Add/Remove Programs | key `HKLM\...\Uninstall\SMB2PCPort`, DisplayName "SMB2"; settings key `HKLM\Software\SMB2PCPort` | Setup |

Names: the app is "SMB2" since 1.0.4 (the full game title is used only to
describe the disc the user needs). The registry keys use the stable ID
`SMB2PCPort`, which no other program is likely to use. The setup file keeps
its name `SMB2_PC_Setup_<version>.exe`, so existing links and scripts still
work.

- **Disk space:** the directory page's "Space required" includes the game
  data. The free-space check before extraction is done on the install drive.
- **Shortcuts and Add/Remove Programs icon:** `gamedata\icon.ico` once the
  game data is there, else the generic `smb2.ico`. `EstimatedSize` includes
  the game data when `gamedata\manifest.txt` exists.
- **Upgrade from 1.0.0** (game data in `%ProgramData%\SMB2 PC Port\gamedata`):
  1. Setup runs the new exe's `--verify-gamedata` on the old folder (only if
     it has our `manifest.txt`). It does this only when the install folder
     has no game data that verifies.
  2. If the old data verifies and is kept (the default; silent mode always
     keeps it unless `/REEXTRACT`), Setup moves it to `<install>\gamedata`:
     - Same drive: one rename. The permissions are then reset to the install
       folder's (`icacls /reset /T`), because users may create files in
       `%ProgramData%`.
     - Otherwise: copy to `gamedata.partial` (with the port's
       `.smb2-extract-partial` marker), `--verify-gamedata` the copy, then
       rename it to `gamedata`.
     Then it deletes the old folder and `%ProgramData%\SMB2 PC Port` if empty.
  3. If the move fails, the old folder is left alone. Setup extracts from the
     disc image instead: in silent mode only if `/ISO=` was given; in the
     wizard it asks for the image. A later Setup run deletes the old folder
     once the install folder has valid game data.
  4. If the old data doesn't verify, or isn't kept, it's deleted. Then Setup
     extracts as usual (or not, with skip).
- **Upgrade from the old name** (1.0.0-1.0.3 "SMB2 PC Port"): see below.
- **Uninstall:** removes the files Setup installed, `assets\`, and
  `gamedata\` (plus `gamedata.partial` and `gamedata.old`), the 1.0.0
  `%ProgramData%` game data, the `%LOCALAPPDATA%` recovery copy (new and old
  name) and game data an upgrade could not move (`OldInstallDir`).
  "Also delete your saves and settings?" (or `/REMOVEUSERDATA`) deletes
  `%APPDATA%\SMB2\installed` and, if the game hasn't moved it yet,
  `%APPDATA%\SMB2 PC Port`; `%APPDATA%\SMB2` only if empty (it may hold the
  portable build's `smb2_pc_port\`). A game
  data folder is deleted recursively only if its `manifest.txt` starts with
  `SMB2PC-GAMEDATA 1`. A `.partial` is deleted only if it has the marker.
  Anything else is removed only when empty. The install folder is removed
  only if it's empty afterwards, so files the user added there are kept.

### Upgrade from the old name ("SMB2 PC Port", Setup 1.0.4)

1. **Detection** (`FindOldInstall`): the old uninstall key's
   `InstallLocation` (`...\Uninstall\SMB2 PC Port`, TESTMODE
   `HKCU\Software\SMB2PCPortTest\Uninstall`) if it holds `smb2_pc_port.exe`;
   else the old default folder `%ProgramFiles%\SMB2 PC Port` (also the x86
   one) if it holds the exe and `install.ini`; else `OldInstallDir` in the new
   settings key (game data an earlier upgrade could not move).
2. **New folder:** `<parent of the old folder>\SMB2` (so
   `%ProgramFiles%\SMB2` for a default install, and the same drive as the old
   folder), unless `/D=` is given or the user picks another on the directory
   page, which says that the old install was found and what happens to it.
   If the new install is chosen in the old folder itself, it's an in-place
   upgrade: only the old shortcuts and registry keys are removed.
3. **Game data:** after the install folder and the 1.0.0 `%ProgramData%`
   location, the old folder's `gamedata\` is checked with
   `--verify-gamedata`. If it verifies and is kept (default; the ISO page
   says it's moved), `MoveGameData` moves it exactly like the 1.0.0
   migration: same volume = rename + `icacls /reset /T`, else copy to
   `gamedata.partial` + verify + rename. If it doesn't verify, or the user
   re-extracts, it's deleted (our manifest only). The even older layout (an
   old install whose install.ini points to `%ProgramData%`) is handled by the
   1.0.0 migration in the same run.
4. **Removing the old install** (`RemoveOldInstall`, after the new files are
   in place): waits until the old exe is closed, deletes exactly the files the
   old Setup installed (exe, `assets\`, install.ini, README, LICENSE,
   smb2.ico, uninstall.exe), its `gamedata\` only if it has our manifest, the
   folder only if it is then empty (files the user added are kept), the old
   `SMB2 PC Port.lnk` shortcuts, the old uninstall key and the old
   `Software\SMB2 PC Port` key, so Add/Remove Programs lists one entry.
5. **If moving the game data fails**: the old `gamedata\` is kept (the old
   program files, shortcuts and keys are still removed), its folder is
   recorded as `OldInstallDir`, and Setup extracts from the disc image (silent:
   only with `/ISO=`; wizard: the extraction page asks). The next Setup run
   (or the uninstaller) deletes it once the new folder has valid game data.
6. Saves and settings are not touched by Setup (it runs elevated, maybe as
   another user); the game moves them on its first start (see "Migration from
   the old name" above).

## Runtime search order

1. `--gamedata`
2. `--iso`
3. Installed mode only:
   1. the `install.ini` gamedata, if it passes the quick check;
   2. `%LOCALAPPDATA%\SMB2\gamedata` (a copy made earlier by
      recovery, or moved there from the old name's folder), if it's valid;
   3. the ISO path saved in settings.dat;
   4. the recovery flow.
4. Portable mode: the ISO path saved in settings.dat, then the
   disc image file dialog (asked until a readable disc image is picked;
   cancel exits with code 1). The picked path, and a working `--iso`, are
   saved as settings.dat `iso=`.

All of this runs before the game window is created, so the dialog and
message boxes can't end up behind a (fullscreen) game window. Images are
checked like `--extract-iso` does (RVZ/NKit/Wii discs are refused with
the reason).

**Recovery flow:**
1. The port explains what's wrong on the console and in a message box with
   *Select ISO...* / *Quit*.
2. It asks for an ISO in the file dialog (console prompt on non-Windows).
3. It extracts the ISO into `%LOCALAPPDATA%\SMB2\gamedata`. This
   needs no admin rights. The 5% progress lines go to the console (the game
   window doesn't exist yet).
4. It loads the extracted copy.

If the image is unusable or extraction fails, it shows the error and asks
again. Cancelling exits with code 1.

The environment variable `SMB2_PC_LOCAL_GAMEDATA` overrides the recovery
target. It exists for tests.

## Test hooks

| Variable | Effect |
|---|---|
| `SMB2_DVD_TRACE=1` | Logs every game file read: `[dvdtrace] <entry> <name> ofs= len=` |
| `SMB2_TEST_WRITE_LIMIT=<bytes>` | Extraction writes fail with ENOSPC after that many bytes (disk-full simulation) |
| `SMB2_PC_LOCAL_GAMEDATA=<dir>` | Overrides the recovery extraction target |
| `SMB2_ISO_PICK_TEST=<path>\|<path>\|cancel` | Answers the disc image picker instead of the file dialog, one entry per ask (none left = cancel); message boxes become log lines |
| `SMB2_PC_TEST_OLD_USERDATA=<dir>` | Old-name userdata folder to migrate (instead of the real `%APPDATA%\SMB2 PC Port`) |
| `SMB2_PC_TEST_OLD_LOCAL_GAMEDATA=<dir>` | Old-name recovery copy to migrate (instead of the real one) |

## Tests

- `installer/test/run_tests.sh <TESTMODE setup> <root> [real]`: the fake-port
  suite (every Setup path; set `SMB2_OLD_SETUP` to a TESTMODE setup built
  from the pre-1.0.4 `smb2.nsi` for the old-name upgrade tests 15-21) and,
  with `real`, the real-ISO suite (`SMB2_TEST_ISO` = the disc image as a
  Windows path). Everything below the root is deleted, so the root must be a
  dedicated folder: empty on first use (the scripts then mark it with
  `.smb2_test_root`) and outside the source checkouts.
- `installer/test/port_migration_test.sh <exe> <root>`: the port-side
  userdata / recovery-copy migration with the real exe and disc image.

## What the port reads

- Every game read goes through `iso_file_read_at(entry, offset, len)`, which
  the shim uses for DVDRead*, the whole-file loader and the ARAM font.
- Outside the FST, the port reads only the disc header (0x000-0x440) and the
  FST itself, both in `iso_open`.
- The port never reads bi2, the apploader or main.dol.
- The disc ID at 0x80000000 is fixed as "GM2E8P" (gc_mem.c).
- `DVDOpen` of `mkb2.*.rel` is intercepted, but the names must exist in the
  FST.

`SMB2_DVD_TRACE=1` logs every read. Across all harness modes the game reads
only a few hundred of the FST files (under 15% of the data), but story
stages, other party games and replays read files the harness doesn't reach,
so the extraction copies the complete FST tree.

The trace line includes the CRC32 of the bytes the caller received:

```
[dvdtrace] <entry> <name> ofs=0x.. len=0x.. crc=xxxxxxxx
```

The same (entry, ofs, len) must produce the same crc from the ISO and from
the folder backend.
