#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "../include/cli_audio_export.h"
#include "../include/wav_io.h"
#include "../include/units.h"
#include "../include/impulse_response.h"
#include "../include/engine.h"
#include "../include/vehicle.h"
#include "../include/transmission.h"
#include "../include/simulator.h"
#include "../include/piston_engine_simulator.h"

#include "../scripting/include/compiler.h"

#include <delta-studio/include/yds_windows_audio_wave_file.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace cli_audio {

namespace {

// Four-stroke: one full cycle is two crank revolutions.
constexpr double kCrankRevsPerCycle = 2.0;
constexpr double kFrameDt = 1.0 / 60.0;
constexpr double kFullScale = 32767.0;

struct LoadedEngine {
    Engine *engine = nullptr;
    Vehicle *vehicle = nullptr;
    Transmission *transmission = nullptr;
    Simulator *simulator = nullptr;
};

struct Levels {
    double peak = 0.0;
    double rms = 0.0;
    bool clipped = false;
};

struct ClipCapture {
    std::vector<int16_t> pcm;
    double measuredRpm = 0.0;
};

// Mean crank speed over a window. The instantaneous reading swings by several
// percent inside one cycle under the combustion pulses, so every RPM decision
// in this file is taken on an average, never on a single sample.
struct RpmMeter {
    double sum = 0.0;
    long long count = 0;

    void add(double rpm) {
        sum += rpm;
        ++count;
    }

