#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QObject>
#include <glm/glm.hpp>

#include "nucleus/DataQuerier.h"

#include <webgpu/base/Buffer.h>
#include <webgpu/base/Context.h>
#include <webgpu/base/raii/BindGroup.h>
#include <webgpu/base/raii/BindGroupLayout.h>
#include <webgpu/base/raii/Pipeline.h>
#include <webgpu/base/raii/RawBuffer.h>
#include <webgpu/base/raii/Sampler.h>
#include <webgpu/base/raii/Texture.h>
#include <webgpu/base/raii/TextureView.h>
#include <webgpu/webgpu.h>

#include "UniformBufferObjects.h"

//This initial implementation is, for now just CPU side and without tiles but is the "fallback" discussed
//with Adam on 2026.06.12

namespace webgpu_engine {

// TODO: I should probably not just declare all these structs in the greater webgpu_engine context...
// either move into own sub-namespace or inside class I guess

struct LabelScreenRect {
    glm::vec2 min, max;
};

// This simple char lookup approach didnt work because we have
//"umlaute" which means they are not simply chars but wide chars (2byte)
//so for the glyph lookup we need the actual codepoint instead
struct LabelData {
    std::string text;
    std::vector<uint32_t> codepoints;
    double lat = 0.0;
    double lon = 0.0;
    double csv_alt = 0.0; // raw altitude from the csv (will pro
    glm::dvec3 world_pos_base; // ECEF position at csv_alt (no offset applied)
    glm::dvec3 up;
};

struct GlyphMetrics {
    glm::vec2 uv_min;
    glm::vec2 uv_max;
    glm::vec2 offset;   // pixel offset from pen origin
    glm::vec2 size;     // glyph size in px at base size
    float advance;      // horizontal advance in px at base size
};

struct alignas(16) GlyphInstance {
                                // Writing down the offsets because man that has cost me time before
    glm::vec2 screen_pos;       // offset  0, 8 bytes
    glm::vec2 size;             // offset  8, 8 bytes
    glm::vec2 uv_min;           // offset 16, 8 bytes
    glm::vec2 uv_max;           // offset 24, 8 bytes
    glm::vec4 color;            // offset 32, 16 bytes
    float ndc_z;                // offset 48, 4 bytes
    float view_dist = 0;        // offset 52
    float _pad1 = 0;            // offset 56
    float _pad2 = 0;            // offset 60
};

struct alignas(16) LabelParams {
    float px_range = 4.0f;
    float outline_width_px = 1.25f;
    float terrain_fade_range_m = 50.0f;
    float terrain_bias_m = 2.0f;
    glm::vec4 outline_color = { 0.0f, 0.0f, 0.0f, 1.0f };
};

// This will later become useful (hopefully)
// to be able to switch to the different modes for comparison in performance etc
enum LabelRenderMode : uint8_t { NAIVE_CPU = 0, NAIVE_GPU /*... more to be defined later when I get to it*/ };

enum LabelType : uint8_t { PEAK = 0, PLACE, /* I guess more to be defined*/};

class LabelRenderer : public QObject {
    Q_OBJECT

public:
  
    struct LabelParameters {

        float label_scale = 0.6f;
        float px_range = 4.0f;
        //This one is a bit tricky to get right (since the its the up on the earth so not screen up)
        //I'll use the label_vertical_offset_px for that one instead, but need to keep that in mind
        float anchor_height_offset_m = 0.0f;
        float label_vertical_offset_px = 40.0f;
        float label_padding_px = 16.0f;
        float occlusion_drop_threshold = 0.15f;
        float outline_width_px = 0.5f;
        float min_label_width_px = 18.0f;
        
        //(very) naive temporal coherence
        bool temporal_coherence_enabled = false;
        float temporal_bias = 2000.0f;

        float distance_fade_start_m = 15000.0f;
        float distance_fade_end_m = 30000.0f;

        float fade_duration_s = 0.35f;
        float terrain_fade_range_m = 50.0f; // distance over which a hidden label fades out
        float terrain_bias_m = 2.0f;

