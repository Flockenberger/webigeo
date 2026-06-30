#pragma once

#include "ui/ImGuiPanel.h"

namespace webgpu_engine {
class LabelRenderer;
class Context;
} // namespace webgpu_engine

namespace webgpu_app {

class LabelPanel : public ImGuiPanel {
public:
    LabelPanel(webgpu_engine::Context* context, webgpu_engine::LabelRenderer* label_renderer);

    void draw() override;
    void draw_panel() override;

private:
    webgpu_engine::Context* m_context;
    webgpu_engine::LabelRenderer* m_label_renderer;
};

} // namespace webgpu_app