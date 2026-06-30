#include "LabelRenderer.h"

#include "nucleus/srs.h"

#include <nucleus/utils/image_loader.h>
#include <webgpu/base/RenderResourceRegistry.h>
#include <webgpu/base/raii/PipelineLayout.h>
#include <webgpu/base/raii/RenderPassEncoder.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace webgpu_engine {

LabelRenderer::LabelRenderer()
    : QObject { nullptr }
{
}

void LabelRenderer::set_dataquerier(std::shared_ptr<nucleus::DataQuerier> data_querier) { m_dataquerier = data_querier; }

void LabelRenderer::init(webgpu::Context& ctx)
{
    m_ctx = &ctx;
    qDebug() << "[LabelRenderer] init...";

    auto& reg = ctx.resource_registry();
    reg.register_shader("render_labels", "webgpu_engine::render_labels");

    reg.register_pipeline([this](WGPUDevice dev, const webgpu::RenderResourceRegistry& reg) {
        WGPUBindGroupLayoutEntry instances {};
        instances.binding = 0;
        instances.visibility = WGPUShaderStage_Vertex;
        instances.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        instances.buffer.minBindingSize = 0;

        WGPUBindGroupLayoutEntry atlas_tex {};
        atlas_tex.binding = 1;
        atlas_tex.visibility = WGPUShaderStage_Fragment;
        atlas_tex.texture.sampleType = WGPUTextureSampleType_Float;
        atlas_tex.texture.viewDimension = WGPUTextureViewDimension_2D;

        WGPUBindGroupLayoutEntry samp {};
        samp.binding = 2;
        samp.visibility = WGPUShaderStage_Fragment;
        samp.sampler.type = WGPUSamplerBindingType_Filtering;

        WGPUBindGroupLayoutEntry params_entry {};
        params_entry.binding = 3;
        params_entry.visibility = WGPUShaderStage_Fragment;
        params_entry.buffer.type = WGPUBufferBindingType_Uniform;
        params_entry.buffer.minBindingSize = sizeof(LabelParams);
        ;

        m_label_bgl = std::make_unique<webgpu::raii::BindGroupLayout>(
            dev, std::vector<WGPUBindGroupLayoutEntry> { instances, atlas_tex, samp, params_entry }, "label_renderer bgl");
        WGPUBlendState blend {};
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_One;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_Zero;
        blend.alpha.dstFactor = WGPUBlendFactor_One;

        webgpu::FramebufferFormat format {};
        format.depth_format = WGPUTextureFormat_Undefined; // no depth attachment needed;
        format.color_formats.emplace_back(WGPUTextureFormat_BGRA8Unorm); // we test manually via textureLoad

        m_pipeline = std::make_unique<webgpu::raii::GenericRenderPipeline>(dev,
            reg.shader("render_labels"),
            reg.shader("render_labels"),
            std::vector<webgpu::util::SingleVertexBufferInfo> {},
            format,
            std::vector<const webgpu::raii::BindGroupLayout*> {
                &reg.bind_group_layout("camera"),
                m_label_bgl.get(),
                &reg.bind_group_layout("depth_texture"),
            },
            std::vector<std::optional<WGPUBlendState>> { blend },
            WGPUPrimitiveTopology_TriangleList);
    });

    // TODO: Just for testing!

    load_atlas("E:\\_2_uni\\Master\\Masterarbeit\\Dev\\webigeo_dev\\assets_dev\\Roboto-Regular.png");
    load_csv("E:\\_2_uni\\Master\\Masterarbeit\\Dev\\webigeo_dev\\assets_dev\\peaks.csv");
}

