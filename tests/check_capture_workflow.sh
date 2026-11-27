#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/src/audio_engine.cpp"
GUI="$ROOT/src/main.cpp"
require() { grep -Fq -- "$1" "$2" || { echo "FAIL: missing capture-workflow requirement: $1" >&2; exit 1; }; }
require 'constexpr unsigned kAnalysisSeconds = 18;' "$ENGINE"
require 'constexpr unsigned kAnalysisIntervalSeconds = 5;' "$ENGINE"
require 'constexpr unsigned kTrackInferenceIntervalSeconds = 15;' "$ENGINE"
require 'trackAccumulator_.add(snapshot)' "$ENGINE"
require 'submitNpuSegment(samples, true)' "$ENGINE"
require 'constexpr unsigned kAutoCapturePreRollSeconds = 18;' "$ENGINE"
require 'constexpr unsigned kAutoCapturePostRollSeconds = 12;' "$ENGINE"
require 'constexpr unsigned kAutoCaptureCooldownSeconds = 45;' "$ENGINE"
require 'void AudioEngine::setAutoCaptureEnabled(bool enabled)' "$ENGINE"
require 'bool AudioEngine::takeAutoCaptureEvent(AutoCaptureEvent& event)' "$ENGINE"
require 'audio.copyFrameRange(event.startFrame, event.endFrame)' "$GUI"
require 'audio.submitCapturedSampleForNpu(stereo)' "$GUI"
require 'L"capture-"' "$ENGINE"
require 'rfind(L"capture-", 0)' "$ROOT/src/npu_worker.cpp"
require 'rfind(L"track-", 0)' "$ROOT/src/npu_worker.cpp"
require 'trackScoreSums' "$ROOT/src/npu_worker.cpp"
require 'Key-uncertain' "$GUI"
require 'filenameToken(reason.empty() ? L"Manual-capture" : reason)' "$GUI"
require 'Auto-saves 30-second clips' "$GUI"
if grep -Eq 'ID_CANDIDATE_IGNORE|ID_CANDIDATE_CAPTURE|CAPTURE SECTION' "$GUI"; then
  echo 'FAIL: prompt/ignore workflow still exists in the GUI.' >&2
  exit 1
fi
if grep -Fq 'lastSubmit + static_cast<uint64_t>(12) * kSampleRate' "$ENGINE"; then
  echo 'FAIL: NPU should classify saved captures, not unrelated rolling windows.' >&2
  exit 1
fi
require 'trackWinnerCounts' "$ROOT/src/npu_worker.cpp"
require 'TRACK_UNCERTAIN|' "$ROOT/src/npu_worker.cpp"
require 'trackWindowCount >= 4' "$ROOT/src/npu_worker.cpp"
require 'Auto-saves 30-second clips' "$GUI"
echo 'Automatic capture workflow checks passed: full-window + post-roll, auto-save, keyed/reasoned names, capture-linked NPU, no Ignore prompt.'
