#include "LicenseManager.h"
#include "Entitlement.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
// Some MinGW header sets omit this flag; its value is fixed in the Windows ABI.
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#endif

namespace fs = std::filesystem;

// ============================================================================
//  SET THIS to your Render service hostname after deploying server/
//  (no "https://", no trailing slash). See server/README.md.
// ============================================================================
static const wchar_t* kServerHost = L"antigravity-license.onrender.com";
// Root page, not /buy directly: it shows both plans (lifetime / monthly) so
// the user actually gets to choose instead of silently defaulting to one.
static const char*    kBuyUrl     = "https://antigravity-license.onrender.com/";

#ifdef _WIN32
static std::string httpGetBody(const std::wstring& path); // defined below
#endif

#ifndef APP_VERSION
#define APP_VERSION "1.9.0"
#endif

// Short OS tag for the per-license usage profile, e.g. "Win11-26200".
// Only [A-Za-z0-9.-] so it is URL-safe as-is.
static std::string osTag() {
#ifdef _WIN32
    char build[64] = {0};
    DWORD sz = sizeof(build);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     "CurrentBuild", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, build, &sz) == ERROR_SUCCESS) {
        long b = atol(build);
        std::string tag = (b >= 22000) ? "Win11-" : "Win10-";
        for (const char* p = build; *p; ++p) {
            if (isalnum(static_cast<unsigned char>(*p))) tag += *p;
        }
        return tag;
    }
    return "Windows";
#else
    return "other";
#endif
}

LicenseManager::LicenseManager() {}

void LicenseManager::init() {
    std::string saved = loadSavedKey();
    if (saved.empty()) {
        m_status.store(Status::NoKey);
        ent::setLicensed(false);
        return;
    }
    m_hadSavedKey.store(true);
    {
        std::lock_guard<std::mutex> lock(m_keyMutex);
        m_activeKey = saved; // known immediately so reset() works mid-check
    }
    // Entitle during the initial re-check so a paying user who starts the
    // engine immediately isn't degraded while the server call is in flight.
    ent::setLicensed(true);
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

void LicenseManager::reset() {
    std::string key;
    {
        std::lock_guard<std::mutex> lock(m_keyMutex);
        key = m_activeKey;
        m_activeKey.clear();
    }
    if (key.empty()) key = loadSavedKey();

    deleteSavedKey();
    m_hadSavedKey.store(false);
    m_status.store(Status::NoKey);
    ent::setLicensed(false);

    // Free this device's slot on the server so the key can be activated on
    // another PC. Fire-and-forget: local reset must not depend on the network.
    if (!key.empty()) {
        std::thread([key]() { releaseOnline(key); }).detach();
    }
}

void LicenseManager::unbindAllDevices() {
    std::string key;
    {
        std::lock_guard<std::mutex> lock(m_keyMutex);
        key = m_activeKey;
    }
    if (key.empty()) key = loadSavedKey();
    if (key.empty()) return;

    std::thread([this, key]() {
#ifdef _WIN32
        std::string q = "/api/unbindall?key=" + key;
        std::wstring path(q.begin(), q.end());
        httpGetBody(path);
#endif
        // Re-verify so this PC immediately re-registers (and the plan
        // display refreshes). Also recovers from the DeviceLimit lock.
        verifyAsync(key, /*saveOnSuccess=*/true);
    }).detach();
}

void LicenseManager::verifyAsync(std::string key, bool saveOnSuccess) {
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) return; // one check at a time

    m_status.store(Status::Checking);

    std::thread([this, key = std::move(key), saveOnSuccess]() {
        int result = verifyOnline(key);
        if (result == 1) {
            if (saveOnSuccess) saveKey(key);
            saveLastKey(key); // refresh the recall cache on every good verify
            {
                std::lock_guard<std::mutex> lock(m_keyMutex);
                m_activeKey = key;
            }
            m_hadSavedKey.store(true);
            m_status.store(Status::Valid);
            // Genuine entitlement - the audio path's scattered guard. Set only
            // here (and the offline-grace path in refreshEntitlement), NOT in
            // the UI's isUnlocked(), so patching the UI gate isn't enough.
            ent::setLicensed(true);
        } else if (result == 2) {
            // Real key, but this machine is over the device limit. Keep the
            // saved key (they may free up a slot) but lock the app. Remember
            // the key so "Unbind All Devices & Retry" can act on it.
            {
                std::lock_guard<std::mutex> lock(m_keyMutex);
                m_activeKey = key;
            }
            m_hadSavedKey.store(false);
            m_status.store(Status::DeviceLimit);
            ent::setLicensed(false);
        } else if (result == 3) {
            // Monthly subscription lapsed. Keep the saved key on disk (a
            // renewal payment revalidates it) but lock the app for now.
            m_hadSavedKey.store(false);
            m_status.store(Status::Expired);
            ent::setLicensed(false);
        } else if (result == 0) {
            // Server explicitly rejected it: drop any saved copy and lock.
            deleteSavedKey();
            m_hadSavedKey.store(false);
            m_status.store(Status::Invalid);
            ent::setLicensed(false);
        } else {
            m_status.store(Status::NetworkError);
            // Offline grace: a previously-saved key keeps entitlement so
            // paying users offline are never degraded.
            ent::setLicensed(m_hadSavedKey.load());
        }
        m_busy.store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// Storage: %LOCALAPPDATA%/AntigravityVoiceEngine/license.key
// (same folder the installer uses, so the key survives reinstalls)
// ---------------------------------------------------------------------------

// Stable per-machine fingerprint: Windows MachineGuid (unique per OS install),
// FNV-1a hashed to a short hex string so we never transmit the raw GUID.
std::string LicenseManager::deviceId() {
    std::string raw = "unknown";
#ifdef _WIN32
    char buf[256];
    DWORD sz = sizeof(buf);
    // Force the 64-bit registry view so a 32-bit build reads the same value.
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                     "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     nullptr, buf, &sz) == ERROR_SUCCESS) {
        raw = buf;
    }
#endif
    uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
    for (unsigned char c : raw) {
        h ^= c;
        h *= 1099511628211ULL; // FNV prime
    }
    char out[17];
    snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
    return std::string(out);
}

std::string LicenseManager::licenseFilePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    fs::path dir = base ? fs::path(base) / "AntigravityVoiceEngine" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "license.key").string();
}

