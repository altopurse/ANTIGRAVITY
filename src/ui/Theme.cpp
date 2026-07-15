#include "Theme.h"
#include <cstdio>

namespace theme {

ImFont* FontBody    = nullptr;
ImFont* FontBold    = nullptr;
ImFont* FontHeading = nullptr;
ImFont* FontSmall   = nullptr;
float   Scale       = 1.0f;

namespace {
bool fileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

ImFont* addFont(const char* path, float sizePx) {
    ImGuiIO& io = ImGui::GetIO();
    if (fileExists(path)) {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, sizePx);
        if (f) return f;
    }
    return nullptr;
}
} // namespace

void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const char* regular  = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold     = "C:\\Windows\\Fonts\\segoeuib.ttf";
    const char* semibold = "C:\\Windows\\Fonts\\seguisb.ttf"; // Segoe UI Semibold

    FontBody    = addFont(regular, 17.0f * scale);
    FontBold    = addFont(bold,    17.0f * scale);
    FontHeading = addFont(fileExists(semibold) ? semibold : bold, 25.0f * scale);
    FontSmall   = addFont(regular, 13.5f * scale);

    // Fallback: ship with whatever ImGui has built in rather than crash
    if (!FontBody)    FontBody    = io.Fonts->AddFontDefault();
    if (!FontBold)    FontBold    = FontBody;
    if (!FontHeading) FontHeading = FontBold;
    if (!FontSmall)   FontSmall   = FontBody;

    io.FontDefault = FontBody;
}

void Apply(float scale) {
    Scale = scale;
    ImGuiStyle& style = ImGui::GetStyle();

    // Metrics (pre-scale values; ScaleAllSizes applies DPI)
    style.WindowPadding     = ImVec2(14, 12);
    style.FramePadding      = ImVec2(10, 6);
    style.CellPadding       = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(9, 8);
    style.ItemInnerSpacing  = ImVec2(6, 5);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 9.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 7.0f;
    style.GrabRounding      = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;

    style.ScaleAllSizes(scale);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                  = Text;
    c[ImGuiCol_TextDisabled]          = TextDim;
    c[ImGuiCol_WindowBg]              = Bg;
    c[ImGuiCol_ChildBg]               = Panel;
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.11f, 0.16f, 0.99f);
    c[ImGuiCol_Border]                = Border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = PanelAlt;
    c[ImGuiCol_FrameBgHovered]        = PanelHover;
    c[ImGuiCol_FrameBgActive]         = PanelActive;

    c[ImGuiCol_TitleBg]               = Bg;
    c[ImGuiCol_TitleBgActive]         = Bg;
    c[ImGuiCol_TitleBgCollapsed]      = Bg;
    c[ImGuiCol_MenuBarBg]             = Panel;

    c[ImGuiCol_Header]                = PanelAlt;
    c[ImGuiCol_HeaderHovered]         = PanelHover;
    c[ImGuiCol_HeaderActive]          = PanelActive;

    c[ImGuiCol_Button]                = Accent;
    c[ImGuiCol_ButtonHovered]         = AccentHi;
    c[ImGuiCol_ButtonActive]          = AccentLo;

    c[ImGuiCol_CheckMark]             = AccentHi;
    c[ImGuiCol_SliderGrab]            = Accent;
    c[ImGuiCol_SliderGrabActive]      = AccentHi;

    c[ImGuiCol_Separator]             = Border;
    c[ImGuiCol_SeparatorHovered]      = AccentLo;
    c[ImGuiCol_SeparatorActive]       = Accent;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = PanelHover;
    c[ImGuiCol_ScrollbarGrabHovered]  = PanelActive;
    c[ImGuiCol_ScrollbarGrabActive]   = AccentLo;

    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = AccentLo;
    c[ImGuiCol_ResizeGripActive]      = Accent;

    c[ImGuiCol_PlotLines]             = Cyan;
    c[ImGuiCol_PlotLinesHovered]      = AccentHi;
    c[ImGuiCol_PlotHistogram]         = Accent;
    c[ImGuiCol_PlotHistogramHovered]  = AccentHi;

    c[ImGuiCol_TableBorderStrong]     = Border;
    c[ImGuiCol_TableBorderLight]      = Border;
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.02f);

    c[ImGuiCol_TextSelectedBg]        = AccentSoft;
    c[ImGuiCol_NavHighlight]          = Accent;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.02f, 0.02f, 0.04f, 0.60f);
}

} // namespace theme
