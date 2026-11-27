#include "audio_engine.hpp"
#include "wav.hpp"
#include "version.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cwchar>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;
using namespace scout;

// Convert UTF-8 analysis strings to UTF-16 for Win32 controls and wide streams.
// Analysis labels can contain non-ASCII symbols (for example, an unknown-key em dash),
// so widening individual UTF-8 bytes is not correct.
static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (required == 0) {
        // Keep the UI/metadata resilient if an upstream label is malformed UTF-8.
        required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    }
    if (required <= 1) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required) == 0) return {};
    result.resize(static_cast<size_t>(required - 1)); // omit the terminating NUL
    return result;
}

enum : int {
    ID_SOURCE = 1001, ID_CHANNEL = 1002, ID_MODE = 1003, ID_LENGTH = 1004, ID_FORMAT = 1005,
    ID_REFRESH = 1006, ID_LISTEN = 1007, ID_STOP = 1008, ID_CAPTURE = 1009,
    ID_AUTODETECT = 1010, ID_SENSITIVITY = 1011, ID_OPEN_FOLDER = 1014, ID_HISTORY = 1015,
    ID_RMS = 1020, ID_PEAK = 1021, ID_BPM = 1022, ID_KEY = 1023,
    ID_KEY_CONF = 1024, ID_TRANSIENTS = 1025, ID_CENTROID = 1026,
    ID_CHARACTER = 1027, ID_SECTION = 1028, ID_NPU_STATUS = 1029, ID_NPU_RESULT = 1030,
    ID_METER_L = 1031, ID_METER_R = 1032, ID_STATUS = 1033, ID_CAPTURE_INFO = 1034, ID_CLIP = 1035,
    ID_TRACK_INFO = 1036, ID_INFERENCE_DEVICE = 1037,
    ID_PANEL_ROUTING = 1101, ID_PANEL_ANALYSIS = 1102, ID_PANEL_AUTOCAPTURE = 1103,
    ID_PANEL_INFERENCE = 1104, ID_PANEL_HISTORY = 1105
};

struct App {
    HWND window = nullptr;
    HFONT font = nullptr;
    HFONT boldFont = nullptr;
    HWND source = nullptr, channel = nullptr, mode = nullptr, length = nullptr, format = nullptr, waveform = nullptr;
    HWND refresh = nullptr, listen = nullptr, stop = nullptr, capture = nullptr;
    HWND autodetect = nullptr, sensitivity = nullptr, candidateText = nullptr;
    HWND openFolder = nullptr, history = nullptr;
    HWND rms = nullptr, peak = nullptr, bpm = nullptr, bpmConfidence = nullptr, key = nullptr, keyConf = nullptr;
    HWND transients = nullptr, centroid = nullptr, character = nullptr, section = nullptr;
    HWND npuStatus = nullptr, npuResult = nullptr, meterL = nullptr, meterR = nullptr, status = nullptr, captureInfo = nullptr, clipping = nullptr;
    HWND trackInfo = nullptr, inferenceDevice = nullptr, captureGenreResult = nullptr;
    HWND inputDiagnostics = nullptr;
    HBRUSH backgroundBrush = nullptr, panelBrush = nullptr, controlBrush = nullptr;
    AudioEngine audio;
    std::wstring appDir;
    std::wstring settingsPath;
    std::wstring captureDir;
    std::wstring selectedSourceName;
    std::wstring selectedChannelName;
    int selectedSourceType = -1;
    bool timerRunning = false;
    DWORD listenStartedAtMs = 0;
    std::wstring lastAutoCaptureMessage = L"Auto-capture is enabled · waiting for an interesting passage.";

    HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
        // Do not combine Win32 button *types* with bitwise OR. For example,
        // BS_GROUPBOX (0x7) | BS_OWNERDRAW (0xB) becomes 0xF, which is
        // BS_DEFCOMMANDLINK. Panels are non-interactive owner-drawn STATICs.
        HWND hWnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
            x, y, w, h, window, id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (font) SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return hWnd;
    }

    HWND panel(const wchar_t* text, int id, int x, int y, int w, int h) {
        // Static owner-drawn panels never receive button clicks or steal focus.
        return make(L"STATIC", text, SS_OWNERDRAW, x, y, w, h, id);
    }

    HWND label(const wchar_t* text, int x, int y, int w, int h, int id = 0) {
        return make(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, x, y, w, h, id);
    }

    HWND valueLabel(const wchar_t* text, int x, int y, int w, int h, int id) {
        HWND hWnd = make(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, x, y, w, h, id);
        SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont), TRUE);
        return hWnd;
    }

    HWND combo(int id, int x, int y, int w, int h = 240) {
        return make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, h, id);
    }

    HWND button(const wchar_t* text, int id, int x, int y, int w, int h = 30) {
        return make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id);
    }

    std::wstring inferenceDeviceId() const {
        const std::wstring choice = getComboText(inferenceDevice);
        if (choice == L"Intel NPU") return L"NPU";
        if (choice == L"GPU (OpenVINO)") return L"GPU";
        if (choice == L"CPU") return L"CPU";
        return L"AUTO";
    }

    void restartInferenceWorker() {
        const std::wstring device = inferenceDeviceId();
        saveSettings();
        audio.stopAnalyzer();
        audio.startAnalyzer(appDir, device);
        setText(status, L"Restarted inference worker · requested " + device + L". Audio monitoring is unaffected.");
    }

    void setText(HWND control, const std::wstring& text) {
        if (control) SetWindowTextW(control, text.c_str());
    }

    std::wstring getComboText(HWND control) const {
        int n = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
        if (n == CB_ERR) return L"";
        const int len = static_cast<int>(SendMessageW(control, CB_GETLBTEXTLEN, n, 0));
        std::wstring s(static_cast<size_t>(std::max(0, len)) + 1, L'\0');
        SendMessageW(control, CB_GETLBTEXT, n, reinterpret_cast<LPARAM>(s.data()));
        if (!s.empty() && s.back() == L'\0') s.pop_back();
        return s;
    }

    int selection(HWND control) const {
        const LRESULT v = SendMessageW(control, CB_GETCURSEL, 0, 0);
        return v == CB_ERR ? -1 : static_cast<int>(v);
    }

    void syncModeToChannel() {
        const int idx = selection(channel);
        const auto& choices = audio.channelChoices();
        const bool isStereoPair = idx >= 0 && static_cast<size_t>(idx) < choices.size() && choices[static_cast<size_t>(idx)].width == 2;
        if (!isStereoPair) {
            SendMessageW(mode, CB_SETCURSEL, 1, 0); // A single input channel is necessarily mono.
            EnableWindow(mode, FALSE);
        } else {
            EnableWindow(mode, TRUE);
        }
    }

    void populateChannels(int preferredIndex = -1) {
        const int src = selection(source);
        if (src < 0) return;
        audio.makeChannelChoices(static_cast<size_t>(src));
        SendMessageW(channel, CB_RESETCONTENT, 0, 0);
        const auto& choices = audio.channelChoices();
        int chosen = 0;
        for (size_t i = 0; i < choices.size(); ++i) {
            const LRESULT idx = SendMessageW(channel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[i].label.c_str()));
            SendMessageW(channel, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
            if (preferredIndex >= 0 && static_cast<int>(i) == preferredIndex) chosen = static_cast<int>(i);
        }
        SendMessageW(channel, CB_SETCURSEL, chosen, 0);
        syncModeToChannel();
    }

    std::wstring typeText(SourceType type) const {
        return type == SourceType::PlaybackLoopback ? L"PLAYBACK LOOPBACK" : L"RECORDING INPUT";
    }

    void refreshDevices() {
        std::wstring error;
        const std::wstring oldName = getComboText(source);
        const int oldType = selectedSourceType;
        if (!audio.refreshSources(error)) {
            MessageBoxW(window, error.c_str(), L"Scarlett Scout — Device Enumeration", MB_ICONERROR | MB_OK);
            return;
        }
        SendMessageW(source, CB_RESETCONTENT, 0, 0);
        const auto& devices = audio.sources();
        int chosen = -1;
        int preferred = -1;
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& d = devices[i];
            std::wstring text = d.type == SourceType::PlaybackLoopback ? L"[PLAYBACK LOOPBACK] " : L"[RECORDING INPUT] ";
            text += d.name + L"  (" + std::to_wstring(d.availableChannels) + L" ch)";
            if (d.isDefault) text += L"  [DEFAULT]";
            SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (oldType == static_cast<int>(d.type) && oldName.find(d.name) != std::wstring::npos) chosen = static_cast<int>(i);
            if (preferred < 0 && (d.name.find(L"Scarlett") != std::wstring::npos || d.name.find(L"Focusrite") != std::wstring::npos) && d.type == SourceType::PlaybackLoopback) preferred = static_cast<int>(i);
        }
        if (chosen < 0) chosen = preferred;
        if (chosen < 0) {
            for (size_t i = 0; i < devices.size(); ++i)
                if (devices[i].type == SourceType::PlaybackLoopback && devices[i].isDefault) { chosen = static_cast<int>(i); break; }
        }
        if (chosen < 0) {
            for (size_t i = 0; i < devices.size(); ++i)
                if (devices[i].isDefault) { chosen = static_cast<int>(i); break; }
        }
        if (chosen < 0 && !devices.empty()) chosen = 0;
        if (chosen >= 0) SendMessageW(source, CB_SETCURSEL, chosen, 0);
        selectedSourceType = chosen >= 0 ? static_cast<int>(devices[static_cast<size_t>(chosen)].type) : -1;
        selectedSourceName = chosen >= 0 ? devices[static_cast<size_t>(chosen)].name : L"";
        populateChannels();
        setText(status, L"Enumerated " + std::to_wstring(devices.size()) + L" WASAPI endpoints (playback and recording). Choose the exact source and channel pair.");
    }

    std::wstring ini(const wchar_t* keyName, const wchar_t* fallback) const {
        wchar_t buffer[1024]{};
        GetPrivateProfileStringW(L"ScarlettScout", keyName, fallback, buffer, 1024, settingsPath.c_str());
        return buffer;
    }

    void saveSettings() {
        auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"ScarlettScout", k, v.c_str(), settingsPath.c_str()); };
        put(L"SourceName", selectedSourceName);
        put(L"SourceType", std::to_wstring(selectedSourceType));
        put(L"Channel", getComboText(channel));
        put(L"Mode", getComboText(mode));
        put(L"CaptureSeconds", getComboText(length));
        put(L"Format", getComboText(format));
        put(L"Sensitivity", getComboText(sensitivity));
        put(L"InferenceDevice", getComboText(inferenceDevice));
        put(L"AutoCapture", SendMessageW(autodetect, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    }

    void loadSettings() {
        const std::wstring wantedSource = ini(L"SourceName", L"");
        const int wantedType = _wtoi(ini(L"SourceType", L"-1").c_str());
        for (size_t i = 0; i < audio.sources().size(); ++i) {
            const auto& d = audio.sources()[i];
            std::wstring text = d.type == SourceType::PlaybackLoopback ? L"[PLAYBACK LOOPBACK] " : L"[RECORDING INPUT] ";
            text += d.name + L"  (" + std::to_wstring(d.availableChannels) + L" ch)";
            if (d.isDefault) text += L"  [DEFAULT]";
            if (d.name == wantedSource && static_cast<int>(d.type) == wantedType) {
                SendMessageW(source, CB_SETCURSEL, i, 0);
                selectedSourceType = wantedType; selectedSourceName = d.name;
                break;
            }
        }
        populateChannels();
        const std::wstring wantedChannel = ini(L"Channel", L"Stereo pair 1 / 2");
        int channelFound = 0;
        for (int i = 0; i < static_cast<int>(SendMessageW(channel, CB_GETCOUNT, 0, 0)); ++i) {
            SendMessageW(channel, CB_SETCURSEL, i, 0);
            if (getComboText(channel) == wantedChannel) { channelFound = i; break; }
        }
        SendMessageW(channel, CB_SETCURSEL, channelFound, 0);
        selectedChannelName = getComboText(channel);
        auto chooseText = [&](HWND c, const std::wstring& wanted, int fallback = 0) {
            int count = static_cast<int>(SendMessageW(c, CB_GETCOUNT, 0, 0));
            int found = fallback;
            for (int i = 0; i < count; ++i) {
                SendMessageW(c, CB_SETCURSEL, i, 0);
                if (getComboText(c) == wanted) { found = i; break; }
            }
            SendMessageW(c, CB_SETCURSEL, found, 0);
        };
        chooseText(mode, ini(L"Mode", L"Stereo"));
        syncModeToChannel();
        chooseText(length, ini(L"CaptureSeconds", L"15 seconds"));
        chooseText(format, ini(L"Format", L"32-bit float"));
        chooseText(sensitivity, ini(L"Sensitivity", L"Normal"));
        chooseText(inferenceDevice, ini(L"InferenceDevice", L"Automatic"));
        std::wstring autoCaptureSetting = ini(L"AutoCapture", L"");
        if (autoCaptureSetting.empty()) autoCaptureSetting = ini(L"AutoDetection", L"1");
        SendMessageW(autodetect, BM_SETCHECK, autoCaptureSetting == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
        applySensitivity();
    }

    void applySensitivity() {
        const std::wstring s = getComboText(sensitivity);
        if (s == L"Low") audio.setDetectionThreshold(0.78f);
        else if (s == L"High") audio.setDetectionThreshold(0.55f);
        else audio.setDetectionThreshold(0.66f);
        audio.setAutoCaptureEnabled(SendMessageW(autodetect, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }

    int captureSeconds() const {
        std::wstring s = getComboText(length);
        const int n = _wtoi(s.c_str());
        return std::clamp(n, 5, 60);
    }

    unsigned captureBits() const {
        const auto s = getComboText(format);
        if (s.find(L"24") != std::wstring::npos) return 24;
        if (s.find(L"32") != std::wstring::npos) return 32;
        return 16;
    }

    bool isMonoMode() const { return getComboText(mode) == L"Mono"; }

    void setRunningUi(bool running) {
        EnableWindow(source, !running); EnableWindow(channel, !running); EnableWindow(mode, !running);
        EnableWindow(refresh, !running); EnableWindow(listen, !running); EnableWindow(stop, running);
        EnableWindow(capture, running);
        if (!running) setText(status, L"Stopped. Existing captures remain available.");
    }

    void startListen() {
        const int sourceIndex = selection(source);
        const int channelIndex = selection(channel);
        if (sourceIndex < 0 || channelIndex < 0) {
            MessageBoxW(window, L"Select an audio source and a channel pair first.", L"Scarlett Scout", MB_ICONINFORMATION);
            return;
        }
        const auto selected = audio.sources()[static_cast<size_t>(sourceIndex)];
        const std::wstring selectedChannel = getComboText(channel);
        selectedChannelName = selectedChannel;
        const ChannelMode selectedMode = isMonoMode() ? ChannelMode::Mono : ChannelMode::Stereo;
        std::wstring error;
        if (!audio.start(static_cast<size_t>(sourceIndex), static_cast<size_t>(channelIndex), selectedMode, error)) {
            MessageBoxW(window, error.c_str(), L"Scarlett Scout — Could Not Start Audio", MB_ICONERROR | MB_OK);
            return;
        }
        selectedSourceName = selected.name;
        selectedSourceType = static_cast<int>(selected.type);
        setRunningUi(true);
        listenStartedAtMs = GetTickCount();
        setText(inputDiagnostics, L"WAITING FOR AUDIO DATA · WASAPI device opened; waiting for input callbacks.");
        setText(status, L"Listening to " + typeText(selected.type) + L": " + selected.name + L" — " + selectedChannel +
                        (selectedMode == ChannelMode::Mono ? L" · mono capture" : L" · stereo capture"));
        setText(captureInfo, L"120-second rolling buffer · " + std::to_wstring(AudioEngine::kSampleRate) + L" Hz · " +
                             std::to_wstring(audio.actualInputChannels()) + L" stream channel(s)");
        saveSettings();
    }

    std::wstring timestamp() const {
        SYSTEMTIME st{}; GetLocalTime(&st);
        wchar_t b[64]{};
        swprintf(b, 64, L"%04u%02u%02u_%02u%02u%02u_%03u", static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
        return b;
    }

    static std::wstring filenameToken(std::wstring value) {
        std::wstring out;
        bool lastDash = false;
        for (wchar_t c : value) {
            if (c == L'#') { out += L"sharp"; lastDash = false; }
            else if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                     (c >= L'0' && c <= L'9')) { out += c; lastDash = false; }
            else if (!lastDash && !out.empty()) { out += L'-'; lastDash = true; }
        }
        while (!out.empty() && out.back() == L'-') out.pop_back();
        return out.empty() ? L"unknown" : out;
    }

    bool saveCaptureSamples(const std::vector<float>& stereo, const std::wstring& suffix,
                            const std::wstring& reason, const AnalysisSnapshot* triggerAnalysis,
                            bool automatic) {
        const size_t frames = stereo.size() / 2;
        if (frames < static_cast<size_t>(AudioEngine::kSampleRate) * 2) {
            if (!automatic) MessageBoxW(window, L"Not enough audio has accumulated yet. Listen for at least two seconds before capturing.", L"Scarlett Scout", MB_ICONINFORMATION);
            return false;
        }
        const auto analysis = analyzeAudio(stereo, AudioEngine::kSampleRate);
        const TrackSummary track = audio.trackSummary();
        AnalysisSnapshot namingAnalysis = analysis;
        if (triggerAnalysis && triggerAnalysis->keyConfidence > namingAnalysis.keyConfidence) namingAnalysis = *triggerAnalysis;
        if (track.keyVotes >= 3 && track.keyAgreement >= 55.0f) {
            namingAnalysis.key = track.key;
            namingAnalysis.keyConfidence = track.keyAgreement;
        }
        std::wstring keyPart = L"Key-uncertain";
        if (namingAnalysis.key != "—" && namingAnalysis.keyConfidence >= 45.0f) {
            keyPart = L"Key-" + filenameToken(utf8ToWide(namingAnalysis.key));
            keyPart += L"-" + std::to_wstring(static_cast<unsigned>(std::lround(namingAnalysis.keyConfidence))) + L"pct";
        }
        std::wstring bpmPart;
        const bool stableTrackTempo = track.bpmVotes >= 3 && track.bpmAgreement >= 50.0f && track.bpm > 0.0f;
        const float namingBpm = stableTrackTempo ? track.bpm : analysis.bpm;
        const float namingBpmStrength = stableTrackTempo ? track.bpmAgreement : analysis.bpmConfidence;
        if (namingBpm > 0.0f && namingBpmStrength >= 40.0f)
            bpmPart = L"_BPM-" + std::to_wstring(static_cast<unsigned>(std::lround(namingBpm)));
        const std::wstring reasonPart = filenameToken(reason.empty() ? L"Manual-capture" : reason);
        fs::create_directories(captureDir);
        const std::wstring name = L"Scout_" + timestamp() + L"_" + keyPart + bpmPart + L"_" + reasonPart + L"_" + suffix + L".wav";
        const fs::path file = fs::path(captureDir) / name;
        std::vector<float> output;
        unsigned channels = 2;
        if (isMonoMode()) {
            channels = 1;
            output.resize(frames);
            for (size_t i = 0; i < frames; ++i) output[i] = 0.5f * (stereo[i * 2] + stereo[i * 2 + 1]);
        } else {
            output = stereo;
        }
        std::wstring error;
        if (!writeWav(file.wstring(), output, AudioEngine::kSampleRate, channels, captureBits(), error)) {
            MessageBoxW(window, error.c_str(), L"Capture failed", MB_ICONERROR);
            return false;
        }
        const std::wstring detectedKey = utf8ToWide(analysis.key);
        std::wofstream meta(fs::path(file.wstring() + L".analysis.txt"));
        if (meta) {
            meta << L"Scarlett Scout capture analysis\n"
                 << L"Capture reason: " << (reason.empty() ? L"Manual capture" : reason) << L"\n"
                 << L"Capture type: " << (automatic ? L"Automatic interesting-section capture" : L"Manual capture") << L"\n"
                 << L"Source: " << selectedSourceName << L"\n"
                 << L"Input type: " << (selectedSourceType == static_cast<int>(SourceType::PlaybackLoopback) ? L"WASAPI loopback" : L"Recording input") << L"\n"
                 << L"Selected channels: " << selectedChannelName << L"\n"
                 << L"Capture mode: " << (isMonoMode() ? L"Mono" : L"Stereo") << L"\n"
                 << L"Sample rate: " << AudioEngine::kSampleRate << L" Hz\n"
                 << L"Capture duration: " << std::fixed << std::setprecision(2)
                 << (static_cast<double>(frames) / AudioEngine::kSampleRate) << L" seconds\n"
                 << L"BPM estimate: " << analysis.bpm << L" (stability score " << analysis.bpmConfidence << L"/100)\n"
                 << L"Alternate BPM interpretation: " << analysis.bpmAlternate << L"\n"
                 << L"Key estimate: " << detectedKey << L" (window score " << analysis.keyConfidence << L"/100)\n"
                 << L"Track consensus key: " << utf8ToWide(track.key) << L" (" << track.keyVotes << L"/" << track.keyObservations
                 << L" confident windows; " << track.keyAgreement << L"% agreement)\n"
                 << L"Track consensus BPM: " << track.bpm << L" (" << track.bpmVotes << L"/" << track.bpmObservations
                 << L" observations; " << track.bpmAgreement << L"% agreement)\n"
                 << L"RMS: " << analysis.rmsDb << L" dBFS\nPeak: " << analysis.peakDb << L" dBFS\n"
                 << L"Clipped samples: " << analysis.clippingPercent << L"%\n"
                 << L"Transient rate: " << analysis.transientRate << L" per second\n"
                 << L"Spectral centroid: " << analysis.spectralCentroidHz << L" Hz\n"
                 << L"Character: " << utf8ToWide(analysis.character) << L"\n"
                 << L"Section: " << utf8ToWide(analysis.section) << L"\n";
            if (triggerAnalysis) {
                meta << L"Trigger-window BPM: " << triggerAnalysis->bpm << L" (strength score " << triggerAnalysis->bpmConfidence << L"/100)\n"
                     << L"Trigger-window key: " << utf8ToWide(triggerAnalysis->key)
                     << L" (confidence heuristic " << triggerAnalysis->keyConfidence << L"/100)\n"
                     << L"Trigger-window interesting score: " << triggerAnalysis->interestingScore << L"/1\n";
            }
            meta << L"NPU classification: A captured-sample inference has been queued.\n"
                 << L"NPU result caveat: broad 10-class genre estimate only; not instrument or sample-type identification.\n";
        }
        audio.submitCapturedSampleForNpu(stereo);
        refreshCaptureHistory();
        setText(status, (automatic ? L"Automatically captured " : L"Captured ") + name + L" · " +
                        std::to_wstring(static_cast<unsigned>(std::lround(static_cast<double>(frames) / AudioEngine::kSampleRate))) + L" seconds");
        setText(captureInfo, L"Saved: " + file.wstring());
        lastAutoCaptureMessage = automatic ? (L"Saved automatically · " + reason) : L"Manual capture saved.";
        return true;
    }

    bool saveCapture(uint64_t endFrame, const std::wstring& suffix, int seconds) {
        const auto stereo = audio.copyLastFrames(endFrame, static_cast<unsigned>(seconds));
        return saveCaptureSamples(stereo, suffix, L"Manual capture", nullptr, false);
    }

    void captureNow() {
        if (!audio.isRunning()) return;
        saveCapture(audio.writtenFrames(), L"Manual", captureSeconds());
    }

    void processAutoCapture() {
        AutoCaptureEvent event;
        if (!audio.takeAutoCaptureEvent(event)) return;
        const auto stereo = audio.copyFrameRange(event.startFrame, event.endFrame);
        if (!saveCaptureSamples(stereo, L"Auto", event.reason, &event.analysis, true)) {
            lastAutoCaptureMessage = L"Automatic capture could not be saved · listening continues.";
            setText(status, lastAutoCaptureMessage);
        }
    }

    void refreshCaptureHistory() {
        SendMessageW(history, LB_RESETCONTENT, 0, 0);
        try {
            std::vector<fs::directory_entry> files;
            if (fs::exists(captureDir)) {
                for (const auto& entry : fs::directory_iterator(captureDir)) {
                    if (entry.is_regular_file() && entry.path().extension() == L".wav") files.push_back(entry);
                }
            }
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.last_write_time() > b.last_write_time(); });
            for (size_t i = 0; i < std::min<size_t>(files.size(), 100); ++i) {
                const std::wstring name = files[i].path().filename().wstring();
                const LRESULT idx = SendMessageW(history, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                SendMessageW(history, LB_SETITEMDATA, idx, static_cast<LPARAM>(i));
            }
        } catch (...) {}
    }

    void openSelectedCapture() {
        const int idx = static_cast<int>(SendMessageW(history, LB_GETCURSEL, 0, 0));
        if (idx == LB_ERR) return;
        wchar_t name[512]{};
        SendMessageW(history, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(name));
        ShellExecuteW(window, L"open", (fs::path(captureDir) / name).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void updateUi() {
        if (waveform) InvalidateRect(waveform, nullptr, FALSE);
        if (audio.isRunning()) {
            const uint64_t receivedFrames = audio.writtenFrames();
            const uint64_t callbacks = audio.callbackCount();
            const uint64_t nullCallbacks = audio.nullInputCallbackCount();
            if (receivedFrames == 0) {
                const DWORD elapsed = GetTickCount() - listenStartedAtMs;
                if (callbacks == 0 && elapsed >= 2500) {
                    setText(inputDiagnostics, L"NO AUDIO CALLBACKS · The endpoint opened, but WASAPI has not delivered audio buffers. Check the selected source and Windows routing.");
                } else if (callbacks > 0 && nullCallbacks == callbacks) {
                    setText(inputDiagnostics, L"CALLBACK ACTIVE, NO INPUT BUFFER · WASAPI callbacks arrived without usable input data.");
                } else if (callbacks > 0) {
                    setText(inputDiagnostics, L"CALLBACKS RECEIVED, ZERO FRAMES WRITTEN · Check the negotiated stream channel count.");
                } else {
                    setText(inputDiagnostics, L"WAITING FOR AUDIO DATA · Device opened; waiting for the first input frames.");
                }
            } else {
                const double seconds = static_cast<double>(receivedFrames) / AudioEngine::kSampleRate;
                setText(inputDiagnostics, L"INPUT STREAM ACTIVE · " + fmt(static_cast<float>(seconds), 1) +
                    L" sec received · " + std::to_wstring(audio.actualInputChannels()) + L" stream channels · " +
                    std::to_wstring(callbacks) + L" callbacks");
            }
            const auto a = audio.latestAnalysis();
            const auto track = audio.trackSummary();
            setText(rms, fmt(a.rmsDb, 1) + L" dBFS");
            setText(peak, fmt(a.peakDb, 1) + L" dBFS");
            const bool stableTrackTempo = track.bpmVotes >= 3 && track.bpmAgreement >= 50.0f && track.bpm > 0.0f;
            const float shownBpm = stableTrackTempo ? track.bpm : a.bpm;
            setText(bpm, shownBpm > 0 ? (stableTrackTempo && track.bpmAgreement < 70.0f ? L"~" : L"") + fmt(shownBpm, 1) + L" BPM" : L"—");
            if (stableTrackTempo) {
                setText(bpmConfidence, L"Track agreement · " + std::to_wstring(track.bpmVotes) + L"/" +
                    std::to_wstring(track.bpmObservations) + L" windows · " + fmt(track.bpmAgreement, 0) + L"%");
            } else if (a.bpm > 0.0f) {
                setText(bpmConfidence, L"Building track consensus · local stability " + fmt(a.bpmConfidence, 0) + L"%" +
                    (a.bpmAlternate > 0.0f ? L" · alt " + fmt(a.bpmAlternate, 1) : L""));
            } else {
                setText(bpmConfidence, L"No stable pulse yet · gathering more sections");
            }
            const bool stableTrackKey = track.keyVotes >= 3 && track.keyAgreement >= 50.0f && track.key != "—";
            const std::string shownKey = stableTrackKey ? track.key : a.key;
            setText(key, utf8ToWide(shownKey));
            if (stableTrackKey) {
                setText(keyConf, L"Track agreement · " + std::to_wstring(track.keyVotes) + L"/" +
                    std::to_wstring(track.keyObservations) + L" windows · " + fmt(track.keyAgreement, 0) + L"%");
            } else if (a.key != "—") {
                setText(keyConf, L"Building track consensus · local score " + fmt(a.keyConfidence, 0) + L"%");
            } else {
                setText(keyConf, L"No reliable tonal center yet");
            }
            setText(transients, fmt(a.transientRate, 1) + L" / sec");
            setText(centroid, fmt(a.spectralCentroidHz, 0) + L" Hz");
            setText(character, utf8ToWide(a.character));
            setText(section, utf8ToWide(a.section));
            const int leftLevel = static_cast<int>(std::clamp((a.rmsLeftDb + 60.0f) * 100.0f / 60.0f, 0.0f, 100.0f));
            const int rightLevel = static_cast<int>(std::clamp((a.rmsRightDb + 60.0f) * 100.0f / 60.0f, 0.0f, 100.0f));
            SendMessageW(meterL, PBM_SETPOS, leftLevel, 0);
            SendMessageW(meterR, PBM_SETPOS, rightLevel, 0);
            setText(clipping, fmt(a.clippingPercent, 2) + L"% clipped");
            setText(trackInfo, L"TRACK PROFILE · " + std::to_wstring(track.windows) + L" analysis windows accumulated · estimates strengthen as the track plays");
        } else {
            setText(inputDiagnostics, L"INPUT STREAM STOPPED · Select a source and start listening.");
            SendMessageW(meterL, PBM_SETPOS, 0, 0);
            SendMessageW(meterR, PBM_SETPOS, 0, 0);
            setText(clipping, L"—");
            setText(bpmConfidence, L"—");
            setText(keyConf, L"—");
            setText(trackInfo, L"TRACK PROFILE · Start listening to build a track-level musical summary");
        }
        setText(npuStatus, audio.npuStatus());
        setText(npuResult, audio.npuResult());
        setText(captureGenreResult, audio.captureGenreResult());
        processAutoCapture();
        ShowWindow(candidateText, SW_SHOW);
        if (audio.autoCapturePending())
            setText(candidateText, L"INTERESTING PASSAGE DETECTED · automatic capture in progress · " + audio.candidateDescription());
        else if (audio.autoCaptureEnabled())
            setText(candidateText, lastAutoCaptureMessage + L" · listening for the next passage.");
        else
            setText(candidateText, L"Auto-capture disabled · use Capture Now for manual recording.");
    }

    static std::wstring fmt(float v, int decimals) {
        std::wostringstream s; s << std::fixed << std::setprecision(decimals) << v; return s.str();
    }

    void createControls() {
        HWND header = valueLabel(L"SCARLETT SCOUT", 24, 16, 520, 42, 0);
        (void)header;
        HWND version = label(L"LIVE SAMPLE INTELLIGENCE  ·  v" SCARLETT_SCOUT_VERSION_W, 26, 51, 550, 24);
        SendMessageW(version, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont), TRUE);
        label(L"Real-time audio analysis and automatic sample capture.", 560, 27, 480, 26);

        panel(L"AUDIO SOURCE & CHANNEL ROUTING", ID_PANEL_ROUTING, 20, 82, 1035, 196);
        label(L"Source (every available WASAPI playback and recording endpoint)", 38, 108, 500, 22);
        source = combo(ID_SOURCE, 38, 132, 500, 300);
        label(L"Channel pair / mono channel", 555, 108, 205, 22);
        channel = combo(ID_CHANNEL, 555, 132, 205, 260);
        label(L"Capture mode", 775, 108, 110, 22);
        mode = combo(ID_MODE, 775, 132, 110, 180);
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Stereo"));
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mono"));
        SendMessageW(mode, CB_SETCURSEL, 0, 0);
        label(L"48 kHz", 900, 132, 90, 28);
        refresh = button(L"Refresh devices", ID_REFRESH, 38, 181, 125);
        listen = button(L"Start listening", ID_LISTEN, 172, 181, 130);
        stop = button(L"Stop", ID_STOP, 311, 181, 75);
        EnableWindow(stop, FALSE);
        label(L"Capture", 406, 184, 55, 22);
        length = combo(ID_LENGTH, 463, 181, 100, 180);
        for (const wchar_t* v : {L"5 seconds", L"10 seconds", L"15 seconds", L"30 seconds", L"60 seconds"}) SendMessageW(length, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
        SendMessageW(length, CB_SETCURSEL, 2, 0);
        capture = button(L"CAPTURE NOW", ID_CAPTURE, 573, 181, 130);
        EnableWindow(capture, FALSE);
        format = combo(ID_FORMAT, 714, 181, 112, 180);
        for (const wchar_t* v : {L"16-bit PCM", L"24-bit PCM", L"32-bit float"}) SendMessageW(format, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
        SendMessageW(format, CB_SETCURSEL, 2, 0);
        autodetect = make(L"BUTTON", L"Auto-capture interesting sections", BS_AUTOCHECKBOX | WS_TABSTOP, 820, 181, 220, 26, ID_AUTODETECT);
        SendMessageW(autodetect, BM_SETCHECK, BST_CHECKED, 0);
        label(L"LIVE INPUT WAVEFORM  ·  2-SECOND ROLLING VIEW", 38, 209, 420, 18);
        waveform = CreateWindowExW(WS_EX_CLIENTEDGE, L"ScarlettScoutWaveform", L"", WS_CHILD | WS_VISIBLE,
                                   38, 229, 980, 42, window, nullptr, GetModuleHandleW(nullptr), this);
        inputDiagnostics = label(waveform ? L"INPUT STATUS · Start listening to inspect the selected endpoint." : L"WAVEFORM CONTROL FAILED TO CREATE · Check Windows control initialization.",
                                 38, 273, 980, 16);

        panel(L"LIVE MUSICAL ANALYSIS", ID_PANEL_ANALYSIS, 20, 291, 1035, 192);
        label(L"LEVEL", 40, 319, 70, 20); valueLabel(L"— dBFS", 40, 340, 190, 34, ID_RMS); rms = GetDlgItem(window, ID_RMS);
        label(L"PEAK", 255, 319, 70, 20); valueLabel(L"— dBFS", 255, 340, 190, 34, ID_PEAK); peak = GetDlgItem(window, ID_PEAK);
        label(L"TRACK TEMPO", 470, 319, 110, 20); valueLabel(L"— BPM", 470, 340, 180, 30, ID_BPM); bpm = GetDlgItem(window, ID_BPM); bpmConfidence = label(L"— confidence", 470, 369, 180, 17);
        label(L"LOCAL KEY", 675, 319, 90, 20); valueLabel(L"—", 675, 340, 145, 34, ID_KEY); key = GetDlgItem(window, ID_KEY);
        label(L"TRACK KEY", 840, 319, 150, 20); valueLabel(L"—", 840, 340, 180, 34, ID_KEY_CONF); keyConf = GetDlgItem(window, ID_KEY_CONF);
        label(L"TRANSIENTS", 40, 389, 100, 20); valueLabel(L"— / sec", 40, 410, 155, 28, ID_TRANSIENTS); transients = GetDlgItem(window, ID_TRANSIENTS);
        label(L"SPECTRAL CENTROID", 220, 389, 170, 20); valueLabel(L"— Hz", 220, 410, 190, 28, ID_CENTROID); centroid = GetDlgItem(window, ID_CENTROID);
        label(L"AUDIO CHARACTER", 425, 389, 140, 20); valueLabel(L"Waiting for audio", 425, 410, 250, 28, ID_CHARACTER); character = GetDlgItem(window, ID_CHARACTER);
        label(L"CURRENT SECTION", 690, 389, 140, 20); valueLabel(L"No section detected", 690, 410, 180, 28, ID_SECTION); section = GetDlgItem(window, ID_SECTION);
        label(L"CLIPPING", 890, 389, 100, 20); clipping = valueLabel(L"—", 890, 410, 130, 28, ID_CLIP);
        label(L"LEFT / RIGHT", 40, 448, 80, 18);
        meterL = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 125, 447, 375, 18, window, reinterpret_cast<HMENU>(ID_METER_L), GetModuleHandleW(nullptr), nullptr);
        meterR = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 520, 447, 375, 18, window, reinterpret_cast<HMENU>(ID_METER_R), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(meterL, PBM_SETRANGE, 0, MAKELPARAM(0, 100)); SendMessageW(meterR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(meterL, PBM_SETBARCOLOR, 0, RGB(64, 177, 155));
        SendMessageW(meterR, PBM_SETBARCOLOR, 0, RGB(79, 151, 203));
        SendMessageW(meterL, PBM_SETBKCOLOR, 0, RGB(34, 43, 55));
        SendMessageW(meterR, PBM_SETBKCOLOR, 0, RGB(34, 43, 55));

        panel(L"AUTOMATIC INTERESTING-SECTION CAPTURE", ID_PANEL_AUTOCAPTURE, 20, 494, 1035, 104);
        candidateText = valueLabel(L"Auto-capture enabled · listening for a distinctive musical passage.", 40, 521, 970, 30, 0);
        label(L"Sensitivity", 40, 563, 72, 24);
        sensitivity = combo(ID_SENSITIVITY, 118, 561, 110, 150);
        for (const wchar_t* v : {L"Low", L"Normal", L"High"}) SendMessageW(sensitivity, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
        SendMessageW(sensitivity, CB_SETCURSEL, 1, 0);
        label(L"Auto-saves 30-second clips · 18-second lead-in + 12-second post-roll.", 247, 563, 775, 24);

        trackInfo = label(L"TRACK PROFILE · Start listening to build a track-level musical summary", 40, 469, 970, 18, ID_TRACK_INFO);

        panel(L"AI INFERENCE · OPENVINO DEVICE BACKEND", ID_PANEL_INFERENCE, 20, 610, 1035, 132);
        label(L"Inference device", 40, 638, 105, 24);
        inferenceDevice = combo(ID_INFERENCE_DEVICE, 150, 634, 165, 220);
        for (const wchar_t* v : {L"Automatic", L"Intel NPU", L"GPU (OpenVINO)", L"CPU"})
            SendMessageW(inferenceDevice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v));
        SendMessageW(inferenceDevice, CB_SETCURSEL, 0, 0);
        npuStatus = valueLabel(L"OpenVINO inference runtime not detected", 335, 638, 680, 24, ID_NPU_STATUS);
        label(L"TRACK GENRE", 40, 672, 105, 20);
        npuResult = label(L"Waiting for multiple track excerpts", 150, 672, 860, 20, ID_NPU_RESULT);
        label(L"LATEST CAPTURE", 40, 696, 105, 20);
        captureGenreResult = label(L"No captured-sample classification yet", 150, 696, 860, 20);
        label(L"GPU requires the OpenVINO GPU plugin and a compatible graphics/OpenCL driver.", 40, 718, 970, 16);

        panel(L"CAPTURE HISTORY", ID_PANEL_HISTORY, 20, 750, 1035, 145);
        history = make(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 40, 777, 760, 100, ID_HISTORY);
        openFolder = button(L"Open capture folder", ID_OPEN_FOLDER, 820, 778, 190, 32);
        captureInfo = label(L"120-second rolling buffer will begin when listening starts.", 820, 820, 190, 52, ID_CAPTURE_INFO);
        status = label(L"Initializing WASAPI...", 25, 899, 1025, 25, ID_STATUS);

        HWND all[] = {source, channel, mode, length, format, refresh, listen, stop, capture, autodetect,
                      sensitivity, inferenceDevice, openFolder, history, captureGenreResult};
        for (HWND h : all) if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    void initializePaths() {
        wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
        appDir = fs::path(module).parent_path().wstring();
        wchar_t appData[MAX_PATH]{};
        DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
        const fs::path settingsDir = n ? fs::path(appData) / L"ScarlettScout" : fs::path(appDir) / L"settings";
        fs::create_directories(settingsDir);
        settingsPath = (settingsDir / L"settings.ini").wstring();
        wchar_t userProfile[MAX_PATH]{};
        n = GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
        const fs::path documents = n ? fs::path(userProfile) / L"Documents" : fs::path(appDir);
        captureDir = (documents / L"Scarlett Scout Captures").wstring();
        fs::create_directories(captureDir);
    }
};


LRESULT CALLBACK WaveformProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        app = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{}; GetClientRect(hwnd, &rc);
        HBRUSH waveBg = CreateSolidBrush(RGB(20, 26, 34));
        FillRect(dc, &rc, waveBg);
        DeleteObject(waveBg);
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        if (app && app->audio.isRunning() && width > 4 && height > 8) {
            const auto samples = app->audio.copyLastSeconds(2);
            const size_t frames = samples.size() / 2;
            if (frames <= 2) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(222, 166, 95));
                const DWORD elapsed = GetTickCount() - app->listenStartedAtMs;
                const uint64_t callbacks = app->audio.callbackCount();
                const wchar_t* message = callbacks > 0
                    ? L"AUDIO CALLBACKS RECEIVED, BUT NO SAMPLE FRAMES  ·  Check stream/channel configuration"
                    : (elapsed >= 2500
                        ? L"NO AUDIO DATA RECEIVED  ·  Check selected source / Windows routing"
                        : L"WAITING FOR INPUT DATA  ·  Audio endpoint opened");
                DrawTextW(dc, message, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                HPEN guide = CreatePen(PS_DOT, 1, RGB(55, 68, 83));
                HGDIOBJ oldPen = SelectObject(dc, guide);
                MoveToEx(dc, 0, height / 2, nullptr); LineTo(dc, width, height / 2);
                SelectObject(dc, oldPen); DeleteObject(guide);

                float signalPeak = 0.0f;
                for (float sample : samples) signalPeak = std::max(signalPeak, std::abs(sample));
                auto drawChannel = [&](unsigned channel, int centerY, int amplitude, COLORREF color) {
                    HPEN pen = CreatePen(PS_SOLID, 1, color);
                    HGDIOBJ previous = SelectObject(dc, pen);
                    for (int x = 0; x < width; ++x) {
                        const size_t begin = static_cast<size_t>(x) * frames / static_cast<size_t>(width);
                        size_t end = static_cast<size_t>(x + 1) * frames / static_cast<size_t>(width);
                        end = std::max(end, begin + 1);
                        end = std::min(end, frames);
                        float minimum = 1.0f, maximum = -1.0f;
                        for (size_t i = begin; i < end; ++i) {
                            const float value = samples[i * 2 + channel];
                            minimum = std::min(minimum, value);
                            maximum = std::max(maximum, value);
                        }
                        const int yTop = centerY - static_cast<int>(maximum * amplitude);
                        const int yBottom = centerY - static_cast<int>(minimum * amplitude);
                        MoveToEx(dc, x, yTop, nullptr);
                        LineTo(dc, x, yBottom + 1);
                    }
                    SelectObject(dc, previous); DeleteObject(pen);
                };
                drawChannel(0, height / 4, std::max(4, height / 4 - 3), RGB(78, 192, 165));
                drawChannel(1, (height * 3) / 4, std::max(4, height / 4 - 3), RGB(97, 158, 218));
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(133, 148, 166));
                TextOutW(dc, 6, 1, L"L", 1);
                TextOutW(dc, 6, height / 2 + 1, L"R", 1);
                if (signalPeak < 0.0005f) {
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, RGB(222, 166, 95));
                    RECT messageRc{42, 0, width - 4, height};
                    DrawTextW(dc, L"INPUT ACTIVE · SIGNAL IS SILENT OR VERY LOW", -1, &messageRc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        } else {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(128, 140, 156));
            const wchar_t* message = L"Start listening to see the live waveform";
            DrawTextW(dc, message, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<App*>(cs->lpCreateParams);
        app->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, app->backgroundBrush ? app->backgroundBrush : GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(218, 225, 235)); SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(app->backgroundBrush ? app->backgroundBrush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(224, 231, 240)); SetBkColor(dc, RGB(25, 31, 40));
        return reinterpret_cast<INT_PTR>(app->panelBrush ? app->panelBrush : GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(235, 240, 247)); SetBkColor(dc, RGB(31, 39, 50));
        return reinterpret_cast<INT_PTR>(app->controlBrush ? app->controlBrush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_STATIC) {
            const int id = static_cast<int>(dis->CtlID);
            const bool isPanel = id == ID_PANEL_ROUTING || id == ID_PANEL_ANALYSIS ||
                                 id == ID_PANEL_AUTOCAPTURE || id == ID_PANEL_INFERENCE ||
                                 id == ID_PANEL_HISTORY;
            if (isPanel) {
                RECT rc = dis->rcItem;
                HBRUSH fill = app->panelBrush ? app->panelBrush : GetSysColorBrush(COLOR_BTNFACE);
                FillRect(dis->hDC, &rc, fill);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(49, 61, 77));
                HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
                HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
                RoundRect(dis->hDC, rc.left, rc.top + 7, rc.right, rc.bottom, 10, 10);
                SelectObject(dis->hDC, oldBrush); SelectObject(dis->hDC, oldPen); DeleteObject(pen);
                wchar_t caption[256]{}; GetWindowTextW(dis->hwndItem, caption, 256);
                RECT titleRc{rc.left + 14, rc.top, rc.right - 12, rc.top + 22};
                SetBkColor(dis->hDC, RGB(25, 31, 40)); SetTextColor(dis->hDC, RGB(131, 190, 184));
                SetBkMode(dis->hDC, OPAQUE);
                HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, app->boldFont));
                DrawTextW(dis->hDC, caption, -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
        }
        if (dis && dis->CtlType == ODT_BUTTON) {
            const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
            RECT rc = dis->rcItem;
            const bool primary = GetDlgCtrlID(dis->hwndItem) == ID_LISTEN || GetDlgCtrlID(dis->hwndItem) == ID_CAPTURE;
            const COLORREF fillColor = disabled ? RGB(35, 41, 50) : (primary ? RGB(34, 106, 99) : RGB(42, 53, 67));
            HBRUSH fill = CreateSolidBrush(fillColor);
            HPEN pen = CreatePen(PS_SOLID, 1, primary ? RGB(63, 152, 139) : RGB(67, 81, 101));
            HGDIOBJ oldBrush = SelectObject(dis->hDC, fill);
            HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
            RoundRect(dis->hDC, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 7, 7);
            SelectObject(dis->hDC, oldBrush); SelectObject(dis->hDC, oldPen);
            DeleteObject(fill); DeleteObject(pen);
            wchar_t caption[256]{}; GetWindowTextW(dis->hwndItem, caption, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, disabled ? RGB(125, 134, 148) : RGB(245, 248, 251));
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, app->boldFont));
            DrawTextW(dis->hDC, caption, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &rc);
            return TRUE;
        }
        break;
    }
    case WM_CREATE: {
        app->backgroundBrush = CreateSolidBrush(RGB(17, 21, 28));
        app->panelBrush = CreateSolidBrush(RGB(25, 31, 40));
        app->controlBrush = CreateSolidBrush(RGB(31, 39, 50));
        app->font = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        app->boldFont = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        app->initializePaths();
        app->createControls();
        std::wstring error;
        if (!app->audio.initialize(error)) {
            MessageBoxW(hwnd, error.c_str(), L"Scarlett Scout — WASAPI initialization", MB_ICONERROR);
            SetWindowTextW(app->status, error.c_str());
        } else {
            app->refreshDevices();
            app->loadSettings();
            app->refreshCaptureHistory();
            app->audio.startAnalyzer(app->appDir, app->inferenceDeviceId());
            SetWindowTextW(app->status, L"Ready. Select a playback loopback or recording input endpoint, then choose the channel pair.");
        }
        SetTimer(hwnd, 1, 250, nullptr);
        return 0;
    }
    case WM_TIMER:
        app->updateUi();
        return 0;
    case WM_DEVICECHANGE:
        if (app->audio.isRunning()) { app->audio.stop(); app->setRunningUi(false); }
        SetWindowTextW(app->status, L"Windows audio device topology changed. Use Refresh devices to re-enumerate available endpoints.");
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == ID_SOURCE && code == CBN_SELCHANGE) {
            const int idx = app->selection(app->source);
            if (idx >= 0) {
                app->selectedSourceType = static_cast<int>(app->audio.sources()[static_cast<size_t>(idx)].type);
                app->selectedSourceName = app->audio.sources()[static_cast<size_t>(idx)].name;
            }
            app->populateChannels(); app->saveSettings();
        } else if (id == ID_CHANNEL && code == CBN_SELCHANGE) {
            app->syncModeToChannel(); app->selectedChannelName = app->getComboText(app->channel); app->saveSettings();
        } else if (id == ID_REFRESH) { app->refreshDevices(); app->saveSettings(); }
        else if (id == ID_LISTEN) {
            app->setText(app->status, L"Start Listening selected · opening the chosen WASAPI endpoint...");
            app->startListen();
        }
        else if (id == ID_STOP) {
            app->audio.stop(); app->setRunningUi(false);
        } else if (id == ID_CAPTURE) app->captureNow();
        else if (id == ID_OPEN_FOLDER) ShellExecuteW(hwnd, L"open", app->captureDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == ID_HISTORY && code == LBN_DBLCLK) app->openSelectedCapture();
        else if (id == ID_AUTODETECT || id == ID_SENSITIVITY || id == ID_CHANNEL || id == ID_MODE || id == ID_LENGTH || id == ID_FORMAT) {
            app->applySensitivity(); app->saveSettings();
        }
        return 0;
    }
    case WM_CLOSE:
        app->audio.stopAnalyzer(); app->audio.stop();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app->font) DeleteObject(app->font);
        if (app->boldFont) DeleteObject(app->boldFont);
        if (app->backgroundBrush) DeleteObject(app->backgroundBrush);
        if (app->panelBrush) DeleteObject(app->panelBrush);
        if (app->controlBrush) DeleteObject(app->controlBrush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    App app;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ScarlettScoutMainWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    WNDCLASSEXW waveformClass{};
    waveformClass.cbSize = sizeof(waveformClass);
    waveformClass.lpfnWndProc = WaveformProc;
    waveformClass.hInstance = instance;
    waveformClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    waveformClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    waveformClass.lpszClassName = L"ScarlettScoutWaveform";
    RegisterClassExW(&waveformClass);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Scarlett Scout — Live Sample Intelligence",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 1000, nullptr, nullptr, instance, &app);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
