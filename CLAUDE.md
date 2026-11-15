# CLAUDE.md — naser949's two-player mod for Twinsanity Evolved

Read this first. It is the hand-over from the cloud session that wrote the mod to a session running on naser949's own PC.

## The person

- **naser949** (GitHub `naser32`), Windows PC, not a programmer. He writes in Saudi Arabic: **answer in the same casual
  Arabic**, short, bottom line first. Technical words (file names, menu rows, commands) stay in English.
- Credit the work to **naser949**. Commit as `naser949 <naser949@users.noreply.github.com>`. Never put his e-mail anywhere.
- Commit / push only as part of work he asked for. **Never merge a pull request** unless he says so.
- He tests by playing (with a friend for online) and reports with videos and `log.txt`. Say plainly what is tested and what
  is not; do not call something fixed until he has seen it work.

## The project

Twinsanity Evolved is a PC decompilation port of Crash Twinsanity (the user supplies his own ISO: NTSC-U SLUS-20909 v2.00).
This fork (`naser32/TwinsanityEvolved-multiplayer`) adds a second player.

- `game/` — the decompiled game (C++), compiled to wasm with wasi-sdk, then wasm2c.
- `platform/nativeee/host/` — the host side: SDL input, graphics, audio, clock, save states, mods.
- `engine/` — the engine under it. `runner/` — the executable's main.
- The game calls the host through a fixed "seam" (239 imports of 254). **Do not add seam functions**: it is costly. The mod
  passes what it needs through seam calls that already exist (see "Channels" below).

Branches: `main` (upstream), `coop-step2` (local split screen, PR #2), **`coop-online`** (online on top of it, PR #3 — the
current work). PR #1 is superseded.

## What exists and works (confirmed by the user)

### Local two players (`game/src/game/coop.cpp`, `game/include/game/coop.h`)

- START on the second controller: a second copy of the played character (`CreateInstanceFrom`). Hold SELECT on pad 2: leave.
- PRIMARY = the game's own played character; SECONDARY = the copy. `g_Swapped` says which pad plays which.
- `CoopPlayerScope` makes the secondary "the player" for its own frame, its scripts, and the scripts of crates / pickups /
  creatures / projectiles nearer to it.
- Split screen: a second `Renderer` + `RenderView` drawn first; the host holds each half's draws to its rectangle (scissor).
  Stable 60 fps. Above 60 (120/144/240) it drops: the port rebuilds geometry on the CPU each frame — tell him to use 60.