    double mean() const { return (count > 0) ? sum / count : 0.0; }
};

std::string joinPath(const std::string &a, const std::string &b) {
    return (std::filesystem::path(a) / b).string();
}

bool ensureDir(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

std::string formatDouble(double v, int decimals) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << v;
    return oss.str();
}

int16_t toPcm(double v) {
    const long r = std::lround(v);
    if (r > 32767) return 32767;
    if (r < -32768) return -32768;
    return static_cast<int16_t>(r);
}

Levels measureLevels(const std::vector<int16_t> &pcm) {
    Levels out;
    if (pcm.empty()) return out;

    double sumSq = 0.0;
    int peakAbs = 0;
    for (const int16_t s : pcm) {
        const int a = std::abs(static_cast<int>(s));
        if (a > peakAbs) peakAbs = a;
        if (a >= 32767) out.clipped = true;

        const double v = static_cast<double>(s);
        sumSq += v * v;
    }

    out.peak = std::min(1.0, peakAbs / kFullScale);
    out.rms = std::min(1.0, std::sqrt(sumSq / pcm.size()) / kFullScale);
    return out;
}

bool writeWrapperScript(
    const ExportConfig &config,
    const std::string &wrapperPath,
    std::string *error)
{
    const std::filesystem::path enginePath = std::filesystem::absolute(config.enginePath);
    const std::string engineFile = enginePath.filename().string();

    std::ofstream out(wrapperPath, std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "cannot write wrapper script";
        return false;
    }

    out << "import \"engine_sim.mr\"\n";
    out << "import \"" << engineFile << "\"\n";
    out << "public node main {\n";
    out << "    set_engine(" << config.engineId << "())\n";
    out << "}\n";
    out << "main()\n";
    return true;
}

bool loadImpulseResponses(Simulator *simulator, Engine *engine) {
    for (int i = 0; i < engine->getExhaustSystemCount(); ++i) {
        ImpulseResponse *response = engine->getExhaustSystem(i)->getImpulseResponse();
        if (response == nullptr) continue;

        ysWindowsAudioWaveFile waveFile;
        if (waveFile.OpenFile(response->getFilename().c_str()) != ysAudioFile::Error::None) {
            return false;
        }

        waveFile.InitializeInternalBuffer(waveFile.GetSampleCount());
        waveFile.FillBuffer(0);
        waveFile.CloseFile();

        simulator->synthesizer().initializeImpulseResponse(
            reinterpret_cast<const int16_t *>(waveFile.GetBuffer()),
            waveFile.GetSampleCount(),
            static_cast<float>(response->getVolume()),
            i);

        waveFile.DestroyInternalBuffer();
    }

    return true;
}

// Pins the leveler to a constant gain. The real-time AGC re-levels every ~23 ms
// with up to 1.9x of gain, which flattens the RPM -> loudness relationship,
// pumps between combustion pulses at low RPM and leaves a different gain state
// at every loop point. A fixed gain keeps the engine's own dynamics intact.
void setFixedGain(Simulator *sim, double gain) {
    Synthesizer::AudioParameters params = sim->synthesizer().getAudioParameters();
    params.levelerMinGain = static_cast<float>(gain);
    params.levelerMaxGain = static_cast<float>(gain);
    sim->synthesizer().setAudioParameters(params);
}

// Pulls everything the rendering thread has produced. Leaving samples in the
// ring buffer stalls the renderer, which caps itself at 2000 queued samples.
int drainAudio(Simulator *sim, std::vector<int16_t> *sink, int limit) {
    int16_t block[8192];
    int total = 0;

    while (true) {
        int want = 8192;
        if (sink != nullptr) {
            want = std::min(8192, limit - total);
            if (want <= 0) break;
        }

        const int got = sim->readAudioOutput(want, block);
        if (got <= 0) break;
        if (sink != nullptr) sink->insert(sink->end(), block, block + got);

        total += got;
        if (got < want) break;
    }

    return total;
}

// Runs one simulation frame, interpolating the throttle and the dyno set-point
// across the solver iterations. Writing the dyno target once per frame makes it
// step at 60 Hz; while the target is moving the constraint solver turns those
// steps into an audible 60 Hz buzz.
void runFrame(
    LoadedEngine &loaded,
    double throttleStart,
    double throttleEnd,
    double rpmStart,
    double rpmEnd,
    RpmMeter *meter)
{
    Simulator *sim = loaded.simulator;
    Engine *engine = loaded.engine;

    sim->startFrame(kFrameDt);
    const int steps = sim->simulationSteps();

    for (int i = 0; ; ++i) {
        const double u = (steps > 0)
            ? std::min(1.0, static_cast<double>(i) / steps)
            : 1.0;

        engine->setSpeedControl(throttleStart + (throttleEnd - throttleStart) * u);
        sim->m_dyno.m_rotationSpeed = units::rpm(rpmStart + (rpmEnd - rpmStart) * u);

        if (!sim->simulateStep()) break;
        if (meter != nullptr) meter->add(engine->getRpm());
    }

    sim->endFrame();
}

// Holds the engine at targetRpm until the *mean* crank speed is inside the
// tolerance band. Budgeted in simulated seconds: a wall-clock budget makes the
// result depend on how fast the host machine happens to be, which silently
// captures tiers at the wrong RPM on a slow run.
bool spinToRpm(
    LoadedEngine &loaded,
    const ExportConfig &config,
    double targetRpm,
    double throttle,
    std::string *error)
{
    Simulator *sim = loaded.simulator;
    sim->m_dyno.m_enabled = true;
    sim->m_dyno.m_hold = true;
    loaded.engine->getIgnitionModule()->m_enabled = true;

    const int windowFrames = std::max(
        4,
        static_cast<int>(std::ceil((2.0 * cycleSeconds(targetRpm)) / kFrameDt)));
    const int maxFrames = std::max(
        windowFrames + 1,
        static_cast<int>(std::ceil(config.spinTimeoutSec / kFrameDt)));

    bool starting = loaded.engine->getRpm() < targetRpm * 0.5;
    sim->m_starterMotor.m_enabled = starting;

    std::deque<double> window;
    double windowSum = 0.0;
    double lastMean = 0.0;

    for (int frame = 0; frame < maxFrames; ++frame) {
        RpmMeter meter;
        runFrame(loaded, throttle, throttle, targetRpm, targetRpm, &meter);
        drainAudio(sim, nullptr, 0);

        if (meter.count > 0) {
            window.push_back(meter.mean());
            windowSum += window.back();
            if (static_cast<int>(window.size()) > windowFrames) {
                windowSum -= window.front();
                window.pop_front();
            }
        }

        if (starting && loaded.engine->getRpm() > targetRpm * 0.6) {
            sim->m_starterMotor.m_enabled = false;
            starting = false;
        }

        if (static_cast<int>(window.size()) == windowFrames) {
            lastMean = windowSum / windowFrames;
            if (std::abs(lastMean - targetRpm) <= targetRpm * config.rpmTolerance) {
                sim->m_starterMotor.m_enabled = false;
                return true;
            }
        }
    }

    sim->m_starterMotor.m_enabled = false;
    if (error != nullptr) {
        *error = "engine did not settle at " + std::to_string(static_cast<int>(targetRpm))
            + " rpm within " + formatDouble(config.spinTimeoutSec, 1)
            + " s of simulated time (mean reached "
            + formatDouble(lastMean, 1) + " rpm)";
    }
    return false;
}

// Runs at the operating point without recording, long enough for the gas system
// transient left by the previous tier (and any gain change) to die out.
void settle(
    LoadedEngine &loaded,
    double targetRpm,
    double throttle,
    double seconds)
{
    const int frames = static_cast<int>(std::ceil(seconds / kFrameDt));
    for (int i = 0; i < frames; ++i) {
        runFrame(loaded, throttle, throttle, targetRpm, targetRpm, nullptr);
        drainAudio(loaded.simulator, nullptr, 0);
    }
}

bool recordSamples(
    LoadedEngine &loaded,
    const ExportConfig &config,
    double targetRpm,
    double throttle,
    int sampleCount,
    ClipCapture *out,
    std::string *error)
{
    const double clipSeconds = static_cast<double>(sampleCount) / config.sampleRate;
    const int frameBudget =
        static_cast<int>(std::ceil(clipSeconds / kFrameDt)) * 4 + 240;

    RpmMeter meter;
    out->pcm.clear();
    out->pcm.reserve(static_cast<size_t>(sampleCount));

    for (int frame = 0; static_cast<int>(out->pcm.size()) < sampleCount; ++frame) {
        if (frame > frameBudget) {
            if (error != nullptr) {
                *error = "audio pipeline produced no samples while capturing "
                    + std::to_string(static_cast<int>(targetRpm)) + " rpm";
            }
            return false;
        }

        runFrame(loaded, throttle, throttle, targetRpm, targetRpm, &meter);
        drainAudio(
            loaded.simulator,
            &out->pcm,
            sampleCount - static_cast<int>(out->pcm.size()));
    }

    out->pcm.resize(static_cast<size_t>(sampleCount));
    out->measuredRpm = meter.mean();
    return true;
}

bool captureClip(
    LoadedEngine &loaded,
    const ExportConfig &config,
    double targetRpm,
    double throttle,
    int sampleCount,
    double warmupSec,
    ClipCapture *out,
    std::string *error)
{
    if (!spinToRpm(loaded, config, targetRpm, throttle, error)) return false;
    settle(loaded, targetRpm, throttle, warmupSec);
    return recordSamples(loaded, config, targetRpm, throttle, sampleCount, out, error);
}

// Captures one clip and, in per-layer mode, retunes the render gain so the clip
// lands on peakTarget.
//
// The measurement pass always runs at the shared gain, which the redline probe
// already proved is non-saturating everywhere. That matters: a saturated
// capture reads back as a peak of exactly 1.0 however far past full scale it
// really is, so correcting from it converges far too slowly to be useful.
bool captureLeveled(
    LoadedEngine &loaded,
    const ExportConfig &config,
    double targetRpm,
    double throttle,
    int sampleCount,
    double sharedGain,
    double *gainUsed,
    ClipCapture *out,
    std::string *error)
{
    if (!spinToRpm(loaded, config, targetRpm, throttle, error)) return false;
    settle(loaded, targetRpm, throttle, config.warmupSec);

    // Flushes whatever is still queued at the previous clip's gain.
    auto applyGain = [&](double gain) {
        setFixedGain(loaded.simulator, gain);
        settle(loaded, targetRpm, throttle, 0.1);
    };

    *gainUsed = sharedGain;
    applyGain(sharedGain);

    if (config.levelMode == LevelMode::Shared) {
        return recordSamples(loaded, config, targetRpm, throttle, sampleCount, out, error);
    }

    const int probeSamples =
        std::min(sampleCount, static_cast<int>(0.5 * config.sampleRate));

    ClipCapture probe;
    if (!recordSamples(loaded, config, targetRpm, throttle, probeSamples, &probe, error)) {
        return false;
    }

    const double probePeak = measureLevels(probe.pcm).peak;
    if (probePeak > 1e-9) {
        *gainUsed = sharedGain * config.peakTarget / probePeak;
        applyGain(*gainUsed);
    }

    // The probe is shorter than the clip and sees a different stretch of the
    // combustion sequence, so its peak estimate can be off in either direction.
    // One correction pass on the full clip pulls the outliers back in.
    for (int pass = 0; ; ++pass) {
        if (!recordSamples(loaded, config, targetRpm, throttle, sampleCount, out, error)) {
            return false;
        }

        const double peak = measureLevels(out->pcm).peak;
        if (pass >= 1 || peak <= 1e-9) break;
        if (peak <= 0.97 && peak >= config.peakTarget * 0.45) break;

        *gainUsed *= config.peakTarget / peak;
        applyGain(*gainUsed);
    }

    return true;
}

// Brackets the engine's own output level so the whole set can go through one
// fixed gain. Both throttle positions are probed at the top of the range:
// closed-throttle motoring is not reliably quieter than wide open throttle, so
// probing only the power stroke under-reads the peak and the top tier clips.
bool measureRenderGain(
    LoadedEngine &loaded,
    const ExportConfig &config,
    int loudestRpm,
    const double *throttles,
    int throttleCount,
    double *gainOut,
    std::string *error)
{
    // Long enough that the probe sees a peak distribution comparable to the
    // real clips -- a short probe systematically under-reads the maximum.
    const int probeSamples = static_cast<int>(0.8 * config.sampleRate);
    double probeGain = 1.0;

    for (int attempt = 0; attempt < 6; ++attempt) {
        setFixedGain(loaded.simulator, probeGain);

        double peak = 0.0;
        for (int i = 0; i < throttleCount; ++i) {
            ClipCapture probe;
            if (!captureClip(
                    loaded, config, loudestRpm, throttles[i],
                    probeSamples, 0.5, &probe, error)) {
                return false;
            }
            peak = std::max(peak, measureLevels(probe.pcm).peak);
        }

        if (peak >= 0.98) {
            probeGain *= 0.05;  // saturated, the true peak is off the top
            continue;
        }
        if (peak <= 0.01) {
            probeGain *= 20.0;  // too quiet to measure accurately
            continue;
        }

        *gainOut = probeGain * config.peakTarget / peak;
        return true;
    }

    if (error != nullptr) {
        *error = "could not bracket the engine's output level; pass --gain <value>";
    }
    return false;
}

double sweepRpm(double phase, double idleRpm, double redlineRpm) {
    const double s = std::sin(3.14159265358979323846 * phase);  // 0..1..0
    return idleRpm + (redlineRpm - idleRpm) * s;
}

// Full throttle on the way up, closed on the way down, with a short smooth
// hand-over at the top instead of a step discontinuity.
double sweepThrottle(double phase) {
    constexpr double width = 0.06;
    const double t = (phase - (0.5 - width * 0.5)) / width;
    const double c = std::min(1.0, std::max(0.0, t));
    return 1.0 - (c * c * (3.0 - 2.0 * c));
}

std::vector<int16_t> captureSweep(
    LoadedEngine &loaded,
    const ExportConfig &config,
    double idleRpm,
    double redlineRpm,
    std::string *error)
{
    Simulator *sim = loaded.simulator;
    sim->m_dyno.m_enabled = true;
    sim->m_dyno.m_hold = true;
    loaded.engine->getIgnitionModule()->m_enabled = true;

    if (!spinToRpm(loaded, config, idleRpm, 1.0, error)) return {};

    const int totalSamples =
        static_cast<int>(config.sweepSeconds * config.sampleRate);
    std::vector<int16_t> captured;
    captured.reserve(static_cast<size_t>(totalSamples));

    const int frameBudget =
        static_cast<int>(std::ceil(config.sweepSeconds / kFrameDt)) * 4 + 240;
    int nextProgress = 0;

    for (int frame = 0; static_cast<int>(captured.size()) < totalSamples; ++frame) {
        if (frame > frameBudget) {
            if (error != nullptr) *error = "audio pipeline stalled during the sweep";
            return {};
        }

        const double phase0 =
            static_cast<double>(captured.size()) / totalSamples;
        const double phase1 =
            std::min(1.0, phase0 + kFrameDt / config.sweepSeconds);

        runFrame(
            loaded,
            sweepThrottle(phase0), sweepThrottle(phase1),
            sweepRpm(phase0, idleRpm, redlineRpm),
            sweepRpm(phase1, idleRpm, redlineRpm),
            nullptr);

        drainAudio(
            sim, &captured, totalSamples - static_cast<int>(captured.size()));

        const int pct = static_cast<int>(phase0 * 100.0);
        if (pct >= nextProgress) {
            std::cout << "PROGRESS " << static_cast<int>(captured.size())
                      << " " << totalSamples << std::endl;
            nextProgress += 5;
        }
    }

    captured.resize(static_cast<size_t>(totalSamples));
    return captured;
}

es_script::Compiler::Output compileEngineScript(
    const ExportConfig &config,
    const std::string &scriptPath,
    std::string *error)
{
    es_script::Compiler compiler;
    compiler.initialize();
    compiler.addSearchPath(std::filesystem::absolute(config.esRoot).string());
    compiler.addSearchPath(std::filesystem::absolute(config.assetsRoot).string());
    compiler.addSearchPath(
        std::filesystem::absolute(config.enginePath).parent_path().string());

    piranha::IrPath mainPath(scriptPath.c_str());
    if (!compiler.compile(mainPath)) {
        if (error != nullptr) *error = "compile failed - see error_log.log";
        compiler.destroy();
        return {};
    }

    const es_script::Compiler::Output output = compiler.execute();
    compiler.destroy();
    return output;
}

bool loadEngineFromScript(const ExportConfig &config, LoadedEngine *loaded, std::string *error) {
    const std::filesystem::path engineAbs = std::filesystem::absolute(config.enginePath);
    if (!std::filesystem::exists(engineAbs)) {
        if (error != nullptr) *error = "engine file not found: " + config.enginePath;
        return false;
    }

    const std::string engineDir = engineAbs.parent_path().string();
    std::string loadError;
    const std::string wrapperPath = joinPath(engineDir, "_cli_export_main.mr");
    if (!writeWrapperScript(config, wrapperPath, error)) return false;

    es_script::Compiler::Output output =
        compileEngineScript(config, wrapperPath, &loadError);

    if (output.engine == nullptr) {
        if (error != nullptr) {
            *error = loadError.empty()
                ? "script did not produce an engine (check --engine-id matches public node)"
                : loadError;
        }
        return false;
    }

    Vehicle *vehicle = output.vehicle;
    Transmission *transmission = output.transmission;

    if (vehicle == nullptr) {
        Vehicle::Parameters vehParams;
        vehParams.mass = units::mass(1597, units::kg);
        vehParams.diffRatio = 3.42;
        vehParams.tireRadius = units::distance(10, units::inch);
        vehParams.dragCoefficient = 0.25;
        vehParams.crossSectionArea = units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
        vehParams.rollingResistance = 2000.0;
        vehicle = new Vehicle;
        vehicle->initialize(vehParams);
    }

    if (transmission == nullptr) {
        const double gearRatios[] = { 2.97, 2.07, 1.43, 1.00, 0.84, 0.56 };
        Transmission::Parameters tParams;
        tParams.GearCount = 6;
        tParams.GearRatios = gearRatios;
        tParams.MaxClutchTorque = units::torque(1000.0, units::ft_lb);
        transmission = new Transmission;
        transmission->initialize(tParams);
    }

    Simulator::Parameters simParams;
    simParams.systemType = Simulator::SystemType::NsvOptimized;
    PistonEngineSimulator *simulator = new PistonEngineSimulator;
    simulator->initialize(simParams);
    simulator->loadSimulation(output.engine, vehicle, transmission);
    // The simulator stores the frequency as an int, so derive the synthesizer's
    // input rate from the same truncated value it actually steps at.
    const int simulationFrequency =
        static_cast<int>(output.engine->getSimulationFrequency());
    simulator->setSimulationFrequency(simulationFrequency);
    simulator->setTargetSynthesizerLatency(0.05);

    Synthesizer::AudioParameters audioParams = simulator->synthesizer().getAudioParameters();
    audioParams.inputSampleNoise = static_cast<float>(output.engine->getInitialJitter());
    audioParams.airNoise = static_cast<float>(output.engine->getInitialNoise());
    audioParams.dF_F_mix = static_cast<float>(output.engine->getInitialHighFrequencyGain());
    audioParams.levelerMinGain = 1.0f;
    audioParams.levelerMaxGain = 1.0f;
    simulator->synthesizer().setAudioParameters(audioParams);

    const float audioSampleRate = static_cast<float>(config.sampleRate);
    const float inputSampleRate = static_cast<float>(simulationFrequency);

    Synthesizer::Parameters synthParams;
    synthParams.audioBufferSize = config.sampleRate * 2;
    synthParams.audioSampleRate = audioSampleRate;
    synthParams.inputBufferSize = config.sampleRate;
    synthParams.inputChannelCount = output.engine->getExhaustSystemCount();
    synthParams.inputSampleRate = inputSampleRate;
    synthParams.initialAudioParameters = audioParams;
    synthParams.renderSeed = config.randomSeed;

    // The up-sampling anti-alias filter only has to reject the images the
    // linear interpolation leaves above the simulator's own Nyquist. The
    // real-time default parks it at 1900 Hz whatever the simulation frequency,
    // which throws away everything the engine produces above that.
    synthParams.inputAntialiasCutoff =
        (config.inputBandwidth == InputBandwidth::Legacy)
            ? 1900.0f
            : std::max(
                1900.0f,
                std::min(0.45f * inputSampleRate, 0.45f * audioSampleRate));

    simulator->synthesizer().destroy();
    simulator->synthesizer().initialize(synthParams);

    if (!loadImpulseResponses(simulator, output.engine)) {
        if (error != nullptr) *error = "failed to load impulse response WAV(s)";
        return false;
    }

    simulator->startAudioRenderingThread();

    loaded->engine = output.engine;
    loaded->vehicle = vehicle;
    loaded->transmission = transmission;
    loaded->simulator = simulator;
    return true;
}

void destroyLoaded(LoadedEngine &loaded) {
    if (loaded.simulator != nullptr) {
        loaded.simulator->releaseSimulation();
        loaded.simulator->destroy();
        delete loaded.simulator;
        loaded.simulator = nullptr;
    }
    if (loaded.transmission != nullptr) {
        delete loaded.transmission;
        loaded.transmission = nullptr;
    }
    if (loaded.vehicle != nullptr) {
        delete loaded.vehicle;
        loaded.vehicle = nullptr;
    }
    if (loaded.engine != nullptr) {
        loaded.engine->destroy();
        delete loaded.engine;
        loaded.engine = nullptr;
    }
}

bool writeWav(
    const std::string &path,
    std::vector<int16_t> pcm,
    int sampleRate,
    std::string *error)
{
    wav_io::WavData wav;
    wav.sampleRate = sampleRate;
    wav.channelCount = 1;
    wav.bitsPerSample = 16;
    wav.samples = std::move(pcm);
    return wav_io::writePcm16Mono(path, wav, error);
}

// `key` keeps the v1 field name ("off"/"on") so existing consumers still find
// the file names; `Key` suffixes the measurements added in v2.
void writeClipJson(
    std::ostream &out,
    const char *key,
    const char *Key,
    const ClipReport &clip)
{
    out << "      \"" << key << "\": \"" << clip.file << "\",\n";
    out << "      \"measuredRpm" << Key << "\": " << formatDouble(clip.measuredRpm, 1) << ",\n";
    out << "      \"gain" << Key << "\": " << formatDouble(clip.playbackGain, 6) << ",\n";
    out << "      \"peak" << Key << "\": " << formatDouble(clip.peak, 4) << ",\n";
    out << "      \"rms" << Key << "\": " << formatDouble(clip.rms, 4);
}

bool writeManifest(
    const std::string &path,
    const ExportConfig &config,
    const ExportResult &result,
    int idleRpm,
    int redlineRpm)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "{\n";
    out << "  \"id\": \"" << config.engineId << "\",\n";
    out << "  \"format\": \"NEODRIVE\",\n";
    out << "  \"version\": 2,\n";
    out << "  \"sampleRate\": " << config.sampleRate << ",\n";
    out << "  \"clipDuration\": " << formatDouble(config.clipDurationSec, 4) << ",\n";
    out << "  \"loopMode\": \"crossfade\",\n";
    out << "  \"crossfadeMs\": " << config.crossfadeMs << ",\n";
    out << "  \"crossfadeCurve\": \""
        << (config.crossfadeCurve == CrossfadeCurve::Linear ? "linear" : "equal-power")
        << "\",\n";
    out << "  \"spacing\": \""
        << (config.spacing == RpmSpacing::Linear ? "linear" : "geometric") << "\",\n";
    out << "  \"cycleAligned\": " << (config.cycleAlign ? "true" : "false") << ",\n";
    out << "  \"levelMode\": \""
        << (config.levelMode == LevelMode::Shared ? "shared" : "per-layer") << "\",\n";
    out << "  \"idleRpm\": " << idleRpm << ",\n";
    out << "  \"redlineRpm\": " << redlineRpm << ",\n";
    // Every clip went through this one gain, so the peak/rms values below are
    // directly comparable: a runtime can reproduce the engine's real
    // RPM -> loudness curve instead of the flat one an AGC would leave.
    out << "  \"renderGain\": " << formatDouble(result.renderGain, 6) << ",\n";
    out << "  \"layers\": [\n";

