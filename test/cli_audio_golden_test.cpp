#include <gtest/gtest.h>

#include "../include/cli_audio_export.h"
#include "../include/wav_io.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr double kPi = 3.14159265358979323846;

// A tone with an exact whole number of periods inside `loopLen`, rendered for
// loopLen + extra samples. Because the period divides the loop length, the
// trailing `extra` samples are sample-for-sample identical to the head -- which
// is exactly the property cycle-aligned engine captures are built to have.
std::vector<int16_t> periodicSignal(int loopLen, int periods, int extra) {
    std::vector<int16_t> out(static_cast<size_t>(loopLen + extra));
    for (size_t i = 0; i < out.size(); ++i) {
        const double phase = 2.0 * kPi * periods * (static_cast<double>(i) / loopLen);
        out[i] = static_cast<int16_t>(std::lround(10000.0 * std::sin(phase)));
    }
    return out;
}

} /* namespace */

TEST(CliAudioLoop, CrossfadeDropsTheBlendedTail) {
    const std::vector<int16_t> src(1000, 0);

    const std::vector<int16_t> out =
        cli_audio::crossfadeLoop(src, 100, cli_audio::CrossfadeCurve::EqualPower);

    // The tail is blended into the head and then removed; keeping it made every
    // wrap replay that slice twice.
    EXPECT_EQ(out.size(), 900u);
}

TEST(CliAudioLoop, CrossfadeIsIdentityWithoutOverlap) {
    const std::vector<int16_t> src(64, 123);

    const std::vector<int16_t> out =
        cli_audio::crossfadeLoop(src, 0, cli_audio::CrossfadeCurve::Linear);

    EXPECT_EQ(out, src);
}

TEST(CliAudioLoop, LinearCurvePreservesPhaseAlignedContent) {
    constexpr int loopLen = 4410;
    constexpr int extra = 441;
    const std::vector<int16_t> src = periodicSignal(loopLen, 20, extra);

    const std::vector<int16_t> out =
        cli_audio::crossfadeLoop(src, extra, cli_audio::CrossfadeCurve::Linear);

    ASSERT_EQ(out.size(), static_cast<size_t>(loopLen));
    for (int i = 0; i < loopLen; ++i) {
        EXPECT_NEAR(out[static_cast<size_t>(i)], src[static_cast<size_t>(i)], 1)
            << "at sample " << i;
    }
}

TEST(CliAudioLoop, EqualPowerCurveIsHalfPowerAtTheMidpoint) {
    // Head is a constant, tail is silence: the weight applied to the head at the
    // centre of the fade is sqrt(0.5) for the equal-power law.
    constexpr int loopLen = 200;
    constexpr int extra = 101;
    std::vector<int16_t> src(loopLen + extra, 0);
    for (int i = 0; i < loopLen; ++i) src[static_cast<size_t>(i)] = 10000;

    const std::vector<int16_t> out =
        cli_audio::crossfadeLoop(src, extra, cli_audio::CrossfadeCurve::EqualPower);

    ASSERT_EQ(out.size(), static_cast<size_t>(loopLen));
    EXPECT_NEAR(out[static_cast<size_t>(extra / 2)], 10000.0 * std::sqrt(0.5), 60.0);
    EXPECT_EQ(out[0], 0);  // fully the (silent) tail at the start of the fade
}

TEST(CliAudioLadder, GeometricSpacingIsConstantInRatio) {
    const std::vector<int> ladder = cli_audio::buildRpmLadder(
        900, 7200, 18, cli_audio::RpmSpacing::Geometric, 25);

    ASSERT_EQ(ladder.size(), 18u);
    EXPECT_EQ(ladder.front(), 900);
    EXPECT_EQ(ladder.back(), 7200);

    const double expected = std::pow(7200.0 / 900.0, 1.0 / 17.0);
    for (size_t i = 1; i < ladder.size(); ++i) {
        const double ratio =
            static_cast<double>(ladder[i]) / ladder[i - 1];
        EXPECT_NEAR(ratio, expected, 0.03) << "between layers " << (i - 1) << " and " << i;
    }
}

TEST(CliAudioLadder, LinearSpacingLeavesAlmostAnOctaveAtTheBottom) {
    // The behaviour the geometric ladder replaces: with a linear ladder the
    // first interval is nearly an octave while the last is a few percent.
    const std::vector<int> ladder = cli_audio::buildRpmLadder(
        900, 7200, 8, cli_audio::RpmSpacing::Linear, 25);

    ASSERT_EQ(ladder.size(), 8u);
    const double firstRatio = static_cast<double>(ladder[1]) / ladder[0];
    const double lastRatio =
        static_cast<double>(ladder[7]) / ladder[6];

    EXPECT_GT(firstRatio, 1.8);
    EXPECT_LT(lastRatio, 1.2);
}

TEST(CliAudioLadder, RoundingCollapsesDuplicates) {
    const std::vector<int> ladder = cli_audio::buildRpmLadder(
        900, 1000, 12, cli_audio::RpmSpacing::Geometric, 100);

    EXPECT_LT(ladder.size(), 12u);
    for (size_t i = 1; i < ladder.size(); ++i) {
        EXPECT_NE(ladder[i], ladder[i - 1]);
    }
}

TEST(CliAudioLoopLength, HoldsAWholeNumberOfEngineCycles) {
    constexpr int sampleRate = 44100;

    for (const int rpm : { 900, 1375, 2350, 4025, 7200 }) {
        const int cycles = cli_audio::cyclesInLoop(rpm, 1.0, 6);
        const int samples = cli_audio::loopSampleCount(rpm, cycles, sampleRate);

        const double loopSeconds = static_cast<double>(samples) / sampleRate;
        const double cycleFraction = loopSeconds / cli_audio::cycleSeconds(rpm);

        // Within half a sample of a whole number of cycles: this is what makes
        // the splice land on the same point of the firing sequence.
        EXPECT_NEAR(cycleFraction, cycles, 0.5 / sampleRate * rpm / 120.0 + 1e-6)
            << "at " << rpm << " rpm";
        EXPECT_GE(cycles, 6) << "at " << rpm << " rpm";
    }
}

TEST(CliAudioLoopLength, FixedSecondsAreNotCycleAligned) {
    // 1.0 s at 900 rpm is 7.5 cycles -- the case the cycle-aligned length fixes.
    const double cycles = 1.0 / cli_audio::cycleSeconds(900);
    EXPECT_NEAR(cycles, 7.5, 1e-9);
    EXPECT_EQ(cli_audio::cyclesInLoop(900, 1.0, 1), 8);
}

TEST(CliAudioGoldenTest, Sha256IsStable) {
    std::vector<int16_t> src(44100, 0);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<int16_t>((i % 200) - 100);
    }

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
