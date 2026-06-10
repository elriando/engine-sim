# CLI Audio Export (NEODRIVE)

Headless engine audio exporter for **[lil-drift](https://github.com/elriando/lil-drift)** and similar games. Branch **`feat/cli-audio-export`** on [elriando/engine-sim](https://github.com/elriando/engine-sim).

Captures **internal PCM** from engine-sim (dyno hold + throttle), writes **mono 16-bit WAV** loops and an optional `manifest.json` per set.

---

## Quick start

### Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 (Desktop C++ workload)
- CMake 3.10+
- [vcpkg](https://github.com/microsoft/vcpkg) + SDL2 / SDL2_image / Boost
- [winflexbison](https://github.com/lexxmark/winflexbison) (see [BUILD-WINDOWS.md](BUILD-WINDOWS.md))

### Clone & build

```powershell
git clone --recurse-submodules https://github.com/elriando/engine-sim.git
cd engine-sim
git checkout feat/cli-audio-export

cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Interactive export

```powershell
.\export-audio.cmd
```

| Menu | Action |
|------|--------|
| **1** | Pick a local `.mr` under `assets/engines/` |
| **2** | Paste a [catalog.engine-sim.parts](https://catalog.engine-sim.parts/) link or part number |
| **C** | Cancel / quit |

Prompts use **`[Y/n/C]`** — **Y** yes, **N** no, **C** cancel.

---

## Naming convention (lil-drift)

All exports should use **lowercase `snake_case`** IDs:

```
out/<engine-id>/
  <engine-id>_<rpm>_off.wav
  <engine-id>_<rpm>_on.wav
  manifest.json          # optional for the game; useful for you
```

Examples: `sr20de_autech_4000_off.wav`, `toyota_2jz_gte_6100_on.wav`

RPM tiers are rounded to **nearest 100** when using `--steps 8`.

Normalize existing folders after manual exports:

```powershell
powershell -File scripts\normalize-engine-exports.ps1
```

---

## Your engine library (local `out/`)

Status after normalization. **Ready** = 8 tiers × 2 WAV (16 files). **MVP** = 1 tier only (smoke test).

| engine-id | Vehicle | Source | Tiers | Status |
|-----------|---------|--------|-------|--------|
| `sr20de_autech` | Nissan Silvia S15 Autech SR20DE | Catalog **#1517** | 8/8 | **Ready** |
| `skyline_r33` | Nissan Skyline R33 GT-R | Catalog **#672** | 8/8 | **Ready** |
| `toyota_2jz_gte` | Toyota Supra 2JZ-GTE | Catalog **#941** | 8/8 | **Ready** |
| `lamborghini_sian` | Lamborghini Sian V12 | Catalog *(exported)* | 8/8 | **Ready** (bonus) |
| `rb26dett_r32` | Nissan R32 GT-R RB26DETT | Catalog **#2531** | 1/8 | **Re-export** with `--steps 8` |
| `subaru_ej25` | Subaru WRX EJ25 boxer | Local `01_subaru_ej25_eh.mr` | 1/8 | **Re-export** with `--steps 8` |
| `kohler_ch750` | Kohler flat-twin (dev test) | Local | 8/8 | Dev only — re-export recommended |

### Still missing for a JDM / drift pack

| engine-id | Vehicle | How to export |
|-----------|---------|---------------|
| `honda_vtec_i4` | Honda VTEC (Civic/Integra vibe) | Menu **1** → `assets\engines\atg-video-1\05_honda_vtec.mr` |
| `rb26dett_r32` | R32 GT-R (full set) | Menu **2** → `2531`, full export **Y** |
| `subaru_ej25` | WRX STI (full set) | Menu **1** → `01_subaru_ej25_eh.mr`, full export **Y** |

### Not supported (this build)

| Engine | Reason |
|--------|--------|
| **13B rotary** (catalog #2906) | `wankel_engine` — not in this engine-sim compiler |
| Catalog scripts with `run()` / new vehicle APIs | Stripped automatically; some still fail |

---

## Re-export incomplete engines

```powershell
# RB26 R32 — full 8 tiers
.\build\Release\engine-sim-cli.exe `
  --engine assets\engines\catalog\part_2531\RB26DETT.mr `
  --engine-id rb26dett_r32 `
  --output out\rb26dett_r32 `
  --steps 8 --clip-duration 1.0 --warmup 2.0 --sample-rate 44100 --loop-mode crossfade

# Subaru EJ25 — full 8 tiers (local)
.\build\Release\engine-sim-cli.exe `
  --engine assets\engines\atg-video-2\01_subaru_ej25_eh.mr `
  --engine-id subaru_ej25 `
  --output out\subaru_ej25 `
  --steps 8 --clip-duration 1.0 --warmup 2.0 --sample-rate 44100 --loop-mode crossfade

# Honda VTEC — new
.\build\Release\engine-sim-cli.exe `
  --engine assets\engines\atg-video-1\05_honda_vtec.mr `
  --engine-id honda_vtec_i4 `
  --output out\honda_vtec_i4 `
  --steps 8 --clip-duration 1.0 --warmup 2.0 --sample-rate 44100 --loop-mode crossfade
```

Always confirm **Detected ID** in `export-audio.cmd` matches the real engine node (not a helper like `vtc_camshaft_builder`).

---

## lil-drift integration

Copy a ready set (WAV only is enough for the game):

```powershell
xcopy /E /I out\sr20de_autech C:\Users\Dorian\Projects\lil-drift\src\assets\audio\engines\sr20de_autech
cd C:\Users\Dorian\Projects\lil-drift
node scripts\gen-engine-manifest.mjs --write
```

Repeat per `engine-id`. `gen-engine-manifest.mjs` scans WAV filenames and fills `ENGINE_SETS` in `src/lib/engineAudio.ts`.

In `carManifest`, set `engineProfile.engineId` to the same id (e.g. `sr20de_autech`).

---

## Manual CLI reference

```powershell
.\build\Release\engine-sim-cli.exe `
  --engine <path-to.mr> `
  --engine-id <lowercase_id> `
  --output out\<lowercase_id> `
  --steps 8 `
  --clip-duration 1.0 --warmup 2.0 --sample-rate 44100 `
  --loop-mode crossfade
```

| Flag | Description |
|------|-------------|
| `--steps N` | Evenly spaced tiers idle→redline (rounded to 100 rpm) |
| `--rpms 1000,3000,5000` | Explicit tiers (overrides `--steps`) |
| `--idle auto` / `--redline auto` | From engine script |

---

## Project layout

| Path | Purpose |
|------|---------|
| `build/Release/engine-sim-cli.exe` | Headless exporter |
| `export-audio.cmd` | Interactive wizard |
| `scripts/export-engine-audio.ps1` | Wizard source |
| `scripts/normalize-engine-exports.ps1` | Rename `out/` folders to lowercase ids |
| `out/<engine-id>/` | Generated WAV sets (gitignored) |
| `assets/engines/catalog/part_<id>/` | Downloaded catalog scripts (gitignored) |

---

## Docs

- [BUILD-WINDOWS.md](BUILD-WINDOWS.md) — full Windows build guide
- [CLI-AUDIO-EXPORT.md](CLI-AUDIO-EXPORT.md) — technical CLI details

## Scope

Windows only · piston engines only · no GUI / transmission / gearing in export path
