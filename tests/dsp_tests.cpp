#include "dsp.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kRate = 48000;

bool testTempoAndTransients() {
    constexpr unsigned seconds = 8;
    std::vector<float> stereo(static_cast<size_t>(kRate) * seconds * 2, 0.0f);
    for (unsigned n = 0; n < kRate * seconds; ++n) {
        const float tone = 0.05f * std::sin(static_cast<float>(2.0 * kPi * 440.0 * n / kRate));
        const unsigned phase = n % (kRate / 2);
        const float click = phase < 240 ? 0.45f * std::exp(-static_cast<float>(phase) / 45.0f) : 0.0f;
        const float v = tone + click;
        stereo[static_cast<size_t>(n) * 2] = v;
        stereo[static_cast<size_t>(n) * 2 + 1] = v;
    }
    const auto result = scout::analyzeAudio(stereo, kRate);
    if (result.rmsDb <= -120.0f || result.peakDb <= -30.0f) {
        std::cerr << "Level analysis did not detect the synthetic signal\n";
        return false;
    }
    if (result.bpm <= 0.0f || std::abs(result.bpm - 120.0f) > 8.0f) {
        std::cerr << "Tempo estimate outside expected range: " << result.bpm << " BPM\n";
        return false;
    }
    if (result.transientRate < 1.0f) {
        std::cerr << "Transient detector did not identify the pulse train\n";
        return false;
    }
    return true;
}


bool testSteadyNinetySixBpm() {
    constexpr unsigned seconds = 12;
    std::vector<float> stereo(static_cast<size_t>(kRate) * seconds * 2, 0.0f);
    constexpr unsigned beatFrames = 30000; // 96 BPM at 48 kHz.
    for (unsigned n = 0; n < kRate * seconds; ++n) {
        const unsigned phase = n % beatFrames;
        const float click = phase < 240 ? 0.42f * std::exp(-static_cast<float>(phase) / 42.0f) : 0.0f;
        const float bed = 0.025f * std::sin(static_cast<float>(2.0 * kPi * 110.0 * n / kRate));
        stereo[static_cast<size_t>(n) * 2] = bed + click;
        stereo[static_cast<size_t>(n) * 2 + 1] = bed + click;
    }
    const auto result = scout::analyzeAudio(stereo, kRate);
    if (result.bpm <= 0.0f || std::abs(result.bpm - 96.0f) > 5.0f || result.bpmConfidence < 25.0f) {
        std::cerr << "Stable 96 BPM test failed: " << result.bpm << " BPM, strength " << result.bpmConfidence << "\n";
        return false;
    }
    return true;
}

bool testSustainedToneDoesNotInventTempo() {
    constexpr unsigned seconds = 18;
    std::vector<float> stereo(static_cast<size_t>(kRate) * seconds * 2, 0.0f);
    for (unsigned n = 0; n < kRate * seconds; ++n) {
        const float tone = 0.08f * std::sin(static_cast<float>(2.0 * kPi * 440.0 * n / kRate));
        stereo[static_cast<size_t>(n) * 2] = tone;
        stereo[static_cast<size_t>(n) * 2 + 1] = tone;
    }
    const auto result = scout::analyzeAudio(stereo, kRate);
    if (result.bpm != 0.0f || result.bpmConfidence >= 25.0f) {
        std::cerr << "Sustained tone was assigned a tempo: " << result.bpm << " BPM, strength " << result.bpmConfidence << "\n";
        return false;
    }
    return true;
}

bool testKeyAcrossMultipleWindows() {
    constexpr unsigned seconds = 8;
    std::vector<float> stereo(static_cast<size_t>(kRate) * seconds * 2, 0.0f);
    const double frequencies[] = {261.6256, 329.6276, 391.9954}; // C major triad.
    for (unsigned n = 0; n < kRate * seconds; ++n) {
        const double t = static_cast<double>(n) / kRate;
        float v = 0.0f;
        for (double f : frequencies) v += static_cast<float>(0.07 * std::sin(2.0 * kPi * f * t));
        stereo[static_cast<size_t>(n) * 2] = v;
        stereo[static_cast<size_t>(n) * 2 + 1] = v;
    }
    const auto result = scout::analyzeAudio(stereo, kRate);
    if (result.key != "C major" || result.keyConfidence < 35.0f) {
        std::cerr << "Multi-window key analysis failed: " << result.key << " (" << result.keyConfidence << "%)\n";
        return false;
    }
    return true;
}

