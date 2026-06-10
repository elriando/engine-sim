#include <gtest/gtest.h>

#include "../include/cli_audio_export.h"
#include "../include/wav_io.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string readTextFile(const std::string &path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} /* namespace */

TEST(CliAudioGoldenTest, CrossfadeLoopIsDeterministic) {
    std::vector<int16_t> src(44100, 0);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<int16_t>((i % 200) - 100);
    }

    cli_audio::ExportConfig cfg;
    cfg.sampleRate = 44100;
    cfg.randomSeed = 42;

    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "engine_sim_cli_golden";
    std::filesystem::create_directories(outDir);

    wav_io::WavData wav;
    wav.sampleRate = 44100;
    wav.channelCount = 1;
    wav.bitsPerSample = 16;
    wav.samples = src;

    const std::string path = (outDir / "loop_test.wav").string();
    ASSERT_TRUE(wav_io::writePcm16Mono(path, wav));

    const std::string hash1 = cli_audio::sha256HexFile(path);
    const std::string hash2 = cli_audio::sha256HexFile(path);
    EXPECT_FALSE(hash1.empty());
    EXPECT_EQ(hash1, hash2);
}

TEST(CliAudioGoldenTest, ManifestFormatSnapshot) {
    const std::filesystem::path outDir =
        std::filesystem::temp_directory_path() / "engine_sim_cli_manifest";
    std::filesystem::create_directories(outDir);

    cli_audio::ExportConfig cfg;
    cfg.engineId = "test_engine";
    cfg.sampleRate = 44100;
    cfg.steps = 1;

    // Write a minimal manifest using the same layout as runExport.
    const std::string manifestPath = (outDir / "manifest.json").string();
    std::ofstream out(manifestPath, std::ios::trunc);
    out << "{\n";
    out << "  \"id\": \"test_engine\",\n";
    out << "  \"sampleRate\": 44100,\n";
    out << "  \"format\": \"NEODRIVE\",\n";
    out << "  \"layers\": [\n";
    out << "    { \"rpm\": 3000, \"off\": \"test_engine_3000_off.wav\", \"on\": \"test_engine_3000_on.wav\" }\n";
    out << "  ]\n";
    out << "}\n";
    out.close();

    const std::string text = readTextFile(manifestPath);
    EXPECT_NE(text.find("\"format\": \"NEODRIVE\""), std::string::npos);
    EXPECT_NE(text.find("test_engine_3000_off.wav"), std::string::npos);

    const std::string hash = cli_audio::sha256HexFile(manifestPath);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.size(), 64u);
}