    for (size_t i = 0; i < result.layers.size(); ++i) {
        const LayerReport &layer = result.layers[i];
        const double loopSeconds =
            static_cast<double>(layer.loopSamples) / config.sampleRate;

        out << "    {\n";
        out << "      \"rpm\": " << layer.rpm << ",\n";
        out << "      \"cycles\": " << layer.cycles << ",\n";
        out << "      \"loopSamples\": " << layer.loopSamples << ",\n";
        out << "      \"loopSeconds\": " << formatDouble(loopSeconds, 6) << ",\n";
        writeClipJson(out, "off", "Off", layer.off);
        out << ",\n";
        writeClipJson(out, "on", "On", layer.on);
        out << "\n    }";
        out << (i + 1 < result.layers.size() ? ",\n" : "\n");
    }

    out << "  ]\n";
    out << "}\n";
    return true;
}

} /* namespace */

double cycleSeconds(double rpm) {
    if (rpm <= 0.0) return 0.0;
    return 60.0 * kCrankRevsPerCycle / rpm;
}

static int roundRpmToStep(int rpm, int step) {
    if (step <= 1) return rpm;
    return static_cast<int>(std::llround(static_cast<double>(rpm) / step) * step);
}

std::vector<int> buildRpmLadder(
    int idleRpm,
    int redlineRpm,
    int steps,
    RpmSpacing spacing,
    int roundStep)
{
    if (idleRpm <= 0 || redlineRpm <= 0) return {};
    if (steps <= 1) return { roundRpmToStep(idleRpm, roundStep) };

    const bool geometric =
        (spacing == RpmSpacing::Geometric) && idleRpm > 0 && redlineRpm > idleRpm;
    const double ratio = static_cast<double>(redlineRpm) / idleRpm;

    std::vector<int> raw;
    raw.reserve(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) {
        const double t = static_cast<double>(i) / (steps - 1);
        const double value = geometric
            ? idleRpm * std::pow(ratio, t)
            : idleRpm + t * (redlineRpm - idleRpm);
        raw.push_back(roundRpmToStep(static_cast<int>(std::llround(value)), roundStep));
    }

    raw.front() = roundRpmToStep(idleRpm, roundStep);
    raw.back() = roundRpmToStep(redlineRpm, roundStep);

    std::vector<int> out;
    out.reserve(raw.size());
    for (const int rpm : raw) {
        if (out.empty() || out.back() != rpm) out.push_back(rpm);
    }
    return out;
}

