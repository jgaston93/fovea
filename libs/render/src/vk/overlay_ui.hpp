#pragma once

#include "fovea/render/renderer.hpp"

namespace fovea {

// The overlay's widgets, between ImGui::NewFrame and ImGui::Render.
void build_overlay_ui(const OverlayContent& content);

}  // namespace fovea
