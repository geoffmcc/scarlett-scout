#include "interesting_detector.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <vector>

namespace scout {
namespace {
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float relativeChange(float current, float baseline) {
    if (baseline <= 1.0e-5f) return current > 1.0e-5f ? 1.0f : 0.0f;
    return std::abs(current - baseline) / std::max(std::abs(baseline), 1.0e-5f);
}

float robustRise(float current, float baseline, float scale) {
    return clamp01((current - baseline) / std::max(scale, 0.001f));
}

std::string joinReasons(const std::vector<std::string>& reasons) {
    if (reasons.empty()) return "Multiple musical signals changed";
    std::ostringstream out;
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i) out << " · ";
        out << reasons[i];
    }
    return out.str();
}
}

const char* interestEventTypeName(InterestEventType type) {
    switch (type) {
    case InterestEventType::Breakdown: return "Breakdown";
    case InterestEventType::BuildUp: return "Build-up";
    case InterestEventType::Drop: return "Drop";
    case InterestEventType::Transition: return "Transition";
    case InterestEventType::PercussionEntrance: return "Percussion entrance";
    case InterestEventType::RhythmicChange: return "Rhythmic change";
    case InterestEventType::EnergyIncrease: return "Energy increase";
    case InterestEventType::EnergyDecrease: return "Energy decrease";
    case InterestEventType::InstrumentalTexture: return "Instrumental / textural change";
    case InterestEventType::SpectralEvent: return "Spectral event";
    case InterestEventType::SilenceReentry: return "Silence / re-entry";
    default: return "Interesting passage";
    }
}

InterestingSectionDetector::InterestingSectionDetector() { reset(); }

void InterestingSectionDetector::reset() {
    history_.clear();
    consecutiveHigh_ = 0;
    lastEventFrame_ = 0;
    pendingStartFrame_ = 0;
}

void InterestingSectionDetector::setConfig(const Config& config) {
    config_ = config;
    config_.threshold = std::clamp(config_.threshold, 0.45f, 0.90f);
    config_.persistence = std::clamp(config_.persistence, 1u, 4u);
    config_.cooldownSeconds = std::clamp(config_.cooldownSeconds, 10u, 180u);
    config_.baselineWindows = std::clamp(config_.baselineWindows, 6u, 30u);
}

float InterestingSectionDetector::medianMetric(float AnalysisSnapshot::*member) const {
    if (history_.empty()) return 0.0f;
    std::vector<float> values;
    values.reserve(history_.size());
    for (const auto& item : history_) values.push_back(item.snapshot.*member);
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    float median = values[mid];
    if (values.size() % 2 == 0) {
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1), values.begin() + static_cast<std::ptrdiff_t>(mid));
        median = 0.5f * (median + values[mid - 1]);
    }
    return median;
}

float InterestingSectionDetector::robustDeviation(float AnalysisSnapshot::*member, float median) const {
    if (history_.size() < 2) return 0.0f;
    std::vector<float> deviations;
    deviations.reserve(history_.size());
    for (const auto& item : history_) deviations.push_back(std::abs(item.snapshot.*member - median));
    const size_t mid = deviations.size() / 2;
    std::nth_element(deviations.begin(), deviations.begin() + static_cast<std::ptrdiff_t>(mid), deviations.end());
    float mad = deviations[mid];
    if (deviations.size() % 2 == 0) {
        std::nth_element(deviations.begin(), deviations.begin() + static_cast<std::ptrdiff_t>(mid - 1), deviations.begin() + static_cast<std::ptrdiff_t>(mid));
        mad = 0.5f * (mad + deviations[mid - 1]);
    }
    return mad;
}

