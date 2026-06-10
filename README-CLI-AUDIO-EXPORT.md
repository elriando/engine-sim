# CLI Audio Export — NEODRIVE workflow

Public tooling for **[engine-sim](https://github.com/ange-yaghi/engine-sim)** that automates the car-engine sound pipeline described in the [NEODRIVE devlog — *How To Make Car Sound*](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound) by [sevencrane](https://sevencrane.itch.io/neodrive).

That guide records engine-sim through **Audacity**, trims loops by hand, and exports **idle + gas** clips per RPM step. **`engine-sim-cli`** does the same job headlessly: dyno hold, throttle off/on, internal PCM capture, seamless crossfade loops — no WASAPI, no manual editing.

Fork: [elriando/engine-sim](https://github.com/elriando/engine-sim) · branch **`feat/cli-audio-export`**

---

## What NEODRIVE needs (from the guide)

1. **RPM tiers** — samples every ~1000 RPM from idle to redline (this tool uses **8 steps**, rounded to **100 RPM**).
2. **Two clips per tier** — throttle off (`_off`) and throttle on (`_on`).
3. **Seamless ~1 s loops** — crossfade at loop boundaries (replaces Audacity fade in/out).
4. **Runtime blending** — interpolate volume between the two surrounding tiers; pitch = `engineRPM / tierRPM`; mix off/on by throttle.

This CLI produces step 1–3. Step 4 is implemented in your game engine (Unity, Godot, etc.) using the same math as the devlog.

---

## Quick start (Windows)

### Build

```powershell
git clone --recurse-submodules https://github.com/elriando/engine-sim.git
cd engine-sim
git checkout feat/cli-audio-export

cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Prerequisites: VS 2022, CMake, vcpkg (SDL2, Boost), winflexbison — see [BUILD-WINDOWS.md](BUILD-WINDOWS.md).

### Export (interactive)

```powershell
.\export-audio.cmd
```

| Key | Action |
|-----|--------|
| **1** | Local engine under `assets/engines/` |
| **2** | [catalog.engine-sim.parts](https://catalog.engine-sim.parts/) link or part number |
| **Y / N / C** | Yes / No / Cancel at each prompt |

### Export (one-liner)

```powershell
.\build\Release\engine-sim-cli.exe `
  --engine assets\engines\kohler\kohler_ch750.mr `
  --engine-id kohler_ch750 `
  --output out\kohler_ch750 `
  --steps 8 `
  --clip-duration 1.0 --warmup 2.0 --sample-rate 44100 `
  --loop-mode crossfade
```

---

## Output layout

```
out/<engine-id>/
  <engine-id>_<rpm>_off.wav    # throttle released  (NEODRIVE "idle")
  <engine-id>_<rpm>_on.wav     # throttle pressed   (NEODRIVE "gas")
  manifest.json                # optional metadata for tooling
```

Use **lowercase `snake_case`** ids (e.g. `rb26dett_r32`, `toyota_2jz_gte`).

Normalize folders after manual exports:

```powershell
powershell -File scripts\normalize-engine-exports.ps1
```

---

## Manual workflow vs this tool

| Step | NEODRIVE guide (manual) | `engine-sim-cli` |
|------|--------------------------|------------------|
| Hold RPM | Engine-sim dyno hold | Dyno hold (programmatic) |
| Record audio | Audacity system capture | Internal PCM buffer |
| Throttle off/on | Two separate recordings | `setSpeedControl(0.01)` / full |
| Seamless loop | Trim at zero + fade in Audacity | Crossfade (~50 ms) |
| Many RPM steps | Repeat for each tier | `--steps 8` or `--rpms` |
| File names | `4000 idle.wav` / `4000 gas.wav` | `{id}_4000_off.wav` / `{id}_4000_on.wav` |

---

## Use in your game

Copy the WAV folder into your project audio assets. At runtime (from the [devlog](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound)):

- Find the **two RPM tiers** that bracket current engine RPM.
- **Volume**: linear blend between lower/upper tier (e.g. 4800 RPM → 80% at 5000, 20% at 4000).
- **Throttle**: multiply each tier’s off/on volume by gas (0–1).
- **Pitch**: `playbackRate = engineRPM / tierRPM` per active layer.

Reference implementation (CC0): EngineAudio class gist linked in the [devlog comments](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound).

`manifest.json` is **not required at runtime** — it documents which RPM tiers were exported. Your game only needs the WAV files and the same naming pattern.

---

## Catalog engines

Paste a catalog URL or part number in `export-audio.cmd`. Scripts are downloaded via API, adapted for CLI (vehicle/`run` blocks stripped), and saved under `assets/engines/catalog/part_<id>/`.

**Not supported:** Wankel / `wankel_engine` scripts (e.g. catalog #2906). Piston engines only on this branch.

Always check **Detected ID** matches the real engine node, not a helper (`vtc_camshaft_builder`, etc.).

---

## CLI flags

| Flag | Description |
|------|-------------|
| `--engine` | Path to `.mr` engine script |
| `--engine-id` | Output prefix / manifest id |
| `--output` | Output directory |
| `--steps N` | Evenly spaced tiers idle→redline (rounded to 100 rpm) |
| `--rpms a,b,c` | Explicit RPM list (overrides `--steps`) |
| `--idle auto\|RPM` | Idle RPM |
| `--redline auto\|RPM` | Redline RPM |
| `--clip-duration` | Loop length in seconds (default 1.0) |
| `--warmup` | Warm-up before capture (default 2.0 s) |
| `--sample-rate` | Sample rate (default 44100) |
| `--loop-mode crossfade` | Seamless loop |

---

## Project files

| Path | Role |
|------|------|
| `build/Release/engine-sim-cli.exe` | Headless exporter |
| `export-audio.cmd` | Interactive wizard |
| `scripts/export-engine-audio.ps1` | Wizard source |
| `scripts/normalize-engine-exports.ps1` | Rename exports to lowercase ids |
| `out/` | Generated WAV sets (gitignored) |

---

## More docs

- [BUILD-WINDOWS.md](BUILD-WINDOWS.md) — build troubleshooting
- [CLI-AUDIO-EXPORT.md](CLI-AUDIO-EXPORT.md) — technical details

## Credits

- [Engine Simulator](https://github.com/ange-yaghi/engine-sim) — ange-yaghi
- [NEODRIVE sound design guide](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound) — sevencrane

## Scope

Windows only · piston engines · no GUI / transmission in export path
