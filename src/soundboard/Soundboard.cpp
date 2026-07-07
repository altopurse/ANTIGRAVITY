#include "Soundboard.h"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

Soundboard::Soundboard(std::shared_ptr<Mixer> mixer) : m_mixer(mixer) {
    m_previousKeyState.assign(256, false);
}

Soundboard::~Soundboard() {}

void Soundboard::updateHotkeys() {
#ifdef _WIN32
    // Check key presses for virtual keys 0..255
    for (auto& clip : m_mixer->getClips()) {
        int key = clip->hotkey;
        if (key >= 0 && key < 256) {
            // GetAsyncKeyState returns MSB as 1 if key is currently down
            bool isDown = (GetAsyncKeyState(key) & 0x8000) != 0;
            bool wasDown = m_previousKeyState[key];
            
            // Rising edge (just pressed)
            if (isDown && !wasDown) {
                m_mixer->playClip(clip);
                std::cout << "Soundboard: Hotkey triggered for clip: " << clip->name << std::endl;
            }
            
            m_previousKeyState[key] = isDown;
        }
    }
#endif
}

std::shared_ptr<SoundBoardClip> Soundboard::addSound(const std::string& filepath, const std::string& name) {
    return m_mixer->loadClip(filepath, name);
}

void Soundboard::playClip(std::shared_ptr<SoundBoardClip> clip) {
    if (clip) m_mixer->playClip(clip);
}

void Soundboard::stopClip(std::shared_ptr<SoundBoardClip> clip) {
    if (clip) m_mixer->stopClip(clip);
}

void Soundboard::setHotkey(std::shared_ptr<SoundBoardClip> clip, int key) {
    if (!clip) return;
    clip->hotkey = key;
}

void Soundboard::clearHotkey(std::shared_ptr<SoundBoardClip> clip) {
    if (!clip) return;
    clip->hotkey = -1;
}

std::string Soundboard::getKeyName(int key) {
    if (key < 0) return "None";
    
#ifdef _WIN32
    char name[64] = {0};
    LONG lParam = MapVirtualKeyA(key, MAPVK_VK_TO_VSC) << 16;
    
    // Add special flag for extended keys (e.g. arrow keys)
    switch (key) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_UP:
        case VK_RIGHT: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            lParam |= 0x01000000;
            break;
    }
    
    if (GetKeyNameTextA(lParam, name, sizeof(name)) > 0) {
        return std::string(name);
    }
#endif

    // Fallback names
    if (key >= 'A' && key <= 'Z') return std::string(1, (char)key);
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    
    return "Key " + std::to_string(key);
}
