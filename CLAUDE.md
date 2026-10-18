# Mystical Ninja Starring Goemon (Goemon64Recomp) – Quest 3 VR port

This is klorfmorf's Goemon64Recomp (N64Recomp + RT64, the older Zelda64Recomp-style
tree with its own RmlUi UI, no RecompFrontend) ported to the Meta Quest 3 through
OpenXR. Work tree `C:\Users\QbAnYtO\gq`, branch `quest-vr` here and in `lib/rt64`
(+ its `contrib/{plume,zstd,nativefiledialog-extended,mupen64plus-win32-deps}`) and
`lib/N64ModernRuntime`. Package `com.goemon64.recompiled`. Started 2026-10-16 (night,
owner asleep, built in one session without headset testing).

The owner is not a programmer: explain in plain words, build and install yourself.
Follow the `quest3-n64-recomp-port` and `talking-to-other-sessions` skills. Install
only, never launch on the Quest (owner's standing rule); announce headset use to the
other sessions first.

## What came from where

- **RT64:** Goemon needs klorfmorf's RT64 (`rsp-fixes-plume-rebase`, 59 commits past
  the upstream base: RSP upgrades, extended GBI the patches use). The Bomberman Hero
  Quest changes (`BMHeroQuest/lib/rt64` quest-vr, f647df1..45ea175) applied onto it
  with `git apply -3` with no conflicts; one fix after: no `rectAspect` in this RT64.
  Added from South Park: ubershader-ready flag, VR view hook (`SetVrViewAdjust`,
  unused so far), and from Wave Race: the menu projection guard fix (2f2db81).
- **plume:** Bomberman's quest-vr (low queue priority = recording fix, OpenXR device
  hooks) + South Park's "never LOAD_OP_NONE on Android" (holes grid on the Quest).
- **N64ModernRuntime:** only the dummy-VI-mode fix for auto start.
- **VR layer:** `src/quest/openxr_quest.cpp` is Bomberman 0.8.2's (frame thread,
  4 picture slots, per-frame hook with RT64 in-between frames, pacing per game frame,
  hang guard, recording watcher, menu quad) with Goemon's camera, frame pairing and
  controls. `src/quest/android_storage.cpp` = Bomberman's ROM window/asset extraction.

## Building

