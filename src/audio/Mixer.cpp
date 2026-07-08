#include "Mixer.h"
#include <algorithm>
#include <iostream>

Mixer::Mixer() {}

Mixer::~Mixer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& clip : m_clips) {
        if (clip->isLoaded) {
            ma_decoder_uninit(&clip->decoder);
        }
    }
}

void Mixer::prepare(double sampleRate, int numChannels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampleRate = sampleRate;
    m_channels = numChannels;

    // Pre-size scratch buffers generously (250ms) so the audio callback never allocates
    size_t maxSamples = static_cast<size_t>(sampleRate * 0.25) * numChannels;
    if (m_mixBuffer.size() < maxSamples) m_mixBuffer.resize(maxSamples);
    if (m_tempBuffer.size() < maxSamples) m_tempBuffer.resize(maxSamples);

    // Re-initialize already loaded decoders to match the new sample rate/channels
    for (auto& clip : m_clips) {
        if (clip->isLoaded) {
            ma_decoder_uninit(&clip->decoder);
            ma_decoder_config config = ma_decoder_config_init(ma_format_f32, m_channels, static_cast<ma_uint32>(m_sampleRate));
            ma_result result = ma_decoder_init_file(clip->path.c_str(), &config, &clip->decoder);
            if (result != MA_SUCCESS) {
                clip->isLoaded = false;
                clip->isPlaying = false;
            }
        }
    }
}

std::shared_ptr<SoundBoardClip> Mixer::loadClip(const std::string& path, const std::string& name) {
    double sampleRate;
    int channels;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Skip duplicates (same file already loaded)
        for (auto& existing : m_clips) {
            if (existing->path == path) return existing;
        }
        sampleRate = m_sampleRate;
        channels = m_channels;
    }

    // Decode-file init does disk I/O: keep it OUTSIDE the lock so the
    // audio callback is never blocked while a file is being opened.
    auto clip = std::make_shared<SoundBoardClip>();
    clip->path = path;
    clip->name = name;

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, static_cast<ma_uint32>(sampleRate));
    ma_result result = ma_decoder_init_file(path.c_str(), &config, &clip->decoder);

    if (result == MA_SUCCESS) {
        clip->isLoaded = true;
        clip->isPlaying = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clips.push_back(clip);
        }
        std::cout << "Mixer: Loaded clip successfully: " << name << std::endl;
        return clip;
    } else {
        std::cerr << "Mixer: Failed to load audio file: " << path << " (Error code " << result << ")" << std::endl;
        return nullptr;
    }
}

void Mixer::playClip(std::shared_ptr<SoundBoardClip> clip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!clip || !clip->isLoaded) return;

    // Seek back to start and play
    ma_decoder_seek_to_pcm_frame(&clip->decoder, 0);
    clip->isPlaying = true;
    clip->fadingOut = false;
    clip->fadeGain = 1.0f;
}

void Mixer::stopClip(std::shared_ptr<SoundBoardClip> clip, bool fade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!clip) return;
    if (fade && clip->isPlaying) {
        clip->fadingOut = true; // the audio callback ramps it out
    } else {
        clip->isPlaying = false;
        clip->fadingOut = false;
        clip->fadeGain = 1.0f;
    }
}

void Mixer::stopAll(bool fade) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& clip : m_clips) {
        if (fade && clip->isPlaying) {
            clip->fadingOut = true;
        } else {
            clip->isPlaying = false;
            clip->fadingOut = false;
            clip->fadeGain = 1.0f;
        }
    }
}

void Mixer::removeClip(std::shared_ptr<SoundBoardClip> clip) {
    if (!clip) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find(m_clips.begin(), m_clips.end(), clip);
    if (it == m_clips.end()) return;

    if ((*it)->isLoaded) {
        ma_decoder_uninit(&(*it)->decoder);
    }
    m_clips.erase(it);
}

