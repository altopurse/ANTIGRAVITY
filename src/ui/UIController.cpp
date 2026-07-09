#include "UIController.h"
#include "dsp/Effects.h"
#include "imgui.h"
#include <iostream>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

UIController::UIController(
    std::shared_ptr<AudioEngine> audioEngine,
    std::shared_ptr<DSPGraph> dspGraph,
    std::shared_ptr<Soundboard> soundboard,
    std::shared_ptr<LicenseManager> license,
    std::shared_ptr<AppConfig> config,
    std::shared_ptr<AdBanner> adBanner
) : m_audioEngine(audioEngine), m_dspGraph(dspGraph), m_soundboard(soundboard),
    m_license(license), m_config(config), m_adBanner(adBanner) {
    applyDarkModernTheme();
    applyConfigDevices();

    // Pre-fill the activation field with whatever key last worked on this
    // PC (survives Reset License Key) so the user isn't stuck retyping it.
    std::string lastKey = LicenseManager::getLastKey();
    if (!lastKey.empty()) {
        size_t n = std::min(lastKey.size(), sizeof(m_licenseKeyInput) - 1);
        std::memcpy(m_licenseKeyInput, lastKey.data(), n);
        m_licenseKeyInput[n] = '\0';
    }
}

UIController::~UIController() {}

void UIController::applyConfigDevices() {
    if (!m_config) return;
    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name == m_config->inputDevice) { m_selectedInputIdx = static_cast<int>(i); break; }
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name == m_config->outputDevice) { m_selectedOutputIdx = static_cast<int>(i); break; }
    }
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name == m_config->monitorDevice) { m_selectedMonitorIdx = static_cast<int>(i); break; }
    }
}

void UIController::captureConfig(AppConfig& cfg) const {
    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();
    if (m_selectedInputIdx >= 0 && m_selectedInputIdx < static_cast<int>(inputs.size()))
        cfg.inputDevice = inputs[m_selectedInputIdx].name;
    if (m_selectedOutputIdx >= 0 && m_selectedOutputIdx < static_cast<int>(outputs.size()))
        cfg.outputDevice = outputs[m_selectedOutputIdx].name;
    if (m_selectedMonitorIdx >= 0 && m_selectedMonitorIdx < static_cast<int>(outputs.size()))
        cfg.monitorDevice = outputs[m_selectedMonitorIdx].name;

    cfg.bufferMs = m_audioEngine->getBufferSizeMs();
    cfg.exclusiveMode = m_audioEngine->isExclusiveMode();
    cfg.monitorEnabled = m_audioEngine->isMonitorEnabled();
    cfg.monitorVolume = m_audioEngine->getMonitorVolume();
    cfg.soundboardMonitorVolume = m_audioEngine->getSoundboardMonitorVolume();

    auto mixer = m_soundboard->getMixer();
    cfg.duckingEnabled = mixer->m_duckingEnabled;
    cfg.duckingAmount = mixer->m_duckingAmount;
    cfg.stopAllHotkey = m_soundboard->getStopAllHotkey();

    cfg.clips.clear();
    for (const auto& clip : m_soundboard->getClips()) {
        ClipConfig c;
        c.path = clip->path;
        c.hotkey = clip->hotkey;
        c.volume = clip->volume;
        c.loop = clip->loop;
        cfg.clips.push_back(c);
    }
}

