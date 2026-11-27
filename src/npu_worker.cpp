#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "version.hpp"
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// Minimal OpenVINO C API declarations. The app loads openvino_c.dll dynamically,
// avoiding any MSVC/MinGW C++ ABI dependency at link time. These signatures and
// layouts match the upstream OpenVINO Runtime C API.
struct ov_core;
struct ov_model;
struct ov_compiled_model;
struct ov_infer_request;
struct ov_tensor;
using ov_core_t = ov_core;
using ov_model_t = ov_model;
using ov_compiled_model_t = ov_compiled_model;
using ov_infer_request_t = ov_infer_request;
using ov_tensor_t = ov_tensor;
using ov_status_e = int;
using ov_element_type_e = int;
struct ov_available_devices_t { char** devices; size_t size; };
struct ov_dimension_t { int64_t min; int64_t max; };
using ov_rank_t = ov_dimension_t;
struct ov_partial_shape_t { ov_rank_t rank; ov_dimension_t* dims; };
static_assert(sizeof(void*) == 8, "Scarlett Scout NPU worker is x64-only");
static_assert(sizeof(ov_available_devices_t) == 16, "Unexpected OpenVINO available-device ABI layout");
static_assert(sizeof(ov_dimension_t) == 16, "Unexpected OpenVINO dimension ABI layout");
static_assert(sizeof(ov_partial_shape_t) == 24, "Unexpected OpenVINO partial-shape ABI layout");

namespace ovapi {
using CoreCreate = ov_status_e (__cdecl*)(ov_core_t**);
using CoreCreateWithConfigUnicode = ov_status_e (__cdecl*)(const wchar_t*, ov_core_t**);
using CoreFree = void (__cdecl*)(ov_core_t*);
using CoreReadModelUnicode = ov_status_e (__cdecl*)(const ov_core_t*, const wchar_t*, const wchar_t*, ov_model_t**);
using CoreReadModel = ov_status_e (__cdecl*)(const ov_core_t*, const char*, const char*, ov_model_t**);
using CoreGetAvailableDevices = ov_status_e (__cdecl*)(const ov_core_t*, ov_available_devices_t*);
using AvailableDevicesFree = void (__cdecl*)(ov_available_devices_t*);
using ModelInputsSize = ov_status_e (__cdecl*)(const ov_model_t*, size_t*);
using ModelReshapeByPortIndexes = ov_status_e (__cdecl*)(const ov_model_t*, const size_t*, const ov_partial_shape_t*, size_t);
using PartialShapeCreateStatic = ov_status_e (__cdecl*)(int64_t, const int64_t*, ov_partial_shape_t*);
using PartialShapeFree = void (__cdecl*)(ov_partial_shape_t*);
using CoreCompileModel = ov_status_e (__cdecl*)(const ov_core_t*, const ov_model_t*, const char*, size_t, ov_compiled_model_t**, ...);
using ModelFree = void (__cdecl*)(ov_model_t*);
using CompiledModelCreateInferRequest = ov_status_e (__cdecl*)(const ov_compiled_model_t*, ov_infer_request_t**);
using CompiledModelFree = void (__cdecl*)(ov_compiled_model_t*);
using InferRequestGetInputTensorByIndex = ov_status_e (__cdecl*)(const ov_infer_request_t*, size_t, ov_tensor_t**);
using InferRequestGetOutputTensorByIndex = ov_status_e (__cdecl*)(const ov_infer_request_t*, size_t, ov_tensor_t**);
using InferRequestInfer = ov_status_e (__cdecl*)(ov_infer_request_t*);
using InferRequestFree = void (__cdecl*)(ov_infer_request_t*);
using TensorData = ov_status_e (__cdecl*)(const ov_tensor_t*, void**);
using TensorGetSize = ov_status_e (__cdecl*)(const ov_tensor_t*, size_t*);
using TensorGetElementType = ov_status_e (__cdecl*)(const ov_tensor_t*, ov_element_type_e*);
using TensorFree = void (__cdecl*)(ov_tensor_t*);
using GetLastErrorMessage = const char* (__cdecl*)();

struct Api {
    HMODULE dll = nullptr;
    CoreCreate coreCreate = nullptr;
    CoreCreateWithConfigUnicode coreCreateWithConfigUnicode = nullptr;
    CoreFree coreFree = nullptr;
    CoreReadModelUnicode coreReadModelUnicode = nullptr;
    CoreReadModel coreReadModel = nullptr;
    CoreGetAvailableDevices coreGetAvailableDevices = nullptr;
    AvailableDevicesFree availableDevicesFree = nullptr;
    ModelInputsSize modelInputsSize = nullptr;
    ModelReshapeByPortIndexes modelReshapeByPortIndexes = nullptr;
    PartialShapeCreateStatic partialShapeCreateStatic = nullptr;
    PartialShapeFree partialShapeFree = nullptr;
    CoreCompileModel coreCompileModel = nullptr;
    ModelFree modelFree = nullptr;
    CompiledModelCreateInferRequest compiledModelCreateInferRequest = nullptr;
    CompiledModelFree compiledModelFree = nullptr;
    InferRequestGetInputTensorByIndex inferRequestGetInputTensorByIndex = nullptr;
    InferRequestGetOutputTensorByIndex inferRequestGetOutputTensorByIndex = nullptr;
    InferRequestInfer inferRequestInfer = nullptr;
    InferRequestFree inferRequestFree = nullptr;
    TensorData tensorData = nullptr;
    TensorGetSize tensorGetSize = nullptr;
    TensorGetElementType tensorGetElementType = nullptr;
    TensorFree tensorFree = nullptr;
    GetLastErrorMessage getLastErrorMessage = nullptr;

