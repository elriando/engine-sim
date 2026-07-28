# CLI audio export (technical)

Headless NEODRIVE export for [engine-sim](https://github.com/ange-yaghi/engine-sim). Automates the manual pipeline from [How To Make Car Sound](https://sevencrane.itch.io/neodrive/devlog/1322297/how-to-make-car-sound) (dyno hold, idle/gas pairs, seamless loops).

## Pipeline

`engine-sim-cli` loads an engine `.mr` script, holds RPM on the dyno, captures **internal synthesizer PCM** (not WASAPI), splices a seamless loop, and writes mono 16-bit WAV files plus `manifest.json`.

Per RPM tier:

```
{id}_{rpm}_off.wav   # throttle released
{id}_{rpm}_on.wav    # throttle fully pressed
```

Four things separate this from a naive capture loop, all of them audible:

1. **Whole-cycle loop length.** A four-stroke cycle lasts `120 / rpm` seconds.
   `--clip-duration` is a target that gets rounded to whole cycles, so the splice
   lands on the same point of the firing sequence instead of mid-cycle. The tail
   blended into the head is therefore phase-aligned with it.
2. **The tail is dropped after blending.** The capture is `loop + crossfade`
   samples long and comes back exactly `loop` samples long.
3. **Fixed render gain.** The real-time `LevelingFilter` AGC is pinned to a
   constant so the RPM→loudness relationship, the inter-pulse noise floor and the
   gain at the loop point all stay put. See *Levels* in the public guide.
4. **Verified RPM.** Every tier is held until the *mean* crank speed is inside
   `--rpm-tolerance`, budgeted in simulated seconds rather than wall clock, and
   the mean measured during the capture is written to the manifest. A tier that
   drifts outside the band fails the export instead of shipping at the wrong
   pitch.

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
  --steps 18
```

Run with `--help` for the full flag list; see
[README-CLI-AUDIO-EXPORT.md](README-CLI-AUDIO-EXPORT.md) for defaults and the
reasoning behind them.

## manifest.json

Version 2 is a superset of version 1: `rpm`, `off` and `on` still carry the tier
and its two file names, so existing consumers keep working. Everything else is
measured during the export and is there to be checked.

```json
{
  "id": "kohler_ch750",
  "format": "NEODRIVE",
  "version": 2,
  "sampleRate": 44100,
  "clipDuration": 1.0,
  "loopMode": "crossfade",
  "crossfadeMs": 15,
  "crossfadeCurve": "equal-power",
  "spacing": "geometric",
  "cycleAligned": true,
  "levelMode": "per-layer",
  "idleRpm": 1000,
  "redlineRpm": 3600,
  "renderGain": 0.034,
  "layers": [
    {
      "rpm": 1000,
      "cycles": 7,
      "loopSamples": 37044,
      "loopSeconds": 0.840000,
      "off": "kohler_ch750_1000_off.wav",
      "measuredRpmOff": 1000.0,
      "gainOff": 0.030700,
      "peakOff": 0.4300,
      "rmsOff": 0.0512,
      "on": "kohler_ch750_1000_on.wav",
      "measuredRpmOn": 1000.0,
      "gainOn": 0.011100,
      "peakOn": 0.6700,
      "rmsOn": 0.0904
    }
  ]
}
```

| Field | Meaning |
|-------|---------|
| `cycles` / `loopSamples` | Whole engine cycles in the loop, and its exact length |
| `measuredRpm*` | Mean crank speed actually recorded, not the requested tier |
| `gain*` | Factor that restores the clip's true level relative to the loudest clip (always `1.0` under `--level shared`) |
| `peak*` / `rms*` | Measured levels, 0..1 |
| `renderGain` | The shared gain the per-layer gains are expressed against |

## Simulation details

1. Dyno enabled + **RPM hold** at target tier; the set-point is written on every
   solver iteration, not once per frame — a set-point that steps at frame rate
   makes the constraint solver ring at 60 Hz whenever the target is moving.
2. Ignition enabled programmatically
3. Throttle off → `setSpeedControl(0.01)`; on → full
4. Noise seeded on the rendering thread (`rand()` keeps per-thread state, so
   seeding from the caller never reached the renderer), making renders repeatable
5. PCM via `Simulator::readAudioOutput()`
6. Whole-cycle truncation + equal-power crossfade at the loop boundary

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
