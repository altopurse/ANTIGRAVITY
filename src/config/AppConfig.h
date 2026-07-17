#pragma once
#include <string>
#include <vector>

// Persists user settings between sessions in %APPDATA%/Antigravity/config.json:
// device selections, engine options, monitoring, ducking, the stop-all hotkey,
// and per-clip soundboard state (hotkey / volume / loop), matched by file path.
//
// The file is written with a minimal JSON emitter and read back with a
// tolerant scanner that only understands what we write - no dependencies.
struct ClipConfig {
    std::string path;
    int hotkey = -1;
    float volume = 0.8f;
    bool loop = false;
    float startSec = 0.0f; // trim in-point (seconds)
    float endSec = 0.0f;   // trim out-point (seconds); 0 = end of file
};

class AppConfig {
public:
    // Device selections (stored by name; names survive device re-ordering)
    std::string inputDevice;
    std::string outputDevice;
    std::string monitorDevice;

    // Engine
    int bufferMs = 20;
    bool exclusiveMode = false;

    // Output loudness stage (Primary Output / Discord feed): makeup gain +
    // look-ahead limiter for consistent, gate-friendly loudness.
    bool outputStageEnabled = true;
    float outputGainDb = 4.0f;

    // Monitoring
    bool monitorEnabled = false;
    float monitorVolume = 0.7f;
    // Soundboard clip loudness on the Voice Monitor (you) only - independent
    // of the Primary Output (what Discord/games hear), which always plays
    // clips at their own Volume slider.
    float soundboardMonitorVolume = 1.0f;

    // Soundboard
    bool duckingEnabled = true;
    float duckingAmount = 0.5f;
    int stopAllHotkey = -1;
    std::vector<ClipConfig> clips;

    // Whole DSP chain state (DspPresets::serialize blob) so effect settings
    // survive restarts too
    std::string dspState;

    // First-launch setup wizard: shown until completed or skipped once
    bool setupDone = false;

    // GDPR / Terms of Service acceptance (recorded at activation)
    bool termsAccepted = false;

    // %APPDATA%/Antigravity/config.json (folder created on demand)
    static std::string configFilePath();

    bool load();       // false if no config existed yet
    bool save() const;
};
