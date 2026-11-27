#include "audio_engine.hpp"
#include "wav.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <sstream>

namespace scout {
namespace fs = std::filesystem;

static std::wstring utf8ToWide(const char* value) {
    if (!value || !*value) return L"";
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (needed <= 1) return std::wstring(value, value + std::strlen(value));
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, out.data(), needed);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

AudioEngine::AudioEngine() {
    ring_ = std::make_unique<std::atomic<uint32_t>[]>(capacityFrames_ * 2);
    for (uint64_t i = 0; i < capacityFrames_ * 2; ++i) ring_[i].store(0, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::setDetectionThreshold(float threshold) {
    threshold = std::clamp(threshold, 0.45f, 0.90f);
    detectionThreshold_.store(threshold, std::memory_order_release);
    InterestingSectionDetector::Config cfg;
    cfg.threshold = threshold;
    cfg.persistence = 2;
    cfg.cooldownSeconds = 45;
    cfg.baselineWindows = 12;
    interestingDetector_.setConfig(cfg);
}

bool AudioEngine::initialize(std::wstring& error) {
    if (contextInitialized_) return refreshSources(error);
    ma_backend backend = ma_backend_wasapi;
    ma_context_config config = ma_context_config_init();
    const ma_result result = ma_context_init(&backend, 1, &config, &context_);
    if (result != MA_SUCCESS) {
        error = L"Could not initialize Windows WASAPI audio. miniaudio error: " + std::to_wstring(result);
        return false;
    }
    contextInitialized_ = true;
    return refreshSources(error);
}

void AudioEngine::shutdown() {
    stopAnalyzer();
    stop();
    if (contextInitialized_) {
        ma_context_uninit(&context_);
        contextInitialized_ = false;
    }
}

bool AudioEngine::refreshSources(std::wstring& error) {
    if (!contextInitialized_) { error = L"Audio context has not been initialized."; return false; }
    ma_device_info* playback = nullptr;
    ma_device_info* capture = nullptr;
    ma_uint32 playbackCount = 0, captureCount = 0;
    const ma_result r = ma_context_get_devices(&context_, &playback, &playbackCount, &capture, &captureCount);
    if (r != MA_SUCCESS) {
        error = L"WASAPI device enumeration failed. miniaudio error: " + std::to_wstring(r);
        return false;
    }
    sources_.clear();
    auto channelsFor = [](const ma_device_info& d) -> unsigned {
        unsigned channels = 0;
        for (ma_uint32 i = 0; i < d.nativeDataFormatCount; ++i)
            channels = std::max(channels, static_cast<unsigned>(d.nativeDataFormats[i].channels));
        return std::clamp(channels == 0 ? 2u : channels, 1u, static_cast<unsigned>(MA_MAX_CHANNELS));
    };
    // Render endpoints become WASAPI loopback sources. Each endpoint is represented separately.
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        AudioSource s;
        s.type = SourceType::PlaybackLoopback;
        s.id = playback[i].id;
        s.name = utf8ToWide(playback[i].name);
        s.availableChannels = channelsFor(playback[i]);
        s.isDefault = playback[i].isDefault == MA_TRUE;
        sources_.push_back(std::move(s));
    }
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        AudioSource s;
        s.type = SourceType::Recording;
        s.id = capture[i].id;
        s.name = utf8ToWide(capture[i].name);
        s.availableChannels = channelsFor(capture[i]);
        s.isDefault = capture[i].isDefault == MA_TRUE;
        sources_.push_back(std::move(s));
    }
    error.clear();
    return true;
}

void AudioEngine::makeChannelChoices(size_t sourceIndex) {
    channelChoices_.clear();
    if (sourceIndex >= sources_.size()) return;
    const unsigned channels = sources_[sourceIndex].availableChannels;
    for (unsigned first = 0; first + 1 < channels; first += 2) {
        ChannelChoice c;
        c.first = first; c.width = 2;
        c.label = L"Stereo pair " + std::to_wstring(first + 1) + L" / " + std::to_wstring(first + 2);
        channelChoices_.push_back(std::move(c));
    }
    for (unsigned ch = 0; ch < channels; ++ch) {
        ChannelChoice c;
        c.first = ch; c.width = 1;
        c.label = L"Mono channel " + std::to_wstring(ch + 1);
        channelChoices_.push_back(std::move(c));
    }
    if (channelChoices_.empty()) channelChoices_.push_back({0, 1, L"Mono channel 1"});
}

bool AudioEngine::start(size_t sourceIndex, size_t channelIndex, ChannelMode mode, std::wstring& error) {
    stop();
    if (sourceIndex >= sources_.size()) { error = L"Select an audio source first."; return false; }
    makeChannelChoices(sourceIndex);
    if (channelIndex >= channelChoices_.size()) { error = L"Select a valid channel pair."; return false; }
    const AudioSource& source = sources_[sourceIndex];
    const ChannelChoice& choice = channelChoices_[channelIndex];
    selectedFirstChannel_ = choice.first;
    selectedPairWidth_ = choice.width;
    mode_ = mode;

    const ma_device_type type = source.type == SourceType::PlaybackLoopback ? ma_device_type_loopback : ma_device_type_capture;
    ma_device_config dc = ma_device_config_init(type);
    dc.capture.pDeviceID = &source.id;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = source.availableChannels; // Request every channel exposed by the endpoint so pairs 1/2, 3/4, etc. map to real stream channels.
    dc.sampleRate = kSampleRate;
    dc.periodSizeInMilliseconds = 10;
    // Keep WASAPI shared-mode format conversion enabled. The app processes
    // 48 kHz float32; Windows/miniaudio can adapt endpoints whose mix rate is
    // 44.1 kHz or another supported native rate instead of failing to open.
    dc.wasapi.noAutoConvertSRC = MA_FALSE;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.dataCallback = &AudioEngine::dataCallback;
    dc.pUserData = this;
    const ma_result initResult = ma_device_init(&context_, &dc, &device_);
    if (initResult != MA_SUCCESS) {
        error = L"Could not open " + source.name + L". WASAPI/miniaudio error: " + std::to_wstring(initResult) +
                L". Check Windows sound settings and that another application has not taken exclusive control.";
        return false;
    }
    deviceInitialized_ = true;
    const unsigned actualChannels = device_.capture.channels;
    actualInputChannels_.store(actualChannels);
    if (actualChannels <= selectedFirstChannel_ || (selectedPairWidth_ == 2 && actualChannels <= selectedFirstChannel_ + 1)) {
        ma_device_uninit(&device_);
        deviceInitialized_ = false;
        error = L"The selected endpoint opened with " + std::to_wstring(actualChannels) +
                L" capture channel(s), so the chosen channel selection is not available. Windows may expose only a stereo mix for this endpoint.";
        return false;
    }
    writeFrame_.store(0, std::memory_order_release);
    callbackCount_.store(0, std::memory_order_release);
    nullInputCallbackCount_.store(0, std::memory_order_release);
    lastCandidateFrame_.store(0, std::memory_order_release);
    autoCapturePending_.store(false, std::memory_order_release);
    autoCaptureReady_.store(false, std::memory_order_release);
    autoCaptureStartFrame_.store(0, std::memory_order_release);
    autoCaptureEndFrame_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(analysisMutex_);
        analysis_ = AnalysisSnapshot{};
        trackAccumulator_.reset();
        interestingDetector_.reset();
        latestInterestingEvent_ = InterestingEvent{};
        candidateDescription_ = L"Automatic capture is enabled. Waiting for a distinct musical passage.";
    }
    lastTrackSampleFrame_.store(0, std::memory_order_release);
    lastNpuTrackSubmitFrame_.store(0, std::memory_order_release);
    // Tell the long-lived inference worker that a new record/track session began.
    if (!appDir_.empty()) {
        try {
            std::ofstream resetFile(fs::path(appDir_) / L"npu-reset.txt", std::ios::binary | std::ios::trunc);
            resetFile << "reset\n";
        } catch (...) {}
    }
    running_.store(true, std::memory_order_release);
    const ma_result startResult = ma_device_start(&device_);
    if (startResult != MA_SUCCESS) {
        running_.store(false, std::memory_order_release);
        ma_device_uninit(&device_);
        deviceInitialized_ = false;
        error = L"The audio endpoint opened but could not start. miniaudio error: " + std::to_wstring(startResult);
        return false;
    }
    error.clear();
    return true;
}

