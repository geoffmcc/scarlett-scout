#pragma once
#include "dsp.hpp"
#include <cstdint>
#include <deque>
#include <string>

namespace scout {

enum class InterestEventType {
    None,
    Breakdown,
    BuildUp,
    Drop,
    Transition,
    PercussionEntrance,
    RhythmicChange,
    EnergyIncrease,
    EnergyDecrease,
    InstrumentalTexture,
    SpectralEvent,
    SilenceReentry
};

struct InterestingEvent {
    bool triggered = false;
    InterestEventType type = InterestEventType::None;
    uint64_t startFrame = 0;
    uint64_t peakFrame = 0;
    float score = 0.0f;
    float confidence = 0.0f;
    std::string explanation;
};

const char* interestEventTypeName(InterestEventType type);

class InterestingSectionDetector {
public:
    struct Config {
        float threshold = 0.66f;
        unsigned persistence = 2;
        unsigned cooldownSeconds = 45;
        unsigned baselineWindows = 12;
    };

    InterestingSectionDetector();
    void reset();
    void setConfig(const Config& config);
    InterestingEvent update(const AnalysisSnapshot& snapshot, uint64_t frame, unsigned sampleRate);

private:
    struct HistoryItem {
        AnalysisSnapshot snapshot;
        uint64_t frame = 0;
    };

    Config config_;
    std::deque<HistoryItem> history_;
    unsigned consecutiveHigh_ = 0;
    uint64_t lastEventFrame_ = 0;
    uint64_t pendingStartFrame_ = 0;

    float medianMetric(float HistoryItem::*member) const;
    float robustDeviation(float HistoryItem::*member, float median) const;
};

}