// This will be replaced in the future as Adam wanted it to use the library
// for the msdf font
void LabelRenderer::load_atlas(const std::filesystem::path& atlas_png)
{
    qDebug() << "Loading Atlast from: " << atlas_png.string();
    auto raster_result = nucleus::utils::image_loader::rgba8(QString::fromStdString(atlas_png.string()));

    if (!raster_result) {
        throw std::runtime_error(raster_result.error().toStdString());
    }

    auto& raster = *raster_result;

    WGPUDevice dev = m_ctx->device();
    WGPUQueue q = m_ctx->queue();

    WGPUTextureDescriptor tex_desc {};
    tex_desc.label = WGPUStringView { .data = "msdf_atlas", .length = WGPU_STRLEN };
    tex_desc.dimension = WGPUTextureDimension_2D;
    tex_desc.size = { static_cast<uint32_t>(raster.width()), static_cast<uint32_t>(raster.height()), 1 };
    tex_desc.format = WGPUTextureFormat_RGBA8Unorm;
    tex_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    tex_desc.mipLevelCount = 1;
    tex_desc.sampleCount = 1;

    m_atlas_texture = std::make_unique<webgpu::raii::Texture>(dev, tex_desc);
    m_atlas_texture->write(q, raster);
    m_atlas_view = m_atlas_texture->create_view();

    WGPUSamplerDescriptor samp_desc {};
    samp_desc.label = WGPUStringView { .data = "msdf_sampler", .length = WGPU_STRLEN };
    samp_desc.addressModeU = WGPUAddressMode_ClampToEdge;
    samp_desc.addressModeV = WGPUAddressMode_ClampToEdge;
    samp_desc.magFilter = WGPUFilterMode_Linear;
    samp_desc.minFilter = WGPUFilterMode_Linear;
    samp_desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    samp_desc.maxAnisotropy = 1;

    m_atlas_sampler = std::make_unique<webgpu::raii::Sampler>(dev, samp_desc);

    auto json_path = atlas_png;
    json_path.replace_extension(".json");

    parse_atlas_json(json_path, static_cast<uint32_t>(raster.width()), static_cast<uint32_t>(raster.height()));

    LabelParams params {};
    params.px_range = m_label_parameters.px_range;
    params.outline_width_px = m_label_parameters.outline_width_px;
    params.outline_color = m_label_parameters.outline_color;
    m_params_buffer
        = std::make_unique<webgpu::raii::RawBuffer<LabelParams>>(m_ctx->device(), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, 1, "label_params");

    update_gpu_params();

    m_atlas_loaded = true;
}

void LabelRenderer::parse_atlas_json(const std::filesystem::path& json_path, uint32_t atlas_w, uint32_t atlas_h)
{
    m_glyph_map.clear();

    QFile f(QString::fromStdString(json_path.string()));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot open atlas json");

    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    for (const auto& v : root["chars"].toArray()) {
        QJsonObject c = v.toObject();

        GlyphMetrics gm {};

        uint32_t id = c["id"].toInt();

        float x = c["x"].toDouble();
        float y = c["y"].toDouble();
        float w = c["width"].toDouble();
        float h = c["height"].toDouble();

        gm.advance = c["xadvance"].toDouble();
        gm.size = { w, h };
        gm.offset = { (float)c["xoffset"].toDouble(), (float)c["yoffset"].toDouble() };
        gm.uv_min = { x / atlas_w, y / atlas_h };
        gm.uv_max = { (x + w) / atlas_w, (y + h) / atlas_h };

        m_glyph_map[id] = gm;
    }

    if (root.contains("distanceField")) {
        m_label_parameters.px_range = (float)root["distanceField"].toObject()["distanceRange"].toDouble(4.0);
    } else if (root.contains("atlas")) {
        m_label_parameters.px_range = (float)root["atlas"].toObject()["distanceRange"].toDouble(4.0);
    } else {
        m_label_parameters.px_range = 4.0f; // default from msdfgen
    }
    qDebug() << "[LabelRenderer] pxRange:" << m_label_parameters.px_range;
}

