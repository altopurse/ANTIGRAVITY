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

    // Copy an external file into the app "sounds" folder (removing the
    // dependency on its original location) and load it from there.
    std::shared_ptr<SoundBoardClip> importSound(const std::string& sourcePath);

    // Load every audio file already present in the app "sounds" folder
    void loadSoundsFromAppFolder();

    // Folder next to the executable where imported sounds are stored
    static std::string getSoundsDirectory();

    void playClip(std::shared_ptr<SoundBoardClip> clip);
    void stopClip(std::shared_ptr<SoundBoardClip> clip);

    // Stop every playing clip (fade-out, click-free)
    void stopAll();

    // Global "stop all sounds" hotkey (persisted via AppConfig)
    void setStopAllHotkey(int key);
    int getStopAllHotkey() const { return m_stopAllHotkey; }
    void clearStopAllHotkey() { m_stopAllHotkey = -1; }

    // Remove a clip from the board. If deleteFile is true, also deletes the
    // underlying file from the app "sounds" folder so it won't reload on
    // the next launch; otherwise it just leaves the board (file stays on disk).
    void removeSound(std::shared_ptr<SoundBoardClip> clip, bool deleteFile);

    // Manage hotkey mapping
    void setHotkey(std::shared_ptr<SoundBoardClip> clip, int key);
    void clearHotkey(std::shared_ptr<SoundBoardClip> clip);

    // Get list of clips
    std::vector<std::shared_ptr<SoundBoardClip>>& getClips() { return m_mixer->getClips(); }

    // Access the mixer (for ducking controls in the UI)
    std::shared_ptr<Mixer> getMixer() { return m_mixer; }

    // Helpers to get string names of virtual keys
    static std::string getKeyName(int key);

private:
    std::shared_ptr<Mixer> m_mixer;

    // Keep track of keys that were pressed in the previous frame to trigger on rising-edge
    std::vector<bool> m_previousKeyState;

    int m_stopAllHotkey = -1;
};
