#include "UIController.h"
#include "Theme.h"
#include "Widgets.h"
#include "util/CrashReporter.h"
#include "dsp/Effects.h"
#include "dsp/DspPresets.h"
#include "license/Entitlement.h"
#include "imgui.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

using theme::S;

namespace {

// In-place iterative radix-2 FFT (real input in re[], im[] zeroed by caller).
// n must be a power of two. Plenty fast for the 512-point display spectrum.
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) { // bit-reversal permutation
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.28318530718f / static_cast<float>(len);
        float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

// Full-width combo with a small dim label above it.
// Returns the (possibly changed) selected index.
int labeledDeviceCombo(const char* label, const char* id,
                       const std::vector<AudioDeviceRef>& devices,
                       int selectedIdx, const char* emptyText) {
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "%s", label);
    ImGui::PopFont();
    std::string preview = devices.empty() ? emptyText :
        (selectedIdx >= 0 && selectedIdx < static_cast<int>(devices.size())
             ? devices[selectedIdx].name : "Select");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
            // PushID: devices can share identical (truncated) names, which
            // would otherwise collide ImGui IDs and break selection.
            ImGui::PushID(i);
            const bool isSelected = (selectedIdx == i);
            if (ImGui::Selectable(devices[i].name.c_str(), isSelected)) {
                selectedIdx = i;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return selectedIdx;
}

} // namespace

UIController::UIController(
    std::shared_ptr<AudioEngine> audioEngine,
    std::shared_ptr<DSPGraph> dspGraph,
    std::shared_ptr<Soundboard> soundboard,
    std::shared_ptr<LicenseManager> license,
    std::shared_ptr<AppConfig> config,
    std::shared_ptr<AdBanner> adBanner
) : m_audioEngine(audioEngine), m_dspGraph(dspGraph), m_soundboard(soundboard),
    m_license(license), m_config(config), m_adBanner(adBanner) {
    applyConfigDevices();

    // First launch (or wizard never finished): walk through device setup
    m_showWizard = m_config && !m_config->setupDone;

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
    cfg.outputStageEnabled = m_audioEngine->isOutputStageEnabled();
    cfg.outputGainDb = m_audioEngine->getOutputGainDb();

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
        c.startSec = clip->startSec;
        c.endSec = clip->endSec;
        cfg.clips.push_back(c);
    }

    // Whole DSP chain (order, enabled flags, params) survives restarts
    cfg.dspState = DspPresets::serialize(*m_dspGraph);
}

void UIController::persistConfig() {
    if (!m_config) return;
    captureConfig(*m_config);
    m_config->save();
}

void UIController::startEngine() {
    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();
    const ma_device_id* pInId = (!inputs.empty() && m_selectedInputIdx < static_cast<int>(inputs.size())) ? &inputs[m_selectedInputIdx].id : nullptr;
    const ma_device_id* pOutId = (!outputs.empty() && m_selectedOutputIdx < static_cast<int>(outputs.size())) ? &outputs[m_selectedOutputIdx].id : nullptr;
    const ma_device_id* pMonId = (!outputs.empty() && m_selectedMonitorIdx < static_cast<int>(outputs.size())) ? &outputs[m_selectedMonitorIdx].id : nullptr;

    if (m_audioEngine->start(pInId, pOutId, pMonId)) {
        m_statusMessage = "Running (Sample Rate: " + std::to_string(static_cast<int>(m_audioEngine->getSampleRate())) + "Hz)";
        m_isStatusError = false;
    } else {
        m_statusMessage = "Failed to start engine. Check device configuration.";
        m_isStatusError = true;
    }
}

void UIController::render() {
    // Periodic anti-tamper check (updates the 'clean' half of the entitlement
    // guard). Runs regardless of lock state, throttled to keep it cheap.
    static float tamperAccum = 1.5f; // first check soon after launch
    tamperAccum += ImGui::GetIO().DeltaTime;
    if (tamperAccum >= 2.0f) {
        tamperAccum = 0.0f;
        ent::runTamperChecks();
    }

    // Freemium: the app is usable without a licence (free tier - reverb +
    // pitch, 2 soundboard clips). Enforcement is engine-level (ent::hasFeature),
    // NOT this screen, so there's no security in gating the UI. The activation
    // screen is now an on-demand overlay reached from the Upgrade button / the
    // License section, shown here only when explicitly requested.
    // A running Pro trial counts as Pro for the UI (padlocks open) - the
    // engine-level gates follow the same entitlement mask, and tickTrial
    // handles the drop back to FREE when the clock runs out.
    ent::tickTrial();
    m_isPro = (m_license && m_license->isUnlocked()) || ent::trialActive();

    if (m_showActivation) {
        drawActivationScreen();
        return;
    }

    // First-launch device setup wizard (also the first place consent is asked)
    if (m_showWizard) {
        drawSetupWizard();
        return;
    }

    // Poll global soundboard hotkeys in the UI thread
    m_soundboard->updateHotkeys();

    // Quick keys: 1-9 play soundboard clips 1-9 while the app is focused
    // (skipped while any text field wants keyboard input)
    if (!ImGui::GetIO().WantTextInput && m_bindingClip == nullptr && !m_bindingStopAll) {
        auto& clips = m_soundboard->getClips();
        for (int n = 0; n < 9 && n < static_cast<int>(clips.size()); ++n) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + n), false)) {
                m_soundboard->playClip(clips[n]);
            }
        }
    }

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
                persistConfig(); // bindings survive a crash/kill
                break;
            }
        }
#endif
    }

    // Fullscreen host window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("VoiceChangerMainWindow", nullptr, windowFlags);

    drawTopBar();

    // Three resizable panes: Devices+Levels | Effect chain | Soundboard
    ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##layout", 3, tableFlags)) {
        ImGui::TableSetupColumn("left",   ImGuiTableColumnFlags_WidthStretch, 0.29f);
        ImGui::TableSetupColumn("middle", ImGuiTableColumnFlags_WidthStretch, 0.37f);
        ImGui::TableSetupColumn("right",  ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        drawSettingsPanel();
        ImGui::Spacing();
        drawVUMeters();

        ImGui::TableSetColumnIndex(1);
        drawDSPGraphPanel();

        ImGui::TableSetColumnIndex(2);
        drawSoundboardPanel();

        ImGui::EndTable();
    }

    ImGui::End();
}

