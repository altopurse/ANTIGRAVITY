#include "Entitlement.h"

#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ent {

// Default g_clean to "clean" so legit users are never degraded; only the
// anti-debug/integrity checks can clear it. g_lic defaults to not-entitled.
// g_feat defaults to the (cloaked) FREE mask so the app is immediately usable
// at the free tier before any license check runs.
std::atomic<uint32_t> g_lic{0};
std::atomic<uint32_t> g_clean{MAGIC_CLEAN};
std::atomic<uint32_t> g_feat{FREE_MASK ^ MAGIC_FEAT};

void setEntitlement(uint32_t mask) {
    g_feat.store(mask ^ MAGIC_FEAT, std::memory_order_relaxed);
}

void setLicensed(bool entitled) {
    // Scramble the "off" value slightly rather than plain 0 so a memory scan
    // for the magic doesn't trivially reveal the on/off pair.
    g_lic.store(entitled ? MAGIC_LIC : 0x13579BDFu, std::memory_order_relaxed);
    // Entitlement follows the licensed flag: paid => everything, otherwise the
    // free tier (base features only). Set from the same verify path as g_lic.
    setEntitlement(entitled ? FULL_MASK : FREE_MASK);
}

void setClean(bool cleanFlag) {
    g_clean.store(cleanFlag ? MAGIC_CLEAN : 0x2468ACE0u, std::memory_order_relaxed);
}

// ---- Pro trial ----

// Epoch-seconds deadline (0 = no active trial) + once-per-session latch.
static std::atomic<int64_t> g_trialEnd{0};
static std::atomic<bool>    g_trialUsed{false};

static int64_t nowSecs() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool startTrial(int seconds) {
    if (g_lic.load(std::memory_order_relaxed) == MAGIC_LIC) return false; // already Pro
    bool expected = false;
    if (!g_trialUsed.compare_exchange_strong(expected, true)) return false; // once per session
    g_trialEnd.store(nowSecs() + seconds, std::memory_order_relaxed);
    setEntitlement(FULL_MASK);
    return true;
}

void tickTrial() {
    int64_t end = g_trialEnd.load(std::memory_order_relaxed);
    if (end == 0) return;
    if (nowSecs() >= end) {
        g_trialEnd.store(0, std::memory_order_relaxed);
        // Drop back to FREE unless a genuine license landed mid-trial.
        if (g_lic.load(std::memory_order_relaxed) != MAGIC_LIC) {
            setEntitlement(FREE_MASK);
        }
    } else {
        // Keep FULL asserted while active: a background verify of a bad saved
        // key calls setLicensed(false) and would otherwise stomp the trial.
        if (g_lic.load(std::memory_order_relaxed) != MAGIC_LIC) {
            setEntitlement(FULL_MASK);
        }
    }
}

bool trialActive() {
    int64_t end = g_trialEnd.load(std::memory_order_relaxed);
    return end != 0 && nowSecs() < end;
}

int trialSecondsLeft() {
    int64_t end = g_trialEnd.load(std::memory_order_relaxed);
    if (end == 0) return 0;
    int64_t left = end - nowSecs();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool trialUsed() {
    return g_trialUsed.load(std::memory_order_relaxed);
}

void runTamperChecks() {
#ifdef _WIN32
    bool clean = true;

    // 1. Attached debugger (own process flag)
    if (IsDebuggerPresent()) clean = false;

    // 2. Remote debugger
    BOOL remote = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) && remote) clean = false;

    // 3. Timing check: a debugger single-stepping this tiny loop blows the
    //    budget wildly. A normal machine finishes in microseconds.
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint32_t acc = 0;
    for (int i = 0; i < 1000; ++i) acc += i * 2654435761u;
    auto t1 = std::chrono::high_resolution_clock::now();
    (void)acc;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (us > 50000) clean = false; // 50ms for 1000 adds => being stepped

    setClean(clean);
#else
    setClean(true);
#endif
}

} // namespace ent
