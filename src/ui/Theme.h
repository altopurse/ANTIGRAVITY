#pragma once
#include "imgui.h"

// Central visual identity: palette, fonts and style metrics.
// Every color used by the UI lives here so the look can be retuned in one place.
namespace theme {

// ---- Palette -------------------------------------------------------------
inline const ImVec4 Bg         = ImVec4(0.047f, 0.055f, 0.086f, 1.00f); // window backdrop
inline const ImVec4 Panel      = ImVec4(0.082f, 0.094f, 0.137f, 1.00f); // cards / panels
inline const ImVec4 PanelAlt   = ImVec4(0.118f, 0.133f, 0.188f, 1.00f); // inputs, frames
inline const ImVec4 PanelHover = ImVec4(0.157f, 0.176f, 0.243f, 1.00f);
inline const ImVec4 PanelActive= ImVec4(0.196f, 0.220f, 0.302f, 1.00f);
inline const ImVec4 Border     = ImVec4(0.165f, 0.184f, 0.251f, 1.00f);

inline const ImVec4 Text       = ImVec4(0.925f, 0.933f, 0.961f, 1.00f);
inline const ImVec4 TextDim    = ImVec4(0.549f, 0.580f, 0.663f, 1.00f);

inline const ImVec4 Accent     = ImVec4(0.510f, 0.431f, 0.980f, 1.00f); // violet
inline const ImVec4 AccentHi   = ImVec4(0.616f, 0.549f, 1.000f, 1.00f);
inline const ImVec4 AccentLo   = ImVec4(0.353f, 0.302f, 0.702f, 1.00f);
inline const ImVec4 AccentSoft = ImVec4(0.510f, 0.431f, 0.980f, 0.16f); // tinted fills

inline const ImVec4 Cyan       = ImVec4(0.251f, 0.749f, 0.949f, 1.00f);
inline const ImVec4 Green      = ImVec4(0.298f, 0.820f, 0.478f, 1.00f);
inline const ImVec4 GreenHi    = ImVec4(0.380f, 0.900f, 0.560f, 1.00f);
inline const ImVec4 GreenLo    = ImVec4(0.137f, 0.478f, 0.263f, 1.00f);
inline const ImVec4 Yellow     = ImVec4(0.949f, 0.749f, 0.298f, 1.00f);
inline const ImVec4 Orange     = ImVec4(0.949f, 0.560f, 0.251f, 1.00f);
inline const ImVec4 Red        = ImVec4(0.941f, 0.329f, 0.353f, 1.00f);
inline const ImVec4 RedHi      = ImVec4(1.000f, 0.420f, 0.440f, 1.00f);
inline const ImVec4 RedLo      = ImVec4(0.478f, 0.157f, 0.176f, 1.00f);
inline const ImVec4 Purple     = ImVec4(0.790f, 0.610f, 0.950f, 1.00f);

// ---- Fonts (null until LoadFonts; ImGui default is the fallback) ----------
extern ImFont* FontBody;
extern ImFont* FontBold;
extern ImFont* FontHeading;
extern ImFont* FontSmall;

// UI scale factor set by Apply(); use S() for fixed pixel sizes so the
// layout tracks the monitor's DPI.
extern float Scale;
inline float S(float v) { return v * Scale; }

// Load Segoe UI variants from the Windows fonts folder (falls back to the
// built-in ImGui font when unavailable). Call once after ImGui::CreateContext.
void LoadFonts(float scale);

// Apply the full style: metrics (scaled) + palette. Call after LoadFonts.
void Apply(float scale);

} // namespace theme
