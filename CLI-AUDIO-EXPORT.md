# CLI audio export (technical)

Headless NEODRIVE export for [engine-sim](https://github.com/ange-yaghi/engine-sim). Automates the manual pipeline from [How To Make Car Sound](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound) (dyno hold, idle/gas pairs, seamless loops).

## Pipeline

`engine-sim-cli` loads an engine `.mr` script, holds RPM on the dyno, captures **internal synthesizer PCM** (not WASAPI), applies a crossfade loop, and writes mono 16-bit WAV files plus optional `manifest.json`.

Per RPM tier:

```
{id}_{rpm}_off.wav   # throttle released
{id}_{rpm}_on.wav    # throttle fully pressed
```

## Interactive export

```cmd
.\export-audio.cmd
```

See [README-CLI-AUDIO-EXPORT.md](README-CLI-AUDIO-EXPORT.md) for the full public guide.

## Usage

```powershell
.\build\Release\engine-sim-cli.exe `
  --engine assets/engines/kohler/kohler_ch750.mr `
  --engine-id kohler_ch750 `
  --output out/kohler_ch750 `
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
| `--engine` | Path to engine `.mr` file |
| `--engine-id` | Output ID prefix |
| `--output` | Output directory |
| `--steps N` | RPM tiers idle→redline (rounded to 100 rpm) |
| `--rpms a,b,c` | Explicit RPM list (overrides `--steps`) |
| `--idle auto\|RPM` | Idle RPM |
| `--redline auto\|RPM` | Redline RPM |
| `--clip-duration` | Loop length in seconds (default 1.0) |
| `--warmup` | Warmup before capture (default 2.0 s) |
| `--sample-rate` | Output sample rate (default 44100) |
| `--loop-mode crossfade` | Seamless loop via crossfade (~50 ms) |

## manifest.json

Optional metadata — not required at game runtime.

```json
{
  "id": "kohler_ch750",
  "sampleRate": 44100,
  "clipDuration": 1.0,
  "loopMode": "crossfade",
  "format": "NEODRIVE",
  "layers": [
    { "rpm": 1000, "off": "kohler_ch750_1000_off.wav", "on": "kohler_ch750_1000_on.wav" }
  ]
}
```

## Simulation details

1. Dyno enabled + **RPM hold** at target tier
2. Ignition enabled programmatically
3. Throttle off → `setSpeedControl(0.01)`; on → full
4. PCM via `Simulator::readAudioOutput()`
5. Crossfade at loop boundaries

## Tests

```powershell
cmake --build build --config Release --target engine-sim-test
.\build\Release\engine-sim-test.exe
```

## Out of scope

- GUI, transmission gearing, vehicle drag
- macOS / Linux
- WASAPI / Audacity capture
- Wankel (`wankel_engine`) catalog scripts