    template <typename T> bool bind(T& target, const char* name, std::string& error) {
        const FARPROC symbol = GetProcAddress(dll, name);
        if (!symbol) { error = std::string("OpenVINO C API is missing ") + name; return false; }
        // Windows specifies GetProcAddress as returning FARPROC. Copy its pointer
        // representation into the exact declared API function-pointer type. This
        // avoids incompatible-function-type casts while preserving the Win32 ABI.
        static_assert(sizeof(T) == sizeof(FARPROC), "Unexpected Windows function pointer size");
        std::memcpy(&target, &symbol, sizeof(target));
        return true;
    }

    template <typename T> void bindOptional(T& target, const char* name) {
        const FARPROC symbol = GetProcAddress(dll, name);
        static_assert(sizeof(T) == sizeof(FARPROC), "Unexpected Windows function pointer size");
        std::memcpy(&target, &symbol, sizeof(target));
    }

    bool load(const fs::path& runtimeDir, std::string& error) {
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        AddDllDirectory(runtimeDir.c_str());
        const fs::path tbb = runtimeDir.parent_path().parent_path().parent_path() / L"3rdparty" / L"tbb" / L"bin";
        if (fs::exists(tbb)) AddDllDirectory(tbb.c_str());
        SetDllDirectoryW(runtimeDir.c_str());
        const fs::path dllPath = runtimeDir / L"openvino_c.dll";
        dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            error = "Could not load openvino_c.dll (Windows error " + std::to_string(GetLastError()) + "). Runtime path: " + dllPath.string();
            return false;
        }
#define BIND(member, symbol) if (!bind(member, symbol, error)) return false
        BIND(coreCreate, "ov_core_create");
        BIND(coreFree, "ov_core_free");
        BIND(coreReadModel, "ov_core_read_model");
        BIND(coreGetAvailableDevices, "ov_core_get_available_devices");
        BIND(availableDevicesFree, "ov_available_devices_free");
        BIND(modelInputsSize, "ov_model_inputs_size");
        BIND(modelReshapeByPortIndexes, "ov_model_reshape_by_port_indexes");
        BIND(partialShapeCreateStatic, "ov_partial_shape_create_static");
        BIND(partialShapeFree, "ov_partial_shape_free");
        BIND(coreCompileModel, "ov_core_compile_model");
        BIND(modelFree, "ov_model_free");
        BIND(compiledModelCreateInferRequest, "ov_compiled_model_create_infer_request");
        BIND(compiledModelFree, "ov_compiled_model_free");
        BIND(inferRequestGetInputTensorByIndex, "ov_infer_request_get_input_tensor_by_index");
        BIND(inferRequestGetOutputTensorByIndex, "ov_infer_request_get_output_tensor_by_index");
        BIND(inferRequestInfer, "ov_infer_request_infer");
        BIND(inferRequestFree, "ov_infer_request_free");
        BIND(tensorData, "ov_tensor_data");
        BIND(tensorGetSize, "ov_tensor_get_size");
        BIND(tensorGetElementType, "ov_tensor_get_element_type");
        BIND(tensorFree, "ov_tensor_free");
        bindOptional(getLastErrorMessage, "ov_get_last_err_msg");
        bindOptional(coreCreateWithConfigUnicode, "ov_core_create_with_config_unicode");
        bindOptional(coreReadModelUnicode, "ov_core_read_model_unicode");
#undef BIND
        return true;
    }