void LicenseManager::writeDeviceIdFile() {
    const char* base = std::getenv("LOCALAPPDATA");
    fs::path dir = base ? fs::path(base) / "AntigravityVoiceEngine" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f((dir / "device.id").string(), std::ios::trunc);
    if (f) f << deviceId() << "\n";
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

// Separate file from license.key: reset() and an invalid/expired key both
// clear license.key (to lock the app), but this one is left alone so the
// activation screen can still pre-fill whatever key last worked here.
std::string LicenseManager::lastKeyFilePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    fs::path dir = base ? fs::path(base) / "AntigravityVoiceEngine" : fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return (dir / "last_key.txt").string();
}

void LicenseManager::saveLastKey(const std::string& key) {
    std::ofstream f(lastKeyFilePath(), std::ios::trunc);
    f << key << "\n";
}

std::string LicenseManager::getLastKey() {
    std::ifstream f(lastKeyFilePath());
    if (!f) return "";
    std::string key;
    std::getline(f, key);
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              key.end());
    return key;
}

// Self-integrity: FNV-1a hash of this executable's own bytes, computed once
// and cached. Sent to the server so the owner can spot obviously-patched
// binaries on the dashboard. Telemetry-grade (a smart cracker can replay the
// original hash), but it catches lazy edits and costs nothing.
static std::string selfHash() {
    static std::string cached;
    if (!cached.empty()) return cached;
#ifdef _WIN32
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) { cached = "0"; return cached; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { cached = "0"; return cached; }
    uint64_t h = 1469598103934665603ULL;
    char buf[65536];
    while (f) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    char out[17];
    snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
    cached = out;
#else
    cached = "0";
#endif
    return cached;
}

