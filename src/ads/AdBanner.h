#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>

// Ad banner shown ONLY on the app's activation (locked) screen for
// unlicensed users - never inside the unlocked app. The server controls
// what image/link is shown (see server.js /api/ad + AD_IMAGE_URL/AD_LINK_URL),
// so the ad can be changed or removed anytime without shipping a new build.
//
// There is no browser engine in this app, so real ad-network embeds (JS,
// iframes) cannot run here - this fetches a plain image and renders it as
// a clickable banner via a real OpenGL texture.
//
// Threading: network fetch + image decode happen on a background thread
// (pure CPU/IO, no GL calls there). The decoded pixels are handed to the
// main thread via pollMainThread(), which is the only place GL texture
// upload happens (OpenGL context is only valid on the thread that owns it).
class AdBanner {
public:
    // Kicks off the background fetch; safe to call once at startup.
    void start();

    // Call once per frame from the main/GL thread. Uploads a decoded image
    // to a GL texture if one is waiting. Cheap no-op most frames.
    void pollMainThread();

    bool isReady() const { return m_ready.load(); }
    unsigned int getTextureId() const { return m_textureId; }
    float getAspectRatio() const { return m_aspectRatio; } // width / height
    std::string getLinkUrl() const;

private:
    void fetchAsync();

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_ready{false};

    // Pixels decoded on the background thread, awaiting main-thread GL upload
    std::mutex m_pendingMutex;
    std::vector<uint8_t> m_pendingPixels; // RGBA8, width*height*4 bytes
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    std::atomic<bool> m_hasPendingUpload{false};

    unsigned int m_textureId = 0;
    float m_aspectRatio = 1.0f;

    mutable std::mutex m_linkMutex;
    std::string m_linkUrl;
};