InterestingEvent InterestingSectionDetector::update(const AnalysisSnapshot& s, uint64_t frame, unsigned sampleRate) {
    InterestingEvent event;
    if (sampleRate == 0) return event;

    const bool usableAudio = s.rmsDb > -58.0f || s.silencePercent < 92.0f;
    if (!usableAudio) {
        ++consecutiveHigh_;
        if (history_.size() >= 4 && s.silencePercent > 88.0f) {
            // Silence itself is not interesting enough to save. Re-entry will
            // be scored when actual audio returns.
            consecutiveHigh_ = 0;
        }
        history_.push_back({s, frame});
        while (history_.size() > config_.baselineWindows) history_.pop_front();
        return event;
    }

    if (history_.size() < 4) {
        history_.push_back({s, frame});
        while (history_.size() > config_.baselineWindows) history_.pop_front();
        return event;
    }

    const float energyBase = medianMetric(&AnalysisSnapshot::rmsDb);
    const float transientBase = medianMetric(&AnalysisSnapshot::transientRate);
    const float centroidBase = medianMetric(&AnalysisSnapshot::spectralCentroidHz);
    const float stereoBase = medianMetric(&AnalysisSnapshot::stereoCorrelation);
    const float silenceBase = medianMetric(&AnalysisSnapshot::silencePercent);

    const float energyMad = robustDeviation(&AnalysisSnapshot::rmsDb, energyBase);
    const float transientMad = robustDeviation(&AnalysisSnapshot::transientRate, transientBase);
    const float centroidMad = robustDeviation(&AnalysisSnapshot::spectralCentroidHz, centroidBase);
    const float stereoMad = robustDeviation(&AnalysisSnapshot::stereoCorrelation, stereoBase);

    // Correlated level/transient changes are intentionally one evidence group.
    // This prevents a loud drum hit from counting twice as independent evidence.
    const float energySignal = std::max(
        robustRise(s.rmsDb, energyBase, std::max(2.0f, 2.5f * energyMad)),
        robustRise(s.transientRate, transientBase, std::max(0.7f, 2.5f * transientMad)));

    const float spectralSignal = std::max(
        robustRise(s.spectralCentroidHz, centroidBase, std::max(250.0f, 3.0f * centroidMad)),
        clamp01(relativeChange(s.spectralCentroidHz, centroidBase) / 0.45f));

    float rhythmSignal = 0.0f;
    if (s.bpm > 0.0f && s.bpmConfidence >= 30.0f) {
        float priorBpm = 0.0f;
        float priorConfidence = 0.0f;
        unsigned count = 0;
        for (const auto& item : history_) {
            if (item.snapshot.bpm > 0.0f && item.snapshot.bpmConfidence >= 30.0f) {
                priorBpm += item.snapshot.bpm;
                priorConfidence += item.snapshot.bpmConfidence;
                ++count;
            }
        }
        if (count) {
            priorBpm /= static_cast<float>(count);
            priorConfidence /= static_cast<float>(count);
            const float direct = std::abs(s.bpm - priorBpm) / std::max(priorBpm, 1.0f);
            const float half = std::abs(s.bpm * 2.0f - priorBpm) / std::max(priorBpm, 1.0f);
            const float doubled = std::abs(s.bpm * 0.5f - priorBpm) / std::max(priorBpm, 1.0f);
            const float bpmChange = std::min({direct, half, doubled});
            rhythmSignal = clamp01((bpmChange - 0.045f) / 0.14f) *
                           clamp01(s.bpmConfidence / 65.0f) *
                           clamp01((priorConfidence + s.bpmConfidence) / 120.0f);
        }
    }

    float tonalSignal = 0.0f;
    if (s.key != "—" && s.keyConfidence >= 30.0f) {
        unsigned supported = 0;
        std::string priorKey;
        for (const auto& item : history_) {
            if (item.snapshot.key != "—" && item.snapshot.keyConfidence >= 30.0f) {
                ++supported;
                if (priorKey.empty()) priorKey = item.snapshot.key;
            }
        }
        bool changedKey = false;
        if (supported >= 2) {
            unsigned same = 0;
            for (const auto& item : history_) {
                if (item.snapshot.key == priorKey && item.snapshot.keyConfidence >= 30.0f) ++same;
            }
            if (same >= supported / 2 + 1 && s.key != priorKey) changedKey = true;
        }
        tonalSignal = changedKey ? clamp01((s.keyConfidence - 25.0f) / 55.0f) : 0.0f;
    }

    const float stereoSignal = clamp01(std::abs(s.stereoCorrelation - stereoBase) /
                                       std::max(0.18f, 3.0f * stereoMad));
    float highestRecentSilence = silenceBase;
    for (const auto& item : history_) highestRecentSilence = std::max(highestRecentSilence, item.snapshot.silencePercent);
    const bool silenceReentry = highestRecentSilence >= 55.0f && s.silencePercent <= 20.0f;
    const float silenceSignal = silenceReentry
        ? clamp01((highestRecentSilence - s.silencePercent - 25.0f) / 45.0f)
        : ((silenceBase > 25.0f && s.silencePercent < silenceBase - 18.0f)
            ? clamp01((silenceBase - s.silencePercent - 12.0f) / 45.0f)
            : 0.0f);

    // Missing/weak measurements do not contribute evidence. Weights are
    // renormalized over available signal groups.
    float weighted = 0.0f;
    float totalWeight = 0.0f;
    auto add = [&](float value, float weight, bool available) {
        if (available) { weighted += value * weight; totalWeight += weight; }
    };
    add(energySignal, 0.28f, s.rmsDb > -90.0f && std::isfinite(s.rmsDb));
    add(spectralSignal, 0.22f, s.spectralCentroidHz > 80.0f && std::isfinite(s.spectralCentroidHz));
    add(rhythmSignal, 0.18f, s.bpmConfidence >= 30.0f);
    add(tonalSignal, 0.12f, s.keyConfidence >= 30.0f);
    add(stereoSignal, 0.08f, std::isfinite(s.stereoCorrelation));
    add(silenceSignal, 0.12f, std::isfinite(s.silencePercent));
    const float rawScore = totalWeight > 0.0f ? weighted / totalWeight : 0.0f;

    // Existing section novelty is retained as a supporting signal, not a
    // duplicate vote for level/transient/spectral changes.
    const float blendedScore = clamp01(0.72f * rawScore + 0.28f * clamp01(s.sectionNovelty));
    // A clear return from a genuinely quiet passage is a structural event in
    // its own right. Do not let unchanged BPM/key measurements dilute it.
    const float score = std::max(blendedScore, silenceReentry ? 0.70f * silenceSignal : 0.0f);

    std::vector<std::string> reasons;
    if (energySignal >= 0.55f) reasons.push_back(s.rmsDb > energyBase ? "energy rises" : "energy falls");
    if (spectralSignal >= 0.55f) reasons.push_back("spectral texture changes");
    if (rhythmSignal >= 0.55f) reasons.push_back("rhythmic/tempo behavior changes");
    if (tonalSignal >= 0.55f) reasons.push_back("tonal center changes");
    if (stereoSignal >= 0.60f) reasons.push_back("stereo image changes");
    if (silenceSignal >= 0.55f) reasons.push_back("audio re-enters after a quiet passage");

    if (score >= config_.threshold) {
        if (pendingStartFrame_ == 0) pendingStartFrame_ = frame;
        ++consecutiveHigh_;
    } else {
        consecutiveHigh_ = 0;
        pendingStartFrame_ = 0;
    }

    const uint64_t cooldown = static_cast<uint64_t>(config_.cooldownSeconds) * sampleRate;
    const bool cooldownPassed = lastEventFrame_ == 0 || frame > lastEventFrame_ + cooldown;
    if (cooldownPassed && consecutiveHigh_ >= config_.persistence) {
        event.triggered = true;
        event.startFrame = pendingStartFrame_ > static_cast<uint64_t>(sampleRate) * 5
            ? pendingStartFrame_ - static_cast<uint64_t>(sampleRate) * 5 : 0;
        event.peakFrame = frame;
        event.score = score;
        event.confidence = clamp01((score - config_.threshold) / std::max(0.05f, 1.0f - config_.threshold)) * 55.0f +
                           clamp01(static_cast<float>(consecutiveHigh_) / 4.0f) * 30.0f +
                           clamp01((totalWeight - 0.35f) / 0.65f) * 15.0f;
        event.confidence = std::clamp(event.confidence, 0.0f, 100.0f);

        if (silenceSignal >= 0.55f) event.type = InterestEventType::SilenceReentry;
        else if (energySignal >= 0.65f && transientBase > 0.0f && s.transientRate > transientBase * 1.7f)
            event.type = InterestEventType::PercussionEntrance;
        else if (energySignal >= 0.75f && s.rmsDb > energyBase + 4.0f &&
                 transientBase > 0.0f && s.transientRate > transientBase * 1.8f)
            event.type = InterestEventType::Drop;
        else if (energySignal >= 0.70f && s.rmsDb > energyBase + 3.0f)
            event.type = InterestEventType::BuildUp;
        else if (energySignal >= 0.70f && s.rmsDb < energyBase - 3.0f)
            event.type = InterestEventType::Breakdown;
        else if (rhythmSignal >= 0.60f) event.type = InterestEventType::RhythmicChange;
        else if (tonalSignal >= 0.60f) event.type = InterestEventType::Transition;
        else if (spectralSignal >= 0.60f) event.type = InterestEventType::InstrumentalTexture;
        else if (stereoSignal >= 0.65f) event.type = InterestEventType::SpectralEvent;
        else if (s.rmsDb > energyBase) event.type = InterestEventType::EnergyIncrease;
        else event.type = InterestEventType::EnergyDecrease;

        event.explanation = joinReasons(reasons);
        lastEventFrame_ = frame;
        consecutiveHigh_ = 0;
        pendingStartFrame_ = 0;
    }

    history_.push_back({s, frame});
    while (history_.size() > config_.baselineWindows) history_.pop_front();
    return event;
}

}
