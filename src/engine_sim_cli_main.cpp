#include "../include/cli_audio_export.h"

#include <iostream>

int main(int argc, char **argv) {
    cli_audio::ExportConfig config;
    std::string usageError;

    if (!cli_audio::parseArgs(argc, argv, &config, &usageError)) {
        std::cerr << usageError << std::endl;
        return 1;
    }

    const cli_audio::ExportResult result = cli_audio::runExport(config);
    if (!result.success) {
        std::cerr << "export failed: " << result.message << std::endl;
        return 2;
    }

    std::cout << result.message << std::endl;
    for (const std::string &file : result.writtenFiles) {
        std::cout << "  " << file << std::endl;
    }

    return 0;
}
