# Scarlett Scout v0.0.8 — Adaptive interesting-section detection

Scarlett Scout is a native Windows x64 desktop application for listening to audio from a Focusrite interface or other Windows audio endpoints, analyzing musical material, and automatically capturing sample-worthy passages. The intended workflow is to leave Scout listening while digging through vinyl, YouTube, a DAW, or other audio sources; qualifying sections are saved automatically so the listener does not need to time a manual recording.

Development and cross-compilation happen in WSL. The deployed Windows application and inference worker are native C++ executables. **Python is not installed, bundled, or invoked on Windows.** The optional AI inference process loads the native OpenVINO C API DLL.

This release is a substantial track-analysis and workflow foundation. It is not yet a claim of professional-grade accuracy across every music style or a substitute for validation with the user's Scarlett hardware and actual sample-digging sessions. The analysis reports its uncertainty instead of inventing a definitive answer when evidence is weak.

## What is new in v0.0.8

- Fixed a Win32 control-style bug in the dark UI. The previous code combined `BS_GROUPBOX` and `BS_OWNERDRAW` using bitwise OR; their values produced `BS_DEFCOMMANDLINK` (`0xF`) rather than an owner-drawn group box. This rendered a white command-link icon area in each of the five sections and allowed the panel backgrounds to receive focus/clicks.
- Replaced the interactive group-box controls with non-interactive owner-drawn STATIC panels. Only actual buttons now receive button clicks, restoring predictable Start Listening and other control behavior.
- Added an explicit Start Listening status transition so the UI visibly confirms that the button event reached the application before WASAPI initialization begins.
- Added end ellipses to single-line labels and tightened long explanatory copy so text does not disappear abruptly at control boundaries.

- Fixed a Windows cross-compilation error in capture metadata: UTF-8 analysis labels are now converted to UTF-16 before being written to wide-character streams and Win32 controls.
- Replaced incompatible `FARPROC` casts in the native OpenVINO worker with size-checked pointer representation copies, avoiding the MinGW `-Wcast-function-type` warnings.

- Track-level BPM and key consensus is accumulated over successive live analysis windows instead of treating each short window as the complete answer.
- Genre inference samples the live track every 15 seconds and aggregates the class scores and winning-label votes across multiple excerpts. A result is explicitly marked mixed/uncertain until it has enough windows, a useful top-score margin, and repeated votes.
- Track genre and latest captured-sample genre are stored and displayed separately, so a capture classification cannot overwrite the overall track profile.
- The tempo estimator tests adjacent half-period lags to reduce avoidable half-time errors caused by a 10 ms onset grid. The alternate metrical interpretation is exposed in the analysis metadata/UI.
- Track-key consensus only counts windows whose local key estimate clears the minimum confidence threshold; weak guesses no longer inflate the winning key's vote count.
- Inference backend selection is available in the UI: Automatic, Intel NPU, GPU (OpenVINO), and CPU. Automatic prefers NPU, then GPU, then CPU; explicit selections do not silently switch devices.
- The interface uses a restrained dark studio theme with clearer live analysis, capture, track-profile, and inference sections.

- Added an adaptive **Interesting Section Detector** that maintains a rolling robust baseline instead of comparing every passage against fixed loudness thresholds.
- Interestingness combines independent signal groups: energy/transient behavior, spectral change, rhythm/tempo behavior, tonal-center changes, stereo-image changes, and silence/re-entry. Correlated level/transient evidence is grouped so a single loud drum hit is not counted twice.
- Every detected event has a 0–100 interest score and a separate confidence score. Weak or unavailable measurements are excluded from the evidence total rather than treated as positive evidence.
- Events use persistence and cooldown to prevent repeated triggers, and the detector classifies probable breakdowns, build-ups, drops, transitions, percussion entrances, rhythmic changes, energy changes, instrumental/textural changes, spectral events, and silence re-entry.
- Automatic capture now uses the detector event lifecycle: the detected section gets a pre-roll, the detector's event timestamp is retained, and post-roll is collected before the WAV is saved.
- The live UI exposes current interestingness and, when an event is active, its score and confidence alongside the explanation. The track profile explicitly distinguishes adaptive event scoring from track-level BPM/key consensus.
- Added host-runnable regression tests for adaptive baselines, persistence, cooldown, multi-signal events, and silence/re-entry behavior.

## Audio source and routing support

- Enumerates Windows WASAPI playback/render endpoints and recording/capture endpoints.
- Playback endpoints are labelled **[PLAYBACK LOOPBACK]** and captured through WASAPI loopback. This is intended for browser/YouTube/DAW audio routed to a Scarlett playback endpoint.
- Recording endpoints are labelled **[RECORDING INPUT]** and captured as input devices. This is intended for the turntable's Focusrite physical input path and other capture endpoints.
- Shows the channel count Windows reports for the selected endpoint and offers every contiguous stereo pair it exposes (1/2, 3/4, 5/6, etc.), plus each individual mono channel.
- Channel-pair selection changes the channels copied into the processing stream; it is not just a UI label.
- Supports stereo and mono modes. Stereo retains the chosen pair; mono combines the selected pair or uses the selected mono channel.
- Requests a 48 kHz float32 shared-mode processing stream. WASAPI/miniaudio may convert the endpoint's native shared-mode format.

