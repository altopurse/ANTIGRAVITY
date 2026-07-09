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

    // Size the monitor ring buffer from the actual period so large buffer
    // settings still leave room for the 3-period jitter cushion plus slack.
    size_t periodSamples = static_cast<size_t>((m_sampleRate * m_bufferSizeMs.load()) / 1000.0) * m_channels;
    m_monitorRingBuffer.resize(std::max<size_t>(16384, periodSamples * 8));
    m_monitorRingBuffer.clear();
    m_monitorJitterBufferReady = false;

    // Sized generously (250ms) so the audio callback never allocates.
    size_t scratchMaxSamples = static_cast<size_t>(m_sampleRate * 0.25) * m_channels;
    if (m_soundboardScratch.size() < scratchMaxSamples) {
        m_soundboardScratch.resize(scratchMaxSamples);
    }
    if (m_voiceOnlyScratch.size() < scratchMaxSamples) {
        m_voiceOnlyScratch.resize(scratchMaxSamples);
    }

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
        // Tear down only the devices; keep the context alive so the user can retry.
        ma_device_uninit(&m_primaryDevice);
        m_primaryDeviceInitialized = false;
        if (m_monitorDeviceInitialized) {
            ma_device_uninit(&m_monitorDevice);
            m_monitorDeviceInitialized = false;
        }
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

    // 2. Process DSP Graph on the copied mic buffer (in-place).
    // The graph expects the TOTAL interleaved sample count, not frames.
    m_dspGraph->process(outBuf, totalSamples, m_channels);

    // 3. Snapshot the DSP-processed mic signal before the soundboard gets
    // mixed in, so the monitor path (step 5) can rebuild its own blend with
    // an independent soundboard volume without touching outBuf below - that
    // buffer is the untouched Primary Output feed (Discord/games), which
    // always gets the soundboard at each clip's own Volume slider.
    if (m_voiceOnlyScratch.size() < totalSamples) {
        m_voiceOnlyScratch.resize(totalSamples);
    }
    std::copy(outBuf, outBuf + totalSamples, m_voiceOnlyScratch.begin());

    // 4. Mix in the soundboard (mic ducking is applied inside the mixer).
    // m_soundboardScratch receives the soundboard's own mix (zeros when
    // nothing is playing) so clip playback can reach the Voice Monitor
    // output even if mic loopback monitoring is off.
    // Pre-sized with margin in start(); this is just a safety net matching
    // the same convention Mixer::processAndMix uses for its own buffers.
    if (m_soundboardScratch.size() < totalSamples) {
        m_soundboardScratch.resize(totalSamples);
    }
    bool soundboardActive = m_mixer->processAndMix(outBuf, frameCount, m_channels, m_soundboardScratch.data());

    // 5. Monitor processed output level (VU)
    updateLevel(m_outputLevel, outBuf, totalSamples);

    // 5b. Feed the waveform/spectrum display: mono-fold each output frame
    // into the viz ring. Plain stores; the UI thread reads without locking
    // (an occasional torn sample is invisible in a scope drawing).
    {
        size_t pos = m_vizWritePos.load(std::memory_order_relaxed);
        for (ma_uint32 f = 0; f < frameCount; ++f) {
            float mono = 0.0f;
            for (int c = 0; c < m_channels; ++c) mono += outBuf[f * m_channels + c];
            m_vizBuffer[pos & (kVizSize - 1)] = mono / static_cast<float>(m_channels);
            ++pos;
        }
        m_vizWritePos.store(pos, std::memory_order_relaxed);
    }

    // 6. Feed the monitor (headphone) output continuously, rebuilt with the
    //    Soundboard Monitor Volume so you can hear clips quieter than what
    //    Discord/games get (outBuf, step 4, is never touched by this):
    //    - Voice Loopback ON  -> ducked voice + soundboard at monitor volume
    //    - Voice Loopback OFF -> soundboard only, at monitor volume
    //    Writing every block keeps the jitter buffer primed, so soundboard
    //    hits are heard immediately regardless of the loopback toggle and
    //    there are no start/stop races between the two audio threads.
    if (m_monitorDeviceInitialized) {
        float sbVol = m_soundboardMonitorVolume.load(std::memory_order_relaxed);
        bool loopbackOn = m_monitorEnabled.load(std::memory_order_relaxed);
        // Mirrors the duck the Primary Output feed already received, so the
        // voice level relationship (ducked while sounds play) is the same
        // for you as for everyone else - only the soundboard loudness differs.
        float duck = (soundboardActive && m_mixer->m_duckingEnabled) ? m_mixer->m_duckingAmount : 1.0f;

        for (size_t i = 0; i < totalSamples; ++i) {
            float mixed = (loopbackOn ? m_voiceOnlyScratch[i] * duck : 0.0f) + m_soundboardScratch[i] * sbVol;
            if (mixed > 1.0f) mixed = 1.0f;
            else if (mixed < -1.0f) mixed = -1.0f;
            m_voiceOnlyScratch[i] = mixed; // reuse: this scratch's raw values are no longer needed
        }

        size_t written = m_monitorRingBuffer.write(m_voiceOnlyScratch.data(), totalSamples);
        if (written < totalSamples)
            m_dbgOverflows.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::monitorCallback(void* pOutput, ma_uint32 frameCount) {
    m_dbgMonitorCallbacks.fetch_add(1, std::memory_order_relaxed);
    float* outBuf = static_cast<float*>(pOutput);
    size_t totalSamples = frameCount * m_channels;

    size_t available = m_monitorRingBuffer.getAvailableRead();
    m_dbgRingBufferLevel.store(available, std::memory_order_relaxed);

    // Warm-up/pre-roll cushion: wait for 3 blocks of audio to accumulate
    // to absorb any thread scheduling jitter between input and output clocks.
    // Clamped to half the ring so a monitor device negotiating a large
    // period can never demand a cushion the buffer cannot hold.
    size_t targetCushion = std::min(totalSamples * 3, m_monitorRingBuffer.getCapacity() / 2);
    if (!m_monitorJitterBufferReady.load(std::memory_order_relaxed)) {
        if (available >= targetCushion) {
            m_monitorJitterBufferReady.store(true, std::memory_order_relaxed);
        } else {
            std::fill(outBuf, outBuf + totalSamples, 0.0f);
            return;
        }
    }

    // Latency guard: if clock drift between the two devices lets the backlog
    // grow past twice the cushion, drop the excess so monitoring latency
    // stays bounded instead of creeping up until the ring overflows.
    if (available > targetCushion * 2) {
        m_monitorRingBuffer.discard(available - targetCushion);
    }

    // Drain the ring buffer that the primary callback filled.
    size_t readCount = m_monitorRingBuffer.read(outBuf, totalSamples);

    // Underflow: silence the remainder and re-arm the pre-roll.
    if (readCount < totalSamples) {
        std::fill(outBuf + readCount, outBuf + totalSamples, 0.0f);
        m_monitorJitterBufferReady.store(false, std::memory_order_relaxed);
        m_dbgUnderflows.fetch_add(1, std::memory_order_relaxed);
    }

    // Apply monitoring volume slider, with a hard safety clamp (the slider
    // goes to 1.5x, which could otherwise push peaks into digital clipping)
    float vol = m_monitorVolume.load(std::memory_order_relaxed);
    for (size_t i = 0; i < totalSamples; ++i) {
        float s = outBuf[i] * vol;
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        outBuf[i] = s;
    }
}

void AudioEngine::copyVizSnapshot(float* out) const {
    size_t pos = m_vizWritePos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kVizSize; ++i) {
        out[i] = m_vizBuffer[(pos + i) & (kVizSize - 1)]; // oldest -> newest
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