void AudioEngine::stop() {
    running_.store(false, std::memory_order_release);
    autoCapturePending_.store(false, std::memory_order_release);
    autoCaptureReady_.store(false, std::memory_order_release);
    if (deviceInitialized_) {
        ma_device_uninit(&device_);
        deviceInitialized_ = false;
    }
    actualInputChannels_.store(0);
}

void AudioEngine::dataCallback(ma_device* device, void*, const void* input, ma_uint32 frameCount) {
    if (!device) return;
    auto* self = static_cast<AudioEngine*>(device->pUserData);
    if (!self || !self->running_.load(std::memory_order_relaxed)) return;
    self->callbackCount_.fetch_add(1, std::memory_order_relaxed);
    if (!input || frameCount == 0) {
        self->nullInputCallbackCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    self->pushFrames(static_cast<const float*>(input), device->capture.channels, frameCount);
}

void AudioEngine::pushFrames(const float* input, unsigned inputChannels, unsigned frameCount) {
    if (!input || inputChannels == 0) return;
    const unsigned first = selectedFirstChannel_;
    const unsigned width = selectedPairWidth_;
    const uint64_t start = writeFrame_.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < frameCount; ++i) {
        const float* frame = input + static_cast<size_t>(i) * inputChannels;
        const float a = first < inputChannels ? frame[first] : 0.0f;
        const float b = (width == 2 && first + 1 < inputChannels) ? frame[first + 1] : a;
        float left = a, right = b;
        if (mode_ == ChannelMode::Mono) {
            const float mono = width == 2 ? 0.5f * (a + b) : a;
            left = mono; right = mono;
        }
        const uint64_t idx = ((start + i) % capacityFrames_) * 2;
        ring_[idx].store(std::bit_cast<uint32_t>(left), std::memory_order_relaxed);
        ring_[idx + 1].store(std::bit_cast<uint32_t>(right), std::memory_order_relaxed);
    }
    writeFrame_.store(start + frameCount, std::memory_order_release);
}

