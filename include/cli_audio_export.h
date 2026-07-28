#ifndef ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H
#define ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace cli_audio {

enum class LoopMode {
    Crossfade
};

// How the RPM ladder is laid out between idle and redline. Pitch perception is
// logarithmic: Geometric keeps the interval between neighbouring layers
// constant in semitones, while Linear wastes resolution near the redline and
// leaves close to an octave between the first two layers.
enum class RpmSpacing {
    Geometric,
    Linear
};

// Weighting law used to splice the loop tail back into the loop head.
// EqualPower keeps uncorrelated content (air noise) at a constant level;
// Linear keeps correlated content (the phase-aligned firing pulses) constant.
enum class CrossfadeCurve {
    EqualPower,
    Linear
};

// How the fixed render gain is chosen.
//   Shared   - one gain for the whole set, so the WAV levels carry the engine's
//              real RPM -> loudness curve. Quiet tiers end up far below full
//              scale and lose 16-bit resolution.
//   PerLayer - every clip is rendered at its own gain so it lands on
//              peakTarget, and the factor that restores its true level relative
//              to the loudest clip is written to the manifest as gainOff/gainOn.
enum class LevelMode {
    PerLayer,
    Shared
};

// Cutoff of the low-pass applied to the up-sampled simulator signal.
// Legacy is the 1900 Hz value hard-coded for the real-time application; Auto
// opens it up to everything the simulation frequency can actually carry.
enum class InputBandwidth {
    Auto,
    Legacy
};

struct ExportConfig {
    std::string enginePath;
    std::string engineId;
    std::string outputDir;
    std::string assetsRoot = "assets";
    std::string esRoot = "es";

    // --- RPM ladder ---------------------------------------------------------
    std::vector<int> rpms;  // explicit list; bypasses the generated ladder
    int steps = 18;
    RpmSpacing spacing = RpmSpacing::Geometric;
    int rpmRoundStep = 25;
    bool idleAuto = true;
    bool redlineAuto = true;
    int idleRpm = 900;
    int redlineRpm = 6500;

    // --- loop geometry ------------------------------------------------------
    // clipDurationSec is a target: the real length is rounded to a whole number
    // of engine cycles so the splice lands on the same point of the firing
    // sequence and the crossfade blends phase-aligned material.
    double clipDurationSec = 1.0;
    int minCyclesPerLoop = 6;
    bool cycleAlign = true;
    int crossfadeMs = 15;
    CrossfadeCurve crossfadeCurve = CrossfadeCurve::EqualPower;
    LoopMode loopMode = LoopMode::Crossfade;

    // --- capture ------------------------------------------------------------
    // All durations below are simulated seconds, never wall clock.
    double warmupSec = 2.0;
    double spinTimeoutSec = 12.0;
    double rpmTolerance = 0.02;  // convergence band and post-capture check
    int sampleRate = 44100;
    unsigned int randomSeed = 42;

    // --- level --------------------------------------------------------------
    // The real-time AGC is replaced by a single fixed gain measured up front so
    // the natural RPM -> loudness relationship survives the export.
    bool autoGain = true;
    double manualGain = 1.0;
    // Clip-to-clip peak scatter is a couple of dB, so aim a little low: the
    // headroom is what keeps the hottest clip off full scale.
    double peakTarget = 0.65;  // fraction of full scale
    LevelMode levelMode = LevelMode::PerLayer;
    InputBandwidth inputBandwidth = InputBandwidth::Auto;

    // When > 0, render a single continuous idle->redline->idle rev sweep of
    // this many seconds to "<id>_sweep.wav" instead of the tiered set.
    double sweepSeconds = 0.0;
};

struct ClipReport {
    std::string file;
    double measuredRpm = 0.0;  // mean crank speed over the captured window
    double peak = 0.0;         // 0..1
    double rms = 0.0;          // 0..1
    // Factor a player applies to this clip to put it back on the engine's real
    // loudness curve. Always 1.0 in LevelMode::Shared, where the curve is
    // already baked into the samples.
    double playbackGain = 1.0;
    bool clipped = false;
};

struct LayerReport {
    int rpm = 0;
    int cycles = 0;
    int loopSamples = 0;
    ClipReport off;
    ClipReport on;
};

struct ExportResult {
    bool success = false;
    std::string message;
    std::vector<std::string> writtenFiles;
    std::vector<LayerReport> layers;
    std::vector<std::string> warnings;
    double renderGain = 1.0;
};

bool parseArgs(int argc, char **argv, ExportConfig *config, std::string *usageError);
ExportResult runExport(const ExportConfig &config);
std::string sha256HexFile(const std::string &path);

// --- pure helpers, exposed for testing --------------------------------------

// Seconds of one full four-stroke cycle (two crank revolutions).
double cycleSeconds(double rpm);

// Layer ladder between idle and redline. The first and last entries are always
// idle and redline; duplicates produced by rounding are collapsed.
std::vector<int> buildRpmLadder(
    int idleRpm,
    int redlineRpm,
    int steps,
    RpmSpacing spacing,
    int roundStep);

// Whole engine cycles that fit in targetSeconds at this RPM, at least minCycles.
int cyclesInLoop(int rpm, double targetSeconds, int minCycles);

// Sample count of a loop holding `cycles` whole engine cycles at this RPM.
int loopSampleCount(int rpm, int cycles, int sampleRate);

// `captured` must hold loopSamples + crossfadeSamples of material. The trailing
// crossfadeSamples are blended into the head and then dropped, so the returned
// buffer is exactly loopSamples long and seamless end-to-start.
std::vector<int16_t> crossfadeLoop(
    const std::vector<int16_t> &captured,
    int crossfadeSamples,
    CrossfadeCurve curve);

} /* namespace cli_audio */

#endif /* ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H */
