#pragma once

// Minimal crash telemetry: installs process-wide handlers (SEH unhandled
// exception filter + std::terminate) that, when the app is dying anyway,
// write a small crash.txt next to the config and fire one best-effort HTTP
// ping to the license server (/api/crash) with the version, OS and exception
// code - just enough to know what's breaking on users' machines.
namespace CrashReporter {
void install();
}