size_t AudioEngine::availableFrames() const {
    return static_cast<size_t>(std::min(writeFrame_.load(std::memory_order_acquire), capacityFrames_));
}

std::vector<float> AudioEngine::copyLastSeconds(unsigned seconds) const {
    const uint64_t end = writeFrame_.load(std::memory_order_acquire);
    return copyLastFrames(end, seconds);
}

std::vector<float> AudioEngine::copyLastFrames(uint64_t endFrame, unsigned seconds) const {
    const uint64_t current = writeFrame_.load(std::memory_order_acquire);
    endFrame = std::min(endFrame, current);
    const uint64_t wanted = static_cast<uint64_t>(seconds) * kSampleRate;
    const uint64_t count = std::min({wanted, endFrame, capacityFrames_});
    const uint64_t begin = endFrame - count;
    std::vector<float> out(static_cast<size_t>(count) * 2);
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t idx = ((begin + i) % capacityFrames_) * 2;
        const uint32_t l = ring_[idx].load(std::memory_order_relaxed);
        const uint32_t r = ring_[idx + 1].load(std::memory_order_relaxed);
        out[static_cast<size_t>(i) * 2] = std::bit_cast<float>(l);
        out[static_cast<size_t>(i) * 2 + 1] = std::bit_cast<float>(r);
    }
    return out;
}