int cyclesInLoop(int rpm, double targetSeconds, int minCycles) {
    const double cycle = cycleSeconds(rpm);
    if (cycle <= 0.0) return std::max(1, minCycles);

    const int cycles = static_cast<int>(std::llround(targetSeconds / cycle));
    return std::max(std::max(1, minCycles), cycles);
}

int loopSampleCount(int rpm, int cycles, int sampleRate) {
    const double seconds = cycleSeconds(rpm) * cycles;
    return std::max(1, static_cast<int>(std::llround(seconds * sampleRate)));
}

std::vector<int16_t> crossfadeLoop(
    const std::vector<int16_t> &captured,
    int crossfadeSamples,
    CrossfadeCurve curve)
{
    if (captured.empty()) return captured;

    const int n = static_cast<int>(captured.size());
    const int c = std::min(std::max(0, crossfadeSamples), n / 2);
    if (c == 0) return captured;

    // The trailing c samples are the continuation past the loop point. Blending
    // them into the head and then dropping them is what makes the file loop; the
    // previous implementation kept them, so every wrap replayed that slice twice.
    const int loopLen = n - c;
    std::vector<int16_t> out(captured.begin(), captured.begin() + loopLen);

    for (int i = 0; i < c; ++i) {
        const double t = (c == 1) ? 1.0 : static_cast<double>(i) / (c - 1);
        double wHead = t;
        double wTail = 1.0 - t;
        if (curve == CrossfadeCurve::EqualPower) {
            wHead = std::sqrt(t);
            wTail = std::sqrt(1.0 - t);
        }

        out[static_cast<size_t>(i)] = toPcm(
            static_cast<double>(captured[static_cast<size_t>(i)]) * wHead
            + static_cast<double>(captured[static_cast<size_t>(loopLen + i)]) * wTail);
    }

    return out;
}

