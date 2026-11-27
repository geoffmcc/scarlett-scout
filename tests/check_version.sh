#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -nE 's/^project\(ScarlettScout VERSION ([0-9]+(\.[0-9]+)*) .*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
[[ "$VERSION" == "0.0.8" ]] || { echo "FAIL: expected project version 0.0.7, got '$VERSION'." >&2; exit 1; }
grep -Fq 'SCARLETT_SCOUT_VERSION_W L"@PROJECT_VERSION@"' "$ROOT/include/version.hpp.in"
grep -Fq 'L"LIVE SAMPLE INTELLIGENCE  ·  v" SCARLETT_SCOUT_VERSION_W' "$ROOT/src/main.cpp"
grep -Fq 'Scarlett Scout inference worker v") + SCARLETT_SCOUT_VERSION_A' "$ROOT/src/npu_worker.cpp"
grep -Fq 'APP_VERSION="$(sed -nE' "$ROOT/install.sh"
echo "Version consistency checks passed: v$VERSION (GUI, NPU worker, CMake, installer)."