std::vector<float> AudioEngine::copyFrameRange(uint64_t beginFrame, uint64_t endFrame) const {
    const uint64_t current = writeFrame_.load(std::memory_order_acquire);
    endFrame = std::min(endFrame, current);
    if (endFrame <= beginFrame) return {};
    const uint64_t oldest = current > capacityFrames_ ? current - capacityFrames_ : 0;
    beginFrame = std::max(beginFrame, oldest);
    if (endFrame <= beginFrame) return {};
    const uint64_t count = std::min(endFrame - beginFrame, capacityFrames_);
    std::vector<float> out(static_cast<size_t>(count) * 2);
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t idx = ((beginFrame + i) % capacityFrames_) * 2;
        const uint32_t l = ring_[idx].load(std::memory_order_relaxed);
        const uint32_t r = ring_[idx + 1].load(std::memory_order_relaxed);
        out[static_cast<size_t>(i) * 2] = std::bit_cast<float>(l);
        out[static_cast<size_t>(i) * 2 + 1] = std::bit_cast<float>(r);
    }
    return out;
}

AnalysisSnapshot AudioEngine::latestAnalysis() const {
    std::lock_guard<std::mutex> lock(analysisMutex_);
    return analysis_;
}

TrackSummary AudioEngine::trackSummary() const {
    std::lock_guard<std::mutex> lock(analysisMutex_);
    return trackAccumulator_.summary();
}

std::wstring AudioEngine::npuStatus() const {
    std::lock_guard<std::mutex> lock(npuMutex_);
    return npuStatus_;
}

std::wstring AudioEngine::npuResult() const {
    std::lock_guard<std::mutex> lock(npuMutex_);
    return npuResult_;
}

std::wstring AudioEngine::captureGenreResult() const {
    std::lock_guard<std::mutex> lock(npuMutex_);
    return captureGenreResult_;
}

std::wstring AudioEngine::candidateDescription() const {
    std::lock_guard<std::mutex> lock(analysisMutex_);
    return candidateDescription_;
}

InterestingEvent AudioEngine::latestInterestingEvent() const {
    std::lock_guard<std::mutex> lock(analysisMutex_);
    return latestInterestingEvent_;
}

