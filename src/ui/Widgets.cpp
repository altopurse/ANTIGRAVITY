#include "Widgets.h"
#include "Theme.h"
#include <algorithm>
#include <cstring>

namespace widgets {

void SectionLabel(const char* text) {
    ImGui::PushFont(theme::FontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
    // Manual letter-spacing: render each char with a small gap
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float x = pos.x;
    const float tracking = theme::S(1.2f);
    for (const char* p = text; *p; ++p) {
        char buf[2] = { *p, 0 };
        dl->AddText(ImVec2(x, pos.y), ImGui::GetColorU32(theme::TextDim), buf);
        x += ImGui::CalcTextSize(buf).x + tracking;
    }
    ImGui::Dummy(ImVec2(x - pos.x, ImGui::GetTextLineHeight()));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
}

bool ToggleSwitch(const char* id, bool* value, const char* label) {
    const float h = ImGui::GetFrameHeight() * 0.80f;
    const float w = h * 1.75f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    // Vertically center against a normal frame row
    float yOff = (ImGui::GetFrameHeight() - h) * 0.5f;
    p.y += yOff;

    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *value = !*value; changed = true; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hovered = ImGui::IsItemHovered();
    ImVec4 track = *value ? (hovered ? theme::AccentHi : theme::Accent)
                          : (hovered ? theme::PanelActive : theme::PanelHover);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(track), h * 0.5f);

    float r = h * 0.5f - theme::S(2.0f);
    float cx = *value ? (p.x + w - h * 0.5f) : (p.x + h * 0.5f);
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32(238, 240, 245, 255), 24);

    if (label && label[0]) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
    }
    return changed;
}

void VuMeter(float value01, float height) {
    float v = std::clamp(value01, 0.0f, 1.0f);
    float w = ImGui::GetContentRegionAvail().x;
    float h = theme::S(height);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const int   nseg = 28;
    const float gap  = theme::S(2.0f);
    const float segW = (w - gap * (nseg - 1)) / nseg;

    for (int i = 0; i < nseg; ++i) {
        float frac = static_cast<float>(i + 1) / nseg;
        ImVec4 col = (frac <= 0.62f) ? theme::Green
                   : (frac <= 0.85f) ? theme::Yellow
                                     : theme::Red;
        bool lit = v >= (static_cast<float>(i) + 0.5f) / nseg;
        if (!lit) col.w = 0.14f;
        float x0 = p.x + i * (segW + gap);
        dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x0 + segW, p.y + h),
                          ImGui::GetColorU32(col), theme::S(2.0f));
    }
    ImGui::Dummy(ImVec2(w, h));
}

void StatusPill(const char* text, const ImVec4& color) {
    ImGui::PushFont(theme::FontSmall);
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float h = ImGui::GetFrameHeight();
    float dotR = theme::S(3.5f);
    float padX = theme::S(11.0f);
    float w = padX * 2 + dotR * 2 + theme::S(6.0f) + textSize.x;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = color; bg.w = 0.14f;
    ImVec4 bd = color; bd.w = 0.45f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bd), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + padX + dotR, p.y + h * 0.5f), dotR,
                        ImGui::GetColorU32(color), 16);
    dl->AddText(ImVec2(p.x + padX + dotR * 2 + theme::S(6.0f),
                       p.y + (h - textSize.y) * 0.5f),
                ImGui::GetColorU32(color), text);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopFont();
}

bool ColoredButton(const char* label, const ImVec2& size, const ImVec4& base) {
    ImVec4 hover = ImVec4(std::min(base.x * 1.18f + 0.03f, 1.0f),
                          std::min(base.y * 1.18f + 0.03f, 1.0f),
                          std::min(base.z * 1.18f + 0.03f, 1.0f), base.w);
    ImVec4 active = ImVec4(base.x * 0.82f, base.y * 0.82f, base.z * 0.82f, base.w);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

void Divider() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

} // namespace widgets
