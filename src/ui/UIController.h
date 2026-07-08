#pragma once
#include "audio/AudioEngine.h"
#include "dsp/DSPGraph.h"
#include "soundboard/Soundboard.h"
#include "license/LicenseManager.h"
#include "config/AppConfig.h"
#include <memory>
#include <string>

class UIController {
public:
    UIController(
        std::shared_ptr<AudioEngine> audioEngine,
        std::shared_ptr<DSPGraph> dspGraph,
        std::shared_ptr<Soundboard> soundboard,
        std::shared_ptr<LicenseManager> license,
        std::shared_ptr<AppConfig> config
    );
    ~UIController();

    // Renders the entire UI frame
    void render();

    // Fill the config object with the current UI/engine/soundboard state
    // (called by main before saving on exit)
    void captureConfig(AppConfig& cfg) const;

private:
    std::shared_ptr<AudioEngine> m_audioEngine;
    std::shared_ptr<DSPGraph> m_dspGraph;
    std::shared_ptr<Soundboard> m_soundboard;
    std::shared_ptr<LicenseManager> m_license;
    std::shared_ptr<AppConfig> m_config;

    // Restore saved device selections (by name) once devices are known
    void applyConfigDevices();

    // Device selection state index
    int m_selectedInputIdx = 0;
    int m_selectedOutputIdx = 0;
    int m_selectedMonitorIdx = 0;

    // GUI status/alert messages
    std::string m_statusMessage = "Engine stopped.";
    bool m_isStatusError = false;

    // Hotkey binding state
    std::shared_ptr<SoundBoardClip> m_bindingClip = nullptr;
    bool m_bindingStopAll = false;

    // License activation input buffer
    char m_licenseKeyInput[128] = {0};

    // Helpers to draw specific sections
    void drawActivationScreen();
    void drawSettingsPanel();
    void drawVUMeters();
    void drawDSPGraphPanel();
    void drawSoundboardPanel();
    
    // Style configuration helper
    void applyDarkModernTheme();

    // Open file dialog wrapper (Windows native)
    std::string openFileDialog();
};