void AudioEngine::setAutoCaptureEnabled(bool enabled) {
    autoCaptureEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        autoCapturePending_.store(false, std::memory_order_release);
        autoCaptureReady_.store(false, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(analysisMutex_);
    candidateDescription_ = enabled
        ? L"Auto-capture enabled · waiting for the next distinctive passage."
        : L"Auto-capture disabled · Capture Now remains available.";
}

bool AudioEngine::takeAutoCaptureEvent(AutoCaptureEvent& event) {
    if (!autoCaptureReady_.exchange(false, std::memory_order_acq_rel)) return false;
    {
        std::lock_guard<std::mutex> lock(autoCaptureMutex_);
        event.startFrame = autoCaptureStartFrame_.load(std::memory_order_acquire);
        event.endFrame = autoCaptureEndFrame_.load(std::memory_order_acquire);
        event.analysis = pendingCaptureAnalysis_;
        event.reason = pendingCaptureReason_;
    }
    autoCapturePending_.store(false, std::memory_order_release);
    return event.endFrame > event.startFrame;
}

void AudioEngine::submitCapturedSampleForNpu(const std::vector<float>& stereo) {
    (void)submitNpuSegment(stereo, false);
}

void AudioEngine::startAnalyzer(const std::wstring& appDir, const std::wstring& inferenceDevice) {
    if (analyzerThread_.joinable()) return;
    appDir_ = appDir;
    inferenceDevice_ = inferenceDevice.empty() ? L"AUTO" : inferenceDevice;
    analyzerStop_.store(false);
    const fs::path root(appDir_);
    const fs::path inbox = root / L"npu-inbox";
    const fs::path worker = root / L"ScarlettScoutNpuWorker.exe";
    const fs::path model = root / L"models" / L"genre-model.onnx";
    const fs::path runtimeDir = root / L"openvino_runtime" / L"runtime" / L"bin" / L"intel64" / L"Release";
    try { fs::create_directories(inbox); } catch (...) {}

    std::error_code ec;
    fs::remove(root / L"npu-stop.txt", ec);
    fs::remove(root / L"npu-status.txt", ec);
    fs::remove(root / L"npu-result.txt", ec); // remove the single-channel result file used by early builds
    fs::remove(root / L"npu-track-result.txt", ec);
    fs::remove(root / L"npu-capture-result.txt", ec);
    fs::remove(root / L"npu-reset.txt", ec);

    if (fs::exists(worker) && fs::exists(model) && fs::exists(runtimeDir / L"openvino_c.dll")) {
        auto quote = [](const std::wstring& value) { return L"\"" + value + L"\""; };
        std::wstring command = quote(worker.wstring()) +
            L" --runtime " + quote(runtimeDir.wstring()) +
            L" --model " + quote(model.wstring()) +
            L" --config " + quote((root / L"models" / L"config.json").wstring()) +
            L" --inbox " + quote(inbox.wstring()) +
            L" --status " + quote((root / L"npu-status.txt").wstring()) +
            L" --track-result " + quote((root / L"npu-track-result.txt").wstring()) +
            L" --capture-result " + quote((root / L"npu-capture-result.txt").wstring()) +
            L" --stop " + quote((root / L"npu-stop.txt").wstring()) +
            L" --reset " + quote((root / L"npu-reset.txt").wstring()) +
            L" --device " + quote(inferenceDevice_);
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           appDir_.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            npuWorkerProcess_ = pi.hProcess;
            std::lock_guard<std::mutex> lock(npuMutex_);
            npuStatus_ = L"Starting OpenVINO inference worker · requested " + inferenceDevice_;
        } else {
            std::lock_guard<std::mutex> lock(npuMutex_);
            npuStatus_ = L"Could not start native NPU worker (Windows error " + std::to_wstring(GetLastError()) + L")";
        }
    } else {
        std::lock_guard<std::mutex> lock(npuMutex_);
        npuStatus_ = L"Native NPU runtime/model not installed — rerun install.sh from WSL";
    }
    analyzerThread_ = std::thread(&AudioEngine::analyzerLoop, this);
}

void AudioEngine::stopAnalyzer() {
    analyzerStop_.store(true);
    if (analyzerThread_.joinable()) analyzerThread_.join();
    if (npuWorkerProcess_) {
        try {
            std::ofstream stopFile(fs::path(appDir_) / L"npu-stop.txt", std::ios::binary | std::ios::trunc);
            stopFile << "stop\n";
        } catch (...) {}
        const DWORD waited = WaitForSingleObject(npuWorkerProcess_, 5000);
        if (waited == WAIT_TIMEOUT) TerminateProcess(npuWorkerProcess_, 0);
        CloseHandle(npuWorkerProcess_);
        npuWorkerProcess_ = nullptr;
    }
}

void AudioEngine::loadNpuState() {
    try {
        std::ifstream statusFile(fs::path(appDir_) / L"npu-status.txt", std::ios::binary);
        if (statusFile) {
            std::string line; std::getline(statusFile, line);
            std::wstring value(line.begin(), line.end());
            std::lock_guard<std::mutex> lock(npuMutex_);
            npuStatus_ = value;
        }
        auto readResult = [&](const fs::path& path, bool trackResult) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return;
            std::string line; std::getline(file, line);
            std::wstring value(line.begin(), line.end());
            std::replace(value.begin(), value.end(), L'|', L' ');
            std::replace(value.begin(), value.end(), L'~', L' ');
            if (trackResult) {
                if (value.rfind(L"TRACK_UNCERTAIN ", 0) == 0)
                    npuResult_ = L"Mixed / uncertain · " + value.substr(16);
                else if (value.rfind(L"TRACK ", 0) == 0)
                    npuResult_ = L"Track consensus · " + value.substr(6);
            } else if (value.rfind(L"CAPTURE ", 0) == 0) {
                captureGenreResult_ = L"Latest capture · " + value.substr(8);
            }
        };
        std::lock_guard<std::mutex> lock(npuMutex_);
        readResult(fs::path(appDir_) / L"npu-track-result.txt", true);
        readResult(fs::path(appDir_) / L"npu-capture-result.txt", false);
    } catch (...) {}
}

