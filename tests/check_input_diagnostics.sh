#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
grep -q 'callbackCount() const' "$ROOT/include/audio_engine.hpp"
grep -q 'nullInputCallbackCount() const' "$ROOT/include/audio_engine.hpp"
grep -q 'callbackCount_.fetch_add' "$ROOT/src/audio_engine.cpp"
grep -q 'NO AUDIO CALLBACKS' "$ROOT/src/main.cpp"
grep -q 'NO AUDIO DATA RECEIVED' "$ROOT/src/main.cpp"
grep -q 'INPUT ACTIVE · SIGNAL IS SILENT OR VERY LOW' "$ROOT/src/main.cpp"
echo 'Input callback and waveform diagnostics checks passed.'
