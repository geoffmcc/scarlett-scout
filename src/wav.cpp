#include "wav.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <limits>

namespace scout {
namespace {
void write16(std::ofstream& f, uint16_t v) { f.put(static_cast<char>(v & 0xff)); f.put(static_cast<char>((v >> 8) & 0xff)); }
void write32(std::ofstream& f, uint32_t v) {
    for (int i = 0; i < 4; ++i) f.put(static_cast<char>((v >> (i * 8)) & 0xff));
}
void writeTag(std::ofstream& f, const char* s) { f.write(s, 4); }
}

bool writeWav(const std::wstring& path, const std::vector<float>& samples, unsigned sampleRate,
              unsigned channels, unsigned bitsPerSample, std::wstring& error) {
    if (channels == 0 || channels > 2 || sampleRate == 0 || (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)) {
        error = L"Unsupported WAV format settings.";
        return false;
    }
    const uint64_t sampleCount = samples.size();
    const uint64_t dataBytes64 = sampleCount * bitsPerSample / 8;
    if (dataBytes64 > std::numeric_limits<uint32_t>::max() - 36) {
        error = L"Capture is too large for a standard WAV file.";
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { error = L"Could not create the WAV file."; return false; }
    const bool floatingPoint = bitsPerSample == 32;
    writeTag(f, "RIFF"); write32(f, 36 + dataBytes); writeTag(f, "WAVE");
    writeTag(f, "fmt "); write32(f, 16);
    write16(f, floatingPoint ? 3 : 1); write16(f, static_cast<uint16_t>(channels));
    write32(f, sampleRate); write32(f, sampleRate * channels * bitsPerSample / 8);
    write16(f, static_cast<uint16_t>(channels * bitsPerSample / 8)); write16(f, static_cast<uint16_t>(bitsPerSample));
    writeTag(f, "data"); write32(f, dataBytes);
    for (float sample : samples) {
        const float v = std::clamp(sample, -1.0f, 1.0f);
        if (bitsPerSample == 16) {
            const auto s = static_cast<int16_t>(std::lrint(v * 32767.0f));
            write16(f, static_cast<uint16_t>(s));
        } else if (bitsPerSample == 24) {
            const int32_t s = static_cast<int32_t>(std::lrint(v * 8388607.0f));
            f.put(static_cast<char>(s & 0xff)); f.put(static_cast<char>((s >> 8) & 0xff)); f.put(static_cast<char>((s >> 16) & 0xff));
        } else {
            const uint32_t bits = std::bit_cast<uint32_t>(v);
            write32(f, bits);
        }
    }
    f.flush();
    if (!f.good()) { error = L"An error occurred while writing the WAV file."; return false; }
    return true;
}

bool writeWavMono16(const std::wstring& path, const std::vector<float>& mono, unsigned sampleRate,
                    std::wstring& error) {
    return writeWav(path, mono, sampleRate, 1, 16, error);
}
}
