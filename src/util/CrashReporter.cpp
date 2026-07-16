#include "CrashReporter.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#endif

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

// Set from CrashReporter::setConsent once the user accepts Terms & Privacy.
// The dying process reads it with a relaxed load; the local crash.txt is
// written regardless, only the network ping is gated.
std::atomic<bool> g_consent{false};

#ifdef _WIN32
const wchar_t* kServerHost = L"antigravity-license.onrender.com";

std::string osTag() {
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
}

// The process is dying; everything here is best-effort and must not throw.
void report(const char* kind, unsigned long code, void* address) {
    char codeHex[32], addrHex[32];
    snprintf(codeHex, sizeof(codeHex), "0x%08lx", code);
    snprintf(addrHex, sizeof(addrHex), "%p", address);

    // 1. Local breadcrumb next to the config so users can attach it to reports
    const char* base = getenv("APPDATA");
    if (base) {
        std::string path = std::string(base) + "\\Antigravity\\crash.txt";
        if (FILE* f = fopen(path.c_str(), "a")) {
            fprintf(f, "crash kind=%s code=%s addr=%s version=%s os=%s\n",
                    kind, codeHex, addrHex, APP_VERSION, osTag().c_str());
            fclose(f);
        }
    }

    // 2. One short-timeout ping to the server (fire and forget) - only if the
    // user consented; without consent we keep the local breadcrumb only.
    if (!g_consent.load(std::memory_order_relaxed)) return;
    std::string q = std::string("/api/crash?v=") + APP_VERSION +
                    "&os=" + osTag() + "&kind=" + kind + "&code=" + codeHex;
    std::wstring path(q.begin(), q.end());

    HINTERNET hSession = WinHttpOpen(L"AntigravityVoiceEngine/" APP_VERSION,
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    // Short timeouts: don't hang a dying process on a sleeping server
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
    HINTERNET hConnect = WinHttpConnect(hSession, kServerHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                WinHttpReceiveResponse(hRequest, nullptr);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS* info) {
    unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    report("seh", code, addr);
    return EXCEPTION_EXECUTE_HANDLER; // let the process exit
}

void onTerminate() {
    report("terminate", 0, nullptr);
    abort();
}
#endif

} // namespace

namespace CrashReporter {

void install() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(sehFilter);
    std::set_terminate(onTerminate);
#endif
}

void setConsent(bool granted) {
    g_consent.store(granted, std::memory_order_relaxed);
}

} // namespace CrashReporter
