#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
GUI="$ROOT/src/main.cpp"
WORKER="$ROOT/src/npu_worker.cpp"
require() { grep -Fq -- "$1" "$2" || { echo "FAIL: missing Windows encoding/function binding requirement: $1" >&2; exit 1; }; }
require 'static std::wstring utf8ToWide(const std::string& value)' "$GUI"
require '<< L"Track consensus key: " << utf8ToWide(track.key)' "$GUI"
require 'utf8ToWide(namingAnalysis.key)' "$GUI"
require 'utf8ToWide(shownKey)' "$GUI"
if grep -Eq 'std::wstring\([^)]*\.begin\(\),[^)]*\.end\(\)\)' "$GUI"; then
  echo 'FAIL: byte-wise widening of UTF-8 remains in the GUI source.' >&2
  exit 1
fi
require 'std::memcpy(&target, &symbol, sizeof(target));' "$WORKER"
if grep -Fq 'reinterpret_cast<T>(GetProcAddress' "$WORKER"; then
  echo 'FAIL: unsafe templated GetProcAddress cast remains in the worker.' >&2
  exit 1
fi
echo 'Windows encoding and dynamic function-binding checks passed.'
