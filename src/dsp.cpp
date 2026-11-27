#include "dsp.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numeric>
#include <utility>

namespace scout {
namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr std::array<const char*, 12> kNoteNames = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
constexpr std::array<float, 12> kMajorProfile = {
    6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f,
    2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f
};
constexpr std::array<float, 12> kMinorProfile = {
    6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f,
    2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f
};

float db(float value) {
    return value <= 1.0e-6f ? -120.0f : 20.0f * std::log10(value);
}

float pearson(const std::array<float, 12>& x, const std::array<float, 12>& y) {
    const float mx = std::accumulate(x.begin(), x.end(), 0.0f) / 12.0f;
    const float my = std::accumulate(y.begin(), y.end(), 0.0f) / 12.0f;
    float num = 0, dx = 0, dy = 0;
    for (size_t i = 0; i < 12; ++i) {
        const float a = x[i] - mx;
        const float b = y[i] - my;
        num += a * b;
        dx += a * a;
        dy += b * b;
    }
    const float den = std::sqrt(dx * dy);
    return den > 1.0e-9f ? num / den : 0.0f;
}

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void estimateKey(const std::vector<float>& mono, unsigned sampleRate,
                 std::string& key, float& confidence) {
    // Aggregate chroma across multiple overlapping windows from the analysis segment.
    // A single 170 ms FFT is not enough to make a robust musical-key estimate.
    constexpr size_t fftSize = 16384;
    constexpr size_t hopSize = 8192;
    confidence = 0.0f;
    key = "—";
    if (mono.size() < fftSize || sampleRate == 0) return;

    std::array<double, 12> accumulated{};
    unsigned usedWindows = 0;
    std::vector<std::complex<float>> bins(fftSize);
    for (size_t offset = 0; offset + fftSize <= mono.size(); offset += hopSize) {
        double windowEnergy = 0.0;
        for (size_t i = 0; i < fftSize; ++i) {
            const float window = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(fftSize - 1));
            const float sample = mono[offset + i] * window;
            bins[i] = std::complex<float>(sample, 0.0f);
            windowEnergy += static_cast<double>(sample) * sample;
        }
        if (windowEnergy < 1.0e-7) continue;
        fft(bins);

        std::array<double, 12> chroma{};
        double totalEnergy = 0.0;
        for (size_t i = 2; i < fftSize / 2; ++i) {
            const float hz = static_cast<float>(i) * static_cast<float>(sampleRate) / static_cast<float>(fftSize);
            if (hz < 55.0f || hz > 5000.0f) continue;
            const float magnitude = std::abs(bins[i]);
            if (magnitude < 1.0e-5f) continue;
            const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
            const int note = static_cast<int>(std::lround(midi));
            const double weight = static_cast<double>(magnitude) / (1.0 + 0.003 * hz);
            chroma[static_cast<size_t>((note % 12 + 12) % 12)] += weight;
            totalEnergy += weight;
        }
        if (totalEnergy < 1.0e-3) continue;
        for (size_t i = 0; i < 12; ++i) accumulated[i] += chroma[i] / totalEnergy;
        ++usedWindows;
        if (offset + fftSize == mono.size()) break;
    }
    if (usedWindows < 3) return;

    std::array<float, 12> chroma{};
    float chromaSum = 0.0f;
    for (size_t i = 0; i < 12; ++i) {
        chroma[i] = static_cast<float>(accumulated[i] / usedWindows);
        chromaSum += chroma[i];
    }
    if (chromaSum <= 0.0f) return;
    for (float& v : chroma) v /= chromaSum;

    float best = -2.0f, second = -2.0f;
    int bestRoot = 0;
    bool bestMinor = false;
    for (int root = 0; root < 12; ++root) {
        std::array<float, 12> rotated{};
        for (int i = 0; i < 12; ++i) rotated[static_cast<size_t>(i)] = chroma[static_cast<size_t>((i + root) % 12)];
        const float major = pearson(rotated, kMajorProfile);
        const float minor = pearson(rotated, kMinorProfile);
        for (const auto& candidate : {std::pair<float, bool>{major, false}, {minor, true}}) {
            if (candidate.first > best) {
                second = best;
                best = candidate.first;
                bestRoot = root;
                bestMinor = candidate.second;
            } else if (candidate.first > second) {
                second = candidate.first;
            }
        }
    }
    if (best < 0.03f) return;
    const float margin = std::max(0.0f, best - second);
    confidence = std::clamp((best + 0.15f) * 45.0f + margin * 115.0f, 0.0f, 97.0f);
    key = std::string(kNoteNames[static_cast<size_t>(bestRoot)]) + (bestMinor ? " minor" : " major");
}