bool parseArgs(int argc, char **argv, ExportConfig *config, std::string *usageError) {
    if (config == nullptr) return false;

    static const char *kUsage =
        "engine-sim-cli --engine <.mr> --engine-id <id> --output <dir>\n"
        "  layers   : [--steps 18] [--spacing geometric|linear] [--rpm-round 25]\n"
        "             [--rpms 900,1100,...] [--idle auto|<rpm>] [--redline auto|<rpm>]\n"
        "  loop     : [--clip-duration 1.0] [--min-cycles 6] [--no-cycle-align]\n"
        "             [--crossfade-ms 15] [--crossfade-curve equal-power|linear]\n"
        "  capture  : [--warmup 2.0] [--spin-timeout 12.0] [--rpm-tolerance 0.02]\n"
        "             [--sample-rate 44100] [--seed 42]\n"
        "  level    : [--level per-layer|shared] [--gain auto|<factor>]\n"
        "             [--peak-target 0.65] [--input-bandwidth auto|legacy]\n"
        "  preview  : [--sweep <seconds>]";

    bool argError = false;
    for (int i = 1; i < argc && !argError; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                if (usageError != nullptr) {
                    *usageError = std::string("missing value for ") + name;
                }
                argError = true;
                return {};
            }
            return argv[++i];
        };

        if (arg == "--engine") {
            config->enginePath = needValue("--engine");
        }
        else if (arg == "--engine-id") {
            config->engineId = needValue("--engine-id");
        }
        else if (arg == "--output") {
            config->outputDir = needValue("--output");
        }
        else if (arg == "--assets") {
            config->assetsRoot = needValue("--assets");
        }
        else if (arg == "--es") {
            config->esRoot = needValue("--es");
        }
        else if (arg == "--rpms") {
            const std::string v = needValue("--rpms");
            std::stringstream ss(v);
            std::string token;
            while (std::getline(ss, token, ',')) {
                if (!token.empty()) config->rpms.push_back(std::stoi(token));
            }
        }
        else if (arg == "--steps") {
            config->steps = std::stoi(needValue("--steps"));
        }
        else if (arg == "--spacing") {
            const std::string v = needValue("--spacing");
            if (v == "geometric") config->spacing = RpmSpacing::Geometric;
            else if (v == "linear") config->spacing = RpmSpacing::Linear;
            else if (!argError) {
                if (usageError != nullptr) *usageError = "--spacing must be geometric or linear";
                return false;
            }
        }
        else if (arg == "--rpm-round") {
            config->rpmRoundStep = std::stoi(needValue("--rpm-round"));
        }
        else if (arg == "--idle") {
            const std::string v = needValue("--idle");
            if (v == "auto") config->idleAuto = true;
            else if (!argError) { config->idleAuto = false; config->idleRpm = std::stoi(v); }
        }
        else if (arg == "--redline") {
            const std::string v = needValue("--redline");
            if (v == "auto") config->redlineAuto = true;
            else if (!argError) { config->redlineAuto = false; config->redlineRpm = std::stoi(v); }
        }
        else if (arg == "--clip-duration") {
            config->clipDurationSec = std::stod(needValue("--clip-duration"));
        }
        else if (arg == "--min-cycles") {
            config->minCyclesPerLoop = std::stoi(needValue("--min-cycles"));
        }
        else if (arg == "--no-cycle-align") {
            config->cycleAlign = false;
        }
        else if (arg == "--warmup") {
            config->warmupSec = std::stod(needValue("--warmup"));
        }
        else if (arg == "--spin-timeout") {
            config->spinTimeoutSec = std::stod(needValue("--spin-timeout"));
        }
        else if (arg == "--rpm-tolerance") {
            config->rpmTolerance = std::stod(needValue("--rpm-tolerance"));
        }
        else if (arg == "--sample-rate") {
            config->sampleRate = std::stoi(needValue("--sample-rate"));
        }
        else if (arg == "--loop-mode") {
            const std::string v = needValue("--loop-mode");
            if (!argError && v != "crossfade") {
                if (usageError != nullptr) *usageError = "only crossfade loop-mode supported";
                return false;
            }
        }
        else if (arg == "--crossfade-ms") {
            config->crossfadeMs = std::stoi(needValue("--crossfade-ms"));
        }
        else if (arg == "--crossfade-curve") {
            const std::string v = needValue("--crossfade-curve");
            if (v == "equal-power") config->crossfadeCurve = CrossfadeCurve::EqualPower;
            else if (v == "linear") config->crossfadeCurve = CrossfadeCurve::Linear;
            else if (!argError) {
                if (usageError != nullptr) *usageError = "--crossfade-curve must be equal-power or linear";
                return false;
            }
        }
        else if (arg == "--gain") {
            const std::string v = needValue("--gain");
            if (v == "auto") config->autoGain = true;
            else if (!argError) { config->autoGain = false; config->manualGain = std::stod(v); }
        }
        else if (arg == "--peak-target") {
            config->peakTarget = std::stod(needValue("--peak-target"));
        }
        else if (arg == "--level") {
            const std::string v = needValue("--level");
            if (v == "per-layer") config->levelMode = LevelMode::PerLayer;
            else if (v == "shared") config->levelMode = LevelMode::Shared;
            else if (!argError) {
                if (usageError != nullptr) *usageError = "--level must be per-layer or shared";
                return false;
            }
        }
        else if (arg == "--input-bandwidth") {
            const std::string v = needValue("--input-bandwidth");
            if (v == "auto") config->inputBandwidth = InputBandwidth::Auto;
            else if (v == "legacy") config->inputBandwidth = InputBandwidth::Legacy;
            else if (!argError) {
                if (usageError != nullptr) *usageError = "--input-bandwidth must be auto or legacy";
                return false;
            }
        }
        else if (arg == "--sweep") {
            config->sweepSeconds = std::stod(needValue("--sweep"));
        }
        else if (arg == "--seed") {
            config->randomSeed = static_cast<unsigned int>(std::stoul(needValue("--seed")));
        }
        else if (arg == "--help" || arg == "-h") {
            if (usageError != nullptr) *usageError = kUsage;
            return false;
        }
        else {
            if (usageError != nullptr) *usageError = "unknown argument: " + arg;
            return false;
        }
    }

    if (argError) return false;

    if (config->enginePath.empty() || config->engineId.empty() || config->outputDir.empty()) {
        if (usageError != nullptr) {
            *usageError = std::string("--engine, --engine-id and --output are required\n\n") + kUsage;
        }
        return false;
    }

    if (config->rpms.empty() && config->steps <= 0) config->steps = 18;
    if (config->rpmRoundStep < 1) config->rpmRoundStep = 1;
    if (config->minCyclesPerLoop < 1) config->minCyclesPerLoop = 1;
    if (config->peakTarget <= 0.0 || config->peakTarget > 1.0) config->peakTarget = 0.65;
    if (config->rpmTolerance <= 0.0) config->rpmTolerance = 0.02;

    return true;
}