void LabelRenderer::load_csv(const std::filesystem::path& path)
{
    m_labels.clear();
    m_visible_last_frame.clear();
    // This will eventually be removed anyway, just for testing now...
    // My csv wasnt really the cleanest
    {
        std::ifstream file(path);
        if (!file.is_open())
            throw std::runtime_error("cannot open CSV: " + path.string());

        constexpr char SEP = ';';

        auto fix = [](std::string& s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t' || s.front() == ';'))
                s.erase(s.begin());

            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
                s.pop_back();

            std::replace(s.begin(), s.end(), ',', '.');
        };

        std::string line;

        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#')
                continue;

            // skip header
            if (line.find("lat") != std::string::npos && line.find("lon") != std::string::npos)
                continue;

            size_t p1 = line.find(SEP);
            size_t p2 = line.find(SEP, p1 + 1);
            size_t p3 = line.find(SEP, p2 + 1);

            if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos)
                continue;

            std::string name = line.substr(0, p1);
            std::string lat_s = line.substr(p1 + 1, p2 - p1 - 1);
            std::string lon_s = line.substr(p2 + 1, p3 - p2 - 1);
            std::string alt_s = line.substr(p3 + 1);

            fix(lat_s);
            fix(lon_s);
            fix(alt_s);

            if (alt_s.empty())
                continue;

            double lat, lon, alt;

            try {
                lat = std::stof(lat_s);
                lon = std::stof(lon_s);
                alt = std::stof(alt_s);
            } catch (...) {
                continue;
            }

            glm::dvec3 world_base = nucleus::srs::lat_long_alt_to_world({ lat, lon, alt });
            glm::dvec3 up = glm::normalize(world_base);

            // if (m_dataquerier) {
            // const auto queried = m_dataquerier->get_altitude({ lat, lon });
            // if (queried.has_value())
            //     terrain_alt = queried.value();
            // }
            m_labels.push_back({ name, utf8_decode(name), lat, lon, alt, world_base, up });
        }
    }
    qDebug() << "[LabelRenderer] - Loaded :" << m_labels.size() << " lables";
}