1. **Game code** (only after a ROM/recompiler change; outputs are git-ignored):
   - `mnsg.z64` = the decompressed ROM, made from the owner's ROM with
     `C:\Users\QbAnYtO\tools\goemon_decomp\decomp.exe <rom> mnsg.z64` (built from the
     game's own `src/game/rom_decompression.cpp`; the ROM is in Downloads).
   - `N64Recomp.exe mnsg.toml`, `RSPRecomp.exe aspMain.toml` (N64Recomp built at the
     commit the upstream CI pins, 81213c1, with the reloc-name bounds fix; source in
     `C:\Users\QbAnYtO\tools\n64recomp-goemon`).
2. **Patches (MIPS):** this PC's LLVM 23 has no MIPS target, but **Zig's bundled clang
   does** (`C:\Users\QbAnYtO\tools\zig\ziglang\zig.exe`, pip wheel ziglang 0.13.0):
   `cd patches && make CC="<zig> clang" LD="<zig> ld.lld"` (cmake must be on PATH for
   the Makefile's echo), then `N64Recomp.exe patches.toml`.
3. **Quest:** `tools/quest/configure_android.sh` once (NDK 29, android-29, arm64,
   Release, `out/build/android-quest`; needs `out/build/x64-Release/file_to_c.exe`,
   made by `tools/quest/host_tools.bat`). Then `tools/quest/package.sh` (ninja, strip,
   SDL2/libc++ libs, assets, SDL Java, Gradle 8.9 with Android Studio's JBR). APK:
   `android/app/build/outputs/apk/release/app-release.apk`.
4. **PC test build:** `tools/quest/build_pc.bat` (clang-cl from LLVM 23, Ninja,
   `out/build/pc`, patches prebuilt). First build of everything ~10 min.

## Goemon facts the VR code uses

- The level is drawn through a `Camera` object (patches/types.h): pos 0x00, look_at
  0x0C, fovy u16 at 0x1A in units of **32768 = 360 degrees** (func_80017D8C_1898C
  passes `u16 * 360 / 32768` to guPerspective, aspect 4:3), near/far 0x1C/0x20,
  bit 0 of byte 0x3E = orthographic, scissor u16 x4 at 0x48. One Camera object
  (0x8088BCB0 seen) is reused for title, menus, cutscenes and gameplay.
- The hook: `patches/anime.c` (func_80016C44, case 0x20000000) copies the camera to
  `g_vr_camera` and calls `recomp_vr_aim_camera` (syms.ld 0x8F000100) right before
  func_80017D8C. The camera logic is `src/quest/goemon_vr_camera.h`, shared by the
  Quest and the PC desk test: it turns the copy where the head looks, sets the
  headset's vertical view, the full 320x240 scissor, pulls the eye in and applies the
  world lock; the game's own camera is never changed, so nothing is restored.
- **World lock** (default on, `adb shell setprop debug.goemon.yawlock 0` = off, live):
  Goemon's camera turns by itself to keep him in view: in many places it stands
  still and tracks him (PC trace: eye fixed, look-at moving), elsewhere it swings
  behind him. In a headset that turning spins the world. With the lock the view
  keeps its compass direction while the eye still goes where the game's camera goes
  (pulled toward Goemon): you stand where the camera stands and follow Goemon with
  your head; only head and right stick turn the view. Past 70 degrees from the
  game's heading the view snaps to it in one step. Camera jumps (> 20 degrees or the
  eye moving half its distance in one frame: new room) and a right stick click reset
  to the game's view. PC walk test at the moat: forward/sideways 0 snaps; walking
  straight back toward the camera snapped ~2 times a second (an orbit-style lock
  tried first snapped every 0.3 s even walking sideways, because the camera there
  mostly stands still).
- Gameplay test: perspective, scissor at least 280x180 (letterboxed cutscenes are
  (0,38)-(320,190), so they stay on the flat screen) and `g_player_1_task`
  0x801FC604 / `g_player_1_object` 0x801FC60C non-zero (zero on title and menus,
  whose 3D backgrounds use the same camera).
- Frame pairing: the camera hook records the display-list head `D_8015C5CC` and the
  matrix pool pointer `D_80168504` (the projection is written at the pool's current
  address). A gfx task (`data_ptr` 0x8008CCC0 / 0x800AA398, double buffered) takes the
  records whose list pointer falls inside its list; fallback: the oldest record since
  the last task. Its projection's X scale is then set exactly (widen_game_projection).
- Gameplay camera in Goemon's house: fovy ~49 degrees from ~90-100 units away. In a
  ~120 degree headset view that is tiny, so the eye moves to 0.5 of the distance
  (`camera_distance.txt`, right stick up/down in levels, saved after 1 s).
- Rooms have no ceilings and the game's camera often sits outside the room (near
  walls aren't drawn): looking up, behind or past a near wall shows black. Outdoors
  the ground ends below the camera (sky colour underneath). That is the game.
- Scenery is not culled to the game's own view: looking sideways and behind at the
  castle moat showed everything (PC check), so no culling fix is needed.
- The town sky is a row of 2D strips; the HUD inset used to squeeze them into the
  HUD box (black sky). RT64 now leaves rect rows that together span the width alone
  (`coversScissorWidthMerged`). Pause is a 2D menu on black (no level camera), so it
  goes to the floating screen.

## Controls (Touch)

Left stick move (forward = where you look; `debug.goemon.lookmove 0` turns that off),
A jump, B attack, right trigger Z (sub-weapon), left trigger R, left grip L, right
grip C-down, Y C-up, X C-left, left stick click C-right, left menu button Start,
right stick left/right smooth turn (click: back to the game's view),
right stick up/down camera distance (menus: C-up/C-down).

## Picture

Default **smooth motion**: RT64 in-between frames, 60 pictures a second at 120 Hz,
each held 2 refreshes, 4.4e6 pixels (the owner asked for 60 fps). The ROM window
offers "Sharp picture (30)" instead (`picture_mode.txt`). Menus, title, cutscenes:
floating screen 2.6 m away.

## Desk testing on the PC (no headset needed)

`out/pctest` is an isolated portable folder (own config/saves, a copy of the ROM).
Run from there: `GOEMON_AUTOSTART=1 GOEMON_LEVEL_SELECT=1 GOEMON_VR_TEST="yaw pitch
fovy pull" GOEMON_PRESS_FILE=<abs path>/press.txt ../build/pc/Goemon64Recompiled.exe >
run.log` (optional `GOEMON_VR_LOCK=0`, `GOEMON_VR_SWEEP="amp period"`,
`GOEMON_VR_HUD="aspect insetx insety"`). Scripts in `tools/quest`:
- `desk_boot.sh`: boot into Goemon's house (~40 s). `goto_select.sh <ups> [downs]`:
  the game's own level select (L+Z+D-right, enabled only by GOEMON_LEVEL_SELECT); the
  cursor's start varies, it saves `out/pctest/select_menu.png` before confirming.
  MAIN SIRO DOUCHU MACHI SHOP BOMBER 3DBOSS DAIMAP ... (DOUCHU 2 ups from SHOP gave
  the castle moat).
- `press.sh "<keys>" hold wait` (a b z r l start cu cd cl cr du dd dl dr up down left
  right turnleft turnright pullin pullout), `vr.sh "yaw pitch fovy pull [lock]"`
  changes the view live (`out/pctest/vrtest.txt`), `shot.sh <name>` screenshots the
  window area (x 1759, y 238 on this 5120x1440 screen) to `out/pctest/<name>.png`.
- Use the D-pad (dd) in menus: the test turns the stick by the view's yaw.
Checked 2026-10-16: the aimed view, full frame, projection at the pool address
([1][1] = cot(fovy/2)), and forward walking with the view turned 90 degrees.
`GOEMON_VR_HUD="0.93 0.18 0.22"` also gives the PC window the Quest's picture shape and
HUD box: with a 22 degree box the HUD sat right above Goemon, so the Quest uses 28.
The Quest's numbers: `GOEMON_VR_HUD="0.93 0.231 0.295"`.
Checked 2026-10-16 morning: shop, town, castle moat, Oedo Castle, dragon map; sky
strips; pause; world lock walking and turning (moat, house).

## Owner test 1 (2026-10-16 afternoon) and fixes

Owner: title and intro fine; pause menu turned with the head; HUD too high; "the world
warps when I turn my head, only outside, rooms are perfect"; wants the game's auto
realign gone and the right stick click to put the camera behind Goemon. Quest log:
60 pictures a second held exactly 2 refreshes at 120 Hz (600/600), 0 missed frames,
frame pairing 100% by list; the owner set the camera distance back to 1.00.
- **Outdoor warp = the 2D sky.** The game's sky backdrop (patches/background.c) is a
  flat strip of a 360 degree panorama (640 texels around, 1.78 texels a degree, measured
  by probing start_x against the camera heading on PC). In the default mode it doesn't
  move when our view turns at all (0 px for a 5 degree turn): glued to the face. Now
  `recomp_vr_background` (syms 0x8F000108, shared math `goemon_vr::fill_background`)
  fills a 28x24 grid with the panorama texel each screen column/row looks at for the
  headset's view, and the patch draws it cell by cell (same texture formats as the game).
  PC check: a 5 degree turn moves the sky 28 px like the scenery. `debug.goemon.vrsky 0`
  = the game's flat sky (live).
- **Pause:** SYS_W+0x3AE26 (u16, 0x800C7AE6) is 1 while paused (diff of the system
  block in play vs pause); the pause menu's 3D backdrop used the same camera with a
  player present, so it was head-aimed. Now not a level camera -> floating screen.
- **Follow mode (no auto realign):** when the game's eye moves (it follows Goemon,
  outdoors) the VR eye keeps its own direction around Goemon (game distance and
  height); standing room cameras keep the game's view. A camera jump ends it. Right
  stick click = camera straight behind Goemon (player object rotation.y, 1024 a turn).
  The right stick orbits around Goemon in follow mode.
- HUD box 35 degrees (lower and bigger).
- Desk test window: `GOEMON_WINDOW="x y w h"` (always on top, no focus) +
  `SDL_WINDOWS_DPI_AWARENESS=unaware`, captured with PrintWindow (tools/quest/shotgame.ps1)
  so it works while the owner uses the PC; close the game after each run (PC memory).

## Owner test 2 (2026-10-16, 16:02) and fixes

Owner (relayed by the No Mercy session): "world still warps. hud still needs to be
lower". Quest log: ~2 min outdoors, game 21-30 fps, 600 shown / 0 missed, pairing
by list; pictures shown 2x ~60%, 3x ~35% (the game runs below 30 outdoors).
- **Why the grid sky still warped:** RT64's in-between pictures (smooth motion)
  re-render the frame with 3D transforms halfway to the next frame, but 2D rects stay
  as drawn, so the grid sky sat where the *next* frame had it while the layer was
  placed at the halfway pose: the sky jumped against the world on every in-between
  picture while the head turned. Also the game scrolls the sky from the camera of the
  frame before (PC order trace: aim -> sky -> list sent each frame, and start_x
  follows the heading one frame late), so the sky slid whenever the game camera turned.
- **Fix: the sky is a dome in the 3D scene.** `recomp_vr_sky` (syms 0x8F00010C,
  shared math `goemon_vr::fill_sky_dome`) fills a table of cells (32x32 texels, plus
  caps to +-89 degrees, ~200 cells); the patch (`vr_draw_sky_dome`, background.c, now
  `#define F3DEX_GBI` - the game is F3DEX, the patches Makefile didn't say so) puts the
  matrix and vertices inside the display list (branch over them), pushes RT64 other
  mode/combine/geometry, draws textured triangles without depth (1-cycle, DECALRGB,
  TLUT for CI), matrix group 0x5C1D0000 (translation interpolated), then pops.
  Modelview = scale(far/2 / 1000) + translate(the aimed eye). Calibration: texel column
  c at heading 0 = start_x+160 + 1.78 * heading of the frame before (constant 480.0 on
  the road while walking/turning); row t0 at the horizon = start_y + 120 + 0.38 * tilt
  (the game scrolls the sky only 0.38 texels a degree of camera tilt; constant 153.9).
  Both held unless they move more than 1.5 texels. `debug.goemon.vrsky` 2 dome
  (default), 1 old grid, 0 game's flat sky (live). PC: GOEMON_VR_SKY.
  Bug on the way: my guMtxF2L copy wrote the fixed-point halves per row; libultra
  writes all integer halves first, then all fractions (dome invisible until fixed).
- **Near plane 4** (`kVrNearPlane`, written into the camera copy): the game's 20 cut
  the ground off in a straight, head-locked line when looking down near Goemon
  (showed the sky below it). PC: looking 45 degrees down now shows ground to the edge.
- **HUD lower:** RT64 `SetVrRectInset(sx, sy, offsetY)` gained a downward offset (share
  of the picture height). The box (35 degrees) now has its middle 17 degrees below the
  view's middle (`kHudDropDeg`, live `adb shell setprop debug.goemon.huddown <deg>`).
  PC: `GOEMON_VR_HUD="0.93 0.231 0.295 0.064"`.
- Commits 1d4f286 (+ rt64 c49a829). APK built 16:47, copied to Downloads as
  MysticalNinjaGoemonVR-Quest-0.1.1.apk; the Quest was not reachable by adb then.

## Owner test 3 (2026-10-16, 23:18) and fixes

Owner, playing with me logging: "world is still warping"; "gameplay is very
smooth"; HUD too low ("bring it up like 50 pixels"), and smaller: "the text windows
are too close to my face". Quest log: 600 pictures each shown exactly 2x, 0 missed,
pairing 1800/1800 by list. So timing and pose pairing were perfect; the warp was
geometry.
- **The real outdoor warp: the follow eye hung off the game's look-at point.** PC
  trace (game eye, game look-at, player position, our eye per frame): the game's
  look-at wanders up to 15 units ahead of/behind Goemon (he is ~20 tall) and the
  game camera's distance stretches 45-64 as it lags. Our eye = look-at + offset slid
  sideways and zoomed with it whenever Goemon moved or turned (and walking follows
  the head, so turning the head turned Goemon). Now the follow eye is centered on
  the player object's position (x/z), at the game's look-at height (no bounce on
  jumps), at a distance that follows the game's over ~4 s (`kFollowDistRate`
  0.008/frame, 25..90). Commit dcf01f1.
- HUD: half height 35 -> 26 degrees, middle 18 degrees below the view's middle
  (live `debug.goemon.hudsize`, `debug.goemon.huddown`). During test 3 I had set
  huddown 10 live; set it back to 18 at the next install.
- Sky dome hold raised to 12 texels (the Quest's measured scroll wobbled up to 4).
- PC memory: commit was 62/64 GB (Windows Explorer alone ~22 GB, likely a leak; only
  the owner can restart it); the PC test failed with "Failed to allocate memory!"
  until it dropped. Packaging uses `gradle --no-daemon` (the daemon is shared: never `gradle --stop`, it killed the Smash session's build).
- APK 0.1.2 in Downloads (23:32).

## Owner tests 4-5 (2026-10-17 ~01:20-02:30) and overnight work

Owner: "world is still warping" (0.1.2, 0.1.3); sharp mode (no in-between pictures)
"still warps", "nauseating"; asked to revoke the right stick click in rooms; the
30 fps sharp option will be removed before release (60 the default). Every log:
pictures 600/600 shown 2x, 0 missed, pairing by list 100%.
Ruled out with measurements:
- FOV/aspect (the skill's "slides/warps sideways" and Wave Race's "drawn rows"): RT64
  readout (`rt64 projection:` log in rt64_projection_processor.cpp, wide perspective
  projections only) on the Quest: final x 0.424 / y 0.404 vs the layer's 0.42 / 0.40;
  source picture 2048x2144 = all 240 rows. PC pixel check: 30 deg yaw / 20 deg pitch
  moved centre features by f = ~155 px per tan unit both ways (= the projection).
  (An earlier "28% too far" came from a bad template match.)
- In-between pictures: sharp mode still warps.
- Camera up vector is (0,1,0) (Camera +0x24..0x2C).
Fixed (PC-verified with the trace; headset test owed):
- **Jump bob:** the follow eye's height rode the game camera, which rises ~15 units
  with every jump and sinks back over 1.5 s: the world bobbed ~1 m per jump. Now the
  eye sits `kFollowEyeHeight` (21) above Goemon's ground estimate, which follows his
  feet only after 6 steady frames (|dy| < 0.6/frame; up 0.1, down 0.2 of the gap per
  frame). Trace: eye 28.0 through a 30-unit jump (was 27 -> 42 -> slowly 27). cd95722.
- **World lock dropping out:** any non-level camera call reset `have_last`, so the next
  level frame was a "cut" that ended follow mode (view snaps to the game's swinging
  camera). Now only 30 calls in a row without a level camera forget it. In follow mode
  the game camera's >20 deg/frame swings no longer cut; only Goemon (or the eye beyond
  3x its distance) moving >60 units in a frame does. Quest logs `world lock: camera
  cut N (reason ...)`, `following Goemon (N)`, `snapped`, and `N level cameras aimed in
  one frame`. 8b7f371, 5709fad/db45dab.
- 6DOF head movement (`debug.goemon.units`, 15/m) moves the eye; the offset is now
  measured in the frame of the starting head direction (was the room's -z, wrong if
  the session started facing elsewhere). f2afeab.
- Right stick click only while following (outdoors). HUD 26 deg half height, 18 down.
- Probe: `probe:` lines now print the shift the head turn predicts; fakeyaw for runs
  (`tools/quest/quest_probe_test.sh`, needs the lock; autoplay + presses + probe).
- PC desk window no longer always-on-top (owner: capture without focus); test folder
  background_input Off; packaging `gradle --no-daemon`.
- APK 0.1.6 in Downloads (02:49), not installed: the headset slept (Wi-Fi adb offline,
  no USB) from 02:38.
- **Prime suspect found (03:00): camera-facing sprites.** Trees/bushes are sprites the
  game keeps parallel to the picture: func_8001B6D4(camera), right after the camera's
  matrices, stores eye - look-at at 0x80168980 and derives the sprite angles. With the
  head-aimed copy every tree and bush swung with the head (rooms have none; also at 30
  fps). PC proof: looking 35 deg down, the tree stayed undistorted and picture-parallel;
  with the fix it is a card standing in the world. Fix (anime.c + `recomp_vr_billboard`,
  syms 0x8F000110, `goemon_vr::billboard_camera`): func_8001B6D4 gets a level copy of
  the aimed camera whose heading follows the view at 5%/frame (~0.7 s), live
  `debug.goemon.billboard <percent>` (0 = body forward, 100 = old head swing, -1 = the
  game's own). Looking 120 deg aside on PC: nothing culled. APK 0.1.7 (03:05).
- **Headset probe (03:11, unattended, `quest_probe_test.sh <dir> 60 60`, fake head
  swing 60 deg):** picture shift / predicted shift = 1.04 median (1.09 least squares)
  over 57 pictures, sub-pixel; the excess is the strip's width (pixels move faster off
  centre), so the rendered rotation matches the head. World-lock logs: cuts only at the
  level start (first camera, the game's camera changes), 2-3 level cameras per frame
  only then. No GPU hang or crash. The walking phase didn't start follow mode (the save
  may start where the presses don't walk). 0.1.7 installed 03:11 (push to
  /data/local/tmp + `pm install`: big streamed installs drop over Wi-Fi).

## Owner test 6 (2026-10-17 morning): "nothing is fixed, exactly the same"

- Recorded the owner's view with `adb shell screenrecord` (4128x2208, both eyes,
  lens-warped; the recorder shows the left eye rolled ~40 deg against the right for
  every app, the home screen too: not ours). What it showed: the camera at Goemon's head
  height with his hair a few units from the eyes, filling the lower view (camera
  distance 0.5). Optical flow over the recording: content moves in ~9 px steps every
  other recorded frame during head turns (recorder or 60 Hz pictures; not resolved).
- Also ruled out: off-centre projection terms ([2][0]/[2][1] = 0 in house and road).
- Owner: "just assume it's warping and assume the fix". 0.1.8 (f04f650): 60 a second
  always, the ROM window's Sharp/Smooth choice removed (owner, all ports, via Smash);
  follow eye `kFollowEyeHeight` 30 above Goemon's ground (was 21 = his head) and never
  closer than `kFollowMinDist` (25) across the ground; the projection layer is placed
  at the head position the picture was drawn from (FrameRecord.head_pos, 6DOF).
  Installed 11:33 (owner out; push + pm install over Wi-Fi).
- Next candidate if it still warps: real stereo (per-eye images from RT64's depth;
  RT64's depth target is D32 and shared/overwritten per picture, so it needs a copy per
  workload), or the HUD as its own head-fixed layer.

## Owner test 7 (2026-10-17 afternoon): "rooms don't warp, outside still warps"

- Owner: "whatever is being used in the rooms should be used in the outside world cuz
  the little rooms don't warp". Rooms film from a standing game camera; outdoors the
  eye rode along behind Goemon (follow mode). 0.1.9 (c9310a4): outdoors the eye now
  stands still like a room camera (`LockState::station_eye`) and cuts to a new spot
  when Goemon gets too far (`kStationFar` 130), too close (`kStationNear` 12, the new
  spot then goes `kStationBackMin..Max` 45-80 behind him on the same line), off to
  the side (`kStationSideDeg` 70, only past 25 units) or his ground moves 50 up/down.
- **The view's direction never changes at a cut** (only the right stick turns it).
  First try put the new spot behind the way Goemon faced: the stick moves him
  relative to the view, so walking toward the camera flipped the view 180 degrees,
  he turned round, walked into the next spot, flipped again (PC trace, every 10
  frames).
- Live switch: `adb shell setprop debug.goemon.outdoor 2|1|0` (2 standing camera,
  default; 1 riding behind Goemon as in 0.1.8; 0 the game's camera spot = room logic
  everywhere). Logs: `outdoor camera: ...`, `world lock: standing camera moved to a
  new spot (N, reason)`. PC: `GOEMON_VR_OUTDOOR`, `[vrtest] standing camera spot`.
- If the owner says it still warps outdoors with the standing camera, ask whether it
  also warps standing still (no cuts) and try `outdoor 0` live.
- **Fog (0.1.10, b6aba0b; rt64 fb8c399).** Goemon turns fog on outdoors (Oedo:
  `gSPFogPosition` 800-1000, mul 640 offset -384; not visible there, the blender
  doesn't use it, but other areas may). Two VR errors fixed in `RSPProcessCS.hlsl`:
  N64 fog follows z/w (depth straight ahead), so with the 134 degree view a far wall
  at the picture's edge had far less fog and the fog changed with every head turn;
  now it uses the distance to the eye (radial). And our near plane 4 (game 15-35)
  made the fog start ~8 times closer; the shader finds each projection's far
  distance from its matrix and uses the game's near from a table
  (`RT64::SetVrFog(radial, fars, nears, count)`, `goemon_vr::FogNearTable`; the game
  draws 2-3 cameras a frame with different planes, so one global correction was
  wrong). Live: `adb shell setprop debug.goemon.fog radial|flat|game` (log `fog:`);
  PC `GOEMON_VR_FOG 2|1|0`. Math checked against a numpy model (exact match).
- **Sprites still while the head turns (0.1.10, b78e96e).** `debug.goemon.billboard`
  default 5 -> 0: every tree/bush faces the view's base heading (turns only with the
  right stick; cuts keep it), instead of following where you look over ~0.7 s, which
  swung every tree after each head turn. The game gives all sprites one angle from
  the camera (func_8001C7E4 fills 0x801689F0.. from eye - look-at; func_800192D0
  uses it for rotations flagged 0x4000), so per-sprite facing of the eye would need a
  rewrite of func_800192D0.

## The warp's real cause: the game's 3D viewport (0.1.11, 826aad6)

- Owner: "smash bros had the same warping problem and fixed it" (Smash: a 300x220
  battle viewport). Goemon's level viewport (Camera::vp at 0x40, copied into the DL by
  func_80017D8C) is **scale 192.5 x 144.5, centre (160, 95.5)**: the 3D is zoomed 1.2x
  and raised 24.5 rows (TV framing; the same without VR). The layer declares the
  projection's own view over the 320x240 picture, so the world was 20% too big,
  shifted up, and moved 20% faster than the head. The projection readout and the
  sky/FOV checks never looked at the viewport.
- Fix: patches/anime.c points the aimed camera copy at `g_vr_viewport`, filled by
  `recomp_vr_viewport` (syms 0x8F000114, `goemon_vr::full_screen_viewport`) with the
  full screen (scale and centre 160, 120; the game's depth terms). Live: `adb shell
  setprop debug.goemon.viewport game` = the game's own; PC `GOEMON_VR_VIEWPORT=game`.
  Log `3D viewport: ...`.
- PC check: a 20 degree turn moves the castle 60 px; the declared view predicts 58.8
  (the game's viewport: ~72). Found by logging RT64's `RSP::setViewport`.
- Installed 2026-10-17 15:42 (owner said install).
- **Owner on 0.1.11: "ok it no longer warps. but the camera has to chase the player".**
  0.1.12 (f143551): outdoor default back to following Goemon (`debug.goemon.outdoor 1`),
  now gliding: the eye moves `kChaseRate` (9%) of the way to its spot behind him each
  game frame (~0.35 s). The view's direction stays the head's and the right stick's
  (no automatic swing behind him; right stick click puts it behind him). The standing
  camera stays as `outdoor 2`. Queued for install behind the owner's turn.

## First person (0.1.13, 9d8eff3)

- Owner: "a first person camera where you can make the head of the player invisible but
  still see the body perform the attacks", "with the right stick click". The right stick
  click now toggles first person (it used to put the chase camera behind Goemon; leaving
  first person does that now). The right stick turns the view (its up/down camera
  distance is off in first person).
- `goemon_vr::aim`: eye = Goemon's feet + 19 up + 3 ahead along the body heading
  (`kFirstPersonEyeHeight`, `kFirstPersonForward`); near plane 7 (`kFirstPersonNear`)
  cuts his own head away. Looking ahead/up shows nothing of him; looking down shows
  the body, feet, shadow and the arms swinging on attacks. Compared on PC: (19, 3, 7)
  shows the most body; (20, 6, 6) and (18, 8, 5) hide it.
- Base heading = `fp_heading` (Goemon's facing when entering, plus the right stick);
  the head turns on top. Goemon's RotY (obj + 0x16, 1024 units, low 10 bits) is set to
  the view heading every aim, so attacks/throws go where you look; the stick already
  walks where you look (`move_yaw`).
- Live: `debug.goemon.fpheight`, `fpforward`, `fpnear` (game units, 0/-1 = default).
  PC: press-file key `fp` toggles, `GOEMON_VR_FP="height forward near"`.
- HD textures: the owner asked for "the latest HD textures from the github"; the repo
  (klorfmorf/Goemon64Recomp, latest v0.2.0-dev 2026-01-31, our tree = origin/dev) has
  no texture pack, only game builds. Texture packs (RT64 .rtz) load as mods; waiting
  for a link from the owner.

## Buttons and MSAA (0.1.14, 00ae616; built 21:32, not yet on the headset)

- Owner on 0.1.13: "beautiful". New layout (owner): left grip = the minimap (C-right;
  the game doesn't use L, which was on the grip), left stick click = first person
  on/off, right stick click = camera straight behind Goemon (outdoors/follow only, as
  before). Actions renamed: `map` (left squeeze), `view_toggle` (left thumbstick click),
  `camera_behind` (right thumbstick click).
- MSAA: the Android config forced it off. Now `debug.goemon.msaa 0|2|4` read at launch
  (default 2, the owner's Smash pick), capped by the device's sample counts, logged as
  `MSAA Nx (asked .., device max .., sample locations ..)`. RT64's Android present
  branch resolves inside `RenderWorkerExecution` when `usesResolve()` (rt64 commit; the
  Smash 0.4.3 crash in vkCmdPipelineBarrier). If 0.1.14 crashes or goes black:
  `adb shell setprop debug.goemon.msaa 0` and relaunch.

## Status

0.1.11 (full-screen 3D viewport, the likely warp cause) installed 2026-10-17 15:42,
owner test owed. Owner tests 1-7 done (see above). 0.1.10 (standing outdoor camera like the rooms' +
radial fog with the game's near plane) built 2026-10-17 14:53, installed when the
headset lock allowed (0.1.9 was never installed; owner test owed). Ask the owner
to walk outdoors and turn the head; read the `world lock:` / `standing camera` lines. If
it still warps: `debug.goemon.outdoor 0` live, then real stereo or a head-fixed HUD layer.
Before release: remove the rt64 projection readout (the 30 fps option is already gone).
Logs: `adb logcat -s OpenXR` (+ `kgsl|GPU hang`); filter by the Goemon pid.