    std::string lastError(const char* fallback) const {
        if (getLastErrorMessage) {
            const char* message = getLastErrorMessage();
            if (message && *message) return message;
        }
        return fallback;
    }

    ~Api() { if (dll) FreeLibrary(dll); }
};
} // namespace ovapi

namespace {
constexpr int64_t kModelSamples = 160000; // 10 seconds at 16 kHz
constexpr int kOvOk = 0;
constexpr int kOvF16 = 3;
constexpr int kOvF32 = 4;
constexpr int kOvI32 = 9;
constexpr int kOvI64 = 10;
constexpr int kOvU8 = 16;
constexpr int kOvBoolean = 1;

std::wstring getArg(const std::map<std::wstring, std::wstring>& args, const std::wstring& key) {
    const auto it = args.find(key);
    return it == args.end() ? L"" : it->second;
}

std::map<std::wstring, std::wstring> parseArgs(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> out;
    for (int i = 1; i + 1 < argc; i += 2) out[argv[i]] = argv[i + 1];
    return out;
}

void writeAtomicLine(const fs::path& path, const std::string& line) {
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << line << '\n';
        f.flush();
        if (!f) return;
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::string sanitise(std::string s) {
    for (char& c : s) if (c == '\r' || c == '\n' || c == '|') c = ' ';
    if (s.size() > 450) s.resize(450);
    return s;
}

uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool readMonoPcm16Wav(const fs::path& path, std::vector<float>& audio, uint32_t& sampleRate, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "Cannot open capture WAV"; return false; }
    unsigned char riff[12]{};
    f.read(reinterpret_cast<char*>(riff), sizeof(riff));
    if (f.gcount() != sizeof(riff) || std::string(reinterpret_cast<char*>(riff), 4) != "RIFF" ||
        std::string(reinterpret_cast<char*>(riff + 8), 4) != "WAVE") {
        error = "Invalid WAV header"; return false;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    sampleRate = 0;
    std::vector<unsigned char> pcm;
    while (f && (!format || pcm.empty())) {
        unsigned char chunk[8]{};
        f.read(reinterpret_cast<char*>(chunk), sizeof(chunk));
        if (f.gcount() != sizeof(chunk)) break;
        const uint32_t size = readU32(chunk + 4);
        const std::string id(reinterpret_cast<char*>(chunk), 4);
        if (id == "fmt ") {
            std::vector<unsigned char> fmt(size);
            f.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(fmt.size()));
            if (fmt.size() < 16 || f.gcount() != static_cast<std::streamsize>(fmt.size())) { error = "Invalid WAV fmt chunk"; return false; }
            format = static_cast<uint16_t>(fmt[0] | (fmt[1] << 8));
            channels = static_cast<uint16_t>(fmt[2] | (fmt[3] << 8));
            sampleRate = readU32(fmt.data() + 4);
            bits = static_cast<uint16_t>(fmt[14] | (fmt[15] << 8));
        } else if (id == "data") {
            pcm.resize(size);
            f.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
            if (f.gcount() != static_cast<std::streamsize>(pcm.size())) { error = "Truncated WAV data"; return false; }
        } else {
            f.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        }
        if ((size & 1u) != 0u) f.seekg(1, std::ios::cur);
    }
    if (format != 1 || channels != 1 || bits != 16 || sampleRate == 0 || pcm.empty()) {
        error = "NPU queue expects mono PCM16 WAV captures"; return false;
    }
    audio.resize(pcm.size() / 2);
    for (size_t i = 0; i < audio.size(); ++i) {
        const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(pcm[i * 2]) |
                              (static_cast<uint16_t>(pcm[i * 2 + 1]) << 8));
        audio[i] = static_cast<float>(sample) / 32768.0f;
    }
    return true;
}

float sinc(float x) { return std::abs(x) < 1.0e-8f ? 1.0f : std::sin(3.14159265358979323846f * x) / (3.14159265358979323846f * x); }