bool AudioEngine::submitNpuSegment(const std::vector<float>& stereo, bool trackWindow) {
    constexpr size_t modelFrames = static_cast<size_t>(kSampleRate) * 10;
    const size_t totalFrames = stereo.size() / 2;
    if (totalFrames < modelFrames) return false;
    // For a captured event, classify the centered ten seconds rather than
    // blindly taking the tail (which can over-weight post-roll or silence).
    const size_t firstFrame = (totalFrames - modelFrames) / 2;
    std::vector<float> mono(modelFrames);
    for (size_t i = 0; i < modelFrames; ++i)
        mono[i] = 0.5f * (stereo[(firstFrame + i) * 2] + stereo[(firstFrame + i) * 2 + 1]);
    const fs::path inbox = fs::path(appDir_) / L"npu-inbox";
    // Bound the inference backlog so a slow accelerator cannot accumulate an
    // unbounded queue while the audio callback continues running normally.
    if (trackWindow) {
        size_t queuedTrackWindows = 0;
        try {
            for (const auto& entry : fs::directory_iterator(inbox))
                if (entry.is_regular_file() && entry.path().filename().wstring().rfind(L"track-", 0) == 0) ++queuedTrackWindows;
        } catch (...) {}
        if (queuedTrackWindows >= 8) return false;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::wstring prefix = trackWindow ? L"track-" : L"capture-";
    const fs::path file = inbox / (prefix + std::to_wstring(stamp) + L".wav");
    const fs::path temp = file.wstring() + L".part";
    std::wstring error;
    if (!writeWavMono16(temp.wstring(), mono, kSampleRate, error)) return false;
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) { fs::remove(temp, ec); return false; }
    return true;
}