**Important Windows/Focusrite limitation:** this build uses WASAPI, not Focusrite ASIO. Scout can only select the input channels and playback endpoints that Windows exposes through the active driver configuration. If a particular endpoint reports two channels, Scout cannot invent a 3/4 pair. If the Scarlett's 3/4 channels are exposed as a separate endpoint or through a multichannel endpoint, Scout can select them there. ASIO-only endpoints are not enumerated in this version.

## Live listening and capture

- A 120-second rolling stereo float buffer feeds analysis and capture.
- Capture Now saves the selected recent duration (5, 10, 15, 30, or 60 seconds).
- Automatic interesting-section capture is enabled by default. There are no Capture/Ignore prompt buttons; Scout saves qualifying passages and resumes listening automatically.
- Automatic capture includes an 18-second analysis lead-in plus 12 seconds of post-roll (nominally 30 seconds total). Analysis runs on five-second intervals, so the final saved clip can include a small additional scheduling margin.
- A 45-second auto-capture cooldown reduces repeated captures of the same passage.
- Sensitivity is adjustable. The automatic detector uses a combination of level, transient activity, tempo stability, tonal evidence, and novelty/interestingness rather than relying on the NPU to decide whether to save.
- The audio callback only copies samples into the rolling buffer. DSP, disk writing, and AI inference happen on separate worker paths so model latency cannot block the audio callback.
- WAV formats: 16-bit PCM, 24-bit PCM, and 32-bit float. New installations default to 32-bit float; existing preferences are retained.
- Captures are saved under `Documents\\Scarlett Scout Captures`.
- Filenames include timestamp, best available key (or `Key-uncertain`), BPM when sufficiently stable, capture reason, and automatic/manual origin.
- A `.analysis.txt` sidecar records endpoint/source, channels, mode, duration, level/clipping data, local estimates, track consensus, and related capture metadata.
- Capture History lists recent WAV files; double-click opens the selected file in the Windows default audio player.

The capture algorithm is intentionally conservative about claims: it may miss subtle/sample-worthy moments and it may save a false positive. Sensitivity and cooldown are tunable; sample quality still needs listening review.

## Track-level musical analysis

- DSP analyzes an 18-second rolling window every five seconds.
- Tempo uses spectral-flux onset tracking and periodicity checks. A candidate must recur in multiple sub-sections of the window. Nearby half-time/double-time interpretations are preserved as an alternate where evidence supports both.
- DSP updates local estimates every five seconds, while track consensus receives one vote every ten seconds. That spacing reduces the correlation caused by repeatedly voting with highly overlapping 18-second windows.
- Track tempo is a consensus over successive valid observations. The interface distinguishes a tentative local result from a track estimate supported by multiple windows.
- Key estimation aggregates chroma across overlapping FFT windows. Track key consensus requires multiple confidence-qualified observations; if sections disagree or evidence is weak, Scout should not pretend the track has one certain key.
- Additional analysis includes left/right RMS, peak level, clipping percentage, stereo correlation, transient rate, spectral centroid, crest factor, silence ratio, and a descriptive character/section heuristic.
- A section-novelty measure compares successive analysis windows for changes in transient density, energy, spectral shape, and sufficiently supported tonal center. This can help auto-capture notice a new passage instead of depending only on a loud/steady signal.

**Confidence language matters:** BPM stability and key-fit scores are algorithmic evidence scores, not calibrated statistical probabilities. Track agreement is the percentage of valid observations that support the selected consensus. A song with modulation, sparse harmony, heavy distortion, drums-only sections, or unusual tuning may legitimately have an uncertain key or tempo.

## Native AI inference and device selection

The optional music-genre classifier runs in a separate native C++ worker through OpenVINO's C API. It is independent from live capture and DSP.

- **Automatic:** prefers an enumerated NPU, then GPU, then CPU. The worker tries the next available backend if compilation fails in Automatic mode.
- **Intel NPU:** explicitly requests the OpenVINO NPU plugin. It requires a compatible Intel NPU and Windows NPU driver.
- **GPU (OpenVINO):** explicitly requests the OpenVINO GPU plugin. GPU inference depends on a compatible OpenCL implementation/graphics driver and the device being enumerated by OpenVINO. OpenVINO's documented GPU support targets Intel graphics hardware; newer OpenVINO device enumeration can also report non-Intel OpenCL devices, but that does not guarantee every GPU/model combination is supported. This is **not a CUDA backend** and should not be assumed to use an NVIDIA RTX GPU.
- **CPU:** explicitly requests the OpenVINO CPU plugin.

An explicit device selection fails visibly if the requested device cannot be enumerated or compile the model. It does not silently fall back. The status reports the actual selected OpenVINO device.

The bundled model is a ten-class music-genre classifier (blues, classical, country, disco, hip-hop, jazz, metal, pop, reggae, and rock) based on a GTZAN-style genre task. It is **not** an instrument recognizer, drum-break detector, subgenre classifier, or general-purpose audio-language model. Its softmax scores are model-relative and are not calibrated probabilities.

