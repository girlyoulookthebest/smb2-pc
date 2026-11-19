SMB2 (installed version)
========================

SMB2 is an unofficial, non-commercial PC port of Super Monkey Ball 2 for the
Nintendo GameCube. It is not affiliated with SEGA or Nintendo.

You need your own Super Monkey Ball 2 GameCube disc image (USA, disc ID GM2E8P,
.iso or .gcm). No game content is included with this program.
Modded disc images work if the mod replaces game files (stages, textures,
sounds); code mods (DOL edits, Gecko/AR codes) don't apply.

Party games not playable yet
----------------------------
Billiards, Shot, Soccer and Tennis are not decompiled yet, so the PC version can't run
them. Party Game Select shows them dimmed with a "NOT YET" tag (they can still be unlocked
with Play Points). Race, Target, Bowling, Boat, Dogfight, Baseball, Golf and Monkey Fight work.

Starting the game
-----------------
Use the "SMB2" shortcut in the Start Menu (or on the desktop).
A console window opens next to the game; it shows the log, which is useful for bug reports.

Where files go
--------------
- Program:   C:\Program Files\SMB2\  (read-only; install.ini there tells the game it is installed)
- Game data: C:\Program Files\SMB2\gamedata\  (extracted from your disc image by Setup,
             about 1 GB, read-only; the game icon there is also generated from your disc)
  Setup manages the assets\ and gamedata\ folders; please don't keep your own files in them.
- Your saves, settings, shader cache and logs:  %APPDATA%\SMB2\installed\
  (type %APPDATA%\SMB2\installed into the Explorer address bar to open it)
  The portable (zip) version keeps its settings in %APPDATA%\SMB2\smb2_pc_port\ and its
  memory card next to its exe; the two versions do not share saves.

If you skipped the disc image step in Setup, or the game data is missing or damaged, the game
asks for your disc image in a file dialog when it starts and extracts it into
%LOCALAPPDATA%\SMB2\gamedata (no administrator rights needed).
To redo it for all users, run Setup again and pick the disc image.

Updates
-------
When a new version is published, the game shows its release notes at the title screen
(at most one check a day; only the version number is sent). "Update now" downloads the new
Setup, checks it (SHA-256) and runs it after the game has closed; Windows asks for
administrator rights. Your saves, settings and game data are kept. "Skip this version" hides
that version. Options > Game Screen > Updates (or F1) turns the check off or checks now.

Saves from Dolphin or a GameCube memory card
--------------------------------------------
The memory card is the folder %APPDATA%\SMB2\installed\memcard\. The game imports
GameCube save files (.gci) and whole memory card images (.raw); only the Super Monkey
Ball 2 (USA, GM2E8P) game data save is used (replays and saves of other games are skipped).
- Dolphin: Tools > Memory Card Manager > select the Super Monkey Ball 2 save > Export GCI.
  Or use Dolphin's whole card file (MemoryCardA.USA.raw).
- A real memory card: dump it with the GCMM homebrew on a softmodded Wii or GameCube
  (to .raw or .gci).
Then either:
- Options > Game Screen > Memory card (or F1) > Import a save..., pick the file. An existing
  save is copied to memcard\backup\ first (the game asks before replacing it). The game then
  closes; start it again to play with the imported save.
- or put the file in %APPDATA%\SMB2\installed\memcard\import\ and start the game; it is
  imported and moved to import\done\.
- or run: smb2_pc_port.exe --import-save <file>
"Export saves..." (or smb2_pc_port.exe --export-save <folder>) writes .gci files for
Dolphin's Memory Card Manager or for GCMM.

Uninstalling
------------
Settings > Apps (or Control Panel > Programs and Features) > SMB2 > Uninstall.
This removes the program and the extracted game data. You are asked whether to also delete
your saves and settings; by default they are kept. Files you added to the program folder
yourself are not deleted.

Upgrading from "SMB2 PC Port" (versions before 1.0.4)
-----------------------------------------------------
Until version 1.0.3 the program was called "SMB2 PC Port" (C:\Program Files\SMB2 PC Port).
Setup finds such an installation and installs SMB2 next to it (C:\Program Files\SMB2, or the
folder you choose). It moves the game data over (it is checked first, so you don't need the disc
image again), removes the old program files, shortcuts and the old "SMB2 PC Port" entry in
Settings > Apps. Files you added to the old folder yourself are kept.
The first time SMB2 starts, it moves your saves and settings from %APPDATA%\SMB2 PC Port to
%APPDATA%\SMB2\installed (and a game data copy from %LOCALAPPDATA%\SMB2 PC Port to
%LOCALAPPDATA%\SMB2). If %APPDATA%\SMB2\installed already has saves or settings, nothing is moved.

Upgrading from version 1.0.0
----------------------------
Version 1.0.0 put the game data in C:\ProgramData\SMB2 PC Port\gamedata. Setup checks it and
moves it into the program folder (no need to choose the disc image again); if it is damaged,
Setup deletes it and extracts the game data again from your disc image.

Controls
--------
  GameCube         Xbox         PlayStation        Switch      Keyboard
  A / B / X / Y    A / B / X / Y  Cross / Circle /  A / B / X / Y  Z / X / C / V
                                  Square / Triangle
  L / R            LT / RT      L2 / R2            ZL / ZR     Q / E
  Z                RB           R1                 R           Space
  START/PAUSE      Menu         Options            +           Enter
  Control Stick    left stick   left stick         left stick  W A S D
  C Stick          right stick  right stick        right stick -
  +Control Pad     d-pad        d-pad              d-pad       arrow keys

  Alt+Enter : toggle fullscreen      F1 : PC settings overlay

- Quit to Desktop: title screen B (or Esc), Mode Select X (or Esc), the pause menu,
  Options > Game Screen, or the F1 overlay. It asks "Quit to Desktop?" (Yes / No).

- Xbox and PlayStation pads are mapped by button position (the bottom button is GameCube A).
  Switch pads are mapped by label, like Nintendo's own games.
- The triggers work like GameCube triggers: analog, plus a full "click" near the end of the pull.
- Button prompts follow the controller in use; to choose: Options > Game Screen > Buttons,
  or the F1 overlay.
- Video options: Options > Game Screen (resolution, fullscreen, aspect, vsync, ...).

Credits
-------
See LICENSE.txt. Controller button icons: Free Input Pack (Universal Version) by juliocacko,
https://juliocacko.gumroad.com/l/free-input-pack (CC0 1.0).
