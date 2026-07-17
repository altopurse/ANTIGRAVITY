#pragma once
#include "miniaudio.h"
#include "RingBuffer.h"
#include "dsp/DSPGraph.h"
#include "dsp/OutputLimiter.h"
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

    // Visualization tap: copies the most recent kVizSize mono output samples
    // (oldest first) into out. Lock-free by design - the audio thread keeps
    // writing while we copy, and a possible tear is harmless for drawing.
    static constexpr size_t kVizSize = 1024;
    void copyVizSnapshot(float* out) const;
    
    // Monitoring Controls
    void setMonitorEnabled(bool enabled) { m_monitorEnabled = enabled; }
    bool isMonitorEnabled() const { return m_monitorEnabled; }
    void setMonitorVolume(float volume) { m_monitorVolume = volume; }
    float getMonitorVolume() const { return m_monitorVolume; }

    // Soundboard volume heard on the Voice Monitor (headphones) ONLY. The
    // Primary Output (what Discord/games receive via the virtual cable)
    // always plays each clip at its own Volume slider, full strength -
    // this just lets you turn clips down for yourself without affecting
    // what everyone else hears.
    void setSoundboardMonitorVolume(float volume) { m_soundboardMonitorVolume = volume; }
    float getSoundboardMonitorVolume() const { return m_soundboardMonitorVolume; }

    // Output stage (Primary Output only - what Discord/games hear). Makeup
    // gain lifts the whole mix to a consistent level so Discord's voice gate
    // stops dropping quiet parts; the look-ahead limiter tames peaks smoothly
    // instead of hard-clipping. On by default - it's core output quality.
    void setOutputStageEnabled(bool enabled) { m_outputStageEnabled = enabled; }
    bool isOutputStageEnabled() const { return m_outputStageEnabled; }
    void setOutputGainDb(float db) { m_outputGainDb = db; }
    float getOutputGainDb() const { return m_outputGainDb; }
    float getOutputGainReductionDb() const { return m_outputLimiter.gainReductionDb(); }

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

    // Scratch buffer holding just the soundboard's own mix each block (no mic,
    // no DSP; zeros while idle), sized in start(). Feeds the Voice Monitor
    // output whenever mic loopback monitoring is turned off.
    std::vector<float> m_soundboardScratch;

    // Scratch buffer holding the DSP-processed mic signal BEFORE the
    // soundboard is mixed in (i.e. what Mixer::processAndMix would duck).
    // Kept so the monitor mix can be rebuilt with an independent soundboard
    // volume without touching outBuf, which stays the untouched Primary
    // Output (Discord/games) feed.
    std::vector<float> m_voiceOnlyScratch;

    // Configuration
    std::atomic<bool> m_monitorEnabled{false};
    std::atomic<bool> m_monitorJitterBufferReady{false};
    std::atomic<float> m_monitorVolume{0.7f};
    std::atomic<float> m_soundboardMonitorVolume{1.0f};
    std::atomic<bool> m_exclusiveMode{false};
    std::atomic<int> m_bufferSizeMs{20}; // 20ms target latency (stable default, avoids crackling)

    // Primary-output loudness stage
    std::atomic<bool> m_outputStageEnabled{true};
    std::atomic<float> m_outputGainDb{4.0f}; // gentle default lift; limiter guards peaks
    OutputLimiter m_outputLimiter;

    double m_sampleRate = 48000.0;
    int m_channels = 2;

    // VU levels
    std::atomic<float> m_micLevel{0.0f};
    std::atomic<float> m_outputLevel{0.0f};

    // Ring of recent mono output samples for the waveform/spectrum display
    float m_vizBuffer[kVizSize] = {};
    std::atomic<size_t> m_vizWritePos{0};

    // Anti-tamper: running sample counter used to schedule the periodic
    // dropout applied when the app is not genuinely entitled (see
    // ent::ok()). Legit users never hit this path.
    uint64_t m_degradeCounter = 0;
    void applyDegrade(float* buf, ma_uint32 frameCount);

    // Diagnostic atomics
    std::atomic<uint64_t> m_dbgUnderflows{0};
    std::atomic<uint64_t> m_dbgOverflows{0};
    std::atomic<size_t> m_dbgRingBufferLevel{0};
    std::atomic<uint64_t> m_dbgPrimaryCallbacks{0};
    std::atomic<uint64_t> m_dbgMonitorCallbacks{0};
    
    void updateLevel(std::atomic<float>& levelStorage, const float* buffer, size_t numSamples);
};