void LabelRenderer::update(const uboCameraConfig& cam)
{
    // if (!m_atlas_loaded || m_labels.empty() || !m_pipeline) {
    //     qDebug() << "[LabelRenderer] - Update early return!";
    //     return;
    // }
    if (!m_atlas_loaded) {
        qDebug() << "[LabelRenderer] - update early return: atlas not loaded";
        return;
    }

    if (m_labels.empty()) {
        qDebug() << "[LabelRenderer] - update early return: no labels";
        return;
    }

    if (!m_pipeline) {
        qDebug() << "[LabelRenderer] - update early return: no pipeline";
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f; // sane default for the very first frame
    if (m_has_last_update_time) {
        dt = std::chrono::duration<float>(now - m_last_update_time).count();
        dt = std::clamp(dt, 0.0f, 0.1f); // guard against huge jumps after a pause/breakpoint
    }
    m_last_update_time = now;
    m_has_last_update_time = true;

    switch (m_label_parameters.render_mode) {
    case NAIVE_CPU:
        build_glyph_instances_naive_cpu(cam, dt);
        break;
    case NAIVE_GPU:
        break;
    default:
        build_glyph_instances_naive_cpu(cam, dt);
        break;
    }

    if (m_cpu_instances.empty())
        return;
    ensure_instance_buffer(m_cpu_instances.size());
    wgpuQueueWriteBuffer(m_ctx->queue(), m_instance_buffer->handle(), 0, m_cpu_instances.data(), m_cpu_instances.size() * sizeof(GlyphInstance));
}

void LabelRenderer::update_gpu_params()
{
    // atlas not loaded yet — nothing to update
    if (!m_params_buffer) {
        return;
    }
    LabelParams gpu_params {};
    gpu_params.px_range = m_label_parameters.px_range;
    gpu_params.outline_width_px = m_label_parameters.outline_width_px;
    gpu_params.terrain_fade_range_m = m_label_parameters.terrain_fade_range_m;
    gpu_params.terrain_bias_m = m_label_parameters.terrain_bias_m;
    gpu_params.outline_color = m_label_parameters.outline_color;

    wgpuQueueWriteBuffer(m_ctx->queue(), m_params_buffer->handle(), 0, &gpu_params, sizeof(gpu_params));
}

void LabelRenderer::build_glyph_instances_naive_cpu(const uboCameraConfig& cam, float dt)
{
    m_cpu_instances.clear();

    const glm::mat4 vp = cam.view_proj_matrix;
    const glm::vec2 vp_size = cam.viewport_size;

    // TODO: move this out
    struct ProjectedLabel {
        const LabelData* data;
        glm::vec2 anchor;
        float depth; // raw distance, used for actual occlusion math
        float sort_depth; // depth with temporal bias applied, used only for sort order
        float ndc_z;
        float total_w;
        float max_h;
        float distance_fade;
    };

    std::vector<ProjectedLabel> projected;
    projected.reserve(m_labels.size());

    // First pass here we calculate the projected label positions
    for (const auto& label : m_labels) {
        glm::dvec3 lifted_world = label.world_pos_base + label.up * (double)m_label_parameters.anchor_height_offset_m;
        glm::dvec3 local_pos_d = lifted_world - glm::dvec3(cam.position);
        glm::vec4 clip = vp * glm::vec4(glm::vec3(local_pos_d), 1.0f);

        if (clip.w <= 0.0f)
            continue;

        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.1f || ndc.x > 1.1f || ndc.y < -1.1f || ndc.y > 1.1f)
            continue;

        glm::vec2 anchor { (ndc.x * 0.5f + 0.5f) * vp_size.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * vp_size.y };

        float total_w = 0.0f, max_h = 0.0f;
        for (uint32_t cp : label.codepoints) {
            auto it = m_glyph_map.find(cp);
            if (it == m_glyph_map.end()) {
                total_w += 8.0f * m_label_parameters.label_scale;
                continue;
            }
            total_w += it->second.advance * m_label_parameters.label_scale;
            max_h = std::max(max_h, it->second.size.y * m_label_parameters.label_scale);
        }

        if (total_w <= 0.0f || max_h <= 0.0f)
            continue;

        if (total_w < m_label_parameters.min_label_width_px)
            continue;

        float depth = clip.w;
        float sort_depth = depth;

        if (depth >= m_label_parameters.distance_fade_end_m)
            continue;

        float distance_fade = 1.0f;
        if (depth > m_label_parameters.distance_fade_start_m) {
            float t = (depth - m_label_parameters.distance_fade_start_m)
                / std::max(1.0f, m_label_parameters.distance_fade_end_m - m_label_parameters.distance_fade_start_m);
            distance_fade = 1.0f - glm::clamp(t, 0.0f, 1.0f);
        }

        // Labels that were visible last frame get a "discount" on sort distance,
        // so a marginally-closer newcomer needs to beat them by more than the
        // bias before it's allowed to take over the same screen-space slot.
        if (m_label_parameters.temporal_coherence_enabled && m_visible_last_frame.count(&label)) {
            sort_depth -= m_label_parameters.temporal_bias;
        }

        projected.push_back({ &label, anchor, depth, sort_depth, clip.z / clip.w, total_w, max_h, distance_fade });
    }

    std::sort(projected.begin(), projected.end(), [](const ProjectedLabel& a, const ProjectedLabel& b) { return a.sort_depth < b.sort_depth; });

    std::vector<LabelScreenRect> placed_rects;
    placed_rects.reserve(projected.size());

    std::unordered_set<const LabelData*> visible_this_frame;

    std::unordered_map<const LabelData*, float> target_alpha;
    target_alpha.reserve(projected.size());

    for (const auto& proj : projected)
        target_alpha[proj.data] = 0.0f;

    for (const auto& proj : projected) {
        const auto& label = *proj.data;

        // Measure the label's screen-space bounding box
        // float total_w = 0.0f;
        // float max_h = 0.0f;
        //
        // for (uint32_t cp : label.codepoints) {
        //    auto it = m_glyph_map.find(cp);
        //    if (it == m_glyph_map.end()) {
        //        total_w += 8.0f * m_label_parameters.label_scale;
        //        continue;
        //    }
        //
        //    total_w += it->second.advance * m_label_parameters.label_scale;
        //    max_h = std::max(max_h, it->second.size.y * m_label_parameters.label_scale);
        //
        //    // if (it->second.size.x < 0.5f || it->second.size.y < 0.5f) {
        //    //     total_w += it->second.advance * m_label_scale;
        //    //     continue;
        //    // }
        //}
        // if (total_w <= 0.0f || max_h <= 0.0f)
        //    continue;
        //
        // if (total_w < m_label_parameters.min_label_width_px)
        //    continue;

        // if (total_w <= 0.0f || max_h <= 0.0f)
        //     continue;
        //
        // float pen_x = proj.anchor.x - total_w * 0.5f;
        // float pen_y = proj.anchor.y;
        //
        // LabelScreenRect rect { .min = { pen_x, pen_y - max_h }, .max = { pen_x + total_w, pen_y } };
        // float label_area = total_w * max_h;
        //
        //// Accumulate intersection area with all already-placed labels
        // float occluded_area = 0.0f;
        // for (const auto& placed : placed_rects) {
        //     float ix = std::max(0.0f, std::min(rect.max.x, placed.max.x) - std::max(rect.min.x, placed.min.x));
        //     float iy = std::max(0.0f, std::min(rect.max.y, placed.max.y) - std::max(rect.min.y, placed.min.y));
        //     occluded_area += ix * iy;
        // }
        //
        // float occlusion = std::min(1.0f, occluded_area / label_area);
        //
        //// Fully occluded: skip entirely (don't even register the rect)
        // if (occlusion >= 0.6f)
        //     continue;
        //
        // float alpha = m_label_color.w * (1.0f - occlusion);

        float pen_x = proj.anchor.x - proj.total_w * 0.5f;
        float pen_y = proj.anchor.y;

        LabelScreenRect rect { .min = { pen_x - m_label_parameters.label_padding_px, pen_y - proj.max_h - m_label_parameters.label_padding_px },
            .max = { pen_x + proj.total_w + m_label_parameters.label_padding_px, pen_y + m_label_parameters.label_padding_px } };

        float label_area = (rect.max.x - rect.min.x) * (rect.max.y - rect.min.y);

        float occluded_area = 0.0f;

        for (const auto& placed : placed_rects) {
            float ix = std::max(0.0f, std::min(rect.max.x, placed.max.x) - std::max(rect.min.x, placed.min.x));
            float iy = std::max(0.0f, std::min(rect.max.y, placed.max.y) - std::max(rect.min.y, placed.min.y));

            occluded_area += ix * iy;
        }
        float occlusion = std::min(1.0f, occluded_area / label_area);
        if (occlusion >= m_label_parameters.occlusion_drop_threshold)
            continue; // dropped — no terrain query wasted

        placed_rects.push_back(rect);
        visible_this_frame.insert(&label);
        target_alpha[&label] = proj.distance_fade;
    }

    float fade_rate = (m_label_parameters.fade_duration_s > 0.001f) ? (1.0f / m_label_parameters.fade_duration_s) : 1000.0f;
    float max_delta = fade_rate * dt;
    for (const auto& proj : projected) {
        const auto& label = *proj.data;
        float current_alpha = 0.0f;
        auto alpha_it = m_label_alpha.find(&label);
        if (alpha_it != m_label_alpha.end())
            current_alpha = alpha_it->second;

        float target = target_alpha[&label];
        current_alpha = move_towards(current_alpha, target, max_delta);

        if (current_alpha <= 0.001f && target <= 0.001f) {
            m_label_alpha.erase(&label); // fully faded out — drop tracking
            continue; // nothing to draw this frame
        }
        m_label_alpha[&label] = current_alpha;

        float pen_x = proj.anchor.x - proj.total_w * 0.5f;
        float pen_y = proj.anchor.y;

        // Terrain query only for labels actually being drawn this frame
        double terrain_alt = label.csv_alt;
        if (m_dataquerier) {
            const auto queried = m_dataquerier->get_altitude({ label.lat, label.lon });
            if (queried.has_value())
                terrain_alt = queried.value();
        }
        double extra_lift = std::max(0.0, terrain_alt - label.csv_alt);

        glm::dvec3 lifted_world = label.world_pos_base; 
        /*+label.up*(extra_lift + (double)m_label_parameters.anchor_height_offset_m);*/
        lifted_world.y += extra_lift + (double)m_label_parameters.anchor_height_offset_m;
        glm::dvec3 local_pos_d = lifted_world - glm::dvec3(cam.position);
        glm::vec4 clip2 = vp * glm::vec4(glm::vec3(local_pos_d), 1.0f);
        float ndc_z = (clip2.w > 0.0f) ? clip2.z / clip2.w : proj.ndc_z;
        float view_dist = (float)glm::length(local_pos_d);

        float alpha = m_label_parameters.label_color.w * current_alpha;

        for (uint32_t cp : label.codepoints) {
            auto it = m_glyph_map.find(cp);
            if (it == m_glyph_map.end()) {
                pen_x += 8.0f * m_label_parameters.label_scale;
                continue;
            }
            const auto& gm = it->second;

            m_cpu_instances.push_back({
                .screen_pos = { pen_x + gm.offset.x * m_label_parameters.label_scale, pen_y + gm.offset.y * m_label_parameters.label_scale },
                .size = gm.size * m_label_parameters.label_scale,
                .uv_min = gm.uv_min,
                .uv_max = gm.uv_max,
                .color = { m_label_parameters.label_color.x, m_label_parameters.label_color.y, m_label_parameters.label_color.z, alpha },
                .ndc_z = ndc_z,
                .view_dist = view_dist,
            });
            pen_x += gm.advance * m_label_parameters.label_scale;
        }
    }

    m_visible_last_frame = std::move(visible_this_frame);
}

