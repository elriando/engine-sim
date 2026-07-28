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
  --steps 18 `
  --clip-duration 1.0 --warmup 2.0 --sample-rate 44100
```

Every other setting has a default tuned for quality; the sections below explain
what they do and when to change them.

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
| Seamless loop | Trim at zero + fade in Audacity | Whole-cycle length + crossfade |
| Many RPM steps | Repeat for each tier | `--steps 18` or `--rpms` |
| File names | `4000 idle.wav` / `4000 gas.wav` | `{id}_4000_off.wav` / `{id}_4000_on.wav` |

---

## Use in your game

Copy the WAV folder into your project audio assets. At runtime (from the [devlog](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound)):

- Find the **two RPM tiers** that bracket current engine RPM.
- **Volume**: linear blend between lower/upper tier (e.g. 4800 RPM → 80% at 5000, 20% at 4000).
- **Throttle**: multiply each tier’s off/on volume by gas (0–1).
- **Pitch**: `playbackRate = engineRPM / tierRPM` per active layer.

Reference implementation (CC0): EngineAudio class gist linked in the [devlog comments](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound).

`manifest.json` is **not required at runtime** — the WAV files and the naming
pattern are enough. It is worth reading anyway: it carries the RPM each clip was
*actually* recorded at, the exact loop length in samples, and the per-clip
`gainOff` / `gainOn` you need if you want the engine's real loudness curve rather
than your own. See [CLI-AUDIO-EXPORT.md](CLI-AUDIO-EXPORT.md) for the schema.

---

## Catalog engines

Paste a catalog URL or part number in `export-audio.cmd`. Scripts are downloaded via API, adapted for CLI (vehicle/`run` blocks stripped), and saved under `assets/engines/catalog/part_<id>/`.

**Not supported:** Wankel / `wankel_engine` scripts (e.g. catalog #2906). Piston engines only on this branch.

Always check **Detected ID** matches the real engine node, not a helper (`vtc_camshaft_builder`, etc.).

---

## CLI flags

| Flag | Default | Description |
|------|---------|-------------|
| `--engine` | — | Path to `.mr` engine script |
| `--engine-id` | — | Output prefix / manifest id |
| `--output` | — | Output directory |
| **Layers** | | |
| `--steps N` | `18` | Number of RPM tiers between idle and redline |
| `--spacing geometric\|linear` | `geometric` | Geometric keeps the gap between tiers constant in semitones |
| `--rpm-round N` | `25` | Rounding applied to generated tiers |
| `--rpms a,b,c` | — | Explicit RPM list (overrides `--steps`) |
| `--idle auto\|RPM` | `auto` | Idle RPM |
| `--redline auto\|RPM` | `auto` | Redline RPM |
| **Loop** | | |
| `--clip-duration S` | `1.0` | Target loop length; rounded to whole engine cycles |
| `--min-cycles N` | `6` | Floor on cycles per loop, for low RPM tiers |
| `--no-cycle-align` | off | Use exactly `--clip-duration`, cycles be damned |
| `--crossfade-ms N` | `15` | Length of the loop splice |
| `--crossfade-curve equal-power\|linear` | `equal-power` | Weighting law for the splice |
| **Capture** | | |
| `--warmup S` | `2.0` | Simulated settling time before recording |
| `--spin-timeout S` | `12.0` | Simulated seconds allowed to reach a tier |
| `--rpm-tolerance F` | `0.02` | Band the mean RPM must land in, or the export fails |
| `--sample-rate N` | `44100` | Sample rate |
| `--seed N` | `42` | Noise seed; same seed gives the same render |
| **Level** | | |
| `--level per-layer\|shared` | `per-layer` | See *Levels* below |
| `--gain auto\|F` | `auto` | Fixed render gain, replacing the real-time AGC |
| `--peak-target F` | `0.65` | Fraction of full scale each clip aims for |
| `--input-bandwidth auto\|legacy` | `auto` | `auto` opens the up-sampling filter to 0.45 × simulation frequency; `legacy` pins it to the 1900 Hz the game uses |
| **Preview** | | |
| `--sweep S` | — | Render one continuous idle→redline→idle sweep instead of tiers |

### Why the tiers are spaced geometrically

Pitch perception is logarithmic, and the player pitch-shifts each layer by
`engineRPM / tierRPM`. With a linear ladder from 900 to 6500 rpm the first gap is
a factor of **1.9** — nearly an octave of pitch-shifting — while the last is
1.14. A geometric ladder spends the layers where the ear needs them:

| Tiers | Gap between neighbours |
|-------|------------------------|
| 8, linear | up to 12 semitones at the bottom |
| 12, geometric | 3.1 semitones |
| **18, geometric** | **2.0 semitones** |
| **24, geometric** | **1.5 semitones** |

### Why loops are a whole number of engine cycles

A four-stroke completes one cycle every `120 / rpm` seconds. A fixed 1.0 s clip
at 900 rpm holds 7.5 cycles, so every wrap shifts the firing sequence by half a
cycle and the loop warbles. `--clip-duration` is therefore a *target*: the real
length is rounded to whole cycles, which also makes the head and the tail
phase-aligned so the crossfade blends matching material.

### Levels

The real-time application runs an AGC that re-levels every ~23 ms. Exporting
through it flattens the RPM→loudness relationship, pumps between combustion
pulses at low RPM, and leaves a different gain at every loop point. The exporter
replaces it with a fixed gain measured up front at the loudest operating point.

- `--level per-layer` (default): each clip is rendered at its own gain so it uses
  the full 16-bit range. `manifest.json` records `gainOff` / `gainOn`, the factor
  that puts the clip back on the engine's real loudness curve. Apply it in the
  player for physical dynamics, or ignore it and use your own volume curve.
- `--level shared`: one gain for the whole set, so the loudness curve is baked
  into the samples. Quiet tiers sit far below full scale. Roughly twice as fast,
  since no measurement pass is needed.

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
