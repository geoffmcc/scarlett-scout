#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APP_VERSION="$(sed -nE 's/^project\(ScarlettScout VERSION ([0-9]+(\.[0-9]+)*) .*/\1/p' "$ROOT/CMakeLists.txt" | head -n 1)"
[[ -n "$APP_VERSION" ]] || { echo "Could not read application version from CMakeLists.txt." >&2; exit 1; }
BUILD_DIR="$ROOT/build-windows"
HOST_BUILD="$ROOT/build-host"
CACHE="$ROOT/.cache"
MINIAUDIO_VERSION="0.11.26"
MINIAUDIO_URL="https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VERSION}/miniaudio.h"

# Official Intel Windows archive, static VC runtime variant. It contains the
# native C/C++ runtime and NPU plugin; no Windows Python distribution is used.
OPENVINO_VERSION="2026.4.1"
OPENVINO_ARCHIVE="openvino_toolkit_windows_vc_mt_2026.4.1.22982.07f9c262b05_x86_64.zip"
OPENVINO_URL="https://storage.openvinotoolkit.org/repositories/openvino/packages/${OPENVINO_VERSION}/windows_vc_mt/${OPENVINO_ARCHIVE}"
MODEL_URL="https://huggingface.co/onnx-community/Musical-genres-Classification-Hubert-V1-ONNX/resolve/main/onnx/model_fp16.onnx"
CONFIG_URL="https://huggingface.co/onnx-community/Musical-genres-Classification-Hubert-V1-ONNX/resolve/main/config.json"
MODEL_SHA256="67a043e1ff0aba037b5841a4b52137317f62a368dcd3f0b24919b07f7be7bbde"

DO_NPU=1
DO_LAUNCH=1
for arg in "$@"; do
  case "$arg" in
    --skip-npu) DO_NPU=0 ;;
    --no-launch) DO_LAUNCH=0 ;;
    -h|--help)
      cat <<'HELP'
Scarlett Scout — WSL build + native Windows install

Usage: ./install.sh [--skip-npu] [--no-launch]

  --skip-npu   Build/install the audio application and native worker, but do
               not download the optional OpenVINO runtime or genre model.
  --no-launch  Install the Windows application but do not launch it.

Python is not installed, bundled, or invoked on Windows. The AI inference
worker is a native C++ executable and loads OpenVINO's native C API DLL.

The installer installs WSL build dependencies, downloads pinned miniaudio,
runs regression tests, cross-compiles the Windows x64 app + native inference worker,
then installs the application and (unless skipped) OpenVINO + model under
%LOCALAPPDATA%\\ScarlettScout.
HELP
      exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

if ! grep -qi microsoft /proc/version 2>/dev/null; then
  echo "This installer is intended to run inside WSL (Ubuntu)." >&2
  exit 2
fi

