#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "audio/AudioEngine.h"
#include "dsp/DSPGraph.h"
#include "dsp/Effects.h"
#include "audio/Mixer.h"
#include "soundboard/Soundboard.h"
#include "ui/UIController.h"
#include "license/LicenseManager.h"

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

    // License check (saved key verifies in the background; app stays usable
    // for previously activated users even if the server is unreachable)
    auto licenseManager = std::make_shared<LicenseManager>();
    licenseManager->init();

    // Create UI Controller
    auto uiController = std::make_unique<UIController>(audioEngine, dspGraph, soundboard, licenseManager);

    // Main GUI Loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

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
