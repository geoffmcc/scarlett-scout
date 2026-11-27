#include "wav.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint16_t u16(const std::vector<unsigned char>& b, size_t p) {
    return static_cast<uint16_t>(b[p] | (static_cast<uint16_t>(b[p + 1]) << 8));
}
static uint32_t u32(const std::vector<unsigned char>& b, size_t p) {
    return static_cast<uint32_t>(b[p]) | (static_cast<uint32_t>(b[p + 1]) << 8) |
           (static_cast<uint32_t>(b[p + 2]) << 16) | (static_cast<uint32_t>(b[p + 3]) << 24);
}
static bool check(unsigned bits, unsigned channels, uint16_t expectedFormat) {
    const fs::path file = fs::temp_directory_path() / ("scarlett-scout-wav-" + std::to_string(bits) + "-" + std::to_string(channels) + ".wav");
    const size_t frames = 17;
    std::vector<float> samples(frames * channels, 0.25f);
    std::wstring error;
    if (!scout::writeWav(file.wstring(), samples, 48000, channels, bits, error)) {
        std::cerr << "WAV write failed: " << std::string(error.begin(), error.end()) << "\n";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
    const uint32_t expectedData = static_cast<uint32_t>(frames * channels * (bits / 8));
    const bool ok = bytes.size() == 44u + expectedData &&
        std::string(bytes.begin(), bytes.begin() + 4) == "RIFF" &&
        std::string(bytes.begin() + 8, bytes.begin() + 12) == "WAVE" &&
        u16(bytes, 20) == expectedFormat && u16(bytes, 22) == channels &&
        u32(bytes, 24) == 48000 && u16(bytes, 34) == bits && u32(bytes, 40) == expectedData;
    fs::remove(file);
    if (!ok) std::cerr << "WAV header/data validation failed for " << bits << "-bit, " << channels << " channel(s)\n";
    return ok;
}
int main() {
    if (!check(16, 2, 1)) return 1;
    if (!check(24, 1, 1)) return 2;
    if (!check(32, 2, 3)) return 3;
    std::cout << "WAV regression tests passed: stereo PCM16, mono PCM24, stereo IEEE-float32.\n";
    return 0;
}
