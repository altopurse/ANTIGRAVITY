#pragma once

// Minimal crash telemetry: installs process-wide handlers (SEH unhandled
// exception filter + std::terminate) that, when the app is dying anyway,
// write a small crash.txt next to the config and fire one best-effort HTTP
// ping to the license server (/api/crash) with the version, OS and exception
// code - just enough to know what's breaking on users' machines.
namespace CrashReporter {
void install();

// Consent gate for the network ping. Until this is set true (user accepted
// Terms & Privacy), a crash still writes the local crash.txt but sends NO
// network request - required now that the free tier can run without ever
// passing through a mandatory activation/consent gate.
void setConsent(bool granted);
}