void estimateTempo(const std::vector<float>& mono, unsigned sampleRate, float& bpm, float& confidence, float& alternate) {
    bpm = 0.0f;
    confidence = 0.0f;
    alternate = 0.0f;
    if (sampleRate == 0 || mono.size() < static_cast<size_t>(sampleRate) * 6) return;

    // Spectral-flux onset envelope: unlike block-RMS differences, this responds
    // to new broadband/percussive content even when the average level barely moves.
    constexpr size_t fftSize = 1024;
    const size_t hopSize = std::max<size_t>(1, sampleRate / 100); // ~10 ms.
    const size_t half = fftSize / 2;
    std::vector<float> onset;
    onset.reserve(mono.size() / hopSize);
    std::vector<float> previousMagnitude(half, 0.0f);
    std::vector<float> currentMagnitude(half, 0.0f);
    std::vector<std::complex<float>> spectrum(fftSize);

    for (size_t offset = 0; offset + fftSize <= mono.size(); offset += hopSize) {
        for (size_t i = 0; i < fftSize; ++i) {
            const float window = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(fftSize - 1));
            spectrum[i] = {mono[offset + i] * window, 0.0f};
        }
        fft(spectrum);
        double flux = 0.0;
        double magnitudeSum = 0.0;
        std::fill(currentMagnitude.begin(), currentMagnitude.end(), 0.0f);
        for (size_t bin = 2; bin < half; ++bin) {
            // Square-root compression reduces domination by a few loud bins.
            const float magnitude = std::sqrt(std::abs(spectrum[bin]));
            currentMagnitude[bin] = magnitude;
            flux += std::max(0.0f, magnitude - previousMagnitude[bin]);
            magnitudeSum += magnitude;
        }
        onset.push_back(static_cast<float>(flux / (magnitudeSum + 1.0e-8)));
        previousMagnitude.swap(currentMagnitude);
    }
    if (onset.size() < 500) return;

    const float mean = std::accumulate(onset.begin(), onset.end(), 0.0f) / static_cast<float>(onset.size());
    for (float& value : onset) value -= mean;

    // Search a broad musical range. Preserve slow hip-hop/half-time pulses.
    constexpr int minLag = 30;  // approximately 200 BPM at 10 ms resolution.
    constexpr int maxLag = 150; // approximately 40 BPM.
    struct Peak { int lag = 0; float strength = 0.0f; float prominence = 0.0f; };
    auto findPeak = [&](size_t begin, size_t end) -> Peak {
        end = std::min(end, onset.size());
        if (end <= begin) return {};
        // Require at least four repetitions of any candidate period inside
        // each validation segment; otherwise short segments prefer spurious
        // long-lag peaks with only one or two cycles.
        const int lagMax = std::min(maxLag, static_cast<int>((end - begin) / 4));
        if (lagMax < minLag) return {};
        std::array<float, maxLag + 1> correlations{};
        float meanCorrelation = 0.0f;
        unsigned correlationCount = 0;
        for (int lag = minLag; lag <= lagMax; ++lag) {
            double dot = 0.0, normA = 0.0, normB = 0.0;
            for (size_t i = begin + static_cast<size_t>(lag); i < end; ++i) {
                const double a = onset[i];
                const double b = onset[i - static_cast<size_t>(lag)];
                dot += a * b;
                normA += a * a;
                normB += b * b;
            }
            const float corr = static_cast<float>(dot / (std::sqrt(normA * normB) + 1.0e-10));
            correlations[static_cast<size_t>(lag)] = corr;
            meanCorrelation += corr;
            ++correlationCount;
        }
        if (correlationCount) meanCorrelation /= static_cast<float>(correlationCount);

        Peak peak;
        for (int lag = minLag; lag <= lagMax; ++lag) {
            const float value = correlations[static_cast<size_t>(lag)];
            const float left = lag > minLag ? correlations[static_cast<size_t>(lag - 1)] : -1.0f;
            const float right = lag < lagMax ? correlations[static_cast<size_t>(lag + 1)] : -1.0f;
            if (value < left || value < right) continue;
            // If two metrical interpretations tie (e.g. 60/120), prefer the
            // faster grid; the displayed estimate is still clearly a tempo estimate.
            if (value > peak.strength + 1.0e-5f ||
                (std::abs(value - peak.strength) <= 1.0e-5f && (peak.lag == 0 || lag < peak.lag))) {
                peak.lag = lag;
                peak.strength = value;
            }
        }
        if (peak.lag != 0) peak.prominence = std::clamp((peak.strength - meanCorrelation - 0.025f) / 0.32f, 0.0f, 1.0f);
        return peak;
    };

    const Peak full = findPeak(0, onset.size());
    if (full.lag == 0 || full.strength < 0.10f) return;
    float candidate = 6000.0f / static_cast<float>(full.lag);
    if (candidate < 40.0f || candidate > 200.0f) return;
    // Check the half-period too. If it has almost the same beat evidence,
    // retain the faster pulse as the primary grid but report the slow reading
    // explicitly as the alternate rather than silently hiding the ambiguity.
    auto correlationForLag = [&](size_t begin, size_t end, int lag) {
        if (lag <= 0 || end <= begin + static_cast<size_t>(lag)) return 0.0f;
        double dot = 0.0, normA = 0.0, normB = 0.0;
        for (size_t i = begin + static_cast<size_t>(lag); i < end; ++i) {
            const double a = onset[i];
            const double b = onset[i - static_cast<size_t>(lag)];
            dot += a * b; normA += a * a; normB += b * b;
        }
        return static_cast<float>(dot / (std::sqrt(normA * normB) + 1.0e-10));
    };
    if (full.lag >= minLag * 2) {
        // A 10 ms onset grid quantizes some beat periods to alternating
        // integer lags (e.g. 62/63 frames at 96 BPM). Checking only floor(lag/2)
        // can miss the faster metrical interpretation and incorrectly report
        // half-time. Test neighbouring bins; a 60% supporting subharmonic is
        // exposed as the primary grid with the slower reading retained as an
        // alternate so the UI does not hide half-time ambiguity.
        int bestHalfLag = 0;
        float halfPeriodStrength = 0.0f;
        const int halfLag = full.lag / 2;
        for (int candidateLag = std::max(minLag, halfLag - 2);
             candidateLag <= std::min(maxLag, halfLag + 2); ++candidateLag) {
            const float strength = correlationForLag(0, onset.size(), candidateLag);
            if (strength > halfPeriodStrength) {
                halfPeriodStrength = strength;
                bestHalfLag = candidateLag;
            }
        }
        if (bestHalfLag != 0 && halfPeriodStrength >= full.strength * 0.60f) {
            alternate = candidate;
            candidate = 6000.0f / static_cast<float>(bestHalfLag);
        }
    }
    if (alternate == 0.0f && candidate < 90.0f && candidate * 2.0f <= 200.0f) alternate = candidate * 2.0f;
    else if (alternate == 0.0f && candidate > 150.0f && candidate * 0.5f >= 40.0f) alternate = candidate * 0.5f;

    // A reliable estimate should recur in multiple portions of the live window,
    // not just be a coincidental peak in one local passage.
    unsigned stableSegments = 0;
    constexpr unsigned segments = 3;
    for (unsigned segment = 0; segment < segments; ++segment) {
        const size_t begin = onset.size() * segment / segments;
        const size_t end = onset.size() * (segment + 1) / segments;
        const Peak local = findPeak(begin, end);
        if (local.lag == 0 || local.strength < 0.12f) continue;
        const float localTempo = 6000.0f / static_cast<float>(local.lag);
        const float directError = std::abs(localTempo - candidate) / std::max(candidate, 1.0f);
        const float halfError = std::abs(localTempo * 2.0f - candidate) / std::max(candidate, 1.0f);
        const float doubleError = std::abs(localTempo * 0.5f - candidate) / std::max(candidate, 1.0f);
        const float relativeError = std::min({directError, halfError, doubleError});
        if (relativeError <= 0.045f) ++stableSegments;
    }
    const float stability = static_cast<float>(stableSegments) / static_cast<float>(segments);
    const float periodicity = std::clamp((full.strength - 0.08f) / 0.52f, 0.0f, 1.0f);
    confidence = std::clamp(96.0f * periodicity * (0.45f + 0.55f * stability) *
                            (0.70f + 0.30f * full.prominence), 0.0f, 96.0f);
    // A whole-window autocorrelation peak can be produced by sustained tones,
    // repeated noise, or only one local phrase. Require the same tempo in at
    // least two of three independently evaluated sections.
    if (stableSegments < 2 || confidence < 25.0f) {
        confidence = std::min(confidence, 24.0f);
        return;
    }
    bpm = candidate;
}
}

