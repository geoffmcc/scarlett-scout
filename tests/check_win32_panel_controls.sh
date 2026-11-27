#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MAIN="$ROOT/src/main.cpp"
grep -q 'HWND panel(const wchar_t\* text, int id' "$MAIN"
grep -q 'make(L"STATIC", text, SS_OWNERDRAW' "$MAIN"
grep -q 'dis->CtlType == ODT_STATIC' "$MAIN"
! grep -q 'actualStyle |= BS_OWNERDRAW' "$MAIN"
! grep -q 'make(L"BUTTON", .*BS_GROUPBOX' "$MAIN"
grep -q 'Start Listening selected' "$MAIN"
echo "PASS: panels are non-interactive owner-drawn STATIC controls; groupbox/button type OR bug removed."
