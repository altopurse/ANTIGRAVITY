#include "LicenseManager.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

// ============================================================================
//  SET THIS to your Render service hostname after deploying server/
//  (no "https://", no trailing slash). See server/README.md.
// ============================================================================
static const wchar_t* kServerHost = L"antigravity-license.onrender.com";
static const char*    kBuyUrl     = "https://antigravity-license.onrender.com/buy";

LicenseManager::LicenseManager() {}

void LicenseManager::init() {
    std::string saved = loadSavedKey();
    if (saved.empty()) {
        m_status.store(Status::NoKey);
        return;
    }
    m_hadSavedKey.store(true);
    verifyAsync(saved, /*saveOnSuccess=*/false);
}

void LicenseManager::activate(const std::string& key) {
    // Trim whitespace (users paste keys with stray spaces/newlines)
    std::string trimmed = key;
    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
                                 [](unsigned char c) { return std::isspace(c); }),
                  trimmed.end());
    if (trimmed.empty()) return;
    verifyAsync(trimmed, /*saveOnSuccess=*/true);
}

void LicenseManager::openPurchasePage() const {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", kBuyUrl, nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void LicenseManager::verifyAsync(std::string key, bool saveOnSuccess) {
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) return; // one check at a time

    m_status.store(Status::Checking);

    std::thread([this, key = std::move(key), saveOnSuccess]() {
        int result = verifyOnline(key);
        if (result == 1) {
            if (saveOnSuccess) saveKey(key);
            m_hadSavedKey.store(true);
            m_status.store(Status::Valid);
        } else if (result == 0) {
            // Server explicitly rejected it: drop any saved copy and lock.
            deleteSavedKey();
            m_hadSavedKey.store(false);
            m_status.store(Status::Invalid);
        } else {
            m_status.store(Status::NetworkError);
        }
        m_busy.store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// Storage: %LOCALAPPDATA%/AntigravityVoiceEngine/license.key
// (same folder the installer uses, so the key survives reinstalls)
// ---------------------------------------------------------------------------

std::string LicenseManager::licenseFilePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    fs::path dir = base ? fs::path(base) / "AntigravityVoiceEngine" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "license.key").string();
}

std::string LicenseManager::loadSavedKey() {
    std::ifstream f(licenseFilePath());
    if (!f) return "";
    std::string key;
    std::getline(f, key);
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              key.end());
    return key;
}

void LicenseManager::saveKey(const std::string& key) {
    std::ofstream f(licenseFilePath(), std::ios::trunc);
    f << key << "\n";
}

void LicenseManager::deleteSavedKey() {
    std::error_code ec;
    fs::remove(licenseFilePath(), ec);
}

// ---------------------------------------------------------------------------
// HTTPS GET via WinHTTP. Generous receive timeout because a sleeping Render
// free instance can take ~50s to cold-start before answering.
// ---------------------------------------------------------------------------

int LicenseManager::verifyOnline(const std::string& key) {
#ifdef _WIN32
    // The key alphabet is [A-Za-z0-9_-], safe to embed in a URL directly.
    std::wstring path = L"/api/verify?key=";
    path.append(key.begin(), key.end());

    int result = -1;

    HINTERNET hSession = WinHttpOpen(L"AntigravityVoiceEngine/1.1",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;
    WinHttpSetTimeouts(hSession, 30000, 60000, 30000, 90000);

    HINTERNET hConnect = WinHttpConnect(hSession, kServerHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
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

        std::string body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > 4096) break; // response is tiny; cap defensively
        }

        if (body.find("\"valid\":true") != std::string::npos)       result = 1;
        else if (body.find("\"valid\":false") != std::string::npos) result = 0;
        // anything else (proxy error page, etc.) stays -1 = network error
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
#else
    (void)key;
    return -1;
#endif
}