need_packages=()
command -v cmake >/dev/null 2>&1 || need_packages+=(cmake)
command -v ninja >/dev/null 2>&1 || need_packages+=(ninja-build)
command -v curl >/dev/null 2>&1 || need_packages+=(curl)
command -v unzip >/dev/null 2>&1 || need_packages+=(unzip)
command -v zip >/dev/null 2>&1 || need_packages+=(zip)
command -v sha256sum >/dev/null 2>&1 || need_packages+=(coreutils)
command -v c++ >/dev/null 2>&1 || need_packages+=(build-essential)
command -v x86_64-w64-mingw32-gcc-posix >/dev/null 2>&1 || need_packages+=(gcc-mingw-w64-x86-64-posix)
command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1 || need_packages+=(g++-mingw-w64-x86-64-posix)
command -v x86_64-w64-mingw32-windres >/dev/null 2>&1 || need_packages+=(binutils-mingw-w64-x86-64)
if ((${#need_packages[@]})); then
  echo "Installing required WSL build tools..."
  if [[ $EUID -eq 0 ]]; then SUDO=; else SUDO=sudo; fi
  $SUDO apt-get update
  $SUDO apt-get install -y ca-certificates "${need_packages[@]}"
fi

for cmd in cmake ninja curl unzip zip sha256sum c++ wslpath powershell.exe cmd.exe \
           x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-windres; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "Required command missing after dependency installation: $cmd" >&2; exit 1; }
done

mkdir -p "$ROOT/vendor" "$ROOT/dist" "$HOST_BUILD" "$CACHE"
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  CACHED_SOURCE="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | head -n 1)"
  if [[ -n "$CACHED_SOURCE" && "$CACHED_SOURCE" != "$ROOT" ]]; then
    echo "Removing stale CMake build cache from: $CACHED_SOURCE"
    rm -rf "$BUILD_DIR"
  fi
fi
mkdir -p "$BUILD_DIR"
if [[ ! -s "$ROOT/vendor/miniaudio.h" ]] || ! grep -q 'MA_VERSION_MINOR.*11' "$ROOT/vendor/miniaudio.h"; then
  echo "Downloading miniaudio ${MINIAUDIO_VERSION}..."
  curl --fail --location --retry 3 --connect-timeout 15 "$MINIAUDIO_URL" -o "$ROOT/vendor/miniaudio.h.tmp"
  grep -q 'MA_VERSION_MINOR.*11' "$ROOT/vendor/miniaudio.h.tmp" && grep -q 'MA_VERSION_REVISION.*26' "$ROOT/vendor/miniaudio.h.tmp" || {
    echo "Downloaded file does not look like the expected miniaudio header." >&2
    rm -f "$ROOT/vendor/miniaudio.h.tmp"
    exit 1
  }
  mv "$ROOT/vendor/miniaudio.h.tmp" "$ROOT/vendor/miniaudio.h"
fi

# Portable DSP/WAV tests execute in WSL; Windows-only worker integration is
# compiled by the MinGW cross-build below.
echo "Building and running DSP regression test..."
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT/include" "$ROOT/tests/dsp_tests.cpp" "$ROOT/src/dsp.cpp" \
  -o "$HOST_BUILD/ScarlettScoutDspTests"
"$HOST_BUILD/ScarlettScoutDspTests"

echo "Building and running WAV regression test..."
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT/include" "$ROOT/tests/wav_tests.cpp" "$ROOT/src/wav.cpp" \
  -o "$HOST_BUILD/ScarlettScoutWavTests"
"$HOST_BUILD/ScarlettScoutWavTests"

echo "Checking native OpenVINO C API ABI declarations..."
"$ROOT/tests/check_npu_api_abi.sh"
echo "Checking Windows UTF-8 metadata and dynamic function binding..."
"$ROOT/tests/check_windows_encoding.sh"
echo "Checking application/worker version consistency..."
"$ROOT/tests/check_version.sh"
echo "Checking automatic capture workflow..."
"$ROOT/tests/check_capture_workflow.sh"
echo "Checking selectable inference backends..."
"$ROOT/tests/check_inference_backends.sh"
echo "Checking track-level analysis, genre consensus and UI contracts..."
"$ROOT/tests/check_track_analysis.sh"
echo "Checking live input callback and waveform diagnostics..."
"$ROOT/tests/check_input_diagnostics.sh"
echo "Checking Win32 panel controls and click routing..."
"$ROOT/tests/check_win32_panel_controls.sh"

echo "Configuring native Windows x64 build..."
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" --parallel "$(nproc)"
APP_EXE="$BUILD_DIR/bin/ScarlettScout.exe"
NPU_EXE="$BUILD_DIR/bin/ScarlettScoutNpuWorker.exe"
[[ -s "$APP_EXE" ]] || { echo "Build completed without producing $APP_EXE" >&2; exit 1; }
[[ -s "$NPU_EXE" ]] || { echo "Build completed without producing $NPU_EXE" >&2; exit 1; }

WIN_LOCAL_APPDATA="$(powershell.exe -NoProfile -Command '[Environment]::GetFolderPath("LocalApplicationData")' | tr -d '\r' | tail -n 1)"
[[ -n "$WIN_LOCAL_APPDATA" ]] || { echo "Could not resolve Windows LocalAppData." >&2; exit 1; }
APP_DIR_WIN="$WIN_LOCAL_APPDATA\\ScarlettScout"
APP_DIR_WSL="$(wslpath -u "$APP_DIR_WIN")"
mkdir -p "$APP_DIR_WSL/models" "$APP_DIR_WSL/npu-inbox"

# A previous install attempt can fail to replace EXEs if the GUI or NPU worker
# is still running. Stop only these two app-owned processes before deployment.
echo "Stopping any running Scarlett Scout processes before replacing the application..."
powershell.exe -NoProfile -NonInteractive -Command '$ErrorActionPreference = "SilentlyContinue"; $names = @("ScarlettScout", "ScarlettScoutNpuWorker"); foreach ($name in $names) { Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }; Start-Sleep -Milliseconds 900; foreach ($name in $names) { if (Get-Process -Name $name -ErrorAction SilentlyContinue) { Write-Error "Could not stop $name.exe; close it manually and retry."; exit 1 } }'

# Remove stale IPC status so the next launch cannot display an error left by a
# previously running worker. Captures, settings, and queued diagnostics remain.
rm -f "$APP_DIR_WSL/npu-status.txt" "$APP_DIR_WSL/npu-result.txt" "$APP_DIR_WSL/npu-track-result.txt" "$APP_DIR_WSL/npu-capture-result.txt" "$APP_DIR_WSL/npu-stop.txt" "$APP_DIR_WSL/npu-reset.txt"

install_windows_exe() {
  local source="$1" destination="$2" temporary="${2}.new"
  local attempt
  for attempt in 1 2 3; do
    rm -f "$temporary"
    if cp -- "$source" "$temporary" && mv -f -- "$temporary" "$destination"; then
      if cmp -s -- "$source" "$destination"; then return 0; fi
    fi
    sleep 1
  done
  echo "Could not install $(basename "$destination") to $destination." >&2
  echo "The file may be locked or Windows may be denying access. Close Scarlett Scout and retry." >&2
  return 1
}

echo "Installing Scarlett Scout v${APP_VERSION}..."
install_windows_exe "$APP_EXE" "$APP_DIR_WSL/ScarlettScout.exe"
install_windows_exe "$NPU_EXE" "$APP_DIR_WSL/ScarlettScoutNpuWorker.exe"
cp "$ROOT/README.md" "$APP_DIR_WSL/README.txt"
cp "$ROOT/THIRD-PARTY-NOTICES.md" "$APP_DIR_WSL/THIRD-PARTY-NOTICES.txt"
printf '%s\n' "$APP_VERSION" > "$APP_DIR_WSL/app-version.txt"

echo "Installed executable verification:"
sha256sum "$APP_DIR_WSL/ScarlettScout.exe" "$APP_DIR_WSL/ScarlettScoutNpuWorker.exe"

if (( DO_NPU )); then
  OV_ARCHIVE_PATH="$CACHE/$OPENVINO_ARCHIVE"
  OV_EXTRACT="$CACHE/openvino-${OPENVINO_VERSION}-windows"
  OV_SHA_PATH="$CACHE/$OPENVINO_ARCHIVE.sha256"
  MODEL_PATH="$CACHE/genre-model.onnx"
  CONFIG_PATH="$CACHE/genre-config.json"

  if [[ ! -s "$OV_ARCHIVE_PATH" ]]; then
    echo "Downloading official OpenVINO ${OPENVINO_VERSION} Windows native runtime archive (this is a large download)..."
    curl --fail --location --retry 3 --connect-timeout 20 "$OPENVINO_URL" -o "$OV_ARCHIVE_PATH"
  fi
  if [[ ! -s "$OV_SHA_PATH" ]]; then
    curl --fail --location --retry 3 --connect-timeout 20 "${OPENVINO_URL}.sha256" -o "$OV_SHA_PATH"
  fi
  OV_EXPECTED_SHA="$(awk 'NF {print $1; exit}' "$OV_SHA_PATH" | tr 'A-F' 'a-f')"
  [[ "$OV_EXPECTED_SHA" =~ ^[0-9a-f]{64}$ ]] || { echo "Could not parse the official OpenVINO SHA-256 sidecar." >&2; exit 1; }
  echo "$OV_EXPECTED_SHA  $OV_ARCHIVE_PATH" | sha256sum --check --status || {
    echo "OpenVINO archive SHA-256 verification failed." >&2; rm -f "$OV_ARCHIVE_PATH"; exit 1;
  }

  if [[ ! -d "$OV_EXTRACT" ]] || ! find "$OV_EXTRACT" -type f -name openvino_c.dll -print -quit | grep -q .; then
    rm -rf "$OV_EXTRACT"
    mkdir -p "$OV_EXTRACT"
    echo "Extracting native OpenVINO runtime files..."
    unzip -q "$OV_ARCHIVE_PATH" -d "$OV_EXTRACT"
  fi
  OV_DLL_DIR="$(find "$OV_EXTRACT" -type f -name openvino_c.dll -printf '%h\n' | head -n 1)"
  [[ -n "$OV_DLL_DIR" ]] || { echo "The downloaded OpenVINO archive did not contain openvino_c.dll." >&2; exit 1; }
  OV_RUNTIME_SUFFIX="/bin/intel64/Release"
  case "$OV_DLL_DIR" in
    *"$OV_RUNTIME_SUFFIX") OV_RUNTIME_SOURCE="${OV_DLL_DIR%$OV_RUNTIME_SUFFIX}" ;;
    *) echo "Unexpected OpenVINO DLL layout: $OV_DLL_DIR" >&2; exit 1 ;;
  esac
  [[ -s "$OV_DLL_DIR/openvino_intel_npu_plugin.dll" ]] || {
    echo "OpenVINO archive does not contain openvino_intel_npu_plugin.dll beside openvino_c.dll." >&2; exit 1;
  }

  # Keep all available inference backends in the app-local plugin map. The
  # worker still compiles explicitly for the user's selected device; this XML
  # only makes the installed plugins discoverable to OpenVINO.
  {
    printf '%s\n' '<?xml version="1.0"?>' '<ie>' '  <plugins>'
    [[ -s "$OV_DLL_DIR/openvino_intel_npu_plugin.dll" ]] && printf '%s\n' '    <plugin name="NPU" location="openvino_intel_npu_plugin.dll" />'
    [[ -s "$OV_DLL_DIR/openvino_intel_gpu_plugin.dll" ]] && printf '%s\n' '    <plugin name="GPU" location="openvino_intel_gpu_plugin.dll" />'
    [[ -s "$OV_DLL_DIR/openvino_intel_cpu_plugin.dll" ]] && printf '%s\n' '    <plugin name="CPU" location="openvino_intel_cpu_plugin.dll" />'
    printf '%s\n' '  </plugins>' '</ie>'
  } > "$OV_DLL_DIR/plugins.xml"
  [[ -s "$OV_DLL_DIR/plugins.xml" ]] || {
    echo "Could not create the Scarlett Scout OpenVINO plugin configuration." >&2; exit 1;
  }
  if [[ ! -s "$OV_DLL_DIR/openvino_intel_npu_plugin.dll" ]]; then
    echo "OpenVINO runtime archive is missing its NPU plugin." >&2; exit 1
  fi
  if [[ -s "$OV_DLL_DIR/openvino_intel_gpu_plugin.dll" ]]; then
    echo "OpenVINO GPU plugin found; GPU inference will be offered if a compatible GPU is enumerated by the Windows driver."
  else
    echo "OpenVINO GPU plugin not present in this runtime archive; GPU inference will be unavailable."
  fi

  [[ -s "$OV_RUNTIME_SOURCE/3rdparty/tbb/bin/tbb12.dll" ]] || {
    echo "OpenVINO archive is missing its expected TBB runtime dependency." >&2; exit 1;
  }

  if [[ ! -s "$MODEL_PATH" ]] || [[ "$(sha256sum "$MODEL_PATH" | awk '{print $1}')" != "$MODEL_SHA256" ]]; then
    echo "Downloading the local music-genre ONNX model..."
    curl --fail --location --retry 3 --connect-timeout 20 "$MODEL_URL" -o "$MODEL_PATH.tmp"
    ACTUAL_MODEL_SHA="$(sha256sum "$MODEL_PATH.tmp" | awk '{print $1}')"
    if [[ "$ACTUAL_MODEL_SHA" != "$MODEL_SHA256" ]]; then
      rm -f "$MODEL_PATH.tmp"
      echo "Genre model SHA-256 mismatch; refusing to install it." >&2
      echo "Expected: $MODEL_SHA256" >&2
      echo "Actual:   $ACTUAL_MODEL_SHA" >&2
      exit 1
    fi
    mv "$MODEL_PATH.tmp" "$MODEL_PATH"
  fi
  if [[ ! -s "$CONFIG_PATH" ]]; then
    curl --fail --location --retry 3 --connect-timeout 20 "$CONFIG_URL" -o "$CONFIG_PATH"
  fi

  echo "Installing native OpenVINO runtime and model under Windows LocalAppData..."
  rm -rf "$APP_DIR_WSL/openvino_runtime/runtime"
  mkdir -p "$APP_DIR_WSL/openvino_runtime"
  cp -a "$OV_RUNTIME_SOURCE" "$APP_DIR_WSL/openvino_runtime/"
  cp "$MODEL_PATH" "$APP_DIR_WSL/models/genre-model.onnx"
  cp "$CONFIG_PATH" "$APP_DIR_WSL/models/config.json"
  printf 'OpenVINO %s (Windows vc_mt archive)\n' "$OPENVINO_VERSION" > "$APP_DIR_WSL/openvino-runtime-version.txt"
