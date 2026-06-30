#include "LabelPanel.h"

#include "ImGuiManager.h"
#include <IconsFontAwesome5.h>
#include <imgui.h>

#include <webgpu/engine/Context.h>
#include <webgpu/engine/labels/LabelRenderer.h>

namespace webgpu_app {

LabelPanel::LabelPanel(webgpu_engine::Context* context, webgpu_engine::LabelRenderer* label_renderer)
    : m_context(context)
    , m_label_renderer(label_renderer)
{
}

void LabelPanel::draw()
{
    auto& cfg = m_context->shared_config();
    if (ImGuiManager::FloatingToggleButton("ToggleLabelsButton", ICON_FA_TAG, "Labels", &cfg.m_labels_enabled)) {
        m_context->request_redraw();
    }
}

void LabelPanel::draw_panel()
{
    if (!m_context->shared_config().m_labels_enabled)
        return;

    auto& params = m_label_renderer->get_parameter();
    bool gpu_dirty = false;

    if (ImGui::CollapsingHeader(ICON_FA_TAG "  Labels")) {

        ImGui::SeparatorText("Layout");

        ImGui::SliderFloat("Label scale", &params.label_scale, 0.1f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("Glyph size relative to font em-size.");

        ImGui::SliderFloat("Min label width (px)", &params.min_label_width_px, 0.0f, 80.0f, "%.0f");
        ImGui::SetItemTooltip("Labels narrower than this on screen are culled entirely.");

        ImGui::SliderFloat("Anchor height offset (m)", &params.anchor_height_offset_m, 0.0f, 2000.0f, "%.0f");
        ImGui::SetItemTooltip("Lifts the label anchor above its world position along the local up vector, to avoid clipping into terrain.");

        ImGui::SliderFloat("Label vertical offset (px)", &params.label_vertical_offset_px, 0.0f, 100.0f, "%.0f");
        ImGui::SetItemTooltip("Pure screen-space nudge upward - does not affect 3D position or terrain occlusion.");

        ImGui::SeparatorText("Overlap handling");

        ImGui::SliderFloat("Label padding (px)", &params.label_padding_px, 0.0f, 30.0f, "%.0f");
        ImGui::SetItemTooltip("Extra margin added around each label's screen-space rect when testing for overlap.");

        ImGui::SliderFloat("Occlusion drop threshold", &params.occlusion_drop_threshold, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip(
            "Fraction of a label's area that can be covered by closer labels before it's dropped entirely. Lower = more aggressive decluttering.");

        ImGui::SeparatorText("Fading");

        ImGui::SliderFloat("Fade duration (s)", &params.fade_duration_s, 0.05f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("Time for a label to fully fade in/out when its visibility changes.");

        ImGui::SliderFloat("Distance fade start (m)", &params.distance_fade_start_m, 0.0f, 50000.0f, "%.0f");
        ImGui::SliderFloat("Distance fade end (m)", &params.distance_fade_end_m, 0.0f, 100000.0f, "%.0f");
        ImGui::SetItemTooltip("Labels fade out smoothly between start and end distance, and are dropped entirely beyond end.");

        if (ImGui::SliderFloat("Terrain fade range (m)", &params.terrain_fade_range_m, 1.0f, 300.0f, "%.0f")) {
            gpu_dirty = true;
        }
        ImGui::SetItemTooltip("How far behind the terrain a label must be before fully disappearing, instead of a hard cutoff.");

        if (ImGui::SliderFloat("Terrain fade bias (m)", &params.terrain_bias_m, 0.0f, 20.0f, "%.1f")) {
            gpu_dirty = true;
        }
        ImGui::SetItemTooltip("Tolerance so labels resting right at the surface don't flicker due to depth precision.");

        ImGui::SeparatorText("Appearance");

        if (ImGui::SliderFloat("MSDF px range", &params.px_range, 1.0f, 16.0f, "%.1f")) {
            gpu_dirty = true;
        }
        ImGui::SetItemTooltip("Must match the distanceRange used when generating the MSDF atlas. Wrong values cause blurry or jagged edges.");
        //label_color is applied per - instance on CPU each frame in build_glyph_instances,
        //no GPU buffer re - upload needed — takes effect next update() automatically.
        ImGui::ColorEdit4("Fill color", &params.label_color.x);
        
        if (ImGui::SliderFloat("Outline width (px)", &params.outline_width_px, 0.0f, 6.0f, "%.2f")) {
            gpu_dirty = true;
        }

        if (ImGui::ColorEdit4("Outline color", &params.outline_color.x)) {
            gpu_dirty = true;
        }

        ImGui::SeparatorText("Temporal coherence");
        ImGui::Checkbox("Enable hysteresis", &params.temporal_coherence_enabled);
        if (params.temporal_coherence_enabled) {
            ImGui::SliderFloat("Temporal bias", &params.temporal_bias, 0.0f, 20000.0f, "%.0f");
        }

        ImGui::Separator();
        if (ImGui::Button("Reset to defaults")) {
            params = webgpu_engine::LabelRenderer::LabelParameters {};
            gpu_dirty = true;
        }

        
    }

    if (gpu_dirty) {
        m_label_renderer->update_gpu_params();
    }
}

} // namespace webgpu_app
