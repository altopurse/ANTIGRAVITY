#include "AdBanner.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <GL/gl.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <thread>

// Same license server that hosts /api/verify etc. - the ad endpoint lives
// there too (server.js), configured via AD_IMAGE_URL/AD_LINK_URL env vars.
static const wchar_t* kServerHost = L"antigravity-license.onrender.com";

#ifdef _WIN32
// GET https://<host><path>. Returns "" on any transport error. Generous
// timeout: a sleeping free-tier server can take ~50s to wake up.
static std::string httpsGet(const std::wstring& host, const std::wstring& path) {
    std::string body;
    HINTERNET hSession = WinHttpOpen(L"AntigravityVoiceEngine-AdBanner/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return body;
    WinHttpSetTimeouts(hSession, 15000, 30000, 15000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = nullptr;
    if (hConnect) {
        hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
    }
    if (hRequest &&
        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > 8 * 1024 * 1024) break; // 8MB safety cap (images can be binary)
        }
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

// GET an arbitrary https:// URL (the ad image may be hosted anywhere, not
// just on the license server), returning raw bytes.
static std::string httpsGetUrl(const std::wstring& url) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {0};
    wchar_t pathBuf[2048] = {0};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = sizeof(hostBuf) / sizeof(wchar_t);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = sizeof(pathBuf) / sizeof(wchar_t);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        return "";
    }
    std::wstring pathWithQuery = pathBuf;
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        pathWithQuery += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    return httpsGet(hostBuf, pathWithQuery);
}

// Pulls "field":"value" (or "field":null) out of a flat JSON body.
static std::string extractJsonField(const std::string& body, const std::string& field, bool& isNull) {
    isNull = false;
    std::string nullNeedle = "\"" + field + "\":null";
    if (body.find(nullNeedle) != std::string::npos) {
        isNull = true;
        return "";
    }
    std::string needle = "\"" + field + "\":\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    size_t end = body.find('"', p);
    if (end == std::string::npos) return "";
    return body.substr(p, end - p);
}
#endif

void AdBanner::start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) return;
    std::thread(&AdBanner::fetchAsync, this).detach();
}

void AdBanner::fetchAsync() {
#ifdef _WIN32
    std::string body = httpsGet(kServerHost, L"/api/ad");
    if (body.empty()) return;

    bool imgIsNull = false;
    std::string imageUrl = extractJsonField(body, "imageUrl", imgIsNull);
    bool linkIsNull = false;
    std::string linkUrl = extractJsonField(body, "linkUrl", linkIsNull);
    if (imgIsNull || linkIsNull || imageUrl.empty() || linkUrl.empty()) return; // ad disabled

    {
        std::lock_guard<std::mutex> lock(m_linkMutex);
        m_linkUrl = linkUrl;
    }

    std::wstring wImageUrl(imageUrl.begin(), imageUrl.end());
    std::string imageBytes = httpsGetUrl(wImageUrl);
    if (imageBytes.empty()) return;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(imageBytes.data()),
        static_cast<int>(imageBytes.size()), &w, &h, &channels, 4 /* force RGBA */);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingPixels.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
        m_pendingWidth = w;
        m_pendingHeight = h;
    }
    stbi_image_free(pixels);
    m_hasPendingUpload.store(true);
#endif
}

void AdBanner::pollMainThread() {
#ifdef _WIN32
    if (!m_hasPendingUpload.load()) return;

    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pixels.swap(m_pendingPixels);
        w = m_pendingWidth;
        h = m_pendingHeight;
    }
    m_hasPendingUpload.store(false);
    if (pixels.empty() || w <= 0 || h <= 0) return;

    if (m_textureId == 0) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        m_textureId = tex;
    }
    glBindTexture(GL_TEXTURE_2D, m_textureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    m_aspectRatio = static_cast<float>(w) / static_cast<float>(h);
    m_ready.store(true);
#endif
}

std::string AdBanner::getLinkUrl() const {
    std::lock_guard<std::mutex> lock(m_linkMutex);
    return m_linkUrl;
}