void UIController::drawTopBar() {
    float barTop = ImGui::GetCursorPosY();

    // Left: brand block
    ImGui::PushFont(theme::FontHeading);
    ImGui::TextColored(theme::Accent, "ANTIGRAVITY");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("VOICE ENGINE");
    ImGui::PopFont();
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Ultra-low-latency voice changer & soundboard  -  v" APP_VERSION);
    ImGui::PopFont();
    float afterBrandY = ImGui::GetCursorPosY();

    // Right cluster, absolutely positioned on the bar: [update?] [pill] [start/stop]
    bool active = m_audioEngine->isActive();
    bool hasUpdate = m_license && m_license->isUpdateAvailable();

    float btnW = S(190.0f);
    float btnH = S(38.0f);
    std::string pillText = active
        ? "Running @ " + std::to_string(static_cast<int>(m_audioEngine->getSampleRate())) + " Hz"
        : "Engine stopped";
    ImGui::PushFont(theme::FontSmall);
    float pillW = ImGui::CalcTextSize(pillText.c_str()).x + S(38.0f);
    float pillH = ImGui::GetFrameHeight();
    ImGui::PopFont();

    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float right = ImGui::GetWindowContentRegionMax().x;
    float centerY = barTop + S(26.0f);

    float x = right - btnW;
    ImGui::SetCursorPos(ImVec2(x, centerY - btnH * 0.5f));
    if (active) {
        if (widgets::ColoredButton("STOP ENGINE", ImVec2(btnW, btnH), theme::RedLo)) {
            m_audioEngine->stop();
            m_statusMessage = "Engine stopped.";
            m_isStatusError = false;
        }
    } else {
        if (widgets::ColoredButton("START ENGINE", ImVec2(btnW, btnH), theme::GreenLo)) {
            startEngine();
        }
    }

    x -= spacing + pillW;
    ImGui::SetCursorPos(ImVec2(x, centerY - pillH * 0.5f));
    widgets::StatusPill(pillText.c_str(), active ? theme::Green : theme::TextDim);

    if (hasUpdate) {
        // One-click updater: downloads the installer in the background and
        // runs it (the app closes itself so the exe can be replaced). If the
        // download fails, the button falls back to opening the browser.
        LicenseManager::UpdateState ust = m_license->getUpdateState();
        std::string updateLabel;
        switch (ust) {
            case LicenseManager::UpdateState::Downloading: updateLabel = "Downloading update..."; break;
            case LicenseManager::UpdateState::Ready:       updateLabel = "Starting installer..."; break;
            case LicenseManager::UpdateState::Failed:      updateLabel = "Download failed - open page"; break;
            default: updateLabel = "Update v" + m_license->getUpdateVersion() + " available"; break;
        }
        float updateW = ImGui::CalcTextSize(updateLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        x -= spacing + updateW;
        ImGui::SetCursorPos(ImVec2(x, centerY - ImGui::GetFrameHeight() * 0.5f));
        if (widgets::ColoredButton(updateLabel.c_str(), ImVec2(0, 0),
                ust == LicenseManager::UpdateState::Failed ? theme::RedLo : theme::GreenLo)) {
            if (ust == LicenseManager::UpdateState::Failed) {
                m_license->openUpdatePage();
            } else if (ust == LicenseManager::UpdateState::Idle) {
                m_license->startUpdateDownload();
            }
        }
        switch (ust) {
            case LicenseManager::UpdateState::Downloading:
                ImGui::SetItemTooltip("Downloading the new version in the background..."); break;
            case LicenseManager::UpdateState::Failed:
                ImGui::SetItemTooltip("The automatic download didn't work - this opens the download page instead."); break;
            default:
                ImGui::SetItemTooltip("Downloads and installs the new version.\nYour settings and Pro key are kept."); break;
        }
    }

    // Free tier: an "Upgrade" button + FREE pill, the always-visible conversion
    // hook. During a Pro trial: the upgrade button plus a live countdown pill
    // (urgency sells). Real Pro users see neither (nothing to sell them).
    bool licensed = m_license && m_license->isUnlocked();
    bool onTrial = ent::trialActive();
    if (!licensed) {
        std::string upLabel = "Upgrade to Pro";
        float upW = ImGui::CalcTextSize(upLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + S(4.0f);
        x -= spacing + upW;
        ImGui::SetCursorPos(ImVec2(x, centerY - btnH * 0.5f));
        if (widgets::ColoredButton(upLabel.c_str(), ImVec2(0, btnH), theme::Accent)) {
            m_showActivation = true;
        }
        ImGui::SetItemTooltip("Unlock all 9 effects and unlimited soundboard clips.");

        if (onTrial) {
            int left = ent::trialSecondsLeft();
            char pill[32];
            snprintf(pill, sizeof(pill), "TRIAL %d:%02d", left / 60, left % 60);
            ImGui::PushFont(theme::FontSmall);
            float trialW = ImGui::CalcTextSize(pill).x + S(38.0f);
            ImGui::PopFont();
            x -= spacing + trialW;
            ImGui::SetCursorPos(ImVec2(x, centerY - pillH * 0.5f));
            widgets::StatusPill(pill, theme::Green);
            ImGui::SetItemTooltip("Every Pro effect is live - this is what your friends would hear.");
        } else {
            if (!ent::trialUsed()) {
                std::string tryLabel = "Try Pro free - 10 min";
                float tryW = ImGui::CalcTextSize(tryLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + S(4.0f);
                x -= spacing + tryW;
                ImGui::SetCursorPos(ImVec2(x, centerY - btnH * 0.5f));
                if (widgets::ColoredButton(tryLabel.c_str(), ImVec2(0, btnH), theme::GreenLo)) {
                    ent::startTrial(10 * 60);
                }
                ImGui::SetItemTooltip("Hear all 9 effects and unlimited clips for 10 minutes.\nNo payment, no account - it just switches on.");
            }
            ImGui::PushFont(theme::FontSmall);
            float freeW = ImGui::CalcTextSize("FREE").x + S(38.0f);
            ImGui::PopFont();
            x -= spacing + freeW;
            ImGui::SetCursorPos(ImVec2(x, centerY - pillH * 0.5f));
            widgets::StatusPill("FREE", theme::Yellow);
        }
    }

    ImGui::SetCursorPosY(std::max(afterBrandY, centerY + btnH * 0.5f));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

void UIController::drawActivationScreen() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("ActivationWindow", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    // Centered activation card
    float cardW = S(520.0f);
    float startX = (viewport->WorkSize.x - cardW) * 0.5f;
    if (startX < 0) startX = 0;
    ImGui::SetCursorPosY(viewport->WorkSize.y * 0.12f);
    ImGui::SetCursorPosX(startX);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26.0f), S(24.0f)));
    ImGui::BeginChild("ActivationCard", ImVec2(cardW, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_AlwaysUseWindowPadding);

    // Header row with a "back to Free" exit (activation is optional now)
    ImGui::PushFont(theme::FontHeading);
    ImGui::TextColored(theme::Accent, "UPGRADE TO");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("PRO");
    ImGui::PopFont();
    ImGui::SameLine();
    {
        const char* back = "Continue with Free  X";
        float bw = ImGui::CalcTextSize(back).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        if (widgets::ColoredButton(back, ImVec2(0, 0), theme::PanelHover)) {
            m_showActivation = false;
        }
    }
    ImGui::TextColored(theme::TextDim, "Pro unlocks all 9 effects and unlimited soundboard clips.");
    ImGui::TextColored(theme::TextDim, "10 GBP lifetime, or 2 GBP/month. Free tier keeps working either way.");
    widgets::Divider();

    if (widgets::ColoredButton("Buy License", ImVec2(-FLT_MIN, S(38.0f)), theme::Accent)) {
        m_license->openPurchasePage();
    }
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Opens the purchase page in your browser. After paying,");
    ImGui::TextColored(theme::TextDim, "copy the license key it shows and paste it below.");
    ImGui::PopFont();
    ImGui::Spacing();

    // Auto-close the overlay the moment entitlement flips to Pro (activation
    // verifies on a background thread; this catches the transition).
    if (m_isPro) m_showActivation = false;

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##licensekey", "ANTI-XXXXXXXXXX-XXXXXXXXXXXXXXXXXXXX",
                             m_licenseKeyInput, sizeof(m_licenseKeyInput));

    // GDPR / Terms consent - required before activation. Once accepted it's
    // remembered in the config so returning users aren't nagged every launch.
    if (m_config && m_config->termsAccepted) m_termsAccepted = true;
    ImGui::Checkbox("##terms", &m_termsAccepted);
    ImGui::SameLine(0.0f, S(6.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("I accept the");
    ImGui::SameLine(0.0f, S(4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentHi);
    if (ImGui::SmallButton("Terms & Privacy")) {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", "https://antigravity-license.onrender.com/legal",
                      nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, S(4.0f));
    ImGui::TextDisabled("(GDPR compliant)");
    if (m_config) m_config->termsAccepted = m_termsAccepted;
    CrashReporter::setConsent(m_termsAccepted);

    LicenseManager::Status status = m_license->getStatus();
    bool checking = (status == LicenseManager::Status::Checking);

    ImGui::BeginDisabled(checking || !m_termsAccepted);
    if (widgets::ColoredButton("ACTIVATE", ImVec2(-FLT_MIN, S(38.0f)), theme::GreenLo)) {
        m_license->activate(m_licenseKeyInput);
    }
    ImGui::EndDisabled();
    if (!m_termsAccepted) {
        ImGui::TextColored(theme::TextDim, "Tick the box above to enable activation.");
    }

    ImGui::Spacing();
    switch (status) {
        case LicenseManager::Status::Checking:
            ImGui::TextColored(theme::Yellow,
                               "Verifying... (can take up to a minute if the server was asleep)");
            break;
        case LicenseManager::Status::Invalid:
            ImGui::TextColored(theme::Red,
                               "That key is not valid. Check for typos and try again.");
            break;
        case LicenseManager::Status::DeviceLimit:
            ImGui::TextColored(theme::Red,
                               "This key is already active on the maximum number of devices.");
            ImGui::TextColored(theme::TextDim, "Changed PCs? Contact support with your key to free a slot.");
            break;
        case LicenseManager::Status::Expired:
            ImGui::TextColored(theme::Orange,
                               "Your monthly subscription has ended. Renew it or buy a lifetime key.");
            break;
        case LicenseManager::Status::NetworkError:
            ImGui::TextColored(theme::Orange,
                               "Could not reach the license server. Check your internet and retry.");
            break;
        default:
            ImGui::TextColored(theme::TextDim, "Enter your license key to unlock the app.");
            break;
    }

    // Ad banner (locked/unlicensed screen only - never shown once activated).
    // Server-controlled: see server.js /api/ad + AD_IMAGE_URL/AD_LINK_URL.
    // Absent entirely if the server hasn't configured one.
    if (m_adBanner && m_adBanner->isReady()) {
        widgets::Divider();

        float bannerW = ImGui::GetContentRegionAvail().x;
        float bannerH = bannerW / m_adBanner->getAspectRatio();
        // Cap height so a tall/odd-shaped ad image can't dominate the screen.
        if (bannerH > S(140.0f)) {
            bannerH = S(140.0f);
            bannerW = bannerH * m_adBanner->getAspectRatio();
        }
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bannerW) * 0.5f);

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

    // Small, honest disclosure at the consent moment (activating starts the
    // app's licence checks / anonymous device telemetry).
    ImGui::Spacing();
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "By activating you agree to our");
    ImGui::SameLine(0.0f, S(4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentHi);
    if (ImGui::SmallButton("Terms & Privacy")) {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", "https://antigravity-license.onrender.com/legal",
                      nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void UIController::drawSetupWizard() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("SetupWizard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    float cardW = S(640.0f);
    float startX = (viewport->WorkSize.x - cardW) * 0.5f;
    if (startX < 0) startX = 0;
    ImGui::SetCursorPosY(viewport->WorkSize.y * 0.08f);
    ImGui::SetCursorPosX(startX);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26.0f), S(24.0f)));
    ImGui::BeginChild("WizardCard", ImVec2(cardW, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_AlwaysUseWindowPadding);

    ImGui::PushFont(theme::FontHeading);
    ImGui::TextColored(theme::Accent, "WELCOME");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("FIRST-TIME SETUP");
    ImGui::PopFont();
    ImGui::TextColored(theme::TextDim, "Three choices and you're live. You can change all of this later in Settings.");
    widgets::Divider();

    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();

    // VB-CABLE detection drives both the hint text and output preselection
    int cableIdx = -1;
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name.find("CABLE Input") != std::string::npos) {
            cableIdx = static_cast<int>(i);
            break;
        }
    }
    // Preselect sensible defaults once (cable for output, default device for monitor)
    static bool preselected = false;
    if (!preselected && !outputs.empty()) {
        if (cableIdx >= 0 && m_config->outputDevice.empty()) m_selectedOutputIdx = cableIdx;
        if (m_config->monitorDevice.empty()) {
            for (size_t i = 0; i < outputs.size(); ++i) {
                if (outputs[i].isDefault) { m_selectedMonitorIdx = static_cast<int>(i); break; }
            }
        }
        preselected = true;
    }

    // 1. Microphone
    ImGui::PushFont(theme::FontBold);
    ImGui::TextColored(theme::Accent, "1");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("Your microphone");
    ImGui::PopFont();
    m_selectedInputIdx = labeledDeviceCombo("", "##wizmic", inputs, m_selectedInputIdx, "No microphones found");
    ImGui::Spacing();

    // 2. Where Discord/games listen
    ImGui::PushFont(theme::FontBold);
    ImGui::TextColored(theme::Accent, "2");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("Output that Discord/games will use as your mic");
    ImGui::PopFont();
    if (cableIdx >= 0) {
        ImGui::TextColored(theme::Green, "VB-CABLE detected - pick 'CABLE Input' below.");
    } else {
        ImGui::TextColored(theme::Yellow, "VB-CABLE not found. Without it, others can't hear your effects.");
        if (ImGui::Button("Get VB-CABLE (free)")) {
#ifdef _WIN32
            ShellExecuteA(nullptr, "open", "https://vb-audio.com/Cable/", nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }
        ImGui::SameLine();
        if (widgets::ColoredButton("I installed it - refresh devices", ImVec2(0, 0), theme::PanelHover)) {
            m_audioEngine->refreshDevices();
            preselected = false; // re-run cable preselection with new list
        }
    }
    m_selectedOutputIdx = labeledDeviceCombo("", "##wizout", outputs, m_selectedOutputIdx, "No outputs found");
    ImGui::Spacing();

    // 3. Headphones
    ImGui::PushFont(theme::FontBold);
    ImGui::TextColored(theme::Accent, "3");
    ImGui::SameLine(0.0f, S(8.0f));
    ImGui::TextUnformatted("Your headphones (to hear the soundboard and yourself)");
    ImGui::PopFont();
    m_selectedMonitorIdx = labeledDeviceCombo("", "##wizmon", outputs, m_selectedMonitorIdx, "No outputs found");

    widgets::Divider();

    // GDPR / Terms consent. This wizard is now the app's unavoidable first
    // screen (there's no forced activation gate anymore), so it's where consent
    // is obtained before any usage/telemetry. Remembered in config so returning
    // users aren't re-asked. The crash reporter's network ping is gated on this.
    if (m_config && m_config->termsAccepted) m_termsAccepted = true;
    ImGui::Checkbox("##wizterms", &m_termsAccepted);
    ImGui::SameLine(0.0f, S(6.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("I accept the");
    ImGui::SameLine(0.0f, S(4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentHi);
    if (ImGui::SmallButton("Terms & Privacy")) {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", "https://antigravity-license.onrender.com/legal",
                      nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, S(4.0f));
    ImGui::TextDisabled("(GDPR compliant)");
    if (m_config) m_config->termsAccepted = m_termsAccepted;
    CrashReporter::setConsent(m_termsAccepted);

    ImGui::Spacing();
    ImGui::BeginDisabled(!m_termsAccepted);
    if (widgets::ColoredButton("FINISH & START ENGINE", ImVec2(cardW * 0.60f, S(40.0f)), theme::GreenLo)) {
        startEngine();
        m_config->setupDone = true;
        m_showWizard = false;
    }
    ImGui::SameLine();
    if (widgets::ColoredButton("Skip for now", ImVec2(-FLT_MIN, S(40.0f)), theme::PanelHover)) {
        m_config->setupDone = true;
        m_showWizard = false;
    }
    ImGui::EndDisabled();
    if (!m_termsAccepted) {
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "Tick the box above to continue.");
        ImGui::PopFont();
    }

    ImGui::Spacing();
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Free tier: Reverb + Pitch Shifter and 2 soundboard clips. Upgrade to Pro anytime for everything.");
    ImGui::TextColored(theme::TextDim, "Tip: in Discord/your game, set the MICROPHONE to 'CABLE Output (VB-Audio Virtual Cable)'.");
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void UIController::drawSettingsPanel() {
    // Scrolls internally; VU meters below get the remaining column height
    float settingsH = std::max(S(320.0f), ImGui::GetContentRegionAvail().y * 0.56f);
    ImGui::BeginChild("SettingsChild", ImVec2(0, settingsH), ImGuiChildFlags_Border);
    widgets::SectionLabel("DEVICES");

    // Quick routing / output setup instructions
    if (ImGui::CollapsingHeader("Setup Guide - How to route your voice")) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::TextWrapped("1. Install a virtual audio cable (free VB-CABLE from vb-audio.com/Cable), then reboot.");
        ImGui::TextWrapped("2. Mic Input = your real microphone.");
        ImGui::TextWrapped("3. Primary Output = 'CABLE Input (VB-Audio Virtual Cable)'. This carries your processed voice to other apps.");
        ImGui::TextWrapped("4. Voice Monitor = your headphones. Soundboard clips always play here; enable Voice Loopback below to also hear your own processed voice.");
        ImGui::TextWrapped("5. Press START ENGINE.");
        ImGui::TextWrapped("6. In Discord / your game, pick 'CABLE Output (VB-Audio Virtual Cable)' as the microphone.");
        ImGui::TextWrapped("No virtual cable? Set Primary Output to your headphones just to try the effects locally.");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
    }

    const auto& inputs = m_audioEngine->getInputDevices();
    const auto& outputs = m_audioEngine->getOutputDevices();

    m_selectedInputIdx   = labeledDeviceCombo("Mic Input", "##mic", inputs, m_selectedInputIdx, "None Detected");
    m_selectedOutputIdx  = labeledDeviceCombo("Primary Output (virtual mic)", "##out", outputs, m_selectedOutputIdx, "None Detected");
    m_selectedMonitorIdx = labeledDeviceCombo("Voice Monitor (your headphones)", "##mon", outputs, m_selectedMonitorIdx, "None Detected");

    // Engine status/error message
    if (!m_statusMessage.empty()) {
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(m_isStatusError ? theme::Red : theme::Green, "%s", m_statusMessage.c_str());
        ImGui::PopFont();
    }

    widgets::Divider();
    widgets::SectionLabel("ENGINE");

    bool exclusive = m_audioEngine->isExclusiveMode();
    if (widgets::ToggleSwitch("##exclusive", &exclusive, "Exclusive Mode (WASAPI)")) {
        m_audioEngine->setExclusiveMode(exclusive);
    }
    ImGui::SetItemTooltip("Exclusive mode bypasses Windows Mixer for lowest possible latency (<10ms), but blocks other apps from using this audio device.");

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Buffer size (ms)");
    ImGui::PopFont();
    int bufferMs = m_audioEngine->getBufferSizeMs();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##buffer", &bufferMs, 5, 100)) {
        m_audioEngine->setBufferSizeMs(bufferMs);
    }
    ImGui::SetItemTooltip("Higher = more stable (no crackling), lower = less latency.\nTakes effect the next time the engine is started.");

    widgets::Divider();
    widgets::SectionLabel("MONITORING");

    bool monitor = m_audioEngine->isMonitorEnabled();
    if (widgets::ToggleSwitch("##loopback", &monitor, "Voice Loopback (hear yourself)")) {
        m_audioEngine->setMonitorEnabled(monitor);
    }
    ImGui::SetItemTooltip("Toggle hearing your OWN VOICE through the Monitor output.\nSoundboard clips play through the Monitor regardless of this setting\n(as long as a Voice Monitor device is selected and the engine is running).");

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Monitor volume (your voice)");
    ImGui::PopFont();
    float monVol = m_audioEngine->getMonitorVolume();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##monvol", &monVol, 0.0f, 1.5f, "%.2f")) {
        m_audioEngine->setMonitorVolume(monVol);
    }
    ImGui::SetItemTooltip("Volume of YOUR VOICE through the Monitor output (headphones).");

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Soundboard volume (your ears only)");
    ImGui::PopFont();
    float sbMonVol = m_audioEngine->getSoundboardMonitorVolume();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##sbmonvol", &sbMonVol, 0.0f, 1.5f, "%.2f")) {
        m_audioEngine->setSoundboardMonitorVolume(sbMonVol);
    }
    ImGui::SetItemTooltip("How loud soundboard clips are in YOUR headphones only.\n"
                          "Discord/games always get clips at each clip's own Volume slider,\n"
                          "full strength - this only turns them down for you.");

    widgets::Divider();
    widgets::SectionLabel("OUTPUT (WHAT DISCORD HEARS)");

    bool outStage = m_audioEngine->isOutputStageEnabled();
    if (widgets::ToggleSwitch("##outstage", &outStage, "Auto-level output (recommended)")) {
        m_audioEngine->setOutputStageEnabled(outStage);
    }
    ImGui::SetItemTooltip("Keeps your voice and soundboard at a steady, clear loudness so\n"
                          "Discord's voice detection doesn't cut out on quiet parts, and\n"
                          "loud sounds don't distort. Turn off only if you run your own\n"
                          "compressor/limiter elsewhere.");

    ImGui::BeginDisabled(!outStage);
    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Output level (boost)");
    ImGui::PopFont();
    float outGain = m_audioEngine->getOutputGainDb();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##outgain", &outGain, 0.0f, 18.0f, "+%.1f dB")) {
        m_audioEngine->setOutputGainDb(outGain);
    }
    ImGui::SetItemTooltip("Louder = easier for others to hear you and less chance Discord\n"
                          "mutes quiet moments. The limiter stops it ever distorting, so\n"
                          "raise this until people say you're clearly audible (try +4 to +9 dB).");

    // Live gain-reduction readout: shows the limiter actually catching peaks.
    if (m_audioEngine->isActive()) {
        float gr = m_audioEngine->getOutputGainReductionDb();
        ImGui::PushFont(theme::FontSmall);
        if (gr > 0.3f)
            ImGui::TextColored(theme::Accent, "Limiting peaks: -%.1f dB", gr);
        else
            ImGui::TextColored(theme::TextDim, "Peaks OK - no limiting needed");
        ImGui::PopFont();
    }
    ImGui::EndDisabled();

    widgets::Divider();
    widgets::SectionLabel("LICENSE");

    if (!m_isPro) {
        // Free tier: show what's unlocked by upgrading, plus the reason if a
        // key was rejected/expired (drops here instead of a hard lock now).
        ImGui::TextColored(theme::Yellow, "Plan: Free");
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "2 soundboard clips - Reverb & Pitch Shifter only.");
        switch (m_license->getStatus()) {
            case LicenseManager::Status::Expired:
                ImGui::TextColored(theme::Orange, "Your subscription ended - renew to restore Pro.");
                break;
            case LicenseManager::Status::DeviceLimit:
                ImGui::TextColored(theme::Orange, "Key is on too many devices - free a slot to restore Pro.");
                break;
            case LicenseManager::Status::Invalid:
                ImGui::TextColored(theme::Red, "Last key was rejected. Enter a valid one to unlock Pro.");
                break;
            default:
                break;
        }
        ImGui::PopFont();
        if (widgets::ColoredButton("Unlock Pro - all effects + unlimited sounds", ImVec2(-FLT_MIN, S(32.0f)), theme::Accent)) {
            m_showActivation = true;
        }
    } else {
        std::string plan = m_license->getPlan();
        if (plan == "lifetime") {
            ImGui::TextColored(theme::Green, "Plan: Pro (Lifetime)");
        } else if (plan == "monthly") {
            std::string until = m_license->getPaidUntil();
            std::string label = "Plan: Pro (Monthly)";
            if (until.size() >= 10) label += " - renews " + until.substr(0, 10);
            ImGui::TextColored(theme::Cyan, "%s", label.c_str());
        } else if (plan == "trial") {
            std::string until = m_license->getPaidUntil();
            std::string label = "Plan: Pro (Trial)";
            if (until.size() >= 10) label += " - expires " + until.substr(0, 10);
            ImGui::TextColored(theme::Purple, "%s", label.c_str());
        } else {
            ImGui::TextColored(theme::Green, "Plan: Pro");
        }
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "App v" APP_VERSION);
        ImGui::PopFont();

        if (widgets::ColoredButton("Sign out on this PC", ImVec2(-FLT_MIN, 0), theme::RedLo)) {
            m_audioEngine->stop();
            m_license->reset();
        }
        ImGui::SetItemTooltip("Drops this PC back to the free tier so you can enter a different key.\n"
                              "Your device slot stays reserved - to move the key to a new PC,\n"
                              "contact support to free a slot (this stops stolen keys being\n"
                              "unbound by whoever took them).");
    }

    // Refer a friend
    widgets::Divider();
    widgets::SectionLabel("SHARE");
    if (widgets::ColoredButton("Copy link for a friend", ImVec2(-FLT_MIN, 0), theme::PanelHover)) {
        ImGui::SetClipboardText("https://antigravity-license.onrender.com/");
        m_shareCopiedTimer = 2.5f;
    }
    ImGui::SetItemTooltip("Copies the download page link to your clipboard.");
    if (m_shareCopiedTimer > 0.0f) {
        m_shareCopiedTimer -= ImGui::GetIO().DeltaTime;
        ImGui::TextColored(theme::Green, "Copied! Paste it anywhere.");
    }

    // Sponsored banner - free tier only (Pro is always ad-free, as promised).
    // Server-controlled and absent unless one is configured, so this is usually
    // nothing. See server.js /api/ad + AdBanner.
    if (!m_isPro && m_adBanner && m_adBanner->isReady()) {
        widgets::Divider();
        widgets::SectionLabel("SPONSORED");
        float bannerW = ImGui::GetContentRegionAvail().x;
        float bannerH = bannerW / m_adBanner->getAspectRatio();
        if (bannerH > S(120.0f)) { bannerH = S(120.0f); bannerW = bannerH * m_adBanner->getAspectRatio(); }
        ImTextureID texId = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(m_adBanner->getTextureId()));
        if (ImGui::ImageButton("##adbannermain", texId, ImVec2(bannerW, bannerH))) {
#ifdef _WIN32
            std::string url = m_adBanner->getLinkUrl();
            if (!url.empty()) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }
        ImGui::SetItemTooltip("Sponsored");
    }

    ImGui::EndChild();
}

