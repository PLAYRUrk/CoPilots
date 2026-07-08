#include "Theme.h"
#include <imgui.h>

namespace cp {
namespace ui {

void Theme::apply()
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding   = 8.f;
    s.ChildRounding    = 6.f;
    s.FrameRounding    = 4.f;
    s.PopupRounding    = 6.f;
    s.ScrollbarRounding= 4.f;
    s.GrabRounding     = 4.f;
    s.TabRounding      = 4.f;

    s.WindowPadding  = ImVec2(12.f, 10.f);
    s.FramePadding   = ImVec2(8.f,  4.f);
    s.ItemSpacing    = ImVec2(8.f,  6.f);
    s.ItemInnerSpacing = ImVec2(4.f, 4.f);
    s.ScrollbarSize  = 12.f;
    s.GrabMinSize    = 8.f;
    s.WindowBorderSize = 1.f;
    s.FrameBorderSize  = 0.f;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.11f, 0.12f, 0.14f, 0.97f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.09f, 0.10f, 0.12f, 0.60f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);

    c[ImGuiCol_Border]           = ImVec4(0.24f, 0.26f, 0.30f, 0.70f);
    c[ImGuiCol_BorderShadow]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(kAccentR*0.6f, kAccentG*0.4f, kAccentB*0.5f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.09f, 0.11f, 0.75f);

    c[ImGuiCol_MenuBarBg]        = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);

    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.08f, 0.09f, 0.10f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.28f, 0.30f, 0.34f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(kAccentR, kAccentG, kAccentB, 0.70f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    c[ImGuiCol_CheckMark]        = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    c[ImGuiCol_SliderGrab]       = ImVec4(kAccentR, kAccentG, kAccentB, 0.90f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(kAccentR*1.2f, kAccentG, kAccentB, 1.00f);

    c[ImGuiCol_Button]           = ImVec4(kAccentR*0.4f, kAccentG*0.3f, kAccentB*0.5f, 0.70f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(kAccentR,      kAccentG,      kAccentB,      0.85f);
    c[ImGuiCol_ButtonActive]     = ImVec4(kAccentR*1.1f, kAccentG*0.9f, kAccentB,      1.00f);

    c[ImGuiCol_Header]           = ImVec4(kAccentR*0.4f, kAccentG*0.3f, kAccentB*0.5f, 0.60f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(kAccentR*0.6f, kAccentG*0.5f, kAccentB*0.7f, 0.80f);
    c[ImGuiCol_HeaderActive]     = ImVec4(kAccentR,      kAccentG,      kAccentB,      0.90f);

    c[ImGuiCol_Separator]        = ImVec4(0.28f, 0.30f, 0.34f, 0.80f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(kAccentR, kAccentG, kAccentB, 0.80f);
    c[ImGuiCol_SeparatorActive]  = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    c[ImGuiCol_ResizeGrip]       = ImVec4(kAccentR, kAccentG, kAccentB, 0.20f);
    c[ImGuiCol_ResizeGripHovered]= ImVec4(kAccentR, kAccentG, kAccentB, 0.70f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);

    c[ImGuiCol_Tab]              = ImVec4(0.12f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(kAccentR*0.6f, kAccentG*0.5f, kAccentB*0.7f, 1.00f);
    c[ImGuiCol_TabActive]        = ImVec4(kAccentR*0.5f, kAccentG*0.4f, kAccentB*0.6f, 1.00f);
    c[ImGuiCol_TabUnfocused]     = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);

    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.17f, 0.20f, 0.80f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.22f, 0.24f, 0.28f, 0.80f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(kAccentR*0.3f, kAccentG*0.2f, kAccentB*0.4f, 0.80f);

    c[ImGuiCol_Text]             = ImVec4(0.92f, 0.93f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.45f, 0.47f, 0.52f, 1.00f);
    c[ImGuiCol_TextSelectedBg]   = ImVec4(kAccentR*0.4f, kAccentG*0.3f, kAccentB*0.5f, 0.70f);

    c[ImGuiCol_PlotLines]        = ImVec4(kAccentR, kAccentG, kAccentB, 1.00f);
    c[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]    = ImVec4(kAccentR, kAccentG*0.8f, 0.20f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    c[ImGuiCol_NavHighlight]     = ImVec4(kAccentR, kAccentG, kAccentB, 0.80f);

    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
}

}
}
