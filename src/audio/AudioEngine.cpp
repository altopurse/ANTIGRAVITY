#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include "AudioEngine.h"
#include <iostream>
#include <cmath>
#include <algorithm>

// Static callback redirects
static void maPrimaryCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioEngine* pEngine = static_cast<AudioEngine*>(pDevice->pUserData);
    if (pEngine) {
        pEngine->primaryCallback(pOutput, pInput, frameCount);
    }
}

static void maMonitorCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioEngine* pEngine = static_cast<AudioEngine*>(pDevice->pUserData);
    if (pEngine) {
        pEngine->monitorCallback(pOutput, frameCount);
    }
}

AudioEngine::AudioEngine(std::shared_ptr<DSPGraph> dspGraph, std::shared_ptr<Mixer> mixer)
    : m_dspGraph(dspGraph), m_mixer(mixer), m_monitorRingBuffer(16384) {
    m_inputs.clear();
    m_outputs.clear();
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::init() {
    if (m_contextInitialized) return true;

    ma_result result = ma_context_init(nullptr, 0, nullptr, &m_context);
    if (result != MA_SUCCESS) {
        std::cerr << "AudioEngine: Failed to initialize miniaudio context. (Error: " << result << ")" << std::endl;
        return false;
    }

    m_contextInitialized = true;
    refreshDevices();
    return true;
}

void AudioEngine::shutdown() {
    stop();

    if (m_primaryDeviceInitialized) {
        ma_device_uninit(&m_primaryDevice);
        m_primaryDeviceInitialized = false;
    }

    if (m_monitorDeviceInitialized) {
        ma_device_uninit(&m_monitorDevice);
        m_monitorDeviceInitialized = false;
    }

    if (m_contextInitialized) {
        ma_context_uninit(&m_context);
        m_contextInitialized = false;
    }
}

void AudioEngine::refreshDevices() {
    if (!m_contextInitialized) return;

    m_inputs.clear();
    m_outputs.clear();

    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info* pCaptureInfos;
    ma_uint32 captureCount;

    ma_result result = ma_context_get_devices(&m_context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount);
    if (result != MA_SUCCESS) {
        std::cerr << "AudioEngine: Failed to get system audio devices." << std::endl;
        return;
    }

    // Capture devices (Microphones)
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        AudioDeviceRef dev;
        dev.id = pCaptureInfos[i].id;
        dev.name = pCaptureInfos[i].name;
        dev.isDefault = pCaptureInfos[i].isDefault;
        m_inputs.push_back(dev);
    }

    // Playback devices (Speakers, Virtual Cable Inputs)
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        AudioDeviceRef dev;
        dev.id = pPlaybackInfos[i].id;
        dev.name = pPlaybackInfos[i].name;
        dev.isDefault = pPlaybackInfos[i].isDefault;
        m_outputs.push_back(dev);
    }
}

bool AudioEngine::start(const ma_device_id* inputId, const ma_device_id* primaryOutputId, const ma_device_id* monitorOutputId) {
    if (!m_contextInitialized) return false;
    
    stop(); // Make sure everything is stopped

    m_sampleRate = 48000.0;
    m_channels = 2;

    // 1. Initialize Primary Duplex Device (Microphone -> Virtual Cable/Speakers)
    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.pDeviceID = const_cast<ma_device_id*>(inputId);
    config.capture.format = ma_format_f32;
    config.capture.channels = m_channels;
    
    config.playback.pDeviceID = const_cast<ma_device_id*>(primaryOutputId);
    config.playback.format = ma_format_f32;
    config.playback.channels = m_channels;

    config.sampleRate = static_cast<ma_uint32>(m_sampleRate);
    config.periodSizeInMilliseconds = m_bufferSizeMs;
    config.dataCallback = maPrimaryCallback;
    config.pUserData = this;

    if (m_exclusiveMode) {
        config.playback.shareMode = ma_share_mode_exclusive;
        config.capture.shareMode = ma_share_mode_exclusive;
    }

    ma_result result = ma_device_init(&m_context, &config, &m_primaryDevice);
    if (result != MA_SUCCESS) {
        std::cerr << "AudioEngine: Failed to initialize primary duplex device. Error: " << result << std::endl;
        return false;
    }
    m_primaryDeviceInitialized = true;

    // Sync actual sample rate and channel count
    m_sampleRate = m_primaryDevice.sampleRate;
    m_channels = m_primaryDevice.playback.channels;

    // Prepare processing graph and soundboard mixer
    m_dspGraph->prepare(m_sampleRate);
    m_mixer->prepare(m_sampleRate, m_channels);
    m_monitorRingBuffer.resize(16384);
    m_monitorRingBuffer.clear();
    m_monitorJitterBufferReady = false;

    // 2. Initialize Monitor Playback Device (if requested and distinct)
    if (monitorOutputId != nullptr) {
        ma_device_config monConfig = ma_device_config_init(ma_device_type_playback);
        monConfig.playback.pDeviceID = const_cast<ma_device_id*>(monitorOutputId);
        monConfig.playback.format = ma_format_f32;
        monConfig.playback.channels = m_channels;
        monConfig.sampleRate = static_cast<ma_uint32>(m_sampleRate);
        monConfig.periodSizeInMilliseconds = m_bufferSizeMs;
        monConfig.dataCallback = maMonitorCallback;
        monConfig.pUserData = this;

        if (m_exclusiveMode) {
            monConfig.playback.shareMode = ma_share_mode_exclusive;
        }

        result = ma_device_init(&m_context, &monConfig, &m_monitorDevice);
        if (result == MA_SUCCESS) {
            m_monitorDeviceInitialized = true;
        } else {
            std::cerr << "AudioEngine: Warning - failed to initialize monitor device. Live monitoring disabled." << std::endl;
        }
    }

    // Start primary stream
    result = ma_device_start(&m_primaryDevice);
    if (result != MA_SUCCESS) {
        std::cerr << "AudioEngine: Failed to start primary audio stream. Error: " << result << std::endl;
        shutdown();
        return false;
    }

    // Start monitoring stream if initialized
    if (m_monitorDeviceInitialized) {
        result = ma_device_start(&m_monitorDevice);
        if (result != MA_SUCCESS) {
            std::cerr << "AudioEngine: Warning - failed to start monitoring stream." << std::endl;
        }
    }

    m_active = true;
    std::cout << "AudioEngine: Started successfully. Sample Rate: " << m_sampleRate << "Hz, Channels: " << m_channels << std::endl;
    return true;
}

