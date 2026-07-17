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
extern std::atomic<uint32_t> g_feat;

// Non-obvious magics (not 1/true). Compared, never branched on as a bool.
constexpr uint32_t MAGIC_LIC   = 0x5CA1AB1Eu;
constexpr uint32_t MAGIC_CLEAN = 0xC0FFEE11u;
// XOR cloak for the feature word: the raw stored value is (mask ^ MAGIC_FEAT),
// so a memory scan for a plausible bitmask (0, 6, ...) finds nothing, and a
// cracker who blindly writes "all bits set" produces a garbage mask, not FULL.
constexpr uint32_t MAGIC_FEAT  = 0xA5A5F00Du;

// Feature bits. Free tier gets none of these (base features always work);
// a valid paid key grants FULL. Kept as odd bits so the cloaked word never
// looks like a small integer.
constexpr uint32_t FEAT_ALL_EFFECTS     = 0x00000002u; // every DSP effect (free = reverb + pitch only)
constexpr uint32_t FEAT_UNLIMITED_CLIPS = 0x00000008u; // soundboard beyond the free cap
constexpr uint32_t FREE_MASK = 0u;
constexpr uint32_t FULL_MASK = FEAT_ALL_EFFECTS | FEAT_UNLIMITED_CLIPS;

// Free-tier soundboard cap (paid = unlimited).
constexpr int FREE_CLIP_LIMIT = 2;

inline bool clean() {
    return g_clean.load(std::memory_order_relaxed) == MAGIC_CLEAN;
}

// Fully entitled (paid) AND integrity intact. The audio path degrades when
// this is false only for tamper reasons now; free users are never degraded.
inline bool ok() {
    return g_lic.load(std::memory_order_relaxed) == MAGIC_LIC
        && g_clean.load(std::memory_order_relaxed) == MAGIC_CLEAN;
}

// Decoded feature mask (0 if never set / cloak mismatch).
inline uint32_t features() {
    return g_feat.load(std::memory_order_relaxed) ^ MAGIC_FEAT;
}

// True only if the given feature bit is granted AND integrity is intact.
// Gating on clean() here is what gives premium features their teeth: a
// patched binary that trips the tamper checks loses premium features even
// if it forced the mask.
inline bool hasFeature(uint32_t bit) {
    return (features() & bit) != 0u && clean();
}

// Called by the license verify path. Grants FULL on genuine entitlement,
// drops to FREE otherwise (the app stays usable at the free tier).
void setLicensed(bool entitled);

// ---- Pro trial (one per app session, no persistence) ----
// The sale happens in the ear: a free user who *hears* the Robot/Distortion/
// Telephone effects live converts far better than one reading a locked list.
// startTrial grants FULL for `seconds`; when it lapses tickTrial drops back
// to FREE (unless a real license arrived meanwhile). Deliberately session-
// scoped: restarting the app re-arms it - generous, but the point is selling
// the sound, not building trial DRM (the whole client is patchable anyway).
bool startTrial(int seconds);   // false if already used or already licensed
void tickTrial();               // call every UI frame; handles expiry
bool trialActive();
int  trialSecondsLeft();        // 0 when inactive
bool trialUsed();               // true once startTrial succeeded this session

// Directly set the entitlement mask (server-driven, future per-plan masks).
void setEntitlement(uint32_t mask);

// Called by the anti-debug / integrity checks (true = looks clean).
void setClean(bool cleanFlag);

// Runs the anti-debug + self checks and updates g_clean. Cheap; call
// periodically from the UI loop. Defined in Entitlement.cpp.
void runTamperChecks();

} // namespace ent
