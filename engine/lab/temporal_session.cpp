#include "temporal_session.h"
#include "reference_renderer.h"
#include "geometry.h"
#include <bit>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>

namespace emberframe::lab {
namespace {
struct Fingerprint {
    std::uint64_t value=14695981039346656037ull;
    void add(std::uint64_t x) {
        // 按字段编码，不散列结构体 padding 或 vector 指针，跨快照复制仍稳定。
        for (int byte=0;byte<8;++byte) { value^=(x>>(byte*8))&255u; value*=1099511628211ull; }
    }
    void add(int x) { add(std::uint64_t(std::uint32_t(x))); }
    void add(bool x) { add(std::uint64_t(x)); }
    void add(float x) { add(std::uint64_t(std::bit_cast<std::uint32_t>(x))); }
    void add(glm::vec2 v) { add(v.x); add(v.y); }
    void add(glm::vec3 v) { add(v.x); add(v.y); add(v.z); }
    void add(glm::vec4 v) { add(v.x); add(v.y); add(v.z); add(v.w); }
    void add(const glm::mat4& m) { for (int col=0;col<4;++col) add(m[col]); }
    void add(const std::string& s) { add(std::uint64_t(s.size())); for (unsigned char c:s) add(std::uint64_t(c)); }
};
std::uint64_t scene_fingerprint(const Scene& scene) {
    Fingerprint hash;
    hash.add(scene.revision); hash.add(scene.name); hash.add(scene.sky_top); hash.add(scene.sky_bottom);
    hash.add(std::uint64_t(scene.materials.size()));
    for (const auto& m:scene.materials) {
        hash.add(m.base_color); hash.add(m.emissive); hash.add(m.metallic); hash.add(m.roughness);
        hash.add(m.normal_scale); hash.add(m.ao_strength); hash.add(m.clearcoat);
        hash.add(m.clearcoat_roughness); hash.add(m.anisotropy); hash.add(m.sheen);
        hash.add(m.base_texture); hash.add(m.mr_texture); hash.add(m.normal_texture);
        hash.add(m.ao_texture); hash.add(m.emissive_texture); hash.add(m.alpha_mode);
        hash.add(m.alpha_cutoff); hash.add(m.double_sided);
    }
    hash.add(std::uint64_t(scene.meshes.size()));
    for (const auto& mesh:scene.meshes) {
        hash.add(std::uint64_t(mesh.vertices.size()));
        for (const auto& v:mesh.vertices) {
            hash.add(v.position); hash.add(v.normal); hash.add(v.uv); hash.add(v.tangent); hash.add(v.color);
        }
        hash.add(std::uint64_t(mesh.indices.size()));
        for (auto index:mesh.indices) hash.add(std::uint64_t(index));
        hash.add(std::uint64_t(mesh.primitives.size()));
        for (const auto& p:mesh.primitives) {
            hash.add(std::uint64_t(p.first_index)); hash.add(std::uint64_t(p.index_count)); hash.add(std::uint64_t(p.material));
        }
    }
    hash.add(std::uint64_t(scene.nodes.size()));
    for (const auto& n:scene.nodes) { hash.add(n.parent); hash.add(n.mesh); hash.add(n.local); }
    hash.add(std::uint64_t(scene.textures.size()));
    for (const auto& t:scene.textures) {
        hash.add(t.srgb); hash.add(std::uint64_t(t.levels.size()));
        for (const auto& level:t.levels) {
            hash.add(level.width); hash.add(level.height); hash.add(std::uint64_t(level.pixels.size()));
            for (const auto& p:level.pixels) hash.add(p);
        }
    }
    hash.add(std::uint64_t(scene.lights.size()));
    for (const auto& l:scene.lights) {
        hash.add(int(l.kind)); hash.add(l.position); hash.add(l.direction); hash.add(l.color);
        hash.add(l.intensity); hash.add(l.range); hash.add(l.size);
    }
    return hash.value;
}
std::uint64_t settings_fingerprint(const Settings& s) {
    Fingerprint hash;
    hash.add(int(s.path)); hash.add(int(s.shadows)); hash.add(int(s.gi)); hash.add(int(s.ao));
    hash.add(int(s.shading)); hash.add(int(s.culling)); hash.add(int(s.filter));
    hash.add(s.reversed_z); hash.add(s.energy_compensation);
    hash.add(s.taa); hash.add(s.denoise); hash.add(s.svgf); hash.add(s.temporal_weight);
    hash.add(s.ao_radius); hash.add(s.ao_strength); hash.add(s.shadow_bias); hash.add(s.light_size);
    hash.add(s.samples); hash.add(s.max_bounces); hash.add(s.shadow_resolution);
    hash.add(s.voxel_resolution); hash.add(s.propagation_steps); hash.add(s.render_width); hash.add(s.render_height);
    hash.add(std::uint64_t(s.seed));
    // 相机、debug、曝光与显示后处理都不改变线性历史的身份。
    return hash.value;
}
std::uint32_t frame_seed(std::uint32_t base,std::uint64_t frame) {
    if (frame==0) return base;
    // SplitMix64 把连续帧索引打散，避免每帧重复同一 Monte Carlo 噪声。
    std::uint64_t v=frame+std::uint64_t(base)+0x9e3779b97f4a7c15ull;
    v=(v^(v>>30))*0xbf58476d1ce4e5b9ull;
    v=(v^(v>>27))*0x94d049bb133111ebull;
    v^=v>>31;
    return std::uint32_t(v)^std::uint32_t(v>>32);
}
bool cancelled(const std::atomic<bool>* flag) { return flag && flag->load(std::memory_order_relaxed); }
glm::vec3 safe_color(glm::vec3 c) {
    for (int i=0;i<3;++i) if (!std::isfinite(c[i])) throw std::runtime_error("session: nonfinite output");
    return glm::max(c,glm::vec3(0));
}
template<class T> void require_extent(const Image<T>& im,int w,int h,const char* name) {
    if (im.width!=w || im.height!=h || im.pixels.size()!=std::size_t(w)*std::size_t(h))
        throw std::runtime_error(std::string("session: invalid renderer ")+name+" extent");
}
void display_debug(RenderOutput& out,const Scene& scene,const Camera& camera,const Settings& s) {
    if (s.debug==DebugView::final_color) return;
    for (std::size_t i=0;i<out.color.pixels.size();++i) {
        const auto& p=out.surfaces.pixels[i]; glm::vec3 c(0);
        switch (s.debug) {
        case DebugView::albedo: c=p.valid ? p.albedo:glm::vec3(0); break;
        case DebugView::normal: c=p.valid ? p.normal*0.5f+0.5f:glm::vec3(0); break;
        case DebugView::depth: c=glm::vec3(p.valid ? p.linear_depth/camera.far_plane:0); break;
        case DebugView::roughness: c=glm::vec3(p.valid ? p.roughness:0); break;
        case DebugView::metallic: c=glm::vec3(p.valid ? p.metallic:0); break;
        case DebugView::ao: c=glm::vec3(out.ao.pixels[i]); break;
        case DebugView::shadow: c=glm::vec3(out.shadow.pixels[i]); break;
        case DebugView::indirect: c=out.indirect.pixels[i]; break;
        case DebugView::motion: c=p.valid ? glm::vec3(p.motion*0.05f+glm::vec2(0.5f),0):glm::vec3(0); break;
        case DebugView::variance: {
            const float sigma=std::sqrt(std::max(0.0f,out.variance.pixels[i]));
            c=glm::vec3(sigma/(1+sigma)); break;
        }
        case DebugView::light_count: {
            // 此图显示表面的候选灯数量，不伪称 GPU tile/cluster 剔除统计。
            int count=0;
            if (p.valid) for (const auto& l:scene.lights)
                if (l.kind!=LightKind::point || glm::length(l.position-p.position)<=l.range) ++count;
            c=glm::vec3(float(count)/float(std::max(std::size_t(1),scene.lights.size()))); break;
        }
        default: c=out.color.pixels[i]; break;
        }
        out.color.pixels[i]=safe_color(c);
    }
}
} // namespace

void ReferenceSession::reset() {
    std::lock_guard lock(mutex_);
    temporal_.reset(); ready_=false; scene_key_=settings_key_=0;
    std::lock_guard info_lock(info_mutex_);
    const auto resets=info_.reset_count+1;
    info_={}; info_.reset_count=resets; info_.history_reset=true;
}

ReferenceSessionInfo ReferenceSession::info() const {
    std::lock_guard lock(info_mutex_); return info_;
}

RenderOutput ReferenceSession::render(const Scene& scene,const Camera& camera,const Settings& settings,
                                      std::atomic<bool>* cancel) {
    std::lock_guard lock(mutex_);
    if (cancelled(cancel)) return {};
    const auto start=std::chrono::steady_clock::now();
    const auto scene_key=scene_fingerprint(scene),settings_key=settings_fingerprint(settings);
    if (cancelled(cancel)) return {};
    const bool restart=!ready_ || scene_key!=scene_key_ || settings_key!=settings_key_;
    const std::uint64_t index=restart ? 0:info_.frames;
    Settings raw_settings=settings;
    raw_settings.seed=frame_seed(settings.seed,index);
    raw_settings.debug=DebugView::final_color;
    raw_settings.bloom=false; raw_settings.outline=false; raw_settings.hatching=false;
    raw_settings.taa=false; raw_settings.denoise=false; raw_settings.svgf=false;

    ReferenceFrameInput frame;
    frame.jitter_ndc=settings.taa ? taa_jitter(index,settings.render_width,settings.render_height):glm::vec2(0);
    frame.previous_camera=restart ? nullptr:&previous_camera_;
    Image<glm::vec3> previous_positions;
    frame.previous_world_positions=&previous_positions;
    frame.apply_postprocess=false;
    // 编辑器快照未必逐帧更新 previous_world。场景变换已纳入 reset key，故本
    // 序列内对象是静止的：使用解析出的世界变换，不能反复投影回默认单位矩阵。
    const Scene* source=&scene;
    std::optional<Scene> normalized_scene;
    const auto worlds=resolve_world_transforms(scene);
    for (std::size_t i=0;i<scene.nodes.size();++i) {
        if (scene.nodes[i].previous_world!=worlds[i]) {
            if (!normalized_scene) normalized_scene=scene;
            normalized_scene->nodes[i].previous_world=worlds[i];
        }
    }
    if (normalized_scene) source=&*normalized_scene;
    auto output=render_reference(*source,camera,raw_settings,frame,cancel);
    if (cancelled(cancel)) return {}; // renderer 允许返回半帧；会话禁止把半帧提交给历史。
    const int w=settings.render_width,h=settings.render_height;
    require_extent(output.color,w,h,"color"); require_extent(output.surfaces,w,h,"surfaces");
    require_extent(output.indirect,w,h,"indirect"); require_extent(output.ao,w,h,"AO");
    require_extent(output.shadow,w,h,"shadow"); require_extent(previous_positions,w,h,"previous positions");

    // 临时状态提供事务边界：取消或异常不污染下一帧重投影的几何/颜色/矩。
    // 这里有一次历史图复制的 CPU/内存代价，换取后台任务中断后的确定性重试。
    TemporalFilter next=restart ? TemporalFilter{}:temporal_;
    TemporalInput temporal_input;
    temporal_input.motion=MotionConvention::current_minus_previous_pixels_explicit;
    temporal_input.jitter_ndc=frame.jitter_ndc;
    temporal_input.previous_world_positions=&previous_positions;
    output.color=next.process(output.color,output.surfaces,camera,settings,temporal_input);
    output.variance=next.variance();
    if (cancelled(cancel)) return {};

    if (settings.debug==DebugView::final_color) {
        if (settings.outline || settings.hatching || settings.shading==ShadingMode::toon)
            output.color=apply_npr(output.color,output.surfaces,settings);
        if (settings.bloom) {
            const auto glow=bloom(output.color,settings);
            for (std::size_t i=0;i<output.color.pixels.size();++i) output.color.pixels[i]+=glow.pixels[i];
        }
    }
    display_debug(output,scene,camera,settings);
    for (auto& c:output.color.pixels) c=safe_color(c);
    if (cancelled(cancel)) return {};

    ReferenceSessionInfo next_info=info_;
    next_info.frames=index+1;
    next_info.reset_count+=restart && (ready_ || info_.reset_count==0) ? 1:0;
    next_info.frame_seed=raw_settings.seed;
    next_info.jitter_ndc=frame.jitter_ndc;
    next_info.history_reset=restart;
    next_info.reused_fraction=0;
    next_info.max_history_length=0;
    std::size_t valid=0,accepted=0;
    for (std::size_t i=0;i<output.surfaces.pixels.size();++i) {
        if (output.surfaces.pixels[i].valid && output.surfaces.pixels[i].linear_depth>0) {
            ++valid; if (next.history_validity().pixels[i]>0) ++accepted;
        }
        next_info.max_history_length=std::max(next_info.max_history_length,next.history_length().pixels[i]);
    }
    next_info.reused_fraction=valid ? float(accepted)/float(valid):0;
    output.cpu_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    // 检查后的提交不调用任何算法；其余操作都是不抛异常的容器移动/标量赋值。
    if (cancelled(cancel)) return {};
    temporal_=std::move(next); previous_camera_=camera;
    scene_key_=scene_key; settings_key_=settings_key; ready_=true;
    { std::lock_guard info_lock(info_mutex_); info_=next_info; }
    return output;
}
} // namespace emberframe::lab