        glm::vec4 outline_color = { 0.0f, 0.0f, 0.0f, 1.0f };
        glm::vec4 label_color = { 1.0f, 0.0f, 0.0f, 1.0f };
        LabelRenderMode render_mode = NAIVE_CPU;
    };

private:
    webgpu::Context* m_ctx = nullptr;

    std::unique_ptr<webgpu::raii::GenericRenderPipeline> m_pipeline;
    std::unique_ptr<webgpu::raii::BindGroupLayout> m_label_bgl;

    // Atlas resources
    std::unique_ptr<webgpu::raii::Texture> m_atlas_texture;
    std::unique_ptr<webgpu::raii::TextureView> m_atlas_view;
    std::unique_ptr<webgpu::raii::Sampler> m_atlas_sampler;
    bool m_atlas_loaded = false;

    // Per-frame glyph instance storage buffer
    std::unique_ptr<webgpu::raii::RawBuffer<GlyphInstance>> m_instance_buffer;
    size_t m_instance_buffer_capacity = 0;

    // Bind group for group(1): instance buffer + atlas + sampler
    std::unique_ptr<webgpu::raii::BindGroup> m_label_bind_group;

    std::vector<LabelData> m_labels;
    std::unordered_map<uint32_t, GlyphMetrics> m_glyph_map;
    std::vector<GlyphInstance> m_cpu_instances;

    std::unique_ptr<webgpu::raii::RawBuffer<LabelParams>> m_params_buffer;

    LabelParameters m_label_parameters;

    std::shared_ptr<nucleus::DataQuerier> m_dataquerier;

    //for cpu temporal coherence
    std::unordered_set<const LabelData*> m_visible_last_frame;
   
    // persistent, current eased alpha per label
    std::unordered_map<const LabelData*, float> m_label_alpha; 
    std::chrono::steady_clock::time_point m_last_update_time {};
    bool m_has_last_update_time = false;

    

public:
    // Just here for debug/initial testing now
    // this will eventually use the nucleus label things which
    // are already there but need updating to work with msdf -> https://github.com/Chlumsky/msdfgen
    void load_csv(const std::filesystem::path& path);
    void load_atlas(const std::filesystem::path& atlas_png);

public:
    explicit LabelRenderer();

    void init(webgpu::Context& ctx);

    void update(const uboCameraConfig& camera_ubo);

    void draw(const WGPUCommandEncoder& command_encoder,
        const WGPUBindGroup& camera_bind_group,
        WGPUTextureView target_color_view,
        const WGPUBindGroup& depth_bind_group);

    LabelParameters& get_parameter() { return m_label_parameters; }

    // Method to update the parameters in case they were changed in the label panel
    void update_gpu_params();

    void set_dataquerier(std::shared_ptr<nucleus::DataQuerier> data_querier);


private:
    
    
    void build_glyph_instances_naive_cpu(const uboCameraConfig& cam, float dt);

    void ensure_instance_buffer(size_t required);
    void rebuild_label_bind_group();
    void parse_atlas_json(const std::filesystem::path& json_path, uint32_t atlas_w, uint32_t atlas_h);
    
    inline float move_towards(float current, float target, float max_delta)
    {
        if (std::abs(target - current) <= max_delta)
            return target;
        return current + (target > current ? max_delta : -max_delta);
    }

    // I didnt see a proper utf8 decode method in utils (maybe I overlooked it)
    // and I didnt weant to use imgui in here where it doesnt belong for a utility method
    inline std::vector<uint32_t> utf8_decode(const std::string& s)
    {
        std::vector<uint32_t> out;
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i];
            uint32_t cp = 0;
            int extra = 0;
            if (c < 0x80) {
                cp = c;
                extra = 0;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                extra = 2;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                extra = 3;
            } else {
                i++;
                continue;
            } // invalid leading byte — skip

            if (i + extra >= s.size())
                break;

            bool valid = true;
            for (int k = 1; k <= extra; ++k) {
                unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!valid) {
                i++;
                continue;
            }

            out.push_back(cp);
            i += extra + 1;
        }
        return out;
    }
};

} // namespace webgpu_engine