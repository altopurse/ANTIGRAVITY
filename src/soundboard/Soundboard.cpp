#include "Soundboard.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static bool isSupportedAudioExtension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
}

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

    // Global stop-all hotkey
    if (m_stopAllHotkey >= 0 && m_stopAllHotkey < 256) {
        bool isDown = (GetAsyncKeyState(m_stopAllHotkey) & 0x8000) != 0;
        bool wasDown = m_previousKeyState[m_stopAllHotkey];
        if (isDown && !wasDown) {
            stopAll();
        }
        m_previousKeyState[m_stopAllHotkey] = isDown;
    }
#endif
}

void Soundboard::stopAll() {
    m_mixer->stopAll(/*fade=*/true);
}

void Soundboard::setStopAllHotkey(int key) {
    m_stopAllHotkey = key;
    // Same anti-instant-trigger treatment as clip hotkeys
    if (key >= 0 && key < static_cast<int>(m_previousKeyState.size())) {
        m_previousKeyState[key] = true;
    }
}

std::shared_ptr<SoundBoardClip> Soundboard::addSound(const std::string& filepath, const std::string& name) {
    return m_mixer->loadClip(filepath, name);
}

std::string Soundboard::getSoundsDirectory() {
    fs::path base;
#ifdef _WIN32
    char exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        base = fs::path(exePath).parent_path();
    }
#endif
    std::error_code ec;
    if (base.empty()) {
        base = fs::current_path(ec);
        if (ec) base = ".";
    }

    fs::path soundsDir = base / "sounds";
    fs::create_directories(soundsDir, ec);
    return soundsDir.string();
}

std::shared_ptr<SoundBoardClip> Soundboard::importSound(const std::string& sourcePath) {
    std::error_code ec;
    fs::path src(sourcePath);
    fs::path soundsDir(getSoundsDirectory());

    // If the file already lives in the app sounds folder, just load it
    fs::path srcParent = fs::weakly_canonical(src, ec).parent_path();
    fs::path dirCanonical = fs::weakly_canonical(soundsDir, ec);
    if (!ec && srcParent == dirCanonical) {
        return addSound(src.string(), src.filename().string());
    }

    // Pick a destination name that doesn't collide with existing files
    fs::path dest = soundsDir / src.filename();
    std::string stem = src.stem().string();
    std::string extension = src.extension().string();
    int suffix = 1;
    while (fs::exists(dest, ec)) {
        dest = soundsDir / (stem + "_" + std::to_string(suffix++) + extension);
    }

    if (!fs::copy_file(src, dest, ec) || ec) {
        std::cerr << "Soundboard: Could not copy into sounds folder, loading original: " << sourcePath << std::endl;
        return addSound(sourcePath, src.filename().string());
    }

    std::cout << "Soundboard: Imported " << src.filename().string() << " -> " << dest.string() << std::endl;
    return addSound(dest.string(), dest.filename().string());
}

void Soundboard::loadSoundsFromAppFolder() {
    std::error_code ec;
    fs::path soundsDir(getSoundsDirectory());

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(soundsDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (isSupportedAudioExtension(entry.path().extension().string())) {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end()); // stable, predictable order
    for (const auto& f : files) {
        addSound(f.string(), f.filename().string());
    }

    if (!files.empty()) {
        std::cout << "Soundboard: Loaded " << files.size() << " sound(s) from " << soundsDir.string() << std::endl;
    }
}

void Soundboard::removeSound(std::shared_ptr<SoundBoardClip> clip, bool deleteFile) {
    if (!clip) return;
    std::string path = clip->path;
    m_mixer->removeClip(clip);

    if (deleteFile) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            std::cerr << "Soundboard: Could not delete file: " << path << std::endl;
        }
    }
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
    // Mark the key as already down so binding it (while pressed) doesn't
    // instantly trigger playback; it must be released and pressed again.
    if (key >= 0 && key < static_cast<int>(m_previousKeyState.size())) {
        m_previousKeyState[key] = true;
    }
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

    // Fallback names (0x70-0x87 = VK_F1-VK_F24; literals keep this portable)
    if (key >= 'A' && key <= 'Z') return std::string(1, (char)key);
    if (key >= '0' && key <= '9') return std::string(1, (char)key);
    if (key >= 0x70 && key <= 0x87) return "F" + std::to_string(key - 0x70 + 1);
    
    return "Key " + std::to_string(key);
}