void UIController::drawVUMeters() {
    ImGui::BeginChild("VUChild", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Border);
    widgets::SectionLabel("LEVELS & LATENCY");

    float micVal = m_audioEngine->getMicLevel();
    float outVal = m_audioEngine->getOutputLevel();

    // Latency details
    if (m_audioEngine->isActive()) {
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "Reported frame size: %d ms", m_audioEngine->getBufferSizeMs());
        ImGui::PopFont();
        if (m_audioEngine->isExclusiveMode()) {
            ImGui::TextColored(theme::Green, "Latency class: Ultra-Low (<10ms)");
        } else {
            ImGui::TextColored(theme::Yellow, "Latency class: Standard (~20ms)");
        }
    } else {
        ImGui::TextColored(theme::TextDim, "Engine offline");
    }

    ImGui::Spacing();

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Microphone");
    ImGui::PopFont();
    widgets::VuMeter(micVal);

    ImGui::Spacing();

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Output mix");
    ImGui::PopFont();
    widgets::VuMeter(outVal);

    // Waveform + spectrum of the processed output
    if (m_audioEngine->isActive()) {
        m_audioEngine->copyVizSnapshot(m_vizSnap);

        ImGui::Spacing();
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "Waveform");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::Bg);
        ImGui::PlotLines("##waveform", m_vizSnap, 1024, 0, nullptr, -1.0f, 1.0f,
                         ImVec2(-1, S(48.0f)));

        // 512-point FFT of the newest half of the snapshot, Hann-windowed
        static float re[512], im[512];
        for (int i = 0; i < 512; ++i) {
            float w = 0.5f - 0.5f * std::cos(6.28318530718f * i / 511.0f);
            re[i] = m_vizSnap[512 + i] * w;
            im[i] = 0.0f;
        }
        fft(re, im, 512);
        for (int b = 0; b < 64; ++b) {
            // First 64 bins cover 0..~6 kHz at 48 kHz - the voice range
            float mag = std::sqrt(re[b + 1] * re[b + 1] + im[b + 1] * im[b + 1]);
            float db = std::log10(1.0f + 12.0f * mag); // gentle log scaling
            // Smooth: fast attack, slow decay (like the VU meters)
            m_spectrum[b] = db > m_spectrum[b] ? db : m_spectrum[b] * 0.85f + db * 0.15f;
        }
        ImGui::PushFont(theme::FontSmall);
        ImGui::TextColored(theme::TextDim, "Spectrum (0-6 kHz)");
        ImGui::PopFont();
        ImGui::PlotHistogram("##spectrum", m_spectrum, 64, 0, nullptr, 0.0f, 2.2f,
                             ImVec2(-1, S(48.0f)));
        ImGui::PopStyleColor();
    }

    widgets::Divider();
    widgets::SectionLabel("DIAGNOSTICS");
    if (m_audioEngine->isActive()) {
        ImGui::PushFont(theme::FontSmall);
        ImGui::Text("Primary callbacks: %llu", m_audioEngine->getDbgPrimaryCallbacks());
        ImGui::Text("Monitor callbacks: %llu", m_audioEngine->getDbgMonitorCallbacks());
        ImGui::TextColored(m_audioEngine->getDbgUnderflows() > 0 ? theme::Red : theme::Green,
                           "Underflows (pops): %llu", m_audioEngine->getDbgUnderflows());
        ImGui::Text("Overflows: %llu", m_audioEngine->getDbgOverflows());
        ImGui::Text("Buffer cushion: %d samples", static_cast<int>(m_audioEngine->getDbgRingBufferLevel()));
        ImGui::PopFont();
        if (widgets::ColoredButton("Reset Diagnostics", ImVec2(0, 0), theme::PanelHover)) {
            m_audioEngine->resetDbgMetrics();
        }
    } else {
        ImGui::TextColored(theme::TextDim, "Telemetry offline (start the engine)");
    }

    ImGui::EndChild();
}

