#include "vulkan_workbench.h"
#include "config_applicability.h"
#include "reference_renderer.h"
#include "systems.h"

namespace emberframe::lab {
TestResults test_vulkan_colored_room(VulkanWorkbench& gpu,const std::filesystem::path& evidence) {
    TestResults results;
    auto check=[&](const char* name,bool okay,std::string detail){results.push_back({name,okay,std::move(detail)});};
    try {
        Scene scene=make_demo_scene(1);scene.revision=800001;
        Camera camera;camera.position={0,1.55f,6.8f};camera.target={0,1.4f,0};camera.far_plane=40;
        Settings settings;settings.render_width=320;settings.render_height=240;settings.shadow_resolution=1024;
        settings.bloom=false;settings.taa=false;settings.energy_compensation=false;settings.gi=GiMode::environment;
        auto facts=inspect_config(scene,settings);
        check("Colored room configuration facts",!facts.has_environment_input&&!facts.has_directional_light&&!facts.has_texture_input&&facts.low_light_count,
            "Black sky, one rectangle source, no texture input; applicability is derived from scene data");
        settings.shadows=ShadowMode::csm;
        check("Colored room CSM explicitly rejected",!VulkanWorkbench::unsupported_modes(scene,settings).empty(),"Area light does not masquerade as a directional cascade source");
        settings.shadows=ShadowMode::hard;
        auto read=[&](const char* filename,DebugView requested=DebugView::final_color){
            // 等完整场景/天空发布，不能把异步上传中的旧帧当作本次配置响应。
            bool ready=false;for(int frame=0;frame<4000;++frame) {
                gpu.draw(scene,camera,settings,{});const auto& stats=gpu.stats();
                if(!stats.uploadError.empty())throw std::runtime_error(stats.uploadError);
                if(!stats.scene_resources_error.empty())throw std::runtime_error(stats.scene_resources_error);
                if(stats.displayedRevision==scene.revision&&!stats.uploadPending&&stats.environment_ready&&stats.scene_resources_ready){ready=true;break;}
            }
            if(!ready)throw std::runtime_error("Room resources never became ready");
            gpu.request_frame_readback(requested);
            for(int frame=0;frame<4000;++frame){
                gpu.draw(scene,camera,settings,{});if(auto result=gpu.take_frame_readback()){
                    if(!evidence.empty()&&filename){save_bmp(evidence/(std::string(filename)+".bmp"),result->image);save_pfm(evidence/(std::string(filename)+".pfm"),result->image);}
                    return std::move(result->image);
                }
            }
            throw std::runtime_error("Room HDR readback timed out");
        };
        auto finite=[](const Image<glm::vec3>& image){for(auto p:image.pixels)for(int c=0;c<3;++c)if(!std::isfinite(p[c])||p[c]<0)return false;return true;};
        auto delta=[](const Image<glm::vec3>& a,const Image<glm::vec3>& b){double sum=0;float maximum=0;for(std::size_t i=0;i<a.pixels.size();++i){auto d=glm::abs(a.pixels[i]-b.pixels[i]);sum+=d.x+d.y+d.z;maximum=std::max(maximum,std::max({d.x,d.y,d.z}));}return glm::vec2(float(sum/(a.pixels.size()*3)),maximum);};
        settings.debug=DebugView::shadow;auto hard=read("room-shadow-hard");
        auto geometry=read(nullptr,DebugView::albedo);
        std::size_t dark=0,lit=0;for(std::size_t i=0;i<hard.pixels.size();++i)if(glm::length(geometry.pixels[i])>1e-5f){dark+=hard.pixels[i].x<.1f;lit+=hard.pixels[i].x>.9f;}
        check("Colored room area shadow active",finite(hard)&&dark>100&&lit>100,
            "Actual center-perspective visibility: dark pixels="+std::to_string(dark)+", lit pixels="+std::to_string(lit));
        auto visibility_at=[&](const Image<glm::vec3>& image,glm::vec3 p) {
            auto projection=camera.projection(float(settings.render_width)/settings.render_height,settings.reversed_z);
            projection[1][1]*=-1;
            const auto q=projection*camera.view()*glm::vec4(p,1);
            const auto uv=glm::vec2(q)/q.w*.5f+.5f;
            const int x=int(uv.x*image.width),y=int(uv.y*image.height);
            if(q.w<=0||x<0||y<0||x>=image.width||y>=image.height)throw std::runtime_error("Room shadow probe outside camera");
            return image.at(x,y).x;
        };
        // 这些地板点相机可见，但从灯到点的线段穿过左右实体墙。
        // 原投影范围错误把它们当作“shadow map 外，直接有光”，形成外侧亮线。
        float exterior_max=0;
        for(float x:{-4.4f,4.4f})for(float z:{-2.6f,-2.3f,-2.f})
            exterior_max=std::max(exterior_max,visibility_at(hard,{x,0,z}));
        check("Colored room exterior floor has no frustum-edge light leak",exterior_max<.01f,
            "Both wall-occluded outer floor strips remain shadowed; maximum visibility="+std::to_string(exterior_max));
        settings.shadows=ShadowMode::pcss;auto pcss=read("room-shadow-pcss");auto shadow_difference=delta(hard,pcss);
        check("Colored room shadow mode changes visibility",finite(pcss)&&shadow_difference.x>1e-4f,"Hard vs PCSS mean absolute HDR difference="+std::to_string(shadow_difference.x));
        float emitter_min=1;
        for(const auto* image:{&hard,&pcss})for(float x:{-.25f,0.f,.25f})
            emitter_min=std::min(emitter_min,visibility_at(*image,{x,2.92f,0}));
        check("Colored room emitter plane has no alternating shadow stripes",emitter_min>.99f,
            "Coplanar emitter is before the center-map near plane; hard/PCSS visibility minimum="+std::to_string(emitter_min));
        for(auto mode:{ShadowMode::vsm,ShadowMode::vssm,ShadowMode::msm}) {
            settings.shadows=mode;auto image=read(nullptr);bool range=finite(image);for(auto p:image.pixels)range&=p.x<=1.001f;
            check(mode==ShadowMode::vsm?"Colored room linear VSM":mode==ShadowMode::vssm?"Colored room linear VSSM":"Colored room linear MSM",range,"Perspective depth converted to linear distance for moments; all visibility values finite in [0,1]");
        }
        settings.shadows=ShadowMode::pcf;settings.debug=DebugView::final_color;
        auto pbr=read("room-pbr");settings.shading=ShadingMode::blinn_phong;auto blinn=read("room-blinn");
        auto model_difference=delta(pbr,blinn);
        check("Colored room area Blinn model responds",finite(blinn)&&model_difference.x>1e-4f,"PBR vs normalized Blinn area integral mean difference="+std::to_string(model_difference.x));
        settings.shading=ShadingMode::disney;auto disney=read("room-disney");model_difference=delta(pbr,disney);
        check("Colored room area Disney model responds",finite(disney)&&model_difference.x>1e-4f,"PBR vs Disney area integral mean difference="+std::to_string(model_difference.x));
        for(auto& material:scene.materials){material.clearcoat=.9f;material.sheen=.6f;material.anisotropy=.45f;}++scene.revision;
        auto lobes=read("room-disney-lobes");auto lobe_difference=delta(disney,lobes);
        check("Colored room Disney lobes respond",finite(lobes)&&lobe_difference.x>1e-4f,"Clearcoat/sheen/anisotropy mean difference="+std::to_string(lobe_difference.x));
        for(auto& material:scene.materials){material.clearcoat=material.sheen=material.anisotropy=0;if(material.metallic>.9f)material.roughness=.8f;}++scene.revision;
        settings.shading=ShadingMode::pbr;settings.energy_compensation=false;auto kc_off=read("room-kc-off");settings.energy_compensation=true;auto kc_on=read("room-kc-on");auto kc_difference=delta(kc_off,kc_on);
        check("Colored room area Kulla Conty responds",finite(kc_on)&&kc_difference.y>.002f,"Real missing-energy area contribution; maximum HDR difference="+std::to_string(kc_difference.y));
        settings.path=RenderPath::deferred;auto deferred=read("room-deferred");auto path_difference=delta(kc_on,deferred);
        check("Colored room forward deferred consistency",path_difference.y<.04f&&path_difference.x<.001f,"Shared area BRDF and visibility; mean/max error="+std::to_string(path_difference.x)+"/"+std::to_string(path_difference.y));
        settings.path=RenderPath::forward;settings.ao=AoMode::gtao;auto ao_final=read("room-ao-final");auto ao_difference=delta(kc_on,ao_final);
        auto ao_facts=inspect_config(scene,settings);
        check("Colored room black sky AO stays honest",ao_difference.y<.002f&&ao_facts.ao_final_color_unaffected,"AO only attenuates indirect light; black-sky final color unchanged and UI explains why");
        settings.debug=DebugView::ao;auto ao_debug=read("room-ao-debug");float minimum=1;
        for(std::size_t i=0;i<ao_debug.pixels.size();++i)if(glm::length(geometry.pixels[i])>1e-5f)minimum=std::min(minimum,ao_debug.pixels[i].x);
        check("Colored room AO debug remains usable",finite(ao_debug)&&minimum<.9f,"AO is computed, not disabled; AO debug minimum="+std::to_string(minimum));
        settings.debug=DebugView::final_color;settings.ao=AoMode::none;settings.gi=GiMode::ssgi;auto gi=read("room-ssgi");auto gi_difference=delta(kc_on,gi);
        check("Colored room SSGI adds actual indirect input",finite(gi)&&gi_difference.x>1e-4f,"Explicit user GI choice, not a hidden sky/ambient boost; mean difference="+std::to_string(gi_difference.x));
    }catch(const std::exception& e){check("Colored room GPU integration",false,e.what());}
    return results;
}
} // namespace emberframe::lab
