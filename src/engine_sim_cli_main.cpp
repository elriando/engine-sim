#include "../include/cli_audio_export.h"

#include <cmath>
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
    cli_audio::ExportConfig config;
    std::string usageError;

    if (!cli_audio::parseArgs(argc, argv, &config, &usageError)) {
        std::cerr << usageError << std::endl;
        return 1;
    }

    const cli_audio::ExportResult result = cli_audio::runExport(config);

    for (const std::string &warning : result.warnings) {
        std::cerr << "warning: " << warning << std::endl;
    }

    if (!result.success) {
        std::cerr << "export failed: " << result.message << std::endl;
        return 2;
    }

    std::cout << result.message << std::endl;

    if (!result.layers.empty()) {
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  rpm    measured(off/on)  cycles  loop(s)  peak(off/on)  gain(off/on)"
                  << std::endl;
        for (const cli_audio::LayerReport &layer : result.layers) {
            const double loopSeconds =
                static_cast<double>(layer.loopSamples) / config.sampleRate;
            std::cout << "  " << std::setw(5) << layer.rpm
                      << "  " << std::setw(7) << layer.off.measuredRpm
                      << " /" << std::setw(7) << layer.on.measuredRpm
                      << "  " << std::setw(6) << layer.cycles
                      << "  " << std::setw(7) << std::setprecision(3) << loopSeconds
                      << "  " << std::setw(5) << std::setprecision(2) << layer.off.peak
                      << " /" << std::setw(5) << layer.on.peak
                      << "  " << std::setw(6) << std::setprecision(4) << layer.off.playbackGain
                      << " /" << std::setw(6) << layer.on.playbackGain
                      << std::setprecision(1)
                      << std::endl;
        }
    }
    else {
        for (const std::string &file : result.writtenFiles) {
            std::cout << "  " << file << std::endl;
        }
    }

    return 0;
}
