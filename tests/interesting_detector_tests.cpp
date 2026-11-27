#include "interesting_detector.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace scout;

static AnalysisSnapshot steady(float rms, float transients, float centroid, float bpm, float bpmConf,
                               const char* key, float keyConf, float stereo = 1.0f, float silence = 0.0f) {
    AnalysisSnapshot s;
    s.rmsDb = rms;
    s.rmsLeftDb = rms;
    s.rmsRightDb = rms;
    s.transientRate = transients;
    s.spectralCentroidHz = centroid;
    s.bpm = bpm;
    s.bpmConfidence = bpmConf;
    s.key = key;
    s.keyConfidence = keyConf;
    s.stereoCorrelation = stereo;
    s.silencePercent = silence;
    s.sectionNovelty = 0.0f;
    return s;
}

int main() {
    InterestingSectionDetector d;
    InterestingSectionDetector::Config cfg;
    cfg.threshold = 0.55f;
    cfg.persistence = 2;
    cfg.cooldownSeconds = 20;
    d.setConfig(cfg);

    uint64_t frame = 0;
    for (int i = 0; i < 8; ++i) {
        auto s = steady(-24.0f, 1.0f, 1200.0f, 96.0f, 70.0f, "C major", 65.0f);
        assert(!d.update(s, frame, 48000).triggered);
        frame += 240000;
    }

    auto a = steady(-18.0f, 5.0f, 3200.0f, 128.0f, 75.0f, "F major", 70.0f);
    assert(!d.update(a, frame, 48000).triggered);
    frame += 240000;
    auto b = d.update(a, frame, 48000);
    assert(b.triggered);
    assert(b.score >= cfg.threshold);
    assert(b.confidence > 0.0f);
    assert(b.type != InterestEventType::None);

    frame += 240000;
    auto c = d.update(a, frame, 48000);
    assert(!c.triggered); // cooldown

    d.reset();
    frame = 0;
    for (int i = 0; i < 8; ++i) {
        auto s = steady(-24.0f, 1.0f, 1200.0f, 96.0f, 70.0f, "C major", 65.0f);
        s.silencePercent = 5.0f;
        assert(!d.update(s, frame, 48000).triggered);
        frame += 240000;
    }
    auto reentry = steady(-22.0f, 3.0f, 1800.0f, 96.0f, 70.0f, "C major", 65.0f);
    reentry.silencePercent = 60.0f;
    frame += 240000;
    assert(!d.update(reentry, frame, 48000).triggered);
    frame += 240000;
    reentry.silencePercent = 5.0f;
    auto e = d.update(reentry, frame, 48000);
    assert(!e.triggered); // first qualifying window; persistence is 2
    frame += 240000;
    auto f = d.update(reentry, frame, 48000);
    assert(f.triggered);
    assert(f.score >= cfg.threshold);
    assert(f.type == InterestEventType::SilenceReentry || f.type == InterestEventType::EnergyIncrease);

    std::cout << "interesting_detector_tests: PASS\n";
    return 0;
}