AnalysisSnapshot analyzeAudio(const std::vector<float>& stereo, unsigned sampleRate) {
    AnalysisSnapshot out;
    if (stereo.size() < static_cast<size_t>(sampleRate) * 2 * 2 || sampleRate == 0) return out;
    const size_t frames = stereo.size() / 2;
    std::vector<float> mono(frames);
    double sumSquares = 0.0;
    double leftSquares = 0.0;
    double rightSquares = 0.0;
    double cross = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0;
    uint64_t silentFrames = 0;
    for (size_t i = 0; i < frames; ++i) {
        const float l = stereo[i * 2];
        const float r = stereo[i * 2 + 1];
        const float v = 0.5f * (l + r);
        mono[i] = v;
        sumSquares += static_cast<double>(v) * v;
        leftSquares += static_cast<double>(l) * l;
        rightSquares += static_cast<double>(r) * r;
        cross += static_cast<double>(l) * r;
        peak = std::max(peak, static_cast<double>(std::max(std::abs(l), std::abs(r))));
        if (std::abs(l) >= 0.999f || std::abs(r) >= 0.999f) ++clipped;
        if (std::abs(v) < 0.001f) ++silentFrames;
    }
    const double corrDenom = std::sqrt(leftSquares * rightSquares);
    out.stereoCorrelation = corrDenom > 1.0e-12 ? static_cast<float>(std::clamp(cross / corrDenom, -1.0, 1.0)) : 0.0f;
    // Avoid destructive L/R cancellation in analysis only. Original stereo is
    // preserved unchanged for every saved capture.
    if (out.stereoCorrelation < 0.15f) {
        const bool useLeft = leftSquares >= rightSquares;
        for (size_t i = 0; i < frames; ++i) mono[i] = stereo[i * 2 + (useLeft ? 0 : 1)];
        sumSquares = useLeft ? leftSquares : rightSquares;
        silentFrames = 0;
        for (float value : mono) if (std::abs(value) < 0.001f) ++silentFrames;
    }
    const float rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(frames)));
    out.rmsDb = db(rms);
    out.rmsLeftDb = db(static_cast<float>(std::sqrt(leftSquares / static_cast<double>(frames))));
    out.rmsRightDb = db(static_cast<float>(std::sqrt(rightSquares / static_cast<double>(frames))));
    out.peakDb = db(static_cast<float>(peak));
    out.clippingPercent = 100.0f * static_cast<float>(clipped) / static_cast<float>(frames * 2);
    out.crestFactorDb = std::max(0.0f, out.peakDb - out.rmsDb);
    out.silencePercent = 100.0f * static_cast<float>(silentFrames) / static_cast<float>(frames);

    constexpr size_t block = 480; // 10 ms at 48 kHz.
    std::vector<float> energy;
    energy.reserve(frames / block);
    size_t transientCount = 0;
    float previous = 0.0f;
    double centroidWeighted = 0.0;
    double centroidMagnitude = 0.0;
    for (size_t begin = 0; begin + block <= frames; begin += block) {
        double e = 0.0;
        for (size_t j = 0; j < block; ++j) e += static_cast<double>(mono[begin + j]) * mono[begin + j];
        const float current = static_cast<float>(std::sqrt(e / static_cast<double>(block)));
        energy.push_back(current);
        if (current > std::max(0.006f, previous * 1.72f) && current - previous > 0.004f) ++transientCount;
        previous = current;
    }
    const float duration = static_cast<float>(frames) / static_cast<float>(sampleRate);
    out.transientRate = duration > 0.0f ? static_cast<float>(transientCount) / duration : 0.0f;

    // Spectral centroid from a 4096-point window, independent of the key estimate.
    constexpr size_t centroidFftSize = 4096;
    if (mono.size() >= centroidFftSize) {
        std::vector<std::complex<float>> spectrum(centroidFftSize);
        const size_t start = mono.size() - centroidFftSize;
        for (size_t i = 0; i < centroidFftSize; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(centroidFftSize - 1));
            spectrum[i] = {mono[start + i] * w, 0.0f};
        }
        fft(spectrum);
        for (size_t i = 1; i < centroidFftSize / 2; ++i) {
            const double mag = std::abs(spectrum[i]);
            const double hz = static_cast<double>(i) * sampleRate / centroidFftSize;
            centroidWeighted += hz * mag;
            centroidMagnitude += mag;
        }
    }
    out.spectralCentroidHz = centroidMagnitude > 0.0 ? static_cast<float>(centroidWeighted / centroidMagnitude) : 0.0f;

    estimateTempo(mono, sampleRate, out.bpm, out.bpmConfidence, out.bpmAlternate);
    // A sustained tone can create a weak periodic spectral-flux peak from the
    // analysis window itself. Require some measurable transient activity before
    // treating that periodicity as a musical beat grid.
    if (out.transientRate < 0.20f) {
        out.bpm = 0.0f;
        out.bpmConfidence = std::min(out.bpmConfidence, 20.0f);
    }
    estimateKey(mono, sampleRate, out.key, out.keyConfidence);

    if (out.transientRate > 2.4f && out.bpmConfidence > 26.0f) {
        out.character = "Percussive · rhythmic";
        out.section = "Rhythmic passage / possible break";
    } else if (out.keyConfidence > 48.0f) {
        out.character = out.spectralCentroidHz > 2600.0f ? "Tonal · bright" : "Tonal · warm";
        out.section = "Harmonic / melodic passage";
    } else if (out.spectralCentroidHz > 3300.0f) {
        out.character = "Bright · transient-rich";
        out.section = "Textural passage";
    } else if (out.rmsDb > -52.0f) {
        out.character = "Dense · textured";
        out.section = "Audio passage";
    } else {
        out.character = "Low-level audio";
        out.section = "Waiting for a stronger passage";
    }

    const float rhythmScore = std::clamp(out.transientRate / 5.0f, 0.0f, 1.0f) * 0.45f +
                              std::clamp(out.bpmConfidence / 75.0f, 0.0f, 1.0f) * 0.35f +
                              std::clamp((out.rmsDb + 55.0f) / 35.0f, 0.0f, 1.0f) * 0.20f;
    const float tonalScore = std::clamp(out.keyConfidence / 85.0f, 0.0f, 1.0f) * 0.7f +
                             std::clamp((out.rmsDb + 60.0f) / 40.0f, 0.0f, 1.0f) * 0.3f;
    out.interestingScore = std::max(rhythmScore, tonalScore);
    if (out.clippingPercent > 1.0f) out.character += " · clipping risk";
    return out;
}

