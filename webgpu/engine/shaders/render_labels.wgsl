///use util/camera_config

@group(0) @binding(0) var<uniform> camera: camera_config;

struct LabelParams 
{
    px_range             : f32,
    outline_width_px     : f32,
    terrain_fade_range_m : f32,
    terrain_bias_m       : f32,
    outline_color        : vec4f,
}

struct GlyphInstance
 {
    screen_pos : vec2f,
    size       : vec2f,
    uv_min     : vec2f,
    uv_max     : vec2f,
    color      : vec4f,
    ndc_z      : f32,
    view_dist  : f32,
    _pad1      : f32,
    _pad2      : f32,
}

@group(1) @binding(0) var<storage, read> glyphs : array<GlyphInstance>;
@group(1) @binding(1) var msdf_atlas             : texture_2d<f32>;
@group(1) @binding(2) var msdf_sampler           : sampler;
@group(1) @binding(3) var<uniform> params        : LabelParams;
@group(2) @binding(0) var depth_texture          : texture_2d<f32>;

struct VertexOut {
    @builtin(position) position : vec4f,
    @location(0)       uv      : vec2f,
    @location(1)       color   : vec4f,
    @location(2)       ndc_z   : f32,
	@location(3)       view_dist : f32,
}

@vertex
fn vertexMain(@builtin(vertex_index) vid: u32) -> VertexOut {
    let glyph_idx  = vid / 6u;
    let corner_idx = vid % 6u;
    let g          = glyphs[glyph_idx];

    var corners = array<vec2f, 6>(
        vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
        vec2f(1.0, 0.0), vec2f(1.0, 1.0), vec2f(0.0, 1.0),
    );
    let corner = corners[corner_idx];
    let px     = g.screen_pos + corner * g.size;
    let ndc    = (px / camera.viewport_size) * 2.0 - vec2f(1.0);

    var out: VertexOut;
    out.position = vec4f(ndc.x, -ndc.y, g.ndc_z, 1.0);
    out.uv       = mix(g.uv_min, g.uv_max, corner);
    out.color    = g.color;
    out.ndc_z    = g.ndc_z;
	out.view_dist = g.view_dist;
    return out;
}

fn sample_msdf(texcoord: vec2f) -> f32 {
    let c = textureSample(msdf_atlas, msdf_sampler, texcoord);
    return max(min(c.r, c.g), min(max(c.r, c.g), c.b));
}

struct FragOut { @location(0) color: vec4f }

@fragment
fn fragmentMain(in: VertexOut) -> FragOut {
    
	//Terrain depth occlusion
    let terrain_d = textureLoad(depth_texture, vec2i(in.position.xy), 0).r;

    //let ndc_xy = (in.position.xy / camera.viewport_size) * 2.0 - vec2f(1.0);
    //let recon_ndc = vec3f(ndc_xy.x, ndc_xy.y, 1-terrain_d);

    //let local4    = camera.inv_view_proj_matrix * vec4f(recon_ndc, 1.0);
    //let local_pos = local4.xyz / local4.w;
    //let terrain_dist = -length(local_pos);

    //let occlusion_diff = terrain_dist - in.view_dist; // positive => terrain is closer = occluding
    //let eased_diff   = max(occlusion_diff - params.terrain_bias_m, 0.0);
    //let terrain_alpha = 1.0 - smoothstep(0.0, max(params.terrain_fade_range_m, 0.001), eased_diff);
	let terrain_alpha = 1.0;
	
	//fade is nice, but kill the label when its behind a mountian regardless
	let bias = in.ndc_z * 0.002;
    if (in.ndc_z < terrain_d - bias) {
        discard;
    }
	
    //MSDF antialiasing
	//as per the webgpu example: https://webgpu.github.io/webgpu-samples/?sample=textRenderingMsdf#msdfText.wgsl
    let sz = vec2f(textureDimensions(msdf_atlas, 0));
	let dx = sz.x * length(vec2f(dpdx(in.uv.x), dpdy(in.uv.x)));
	let dy = sz.y * length(vec2f(dpdx(in.uv.y), dpdy(in.uv.y)));

	let denom = max(dx * dx + dy * dy, 1e-5);
	let toPixels = params.px_range * inverseSqrt(denom);

    let sigDist = sample_msdf(in.uv) - 0.5;
    let pxDist  = sigDist * toPixels;

    let fill_alpha    = smoothstep(-0.5, 0.5, pxDist);
    let outline_alpha = smoothstep(-0.5, 0.5, pxDist + params.outline_width_px);

    let rgb   = mix(params.outline_color.rgb, in.color.rgb, fill_alpha);
    let alpha = outline_alpha * in.color.a * terrain_alpha;

    if (alpha < 0.001) {
        discard;
    }

    var out: FragOut;
    out.color = vec4f(rgb * alpha, alpha);
    return out;
}