void AudioEngine::analyzerLoop() {
    constexpr unsigned kAnalysisSeconds = 18;
    constexpr unsigned kAnalysisIntervalSeconds = 2;
    constexpr unsigned kTrackConsensusIntervalSeconds = 10;
    constexpr unsigned kTrackInferenceIntervalSeconds = 15;
    // Preserve the analysis window as lead-in, then collect additional audio
    // after the event so that the interesting passage is not cut off.
    constexpr unsigned kAutoCapturePreRollSeconds = 18;
    constexpr unsigned kAutoCapturePostRollSeconds = 12;
    constexpr unsigned kAutoCaptureCooldownSeconds = 45;
    uint64_t lastAnalysisFrame = 0;
    uint64_t lastAutoCaptureFrame = 0;

    while (!analyzerStop_.load()) {
        loadNpuState();
        if (running_.load(std::memory_order_acquire)) {
            const uint64_t end = writeFrame_.load(std::memory_order_acquire);
            const uint64_t interval = static_cast<uint64_t>(kAnalysisIntervalSeconds) * kSampleRate;
            if (end >= static_cast<uint64_t>(kSampleRate) * kAnalysisSeconds &&
                (lastAnalysisFrame == 0 || end - lastAnalysisFrame >= interval)) {
                const bool havePriorAnalysis = lastAnalysisFrame != 0;
                lastAnalysisFrame = end;
                auto samples = copyLastFrames(end, kAnalysisSeconds);
                auto snapshot = analyzeAudio(samples, kSampleRate);
                AnalysisSnapshot previousSnapshot;
                {
                    std::lock_guard<std::mutex> lock(analysisMutex_);
                    previousSnapshot = analysis_;
                }
                bool changedMusicalSection = false;
                if (havePriorAnalysis) {
                    snapshot.sectionNovelty = sectionNoveltyScore(previousSnapshot, snapshot);
                    changedMusicalSection = snapshot.sectionNovelty >= 0.60f;
                    if (changedMusicalSection)
                        snapshot.section = "New section / notable change";
                }

                const InterestingEvent interestingEvent =
                    interestingDetector_.update(snapshot, end, kSampleRate);
                {
                    std::lock_guard<std::mutex> lock(analysisMutex_);
                    if (interestingEvent.triggered) {
                        latestInterestingEvent_ = interestingEvent;
                        snapshot.interestingScore = std::max(snapshot.interestingScore, interestingEvent.score);
                        snapshot.section = interestEventTypeName(interestingEvent.type);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(analysisMutex_);
                    analysis_ = snapshot;
                    const uint64_t lastConsensusFrame = lastTrackSampleFrame_.load(std::memory_order_acquire);
                    if (lastConsensusFrame == 0 || end - lastConsensusFrame >=
                        static_cast<uint64_t>(kTrackConsensusIntervalSeconds) * kSampleRate) {
                        trackAccumulator_.add(snapshot);
                        lastTrackSampleFrame_.store(end, std::memory_order_release);
                    }
                }

                const uint64_t lastGenreFrame = lastNpuTrackSubmitFrame_.load(std::memory_order_acquire);
                if (lastGenreFrame == 0 || end - lastGenreFrame >= static_cast<uint64_t>(kTrackInferenceIntervalSeconds) * kSampleRate) {
                    if (submitNpuSegment(samples, true)) lastNpuTrackSubmitFrame_.store(end, std::memory_order_release);
                }

                // When an event is found, preserve the full 18-second analysis
                // window, then wait for 12 seconds of post-roll before saving.
                // The earlier prompt-based workflow saved at detection time and
                // could cut off the passage that caused the trigger.
                if (autoCapturePending_.load(std::memory_order_acquire)) {
                    const uint64_t targetEnd = autoCaptureEndFrame_.load(std::memory_order_acquire);
                    if (targetEnd != 0 && end >= targetEnd) {
                        autoCaptureReady_.store(true, std::memory_order_release);
                        std::lock_guard<std::mutex> lock(analysisMutex_);
                        candidateDescription_ = L"Post-roll complete · saving the detected passage.";
                    }
                } else if (autoCaptureEnabled_.load(std::memory_order_acquire) &&
                           !autoCaptureReady_.load(std::memory_order_acquire) &&
                           interestingEvent.triggered &&
                           (lastAutoCaptureFrame == 0 ||
                            end - lastAutoCaptureFrame >=
                                static_cast<uint64_t>(kAutoCaptureCooldownSeconds) * kSampleRate)) {
                    const uint64_t preFrames = static_cast<uint64_t>(kAutoCapturePreRollSeconds) * kSampleRate;
                    const uint64_t postFrames = static_cast<uint64_t>(kAutoCapturePostRollSeconds) * kSampleRate;
                    const uint64_t begin = interestingEvent.startFrame > preFrames
                        ? interestingEvent.startFrame - preFrames : 0;
                    const uint64_t finish = end + postFrames;

                    std::wstring reason = utf8ToWide(interestEventTypeName(interestingEvent.type));
                    if (!interestingEvent.explanation.empty())
                        reason += L" · " + utf8ToWide(interestingEvent.explanation.c_str());
                    {
                        std::lock_guard<std::mutex> lock(autoCaptureMutex_);
                        pendingCaptureAnalysis_ = snapshot;
                        pendingCaptureReason_ = reason;
                    }
                    autoCaptureStartFrame_.store(begin, std::memory_order_release);
                    autoCaptureEndFrame_.store(finish, std::memory_order_release);
                    lastCandidateFrame_.store(end, std::memory_order_release);
                    lastAutoCaptureFrame = end;
                    autoCapturePending_.store(true, std::memory_order_release);
                    std::lock_guard<std::mutex> lock(analysisMutex_);
                    candidateDescription_ = reason + L" · recording twelve seconds of post-roll.";
                }

            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
}
