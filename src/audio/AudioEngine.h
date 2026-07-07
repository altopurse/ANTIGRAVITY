#pragma once
#include "miniaudio.h"
#include "RingBuffer.h"
#include "dsp/DSPGraph.h"
#include "audio/Mixer.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>

struct AudioDeviceRef {
    ma_device_id id;
    std::string name;
    bool isDefault = false;
};

class AudioEngine {
public:
    AudioEngine(std::shared_ptr<DSPGraph> dspGraph, std::shared_ptr<Mixer> mixer);
    ~AudioEngine();

    bool init();
    void shutdown();

    // Device enumeration
    void refreshDevices();
    const std::vector<AudioDeviceRef>& getInputDevices() const { return m_inputs; }
    const std::vector<AudioDeviceRef>& getOutputDevices() const { return m_outputs; }

    // Stream control
    bool start(const ma_device_id* inputId, const ma_device_id* primaryOutputId, const ma_device_id* monitorOutputId);
    void stop();
    bool isActive() const { return m_active; }

    // Live levels (for VU meters)
    float getMicLevel() const { return m_micLevel.load(); }
    float getOutputLevel() const { return m_outputLevel.load(); }
    
    // Monitoring Controls
    void setMonitorEnabled(bool enabled) { m_monitorEnabled = enabled; }
    bool isMonitorEnabled() const { return m_monitorEnabled; }
    void setMonitorVolume(float volume) { m_monitorVolume = volume; }
    float getMonitorVolume() const { return m_monitorVolume; }

    // Low latency settings
    void setExclusiveMode(bool exclusive) { m_exclusiveMode = exclusive; }
    bool isExclusiveMode() const { return m_exclusiveMode; }
    void setBufferSizeMs(int ms) { m_bufferSizeMs = ms; }
    int getBufferSizeMs() const { return m_bufferSizeMs; }
    
    double getSampleRate() const { return m_sampleRate; }
    int getChannels() const { return m_channels; }

    // Debugging diagnostics
    uint64_t getDbgUnderflows() const { return m_dbgUnderflows.load(); }
    uint64_t getDbgOverflows() const { return m_dbgOverflows.load(); }
    size_t getDbgRingBufferLevel() const { return m_dbgRingBufferLevel.load(); }
    uint64_t getDbgPrimaryCallbacks() const { return m_dbgPrimaryCallbacks.load(); }
    uint64_t getDbgMonitorCallbacks() const { return m_dbgMonitorCallbacks.load(); }
    void resetDbgMetrics() {
        m_dbgUnderflows = 0;
        m_dbgOverflows = 0;
        m_dbgPrimaryCallbacks = 0;
        m_dbgMonitorCallbacks = 0;
    }

    // Callbacks (internal use)
    void primaryCallback(void* pOutput, const void* pInput, ma_uint32 frameCount);
    void monitorCallback(void* pOutput, ma_uint32 frameCount);

private:
    std::shared_ptr<DSPGraph> m_dspGraph;
    std::shared_ptr<Mixer> m_mixer;
    
    ma_context m_context;
    ma_device m_primaryDevice;  // Duplex: Mic Input -> Primary Output (Virtual Cable)
    ma_device m_monitorDevice;  // Playback: Monitor Output (Speakers)
    
    bool m_contextInitialized = false;
    bool m_primaryDeviceInitialized = false;
    bool m_monitorDeviceInitialized = false;
    std::atomic<bool> m_active{false};

    std::vector<AudioDeviceRef> m_inputs;
    std::vector<AudioDeviceRef> m_outputs;

    // Buffer for passing processed audio from primary stream to monitoring stream
    RingBuffer m_monitorRingBuffer;

    // Configuration
    std::atomic<bool> m_monitorEnabled{false};
    std::atomic<bool> m_monitorJitterBufferReady{false};
    std::atomic<float> m_monitorVolume{0.7f};
    std::atomic<bool> m_exclusiveMode{false};
    std::atomic<int> m_bufferSizeMs{10}; // 10ms target latency

    double m_sampleRate = 48000.0;
    int m_channels = 2;

    // VU levels
    std::atomic<float> m_micLevel{0.0f};
    std::atomic<float> m_outputLevel{0.0f};

    // Diagnostic atomics
    std::atomic<uint64_t> m_dbgUnderflows{0};
    std::atomic<uint64_t> m_dbgOverflows{0};
    std::atomic<size_t> m_dbgRingBufferLevel{0};
    std::atomic<uint64_t> m_dbgPrimaryCallbacks{0};
    std::atomic<uint64_t> m_dbgMonitorCallbacks{0};
    
    void updateLevel(std::atomic<float>& levelStorage, const float* buffer, size_t numSamples);
};