std::vector<float> resample48kTo16k(const std::vector<float>& input) {
    if (input.empty()) return {};
    if (input.size() < 64) {
        std::vector<float> out;
        out.reserve((input.size() + 2) / 3);
        for (size_t i = 0; i < input.size(); i += 3) out.push_back(input[i]);
        return out;
    }
    constexpr int taps = 63;
    constexpr int half = taps / 2;
    constexpr float cutoff = 1.0f / 3.0f;
    std::array<float, taps> kernel{};
    float sum = 0.0f;
    for (int k = -half; k <= half; ++k) {
        const float window = 0.54f + 0.46f * std::cos(3.14159265358979323846f * static_cast<float>(k) / static_cast<float>(half));
        const float value = cutoff * sinc(cutoff * static_cast<float>(k)) * window;
        kernel[static_cast<size_t>(k + half)] = value;
        sum += value;
    }
    for (float& v : kernel) v /= sum;
    std::vector<float> out;
    out.reserve((input.size() + 2) / 3);
    for (size_t center = 0; center < input.size(); center += 3) {
        float value = 0.0f;
        for (int k = -half; k <= half; ++k) {
            const int64_t index = static_cast<int64_t>(center) + k;
            if (index >= 0 && index < static_cast<int64_t>(input.size()))
                value += input[static_cast<size_t>(index)] * kernel[static_cast<size_t>(k + half)];
        }
        out.push_back(value);
    }
    return out;
}

