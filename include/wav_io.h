#ifndef ATG_ENGINE_SIM_WAV_IO_H
#define ATG_ENGINE_SIM_WAV_IO_H

#include <cstdint>
#include <string>
#include <vector>

namespace wav_io {

struct WavData {
    int sampleRate = 44100;
    int channelCount = 1;
    int bitsPerSample = 16;
    std::vector<int16_t> samples;
};

bool readPcm16Mono(const std::string &path, WavData *out, std::string *error = nullptr);
bool writePcm16Mono(const std::string &path, const WavData &data, std::string *error = nullptr);

} /* namespace wav_io */

#endif /* ATG_ENGINE_SIM_WAV_IO_H */
