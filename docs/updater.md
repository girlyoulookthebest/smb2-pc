# In-game updater

The game checks the GitHub Releases of its repository for a newer version,
shows the release notes in a box in the game's own style and lets the player
decide: **Update now**, **Later** or **Skip this version**.

Code: `src/platform/pc_update.c` (model, network, SHA-256; `include/pc_update.h`),
`src/game/shim/pc_update_ui.c` (notes box, Updates screen), hooks in
`pc_quit.c` (safe moment), `video_shim.c` (modal run), `pc_settings_menu.c`
(Updates row), `pc_settings.c` (settings.dat), `installer/smb2.nsi`
(`/UPDATE`, `/WAITPID`), `tools/make_release.sh` (release assets).

## Configuration (build time)

| CMake option | Default | |
|---|---|---|
| `SMB2_VERSION` | `0.0.0-dev` | the exe's own version (`1.2.3`, `1.2.3-rc1`); also VERSIONINFO, the log line `[update] SMB2 <version> (installed\|portable)` and the F1 overlay (top right) |
| `SMB2_UPDATE_REPO` | empty | `owner/name` on GitHub. Empty = the updater is compiled in but disabled: no network access at all, the Updates screen says "Update checks are not set up in this build" |

`installer/build.sh --version 1.2.3` passes the version to CMake when it builds
the exes. With `--exe32/--exe64/--no-build` it packages exes as they are, so
build those with `-DSMB2_VERSION=1.2.3` (and the repo) yourself:

```sh
cmake -S . -B build64 ... -DSMB2_VERSION=1.2.3 -DSMB2_UPDATE_REPO=owner/smb2
```

Both definitions reach only `pc_update.c`, so changing them rebuilds one file.

## What it does

1. **Check** (background thread, WinHTTP, Windows only):
   `GET https://api.github.com/repos/<repo>/releases/latest` with
   `User-Agent: SMB2-PC/<version>` and the GitHub `Accept` header, nothing
   else (no IDs, cookies disabled). Timeouts 5 s resolve/connect, 8 s send,
   10 s receive; startup never waits for it. Offline, timeouts, HTTP errors
   and unparsable replies are only logged (`[update] check failed: error N`).
   Drafts and pre-releases are never "latest". The tag (`v1.2.3` or `1.2.3`)
   is compared as semver with `SMB2_VERSION` (1.2.3 > 1.2.3-rc.2 > 1.2.3-rc.1
   > 1.2.2).
2. **When**: at start if "Check automatically" is On (default) and the last
   answered check is 24 h old or more (`update_last` in settings.dat; a check
   that got no answer, e.g. offline, does not count). Never in automated runs
   (`--input-script`, `--dump-interval`, `--exit-frame`) unless the test hook
   is set. "Check Now" on the Updates screen checks any time.
3. **Box** (automatic check): only at a safe moment. `pc_menu_quit_tick`
   (pc_quit.c, called by the title screen and Mode Select) requests it when
   the screen is idle (the same condition that allows its Quit box: real title
   screen with PRESS START, or Mode Select with no transition), no Quit box is
   open, no quit is pending and no memory card job is running
   (`pc_memcard_io_busy`). `video_finish_frame` then runs the box modally over
   the frozen, dimmed frame, like the F1 overlay. Never during a stage, a
   cutscene, the attract demo or a card job. Shown once per run; not for a
   version the player skipped (`update_skip` in settings.dat). After "Check
   now" the box opens for any newer version, skipped or not.
