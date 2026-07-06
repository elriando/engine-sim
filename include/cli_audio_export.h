#ifndef ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H
#define ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H

#include <string>
#include <vector>

namespace cli_audio {

enum class LoopMode {
    Crossfade
};

struct ExportConfig {
    std::string enginePath;
    std::string engineId;
    std::string outputDir;
    std::string assetsRoot = "assets";
    std::string esRoot = "es";

    std::vector<int> rpms;
    int steps = 0;
    bool idleAuto = true;
    bool redlineAuto = true;
    int idleRpm = 900;
    int redlineRpm = 6500;

    double clipDurationSec = 1.0;
    double warmupSec = 2.0;
    int sampleRate = 44100;
    LoopMode loopMode = LoopMode::Crossfade;
    int crossfadeMs = 15;
    unsigned int randomSeed = 42;

    // When > 0, render a single continuous idle->redline->idle rev sweep of this
    // many seconds to "<id>_sweep.wav" instead of the tiered NEODRIVE set.
    double sweepSeconds = 0.0;
};

struct ExportResult {
    bool success = false;
    std::string message;
    std::vector<std::string> writtenFiles;
};

bool parseArgs(int argc, char **argv, ExportConfig *config, std::string *usageError);
ExportResult runExport(const ExportConfig &config);
std::string sha256HexFile(const std::string &path);

} /* namespace cli_audio */

#endif /* ATG_ENGINE_SIM_CLI_AUDIO_EXPORT_H */
