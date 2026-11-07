#include "overlay_ui.hpp"

#include <imgui.h>

namespace fovea {

void build_overlay_ui(const OverlayContent& c) {
    ImGui::SetNextWindowPos(ImVec2(12.0F, 12.0F));
    ImGui::SetNextWindowBgAlpha(0.65F);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::Begin("Fovea frame timing", nullptr, flags);
    const double fps = c.interval_mean_ms > 0.0 ? 1000.0 / c.interval_mean_ms : 0.0;
    ImGui::Text("%.1f fps (target %.1f)", fps, 1000.0 / static_cast<double>(c.target_ms));
    ImGui::Text("interval %.2f ms mean, %.2f max", c.interval_mean_ms, c.interval_max_ms);
    ImGui::Text("missed %llu of %llu", static_cast<unsigned long long>(c.missed),
                static_cast<unsigned long long>(c.frames));
    ImGui::Text("CPU work %.2f ms", c.work_mean_ms);
    if (c.host_age_ms >= 0.0F) {
        ImGui::Text("Host sample %.1f ms old", static_cast<double>(c.host_age_ms));
    }
    if (c.terrain.resident > 0) {
        ImGui::Text("Terrain %u drawn, %u resident (%.0f MB), %u loading", c.terrain.drawn,
                    c.terrain.resident, static_cast<double>(c.terrain.bytes) / 1048576.0,
                    c.terrain.loading);
        ImGui::Text("  %u uploads, %u misses", c.terrain.uploads, c.terrain.misses);
    }
    if (!c.note.empty()) {
        ImGui::TextUnformatted(c.note.data(), c.note.data() + c.note.size());
    }
    for (std::uint32_t i = 0; i < c.gpu_outputs; ++i) {
        ImGui::Text("GPU output %u: %.3f ms", i, static_cast<double>(c.gpu_ms[i]));
    }
    // Recent frame intervals, oldest on the left; the scale tops out at three
    // periods, so a missed frame stands out as a spike.
    if (!c.intervals_ms.empty()) {
        ImGui::PlotLines("##intervals", c.intervals_ms.data(),
                         static_cast<int>(c.intervals_ms.size()),
                         static_cast<int>(c.intervals_oldest), "frame interval", 0.0F,
                         3.0F * c.target_ms, ImVec2(260.0F, 70.0F));
    }
    ImGui::End();
}

}  // namespace fovea