void LabelRenderer::ensure_instance_buffer(size_t required)
{
    if (required <= m_instance_buffer_capacity)
        return;

    size_t new_cap = std::max(required * 2, (size_t)64);

    m_instance_buffer = std::make_unique<webgpu::raii::RawBuffer<GlyphInstance>>(
        m_ctx->device(), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, new_cap, "label_renderer_instances");
    m_instance_buffer_capacity = new_cap;

    rebuild_label_bind_group();
}

void LabelRenderer::rebuild_label_bind_group()
{
    // if (!m_label_bgl || !m_atlas_view || !m_atlas_sampler || !m_instance_buffer) {
    //     return;
    // }
    // qDebug() << "[LabelRenderer] - REBUILDING LABEL BIND GROUP!";

    if (!m_label_bgl) {
        qDebug() << "[LabelRenderer] bind group layout missing";
        return;
    }

    if (!m_atlas_view) {
        qDebug() << "[LabelRenderer] atlas view missing";
        return;
    }

    if (!m_atlas_sampler) {
        qDebug() << "[LabelRenderer] atlas sampler missing";
        return;
    }

    if (!m_instance_buffer) {
        qDebug() << "[LabelRenderer] instance buffer missing";
        return;
    }
    m_label_bind_group = std::make_unique<webgpu::raii::BindGroup>(m_ctx->device(),
        *m_label_bgl,
        std::initializer_list<WGPUBindGroupEntry> {
            m_instance_buffer->create_bind_group_entry(0),
            WGPUBindGroupEntry { .binding = 1, .textureView = m_atlas_view->handle() },
            WGPUBindGroupEntry { .binding = 2, .sampler = m_atlas_sampler->handle() },
            m_params_buffer->create_bind_group_entry(3),
        });
}

