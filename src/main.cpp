#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "audio/AudioEngine.h"
#include "dsp/DSPGraph.h"
#include "dsp/Effects.h"
#include "audio/Mixer.h"
#include "soundboard/Soundboard.h"
#include "ui/UIController.h"
#include "ui/Theme.h"
#include "license/LicenseManager.h"
#include "config/AppConfig.h"
#include "ads/AdBanner.h"
#include "dsp/DspPresets.h"
#include "util/CrashReporter.h"

#include <iostream>
#include <memory>

static void glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

int main(int, char**) {
    // 0. Crash telemetry (best-effort ping + local crash.txt on hard crashes)
    CrashReporter::install();

    // 1. Setup GLFW error callback
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Size the window (and our fonts, below) to the monitor's DPI scale so
    // the UI is crisp instead of bitmap-stretched on high-DPI screens.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    // 2. Create Window
    GLFWwindow* window = glfwCreateWindow(1280, 800, "Antigravity Voice Engine & Soundboard", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync (locks UI loop to screen refresh rate)

    float contentScaleX = 1.0f, contentScaleY = 1.0f;
    glfwGetWindowContentScale(window, &contentScaleX, &contentScaleY);
    glfwSetWindowSizeLimits(window,
                            static_cast<int>(1024 * contentScaleX),
                            static_cast<int>(640 * contentScaleX),
                            GLFW_DONT_CARE, GLFW_DONT_CARE);

    // GLFW doesn't automatically apply the .exe's embedded icon (installer/app.rc)
    // to the live window - Explorer/the taskbar shortcut show it, but the running
    // window and Alt-Tab don't unless we set it explicitly via WM_SETICON.
#ifdef _WIN32
    {
        HWND hwnd = glfwGetWin32Window(window);
        HICON hIcon = LoadIconA(GetModuleHandleA(nullptr), "IDI_ICON1");
        if (hIcon) {
            SendMessageA(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
            SendMessageA(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
        }
    }
#endif

    // 3. Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls

    // Fonts + full application theme (palette, metrics), DPI-scaled
    theme::LoadFonts(contentScaleX);
    theme::Apply(contentScaleX);

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // 4. Initialize Core Components
    auto dspGraph = std::make_shared<DSPGraph>();
    auto mixer = std::make_shared<Mixer>();
    auto soundboard = std::make_shared<Soundboard>(mixer);
    auto audioEngine = std::make_shared<AudioEngine>(dspGraph, mixer);

    // Populate initial DSP Graph (suppressor first: clean the mic before
    // anything else shapes or amplifies it)
    dspGraph->addNode(std::make_unique<NoiseSuppressorNode>());
    dspGraph->addNode(std::make_unique<NoiseGateNode>());
    dspGraph->addNode(std::make_unique<CompressorNode>());
    dspGraph->addNode(std::make_unique<ParametricEQNode>());
    dspGraph->addNode(std::make_unique<PitchShifterNode>());
    dspGraph->addNode(std::make_unique<RobotizerNode>());
    dspGraph->addNode(std::make_unique<DistortionNode>());
    dspGraph->addNode(std::make_unique<TelephoneFilterNode>());
    dspGraph->addNode(std::make_unique<ReverbNode>());

    // Initialize Audio Engine context and list devices
    if (!audioEngine->init()) {
        std::cerr << "Warning: Could not initialize Audio Engine context!" << std::endl;
    }

    // Auto-load every sound stored in the app's "sounds" folder
    soundboard->loadSoundsFromAppFolder();

    // Restore persisted settings (%APPDATA%/Antigravity/config.json):
    // engine options, monitoring, ducking, hotkeys, per-clip state.
    auto config = std::make_shared<AppConfig>();
    bool configLoaded = config->load();
    // Honour prior consent for the crash ping before the UI even shows (the UI
    // keeps this in sync as the user accepts Terms in the wizard/activation).
    CrashReporter::setConsent(config->termsAccepted);
    if (configLoaded) {
        audioEngine->setBufferSizeMs(config->bufferMs);
        audioEngine->setExclusiveMode(config->exclusiveMode);
        audioEngine->setMonitorEnabled(config->monitorEnabled);
        audioEngine->setMonitorVolume(config->monitorVolume);
        audioEngine->setSoundboardMonitorVolume(config->soundboardMonitorVolume);
        mixer->m_duckingEnabled = config->duckingEnabled;
        mixer->m_duckingAmount = config->duckingAmount;
        soundboard->setStopAllHotkey(config->stopAllHotkey);
        // Re-apply saved hotkey/volume/loop to clips, matched by file path
        for (auto& clip : soundboard->getClips()) {
            for (const auto& saved : config->clips) {
                if (saved.path == clip->path) {
                    clip->hotkey = saved.hotkey;
                    clip->volume = saved.volume;
                    clip->loop = saved.loop;
                    // Restore the trim region (recomputes frame bounds)
                    mixer->setTrim(clip, saved.startSec, saved.endSec);
                    break;
                }
            }
        }
        // Restore the whole DSP chain (effect order, enabled flags, params)
        if (!config->dspState.empty()) {
            DspPresets::apply(*dspGraph, config->dspState);
        }
    }

    // License check (saved key verifies in the background; app stays usable
    // for previously activated users even if the server is unreachable)
    auto licenseManager = std::make_shared<LicenseManager>();
    licenseManager->init();
    licenseManager->checkForUpdate();
    // Drop this PC's device id where the uninstaller can find it, so removal
    // can be reported back to the dashboard.
    LicenseManager::writeDeviceIdFile();

    // Ad banner (activation/locked screen only, server-controlled - see
    // server.js /api/ad). Background fetch + decode; texture upload happens
    // once per frame on the main thread via pollMainThread() below.
    auto adBanner = std::make_shared<AdBanner>();
    adBanner->start();

    // Create UI Controller
    auto uiController = std::make_unique<UIController>(audioEngine, dspGraph, soundboard, licenseManager, config, adBanner);

    // Main GUI Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Upload any ad image decoded by the background thread (GL calls
        // must happen on this thread, which owns the OpenGL context).
        adBanner->pollMainThread();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render our app views
        uiController->render();

        // Rendering compile
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(theme::Bg.x, theme::Bg.y, theme::Bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // In-app update: once the new installer has been launched, close
        // normally (config saved below) so it can replace this executable.
        if (licenseManager->shouldQuitForUpdate()) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }

    // Persist settings/hotkeys/soundboard state for the next session
    uiController->captureConfig(*config);
    if (!config->save()) {
        std::cerr << "Warning: could not save config to " << AppConfig::configFilePath() << std::endl;
    }

    // Cleanup
    std::cout << "Shutting down application..." << std::endl;
    audioEngine->shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
