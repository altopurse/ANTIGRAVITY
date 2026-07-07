# Create dist directory structure
New-Item -ItemType Directory -Force -Path "dist"
New-Item -ItemType Directory -Force -Path "dist/sounds"

# Write a quick C++ program to generate standard wav files for the soundboard
$wavGeneratorSource = @"
#include <iostream>
#include <fstream>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void writeWav(const std::string& filename, double freq, double duration, double sampleRate = 44100.0) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return;

    int numSamples = static_cast<int>(duration * sampleRate);
    int numChannels = 1;
    int bitsPerSample = 16;
    int byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    int blockAlign = numChannels * (bitsPerSample / 8);
    int subChunk2Size = numSamples * numChannels * (bitsPerSample / 8);
    int chunkSize = 36 + subChunk2Size;

    // Header
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&chunkSize), 4);
    file.write("WAVE", 4);
    
    // Subchunk 1 (fmt)
    file.write("fmt ", 4);
    int subChunk1Size = 16;
    file.write(reinterpret_cast<const char*>(&subChunk1Size), 4);
    uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    uint16_t channels = numChannels;
    file.write(reinterpret_cast<const char*>(&channels), 2);
    uint32_t sRate = sampleRate;
    file.write(reinterpret_cast<const char*>(&sRate), 4);
    uint32_t bRate = byteRate;
    file.write(reinterpret_cast<const char*>(&bRate), 4);
    uint16_t bAlign = blockAlign;
    file.write(reinterpret_cast<const char*>(&bAlign), 2);
    uint16_t bps = bitsPerSample;
    file.write(reinterpret_cast<const char*>(&bps), 2);

    // Subchunk 2 (data)
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&subChunk2Size), 4);

    // Generate sine wave samples
    for (int i = 0; i < numSamples; ++i) {
        double t = static_cast<double>(i) / sampleRate;
        double angle = 2.0 * M_PI * freq * t;
        
        // Basic envelope (decay) to make it sound pleasant
        double envelope = exp(-3.0 * t); // Decays over duration
        int16_t sample = static_cast<int16_t>(32767.0 * sin(angle) * envelope);
        
        file.write(reinterpret_cast<const char*>(&sample), 2);
    }
}

int main() {
    writeWav("dist/sounds/chime.wav", 587.33, 1.2); // D5 chime
    writeWav("dist/sounds/beep.wav", 880.0, 0.4);   // A5 high beep
    return 0;
}
"@

$wavGeneratorSource | Out-File -FilePath "generate_wav.cpp" -Encoding utf8

# Compile wav generator
g++ -O2 generate_wav.cpp -o generate_wav.exe

# Execute to build the sounds
.\generate_wav.exe

# Cleanup generator files
Remove-Item -Force "generate_wav.cpp"
Remove-Item -Force "generate_wav.exe"

# Copy binary
Copy-Item -Force "build/bin/voice-changer.exe" -Destination "dist/"

# Write README.txt
$readmeContent = @"
========================================================================
ANTIGRAVITY VOICE ENGINE & SOUNDBOARD - USER GUIDE
========================================================================

Welcome to the Antigravity Voice Engine!

GETTING STARTED:
1. Run "Install.ps1" in PowerShell (Right-click -> Run with PowerShell) 
   to install the application to your local system and create shortcuts.
2. Ensure you have a virtual audio cable installed (like VB-CABLE) to route
   your processed voice into Discord, Zoom, or games.
3. Open the app, select your physical microphone as "Mic Input", select
   your virtual cable input as "Primary Output", and select your headphones
   as "Voice Monitor".
4. Enjoy real-time high-performance voice morphing and soundboards!

For detailed configuration guides, refer to your installed shortcuts.
"@
$readmeContent | Out-File -FilePath "dist/README.txt" -Encoding utf8