void UIController::render() {
    // Gate the whole app behind license activation
    if (m_license && !m_license->isUnlocked()) {
        drawActivationScreen();
        return;
    }

    // Poll global soundboard hotkeys in the UI thread
    m_soundboard->updateHotkeys();

    // Check if we are binding a hotkey (clip or the global stop-all)
    if (m_bindingClip != nullptr || m_bindingStopAll) {
#ifdef _WIN32
        // Scan virtual keys 8..254 (skip mouse buttons VK 1-6 and undefined 7)
        for (int k = 8; k < 255; ++k) {
            if (GetAsyncKeyState(k) & 0x8000) {
                // setters mark the key as already-down, so no Sleep needed
                if (m_bindingStopAll) {
                    m_soundboard->setStopAllHotkey(k);
                    m_bindingStopAll = false;
                } else {
                    m_soundboard->setHotkey(m_bindingClip, k);
                    m_bindingClip = nullptr;
                }
                break;
            }
        }
#endif
    }

    // Set up main window layout
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    
    ImGui::Begin("VoiceChangerMainWindow", nullptr, windowFlags);

    // Header Title
    ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.97f, 1.0f), "ANTIGRAVITY VOICE ENGINE");
    ImGui::TextDisabled("Desktop-Only Ultra-Low-Latency Voice Changer & Soundboard   -   v" APP_VERSION);

    // Update banner (populated by the background version check)
    if (m_license && m_license->isUpdateAvailable()) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        std::string label = "Update v" + m_license->getUpdateVersion() + " available - download";
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.50f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.32f, 1.0f));
        if (ImGui::Button(label.c_str())) {
            m_license->openUpdatePage();
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Main Columns layout: Left = Controls & VU meters, Middle = DSP Graph, Right = Soundboard
    ImGui::Columns(3, "MainColumns", true);
    
    // Set column widths
    static bool setWidths = true;
    if (setWidths) {
        ImGui::SetColumnWidth(0, viewport->WorkSize.x * 0.28f);
        ImGui::SetColumnWidth(1, viewport->WorkSize.x * 0.38f);
        ImGui::SetColumnWidth(2, viewport->WorkSize.x * 0.34f);
        setWidths = false;
    }

    // --- Column 1: Settings, Device Selection, VU Meters ---
    drawSettingsPanel();
    ImGui::Spacing();
    drawVUMeters();
    
    ImGui::NextColumn();

    // --- Column 2: DSP Graph ---
    drawDSPGraphPanel();

    ImGui::NextColumn();

    // --- Column 3: Soundboard ---
    drawSoundboardPanel();

    ImGui::Columns(1);
    ImGui::End();
}

void UIController::drawActivationScreen() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("ActivationWindow", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    // Center a fixed-width column
    float colWidth = 460.0f;
    float startX = (viewport->WorkSize.x - colWidth) * 0.5f;
    if (startX < 0) startX = 0;
    ImGui::SetCursorPosY(viewport->WorkSize.y * 0.22f);

    ImGui::SetCursorPosX(startX);
    ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.97f, 1.0f), "ANTIGRAVITY VOICE ENGINE");
    ImGui::SetCursorPosX(startX);
    ImGui::TextDisabled("License required: 10 GBP lifetime, or 2 GBP/month");
    ImGui::SetCursorPosX(startX);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetCursorPosX(startX);
    if (ImGui::Button("Buy License", ImVec2(colWidth, 36))) {
        m_license->openPurchasePage();
    }
    ImGui::SetCursorPosX(startX);
    ImGui::TextDisabled("Opens the purchase page in your browser. After paying,");
    ImGui::SetCursorPosX(startX);
    ImGui::TextDisabled("copy the license key it shows and paste it below.");
    ImGui::Spacing();

    ImGui::SetCursorPosX(startX);
    ImGui::SetNextItemWidth(colWidth);
    ImGui::InputTextWithHint("##licensekey", "ANTI-XXXXXXXXXX-XXXXXXXXXXXXXXXXXXXX",
                             m_licenseKeyInput, sizeof(m_licenseKeyInput));

    LicenseManager::Status status = m_license->getStatus();
    bool checking = (status == LicenseManager::Status::Checking);

    ImGui::SetCursorPosX(startX);
    ImGui::BeginDisabled(checking);
    if (ImGui::Button("ACTIVATE", ImVec2(colWidth, 36))) {
        m_license->activate(m_licenseKeyInput);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SetCursorPosX(startX);
    switch (status) {
        case LicenseManager::Status::Checking:
            ImGui::TextColored(ImVec4(0.86f, 0.78f, 0.39f, 1.0f),
                               "Verifying... (can take up to a minute if the server was asleep)");
            break;
        case LicenseManager::Status::Invalid:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "That key is not valid. Check for typos and try again.");
            break;
        case LicenseManager::Status::DeviceLimit:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "This key is already active on the maximum number of devices.");
            ImGui::SetCursorPosX(startX);
            if (ImGui::Button("Unbind All Devices & Retry", ImVec2(colWidth, 30))) {
                m_license->unbindAllDevices();
            }
            ImGui::SetCursorPosX(startX);
            ImGui::TextDisabled("Frees the key from every old PC, then activates it here.");
            break;
        case LicenseManager::Status::Expired:
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                               "Your monthly subscription has ended. Renew it or buy a lifetime key.");
            break;
        case LicenseManager::Status::NetworkError:
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Could not reach the license server. Check your internet and retry.");
            break;
        default:
            ImGui::TextDisabled("Enter your license key to unlock the app.");
            break;
    }

    // Ad banner (locked/unlicensed screen only - never shown once activated).
    // Server-controlled: see server.js /api/ad + AD_IMAGE_URL/AD_LINK_URL.
    // Absent entirely if the server hasn't configured one.
    if (m_adBanner && m_adBanner->isReady()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::SetCursorPosX(startX);
        ImGui::Separator();
        ImGui::Spacing();

        float bannerW = colWidth;
        float bannerH = bannerW / m_adBanner->getAspectRatio();
        // Cap height so a tall/odd-shaped ad image can't dominate the screen.
        if (bannerH > 140.0f) {
            bannerH = 140.0f;
            bannerW = bannerH * m_adBanner->getAspectRatio();
        }
        ImGui::SetCursorPosX((viewport->WorkSize.x - bannerW) * 0.5f);

        ImTextureID texId = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(m_adBanner->getTextureId()));
        if (ImGui::ImageButton("##adbanner", texId, ImVec2(bannerW, bannerH))) {
#ifdef _WIN32
            std::string url = m_adBanner->getLinkUrl();
            if (!url.empty()) {
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
#endif
        }
        ImGui::SetItemTooltip("Sponsored");
    }

    ImGui::End();
}