bool testTrackConsensusIgnoresWeakKeyVotes() {
    scout::TrackAccumulator accumulator;
    for (int i = 0; i < 4; ++i) {
        scout::AnalysisSnapshot s;
        s.key = "F minor";
        s.keyConfidence = 65.0f;
        s.bpm = 92.0f;
        s.bpmConfidence = 65.0f;
        accumulator.add(s);
    }
    scout::AnalysisSnapshot weak;
    weak.key = "C major";
    weak.keyConfidence = 12.0f; // should not count as a usable key observation
    accumulator.add(weak);
    const auto summary = accumulator.summary();
    if (summary.key != "F minor" || summary.keyVotes != 4 || summary.keyObservations != 4 ||
        summary.keyAgreement < 99.0f || summary.bpmVotes != 4) {
        std::cerr << "Track consensus included a weak key vote or lost stable evidence\n";
        return false;
    }
    return true;
}

bool testSectionNoveltyDetectsMeaningfulChange() {
    scout::AnalysisSnapshot previous;
    previous.rmsDb = -29.0f;
    previous.transientRate = 0.5f;
    previous.spectralCentroidHz = 650.0f;
    previous.key = "C major";
    previous.keyConfidence = 72.0f;

    scout::AnalysisSnapshot unchanged = previous;
    if (scout::sectionNoveltyScore(previous, unchanged) > 0.10f) {
        std::cerr << "Unchanged audio was assigned high section novelty\n";
        return false;
    }

    scout::AnalysisSnapshot changed = previous;
    changed.rmsDb = -26.0f;
    changed.transientRate = 5.2f;
    changed.spectralCentroidHz = 3200.0f;
    changed.key = "D minor";
    changed.keyConfidence = 68.0f;
    if (scout::sectionNoveltyScore(previous, changed) < 0.55f) {
        std::cerr << "A strong musical/spectral section change was not detected\n";
        return false;
    }
    return true;
}

bool testIndependentStereoMeters() {
    constexpr unsigned seconds = 2;
    std::vector<float> stereo(static_cast<size_t>(kRate) * seconds * 2, 0.0f);
    for (unsigned n = 0; n < kRate * seconds; ++n) {
        const double t = static_cast<double>(n) / kRate;
        stereo[static_cast<size_t>(n) * 2] = static_cast<float>(0.10 * std::sin(2.0 * kPi * 220.0 * t));
        stereo[static_cast<size_t>(n) * 2 + 1] = static_cast<float>(0.20 * std::sin(2.0 * kPi * 220.0 * t));
    }
    const auto result = scout::analyzeAudio(stereo, kRate);
    const float difference = result.rmsRightDb - result.rmsLeftDb;
    if (difference < 5.5f || difference > 6.5f) {
        std::cerr << "Independent stereo meter test failed: L=" << result.rmsLeftDb
                  << " dBFS R=" << result.rmsRightDb << " dBFS\n";
        return false;
    }
    return true;
}
}

int main() {
    if (!testTempoAndTransients()) return 1;
    if (!testSteadyNinetySixBpm()) return 2;
    if (!testSustainedToneDoesNotInventTempo()) return 3;
    if (!testKeyAcrossMultipleWindows()) return 4;
    if (!testIndependentStereoMeters()) return 5;
    if (!testTrackConsensusIgnoresWeakKeyVotes()) return 6;
    if (!testSectionNoveltyDetectsMeaningfulChange()) return 7;
    std::cout << "DSP regression tests passed: 120/96 BPM, false-tempo rejection, multi-window key, independent stereo RMS, confidence-qualified track consensus, section novelty.\n";
    return 0;
}
