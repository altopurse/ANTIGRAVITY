#pragma once
#include "imgui.h"

// Small reusable UI pieces shared by every screen/panel.
namespace widgets {

// Dim, small, uppercase section label ("DEVICES", "MONITORING", ...)
void SectionLabel(const char* text);

// iOS-style on/off switch. Optional visible label to the right.
// Returns true when toggled this frame.
bool ToggleSwitch(const char* id, bool* value, const char* label = nullptr);

// Segmented level meter (0..1), full available width, green->yellow->red.
void VuMeter(float value01, float height = 14.0f);

// Rounded status chip with a leading dot, e.g. "Running @ 48000 Hz".
void StatusPill(const char* text, const ImVec4& color);

// Button in a custom color family (base is auto-lightened for hover/active).
bool ColoredButton(const char* label, const ImVec2& size, const ImVec4& base);

// Full-width thin divider with vertical breathing room.
void Divider();

} // namespace widgets