void UIController::drawSettingsPanel() {
    ImGui::BeginChild("SettingsChild", ImVec2(0, 430), true);
    ImGui::Text("Device Configuration");
    ImGui::Separator();
    ImGui::Spacing();

    // Quick routing / output setup instructions
    if (ImGui::CollapsingHeader("Setup Guide - How to route your voice")) {
        ImGui::TextWrapped("1. Install a virtual audio cable (free VB-CABLE from vb-audio.com/Cable), then reboot.");
        ImGui::TextWrapped("2. Mic Input = your real microphone.");
        ImGui::TextWrapped("3. Primary Output = 'CABLE Input (VB-Audio Virtual Cable)'. This carries your processed voice to other apps.");
        ImGui::TextWrapped("4. Voice Monitor = your headphones. Soundboard clips always play here; enable Voice Loopback below to also hear your own processed voice.");
        ImGui::TextWrapped("5. Press START AUDIO ENGINE.");
        ImGui::TextWrapped("6. In Discord / your game, pick 'CABLE Output (VB-Audio Virtual Cable)' as the microphone.");
        ImGui::TextWrapped("No virtual cable? Set Primary Output to your headphones just to try the effects locally.");
        ImGui::Separator();
        ImGui::Spacing();
    }

    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();

    // Input Device Combo
    std::string previewInput = inputs.empty() ? "None Detected" : (m_selectedInputIdx < inputs.size() ? inputs[m_selectedInputIdx].name : "Select Input");
    if (ImGui::BeginCombo("Mic Input", previewInput.c_str())) {
        for (int i = 0; i < inputs.size(); ++i) {
            // PushID: devices can share identical (truncated) names, which
            // would otherwise collide ImGui IDs and break selection.
            ImGui::PushID(i);
            const bool isSelected = (m_selectedInputIdx == i);
            if (ImGui::Selectable(inputs[i].name.c_str(), isSelected)) {
                m_selectedInputIdx = i;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Primary Output Combo (Virtual Mic / Speakers)
    std::string previewOutput = outputs.empty() ? "None Detected" : (m_selectedOutputIdx < outputs.size() ? outputs[m_selectedOutputIdx].name : "Select Output");
    if (ImGui::BeginCombo("Primary Output", previewOutput.c_str())) {
        for (int i = 0; i < outputs.size(); ++i) {
            ImGui::PushID(i);
            const bool isSelected = (m_selectedOutputIdx == i);
            if (ImGui::Selectable(outputs[i].name.c_str(), isSelected)) {
                m_selectedOutputIdx = i;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Monitor Output Combo (Physical Headset)
    std::string previewMonitor = outputs.empty() ? "None Detected" : (m_selectedMonitorIdx < outputs.size() ? outputs[m_selectedMonitorIdx].name : "Select Monitor");
    if (ImGui::BeginCombo("Voice Monitor", previewMonitor.c_str())) {
        for (int i = 0; i < outputs.size(); ++i) {
            ImGui::PushID(i);
            const bool isSelected = (m_selectedMonitorIdx == i);
            if (ImGui::Selectable(outputs[i].name.c_str(), isSelected)) {
                m_selectedMonitorIdx = i;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Duplex latency properties
    bool exclusive = m_audioEngine->isExclusiveMode();
    if (ImGui::Checkbox("Exclusive Mode (WASAPI)", &exclusive)) {
        m_audioEngine->setExclusiveMode(exclusive);
    }
    ImGui::SetItemTooltip("Exclusive mode bypasses Windows Mixer for lowest possible latency (<10ms), but blocks other apps from using this audio device.");

    int bufferMs = m_audioEngine->getBufferSizeMs();
    if (ImGui::SliderInt("Buffer size (ms)", &bufferMs, 5, 100)) {
        m_audioEngine->setBufferSizeMs(bufferMs);
    }
    ImGui::SetItemTooltip("Higher = more stable (no crackling), lower = less latency.\nTakes effect the next time the engine is started.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Start / Stop Control Buttons
    bool active = m_audioEngine->isActive();
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("STOP AUDIO ENGINE", ImVec2(-1, 40))) {
            m_audioEngine->stop();
            m_statusMessage = "Engine stopped.";
            m_isStatusError = false;
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.8f, 0.3f, 1.0f));
        if (ImGui::Button("START AUDIO ENGINE", ImVec2(-1, 40))) {
            const ma_device_id* pInId = (!inputs.empty() && m_selectedInputIdx < inputs.size()) ? &inputs[m_selectedInputIdx].id : nullptr;
            const ma_device_id* pOutId = (!outputs.empty() && m_selectedOutputIdx < outputs.size()) ? &outputs[m_selectedOutputIdx].id : nullptr;
            const ma_device_id* pMonId = (!outputs.empty() && m_selectedMonitorIdx < outputs.size()) ? &outputs[m_selectedMonitorIdx].id : nullptr;
            
            if (m_audioEngine->start(pInId, pOutId, pMonId)) {
                m_statusMessage = "Running (Sample Rate: " + std::to_string(static_cast<int>(m_audioEngine->getSampleRate())) + "Hz)";
                m_isStatusError = false;
            } else {
                m_statusMessage = "Failed to start engine. Check device configuration.";
                m_isStatusError = true;
            }
        }
        ImGui::PopStyleColor(2);
    }

    // Status Message Box
    ImGui::Spacing();
    if (m_isStatusError) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", m_statusMessage.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", m_statusMessage.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Voice monitoring (hear yourself) controls
    bool monitor = m_audioEngine->isMonitorEnabled();
    if (ImGui::Checkbox("Voice Loopback (Monitoring)", &monitor)) {
        m_audioEngine->setMonitorEnabled(monitor);
    }
    ImGui::SetItemTooltip("Toggle hearing your OWN VOICE through the Monitor output.\nSoundboard clips play through the Monitor regardless of this setting\n(as long as a Voice Monitor device is selected and the engine is running).");

    float monVol = m_audioEngine->getMonitorVolume();
    if (ImGui::SliderFloat("Monitor Vol", &monVol, 0.0f, 1.5f, "%.2f")) {
        m_audioEngine->setMonitorVolume(monVol);
    }
    ImGui::SetItemTooltip("Volume of YOUR VOICE through the Monitor output (headphones).");

    float sbMonVol = m_audioEngine->getSoundboardMonitorVolume();
    if (ImGui::SliderFloat("Soundboard Vol (you)", &sbMonVol, 0.0f, 1.5f, "%.2f")) {
        m_audioEngine->setSoundboardMonitorVolume(sbMonVol);
    }
    ImGui::SetItemTooltip("How loud soundboard clips are in YOUR headphones only.\n"
                          "Discord/games always get clips at each clip's own Volume slider,\n"
                          "full strength - this only turns them down for you.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // License management
    ImGui::TextDisabled("License  (app v" APP_VERSION ")");

    std::string plan = m_license->getPlan();
    if (plan == "lifetime") {
        ImGui::TextColored(ImVec4(0.39f, 0.86f, 0.39f, 1.0f), "Plan: Lifetime");
    } else if (plan == "monthly") {
        std::string until = m_license->getPaidUntil();
        std::string label = "Plan: Monthly";
        if (until.size() >= 10) label += " (renews " + until.substr(0, 10) + ")";
        ImGui::TextColored(ImVec4(0.39f, 0.75f, 0.86f, 1.0f), "%s", label.c_str());
    } else {
        ImGui::TextDisabled("Plan: unknown");
    }

    if (ImGui::Button("Unbind All Devices", ImVec2(-1, 0))) {
        m_license->unbindAllDevices();
    }
    ImGui::SetItemTooltip("Frees this key's device slots on ALL PCs at once, then\n"
                          "re-registers this PC automatically. Use after reinstalling\n"
                          "Windows or replacing machines when slots are used up.");

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Reset License Key", ImVec2(-1, 0))) {
        m_audioEngine->stop();
        m_license->reset();
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Deactivates this PC and clears the saved key.\n"
                          "Frees the device slot so the key can be used on another PC.\n"
                          "You'll need to enter a license key again to use the app.");

    ImGui::EndChild();
}

void UIController::drawVUMeters() {
    ImGui::BeginChild("VUChild", ImVec2(0, 0), true);
    ImGui::Text("Audio Levels & Latency");
    ImGui::Separator();
    ImGui::Spacing();

    float micVal = m_audioEngine->getMicLevel();
    float outVal = m_audioEngine->getOutputLevel();

    // Latency details
    if (m_audioEngine->isActive()) {
        ImGui::Text("Reported Frame Size: %d ms", m_audioEngine->getBufferSizeMs());
        if (m_audioEngine->isExclusiveMode()) {
            ImGui::TextColored(ImVec4(0.39f, 0.86f, 0.39f, 1.0f), "Latency Class: Ultra-Low (<10ms)");
        } else {
            ImGui::TextColored(ImVec4(0.86f, 0.78f, 0.39f, 1.0f), "Latency Class: Standard (~20ms)");
        }
    } else {
        ImGui::Text("Engine Offline");
    }

    ImGui::Spacing();

    // Mic Level Bar
    ImGui::Text("Microphone:");
    ImGui::ProgressBar(micVal, ImVec2(-1, 16), "");

    ImGui::Spacing();

    // Output Level Bar
    ImGui::Text("Output Mix:");
    ImGui::ProgressBar(outVal, ImVec2(-1, 16), "");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Diagnostic Telemetry:");
    if (m_audioEngine->isActive()) {
        ImGui::Text("Primary Callbacks: %llu", m_audioEngine->getDbgPrimaryCallbacks());
        ImGui::Text("Monitor Callbacks: %llu", m_audioEngine->getDbgMonitorCallbacks());
        ImGui::TextColored(m_audioEngine->getDbgUnderflows() > 0 ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.5f, 0.9f, 0.5f, 1.0f),
                           "Underflows (Pops): %llu", m_audioEngine->getDbgUnderflows());
        ImGui::Text("Overflows: %llu", m_audioEngine->getDbgOverflows());
        ImGui::Text("Buffer Cushion: %d samples", static_cast<int>(m_audioEngine->getDbgRingBufferLevel()));
        if (ImGui::Button("Reset Diagnostics")) {
            m_audioEngine->resetDbgMetrics();
        }
    } else {
        ImGui::TextDisabled("Telemetry Offline (Start Engine)");
    }

    ImGui::EndChild();
}

void UIController::drawDSPGraphPanel() {
    ImGui::BeginChild("DSPChild", ImVec2(0, 0), true);
    ImGui::Text("Real-Time DSP Chain Editor");
    ImGui::Separator();
    ImGui::Spacing();

    const auto& nodes = m_dspGraph->getNodes();
    
    if (nodes.empty()) {
        ImGui::TextDisabled("No active nodes in the DSP graph.");
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        ImGui::PushID(static_cast<int>(i));

        // Enable toggle + reorder arrows are drawn BEFORE the header on the
        // same row (no overlap tricks), so every control is always clickable.
        ImGui::Checkbox("##enabled", &node->isEnabled());
        ImGui::SetItemTooltip("Enable / bypass this effect.");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            m_dspGraph->moveNodeUp(i);
        }
        ImGui::SetItemTooltip("Move earlier in the chain.");
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            m_dspGraph->moveNodeDown(i);
        }
        ImGui::SetItemTooltip("Move later in the chain.");
        ImGui::SameLine();

        // Background styling depending on state
        bool enabled = node->isEnabled();
        if (enabled) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.22f, 0.35f, 0.7f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        }

        char label[128];
        snprintf(label, sizeof(label), "%s##node_hdr", node->getName());
        bool isOpen = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

        ImGui::PopStyleColor();

        if (isOpen) {
            ImGui::Indent(15.0f);
            ImGui::Spacing();

            // Downcast nodes to edit custom parameters
            if (auto* gate = dynamic_cast<NoiseGateNode*>(node.get())) {
                ImGui::SliderFloat("Threshold (dB)", &gate->m_thresholdDB, -70.0f, -10.0f, "%.1f dB");
                ImGui::SliderFloat("Release Time (ms)", &gate->m_releaseMs, 20.0f, 600.0f, "%.0f ms");
            } 
            else if (auto* comp = dynamic_cast<CompressorNode*>(node.get())) {
                ImGui::SliderFloat("Threshold (dB)", &comp->m_thresholdDB, -40.0f, 0.0f, "%.1f dB");
                ImGui::SliderFloat("Ratio", &comp->m_ratio, 1.0f, 20.0f, "%.1f:1");
                ImGui::SliderFloat("Attack (ms)", &comp->m_attackMs, 1.0f, 100.0f, "%.1f ms");
                ImGui::SliderFloat("Release (ms)", &comp->m_releaseMs, 10.0f, 500.0f, "%.0f ms");
                ImGui::SliderFloat("Makeup Gain (dB)", &comp->m_makeupGainDB, 0.0f, 24.0f, "%.1f dB");
            } 
            else if (auto* eq = dynamic_cast<ParametricEQNode*>(node.get())) {
                if (ImGui::TreeNode("Low Shelf")) {
                    ImGui::SliderFloat("Freq (Hz)##low", &eq->m_lowShelfFreq, 50.0f, 600.0f, "%.0f Hz");
                    ImGui::SliderFloat("Gain (dB)##low", &eq->m_lowShelfGain, -15.0f, 15.0f, "%.1f dB");
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("Mid Peaking")) {
                    ImGui::SliderFloat("Freq (Hz)##mid", &eq->m_midFreq, 200.0f, 6000.0f, "%.0f Hz");
                    ImGui::SliderFloat("Q factor##mid", &eq->m_midQ, 0.1f, 5.0f, "%.2f");
                    ImGui::SliderFloat("Gain (dB)##mid", &eq->m_midGain, -15.0f, 15.0f, "%.1f dB");
                    ImGui::TreePop();
                }
                if (ImGui::TreeNode("High Shelf")) {
                    ImGui::SliderFloat("Freq (Hz)##high", &eq->m_highShelfFreq, 2000.0f, 16000.0f, "%.0f Hz");
                    ImGui::SliderFloat("Gain (dB)##high", &eq->m_highShelfGain, -15.0f, 15.0f, "%.1f dB");
                    ImGui::TreePop();
                }
            } 
            else if (auto* pitch = dynamic_cast<PitchShifterNode*>(node.get())) {
                ImGui::SliderFloat("Pitch Factor", &pitch->m_pitchFactor, 0.5f, 2.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button("Reset")) { pitch->m_pitchFactor = 1.0f; }

                // Quick presets
                if (ImGui::Button("Deep Voice")) { pitch->m_pitchFactor = 0.72f; } ImGui::SameLine();
                if (ImGui::Button("Minion/Chipmunk")) { pitch->m_pitchFactor = 1.45f; } ImGui::SameLine();
                if (ImGui::Button("Female preset")) { pitch->m_pitchFactor = 1.2f; } ImGui::SameLine();
                if (ImGui::Button("Male preset")) { pitch->m_pitchFactor = 0.85f; }

                ImGui::SliderFloat("Dry / Wet", &pitch->m_dryWet, 0.0f, 1.0f, "%.2f");
            } 
            else if (auto* robot = dynamic_cast<RobotizerNode*>(node.get())) {
                ImGui::SliderFloat("Modulation Freq (Hz)", &robot->m_modFreq, 30.0f, 250.0f, "%.1f Hz");
                ImGui::SliderFloat("Dry / Wet", &robot->m_dryWet, 0.0f, 1.0f, "%.2f");
            } 
            else if (auto* reverb = dynamic_cast<ReverbNode*>(node.get())) {
                ImGui::SliderFloat("Room Size", &reverb->m_roomSize, 0.1f, 0.95f, "%.2f");
                ImGui::SliderFloat("Damping", &reverb->m_damping, 0.0f, 0.8f, "%.2f");
                ImGui::SliderFloat("Dry / Wet", &reverb->m_dryWet, 0.0f, 1.0f, "%.2f");
            } 
            else if (auto* dist = dynamic_cast<DistortionNode*>(node.get())) {
                ImGui::SliderFloat("Drive (Gain)", &dist->m_drive, 1.0f, 15.0f, "%.1f");
                ImGui::SliderFloat("Dry / Wet", &dist->m_dryWet, 0.0f, 1.0f, "%.2f");
            } 
            else if (auto* tel = dynamic_cast<TelephoneFilterNode*>(node.get())) {
                ImGui::Text("Bandpass 300Hz-3.4kHz + Crunch");
                ImGui::SliderFloat("Dry / Wet", &tel->m_dryWet, 0.0f, 1.0f, "%.2f");
            }

            ImGui::Spacing();
            ImGui::Unindent(15.0f);
            ImGui::Separator();
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
}

void UIController::drawSoundboardPanel() {
    ImGui::BeginChild("SoundboardChild", ImVec2(0, 0), true);
    
    ImGui::Text("Soundboard Engine  v" APP_VERSION);
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    if (ImGui::Button("ADD AUDIO FILE")) {
        std::string path = openFileDialog();
        if (!path.empty()) {
            // Copies the file into the app's "sounds" folder and loads it
            // from there, so the clip survives if the original moves.
            m_soundboard->importSound(path);
        }
    }
    if (ImGui::IsItemHovered()) {
        static std::string soundsDir = Soundboard::getSoundsDirectory();
        ImGui::SetTooltip("Files are copied into:\n%s\nSounds in that folder load automatically at startup.", soundsDir.c_str());
    }

    ImGui::Separator();

    // Mic ducking controls (lower the mic while a sound is playing)
    auto mixer = m_soundboard->getMixer();
    ImGui::Checkbox("Duck Mic", &mixer->m_duckingEnabled);
    ImGui::SetItemTooltip("Lower your microphone while a sound is playing.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Mic level##duck", &mixer->m_duckingAmount, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("Mic volume while sounds play (0 = muted, 1 = unchanged).");

    // Stop-all: button + bindable global hotkey
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.25f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.35f, 0.15f, 1.0f));
    if (ImGui::Button("STOP ALL SOUNDS", ImVec2(160, 0))) {
        m_soundboard->stopAll();
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Fade out and stop every playing clip.");

    ImGui::SameLine();
    std::string stopAllLabel = "Hotkey: ";
    if (m_bindingStopAll) {
        stopAllLabel += "[Press Key]";
    } else {
        stopAllLabel += Soundboard::getKeyName(m_soundboard->getStopAllHotkey());
    }
    if (ImGui::Button((stopAllLabel + "##stopall").c_str(), ImVec2(-1, 0))) {
        m_bindingStopAll = true;
        m_bindingClip = nullptr;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Left-click to bind a global stop-all key.\nRight-click to clear.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        m_soundboard->clearStopAllHotkey();
        m_bindingStopAll = false;
    }

    ImGui::Separator();
    ImGui::Spacing();

    auto& clips = m_soundboard->getClips();
    
    if (clips.empty()) {
        ImGui::TextDisabled("No sounds loaded. Click 'ADD AUDIO FILE' to load WAV/MP3 files.");
    }

    // Grid implementation for soundboard clips (2 columns)
    ImGui::Columns(2, "SoundboardGrid", false);
    
    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];
        ImGui::PushID(static_cast<int>(i));

        // Draw a nice boxed area for each clip
        ImGui::BeginChild("ClipCard", ImVec2(0, 150), true, ImGuiWindowFlags_NoScrollbar);
        
        // Clip Name (clipped)
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.95f, 1.0f), "%s", clip->name.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        // Control Buttons
        if (clip->isPlaying) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.7f, 0.2f, 1.0f));
            if (ImGui::Button("PLAYING", ImVec2(60, 24))) {
                m_soundboard->playClip(clip);
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("PLAY", ImVec2(60, 24))) {
                m_soundboard->playClip(clip);
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("STOP", ImVec2(60, 24))) {
            m_soundboard->stopClip(clip);
        }

        ImGui::Spacing();
        
        // Loop Toggle
        ImGui::Checkbox("Loop", &clip->loop);
        
        // Volume Slider
        ImGui::SliderFloat("Vol", &clip->volume, 0.0f, 1.0f, "%.1f");

        // Hotkey Button
        std::string keyLabel = "Hotkey: ";
        if (m_bindingClip == clip) {
            keyLabel += "[Press Key]";
        } else {
            keyLabel += Soundboard::getKeyName(clip->hotkey);
        }

        if (ImGui::Button(keyLabel.c_str(), ImVec2(-1, 22))) {
            m_bindingClip = clip;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Left-click to bind key. Right-click card/button to clear.");
        }
        
        // Right click for hotkey/removal actions
        if (ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem("Clear Hotkey")) {
                m_soundboard->clearHotkey(clip);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from Board")) {
                m_soundboard->stopClip(clip);
                m_soundboard->removeSound(clip, /*deleteFile=*/false);
            }
            if (ImGui::MenuItem("Remove & Delete File")) {
                m_soundboard->stopClip(clip);
                m_soundboard->removeSound(clip, /*deleteFile=*/true);
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();

        ImGui::PopID();
        ImGui::NextColumn();
    }

    ImGui::Columns(1);
    ImGui::EndChild();
}

std::string UIController::openFileDialog() {
#ifdef _WIN32
    char filename[260] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = sizeof(filename);
    ofn.lpstrFilter = "Audio Files (*.wav;*.mp3;*.ogg;*.flac)\0*.wav;*.mp3;*.ogg;*.flac\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(filename);
    }
#endif
    return "";
}