ExportResult runExport(const ExportConfig &config) {
    ExportResult result;

    if (!ensureDir(config.outputDir)) {
        result.message = "cannot create output directory: " + config.outputDir;
        return result;
    }

    LoadedEngine loaded;
    std::string error;
    if (!loadEngineFromScript(config, &loaded, &error)) {
        result.message = error;
        destroyLoaded(loaded);
        return result;
    }

    const int idleRpm = config.idleAuto
        ? static_cast<int>(units::toRpm(loaded.engine->getDynoMinSpeed()))
        : config.idleRpm;
    const int redlineRpm = config.redlineAuto
        ? static_cast<int>(units::toRpm(loaded.engine->getRedline()))
        : config.redlineRpm;

    if (idleRpm <= 0 || redlineRpm <= idleRpm) {
        result.message = "invalid RPM range (idle " + std::to_string(idleRpm)
            + ", redline " + std::to_string(redlineRpm) + ")";
        destroyLoaded(loaded);
        return result;
    }

    const double throttleByVariant[2] = { 0.01, 1.0 };
    const char *suffixByVariant[2] = { "_off", "_on" };

    // One fixed gain for the whole set, measured at the loudest operating point.
    result.renderGain = config.manualGain;
    if (config.autoGain) {
        if (!measureRenderGain(
                loaded, config, redlineRpm, throttleByVariant, 2,
                &result.renderGain, &error)) {
            result.message = error;
            destroyLoaded(loaded);
            return result;
        }
    }
    setFixedGain(loaded.simulator, result.renderGain);
    std::cout << "render gain " << formatDouble(result.renderGain, 3)
              << " (peak target " << formatDouble(config.peakTarget, 2)
              << " at " << redlineRpm << " rpm)" << std::endl;

    // --- Rev-sweep preview mode: one continuous WAV, no tiers ---
    if (config.sweepSeconds > 0.0) {
        std::vector<int16_t> pcm =
            captureSweep(loaded, config, idleRpm, redlineRpm, &error);
        if (pcm.empty()) {
            result.message = error.empty() ? "sweep capture failed" : error;
            destroyLoaded(loaded);
            return result;
        }

        const std::string outPath =
            joinPath(config.outputDir, config.engineId + "_sweep.wav");
        if (!writeWav(outPath, std::move(pcm), config.sampleRate, &error)) {
            result.message = error;
            destroyLoaded(loaded);
            return result;
        }

        destroyLoaded(loaded);
        result.writtenFiles.push_back(outPath);
        result.success = true;
        result.message = "rendered sweep (" + std::to_string(idleRpm) + "-"
            + std::to_string(redlineRpm) + " rpm)";
        return result;
    }

    std::vector<int> rpms = config.rpms;
    if (rpms.empty()) {
        rpms = buildRpmLadder(
            idleRpm, redlineRpm, config.steps, config.spacing, config.rpmRoundStep);
        if (static_cast<int>(rpms.size()) < config.steps) {
            result.warnings.push_back(
                "rounding to " + std::to_string(config.rpmRoundStep) + " rpm collapsed "
                + std::to_string(config.steps - static_cast<int>(rpms.size()))
                + " of the " + std::to_string(config.steps)
                + " requested layers; lower --rpm-round to keep them");
        }
    }

    if (rpms.empty()) {
        result.message = "empty RPM ladder";
        destroyLoaded(loaded);
        return result;
    }

    const int totalFiles = static_cast<int>(rpms.size()) * 2;
    int doneFiles = 0;

    for (const int rpm : rpms) {
        LayerReport layer;
        layer.rpm = rpm;
        layer.cycles = config.cycleAlign
            ? cyclesInLoop(rpm, config.clipDurationSec, config.minCyclesPerLoop)
            : 0;
        layer.loopSamples = config.cycleAlign
            ? loopSampleCount(rpm, layer.cycles, config.sampleRate)
            : static_cast<int>(std::llround(config.clipDurationSec * config.sampleRate));

        const int crossfadeSamples = std::min(
            layer.loopSamples / 2,
            std::max(0, config.sampleRate * config.crossfadeMs / 1000));

        for (int variant = 0; variant < 2; ++variant) {
            const double throttle = throttleByVariant[variant];
            const std::string fileName = config.engineId + "_"
                + std::to_string(rpm) + suffixByVariant[variant] + ".wav";
            const std::string outPath = joinPath(config.outputDir, fileName);

            ClipCapture capture;
            double gainUsed = result.renderGain;
            if (!captureLeveled(
                    loaded,
                    config,
                    rpm,
                    throttle,
                    layer.loopSamples + crossfadeSamples,
                    result.renderGain,
                    &gainUsed,
                    &capture,
                    &error)) {
                result.message = error;
                destroyLoaded(loaded);
                return result;
            }

            const double deviation = std::abs(capture.measuredRpm - rpm) / rpm;
            if (deviation > config.rpmTolerance) {
                result.message = "layer " + std::to_string(rpm) + suffixByVariant[variant]
                    + " captured at " + formatDouble(capture.measuredRpm, 1)
                    + " rpm (" + formatDouble(deviation * 100.0, 2)
                    + "% off target); raise --spin-timeout or --rpm-tolerance";
                destroyLoaded(loaded);
                return result;
            }

            std::vector<int16_t> pcm = crossfadeLoop(
                capture.pcm, crossfadeSamples, config.crossfadeCurve);

            const Levels levels = measureLevels(pcm);
            ClipReport &report = (variant == 0) ? layer.off : layer.on;
            report.file = fileName;
            report.measuredRpm = capture.measuredRpm;
            report.peak = levels.peak;
            report.rms = levels.rms;
            report.clipped = levels.clipped;
            // Undoing the per-clip gain reproduces exactly what the shared gain
            // would have written, so a player that applies it hears the engine's
            // real loudness curve.
            report.playbackGain =
                (gainUsed > 0.0) ? result.renderGain / gainUsed : 1.0;

            if (levels.clipped) {
                result.warnings.push_back(
                    fileName + " reached full scale; lower --peak-target");
            }

            if (!writeWav(outPath, std::move(pcm), config.sampleRate, &error)) {
                result.message = error;
                destroyLoaded(loaded);
                return result;
            }

            result.writtenFiles.push_back(outPath);
            ++doneFiles;
            std::cout << "PROGRESS " << doneFiles << " " << totalFiles << std::endl;
        }

        result.layers.push_back(layer);
    }

    const std::string manifestPath = joinPath(config.outputDir, "manifest.json");
    if (!writeManifest(manifestPath, config, result, idleRpm, redlineRpm)) {
        result.message = "failed to write manifest.json";
        destroyLoaded(loaded);
        return result;
    }
    result.writtenFiles.push_back(manifestPath);

    destroyLoaded(loaded);
    result.success = true;
    result.message = "exported " + std::to_string(result.layers.size())
        + " layers (" + std::to_string(result.writtenFiles.size()) + " files)";
    return result;
}

std::string sha256HexFile(const std::string &path) {
#ifdef _WIN32
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContext(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return {};
    }
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return {};
    }

    std::ifstream in(path, std::ios::binary);
    char buf[8192];
    while (in) {
        in.read(buf, sizeof(buf));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            CryptHashData(hash, reinterpret_cast<BYTE *>(buf), static_cast<DWORD>(n), 0);
        }
    }

    BYTE digest[32];
    DWORD digestLen = 32;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0)) {
        CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
        return {};
    }

    std::ostringstream oss;
    for (DWORD i = 0; i < digestLen; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }

    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return oss.str();
#else
    return {};
#endif
}

} /* namespace cli_audio */
