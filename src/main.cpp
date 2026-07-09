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
#include "license/LicenseManager.h"
#include "config/AppConfig.h"
#include "ads/AdBanner.h"

#include <iostream>
#include <memory>

static void glfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

int main(int, char**) {
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
    // 2. Create Window
    GLFWwindow* window = glfwCreateWindow(1100, 720, "Antigravity Voice Engine & Soundboard", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync (locks UI loop to screen refresh rate)

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

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // 4. Initialize Core Components
    auto dspGraph = std::make_shared<DSPGraph>();
    auto mixer = std::make_shared<Mixer>();
    auto soundboard = std::make_shared<Soundboard>(mixer);
    auto audioEngine = std::make_shared<AudioEngine>(dspGraph, mixer);

    // Populate initial DSP Graph
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
    if (config->load()) {
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
                    break;
                }
            }
        }
    }

    // License check (saved key verifies in the background; app stays usable
    // for previously activated users even if the server is unreachable)
    auto licenseManager = std::make_shared<LicenseManager>();
    licenseManager->init();
    licenseManager->checkForUpdate();

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
        glClearColor(0.06f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
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
