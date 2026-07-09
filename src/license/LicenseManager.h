#pragma once
#include <string>
#include <atomic>
#include <mutex>

// Verifies a purchased license key against the license server (server/ in this
// repo, deployed on Render). Keys are bought via Mollie on the server's /buy page.
//
// Policy: the app is locked until a key validates. A previously saved key
// unlocks the app immediately while it re-verifies in the background, and a
// network failure keeps it unlocked (offline grace) - paying users are never
// punished for the server sleeping or being offline.
class LicenseManager {
public:
    enum class Status {
        NoKey,        // nothing entered/saved yet
        Checking,     // verification request in flight
        Valid,        // server confirmed the key
        Invalid,      // server rejected the key
        DeviceLimit,  // real key, but activated on too many devices
        Expired,      // monthly subscription lapsed
        NetworkError  // could not reach the server
    };

    LicenseManager();

    // Load the saved key (if any) and start re-verifying it in the background.
    void init();

    // Write this PC's anonymous device id to device.id in the app data folder,
    // so the uninstaller can tell the server this machine was removed.
    static void writeDeviceIdFile();

    // Verify a user-entered key in the background; saves it on success.
    void activate(const std::string& key);

    // "Reset License Key": clears the saved key and frees this device's slot
    // on the server, then locks the app so a key must be entered again. The
    // key itself is kept in a separate recall file (see getLastKey()) so the
    // activation screen can pre-fill it - resetting doesn't mean retyping it.
    void reset();

    // Last key that was successfully activated on this PC, kept even after
    // Reset so the activation screen can pre-fill it instead of the user
    // having to dig it up again. Empty if nothing has ever activated here.
    static std::string getLastKey();

    // "Unbind All Devices": frees every device slot for the current key on
    // the server (all PCs), then re-verifies so this PC re-registers. Use
    // after hitting the device limit from reinstalls or replaced machines.
    void unbindAllDevices();

    // Open the server's purchase page in the default browser.
    void openPurchasePage() const;

    // Update check (async): asks the server for the latest released version.
    void checkForUpdate();
    bool isUpdateAvailable() const { return m_updateAvailable.load(); }
    std::string getUpdateVersion();
    void openUpdatePage();

    Status getStatus() const { return m_status.load(); }

    // "lifetime" or "monthly" (empty until the first successful verify with
    // device binding enabled on the server).
    std::string getPlan();
    // ISO date string, only meaningful when getPlan() == "monthly".
    std::string getPaidUntil();

    // True when the main app should be usable.
    bool isUnlocked() const {
        Status s = m_status.load();
        if (s == Status::Valid) return true;
        // Saved key: stay unlocked during re-check and on network failure.
        return m_hadSavedKey.load() && (s == Status::Checking || s == Status::NetworkError);
    }

private:
    void verifyAsync(std::string key, bool saveOnSuccess);
    // Stable, opaque per-machine id (hashed Windows MachineGuid)
    static std::string deviceId();
    static std::string licenseFilePath();
    static std::string loadSavedKey();
    static void saveKey(const std::string& key);
    static void deleteSavedKey();
    // Separate recall cache, deliberately NOT cleared by reset()/deleteSavedKey()
    static std::string lastKeyFilePath();
    static void saveLastKey(const std::string& key);

    // Returns 1 = valid, 0 = invalid, 2 = device limit, 3 = expired, -1 = network error.
    // Not static: fills m_plan/m_paidUntil from the response on success.
    int verifyOnline(const std::string& key);
    // Frees this device's slot server-side (fire and forget)
    static void releaseOnline(const std::string& key);

    std::atomic<Status> m_status{Status::NoKey};
    std::atomic<bool> m_hadSavedKey{false};
    std::atomic<bool> m_busy{false};

    // Key currently in use (set on successful verify), for reset/release
    std::mutex m_keyMutex;
    std::string m_activeKey;

    // Plan info from the last successful verify
    std::mutex m_planMutex;
    std::string m_plan;
    std::string m_paidUntil;

    // Update-check state (filled by the background thread)
    std::atomic<bool> m_updateAvailable{false};
    std::mutex m_updateMutex;
    std::string m_updateVersion;
    std::string m_updateUrl;
};
