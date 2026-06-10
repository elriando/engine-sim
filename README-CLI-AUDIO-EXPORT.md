# CLI Audio Export (NEODRIVE)

Headless engine audio exporter for games and tools. This fork (`feat/cli-audio-export`) adds **`engine-sim-cli`**: capture internal PCM from the physics/synthesizer pipeline (no SDL window, no WASAPI recording) and write **NEODRIVE** loop sets — mono 16-bit WAV files per RPM tier (`_off` / `_on`) plus `manifest.json`.

Designed for integration with [lil-drift](https://github.com/elriando/lil-drift) and similar N-layer engine audio systems.

---

## Quick start (Windows)

### 1. Prerequisites

| Tool | Notes |
|------|--------|
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | Desktop development with C++ |
| [CMake](https://cmake.org/download/) | 3.10+ |
| [vcpkg](https://github.com/microsoft/vcpkg) | Package manager |
| Git | With submodule support |

### 2. Clone

```powershell
git clone --recurse-submodules https://github.com/elriando/engine-sim.git
cd engine-sim
git remote add upstream https://github.com/ange-yaghi/engine-sim.git
git checkout feat/cli-audio-export
```

### 3. Install dependencies (vcpkg)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install sdl2 sdl2-image boost-filesystem --triplet x64-windows
```

Install [winflexbison](https://github.com/lexxmark/winflexbison) for the Piranha scripting compiler. See [BUILD-WINDOWS.md](BUILD-WINDOWS.md) for Flex/Bison paths and troubleshooting.

### 4. Build (Release)

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

Outputs:

| Binary | Path |
|--------|------|
| GUI | `build/Release/engine-sim-app.exe` |
| CLI exporter | `build/Release/engine-sim-cli.exe` |

---

## Interactive export (recommended)

From the repo root:

```powershell
.\export-audio.cmd
```

Or in PowerShell (note the `.\` prefix):

```powershell
.\export-audio.cmd
```

### Menu

```
  1. Local engine (assets\engines folder)
  2. Catalog link (catalog.engine-sim.parts)
  C. Quit
```

### Prompts

At every step you can answer:

| Key | Meaning |
|-----|---------|
| **Y** | Yes (default when shown as `[Y/n/C]`) |
| **N** | No (default when shown as `[y/N/C]`) |
| **C** | Cancel / go back |

### Catalog import

Choose **2**, then paste a link or part number:

```
https://catalog.engine-sim.parts/parts/2531
```

or just:

```
2531
```

The script downloads the engine script via the catalog API, adapts it for CLI export (strips GUI-only `vehicle` / `run` blocks), saves under `assets/engines/catalog/part_<id>/`, and runs the export.

### Output

WAV files land in:

```
out/<engine-id>/
  <engine-id>_1000_off.wav
  <engine-id>_1000_on.wav
  ...
  manifest.json
```

RPM tiers are spaced evenly from idle to redline and **rounded to the nearest 100 RPM** for clean game integration.

---

## Manual CLI usage

```powershell
.\build\Release\engine-sim-cli.exe `
  --engine assets\engines\kohler\kohler_ch750.mr `
  --engine-id kohler_ch750 `
  --output out\kohler_ch750 `
  --steps 8 `
  --clip-duration 1.0 `
  --warmup 2.0 `
  --sample-rate 44100 `
  --loop-mode crossfade
```

| Flag | Description |
|------|-------------|
| `--engine` | Path to `.mr` engine script |
| `--engine-id` | Output file prefix / manifest id |
| `--output` | Output directory |
| `--steps N` | Evenly spaced RPM tiers (rounded to 100 rpm) |
| `--rpms a,b,c` | Explicit RPM list (overrides `--steps`) |
| `--idle auto\|RPM` | Idle RPM |
| `--redline auto\|RPM` | Redline RPM |
| `--clip-duration` | Loop length in seconds (default 1.0) |
| `--warmup` | Warm-up time before capture (default 2.0 s) |
| `--sample-rate` | Output sample rate (default 44100) |
| `--loop-mode crossfade` | Seamless loop via crossfade |

---

## lil-drift integration

Copy the export folder into the game assets tree:

```powershell
xcopy /E /I out\kohler_ch750 ..\lil-drift\src\assets\audio\engines\kohler_ch750
cd ..\lil-drift
node scripts\gen-engine-manifest.mjs --write
```

This fills `ENGINE_SETS` in `src/lib/engineAudio.ts`. Runtime mixing is handled by `soundManager.ts` (NEODRIVE N-layer crossfade).

---

## How it works

1. Dyno enabled + **RPM hold** at each target tier
2. Throttle **off** (`0.01`) / **on** (`1.0`) per tier
3. PCM read from `Simulator::readAudioOutput()` (internal buffer)
4. Crossfade applied for seamless ~1 s loops
5. `manifest.json` written in NEODRIVE format

---

## Tests

```powershell
cmake --build build --config Release --target engine-sim-test
.\build\Release\engine-sim-test.exe
```

---

## Further reading

- [BUILD-WINDOWS.md](BUILD-WINDOWS.md) — detailed Windows build guide
- [CLI-AUDIO-EXPORT.md](CLI-AUDIO-EXPORT.md) — CLI reference

## Scope

- Windows only (this branch)
- Headless export only (no GUI changes)
- No transmission / gearing / vehicle drag tuning in export path

Upstream GUI docs remain in [README.md](README.md).
