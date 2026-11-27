#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scout {
bool writeWav(const std::wstring& path, const std::vector<float>& stereo, unsigned sampleRate,
              unsigned channels, unsigned bitsPerSample, std::wstring& error);
bool writeWavMono16(const std::wstring& path, const std::vector<float>& mono, unsigned sampleRate,
                    std::wstring& error);
}
