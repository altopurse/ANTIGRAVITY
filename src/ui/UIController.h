#pragma once
#include "audio/AudioEngine.h"
#include "dsp/DSPGraph.h"
#include "soundboard/Soundboard.h"
#include "license/LicenseManager.h"
#include "config/AppConfig.h"
#include "ads/AdBanner.h"
#include <memory>
#include <string>
#include <set>

class UIController {
public:
    UIController(
        std::shared_ptr<AudioEngine> audioEngine,
        std::shared_ptr<DSPGraph> dspGraph,
        std::shared_ptr<Soundboard> soundboard,
        std::shared_ptr<LicenseManager> license,
        std::shared_ptr<AppConfig> config,
        std::shared_ptr<AdBanner> adBanner
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
    std::shared_ptr<AdBanner> m_adBanner;

    // Restore saved device selections (by name) once devices are known
    void applyConfigDevices();

    // Start the engine with the currently selected devices; updates status
    void startEngine();

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

    // GDPR/Terms consent (mirrors AppConfig::termsAccepted; required to activate)
    bool m_termsAccepted = false;

    // First-launch setup wizard (shown until finished or skipped once)
    bool m_showWizard = false;

    // DSP preset UI state
    int m_selectedPresetIdx = 0;
    char m_presetNameInput[64] = {0};
    std::string m_presetStatus;

    // Collapsed DSP node cards (keyed by effect name; default = expanded)
    std::set<std::string> m_collapsedNodes;

    // "Copied!" feedback timer for the share button
    float m_shareCopiedTimer = 0.0f;

    // Waveform/spectrum scratch (snapshot of the engine's viz tap)
    float m_vizSnap[1024] = {0};
    float m_spectrum[64] = {0};

    // Helpers to draw specific sections
    void drawActivationScreen();
    void drawSetupWizard();
    void drawTopBar();
    void drawSettingsPanel();
    void drawVUMeters();
    void drawDSPGraphPanel();
    void drawSoundboardPanel();

    // Open file dialog wrapper (Windows native)
    std::string openFileDialog();
};