uint16_t floatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
    }
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | ((mantissa + 0x1000u) >> 13));
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    int32_t exp = static_cast<int32_t>((h >> 10) & 0x1fu);
    uint32_t mant = h & 0x03ffu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {
            exp = 1;
            while ((mant & 0x0400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x03ffu;
            bits = sign | (static_cast<uint32_t>(exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | (static_cast<uint32_t>(exp + 127 - 15) << 23) | (mant << 13);
    }
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::map<int, std::string> readLabels(const fs::path& configPath) {
    std::map<int, std::string> labels;
    std::ifstream f(configPath, std::ios::binary);
    if (!f) return labels;
    const std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t begin = json.find("\"id2label\"");
    if (begin == std::string::npos) return labels;
    const size_t open = json.find('{', begin);
    const size_t close = open == std::string::npos ? std::string::npos : json.find('}', open);
    if (close == std::string::npos) return labels;
    const std::string section = json.substr(open, close - open + 1);
    const std::regex entry(R"re("([0-9]+)"\s*:\s*"([^"]+)")re");
    for (std::sregex_iterator it(section.begin(), section.end(), entry), end; it != end; ++it)
        labels[std::stoi((*it)[1].str())] = (*it)[2].str();
    return labels;
}

bool inferGenre(ovapi::Api& api, ov_infer_request_t* request, size_t modelInputCount, const std::vector<float>& audio,
                const std::map<int, std::string>& labels, std::string& result, std::string& error) {
    if (audio.size() != static_cast<size_t>(kModelSamples)) { error = "Prepared audio is not exactly 10 seconds at 16 kHz"; return false; }
    // The source model is constrained to one audio input and, optionally, one
    // attention-mask input during initialization.
    for (size_t i = 0; i < modelInputCount; ++i) {
        ov_tensor_t* tensor = nullptr;
        const ov_status_e getStatus = api.inferRequestGetInputTensorByIndex(request, i, &tensor);
        if (getStatus != kOvOk || !tensor) {
            error = "Could not get model input tensor " + std::to_string(i) + ": " + api.lastError("OpenVINO input tensor error");
            return false;
        }
        size_t count = 0;
        ov_element_type_e type = 0;
        void* data = nullptr;
        const bool ok = api.tensorGetSize(tensor, &count) == kOvOk &&
                         api.tensorGetElementType(tensor, &type) == kOvOk &&
                         api.tensorData(tensor, &data) == kOvOk && data && count > 0;
        if (!ok) { api.tensorFree(tensor); error = "Could not access OpenVINO model input tensor"; return false; }
        if (i == 0) {
            if (count != audio.size()) { api.tensorFree(tensor); error = "Model audio input shape does not match the fixed 160000-sample analysis window"; return false; }
            if (type == kOvF32) {
                std::copy(audio.begin(), audio.end(), static_cast<float*>(data));
            } else if (type == kOvF16) {
                auto* dst = static_cast<uint16_t*>(data);
                for (size_t n = 0; n < count; ++n) dst[n] = floatToHalf(audio[n]);
            } else {
                api.tensorFree(tensor); error = "Unsupported model audio input type (expected FP32 or FP16)"; return false;
            }
        } else {
            // The Hugging Face HuBERT export uses an attention mask as its optional
            // second input. Every sample in our fixed 10 s window is valid.
            if (type == kOvI64) std::fill_n(static_cast<int64_t*>(data), count, int64_t{1});
            else if (type == kOvI32) std::fill_n(static_cast<int32_t*>(data), count, int32_t{1});
            else if (type == kOvU8 || type == kOvBoolean) std::fill_n(static_cast<uint8_t*>(data), count, uint8_t{1});
            else if (type == kOvF32) std::fill_n(static_cast<float*>(data), count, 1.0f);
            else if (type == kOvF16) std::fill_n(static_cast<uint16_t*>(data), count, uint16_t{0x3c00});
            else { api.tensorFree(tensor); error = "Unsupported attention-mask input element type"; return false; }
        }
        api.tensorFree(tensor);
    }
    if (modelInputCount == 0) { error = "The model has no usable input tensor"; return false; }
    const ov_status_e inferStatus = api.inferRequestInfer(request);
    if (inferStatus != kOvOk) { error = "OpenVINO inference failed: " + api.lastError("OpenVINO infer error"); return false; }

    ov_tensor_t* output = nullptr;
    if (api.inferRequestGetOutputTensorByIndex(request, 0, &output) != kOvOk || !output) {
        error = "Could not retrieve model output: " + api.lastError("OpenVINO output tensor error"); return false;
    }
    size_t outputCount = 0;
    ov_element_type_e outputType = 0;
    void* outputData = nullptr;
    const bool outputOk = api.tensorGetSize(output, &outputCount) == kOvOk &&
                          api.tensorGetElementType(output, &outputType) == kOvOk &&
                          api.tensorData(output, &outputData) == kOvOk && outputData && outputCount > 0;
    if (!outputOk || (outputType != kOvF32 && outputType != kOvF16)) {
        api.tensorFree(output); error = "Model output is unavailable or is not FP32/FP16"; return false;
    }
    std::vector<float> scores(outputCount);
    if (outputType == kOvF32) std::copy_n(static_cast<const float*>(outputData), outputCount, scores.begin());
    else {
        const auto* src = static_cast<const uint16_t*>(outputData);
        for (size_t i = 0; i < outputCount; ++i) scores[i] = halfToFloat(src[i]);
    }
    api.tensorFree(output);

    const float maxScore = *std::max_element(scores.begin(), scores.end());
    double total = 0.0;
    std::vector<double> probabilities(scores.size());
    for (size_t i = 0; i < scores.size(); ++i) { probabilities[i] = std::exp(static_cast<double>(scores[i] - maxScore)); total += probabilities[i]; }
    if (!(total > 0.0) || !std::isfinite(total)) { error = "Model returned invalid class scores"; return false; }
    for (double& p : probabilities) p /= total;
    std::vector<size_t> order(scores.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return probabilities[a] > probabilities[b]; });
    // Send every class score to the parent worker loop. It aggregates several
    // independent excerpts across the playing track rather than letting one
    // short slice dictate the genre shown in the UI.
    result = "WINDOW";
    for (size_t i = 0; i < order.size(); ++i) {
        const int index = static_cast<int>(order[i]);
        const auto found = labels.find(index);
        const std::string label = found == labels.end() ? ("Class " + std::to_string(index)) : found->second;
        std::ostringstream score;
        score << std::fixed << std::setprecision(3) << (probabilities[order[i]] * 100.0);
        result += "|" + sanitise(label) + "~" + score.str();
    }
    return true;
}

bool initializeOpenVino(ovapi::Api& api, const fs::path& runtimeDir, const fs::path& modelPath,
                        const std::string& requestedDevice,
                        ov_core_t*& core, ov_model_t*& model, ov_compiled_model_t*& compiled,
                        ov_infer_request_t*& inferRequest, size_t& modelInputCount,
                        std::string& selectedDevice, std::string& error) {
    if (!api.load(runtimeDir, error)) return false;
    const fs::path pluginConfig = runtimeDir / L"plugins.xml";
    ov_status_e status = -1;
    if (api.coreCreateWithConfigUnicode && fs::exists(pluginConfig))
        status = api.coreCreateWithConfigUnicode(pluginConfig.c_str(), &core);
    else
        status = api.coreCreate(&core);
    if (status != kOvOk || !core) { error = "OpenVINO Core initialization failed: " + api.lastError("Could not create OpenVINO Core"); return false; }

    ov_available_devices_t devices{};
    status = api.coreGetAvailableDevices(core, &devices);
    if (status != kOvOk) { error = "Could not enumerate OpenVINO devices: " + api.lastError("device enumeration failed"); return false; }
    std::vector<std::string> available;
    for (size_t i = 0; i < devices.size; ++i) {
        if (devices.devices && devices.devices[i]) available.emplace_back(devices.devices[i]);
    }
    api.availableDevicesFree(&devices);

    const std::string request = requestedDevice.empty() ? "AUTO" : requestedDevice;
    auto findDevice = [&](const std::string& prefix) -> std::string {
        for (const auto& device : available) {
            if (device == prefix || device.rfind(prefix + ".", 0) == 0) return device;
        }
        return {};
    };
    std::vector<std::string> compileCandidates;
    if (request == "AUTO") {
        // Availability alone does not guarantee this particular model will
        // compile on a plugin. Try each visible backend in a predictable order.
        for (const auto& preference : {std::string("NPU"), std::string("GPU"), std::string("CPU")}) {
            const std::string found = findDevice(preference);
            if (!found.empty()) compileCandidates.push_back(found);
        }
    } else if (request == "NPU" || request == "GPU" || request == "CPU") {
        const std::string found = findDevice(request);
        if (!found.empty()) compileCandidates.push_back(found);
    } else if (std::find(available.begin(), available.end(), request) != available.end()) {
        compileCandidates.push_back(request);
    }
    if (compileCandidates.empty()) {
        std::ostringstream list;
        for (size_t i = 0; i < available.size(); ++i) { if (i) list << ", "; list << available[i]; }
        error = "Requested OpenVINO device '" + request + "' is unavailable. Detected devices: " +
                (available.empty() ? std::string("none") : list.str()) +
                ". Check the relevant Windows accelerator driver and OpenVINO plugin.";
        return false;
    }

    if (api.coreReadModelUnicode)
        status = api.coreReadModelUnicode(core, modelPath.c_str(), nullptr, &model);
    else {
        const std::wstring wide = modelPath.wstring();
        const int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string modelUtf8(static_cast<size_t>(std::max(utf8Bytes, 1)), '\0');
        if (utf8Bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, modelUtf8.data(), utf8Bytes, nullptr, nullptr);
        status = api.coreReadModel(core, modelUtf8.c_str(), nullptr, &model);
    }
    if (status != kOvOk || !model) { error = "Could not load the local ONNX genre model: " + api.lastError("model loading failed"); return false; }

    size_t modelInputs = 0;
    status = api.modelInputsSize(model, &modelInputs);
    if (status != kOvOk || modelInputs == 0 || modelInputs > 2) {
        error = "The selected audio model must expose one audio input and at most one attention-mask input."; return false;
    }
    modelInputCount = modelInputs;
    std::array<size_t, 2> indexes{0, 1};
    std::array<int64_t, 2> dims{1, kModelSamples};
    std::array<ov_partial_shape_t, 2> shapes{};
    size_t preparedShapes = 0;
    for (size_t i = 0; i < modelInputs; ++i) {
        status = api.partialShapeCreateStatic(2, dims.data(), &shapes[i]);
        if (status != kOvOk) {
            for (size_t j = 0; j < preparedShapes; ++j) api.partialShapeFree(&shapes[j]);
            error = "Could not prepare the fixed model input shape: " + api.lastError("partial shape error");
            return false;
        }
        ++preparedShapes;
    }
    status = api.modelReshapeByPortIndexes(model, indexes.data(), shapes.data(), modelInputs);
    for (size_t i = 0; i < preparedShapes; ++i) api.partialShapeFree(&shapes[i]);
    if (status != kOvOk) { error = "Could not reshape the audio model to 10 seconds at 16 kHz: " + api.lastError("model reshape failed"); return false; }

    // Compile on the selected explicit device, or try AUTO candidates in
    // NPU > GPU > CPU order. Explicit choices never silently fall back.
    std::string lastCompileError;
    for (const auto& candidate : compileCandidates) {
        ov_compiled_model_t* attempt = nullptr;
        status = api.coreCompileModel(core, model, candidate.c_str(), 0, &attempt);
        if (status == kOvOk && attempt) {
            compiled = attempt;
            selectedDevice = candidate;
            break;
        }
        lastCompileError = candidate + ": " + api.lastError("model compilation failed");
        if (attempt) api.compiledModelFree(attempt);
        if (request != "AUTO") break;
    }
    if (!compiled) {
        error = "OpenVINO could not compile the audio model for the requested backend" +
                (lastCompileError.empty() ? std::string() : ". Last attempt: " + lastCompileError);
        return false;
    }
    status = api.compiledModelCreateInferRequest(compiled, &inferRequest);
    if (status != kOvOk || !inferRequest) { error = "Could not create an inference request: " + api.lastError("request creation failed"); return false; }
    return true;
}

int runWorker(const std::map<std::wstring, std::wstring>& args) {
    const fs::path runtimeDir = getArg(args, L"--runtime");
    const fs::path modelPath = getArg(args, L"--model");
    const fs::path configPath = getArg(args, L"--config");
    const fs::path inbox = getArg(args, L"--inbox");
    const fs::path statusPath = getArg(args, L"--status");
    const fs::path trackResultPath = getArg(args, L"--track-result");
    const fs::path captureResultPath = getArg(args, L"--capture-result");
    const fs::path stopPath = getArg(args, L"--stop");
    const fs::path resetPath = getArg(args, L"--reset");
    const std::wstring requestedDeviceWide = getArg(args, L"--device");
    const std::string requestedDevice(requestedDeviceWide.begin(), requestedDeviceWide.end());
    if (runtimeDir.empty() || modelPath.empty() || inbox.empty() || statusPath.empty() ||
        trackResultPath.empty() || captureResultPath.empty() || stopPath.empty()) {
        std::wcerr << L"Missing required inference worker arguments.\n"; return 2;
    }
    fs::create_directories(inbox);
    writeAtomicLine(statusPath, std::string("STARTING|Scarlett Scout inference worker v") + SCARLETT_SCOUT_VERSION_A + " — loading native OpenVINO C API and enumerating devices");

    ovapi::Api api;
    ov_core_t* core = nullptr;
    ov_model_t* model = nullptr;
    ov_compiled_model_t* compiled = nullptr;
    ov_infer_request_t* request = nullptr;
    size_t modelInputCount = 0;
    std::string selectedDevice;
    std::string error;
    if (!initializeOpenVino(api, runtimeDir, modelPath, requestedDevice, core, model, compiled, request, modelInputCount, selectedDevice, error)) {
        writeAtomicLine(statusPath, "ERROR|" + sanitise(error));
        if (request) api.inferRequestFree(request);
        if (compiled) api.compiledModelFree(compiled);
        if (model) api.modelFree(model);
        if (core) api.coreFree(core);
        return 3;
    }
    const auto labels = readLabels(configPath);
    writeAtomicLine(statusPath, std::string("READY|") + selectedDevice + "|Scarlett Scout v" + SCARLETT_SCOUT_VERSION_A + " · native OpenVINO loaded the local HuBERT genre model on " + selectedDevice);
    std::map<std::string, double> trackScoreSums;
    std::map<std::string, unsigned> trackWinnerCounts;
    unsigned trackWindowCount = 0;

    while (!fs::exists(stopPath)) {
        if (!resetPath.empty() && fs::exists(resetPath)) {
            trackScoreSums.clear();
            trackWinnerCounts.clear();
            trackWindowCount = 0;
            std::error_code resetError;
            fs::remove(resetPath, resetError);
            writeAtomicLine(trackResultPath, "TRACK|0|Waiting for track-analysis windows");
        }
        std::vector<fs::path> jobs;
        try {
            for (const auto& entry : fs::directory_iterator(inbox)) {
                if (entry.is_regular_file() && entry.path().extension() == L".wav") {
                    const auto name = entry.path().filename().wstring();
                    if (name.rfind(L"capture-", 0) == 0 || name.rfind(L"track-", 0) == 0)
                        jobs.push_back(entry.path());
                }
            }
            std::sort(jobs.begin(), jobs.end(), [](const fs::path& a, const fs::path& b) {
                std::error_code ea, eb;
                return fs::last_write_time(a, ea) < fs::last_write_time(b, eb);
            });
        } catch (...) {}
        if (jobs.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(250)); continue; }
        const fs::path job = jobs.front();
        std::vector<float> captured;
        uint32_t rate = 0;
        if (!readMonoPcm16Wav(job, captured, rate, error)) {
            writeAtomicLine(statusPath, "ERROR|Could not read queued sample: " + sanitise(error));
            std::error_code ec; fs::rename(job, job.wstring() + L".failed", ec);
            continue;
        }
        if (rate != 48000) {
            writeAtomicLine(statusPath, "ERROR|Expected 48 kHz capture audio for the model resampler");
            std::error_code ec; fs::rename(job, job.wstring() + L".failed", ec);
            continue;
        }
        std::vector<float> audio = resample48kTo16k(captured);
        if (audio.size() > static_cast<size_t>(kModelSamples)) audio.erase(audio.begin(), audio.end() - kModelSamples);
        if (audio.size() < static_cast<size_t>(kModelSamples)) audio.resize(static_cast<size_t>(kModelSamples), 0.0f);
        double mean = 0.0;
        for (float v : audio) mean += v;
        mean /= static_cast<double>(audio.size());
        double variance = 0.0;
        for (float v : audio) { const double d = static_cast<double>(v) - mean; variance += d * d; }
        const double stddev = std::sqrt(variance / static_cast<double>(audio.size()));
        const double denom = std::max(stddev, 1.0e-7);
        for (float& v : audio) v = static_cast<float>(std::clamp((static_cast<double>(v) - mean) / denom, -5.0, 5.0));

        std::string result;
        if (inferGenre(api, request, modelInputCount, audio, labels, result, error)) {
            const bool trackWindow = job.filename().wstring().rfind(L"track-", 0) == 0;
            std::vector<std::pair<std::string, double>> probabilities;
            std::istringstream parts(result);
            std::string token;
            std::getline(parts, token, '|'); // WINDOW
            while (std::getline(parts, token, '|')) {
                const size_t split = token.rfind('~');
                if (split == std::string::npos) continue;
                try { probabilities.emplace_back(token.substr(0, split), std::stod(token.substr(split + 1))); }
                catch (...) {}
            }
            if (trackWindow) {
                for (const auto& item : probabilities) trackScoreSums[item.first] += item.second;
                if (!probabilities.empty()) ++trackWinnerCounts[probabilities.front().first];
                ++trackWindowCount;
                std::vector<std::pair<std::string, double>> averages;
                for (const auto& item : trackScoreSums)
                    averages.emplace_back(item.first, item.second / std::max(1u, trackWindowCount));
                std::sort(averages.begin(), averages.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                const std::string leadingLabel = averages.empty() ? std::string("unknown") : averages.front().first;
                const unsigned leadingVotes = trackWinnerCounts[leadingLabel];
                const double voteShare = 100.0 * static_cast<double>(leadingVotes) / std::max(1u, trackWindowCount);
                const double leadingScore = averages.empty() ? 0.0 : averages.front().second;
                const double runnerUpScore = averages.size() > 1 ? averages[1].second : 0.0;
                // A high softmax score from one or two excerpts is not enough
                // to declare a track genre. Require coverage, a useful margin,
                // and repeatable winning votes across independent excerpts.
                const bool reliableTrackVote = trackWindowCount >= 4 && voteShare >= 50.0 &&
                                               leadingScore >= 38.0 &&
                                               (leadingScore - runnerUpScore) >= 6.0;
                std::ostringstream summary;
                summary << (reliableTrackVote ? "TRACK|" : "TRACK_UNCERTAIN|")
                        << trackWindowCount << "|" << std::fixed << std::setprecision(0) << voteShare;
                for (size_t i = 0; i < std::min<size_t>(3, averages.size()); ++i)
                    summary << "|" << sanitise(averages[i].first) << "~" << std::fixed << std::setprecision(1) << averages[i].second;
                writeAtomicLine(trackResultPath, summary.str());
            } else {
                std::sort(probabilities.begin(), probabilities.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                std::ostringstream summary;
                summary << "CAPTURE";
                for (size_t i = 0; i < std::min<size_t>(3, probabilities.size()); ++i)
                    summary << "|" << sanitise(probabilities[i].first) << "~" << std::fixed << std::setprecision(1) << probabilities[i].second;
                writeAtomicLine(captureResultPath, summary.str());
            }
            writeAtomicLine(statusPath, std::string("READY|") + selectedDevice + "|Scarlett Scout v" + SCARLETT_SCOUT_VERSION_A + " · native OpenVINO inference completed successfully on " + selectedDevice);
            std::error_code ec; fs::remove(job, ec);
        } else {
            writeAtomicLine(statusPath, "ERROR|OpenVINO inference failed: " + sanitise(error));
            std::error_code ec; fs::rename(job, job.wstring() + L".failed", ec);
        }
    }

    if (request) api.inferRequestFree(request);
    if (compiled) api.compiledModelFree(compiled);
    if (model) api.modelFree(model);
    if (core) api.coreFree(core);
    writeAtomicLine(statusPath, "STOPPED|Native OpenVINO inference worker stopped cleanly");
    return 0;
}
} // namespace

int wmain() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    const auto args = parseArgs(argc, argv);
    const int result = runWorker(args);
    LocalFree(argv);
    return result;
}
