# High frame rate rendering (HFR)

Shows more than 60 frames per second (120 / 144 / 165 / 240 / the display's
refresh rate / uncapped) on high-refresh monitors without changing the
game's speed or logic.  The decompiled game is a fixed 60 Hz tick
(GameCube VI rate): every tick advances physics, cameras, animation, timers
and effects by exactly one step.  HFR keeps the 60 Hz tick untouched and
draws extra frames *between* ticks by replaying what the last two ticks
drew, with transforms, vertices, lights and colours blended.

At the frame rate setting **60 (the default) nothing is recorded or
replayed** (section 9).

## 1. Using it

* **Options > Game Screen > Frame rate** and the **F1 overlay** row
  "Frame rate": `60` (default, no interpolation) -> `Display (N)` (the
  monitor's refresh rate) -> `120` -> `144` -> `165` -> `240` ->
  `Uncapped`.  Applies immediately (next tick), no restart.
* `settings.dat`: `fps=60|display|120|144|165|240|uncapped|<n>`.
* Command line: `--fps 60|120|144|240|display|uncapped|<n>` (overrides
  settings.dat for the run).
* V-Sync on: the target is capped at the display refresh (asking for more
  presents than the display shows would only block).  `Display` with V-Sync
  on is the recommended setting for a 120/144/240 Hz monitor; with V-Sync
  off (tearing allowed) or on a VRR (G-Sync / FreeSync) display any target
  works and `Uncapped` draws as many in-between frames as fit.
* A 60 Hz display with `Display` gives 60 = off.

## 2. How a frame flows

```
decomp main_loop.c loop (one iteration = one 60 Hz tick):
  gx_start_new_frame (gx_core.c)       present of the previous tick frame;
                                       hfr_select (proxy on/off), hfr_tick_begin
  inputs, mode_tick(), pools, events   (logic - unchanged)
  polydisp_main() + draw_ui()          (3D, EFB-copy passes, 2D) -> GX calls
                                       -> gx_core -> GxGpu proxy (recorded)
  video_finish_frame (video_shim.c)    hfr_tick_end (matching), audio, input,
                                       F1 overlay, then the pacer:
                                       in-between frames (replays at alpha j/k),
                                       the tick frame (replay at alpha 1),
                                       wait for the 60 Hz deadline
```

### Recording (src/render/gx_hfr.c)

A `GxGpu` proxy sits in front of the real backend (gx_core.c selects it at
each tick start when the frame-rate setting is not 60).  While a tick's
stream is open it records every replayable call: viewport, scissor, blend,
depth, cull, clears, texture binds, texture sampler state, asynchronous EFB
copies (`copy_tex`) and draws (vertices, 2D quad UV rects, a copy of
`GxDrawConsts`, `GxTevConsts` once per serial).  Two streams are kept: the
previous tick (P) and the newest (C).

**Deferred recording.**  Recorded calls are not sent to the GPU when they
arrive.  The tick frame itself is drawn by the replay at alpha = 1, so the
main pass is not submitted twice.  Recorded calls are sent early ("flush")
only when something needs the frame so far: an EFB copy or readback (the
offscreen pass it copies), a texture re-upload (draws before it must see
the old contents), present, frame capture (F1, frame dumps).  If memory
runs out while recording, everything is sent at once from then on and the
tick is not replayed.  `SMB2_HFR_DEFER=0` disables deferral (A/B checks).

Texture destruction is deferred by two ticks (both streams may reference
the texture).  A texture re-uploaded during tick C that stream P uses
("hazard") keeps P from being drawn (in-between frames then use C only).

### Matching (hfr_tick_end)

Each draw gets two keys.  The full key: identity tag (section 3),
primitive, vertex count, 2D flag, draw modes, and the state the draw
actually reads: the textures / texgen masks of the texture maps and
coordinates its active TEV (and indirect) stages use (texture maps 0-2 and
coordinate 0 on the fixed paths), and the TEV / channel / indirect
configuration of the active stages only (without the animated values:
lights, konst and register colours).  GX keeps the setup of unused stages
and texgens from whatever was drawn before, so hashing it gave an object
another key whenever an earlier draw changed (a dogfight monkey passing the
camera then fell back to the loose key and broke into pieces).  The loose
key: tag, primitive, vertex count, 2D flag.

1. Per full key, the draws of P and C pair **in order** when that is
   consistent: same count, and every pair is the same mesh (model draws:
   equal model-space first and last vertex) or (CPU-transformed draws)
   close in view space, both relative to the group's mean move: a skinned
   monkey passing the camera moves farther in one tick than its pieces are
   apart, and copies of one model drawn in another order (a model pair
   that left the group's move while a copy of the same orientation is much
   nearer) are not in order.  When draws appeared / disappeared inside the
   group, the runs at its start and end that are still in order pair in
   order (a run starts at a pair that stayed close and continues while the
   same mesh moves like its predecessor); the rest goes on to step 2.
2. Otherwise **nearest first**: candidate pairs within the jump distance,
   sorted by a distance made of the view-space move, the model-space
   first/last vertex distance x4 (the same mesh is ~0, a neighbouring strip
   or tile is not) and the orientation difference.  Model draws only pair
   with the same mesh (first/last vertex within 2%).  Groups too big for
   all pairs (hundreds of particles / tiles) search a window around the
   proportional position.
3. Leftovers pair through the loose key (texture flipbooks), strictly.
4. Every pair is analysed: a jump (view-space move > 4 + 25% of the
   distance, rotation > 150 deg, 2D move > 160 EFB px, 2D UVs differ, 2D
   vertex count differs) unpairs it.  Equal constant blocks = nothing to
   blend; a rigid transform turning > 3 deg = slerp; vertices that differ
   = vertex blend; TEV lights / colours that differ = TEV blend.
5. A camera cut (game hook, a camera view jump, or > 50% of the paired 3D
   draws jumping) unpairs everything.

Cost: ~0.3-1 ms per tick for 7500 draws (sorts, one memcmp of the vertices
per pair).

### Replay (hfr_render(alpha))

* alpha < 0.5 draws the **previous** stream's ops, alpha >= 0.5 the
  newest, each paired draw blended between its two ticks at alpha.
  Continuous motion is interpolated; **discrete changes happen at the
  midpoint**: an object appearing or disappearing, a timer digit changing,
  a sprite frame, a camera cut, text being typed.  3D and 2D change at the
  same moment, so the HUD never runs ahead of the scene.
* Matrices: projection lerp (same projection type), modelview / normal
  matrix element-wise below 3 deg of rotation per tick (exact for
  translation, consistent across the pieces of an object), **rotation slerp
  + per-axis scale lerp + translation lerp** above (spinning objects keep
  their size: element-wise blending shrinks a 60 deg/tick spin by 13% at the
  midpoint).  Mirrored transforms keep their reflection.
* Texture matrices element-wise; scroll translations that wrapped by whole
  repeats blend the short way, other jumps keep the base value.
* CPU-transformed vertices (skinned monkeys, effects, 2D sprites): position,
  normal and colour lerp; UVs as drawn.
* TEV: light positions / directions / colours, konst and register colours,
  material / ambient colours, post-texgen matrices lerp; draw constants: TEV
  colour registers, fog range and colour lerp.  Colour fades are smooth.
* EFB copies (`copy_tex`, docs/d3d12.md section 4): **re-executed in
  every replay** from the interpolated offscreen pass, texture only
  (`GXCOPY_NO_READBACK`, no readback, no CPU work; GC memory keeps the tick's
  copy, so game logic never sees in-between data).  A copy whose own
  texture was sampled earlier in the tick before any copy into it (a
  feedback effect) is not re-executed: it keeps the tick's result.

## 3. Game-side hooks (decomp `include/pc_hfr.h`, empty on GC)

| Hook | Where | Why |
|---|---|---|
| `HFR_TAG(BALL, i)` | ball_draw.c `g_draw_ball_and_ape` | ball + its monkey per player |
| `HFR_TAG(ITEM, item->id)` | item.c `draw_items` | bananas etc. (pool slot) |
| `HFR_TAG(STOBJ, index)` | stobj.c `g_draw_stobjs` | goal bags, bumpers, ... |
| `HFR_TAG(EFFECT, uid)` | effect.c `g_draw_effects` | particles: spawn / die without order shifts |
| `HFR_TAG(STAGE_IG, i)` | stage_draw.c `g_draw_stage_models` | animated stage parts per item group |
| `HFR_TAG(BG, ...)` | bg_lava.c embers, bg_wat2.c ambient particles, bg_pot.c bubbles | camera-space quads that all look alike: without a tag a particle that appears / disappears shifts the nearest-first pairing, and an in-between frame sweeps sparks between neighbours several units apart |
| `HFR_TAG(MINI, i)` | race2_2.c `fn_10_C69C` speed / recover plates | one mesh x 40 per course, drawn through the depth sort: the order changes as the camera approaches, and untagged in-between frames drew plates between neighbours or with pieces missing (flicker) |
| `HFR_TAG(MINI, player)` | race2_2.c `fn_10_D090` racer labels | one label model coloured per racer, depth-sorted: overtakes swapped the pairing and in-between frames showed a label in a blend of two racers' colours |
| `HFR_TAG(BG, list/index)` | background.c `g_render_foreground_objects` | stage background / foreground models repeat one mesh and are culled or blinked one by one (bowling marquee bulbs): each change shifted the pairing of the copies after it |
| `HFR_TAG(STAGE_INST, j)` | stage_draw.c `g_draw_stage_models`, model references and instances inside the item group's tag | many copies of one model (title-screen palms): one leaving the view made a palm crown fly between two palms |
| `HFR_NODE_ALLOC/BEGIN/END` | ord_tbl.c | depth-sorted draws (drawn later, in another order) keep the tag they were queued under |
| `HFR_VIEW(cam, view)` | camera.c `camera_apply_viewport[_2]`, default projection | camera view per tick: cut when it turns > 43 deg or moves more than 3x its speed + 3 units in one tick |
| `HFR_CUT(CAMERA_RESET)` | camera.c `camera_clear` | stage start, respawn, replay / mode camera resets.  Soft: cuts only if a camera view then moved more than 1.5x its recent speed + 0.5 units or turned > 10 deg (baseball's intro pan resets its camera every tick while panning smoothly) |
| main mode change | video_shim.c (PC side) | menus <-> game <-> minigames |

A tag change ends the pending vertex batch (gx_core `gx_hfr_flush`) so a
batch never spans two objects; with HFR off the hooks return at once.
Minigame-private objects (boat, race, baseball ...) need no tags: the
generic matching (nearest position + mesh identity) pairs them.

## 4. Frame pacing (video_shim.c `hfr_grid`, `hfr_subframes`)

### Display-grid pacer (V-Sync on, D3D12, display confirms the plan)

Used when V-Sync is on and the backend reports where each present went
(D3D12 swap chain frame statistics), otherwise - GL, V-Sync off, VRR,
`SMB2_PACE=timer` - the timer pacer below runs.

* Frames go out one every q refreshes, each showing the content of the
  moment it is on screen: `content(V) = N + (V - S_N - lat) / T` (tick N's
  logic starts at S_N, T = 16.67 ms), so the scene advances by the same
  amount per shown frame whatever the presents' CPU timing.  (The timer
  pacer takes the phase at the GPU start and the display rounds the frames
  to refreshes: on a 240 Hz display the cadence mixes 1, 2 and 3 refreshes,
  i.e. 60 Hz judder on scrolling surfaces.)
* q = refresh / requested rate, and only while that rate fits: CPU (logic +
  the tick's replays), GPU (its frames) and the flip queue (a tick's last
  frame is shown ~lat after the tick starts, its first is drawn after the
  logic; DXGI queues 3 presents).  Otherwise - e.g. `Display` / 240 on a
  240 Hz display, where one frame per refresh never fits 3 queued presents -
  the timer pacer shows as many frames as fit, as before.  Back on the grid
  after 2 s of headroom; 3 misses count as not fitting and double that wait
  (up to 32 s).  In practice the grid runs at 2 refreshes per frame (120 on
  240 Hz, 72 on 144 Hz) or at the full rate of a ~120 Hz display; full rate
  at 240 Hz would need a deeper frame queue (a waitable swap chain with a
  higher maximum frame latency).
* q = 1 or 2 only: measured on the 240 Hz G8 (exclusive fullscreen), frames
  presented with sync interval 2 keep exactly 2 refreshes apart, with 3-4
  they landed 2 refreshes early.  An explicit rate that is not refresh or
  refresh / 2 (144 on 240 Hz) uses the timer pacer.
* Every frame is presented with the sync interval that puts it q refreshes
  after the previous one, so a tick's frames are drawn right after its
  logic and DXGI queues them (6 back buffers while the pacer runs; at most
  3 presents queued, a fourth would block the game thread: that slot is
  skipped).
* `lat` (content latency) is the least that lets every frame be drawn after
  the logic of the tick it interpolates to: W = logic (p90 over 2 s, from
  the tick's real start) + replay + GPU + margin; the next tick's first
  frame is the first slot after S_N+1 + W, so the slot before belongs to
  this tick and lat >= that slot - S_N.  Raised at once, lowered by 0.3 %
  of a tick per tick.  The grid's phase is chosen at a start to need the
  least lat, and moved by one refresh when another phase needs >= 2 ms less
  for 2 s.  A hitch (a tick over a whole tick late, loading) restarts the
  chain instead of raising lat.
* A slot that cannot be ready in time is skipped (the previous frame stays
  q refreshes longer).  The frame statistics say at which refresh each
  present was shown; a frame that landed elsewhere moves the predicted chain
  (margin +0.25 ms, up to 1.5 ms).  When more than 10 % of 120 checked
  frames land elsewhere (a composed window shows the newest frame at every
  refresh), the timer pacer takes over for 30 s, doubling per failure.
* The tick-start present is skipped (the tick's frames are on their way),
  except on frame-dump ticks, which present the alpha-1 frame as before.
* Display timing: the swap chain's frame statistics over the last ~2 s;
  SyncQPCTime is read by the scheduler near the refresh, sometimes 0.5-1.5
  ms late and never early, so the period is the median of consecutive
  ratios and the vblank the earliest stamp relative to that grid (a
  two-point average from the first sample gave 220-230 Hz on the 240 Hz G8,
  and 120 fps was declined as an uneven rate).  `SMB2_PACE_DBG=1` prints why
  the pacer declined a tick, once a second.
* GPU times: D3D12 timestamps; a list executed without its end timestamp
  (swap chain resize) is not counted, nor samples over 100 ms.

### Timer pacer (V-Sync off, VRR, GL, no display feedback)

### Timer pacer (V-Sync off, VRR, no display timing)


* The 60 Hz deadline is unchanged (tick frame presented at `next_ns`).
* In-between phases on the target's grid, `tick0 + j/fps` (exact alpha j/k
  for multiples of 60); each frame is replayed, then presented at its phase
  (waitable timer + 1 ms spin).  A phase the slow tick already passed is
  drawn late at the alpha of its real present time if it still fits.
* A frame is only started if it and the tick frame still fit before
  `next_ns` (running averages of the replay's CPU time and, with V-Sync off,
  the present call's time; with V-Sync on present blocking is pacing).
* **No game slowdown**: `kcap` (most in-between frames per tick) drops to
  what fit whenever a tick still ends > 1.5 ms late (GPU backpressure the
  estimates missed) and recovers by one per 30 calm ticks.  A GPU that
  cannot keep up gets fewer in-between frames, the game keeps 60 ticks/s.
* The tick frame (alpha 1) is always the last replay of the tick.
* GPU timeline (D3D12, timestamp queries): a frame's GPU work starts at its
  present (or when the GPU is free); the tick frame of the previous tick
  occupies the GPU from tick0.  An in-between frame is only started if its
  GPU work ends, and the tick frame's CPU replay is done, before `next_ns`.
  Its alpha is taken at its GPU start (the tick frame's display is delayed
  by the same GPU time, so motion stays even).  GL has no reliable GPU
  timing (GL_TIME_ELAPSED includes submission gaps): the GL pacer relies on
  the present time and the cap.
* Estimates (replay CPU time, present call time) are updated by every
  replay and every present, so they cannot freeze after a hitch.
* **Degrade**: a tick that ends > 1.5 ms late without any in-between frame
  (recording + matching + replay cost more than this machine has, e.g. the
  10k-draw goal party-ball scene on a loaded CPU) turns recording off for
  2 s: the frame is drawn directly exactly like the 60 fps path, so HFR is
  never slower than 60 fps.
* Uncapped: back to back, same fit test.
* F1 overlay ticks skip in-between frames (the overlay presents its own).

## 5. Input latency

Input is sampled once per tick, as at 60 fps, and the tick frame reaches the
screen at the same time as at 60 fps (presented at the next tick start).
The in-between frames do not delay it: they are shown *before* it, during
the interval in which 60 fps keeps showing the previous tick.  So:

* added latency for the newest tick: **0** (same present time as 60 fps);
* the first visible response to an input comes earlier: the first
  in-between frame after the tick already blends 1/k of the new tick in
  (at alpha >= 0.5 it is drawn from the new tick's stream), i.e. 1/k tick
  after logic instead of a full tick (240 Hz: ~4 ms after the tick);
* cost: the replays run between the tick's logic and its present, so on a
  machine that cannot fit them the pacer drops in-between frames rather
  than delaying the tick frame (section 4).

Compared with an ideal renderer that would draw the newest state at every
present, interpolation shows the scene up to one tick (16.7 ms) behind;
extrapolation (alpha > 1) would remove that but mispredicts bounces and
turns, so it is not used.

## 6. Per-effect decisions (EFB copies, 2D)

| Effect | Path | HFR result |
|---|---|---|
| Reflections / mirrors, water (boat, bg_wat, ripples), wormhole, stage-select previews | async GPU copy | re-executed per frame: at the render rate |
| Feedback effects (pass samples its own last copy) | async GPU copy | tick's copy kept (60 Hz), consistent |
| Z / non-convertible formats, CPU fallback | sync copy | tick's copy (60 Hz) |
| Pause screenshot | copy | static anyway |
| HUD sliding elements, banners, "GO", fades | 2D quads | positions / colours interpolated (same UVs) |
| Timers, digits, text being typed, sprite frames | 2D quads, other UVs | discrete, switch at the midpoint with the 3D |

## 7. Testing tools

* `--dump-ranges a-b,c-d`: dumps every tick in the ranges plus its
  in-between frames (`frame_T.ppm`, `frame_T_j.ppm` = in-between j shown
  before tick frame T) and switches the pacer to fixed phases j/k.
  `--dump-from N` = every tick from N.
* `tools/hfr/hfrcap.sh <label> <bindir> <mode> <ranges> [-- args]`: a
  `tools/x64test/modes.sh` mode at 240 fps (1280x720) with dump ranges, then
  `tools/hfr/hfrcheck` per range (per-step frame differences; 32x32 blocks
  whose change lands in one step = "pops", PNG strips of those ticks for
  review) and an overview strip per range.  Legitimate discrete changes
  (timer digits, a camera cut) show up as pops too; review the strips.
* `tools/hfr/hfrperf.py log.txt [min_draws]`: the `[hfr]` 5-second windows
  as a table (fps, interval mean / p50 / p95 / p99 / max, late presents,
  replay ms, replays per tick, kcap, 3D draws per tick).
* `SMB2_HFR_DEBUG=<tick>`: list both streams' draws and their pairs for
  that tick (`SMB2_HFR_DEBUG_FULL=1`: matrices and vertices of small draws).
* modes.sh `hfr_roll` (Challenge 1-1 at full tilt: fast ball spin, goal
  party ball, replay) and `hfr_fall` (fall-out, respawn cut).
* **Frame pacing measurements** (section 4): `SMB2_PACE_LOG=1` keeps every
  present (content time, present id, sync interval, planned refresh), every
  tick (start, end of logic), every frame's GPU start / end (D3D12
  timestamps on the CPU clock) and, on D3D12, every change of the swap
  chain's frame statistics (which present reached the screen at which
  refresh) in memory and prints them at exit.  `tools/hfr/pacestat.py
  log.txt [first_tick last_tick]` turns that into: content advance per
  refresh between shown frames and its sd (0 = even motion), cadence
  histogram, presents per tick, present -> display, GPU end -> display, tick
  start -> display (input latency).  `SMB2_PACE_DBG=1` adds a `[grid]` line
  per second (refresh, q, latency, its requirement, miss / late counts).
* GL has no frame statistics: `tools/hfr/dispcap/dispcap.c` (build with
  llvm-mingw: `-ld3d11 -ldxgi -luser32`) reads the desktop through Desktop
  Duplication while the game runs with `SMB2_PACE_MARK=1` (present id as an
  18-block code in the top-left corner); `pacestat.py log.txt a b --dc
  dc.txt` then measures from the display side.  Only that 144x8 strip is
  read.  Duplication keeps the compositor in the path, so use it for
  windowed runs.
* `--display windowed|borderless|fullscreen` picks the display mode of an
  automated run (scripted runs are windowed otherwise).
* Measurements need the display awake and the window on screen: a sleeping
  monitor reports no vertical blanks (all-zero frame statistics) and the
  pacer then falls back to the timer pacer.

## 8. Performance and latency

Measured on an RTX 3080 at 3840x2160, MSAA 4x, V-Sync on, x64 build:

* Menus / light scenes: 240 fps locked at a 240 target (p99 interval
  ~4.3 ms), 120 locked at 120; uncapped with V-Sync off 1300-1400 fps.
* Light gameplay (2.7-3.2k draws per tick): D3D12 at 240 reaches 180-199 fps;
  120 stays locked.
* Party games (D3D12, 240): dogfight ~175, baseball ~179, boat ~97 (EFB
  copies), story cutscenes ~120 (median).
* Heavy scenes (goal party ball + replay, 8-10k draws): 60-64 fps; the GPU
  cannot fit a second frame in the tick and the pacer keeps the game at 60.
* GL replays are slower per draw and have no GPU timing, so GL reaches fewer
  in-between frames in gameplay (boat stays at 60: its EFB copies blit the
  whole 4K frame per copy).  **D3D12 is the recommended renderer for high
  frame rates at 4K.**
* The game keeps 60 ticks/s in every scene; ticks that would run late
  degrade to the plain 60 fps path.

Latency: the tick frame is presented at the same time as at 60 fps.  The
first frame that shows a new tick (the first in-between frame) appears
4.5-5.1 ms after the tick start at 240 fps and 8.4-8.8 ms at 120 fps, vs
16.7 ms at 60 fps (`first` in the `[hfr]` line).  In heavy windows without
in-between frames it is 16.7 ms, like 60 fps.

### Frame pacing measurements (2026-11-19, owner's PC)

RTX 3080, Samsung Odyssey G8 (239.997 Hz, fixed refresh), 3840x2160, MSAA
8x, V-Sync on, x64 build, `SMB2_PACE_LOG`; 2-5 (launcher stage 11, lava
falls, ticks 2200-2690) unless noted.  speed sd = sd of the content
advance per refresh between shown frames (ideal 0.25 tick, 0 = even);
latency = tick start -> that tick's state on screen (mean).

| D3D12 exclusive fullscreen | pacer | shown fps | refreshes per frame | speed sd | latency ms |
|---|---|---|---|---|---|
| Display | timer (before) | 168.8 | 1: 802, 2: 574, 3: 4 | 0.0289 | 23.1 |
| 120 | timer (before) | 120.0 | 1: 7, 2: 967, 3: 7 | 0.0202 | 21.5 |
| 120 | grid | 120.0 | 2: 981 | **0.0000** | 25.7 |
| 144 | timer (both) | 120.0 | mostly 2 | 0.0260 | 22.3 |
| Race, Display | timer (before) | 209.6 | 1: 1466, 2: 247 | 0.0954 | 23.3 |
| Race, 120 (*) | grid | 119.1 | 2: 966, 3: 1, 4: 7 | **0.0245** | 25.3 |

Every grid frame of the 120 run reached the screen on its planned refresh
(982 of 982), present -> display 11 ms (the tick's frames wait queued).
(*) measured with a build in which `Display` chose 120 by itself (not
kept: `Display` asks for the full rate).  Full rate (240) does not fit this
scene (GPU 4.4 ms x 4 > 16.7 ms) and, at 240 Hz, never fits the flip queue
(section 4): `Display` / 240 runs the timer pacer, exactly as before
(~169 frames/s, speed sd 0.029); 120 gives even frames (sd 0.000) for
~2-4 ms more latency.

| windowed (composed) | pacer | shown fps | speed sd | latency ms |
|---|---|---|---|---|
| D3D12 Display | timer | 165.4 | 0.0301 | 22.0 |
| D3D12 Display | grid detects the composition, timer | - | - | - |
| GL Display (dispcap) | timer | 120.7 (464 of 1452 presents never shown) | 0.1105 | 19.7 |

GL has no per-present feedback: the grid pacer is not used (an open-loop
attempt over the compositor's vblank timing measured 16 frames/s matched,
q = 4 from an inflated present -> GPU-done time).

## 9. 60 fps

With the frame-rate setting at 60 the proxy is never selected
(`hfr_select` returns the backend itself): nothing is recorded or
replayed, the hooks return at once (no batch flush), and the pacer takes
the 60 Hz path.

Determinism: the logic loop, its inputs and its order are untouched;
replays run no game code and never touch gx_core state the game reads
(GC-memory EFB copies keep the tick's copy: replays use
`GXCOPY_NO_READBACK`), so replays, ghosts, memory cards, RNG and the
32/64-bit identity are unaffected at any frame rate.

## 10. Known artefacts / limits

* **GPU cost at 4K**: the renderer draws every GX batch as its own draw
  (4-10k draws per frame in gameplay), and the D3D12 GPU time per frame
  (timestamps) is ~4 ms for 3k draws and 6-9 ms for 8-10k draws at
  3840x2160 with MSAA 4x, about half of it per-draw overhead (720p: 1.5 /
  3-5 ms).  So at 4K, 240 Hz is reached in menus and light scenes, light
  gameplay runs 120-170 fps and the heaviest scenes stay near 60-110.
  Making the backends cheaper per draw (merging GX batches, bindless
  constants) is renderer work outside HFR.
* Discrete content (timer digits, text being typed, sprite frames,
  objects appearing) changes at the midpoint between ticks, i.e. it may
  appear up to half a tick earlier or later than at 60 fps.
* EFB copies made through the synchronous / CPU path (Z copies, formats
  the GPU conversion does not cover) and feedback passes keep the tick's
  copy (60 Hz).  In the tested modes every copy took the async path and
  none was kept (statistics `copies N kept 0`).
* Objects whose model-space vertices change completely between ticks
  (CPU-deformed meshes with a new vertex order) are not paired: they step
  at 60 Hz (switch at the midpoint).
* Rigid transforms turning more than ~150 degrees per tick are not
  interpolated (ambiguous direction); a fast-rolling ball's texture can
  alias at 60 Hz anyway.
* The `[hfr]` statistics line is printed every 5 s while HFR is on.
* V-Sync on a fixed-refresh display when only one in-between frame fits per
  tick (4K, MSAA 8x: ~7.5 ms of logic + recording, ~5 ms GPU per frame):
  frames are shown on the display's refresh grid, typically 3 refreshes of
  the tick frame and 1 of the in-between frame, whose phase is taken at its
  GPU start (~0.6) rather than its display time (0.75), and in a loaded
  system some ticks get no in-between frame at all.  Scrolling surfaces (the
  lava falls) then judder at 60 Hz.  The display time is not predictable
  from the vblank grid alone (DXGI frame statistics show 2-3 refreshes of
  queue latency that differ between frames).  The display-grid pacer
  (section 4) addresses this with per-present display feedback; a waitable
  latency-1 swap chain does not fit a single game thread that must queue a
  tick's frames ahead (the tick's logic runs between them).