// Pulls "field":"value" out of a flat JSON body (no JSON library needed)
static std::string extractJsonString(const std::string& body, const std::string& field) {
    std::string needle = "\"" + field + "\":\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    size_t end = body.find('"', p);
    if (end == std::string::npos) return "";
    return body.substr(p, end - p);
}

// ---------------------------------------------------------------------------
// HTTPS GET via WinHTTP. Generous receive timeout because a sleeping Render
// free instance can take ~50s to cold-start before answering.
// ---------------------------------------------------------------------------

#ifdef _WIN32
// GET https://<kServerHost><path>, returns response body ("" = transport error).
static std::string httpGetBody(const std::wstring& path) {
    std::string body;

    HINTERNET hSession = WinHttpOpen(L"AntigravityVoiceEngine/" APP_VERSION,
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return body;
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

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > 4096) break; // responses are tiny; cap defensively
        }
    }

    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}
#endif

int LicenseManager::verifyOnline(const std::string& key) {
#ifdef _WIN32
    // Key alphabet is [A-Za-z0-9_-], device id is hex, os/version tags are
    // alphanumeric-dot-dash: all safe to embed in a URL directly.
    std::string dev = deviceId();
    std::string os = osTag();
    std::string q = "/api/verify?key=" + key + "&device=" + dev +
                    "&os=" + os + "&v=" + APP_VERSION + "&h=" + selfHash();
    std::wstring path(q.begin(), q.end());

    std::string body = httpGetBody(path);

    if (body.find("\"valid\":true") != std::string::npos) {
        std::lock_guard<std::mutex> lock(m_planMutex);
        m_plan = extractJsonString(body, "plan");           // "lifetime" or "monthly"
        m_paidUntil = extractJsonString(body, "paidUntil");  // set only for monthly
        return 1;
    }
    if (body.find("device_limit") != std::string::npos)         return 2;
    if (body.find("expired") != std::string::npos)              return 3;
    if (body.find("\"valid\":false") != std::string::npos)      return 0;
    return -1; // empty/garbage (proxy error page, etc.) = network error
#else
    (void)key;
    return -1;
#endif
}

std::string LicenseManager::getPlan() {
    std::lock_guard<std::mutex> lock(m_planMutex);
    return m_plan;
}

std::string LicenseManager::getPaidUntil() {
    std::lock_guard<std::mutex> lock(m_planMutex);
    return m_paidUntil;
}

void LicenseManager::releaseOnline(const std::string& key) {
#ifdef _WIN32
    std::string q = "/api/release?key=" + key + "&device=" + deviceId();
    std::wstring path(q.begin(), q.end());
    httpGetBody(path); // best-effort; response intentionally ignored
#else
    (void)key;
#endif
}

// ---------------------------------------------------------------------------
// Update check: /api/version returns {"version":"x.y.z","url":"https://..."}.
// A non-empty version different from this build's shows an in-app banner.
// ---------------------------------------------------------------------------

void LicenseManager::checkForUpdate() {
    std::thread([this]() {
#ifdef _WIN32
        std::string body = httpGetBody(L"/api/version");
        std::string version = extractJsonString(body, "version");
        std::string url = extractJsonString(body, "url");
        if (!version.empty() && version != APP_VERSION) {
            {
                std::lock_guard<std::mutex> lock(m_updateMutex);
                m_updateVersion = version;
                m_updateUrl = url;
            }
            m_updateAvailable.store(true);
        }
#endif
    }).detach();
}

std::string LicenseManager::getUpdateVersion() {
    std::lock_guard<std::mutex> lock(m_updateMutex);
    return m_updateVersion;
}

void LicenseManager::openUpdatePage() {
#ifdef _WIN32
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_updateMutex);
        url = m_updateUrl;
    }
    if (url.empty()) url = kBuyUrl; // fall back to the site
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}
