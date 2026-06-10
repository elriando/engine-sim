# CLI audio export (NEODRIVE)

Headless export of engine-sim PCM audio into NEODRIVE loop sets for [lil-drift](https://github.com/elriando/lil-drift).

## Overview

`engine-sim-cli` loads an engine `.mr` script, holds RPM on the dyno (`HOLD` + programmatic throttle), captures **internal synthesizer PCM** (not WASAPI), applies a crossfade loop, and writes mono 16-bit WAV files plus `manifest.json`.

Each RPM tier produces two files:

```
{id}_{rpm}_off.wav   # throttle released
{id}_{rpm}_on.wav    # throttle fully pressed
```

## Usage

```powershell
engine-sim-cli `
  --engine assets/engines/kohler/kohler_ch750.mr `
  --engine-id kohler_ch750 `
  --output ./export/kohler_ch750 `
  --steps 8 `
  --idle auto --redline auto `
  --clip-duration 1.0 `
  --warmup 2.0 `
  --sample-rate 44100 `
  --loop-mode crossfade
```

### Arguments

| Flag | Description |
|------|-------------|
| `--engine` | Path to engine `.mr` file (must expose `main` node) |
| `--engine-id` | Output ID prefix (e.g. `v8` → `v8_2500_on.wav`) |
| `--output` | Output directory |
| `--steps N` | Evenly spaced RPM tiers from idle to redline (default 8) |
| `--rpms a,b,c` | Explicit RPM list (overrides `--steps`) |
| `--idle auto\|RPM` | Idle RPM (`auto` = engine dyno min) |
| `--redline auto\|RPM` | Redline RPM (`auto` = engine redline) |
| `--clip-duration` | Loop length in seconds (default 1.0) |
| `--warmup` | Simulation warmup before capture (default 2.0 s) |
| `--sample-rate` | Output sample rate (default 44100) |
| `--loop-mode crossfade` | Seamless loop via equal-power crossfade (~50 ms) |

## MVP tiers

| Phase | Tiers × throttle | WAV count |
|-------|------------------|-----------|
| MVP | 1 × 2 | 2 |
| Full | 8 × 2 | 16 |

Use `--steps 1` for MVP smoke tests.

## manifest.json

```json
{
  "id": "kohler_ch750",
  "idleRpm": 800,
  "redline": 3600,
  "sampleRate": 44100,
  "clipDuration": 1.0,
  "loopMode": "crossfade",
  "layers": [
    { "rpm": 800, "off": "kohler_ch750_800_off.wav", "on": "kohler_ch750_800_on.wav" }
  ]
}
```

## lil-drift integration

Copy export output into the game assets tree:

```powershell
xcopy /E /I export\kohler_ch750 ..\lil-drift\src\assets\audio\engines\kohler_ch750
cd ..\lil-drift
node scripts/gen-engine-manifest.mjs --write
```

The script scans `src/assets/audio/engines/<id>/` and fills `ENGINE_SETS` in `src/lib/engineAudio.ts`. Runtime mixing is handled by `soundManager.ts` (NEODRIVE N-layer crossfade).

## Simulation details

Export uses the same physics/audio path as the GUI:

1. Dyno enabled + **RPM hold** at target tier
2. Ignition enabled programmatically
3. Throttle: `off` → `setSpeedControl(0.01)` / `setThrottle(0)`; `on` → full
4. PCM read via `Simulator::readAudioOutput()` (internal ring buffer)
5. Crossfade applied to loop boundaries for seamless playback

## Tests

```powershell
ctest --test-dir build -C Release -R CliExportGolden
```

Golden test exports a single Kohler tier and verifies WAV + manifest structure.

## Out of scope

- GUI, transmission gearing, vehicle drag tuning
- macOS / Linux builds
- WASAPI or external recorder capture