void AudioEngine::stop() {
    if (!m_active) return;
    
    m_active = false;

    if (m_primaryDeviceInitialized) {
        ma_device_stop(&m_primaryDevice);
        ma_device_uninit(&m_primaryDevice);
        m_primaryDeviceInitialized = false;
    }

    if (m_monitorDeviceInitialized) {
        ma_device_stop(&m_monitorDevice);
        ma_device_uninit(&m_monitorDevice);
        m_monitorDeviceInitialized = false;
    }
    
    m_micLevel = 0.0f;
    m_outputLevel = 0.0f;
}

void AudioEngine::primaryCallback(void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (!m_active.load(std::memory_order_relaxed)) return;
    
    m_dbgPrimaryCallbacks.fetch_add(1, std::memory_order_relaxed);

    float* outBuf = static_cast<float*>(pOutput);
    const float* inBuf = static_cast<const float*>(pInput);
    size_t totalSamples = frameCount * m_channels;

    // 1. Monitor microphone raw level (VU)
    if (inBuf != nullptr) {
        updateLevel(m_micLevel, inBuf, totalSamples);
        // Copy capture buffer to playback buffer as a starting point
        std::copy(inBuf, inBuf + totalSamples, outBuf);
    } else {
        std::fill(outBuf, outBuf + totalSamples, 0.0f);
        m_micLevel = 0.0f;
    }

    // 2. Process DSP Graph on the copied mic buffer (in-place)
    m_dspGraph->process(outBuf, frameCount, m_channels);

    // 3. Mix in the soundboard (and apply microphone ducking if playing)
    bool isSoundboardPlaying = m_mixer->processAndMix(outBuf, frameCount, m_channels);
    
    // Optional ducking implementation: if soundboard is playing, duck the mic part
    if (isSoundboardPlaying && m_mixer->m_duckingEnabled && inBuf != nullptr) {
        // Simple ducking is handled
    }

    // 4. Monitor processed output level (VU)
    updateLevel(m_outputLevel, outBuf, totalSamples);

    // 5. If live monitoring is enabled, write to the monitor ring buffer
    if (m_monitorEnabled.load(std::memory_order_relaxed) && m_monitorDeviceInitialized) {
        size_t written = m_monitorRingBuffer.write(outBuf, totalSamples);
        if (written < totalSamples) {
            m_dbgOverflows.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AudioEngine::monitorCallback(void* pOutput, ma_uint32 frameCount) {
    m_dbgMonitorCallbacks.fetch_add(1, std::memory_order_relaxed);
    float* outBuf = static_cast<float*>(pOutput);
    size_t totalSamples = frameCount * m_channels;

    if (!m_monitorEnabled.load(std::memory_order_relaxed)) {
        std::fill(outBuf, outBuf + totalSamples, 0.0f);
        m_monitorJitterBufferReady = false;
        return;
    }

    size_t available = m_monitorRingBuffer.getAvailableRead();
    m_dbgRingBufferLevel.store(available, std::memory_order_relaxed);
    
    // Warm-up/pre-roll cushion: wait for 3 blocks of audio (e.g. 30ms) to accumulate 
    // to absorb any thread scheduling jitter between input and output clocks.
    size_t targetCushion = totalSamples * 3;
    if (!m_monitorJitterBufferReady.load(std::memory_order_relaxed)) {
        if (available >= targetCushion) {
            m_monitorJitterBufferReady.store(true, std::memory_order_relaxed);
        } else {
            // Output silence while accumulating cushion
            std::fill(outBuf, outBuf + totalSamples, 0.0f);
            return;
        }
    }

    // Read from the ring buffer filled by primary callback
    size_t readCount = m_monitorRingBuffer.read(outBuf, totalSamples);
    
    // If underflow, zero the rest and reset pre-roll so we buffer again instead of cracking
    if (readCount < totalSamples) {
        std::fill(outBuf + readCount, outBuf + totalSamples, 0.0f);
        m_monitorJitterBufferReady.store(false, std::memory_order_relaxed);
        m_dbgUnderflows.fetch_add(1, std::memory_order_relaxed);
    }

    // Apply monitoring volume slider
    float vol = m_monitorVolume.load(std::memory_order_relaxed);
    if (vol != 1.0f) {
        for (size_t i = 0; i < totalSamples; ++i) {
            outBuf[i] *= vol;
        }
    }
}

void AudioEngine::updateLevel(std::atomic<float>& levelStorage, const float* buffer, size_t numSamples) {
    float peak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        float absVal = std::abs(buffer[i]);
        if (absVal > peak) {
            peak = absVal;
        }
    }

    // Exponential decay to make VU meters smooth
    float current = levelStorage.load(std::memory_order_relaxed);
    if (peak > current) {
        levelStorage.store(peak, std::memory_order_relaxed); // instant attack
    } else {
        levelStorage.store(current * 0.92f + peak * 0.08f, std::memory_order_relaxed); // slow decay
    }
}
