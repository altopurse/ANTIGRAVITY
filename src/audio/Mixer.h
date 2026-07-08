#pragma once
#include "miniaudio.h"
#include <vector>
#include <string>
#include <mutex>
#include <memory>

struct SoundBoardClip {
    std::string path;
    std::string name;
    ma_decoder decoder;
    bool isLoaded = false;
    float volume = 0.8f;
    bool isPlaying = false;
    bool loop = false;
    int hotkey = -1; // virtual key code, e.g. VK_F1

    // Click-free stopping: stop requests ramp the gain to zero over ~30ms
    // inside the audio callback instead of cutting the waveform mid-sample.
    bool fadingOut = false;
    float fadeGain = 1.0f;
};

class Mixer {
public:
    Mixer();
    ~Mixer();

    void prepare(double sampleRate, int numChannels);
    
    // Process soundboard streams and mix into output buffer.
    // If soundboardOnlyOut is non-null, it receives just the soundboard's
    // own mix (post-volume, pre-ducking) so the caller can route the sound
    // effects to a monitor/speaker output independently of the main mix.
    // Also returns if there's any active playback (for ducking calculations).
    bool processAndMix(float* outBuffer, size_t frameCount, int channels, float* soundboardOnlyOut = nullptr);

    // Load a clip into the soundboard
    std::shared_ptr<SoundBoardClip> loadClip(const std::string& path, const std::string& name);
    void playClip(std::shared_ptr<SoundBoardClip> clip);
    // fade=true ramps out over ~30ms (click-free); false cuts immediately
    void stopClip(std::shared_ptr<SoundBoardClip> clip, bool fade = true);
    void stopAll(bool fade = true);

    // Remove a clip from the board (stops it and releases its decoder)
    void removeClip(std::shared_ptr<SoundBoardClip> clip);

    std::vector<std::shared_ptr<SoundBoardClip>>& getClips() { return m_clips; }
    
    // Ducking options
    float m_duckingAmount = 0.5f; // how much to reduce microphone volume (0.0 = total silence, 1.0 = no reduction)
    bool m_duckingEnabled = true;

private:
    std::vector<std::shared_ptr<SoundBoardClip>> m_clips;
    std::mutex m_mutex;
    double m_sampleRate = 48000.0;
    int m_channels = 2;
    std::vector<float> m_mixBuffer;
    std::vector<float> m_tempBuffer; // per-clip decode scratch (member: no heap alloc in audio callback)
};
