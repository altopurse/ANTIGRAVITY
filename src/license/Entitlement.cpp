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
