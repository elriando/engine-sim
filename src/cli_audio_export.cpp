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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace cli_audio {

namespace {

struct LoadedEngine {
    Engine *engine = nullptr;
    Vehicle *vehicle = nullptr;
    Transmission *transmission = nullptr;
    Simulator *simulator = nullptr;
};

std::string joinPath(const std::string &a, const std::string &b) {
    return (std::filesystem::path(a) / b).string();
}

bool ensureDir(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

std::vector<int16_t> applyCrossfadeLoop(
    const std::vector<int16_t> &src,
    int sampleRate,
    int crossfadeMs)
{
    if (src.empty()) return src;

    std::vector<int16_t> out = src;
    const int crossfadeSamples = std::max(
        1,
        std::min(static_cast<int>(src.size() / 4), sampleRate * crossfadeMs / 1000));

    for (int i = 0; i < crossfadeSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(crossfadeSamples);
        const float head = static_cast<float>(src[static_cast<size_t>(i)]);
        const float tail = static_cast<float>(src[src.size() - static_cast<size_t>(crossfadeSamples) + i]);
        out[static_cast<size_t>(i)] =
            static_cast<int16_t>(head * t + tail * (1.0f - t));
    }

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

bool spinToRpm(
    LoadedEngine &loaded,
    double targetRpm,
    double throttleSpeedControl,
    double maxSeconds,
    int sampleRate)
{
    Engine *engine = loaded.engine;
    Simulator *sim = loaded.simulator;

    sim->m_dyno.m_enabled = true;
    sim->m_dyno.m_hold = true;
    sim->m_dyno.m_rotationSpeed = units::rpm(targetRpm);
    engine->getIgnitionModule()->m_enabled = true;
    sim->m_starterMotor.m_enabled = true;

    const double frameDt = 1.0 / 60.0;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(static_cast<int>(maxSeconds * 1000.0));

    engine->setSpeedControl(throttleSpeedControl);

    while (std::chrono::steady_clock::now() < deadline) {
        sim->startFrame(frameDt);
        while (sim->simulateStep()) {
            engine->setSpeedControl(throttleSpeedControl);
        }
        sim->endFrame();

        int16_t scratch[4096];
        sim->readAudioOutput(4096, scratch);

        const double rpm = units::toRpm(engine->getSpeed());
        if (rpm >= targetRpm * 0.98) {
            sim->m_starterMotor.m_enabled = false;
            return true;
        }
    }

    sim->m_starterMotor.m_enabled = false;
    return units::toRpm(engine->getSpeed()) >= targetRpm * 0.90;
}

std::vector<int16_t> capturePcm(
    LoadedEngine &loaded,
    double targetRpm,
    double throttleSpeedControl,
    double warmupSec,
    double clipSec,
    int sampleRate)
{
    Engine *engine = loaded.engine;
    Simulator *sim = loaded.simulator;

    spinToRpm(loaded, targetRpm, throttleSpeedControl, warmupSec + 2.0, sampleRate);

    const double frameDt = 1.0 / 60.0;
    const int totalSamples = static_cast<int>(clipSec * sampleRate);
    std::vector<int16_t> captured;
    captured.reserve(static_cast<size_t>(totalSamples));

    double elapsedWarmup = 0.0;
    while (elapsedWarmup < warmupSec) {
        sim->startFrame(frameDt);
        while (sim->simulateStep()) {
            engine->setSpeedControl(throttleSpeedControl);
        }
        sim->endFrame();
        int16_t scratch[8192];
        sim->readAudioOutput(8192, scratch);
        elapsedWarmup += frameDt;
    }

    while (static_cast<int>(captured.size()) < totalSamples) {
        sim->startFrame(frameDt);
        while (sim->simulateStep()) {
            engine->setSpeedControl(throttleSpeedControl);
        }
        sim->endFrame();

        int16_t block[8192];
        const int got = sim->readAudioOutput(
            std::min(8192, totalSamples - static_cast<int>(captured.size())),
            block);
        captured.insert(captured.end(), block, block + got);
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
        if (error != nullptr) *error = "compile failed — see error_log.log";
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
    simulator->setSimulationFrequency(output.engine->getSimulationFrequency());
    simulator->setTargetSynthesizerLatency(0.05);

    Synthesizer::AudioParameters audioParams = simulator->synthesizer().getAudioParameters();
    audioParams.inputSampleNoise = static_cast<float>(output.engine->getInitialJitter());
    audioParams.airNoise = static_cast<float>(output.engine->getInitialNoise());
    audioParams.dF_F_mix = static_cast<float>(output.engine->getInitialHighFrequencyGain());
    simulator->synthesizer().setAudioParameters(audioParams);

    Synthesizer::Parameters synthParams;
    synthParams.audioBufferSize = 44100 * 2;
    synthParams.audioSampleRate = 44100.0f;
    synthParams.inputBufferSize = 44100;
    synthParams.inputChannelCount = output.engine->getExhaustSystemCount();
    synthParams.inputSampleRate = static_cast<float>(output.engine->getSimulationFrequency());
    synthParams.initialAudioParameters = audioParams;
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

int roundRpmToStep(int rpm, int step) {
    if (step <= 1) return rpm;
    return static_cast<int>(std::llround(static_cast<double>(rpm) / step) * step);
}

std::vector<int> dedupeAdjacentRpms(const std::vector<int> &rpms) {
    std::vector<int> out;
    out.reserve(rpms.size());
    for (const int rpm : rpms) {
        if (out.empty() || out.back() != rpm) {
            out.push_back(rpm);
        }
    }
    return out;
}

std::vector<int> resolveRpms(const ExportConfig &config, Engine *engine) {
    int idle = config.idleAuto
        ? static_cast<int>(units::toRpm(engine->getDynoMinSpeed()))
        : config.idleRpm;
    int redline = config.redlineAuto
        ? static_cast<int>(units::toRpm(engine->getRedline()))
        : config.redlineRpm;

    if (!config.rpms.empty()) {
        return config.rpms;
    }

    constexpr int kRpmRoundStep = 100;
    const int idleRounded = roundRpmToStep(idle, kRpmRoundStep);
    const int redlineRounded = roundRpmToStep(redline, kRpmRoundStep);

    const int steps = std::max(1, config.steps);
    std::vector<int> rpms;
    rpms.reserve(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) {
        const double t = (steps == 1) ? 0.0 : static_cast<double>(i) / (steps - 1);
        const int raw = static_cast<int>(idle + t * (redline - idle));
        rpms.push_back(roundRpmToStep(raw, kRpmRoundStep));
    }

    if (!rpms.empty()) {
        rpms.front() = idleRounded;
        rpms.back() = redlineRounded;
    }

    return dedupeAdjacentRpms(rpms);
}

bool writeManifest(
    const std::string &path,
    const ExportConfig &config,
    const std::vector<int> &rpms)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "{\n";
    out << "  \"id\": \"" << config.engineId << "\",\n";
    out << "  \"sampleRate\": " << config.sampleRate << ",\n";
    out << "  \"clipDuration\": " << config.clipDurationSec << ",\n";
    out << "  \"loopMode\": \"crossfade\",\n";
    out << "  \"format\": \"NEODRIVE\",\n";
    out << "  \"layers\": [\n";
    for (size_t i = 0; i < rpms.size(); ++i) {
        const int rpm = rpms[i];
        out << "    { \"rpm\": " << rpm
            << ", \"off\": \"" << config.engineId << "_" << rpm << "_off.wav\""
            << ", \"on\": \"" << config.engineId << "_" << rpm << "_on.wav\" }";
        out << (i + 1 < rpms.size() ? ",\n" : "\n");
    }
    out << "  ]\n";
    out << "}\n";
    return true;
}

} /* namespace */

bool parseArgs(int argc, char **argv, ExportConfig *config, std::string *usageError) {
    if (config == nullptr) return false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                if (usageError != nullptr) {
                    *usageError = std::string("missing value for ") + name;
                }
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
        else if (arg == "--idle") {
            const std::string v = needValue("--idle");
            if (v == "auto") config->idleAuto = true;
            else { config->idleAuto = false; config->idleRpm = std::stoi(v); }
        }
        else if (arg == "--redline") {
            const std::string v = needValue("--redline");
            if (v == "auto") config->redlineAuto = true;
            else { config->redlineAuto = false; config->redlineRpm = std::stoi(v); }
        }
        else if (arg == "--clip-duration") {
            config->clipDurationSec = std::stod(needValue("--clip-duration"));
        }
        else if (arg == "--warmup") {
            config->warmupSec = std::stod(needValue("--warmup"));
        }
        else if (arg == "--sample-rate") {
            config->sampleRate = std::stoi(needValue("--sample-rate"));
        }
        else if (arg == "--loop-mode") {
            const std::string v = needValue("--loop-mode");
            if (v != "crossfade") {
                if (usageError != nullptr) *usageError = "only crossfade loop-mode supported";
                return false;
            }
        }
        else if (arg == "--crossfade-ms") {
            config->crossfadeMs = std::stoi(needValue("--crossfade-ms"));
        }
        else if (arg == "--seed") {
            config->randomSeed = static_cast<unsigned int>(std::stoul(needValue("--seed")));
        }
        else if (arg == "--help" || arg == "-h") {
            if (usageError != nullptr) {
                *usageError =
                    "engine-sim-cli --engine <.mr> --engine-id <id> --output <dir> "
                    "[--rpms 900,2500,... | --steps 8] [--idle auto|<rpm>] [--redline auto|<rpm>] "
                    "--clip-duration 1.0 --warmup 2.0 --sample-rate 44100 --loop-mode crossfade";
            }
            return false;
        }
        else {
            if (usageError != nullptr) *usageError = "unknown argument: " + arg;
            return false;
        }
    }

    if (config->enginePath.empty() || config->engineId.empty() || config->outputDir.empty()) {
        if (usageError != nullptr) {
            *usageError = "--engine, --engine-id and --output are required";
        }
        return false;
    }

    if (config->rpms.empty() && config->steps <= 0) {
        config->steps = 8;
    }

    return true;
}

ExportResult runExport(const ExportConfig &config) {
    ExportResult result;
    srand(config.randomSeed);

    if (!ensureDir(config.outputDir)) {
        result.message = "cannot create output directory: " + config.outputDir;
        return result;
    }

    LoadedEngine loaded;
    std::string loadError;
    if (!loadEngineFromScript(config, &loaded, &loadError)) {
        result.message = loadError;
        destroyLoaded(loaded);
        return result;
    }

    const std::vector<int> rpms = resolveRpms(config, loaded.engine);
    const double throttleOff = 0.01;
    const double throttleOn = 1.0;

    for (const int rpm : rpms) {
        for (int variant = 0; variant < 2; ++variant) {
            const bool isOn = variant == 1;
            const double throttle = isOn ? throttleOn : throttleOff;
            const std::string suffix = isOn ? "_on" : "_off";
            const std::string fileName =
                config.engineId + "_" + std::to_string(rpm) + suffix + ".wav";
            const std::string outPath = joinPath(config.outputDir, fileName);

            std::vector<int16_t> pcm = capturePcm(
                loaded,
                static_cast<double>(rpm),
                throttle,
                config.warmupSec,
                config.clipDurationSec,
                config.sampleRate);

            pcm = applyCrossfadeLoop(pcm, config.sampleRate, config.crossfadeMs);

            wav_io::WavData wav;
            wav.sampleRate = config.sampleRate;
            wav.channelCount = 1;
            wav.bitsPerSample = 16;
            wav.samples = std::move(pcm);

            std::string wavError;
            if (!wav_io::writePcm16Mono(outPath, wav, &wavError)) {
                result.message = wavError;
                destroyLoaded(loaded);
                return result;
            }

            result.writtenFiles.push_back(outPath);
        }
    }

    const std::string manifestPath = joinPath(config.outputDir, "manifest.json");
    if (!writeManifest(manifestPath, config, rpms)) {
        result.message = "failed to write manifest.json";
        destroyLoaded(loaded);
        return result;
    }
    result.writtenFiles.push_back(manifestPath);

    destroyLoaded(loaded);
    result.success = true;
    result.message = "exported " + std::to_string(result.writtenFiles.size()) + " files";
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
