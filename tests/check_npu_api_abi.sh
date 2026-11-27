#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/npu_worker.cpp"

require() {
  if ! grep -Fq -- "$1" "$SRC"; then
    echo "FAIL: missing expected OpenVINO C API declaration: $1" >&2
    exit 1
  fi
}

require 'using AvailableDevicesFree = void (__cdecl*)(ov_available_devices_t*);'
require 'using PartialShapeFree = void (__cdecl*)(ov_partial_shape_t*);'
require 'using TensorFree = void (__cdecl*)(ov_tensor_t*);'
require 'api.availableDevicesFree(&devices);'

if grep -Fq 'devicesFreeStatus' "$SRC"; then
  echo 'FAIL: ov_available_devices_free is void in the upstream C header; never inspect a return status.' >&2
  exit 1
fi
if grep -Fq 'partialShapeFreeStatus' "$SRC"; then
  echo 'FAIL: ov_partial_shape_free is void in the upstream C header; never inspect a return status.' >&2
  exit 1
fi

shape_free_count="$(grep -Fc 'api.partialShapeFree(&shapes[' "$SRC")"
if [[ "$shape_free_count" -ne 2 ]]; then
  echo "FAIL: expected two mutually exclusive partial-shape cleanup paths; found $shape_free_count." >&2
  exit 1
fi

if grep -Fq 'OpenVINO could not release its available-device list' "$SRC"; then
  echo 'FAIL: stale v0.0.1 cleanup-status error string remains in the worker.' >&2
  exit 1
fi

echo 'OpenVINO C API ABI regression checks passed (void cleanup signatures verified). '