bool Mixer::processAndMix(float* outBuffer, size_t frameCount, int channels, float* soundboardOnlyOut) {
    // Never block the real-time audio thread: if the UI thread holds the
    // lock (e.g. loading a file), skip the soundboard this block instead
    // of stalling and causing crackles.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Still hand the caller silence for the monitor path so it never
        // forwards stale samples from a previous block.
        if (soundboardOnlyOut) {
            std::fill(soundboardOnlyOut, soundboardOnlyOut + frameCount * channels, 0.0f);
        }
        return false;
    }

    size_t sampleCount = frameCount * channels;
    if (m_mixBuffer.size() < sampleCount) {
        m_mixBuffer.resize(sampleCount);
    }
    if (m_tempBuffer.size() < sampleCount) {
        m_tempBuffer.resize(sampleCount);
    }
    std::fill(m_mixBuffer.begin(), m_mixBuffer.begin() + sampleCount, 0.0f);

    bool anySoundboardActive = false;

    for (auto& clip : m_clips) {
        if (!clip->isPlaying || !clip->isLoaded) continue;

        anySoundboardActive = true;
        ma_uint64 totalFramesRead = 0;
        int consecutiveZeroReads = 0;

        while (totalFramesRead < frameCount) {
            ma_uint64 framesToRead = frameCount - totalFramesRead;
            ma_uint64 framesRead = 0;

            ma_result res = ma_decoder_read_pcm_frames(
                &clip->decoder,
                m_tempBuffer.data() + (totalFramesRead * channels),
                framesToRead,
                &framesRead
            );

            totalFramesRead += framesRead;

            if (framesRead == 0) {
                // Guard against a stuck/empty decoder spinning forever
                if (++consecutiveZeroReads >= 3) {
                    clip->isPlaying = false;
                    break;
                }
            } else {
                consecutiveZeroReads = 0;
            }

            if (res != MA_SUCCESS || framesRead < framesToRead) {
                if (clip->loop) {
                    // Loop: seek back to 0 and continue reading
                    ma_decoder_seek_to_pcm_frame(&clip->decoder, 0);
                } else {
                    // Stop playing once file finishes
                    clip->isPlaying = false;
                    break;
                }
            }
        }

        // Apply volume (and fade-out ramp when stopping) and mix in
        float vol = clip->volume;
        if (clip->fadingOut) {
            // ~30ms linear ramp to zero, per-frame so all channels match
            float step = 1.0f / static_cast<float>(m_sampleRate * 0.03);
            for (size_t fr = 0; fr < totalFramesRead; ++fr) {
                float g = clip->fadeGain * vol;
                for (int c = 0; c < channels; ++c) {
                    m_mixBuffer[fr * channels + c] += m_tempBuffer[fr * channels + c] * g;
                }
                clip->fadeGain -= step;
                if (clip->fadeGain <= 0.0f) {
                    clip->fadeGain = 0.0f;
                    break;
                }
            }
            if (clip->fadeGain <= 0.0f) {
                clip->isPlaying = false;
                clip->fadingOut = false;
                clip->fadeGain = 1.0f;
            }
        } else {
            for (size_t i = 0; i < totalFramesRead * channels; ++i) {
                m_mixBuffer[i] += m_tempBuffer[i] * vol;
            }
        }
    }

    if (soundboardOnlyOut) {
        std::copy(m_mixBuffer.begin(), m_mixBuffer.begin() + sampleCount, soundboardOnlyOut);
    }

    if (anySoundboardActive) {
        // Duck the mic (already in outBuffer) while the soundboard plays,
        // then sum the soundboard mix on top (with basic clipping safety).
        float duck = m_duckingEnabled ? m_duckingAmount : 1.0f;
        for (size_t i = 0; i < sampleCount; ++i) {
            float mixed = outBuffer[i] * duck + m_mixBuffer[i];
            // Soft clipping to prevent harsh digital distortion
            if (mixed > 1.0f) mixed = 1.0f;
            else if (mixed < -1.0f) mixed = -1.0f;
            outBuffer[i] = mixed;
        }
    }

    return anySoundboardActive;
}