4. **Contents**: "Update available", "Version 1.2.3 (you have 1.2.2)", the
   release title, the release body as plain text (Markdown headings in gold,
   `**`/`_`/`` ` ``/links/HTML tags removed, lists kept with a hanging indent,
   entities decoded, emoji dropped, typographic quotes/dashes made ASCII), word
   wrapped to the box with the menu font and scrolled with Up/Down (stick or
   d-pad, with repeat; L/R page). The notes stay as written; the labels are in
   the game's language (en, de, fr, es, it, ja). Every `/` is printed as its
   own piece because the game's text parser reads `a/`, `p/`, `c/0x` and
   `/bc...` as control codes.
5. **Choices** (Left/Right, A; B = Later):
   - **Update now** (installed mode = `install.ini` next to the exe):
     downloads `SHA256SUMS.txt` of the same release (refused if missing or if
     it does not list the setup: "No checksum ... not installing"), then
     `SMB2_PC_Setup_<ver>.exe` to `%TEMP%\SMB2_update\` (`.part`, progress bar,
     B cancels) hashing it on the fly; size and SHA-256 must match, otherwise
     the file is deleted and the box says so. Then Setup is started with
     `ShellExecuteEx` (`/UPDATE /WAITPID=<pid>`): Windows shows the admin
     prompt (declined = "Setup needs administrator rights", the game keeps
     running). Once Setup runs the box shows "Setup is running. The game
     closes now." and calls `pc_quit_request("update")`; pc_quit waits until
     no memory card job is running, saves settings.dat and exits. Setup waits
     for the game's process to end.
   - **Open download page** instead of Update now in portable mode (no
     install.ini): opens the release's `html_url` in the browser (https only).
     Also offered after a failed download.
   - **Later**: closes; the box comes back on the next start (subject to the
     daily limit).
   - **Skip this version**: remembered; no automatic box for that version
     (a newer one shows again).

## Settings placement

Options > Game Screen has one row for the updater, **Updates**, after
"Controllers": its value shows the automatic check (On/Off) and A opens the
**Updates screen** (version, installed/portable, repo, "Check automatically"
On/Off, "Check Now", Back, status of the last check). The page uses a 19 px
row pitch so its 15 rows (2 game rows, 12 PC rows, Quit to Desktop) fit above
the description box. The F1 overlay has the same row (22 px pitch, 13 rows)
and shows the version at the top right. Script token `UPD` opens the Updates
screen.

settings.dat keys: `update_check=1`, `update_last=<unix time>`,
`update_skip=<version>`.

## Setup on top of an existing install

A normal Setup run on an installed copy upgrades in place: the folder comes
from the uninstall key (directory page skipped), `assets\` is replaced, the
exe/README/license/install.ini are rewritten, verified game data in
`$INSTDIR\gamedata` is kept ("Keep the existing game data"), and the per-user
data in `%APPDATA%\SMB2\installed` (saves, settings, shader cache, logs) is
never touched. Interactively it still shows Welcome, License, Components and
the disc image page and ends on the Finish page.

Switches used by the updater (installer/smb2.nsi):

- `/WAITPID=<pid>`: first waits (max 60 s) for that process to exit, so the
  game's exe is not locked.
- `/UPDATE` (only when an install exists, otherwise ignored): skips Welcome,
  License, Components and the disc image page (keeps verified game data; if
  there is none, the game asks for the disc), keeps the desktop shortcut only
  if it exists, closes the progress page automatically and starts the game
  (through Explorer, as the normal user) instead of showing the Finish page.
  With `/S` it starts the game after a successful silent update.

## Release procedure

1. Pick the version, e.g. `1.2.3`; write the release notes (Markdown, they
   are shown in the game).
2. Build both exes with the version and the repo, then the setup and the
   zips:
   ```sh
   cmake build32 -DSMB2_VERSION=1.2.3 -DSMB2_UPDATE_REPO=owner/smb2 && ninja -C build32
   cmake build64 -DSMB2_VERSION=1.2.3 -DSMB2_UPDATE_REPO=owner/smb2 && ninja -C build64
   installer/build.sh --version 1.2.3 --no-build
   # SMB2_PC.zip (build32/bin) and SMB2_PC_x64.zip (build64/bin): exe + assets + README
   ```
3. Collect the assets and the checksums:
   ```sh
   tools/make_release.sh --version 1.2.3 --setup installer/out/SMB2_PC_Setup_1.2.3.exe \
       --zip32 SMB2_PC.zip --zip64 SMB2_PC_x64.zip
   ```
   It checks the setup name and the version inside each zip's exe and writes
   `release/v1.2.3/` with the three files and `SHA256SUMS.txt`.
4. Tag `v1.2.3` and create the GitHub release `v1.2.3` (not a pre-release,
   not a draft) with the notes as the body and these assets:
   `SMB2_PC_Setup_1.2.3.exe`, `SMB2_PC.zip`, `SMB2_PC_x64.zip`,
   `SHA256SUMS.txt`, e.g.
   `gh release create v1.2.3 release/v1.2.3/* --title "SMB2 1.2.3" --notes-file NOTES.md`.
   The asset names matter: the updater looks for `SMB2_PC_Setup_<tag without v>.exe`
   and `SHA256SUMS.txt`.
5. Check: an older installed build shows the box at the title screen ("Check
   now" on the Updates screen skips the daily limit).

## Test hooks

| Env | |
|---|---|
| `SMB2_UPDATE_TEST=<url>` | URL of a releases/latest JSON instead of api.github.com; enables the updater without a repo, also in automated runs, and ignores the daily limit |
| `SMB2_UPDATE_TEMP=<dir>` | download folder instead of `%TEMP%\SMB2_update` |
| `SMB2_UPDATE_DRYRUN=1` | log "would start Setup / open the page" instead of doing it (the quit still happens) |

`tools/updater_test/fakegh.py <port> <setup file>` serves fake releases on
127.0.0.1: `http://127.0.0.1:<port>/<scenario>/latest` with scenarios `same`
(1.0.9 = no update for a 1.0.9 build), `new`, `slow` (throttled download),
`badsum`, `nosums`, `noasset`, `dlfail` (connection dropped mid-download),
`404`, `500`. An unused port is the offline case. Installed mode is tested
with an `install.ini` next to a test exe, or a TESTMODE Setup
(`installer/build.sh --test-root`) installed under a test folder, whose
`SMB2_PC_Setup_<ver>.exe` the fake server offers for the real `/UPDATE` run.
