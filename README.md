# Frame Perfect Buffer (Geode mod)

Holds an early jump press and fires it on the **last viable 240 Hz tick** instead of the tick you actually clicked on.

GD 2.2 runs physics at a fixed 240 steps per second no matter what FPS you render at, so a "240 fps frame" is one physics tick. If a jump has to happen on tick 20 and you click on tick 14, the mod swallows the press, watches the player each tick, and forwards the press on tick 20.

## How it decides which tick is "the one"

The mod can't read the level designer's mind, but it can detect, one tick ahead, the moment an input is about to stop being possible:

| Check | What it looks for | Typical frame perfect |
|---|---|---|
| Ground edge | You're grounded now, and after the next step there's no solid under you | "jump on the very last pixel of the block" |
| Orb exit | You're overlapping an orb now, and after the next step you aren't | "hit the orb as late as possible" |
| Death | Your inner (hazard) hitbox overlaps a hazard after the next step | "jump now or die" |

While none of those is true the press is held, up to **Max delay** ticks. When that runs out the press fires anyway. The checks use the actual object hitboxes from the level, found through a sorted index so the per-tick cost is tiny.

**What this means in practice:** every grounded jump within the window gets moved to its last viable tick. That is only what you want on inputs that were going to be late-or-die anyway, so keep Max delay small (default 8 ticks ≈ 33 ms). An ordinary early jump inside the window is delayed by at most that much.

### Modes
- Cube / robot / ball / spider: buffered while grounded or while touching an orb.
- Ship / wave: never buffered (held modes, nothing to time).
- UFO / swing: off by default; when enabled, a tap is held back until the death check or the cap releases it.
- Dual mode: both players, independently (Player 2 can be turned off).

### Safe mode
If **any** press in an attempt was actually delayed, that attempt's progress percentage is not saved and a completion is run as test mode (no stars, no completion recorded). An attempt where every press fired on the tick you clicked is left alone. The corner indicator shows `[safe]` when the current attempt is tainted.

## Limitations (read this)
- The checks are one-tick lookaheads using axis-aligned hitboxes. Rotated spikes, slopes and moving/scaled objects are approximated, not simulated. It is **not** a bot and does not simulate the player forward.
- "Last grounded tick" is detected from solids under the player; the level floor (y = 0) counts as ground, the ceiling in upside-down gravity does not (no object there), so flipped-gravity edge jumps may fire by the cap rather than the edge.
- Deaths caused by hitting the side of a solid block are not predicted, only hazards.
- Hazard hitbox scale defaults to 0.3 of the full hitbox. If deaths seem to be predicted too early or too late, adjust it (GD's cube uses roughly 0.25–0.33).
- Untested on anything but Windows 2.2081. Treat the first build as a prototype: play a few levels with the indicator on and see whether the numbers match what you expect.

## Building

The binary is produced by GitHub Actions, you don't need a local toolchain.

1. Push this repository to GitHub (`main` branch).
2. The **Build Geode Mod** workflow runs automatically. It produces:
   - a build artifact (`frame-perfect-buffer-win`) on the Actions run page, and
   - a `latest` pre-release on the Releases page with the `.geode` file attached (stable public download URL).
3. Download `owazamono.frame_perfect_buffer.geode`.

To build locally instead, install the Geode SDK + CLI and run `geode build` (or the usual `cmake -B build -A x64 && cmake --build build --config Release`).

## Installing

Drop the `.geode` file into your Geometry Dash folder under `geode/mods/`:

```
C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods\
```

(or whatever folder your GD executable with Geode installed lives in; the depot folder you downloaded through Steam's console is not where Geode looks). Restart GD; the mod shows up in the Geode mod list with its settings.

## Project layout

```
mod.json                     mod metadata + settings
CMakeLists.txt               standard Geode CMake
src/main.cpp                 all the logic (hooks, lookahead checks, safe mode, HUD)
.github/workflows/build.yml  GitHub Actions build + release
```