void UIController::applyDarkModernTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.5f;

    ImVec4* colors = style.Colors;
    
    // Deep black-gray background colors
    colors[ImGuiCol_Text]                   = ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg]               = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.09f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.12f, 0.12f, 0.16f, 0.98f);
    
    // Subtle borders
    colors[ImGuiCol_Border]                 = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Sleek frame/widgets (sliders, checkboxes, comboboxes)
    colors[ImGuiCol_FrameBg]                = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.25f, 0.25f, 0.32f, 1.00f);
    
    // Headers & Collapsing categories
    colors[ImGuiCol_Header]                 = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.22f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.28f, 0.28f, 0.38f, 1.00f);

    // Active sky-blue highlights for buttons and sliders
    colors[ImGuiCol_Button]                 = ImVec4(0.16f, 0.44f, 0.65f, 1.00f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.22f, 0.58f, 0.85f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.28f, 0.69f, 0.98f, 1.00f);
    
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.22f, 0.58f, 0.85f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.28f, 0.69f, 0.98f, 1.00f);
    
    colors[ImGuiCol_CheckMark]              = ImVec4(0.28f, 0.69f, 0.98f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.10f, 0.10f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.24f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
    
    colors[ImGuiCol_PlotLines]              = ImVec4(0.22f, 0.58f, 0.85f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(0.28f, 0.69f, 0.98f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.22f, 0.58f, 0.85f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(0.28f, 0.69f, 0.98f, 1.00f);
}
