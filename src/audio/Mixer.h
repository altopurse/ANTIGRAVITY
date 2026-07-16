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

    // Trim / region playback: only play the slice [startSec, endSec) of the
    // file (great for grabbing one moment out of a long track). endSec <= 0
    // means "to the end of the file". When loop is on, the region loops.
    // The *Frame / durationSec fields are derived (recomputeRegion) from these
    // seconds + the current sample rate, and are what the audio callback reads.
    float startSec = 0.0f;
    float endSec = 0.0f;      // 0 = end of file
    float durationSec = 0.0f; // full file length (for the UI), set on load

    ma_uint64 lengthFrames = 0; // full file length in frames at engine rate
    ma_uint64 startFrame = 0;   // region start (derived)
    ma_uint64 endFrame = 0;     // region end, exclusive (derived; 0 until loaded)
    ma_uint64 playCursor = 0;   // current playhead within the file (frames)

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

    // Set the play region (in seconds; endSec <= 0 means "to end of file").
    // Recomputes the derived frame bounds under the lock so the audio thread
    // sees a consistent region. Safe to call while the clip is playing.
    void setTrim(std::shared_ptr<SoundBoardClip> clip, float startSec, float endSec);
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
    // Recompute startFrame/endFrame/durationSec from startSec/endSec + the
    // current sample rate and the clip's cached lengthFrames. Caller must hold
    // m_mutex (or be in a context where the clip isn't being read by audio).
    void recomputeRegion(const std::shared_ptr<SoundBoardClip>& clip);

    std::vector<std::shared_ptr<SoundBoardClip>> m_clips;
    std::mutex m_mutex;
    double m_sampleRate = 48000.0;
    int m_channels = 2;
    std::vector<float> m_mixBuffer;
    std::vector<float> m_tempBuffer; // per-clip decode scratch (member: no heap alloc in audio callback)
};