else
  echo "Skipping OpenVINO runtime/model download (--skip-npu)."
fi

# Create a portable Windows folder too. It includes native runtime binaries and
# the model only when NPU installation was requested.
PORTABLE="$ROOT/dist/ScarlettScout-${APP_VERSION}-Windows-x64"
rm -rf "$PORTABLE"
mkdir -p "$PORTABLE/models" "$PORTABLE/npu-inbox"
cp "$APP_EXE" "$NPU_EXE" "$PORTABLE/"
cp "$ROOT/README.md" "$PORTABLE/README.txt"
cp "$ROOT/THIRD-PARTY-NOTICES.md" "$PORTABLE/THIRD-PARTY-NOTICES.txt"
if (( DO_NPU )); then
  mkdir -p "$PORTABLE/openvino_runtime"
  cp -a "$OV_RUNTIME_SOURCE" "$PORTABLE/openvino_runtime/"
  cp "$MODEL_PATH" "$PORTABLE/models/genre-model.onnx"
  cp "$CONFIG_PATH" "$PORTABLE/models/config.json"
fi
(cd "$ROOT/dist" && zip -qr "ScarlettScout-${APP_VERSION}-Windows-x64.zip" "ScarlettScout-${APP_VERSION}-Windows-x64")

printf '\nBuild and installation completed.\n'
printf 'Windows application: %s\\ScarlettScout.exe\n' "$APP_DIR_WIN"
printf 'Native inference worker: %s\\ScarlettScoutNpuWorker.exe\n' "$APP_DIR_WIN"
printf 'Application version:  v%s\n' "$APP_VERSION"
printf 'Portable Windows ZIP: %s\n' "$ROOT/dist/ScarlettScout-${APP_VERSION}-Windows-x64.zip"
printf 'Capture folder: Documents\\Scarlett Scout Captures\n'
printf 'Python was not installed or invoked on Windows.\n'
printf 'NPU worker handshake reports its own version on launch; physical Scarlett routing and NPU inference still require hardware verification.\n'

if (( DO_LAUNCH )); then
  EXE_WIN="$(wslpath -w "$APP_DIR_WSL/ScarlettScout.exe")"
  echo "Launching Scarlett Scout..."
  cmd.exe /C start "" "$EXE_WIN" || true
fi
