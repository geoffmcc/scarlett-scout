#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace scout {
struct AnalysisSnapshot {
    float rmsDb = -120.0f;
    float rmsLeftDb = -120.0f;
    float rmsRightDb = -120.0f;
    float peakDb = -120.0f;
    float clippingPercent = 0.0f;
    float bpm = 0.0f;
    float bpmConfidence = 0.0f;
    float keyConfidence = 0.0f;
    float transientRate = 0.0f;
    float spectralCentroidHz = 0.0f;
    float interestingScore = 0.0f;
    float sectionNovelty = 0.0f;
    float bpmAlternate = 0.0f;
    float stereoCorrelation = 0.0f;
    float crestFactorDb = 0.0f;
    float silencePercent = 0.0f;
    std::string key = "—";
    std::string character = "Waiting for audio";
    std::string section = "No section detected";
};

struct TrackSummary {
    unsigned windows = 0;
    float bpm = 0.0f;
    float bpmAgreement = 0.0f;
    float bpmStrength = 0.0f;
    unsigned bpmVotes = 0;
    unsigned bpmObservations = 0;
    std::string key = "—";
    float keyAgreement = 0.0f;
    float keyStrength = 0.0f;
    unsigned keyVotes = 0;
    unsigned keyObservations = 0;
};

class TrackAccumulator {
public:
    void reset();
    void add(const AnalysisSnapshot& snapshot);
    TrackSummary summary() const;
private:
    std::vector<AnalysisSnapshot> windows_;
};

AnalysisSnapshot analyzeAudio(const std::vector<float>& interleavedStereo, unsigned sampleRate);
float sectionNoveltyScore(const AnalysisSnapshot& previous, const AnalysisSnapshot& recent);
}