void LabelRenderer::draw(
    const WGPUCommandEncoder& encoder, const WGPUBindGroup& camera_bind_group, WGPUTextureView target_color_view, const WGPUBindGroup& depth_bind_group)
{
    // if (!m_pipeline || !m_label_bind_group || m_cpu_instances.empty()) {
    //     qDebug() << "[LabelRenderer] - draw early return!";
    //     return;
    // }
    if (!m_pipeline) {
        qDebug() << "[LabelRenderer] - draw early return: no pipeline";
        return;
    }

    if (!m_label_bind_group) {
        qDebug() << "[LabelRenderer] - draw early return: no label bind group";
        return;
    }

    if (m_cpu_instances.empty()) {
        qDebug() << "[LabelRenderer] - draw early return: no instances";
        return;
    }

    WGPURenderPassColorAttachment color_att {};
    color_att.view = target_color_view;
    color_att.loadOp = WGPULoadOp_Load;
    color_att.storeOp = WGPUStoreOp_Store;
    color_att.clearValue = { 0, 0, 0, 0 };
    color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDescriptor pass_desc {};
    pass_desc.label = WGPUStringView { .data = "label render pass", .length = WGPU_STRLEN };
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_att;

    auto pass = std::make_unique<webgpu::raii::RenderPassEncoder>(encoder, pass_desc);

    wgpuRenderPassEncoderSetPipeline(pass->handle(), m_pipeline->pipeline().handle());

    wgpuRenderPassEncoderSetBindGroup(pass->handle(), 0, camera_bind_group, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass->handle(), 1, m_label_bind_group->handle(), 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass->handle(), 2, depth_bind_group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass->handle(), (uint32_t)(m_cpu_instances.size() * 6), 1, 0, 0);
}
} // namespace webgpu_engine