float sectionNoveltyScore(const AnalysisSnapshot& previous, const AnalysisSnapshot& recent) {
    if (recent.rmsDb <= -58.0f) return 0.0f;
    // Section changes are bidirectional: a breakdown, dropout, or sparse
    // passage can be as useful as a build or percussion entrance.
    const float energyDelta = std::abs(recent.rmsDb - previous.rmsDb);
    const float energyChange = std::clamp((energyDelta - 1.0f) / 7.0f, 0.0f, 1.0f);
    const float transientDelta = std::abs(recent.transientRate - previous.transientRate);
    const float transientChange = std::clamp((transientDelta - 0.25f) / 1.8f, 0.0f, 1.0f);
    const float oldCentroid = std::max(previous.spectralCentroidHz, 100.0f);
    const float newCentroid = std::max(recent.spectralCentroidHz, 100.0f);
    const float centroidRatio = std::max(newCentroid / oldCentroid, oldCentroid / newCentroid);
    const float spectralChange = std::clamp((centroidRatio - 1.15f) / 0.9f, 0.0f, 1.0f);
    const float keyFitChange = std::abs(recent.keyConfidence - previous.keyConfidence);
    const bool changedTonalCenter = previous.key != "—" && recent.key != "—" &&
        previous.key != recent.key && previous.keyConfidence >= 30.0f && recent.keyConfidence >= 30.0f;
    const float tonalChange = std::max(
        std::clamp((keyFitChange - 10.0f) / 45.0f, 0.0f, 1.0f),
        changedTonalCenter ? 1.0f : 0.0f);
    return std::clamp(std::max({
        0.52f * transientChange + 0.30f * energyChange + 0.18f * spectralChange,
        0.52f * spectralChange + 0.28f * energyChange + 0.20f * transientChange,
        0.60f * tonalChange + 0.25f * energyChange + 0.15f * spectralChange
    }), 0.0f, 1.0f);
}