For live track analysis, Scout sends a centered ten-second excerpt from an 18-second analysis window to the worker every 15 seconds. The worker aggregates class scores and winning-label votes over the listening session. It only presents a track-level winner when there are at least four excerpts, the leading class receives at least half the winning votes, the mean score is at least 38%, and its margin over the runner-up is at least six percentage points. Otherwise the result is labelled mixed/uncertain. These thresholds reduce premature labels; they cannot make a limited model understand every musical context correctly.

A saved clip is classified separately. Its result is shown independently from the overall track consensus.

## UI design

The app uses a dark, restrained studio-style interface: muted charcoal panels, clear typographic hierarchy, teal/blue signal accents, compact live meters, and separate areas for routing, musical analysis, automatic capture, inference, and capture history. The visual treatment is intended to make the application legible during listening rather than add decorative effects.

## Build and install from WSL

The source archive contains `scarlett-scout/` as its top-level directory. From WSL, when the ZIP is on the Windows Desktop:

```bash
unzip -o /mnt/c/Users/geoff/Desktop/ScarlettScout-0.0.7-WSL-source.zip -d ~/Projects
cd ~/Projects/scarlett-scout
chmod +x install.sh
./install.sh --no-launch
```

The installer:

1. Installs missing WSL build dependencies.
2. Downloads the pinned miniaudio header into `vendor/`.
3. Builds/runs the DSP and WAV regression tests plus source-contract checks (including Windows UTF-8 metadata, dynamic API binding, and non-interactive panel/control routing).
4. Cross-compiles the native Windows x64 GUI and inference worker using CMake/MinGW.
5. Downloads and SHA-256 verifies the official Windows OpenVINO archive and model.
6. Builds an app-local plugin configuration for NPU/GPU/CPU plugins that are present.
7. Deploys the native executables, OpenVINO runtime and model to `%LOCALAPPDATA%\\ScarlettScout`.
8. Creates a portable Windows ZIP under `dist/`.

Options:

- `./install.sh --no-launch` builds/installs but does not start the Windows app.
- `./install.sh --skip-npu` builds/installs the audio application without downloading/deploying OpenVINO or the genre model.

No Python is required on Windows. WSL may have Python for other development tasks, but this project does not use it for the Windows application or inference worker.

## Suggested first-run verification

1. Connect the Scarlett 4i4 and open Focusrite Control. Enable the physical input pair receiving the turntable.
2. Launch Scout and refresh devices.
3. For vinyl, select the matching **[RECORDING INPUT]** endpoint and its available channel pair; confirm meters respond before relying on capture.
4. For YouTube/DAW playback, route Windows/application audio to the Scarlett playback endpoint, select the corresponding **[PLAYBACK LOOPBACK]** endpoint and channel pair, and confirm meters respond.
5. Start listening before a track begins when possible. The track consensus accumulates over time and resets when the listen session is restarted.
6. Leave automatic capture enabled; check the Capture History and listen to the saved WAVs to judge whether the event detector's thresholds suit your material.
7. Check the AI inference status for the actual selected device. If selecting GPU or NPU fails, return to Automatic and inspect the reported driver/plugin error.

## Test evidence and limits

The source package includes host-runnable DSP tests, WAV format tests, and source-contract tests for capture flow, OpenVINO C API declarations, backend selection, and version consistency. Tests that can run in a Linux/WSL development environment are executed by `install.sh` before cross-compilation.

This development container does not have the MinGW cross-compiler or a physical Scarlett 4i4/Windows Intel NPU, so the Windows x64 binary, device enumeration, actual channel-pair routing, NPU driver/model compilation, and real-time performance must be validated by running the supplied WSL installer and app on the user's Windows machine. Do not treat source-contract tests as proof of physical hardware behavior.

## Planned sample-discovery work

The next major capabilities are library indexing, audio embeddings and similarity search, better instrument/sample-type classification, harmonic compatibility filtering, transposition previews, section-level metadata browsing, and optional MPC workflow integration. These are intentionally not represented as already-working features in this release.

## Project layout

```text
scarlett-scout/
├── CMakeLists.txt
├── cmake/                MinGW cross-compilation toolchain
├── include/              Audio engine, DSP and WAV interfaces
├── src/                  Win32 UI, miniaudio capture, DSP, WAV writer, native OpenVINO worker
├── tests/                DSP/WAV and source-contract regression tests
├── vendor/               miniaudio header downloaded by install.sh
├── models/               model metadata; model file downloaded to Windows app folder
└── install.sh            WSL test, cross-build, runtime deployment and installation
```


## v0.0.7 input diagnostics

The live waveform now reports distinct states instead of drawing an empty dark strip when no samples have arrived. It shows whether the endpoint is waiting for callbacks, callbacks are arriving without input buffers, the input stream is active, or the stream is active but near-silent. A compact diagnostic line reports received audio duration, stream channel count, and callback count. These diagnostics help separate endpoint/routing problems from genuinely quiet audio.
