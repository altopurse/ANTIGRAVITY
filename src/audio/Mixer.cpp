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
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto clip = std::make_shared<SoundBoardClip>();
    clip->path = path;
    clip->name = name;
    
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, m_channels, static_cast<ma_uint32>(m_sampleRate));
    ma_result result = ma_decoder_init_file(path.c_str(), &config, &clip->decoder);
    
    if (result == MA_SUCCESS) {
        clip->isLoaded = true;
        clip->isPlaying = false;
        m_clips.push_back(clip);
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
}

void Mixer::stopClip(std::shared_ptr<SoundBoardClip> clip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!clip) return;
    clip->isPlaying = false;
}

void Mixer::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& clip : m_clips) {
        clip->isPlaying = false;
    }
}

bool Mixer::processAndMix(float* outBuffer, size_t frameCount, int channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t sampleCount = frameCount * channels;
    if (m_mixBuffer.size() < sampleCount) {
        m_mixBuffer.resize(sampleCount);
    }
    std::fill(m_mixBuffer.begin(), m_mixBuffer.begin() + sampleCount, 0.0f);
    
    bool anySoundboardActive = false;
    std::vector<float> tempBuffer(sampleCount);

    for (auto& clip : m_clips) {
        if (!clip->isPlaying || !clip->isLoaded) continue;
        
        anySoundboardActive = true;
        ma_uint64 totalFramesRead = 0;
        float* pDst = m_mixBuffer.data();
        
        while (totalFramesRead < frameCount) {
            ma_uint64 framesToRead = frameCount - totalFramesRead;
            ma_uint64 framesRead = 0;
            
            ma_result res = ma_decoder_read_pcm_frames(
                &clip->decoder, 
                tempBuffer.data() + (totalFramesRead * channels), 
                framesToRead, 
                &framesRead
            );

            totalFramesRead += framesRead;

            if (res != MA_SUCCESS || framesRead < framesToRead) {
                if (clip->loop) {
                    // Loop: seek back to 0 and continue reading
                    ma_decoder_seek_to_pcm_frame(&clip->decoder, 0);
                    if (framesRead == 0 && totalFramesRead == 0) {
                        // Avoid infinite loop if file is empty
                        break;
                    }
                } else {
                    // Stop playing once file finishes
                    clip->isPlaying = false;
                    break;
                }
            }
        }

        // Apply volume and mix into m_mixBuffer
        float vol = clip->volume;
        for (size_t i = 0; i < totalFramesRead * channels; ++i) {
            m_mixBuffer[i] += tempBuffer[i] * vol;
        }
    }

    if (anySoundboardActive) {
        // Sum soundboard mix buffer into output buffer (with basic clipping safety)
        for (size_t i = 0; i < sampleCount; ++i) {
            float mixed = outBuffer[i] + m_mixBuffer[i];
            // Soft clipping to prevent harsh digital distortion
            if (mixed > 1.0f) mixed = 1.0f;
            else if (mixed < -1.0f) mixed = -1.0f;
            outBuffer[i] = mixed;
        }
    }

    return anySoundboardActive;
}
