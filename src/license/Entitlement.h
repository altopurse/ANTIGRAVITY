#pragma once
#include <atomic>
#include <cstdint>

// Anti-tamper "scatter": entitlement is deliberately NOT a single bool that a
// cracker can flip in one place. Two independent global words must both hold
// their magic value for the app to run un-degraded:
//   g_lic   - set only inside LicenseManager's real "server said valid / valid
//             saved key on grace" branches (a different code location from the
//             UI's isUnlocked() gate). Patching the UI gate does NOT set this.
//   g_clean - cleared by the anti-debug / integrity checks if tampering is seen.
//
// The audio engine (a separate translation unit) reads ok() every block and
// silently degrades output when it's false. So a naive "make isUnlocked return
// true" patch opens the UI but leaves the sound quietly broken for real use.
namespace ent {

extern std::atomic<uint32_t> g_lic;
extern std::atomic<uint32_t> g_clean;

// Non-obvious magics (not 1/true). Compared, never branched on as a bool.
constexpr uint32_t MAGIC_LIC   = 0x5CA1AB1Eu;
constexpr uint32_t MAGIC_CLEAN = 0xC0FFEE11u;

inline bool ok() {
    return g_lic.load(std::memory_order_relaxed) == MAGIC_LIC
        && g_clean.load(std::memory_order_relaxed) == MAGIC_CLEAN;
}

// Called by the license verify path (set true only on genuine entitlement).
void setLicensed(bool entitled);

// Called by the anti-debug / integrity checks (true = looks clean).
void setClean(bool clean);

// Runs the anti-debug + self checks and updates g_clean. Cheap; call
// periodically from the UI loop. Defined in Entitlement.cpp.
void runTamperChecks();

} // namespace ent
