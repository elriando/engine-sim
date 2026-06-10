#include "../include/wav_io.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace wav_io {

namespace {

bool readU32(std::ifstream &in, uint32_t *v) {
    in.read(reinterpret_cast<char *>(v), 4);
    return static_cast<bool>(in);
}

bool readU16(std::ifstream &in, uint16_t *v) {
    in.read(reinterpret_cast<char *>(v), 2);
    return static_cast<bool>(in);
}

} /* namespace */

bool readPcm16Mono(const std::string &path, WavData *out, std::string *error) {
    if (out == nullptr) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "cannot open wav: " + path;
        return false;
    }

    char riff[4];
    in.read(riff, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0) {
        if (error != nullptr) *error = "not a RIFF file: " + path;
        return false;
    }

    uint32_t riffSize = 0;
    if (!readU32(in, &riffSize)) return false;

    char wave[4];
    in.read(wave, 4);
    if (std::strncmp(wave, "WAVE", 4) != 0) {
        if (error != nullptr) *error = "not a WAVE file: " + path;
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    bool foundFmt = false;
    bool foundData = false;

    while (in && !(foundFmt && foundData)) {
        char chunkId[4];
        in.read(chunkId, 4);
        if (!in) break;

        uint32_t chunkSize = 0;
        if (!readU32(in, &chunkSize)) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            readU16(in, &audioFormat);
            readU16(in, &channels);
            readU32(in, &sampleRate);
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            readU32(in, &byteRate);
            readU16(in, &blockAlign);
            readU16(in, &bitsPerSample);
            if (chunkSize > 16) {
                in.seekg(chunkSize - 16, std::ios::cur);
            }
            foundFmt = true;
        }
        else if (std::strncmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            foundData = true;
            break;
        }
        else {
            in.seekg(chunkSize, std::ios::cur);
        }
    }

    if (!foundFmt || !foundData) {
        if (error != nullptr) *error = "missing fmt/data chunks: " + path;
        return false;
    }

    if (audioFormat != 1 || bitsPerSample != 16) {
        if (error != nullptr) *error = "only PCM16 supported: " + path;
        return false;
    }

    const size_t sampleCount = dataSize / (channels * (bitsPerSample / 8));
    std::vector<int16_t> interleaved(sampleCount * channels);
    in.read(reinterpret_cast<char *>(interleaved.data()), static_cast<std::streamsize>(dataSize));

    out->sampleRate = static_cast<int>(sampleRate);
    out->channelCount = 1;
    out->bitsPerSample = 16;
    out->samples.resize(sampleCount);

    if (channels == 1) {
        out->samples = interleaved;
    }
    else {
        for (size_t i = 0; i < sampleCount; ++i) {
            int32_t sum = 0;
            for (uint16_t c = 0; c < channels; ++c) {
                sum += interleaved[i * channels + c];
            }
            out->samples[i] = static_cast<int16_t>(sum / channels);
        }
    }

    return true;
}

bool writePcm16Mono(const std::string &path, const WavData &data, std::string *error) {
    if (data.bitsPerSample != 16 || data.channelCount != 1) {
        if (error != nullptr) *error = "writePcm16Mono expects mono 16-bit PCM";
        return false;
    }

    const uint32_t dataSize =
        static_cast<uint32_t>(data.samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataSize;
    const uint16_t audioFormat = 1;
    const uint16_t channels = 1;
    const uint32_t sampleRate = static_cast<uint32_t>(data.sampleRate);
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const uint32_t byteRate = sampleRate * blockAlign;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "cannot write wav: " + path;
        return false;
    }

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char *>(&riffSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    out.write(reinterpret_cast<const char *>(&fmtSize), 4);
    out.write(reinterpret_cast<const char *>(&audioFormat), 2);
    out.write(reinterpret_cast<const char *>(&channels), 2);
    out.write(reinterpret_cast<const char *>(&sampleRate), 4);
    out.write(reinterpret_cast<const char *>(&byteRate), 4);
    out.write(reinterpret_cast<const char *>(&blockAlign), 2);
    out.write(reinterpret_cast<const char *>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char *>(&dataSize), 4);
    out.write(
        reinterpret_cast<const char *>(data.samples.data()),
        static_cast<std::streamsize>(dataSize));

    return static_cast<bool>(out);
}

} /* namespace wav_io */