- Lives and wumpa are **shared** (the game's own counters). No per-player HUD except a small "P2".
- Death: the dead player spectates the other until a checkpoint (`CoopCheckpointReached`); when the primary dies and the
  secondary lives, the two bodies exchange places (`ChangePlaces`). Cutscenes do not respawn a dead player.
- Options page "two players" in `game/src/game/olegpages.cpp`: camera distance, split direction, camera tilt (not saved
  between launches).
- Hooks in the game's own files are each marked with a `// TwinsanityEvolved:` comment and include `game/coop.h`.

### Online (`platform/nativeee/host/netplay.cpp`, `te_netplay.h`)

Design chosen by the user: **input sync (lockstep)** + **a lobby code that is the host's address, over Radmin VPN**.

- Options > Mods > **online**: `host a lobby` (code copied to the clipboard), `join with the copied code`, `leave`.
- The host saves a state (the port's own save states), sends it over one TCP connection (port 26950), both machines load
  it, then exchange only the pads every boundary: host = pad 0, joiner = pad 1, with an input delay from the round trip.
- While a session runs: the clock fires exactly one field per boundary (`time.cpp`), loads land as under the deterministic
  profile (`stream_service.cpp`, `audio_service.cpp`), F5/F8 are off.
- Each machine shows its own player on the whole screen: the game draws both views full size and marks whose each is; the
  host scissors the other's to one pixel (`graphics.cpp`, `g_online_view`).
- Every 2 s the two compare the **game's own account** (players' positions, state, lives, random seed — `TellState` in
  coop.cpp). A difference makes the host send its state again, as a difference from the last one (`Difference` / `Apply`).
- Every 30 s the whole memory's hash is compared **for the log only**: it differs on every check even when the games are in
  step (blocks around 2C0000, 450000, B80000, C60000, D00000–EB0000) — host-side data, not a desync. Do not resync on it.

Channels through the existing seam:
- game asks "is a session running": `Platform::System::ModPageRowLabel(onlineMod, 100, ...)` returns "1" / "0".
- game tells the host whose view follows / its account of itself: `Platform::Graphics::StartFrame` with width = height = 0,
  blue = 0x0F and red = 0xC0 (green = 0 or 1 + pad) or red = 0xC1 (x, y = the hash).

Last test (two PCs, Radmin VPN, 33 ms round trip): 13.5 minutes and 408 checks in step from the beach through the first
boss into the cavern; one real desync in `levels\earth\cavern\tunnel02` (the Crash + Cortex ball section).

## Not yet confirmed by the user (latest commit on `coop-online`)

- A state sent again should now take seconds, not 35 s (new difference format) — tested only on made-up data.
- Silence instead of a repeated sound while the game waits on a state.
- The log line "the other's pad waited for ..." (where the session's stops come from).
- On `coop-step2`: after P2 dies, the camera / interactions / checkpoint respawn of P1; and P1 dying while P2 lives.

## Open problems and the user's requests, in his order of asking

1. **Stutter in some areas online.** In his log the *host* was the slow one (scale 4x fullscreen, 51 fps in heavy areas;
   the joiner waits for it). Told him to try Scale 2x and 60 fps on both. There are also ~0.25 s stops on both machines at
   once, cause unknown — read the new "pad waited for" lines. A real saving would be to not build the other player's draws
   at all on the host side instead of scissoring them.
2. **Each player opens START / options without stopping the other.** In lockstep the pause is game state. The workable form:
   online, the world keeps running while a pause menu is open. Needs work in `GameController`'s pause state. Not started.
3. **Two balls in the Crash + Cortex ball sections** (one per player). Not studied: find how the game makes the ball
   character and whether the second player's copy can be one.
4. **Bosses target player 1, then player 2, in turn.** Each boss has its own scripts; start with one.
5. Smaller, deferred: a New Game screen listing the controllers; saving the co-op options; decals in P2's half; a small dot in
   the top-left corner online (the hidden view's pixel).
6. He asked for "each one loads his own game and only some things are sent" — explained to him that it is a different
   netcode (months, no guarantee). Stay with lockstep.

## Build and test on Windows

Prerequisites (README "Build > Windows" has the full text): Visual Studio 2022 Build Tools with "Desktop development with
C++" and "C++ Clang tools for Windows", CMake 3.24+, Python 3 as `python`, Git for Windows, a bootstrapped vcpkg with
`VCPKG_ROOT` set. Use a "Developer PowerShell for VS 2022".

```
powershell -ExecutionPolicy Bypass -File tools\fetch_toolchain.ps1
cmake --preset windows-clang-cl-release
cmake --build --preset windows-clang-cl-release -j 8
ctest --preset windows-clang-cl-release
.\tools\ci\release_dist.ps1 -SkipBuild        # dist\windows-x64\ and the zip
```

The first configure builds FFmpeg from source and is slow. GitHub Actions ("TwinsanityEvolved Windows", `windows.yml`)
builds the same thing on every push to a pull request: artifact `twinsanityevolved-windows-x64`.

- The unit tests count the built-in mods: `platform/nativeee/tests/test_mods_page.cpp` and `test_mods_folder.cpp` were
  updated for the `online` mod. Run `ctest` after touching the mods.
- Run with a log: `twinsanityevolved.exe 2> log.txt`. Profile: `set TE_PROF=1` first. Online lines start with
  `[twinsanityevolved] online:`.
- **Online needs both players on the very same build** (and the same ISO, mods and settings): after a local build, he must
  send the `dist` folder to his friend. Local split screen needs only his PC and two controllers.
- Online saves go to the `-mods` memory card (any mod opened does that); the friend's own saves are untouched.

## Working rules that saved time

- Keep all mod logic in `coop.cpp` (game) or host-only files; hook the game's files with one marked line each.
- Anything the game decides must be identical on both machines online: never let the game know which player is local.
  Which view is shown, which pad is local — host side only.
- He cannot read code or logs himself: when you need information, tell him exactly what to do and what to send.
- Batch fixes: he prefers to play a whole session, then report everything at once.