void UIController::drawDSPGraphPanel() {
    ImGui::BeginChild("DSPChild", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Border);
    widgets::SectionLabel("EFFECT CHAIN");

    // ----- Preset bar: load factory/user presets, save the current chain -----
    {
        auto presets = DspPresets::list();
        if (m_selectedPresetIdx >= static_cast<int>(presets.size())) m_selectedPresetIdx = 0;

        std::string preview = presets.empty() ? "No presets" :
            presets[m_selectedPresetIdx].name + (presets[m_selectedPresetIdx].builtIn ? "  [built-in]" : "");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
        if (ImGui::BeginCombo("##preset", preview.c_str())) {
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                ImGui::PushID(i);
                std::string label = presets[i].name + (presets[i].builtIn ? "  [built-in]" : "");
                if (ImGui::Selectable(label.c_str(), m_selectedPresetIdx == i)) m_selectedPresetIdx = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Load") && !presets.empty()) {
            std::string blob = DspPresets::get(presets[m_selectedPresetIdx].name);
            if (!blob.empty()) {
                DspPresets::apply(*m_dspGraph, blob);
                m_presetStatus = "Loaded '" + presets[m_selectedPresetIdx].name + "'";
            }
        }
        ImGui::SameLine();
        if (widgets::ColoredButton("Save as...", ImVec2(0, 0), theme::PanelHover)) {
            ImGui::OpenPopup("SavePresetPopup");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(presets.empty() || presets[m_selectedPresetIdx].builtIn);
        if (widgets::ColoredButton("Delete", ImVec2(0, 0), theme::RedLo) && !presets.empty()) {
            DspPresets::remove(presets[m_selectedPresetIdx].name);
            m_presetStatus = "Deleted '" + presets[m_selectedPresetIdx].name + "'";
            m_selectedPresetIdx = 0;
        }
        ImGui::EndDisabled();

        if (ImGui::BeginPopup("SavePresetPopup")) {
            ImGui::Text("Save current chain as:");
            ImGui::SetNextItemWidth(S(220.0f));
            ImGui::InputTextWithHint("##presetname", "My preset", m_presetNameInput, sizeof(m_presetNameInput));
            if (ImGui::Button("Save", ImVec2(S(100.0f), 0))) {
                std::string name = m_presetNameInput;
                if (!name.empty()) {
                    if (DspPresets::save(name, DspPresets::serialize(*m_dspGraph))) {
                        m_presetStatus = "Saved '" + name + "'";
                    } else {
                        m_presetStatus = "Can't use that name (built-in presets are protected)";
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(S(100.0f), 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (!m_presetStatus.empty()) {
            ImGui::PushFont(theme::FontSmall);
            ImGui::TextColored(theme::Green, "%s", m_presetStatus.c_str());
            ImGui::PopFont();
        }
        ImGui::Spacing();
    }

    const auto& nodes = m_dspGraph->getNodes();

    if (nodes.empty()) {
        ImGui::TextDisabled("No active nodes in the DSP graph.");
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        ImGui::PushID(static_cast<int>(i));

        bool enabled = node->isEnabled();
        const char* name = node->getName();
        bool open = m_collapsedNodes.find(name) == m_collapsedNodes.end();
        // Premium effect the free tier can't run: shown but locked. (Engine
        // enforces separately; this is purely the visual treatment.)
        bool locked = !m_isPro && node->requiredFeature() != 0u;

        // Each effect is a rounded card; an accent strip on the left marks
        // enabled effects (drawn after EndChild once the height is known).
        ImGui::PushStyleColor(ImGuiCol_ChildBg, (enabled && !locked) ? theme::Panel : theme::Bg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12.0f), S(9.0f)));
        ImGui::BeginChild("NodeCard", ImVec2(0, 0),
                          ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);

        // Header row: [toggle / PRO badge] [name....] [up] [down] [chevron]
        if (locked) {
            if (widgets::ColoredButton("PRO", ImVec2(ImGui::GetFrameHeight() * 1.75f, ImGui::GetFrameHeight()), theme::Accent)) {
                m_showActivation = true;
            }
            ImGui::SetItemTooltip("Locked on the free tier - click to unlock all effects with Pro.");
        } else {
            widgets::ToggleSwitch("##enabled", &node->isEnabled());
            ImGui::SetItemTooltip("Enable / bypass this effect.");
        }
        ImGui::SameLine();

        float rightControls = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.x * 2 + S(22.0f);
        float nameWidth = ImGui::GetContentRegionAvail().x - rightControls;

        ImGui::PushFont(theme::FontBold);
        ImGui::PushStyleColor(ImGuiCol_Text, locked ? theme::TextDim : (enabled ? theme::Text : theme::TextDim));
        ImGui::AlignTextToFramePadding();
        if (ImGui::Selectable(name, false, 0, ImVec2(nameWidth, 0))) {
            if (locked) {
                m_showActivation = true;
            } else if (open) {
                m_collapsedNodes.insert(name);
            } else {
                m_collapsedNodes.erase(name);
            }
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetItemTooltip(locked ? "Pro effect - click to unlock" : (open ? "Click to collapse" : "Click to expand"));

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

        // Chevron marking the collapsed state
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::TextDim, open ? "v" : ">");

        if (open) {
            ImGui::Spacing();
            ImGui::Indent(S(8.0f));

            // Locked premium effect: show a one-line upsell, and render the
            // params visible-but-greyed (disabled) so users see what they'd get.
            if (locked) {
                ImGui::PushFont(theme::FontSmall);
                ImGui::TextColored(theme::Accent, "Pro effect");
                ImGui::SameLine();
                if (ImGui::SmallButton("Unlock ->")) m_showActivation = true;
                ImGui::PopFont();
            }
            ImGui::BeginDisabled(locked);

            // Leave room on the right so slider labels are never clipped
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - S(168.0f));

            // Downcast nodes to edit custom parameters
            if (auto* ns = dynamic_cast<NoiseSuppressorNode*>(node.get())) {
                ImGui::TextColored(theme::TextDim, "AI denoiser: removes fans, hum & hiss while you talk");
                ImGui::SliderFloat("Strength", &ns->m_strength, 0.0f, 1.0f, "%.2f");
                ImGui::SetItemTooltip("1.0 = fully denoised. Lower it to blend some\nof the original mic back in if your voice\nstarts sounding processed.");
            }
            else if (auto* gate = dynamic_cast<NoiseGateNode*>(node.get())) {
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

                // Quick presets
                ImGui::PushFont(theme::FontSmall);
                if (widgets::ColoredButton("Deep Voice", ImVec2(0, 0), theme::PanelHover)) { pitch->m_pitchFactor = 0.72f; } ImGui::SameLine();
                if (widgets::ColoredButton("Minion/Chipmunk", ImVec2(0, 0), theme::PanelHover)) { pitch->m_pitchFactor = 1.45f; } ImGui::SameLine();
                if (widgets::ColoredButton("Female preset", ImVec2(0, 0), theme::PanelHover)) { pitch->m_pitchFactor = 1.2f; } ImGui::SameLine();
                if (widgets::ColoredButton("Male preset", ImVec2(0, 0), theme::PanelHover)) { pitch->m_pitchFactor = 0.85f; } ImGui::SameLine();
                if (widgets::ColoredButton("Reset", ImVec2(0, 0), theme::PanelHover)) { pitch->m_pitchFactor = 1.0f; }
                ImGui::PopFont();

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
                ImGui::TextColored(theme::TextDim, "Bandpass 300Hz-3.4kHz + Crunch");
                ImGui::SliderFloat("Dry / Wet", &tel->m_dryWet, 0.0f, 1.0f, "%.2f");
            }

            ImGui::PopItemWidth();
            ImGui::EndDisabled();
            ImGui::Unindent(S(8.0f));
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Accent strip for enabled effects (not for locked ones - they don't run)
        if (enabled && !locked) {
            ImVec2 mn = ImGui::GetItemRectMin();
            ImVec2 mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(mn.x, mn.y), ImVec2(mn.x + S(3.0f), mx.y),
                ImGui::GetColorU32(theme::Accent), S(2.0f));
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::EndChild();
}

void UIController::drawSoundboardPanel() {
    ImGui::BeginChild("SoundboardChild", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Border);

    bool canAdd = m_soundboard->canAddMoreClips();

    widgets::SectionLabel("SOUNDBOARD");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(150.0f));
    if (canAdd) {
        if (widgets::ColoredButton("+ Add Sound", ImVec2(S(158.0f), 0), theme::Accent)) {
            std::string path = openFileDialog();
            if (!path.empty()) {
                // Copies the file into the app's "sounds" folder and loads it
                // from there, so the clip survives if the original moves.
                m_soundboard->importSound(path);
                persistConfig();
            }
        }
        if (ImGui::IsItemHovered()) {
            static std::string soundsDir = Soundboard::getSoundsDirectory();
            ImGui::SetTooltip("Files are copied into:\n%s\nSounds in that folder load automatically at startup.", soundsDir.c_str());
        }
    } else {
        // Free tier at the clip cap: the add button becomes an upsell.
        if (widgets::ColoredButton("Unlock unlimited - PRO", ImVec2(S(158.0f), 0), theme::Accent)) {
            m_showActivation = true;
        }
        ImGui::SetItemTooltip("The free tier holds 2 sounds. Upgrade to Pro for unlimited.");
    }

    // Free-tier slot counter
    if (!m_isPro) {
        ImGui::PushFont(theme::FontSmall);
        int used = static_cast<int>(m_soundboard->getClips().size());
        ImGui::TextColored(theme::TextDim, "%d / %d free clip slots used", std::min(used, ent::FREE_CLIP_LIMIT), ent::FREE_CLIP_LIMIT);
        ImGui::PopFont();
    }

    // Mic ducking controls (lower the mic while a sound is playing)
    auto mixer = m_soundboard->getMixer();
    widgets::ToggleSwitch("##duck", &mixer->m_duckingEnabled, "Duck Mic");
    ImGui::SetItemTooltip("Lower your microphone while a sound is playing.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##ducklevel", &mixer->m_duckingAmount, 0.0f, 1.0f, "Mic level  %.2f");
    ImGui::SetItemTooltip("Mic volume while sounds play (0 = muted, 1 = unchanged).");

    // Stop-all: button + bindable global hotkey
    if (widgets::ColoredButton("STOP ALL SOUNDS", ImVec2(S(160.0f), 0), theme::Orange)) {
        m_soundboard->stopAll();
    }
    ImGui::SetItemTooltip("Fade out and stop every playing clip.");

    ImGui::SameLine();
    std::string stopAllLabel = "Hotkey: ";
    if (m_bindingStopAll) {
        stopAllLabel += "[Press Key]";
    } else {
        stopAllLabel += Soundboard::getKeyName(m_soundboard->getStopAllHotkey());
    }
    if (widgets::ColoredButton((stopAllLabel + "##stopall").c_str(), ImVec2(-FLT_MIN, 0), theme::PanelHover)) {
        m_bindingStopAll = true;
        m_bindingClip = nullptr;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Left-click to bind a global stop-all key.\nRight-click to clear.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        m_soundboard->clearStopAllHotkey();
        m_bindingStopAll = false;
        persistConfig();
    }

    ImGui::PushFont(theme::FontSmall);
    ImGui::TextColored(theme::TextDim, "Tip: click a clip's Hotkey button to bind a global key - bindings are saved automatically\n"
                                       "and work while gaming. Keys 1-9 also play the first nine clips while the app is focused.");
    ImGui::PopFont();
    ImGui::Spacing();

    auto& clips = m_soundboard->getClips();

    if (clips.empty()) {
        ImGui::TextDisabled("No sounds loaded. Click '+ Add Sound' to load WAV/MP3 files.");
    }

    // Responsive clip card grid: as many columns as the panel width allows
    float availW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, static_cast<int>(availW / S(225.0f)));
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float cellW = (availW - spacing * (cols - 1)) / cols;

    for (size_t i = 0; i < clips.size(); ++i) {
        auto& clip = clips[i];
        if (i % cols != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));

        bool playing = clip->isPlaying;
        // Clips past the free cap are kept (never deleted) but locked until Pro.
        bool clipLocked = !m_isPro && static_cast<int>(i) >= ent::FREE_CLIP_LIMIT;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, clipLocked ? theme::Bg : theme::PanelAlt);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.0f), S(9.0f)));
        ImGui::BeginChild("ClipCard", ImVec2(cellW, S(200.0f)),
                          ImGuiChildFlags_Border,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysUseWindowPadding);

        // Clip Name (clipped to one line)
        ImGui::PushFont(theme::FontBold);
        ImGui::PushStyleColor(ImGuiCol_Text, clipLocked ? theme::TextDim : (playing ? theme::Green : theme::Text));
        ImGui::TextUnformatted(clip->name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        // Control Buttons
        float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (clipLocked) {
            // Play is the upsell; the rest of the card is greyed out.
            if (widgets::ColoredButton("PRO - Unlock", ImVec2(-FLT_MIN, S(26.0f)), theme::Accent)) {
                m_showActivation = true;
            }
            ImGui::SetItemTooltip("Free holds 2 clips. Upgrade to Pro to play this one.");
        } else {
            if (playing) {
                if (widgets::ColoredButton("PLAYING", ImVec2(half, S(26.0f)), theme::Green)) {
                    m_soundboard->playClip(clip);
                }
            } else {
                if (widgets::ColoredButton("PLAY", ImVec2(half, S(26.0f)), theme::Accent)) {
                    m_soundboard->playClip(clip);
                }
            }
            ImGui::SameLine();
            if (widgets::ColoredButton("STOP", ImVec2(-FLT_MIN, S(26.0f)), theme::PanelHover)) {
                m_soundboard->stopClip(clip);
            }
        }

        ImGui::Spacing();

        // Loop / volume / hotkey - greyed for locked clips (still removable via
        // the right-click menu below, which stays active).
        ImGui::BeginDisabled(clipLocked);
        ImGui::Checkbox("Loop", &clip->loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##vol", &clip->volume, 0.0f, 1.0f, "Vol %.1f");

        // Hotkey Button
        std::string keyLabel = "Hotkey: ";
        if (m_bindingClip == clip) {
            keyLabel += "[Press Key]";
        } else {
            keyLabel += Soundboard::getKeyName(clip->hotkey);
        }
        if (widgets::ColoredButton(keyLabel.c_str(), ImVec2(-FLT_MIN, S(24.0f)), theme::PanelHover)) {
            m_bindingClip = clip;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Left-click to bind key. Right-click card/button to clear.");
        }

        // Trim: play only a slice of a (often long) file. With Loop on, the
        // slice repeats. Not gated - available on free and Pro clips alike.
        float dur = clip->durationSec;
        bool trimmed = clip->startSec > 0.001f ||
                       (clip->endSec > 0.001f && (dur <= 0.0f || clip->endSec < dur - 0.001f));
        char trimLabel[64];
        if (trimmed) {
            float shownEnd = (clip->endSec <= 0.0f) ? dur : clip->endSec;
            snprintf(trimLabel, sizeof(trimLabel), "Trim %.1f-%.1fs##trim", clip->startSec, shownEnd);
        } else {
            snprintf(trimLabel, sizeof(trimLabel), "Trim: full clip##trim");
        }
        if (widgets::ColoredButton(trimLabel, ImVec2(-FLT_MIN, S(22.0f)),
                                   trimmed ? theme::AccentLo : theme::PanelHover)) {
            ImGui::OpenPopup("TrimPopup");
        }
        ImGui::SetItemTooltip("Choose which part of the file plays.\nTurn on Loop to repeat just that slice.");
        ImGui::EndDisabled();

        if (ImGui::BeginPopup("TrimPopup")) {
            ImGui::PushFont(theme::FontBold);
            ImGui::TextUnformatted(clip->name.c_str());
            ImGui::PopFont();
            ImGui::TextColored(theme::TextDim, "Length: %.1f s", dur);
            ImGui::Spacing();

            float startS = clip->startSec;
            float endS = (clip->endSec <= 0.0f) ? dur : clip->endSec;
            bool changed = false, released = false;
            ImGui::SetNextItemWidth(S(240.0f));
            if (ImGui::SliderFloat("Start (s)", &startS, 0.0f, dur > 0.0f ? dur : 1.0f, "%.2f")) changed = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) released = true;
            ImGui::SetNextItemWidth(S(240.0f));
            if (ImGui::SliderFloat("End (s)", &endS, 0.0f, dur > 0.0f ? dur : 1.0f, "%.2f")) changed = true;
            if (ImGui::IsItemDeactivatedAfterEdit()) released = true;

            if (changed) {
                if (startS < 0.0f) startS = 0.0f;
                if (endS < startS + 0.05f) endS = startS + 0.05f; // keep a minimum slice
                if (dur > 0.0f && endS > dur) endS = dur;
                // "at/after the end" is stored as 0 so it tracks the true file end
                float storeEnd = (dur > 0.0f && endS >= dur - 0.001f) ? 0.0f : endS;
                m_soundboard->getMixer()->setTrim(clip, startS, storeEnd); // live (cheap)
            }
            if (released) persistConfig(); // write config.json only when the drag ends

            ImGui::Spacing();
            if (ImGui::Button("Full clip")) {
                m_soundboard->getMixer()->setTrim(clip, 0.0f, 0.0f);
                persistConfig();
            }
            ImGui::SameLine();
            if (widgets::ColoredButton("Preview", ImVec2(0, 0), theme::Accent)) {
                m_soundboard->playClip(clip);
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop")) {
                m_soundboard->stopClip(clip);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::PushFont(theme::FontSmall);
            ImGui::TextColored(theme::TextDim, "Tip: enable Loop to repeat this slice.");
            ImGui::PopFont();
            ImGui::EndPopup();
        }

        // Right click for hotkey/removal actions
        if (ImGui::BeginPopupContextWindow()) {
            if (ImGui::MenuItem("Clear Hotkey")) {
                m_soundboard->clearHotkey(clip);
                persistConfig();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from Board")) {
                m_soundboard->stopClip(clip);
                m_soundboard->removeSound(clip, /*deleteFile=*/false);
                persistConfig();
            }
            if (ImGui::MenuItem("Remove & Delete File")) {
                m_soundboard->stopClip(clip);
                m_soundboard->removeSound(clip, /*deleteFile=*/true);
                persistConfig();
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

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
