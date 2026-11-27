#pragma once
#include "dsp.hpp"
#include "interesting_detector.hpp"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "miniaudio.h"

namespace scout {
enum class SourceType { Recording, PlaybackLoopback };
enum class ChannelMode { Stereo, Mono };

struct AudioSource {
    SourceType type = SourceType::Recording;
    ma_device_id id{};
    std::wstring name;
    unsigned availableChannels = 2;
    bool isDefault = false;
};

struct ChannelChoice {
    unsigned first = 0; // zero-based
    unsigned width = 2;
    std::wstring label;
};

struct AutoCaptureEvent {
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;
    AnalysisSnapshot analysis;
    std::wstring reason;
};

class AudioEngine {
public:
    static constexpr unsigned kSampleRate = 48000;
    static constexpr unsigned kBufferSeconds = 120;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool initialize(std::wstring& error);
    void shutdown();
    bool refreshSources(std::wstring& error);
    const std::vector<AudioSource>& sources() const { return sources_; }
    const std::vector<ChannelChoice>& channelChoices() const { return channelChoices_; }
    void makeChannelChoices(size_t sourceIndex);

    bool start(size_t sourceIndex, size_t channelIndex, ChannelMode mode, std::wstring& error);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    unsigned actualInputChannels() const { return actualInputChannels_.load(); }

    uint64_t writtenFrames() const { return writeFrame_.load(std::memory_order_acquire); }
    uint64_t callbackCount() const { return callbackCount_.load(std::memory_order_acquire); }
    uint64_t nullInputCallbackCount() const { return nullInputCallbackCount_.load(std::memory_order_acquire); }
    std::vector<float> copyLastSeconds(unsigned seconds) const;
    std::vector<float> copyLastFrames(uint64_t endFrame, unsigned seconds) const;
    std::vector<float> copyFrameRange(uint64_t beginFrame, uint64_t endFrame) const;
    size_t availableFrames() const;

    AnalysisSnapshot latestAnalysis() const;
    TrackSummary trackSummary() const;
    std::wstring npuStatus() const;
    std::wstring npuResult() const;
    std::wstring captureGenreResult() const;
    void startAnalyzer(const std::wstring& appDir, const std::wstring& inferenceDevice = L"AUTO");
    void stopAnalyzer();
    void setAutoCaptureEnabled(bool enabled);
    bool autoCaptureEnabled() const { return autoCaptureEnabled_.load(std::memory_order_acquire); }
    bool autoCapturePending() const { return autoCapturePending_.load(std::memory_order_acquire); }
    bool takeAutoCaptureEvent(AutoCaptureEvent& event);
    void setDetectionThreshold(float threshold) { detectionThreshold_.store(threshold); }
    std::wstring candidateDescription() const;
    InterestingEvent latestInterestingEvent() const;
    void submitCapturedSampleForNpu(const std::vector<float>& stereo);

private:
    static void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount);
    void pushFrames(const float* input, unsigned inputChannels, unsigned frameCount);
    void analyzerLoop();
    bool submitNpuSegment(const std::vector<float>& stereo, bool trackWindow = false);
    void loadNpuState();

    ma_context context_{};
    bool contextInitialized_ = false;
    ma_device device_{};
    bool deviceInitialized_ = false;
    std::vector<AudioSource> sources_;
    std::vector<ChannelChoice> channelChoices_;
    unsigned selectedFirstChannel_ = 0;
    unsigned selectedPairWidth_ = 2;
    ChannelMode mode_ = ChannelMode::Stereo;

    std::unique_ptr<std::atomic<uint32_t>[]> ring_;
    uint64_t capacityFrames_ = static_cast<uint64_t>(kSampleRate) * kBufferSeconds;
    std::atomic<uint64_t> writeFrame_{0};
    // Lightweight callback telemetry lets the UI distinguish a silent source
    // from a WASAPI device that opened but never delivered input buffers.
    std::atomic<uint64_t> callbackCount_{0};
    std::atomic<uint64_t> nullInputCallbackCount_{0};
    std::atomic<bool> running_{false};
    std::atomic<unsigned> actualInputChannels_{0};

    std::thread analyzerThread_;
    std::atomic<bool> analyzerStop_{false};
    mutable std::mutex analysisMutex_;
    mutable std::mutex npuMutex_;
    AnalysisSnapshot analysis_;
    TrackAccumulator trackAccumulator_;
    InterestingSectionDetector interestingDetector_;
    std::atomic<bool> autoCaptureEnabled_{true};
    std::atomic<bool> autoCapturePending_{false};
    std::atomic<bool> autoCaptureReady_{false};
    std::atomic<float> detectionThreshold_{0.66f};
    std::atomic<uint64_t> autoCaptureStartFrame_{0};
    std::atomic<uint64_t> autoCaptureEndFrame_{0};
    std::atomic<uint64_t> lastCandidateFrame_{0};
    std::atomic<uint64_t> lastTrackSampleFrame_{0};
    std::atomic<uint64_t> lastNpuTrackSubmitFrame_{0};
    mutable std::mutex autoCaptureMutex_;
    AnalysisSnapshot pendingCaptureAnalysis_;
    std::wstring pendingCaptureReason_;
    std::wstring candidateDescription_ = L"Automatic capture is ready.";
    InterestingEvent latestInterestingEvent_{};
    std::wstring appDir_;
    std::wstring inferenceDevice_ = L"AUTO";
    std::wstring npuStatus_ = L"Optional OpenVINO model not installed";
    std::wstring npuResult_ = L"Track genre consensus is waiting for analysis windows";
    std::wstring captureGenreResult_ = L"No captured-sample classification yet";
    HANDLE npuWorkerProcess_ = nullptr;
};
}