void TrackAccumulator::reset() {
    windows_.clear();
}

void TrackAccumulator::add(const AnalysisSnapshot& snapshot) {
    windows_.push_back(snapshot);
    // Recent windows represent the current record/track being auditioned.
    constexpr size_t kMaximumWindows = 300; // 50 minutes at one consensus vote every ten seconds.
    if (windows_.size() > kMaximumWindows)
        windows_.erase(windows_.begin(), windows_.begin() + static_cast<std::ptrdiff_t>(windows_.size() - kMaximumWindows));
}

TrackSummary TrackAccumulator::summary() const {
    TrackSummary out;
    out.windows = static_cast<unsigned>(windows_.size());
    if (windows_.empty()) return out;

    struct TempoVote { float bpm; float strength; };
    std::vector<TempoVote> tempos;
    for (const auto& w : windows_) {
        if (w.bpm >= 40.0f && w.bpm <= 200.0f && w.bpmConfidence >= 25.0f)
            tempos.push_back({w.bpm, w.bpmConfidence});
    }
    out.bpmObservations = static_cast<unsigned>(tempos.size());
    if (!tempos.empty()) {
        double bestWeight = -1.0;
        size_t bestCount = 0;
        float bestAnchor = 0.0f;
        for (const auto& anchor : tempos) {
            double weight = 0.0;
            size_t count = 0;
            for (const auto& candidate : tempos) {
                const float tolerance = std::max(2.5f, anchor.bpm * 0.035f);
                if (std::abs(candidate.bpm - anchor.bpm) <= tolerance) {
                    weight += 0.25 + static_cast<double>(candidate.strength) / 100.0;
                    ++count;
                }
            }
            if (weight > bestWeight || (std::abs(weight - bestWeight) < 1.0e-8 && count > bestCount)) {
                bestWeight = weight;
                bestCount = count;
                bestAnchor = anchor.bpm;
            }
        }
        double weightedBpm = 0.0, totalWeight = 0.0, strengthSum = 0.0;
        for (const auto& candidate : tempos) {
            const float tolerance = std::max(2.5f, bestAnchor * 0.035f);
            if (std::abs(candidate.bpm - bestAnchor) <= tolerance) {
                const double weight = 0.25 + static_cast<double>(candidate.strength) / 100.0;
                weightedBpm += candidate.bpm * weight;
                totalWeight += weight;
                strengthSum += candidate.strength;
                ++out.bpmVotes;
            }
        }
        if (totalWeight > 0.0) out.bpm = static_cast<float>(weightedBpm / totalWeight);
        out.bpmAgreement = 100.0f * static_cast<float>(out.bpmVotes) / static_cast<float>(out.bpmObservations);
        out.bpmStrength = out.bpmVotes ? static_cast<float>(strengthSum / out.bpmVotes) : 0.0f;
    }

    std::vector<std::pair<std::string, double>> keyWeights;
    for (const auto& w : windows_) {
        if (w.key == "—" || w.key.empty() || w.keyConfidence < 25.0f) continue;
        ++out.keyObservations;
        auto weight = std::find_if(keyWeights.begin(), keyWeights.end(), [&](const auto& v) { return v.first == w.key; });
        if (weight == keyWeights.end()) keyWeights.emplace_back(w.key, 0.25 + w.keyConfidence / 100.0);
        else weight->second += 0.25 + w.keyConfidence / 100.0;
    }
    if (!keyWeights.empty()) {
        const auto best = std::max_element(keyWeights.begin(), keyWeights.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        out.key = best->first;
        out.keyStrength = 0.0f;
        for (const auto& w : windows_) {
            if (w.key == out.key && w.key != "—" && w.keyConfidence >= 25.0f) {
                out.keyStrength += w.keyConfidence;
                ++out.keyVotes;
            }
        }
        out.keyStrength = out.keyVotes ? out.keyStrength / out.keyVotes : 0.0f;
        out.keyAgreement = out.keyObservations ? 100.0f * static_cast<float>(out.keyVotes) / static_cast<float>(out.keyObservations) : 0.0f;
    }
    return out;
}

}
