#pragma once
#include "audio/Mixer.h"
#include <vector>
#include <string>
#include <memory>

class Soundboard {
public:
    Soundboard(std::shared_ptr<Mixer> mixer);
    ~Soundboard();

    // Scan for registered hotkeys and trigger playbacks (called in the main thread loop)
    void updateHotkeys();

    // Register a sound file
    std::shared_ptr<SoundBoardClip> addSound(const std::string& filepath, const std::string& name);
    void playClip(std::shared_ptr<SoundBoardClip> clip);
    void stopClip(std::shared_ptr<SoundBoardClip> clip);
    
    // Manage hotkey mapping
    void setHotkey(std::shared_ptr<SoundBoardClip> clip, int key);
    void clearHotkey(std::shared_ptr<SoundBoardClip> clip);

    // Get list of clips
    std::vector<std::shared_ptr<SoundBoardClip>>& getClips() { return m_mixer->getClips(); }

    // Helpers to get string names of virtual keys
    static std::string getKeyName(int key);

private:
    std::shared_ptr<Mixer> m_mixer;
    
    // Keep track of keys that were pressed in the previous frame to trigger on rising-edge
    std::vector<bool> m_previousKeyState;
};
