#include "visual_experiments.h"
#include <cmath>
#ifdef EMBERFRAME_VISUAL_EXPERIMENTS_TEST_MAIN
#include "shading.h"
#include <fstream>
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
void check(bool value,const std::string& message) { if(!value)throw std::runtime_error(message); }
void check_frame(const RenderOutput& frame,int w,int h) {
    check(frame.color.width==w&&frame.color.height==h,"wrong color dimensions");
    check(frame.surfaces.width==w&&frame.surfaces.height==h,"wrong surface dimensions");
    check(frame.shadow.width==w&&frame.indirect.height==h,"missing auxiliary images");
    for(const auto color:frame.color.pixels)for(int channel=0;channel<3;++channel)
        check(std::isfinite(color[channel])&&color[channel]>=0,"nonfinite/negative output radiance");
}
}
TestResults test_visual_experiments() {
    TestResults results;
    const auto run=[&](const char* name,const std::function<void()>& test) {
        try { test();results.push_back({name,true,"Deterministic rendered-image and integration invariants passed"}); }
        catch(const std::exception& e) { results.push_back({name,false,e.what()}); }
    };
    run("Visual D30 analytic/grid SDF surfaces, cast shadows, repeatability and cancel",[] {
        Scene scene;Camera camera;Settings settings;settings.render_width=96;settings.render_height=64;settings.voxel_resolution=24;settings.samples=2;
        const auto output=run_visual_experiment(29,scene,camera,settings);check_frame(output,96,64);
        std::array<int,2> hits{},sphere{},box{},shadowed{},lit{};int corresponding=0;double error=0;
        for(int y=10;y<64;++y)for(int x=0;x<96;++x) {
            const auto& s=output.surfaces.at(x,y);if(!s.valid)continue;const int panel=x/48;++hits[panel];
            sphere[panel]+=s.object==1;box[panel]+=s.object==2;
            if(s.object==0) {
                shadowed[panel]+=output.shadow.at(x,y)<0.5f;lit[panel]+=output.shadow.at(x,y)>0.9f;
            }
            check(s.depth>=0&&s.depth<=1,"invalid projected SDF depth");
            check(std::abs(glm::length(s.normal)-1)<0.001f,"SDF normal not normalized");
            if(panel==0) {
                const auto& reference=output.surfaces.at(x+48,y);
                if(reference.valid&&reference.object==s.object) { error+=glm::length(reference.position-s.position);++corresponding; }
            }
        }
        for(int panel=0;panel<2;++panel) {
            check(hits[panel]>150&&sphere[panel]>10&&box[panel]>10,"SDF panel missing visible geometry");
            check(shadowed[panel]>2&&lit[panel]>10,"SDF panel lacks both cast shadow and lit ground");
        }
        check(corresponding>100&&error/corresponding<0.16,"grid/analytic surfaces disagree beyond coarse-grid tolerance");
        check(output.rays>std::uint64_t(96*64)&&output.triangles==0,"SDF work counters are not raymarched output");
        const auto repeat=run_visual_experiment(29,scene,camera,settings);
        for(std::size_t i=0;i<output.color.pixels.size();++i)
            check(glm::length(output.color.pixels[i]-repeat.color.pixels[i])<1e-7f,"SDF repeat changed pixels");
        std::atomic<bool> stop{true};const auto cancelled=run_visual_experiment(29,scene,camera,settings,&stop);
        check_frame(cancelled,96,64);check(cancelled.rays==0,"pre-cancelled render did work");
        for(const auto& s:cancelled.surfaces.pixels)check(!s.valid,"cancelled render claimed valid geometry");
        bool rejected=false;try {(void)run_visual_experiment(0,scene,camera,settings);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"unsupported topic silently selected another algorithm");
    });
    run("Visual D11/D12 SH-PRT contrast and D13 prefiltered IBL atlas",[] {
        Scene scene;scene.sky_top=scene.sky_bottom={0.7f,0.5f,0.3f};Camera camera;
        Settings settings;settings.render_width=96;settings.render_height=60;settings.samples=2;
        const auto sh=run_visual_experiment(10,scene,camera,settings);check_frame(sh,96,60);
        int compared=0;double sh_error=0;
        for(int y=10;y<60;++y)for(int x=0;x<48;++x)if(sh.surfaces.at(x,y).valid&&sh.surfaces.at(x+48,y).valid) {
            sh_error+=glm::length(sh.indirect.at(x,y)-sh.indirect.at(x+48,y));++compared;
        }
        check(compared>100&&sh_error/compared<0.02,"SH9 constant environment disagrees with integration baseline");
        const auto prt=run_visual_experiment(11,scene,camera,settings);check_frame(prt,96,60);
        int occluded=0,visible=0;double left_energy=0,right_energy=0;
        for(int y=10;y<60;++y)for(int x=0;x<48;++x)if(prt.surfaces.at(x,y).valid&&prt.surfaces.at(x+48,y).valid) {
            occluded+=prt.shadow.at(x+48,y)<0.9f;visible+=prt.shadow.at(x+48,y)>0.98f;
            left_energy+=glm::length(prt.indirect.at(x,y));right_energy+=glm::length(prt.indirect.at(x+48,y));
        }
        check(occluded>10&&visible>10,"PRT did not expose geometry-dependent visibility");
        check(right_energy<left_energy*0.97,"PRT image did not darken occluded environment lighting");
        check(prt.rays>sh.rays&&prt.node_tests>0,"PRT did not trace BVH visibility rays");
        const auto atlas=run_visual_experiment(12,scene,camera,settings);check_frame(atlas,96,60);
        for(const auto& s:atlas.surfaces.pixels)check(!s.valid,"IBL atlas fabricated geometry");
        float brightest=0;for(int y=0;y<20;++y)for(int x=0;x<32;++x)brightest=std::max(brightest,atlas.indirect.at(x,y).x);
        check(brightest>2,"HDR source lobe missing from atlas");
        const auto brdf=atlas.indirect.at(80,10);check(brdf.x>0.1f&&brdf.x<1.1f&&brdf.y>0&&brdf.y<0.5f&&brdf.z==0,"BRDF A/B tile invalid");
        double difference=0;for(int y=0;y<20;++y)for(int x=0;x<32;++x)
            difference+=glm::length(atlas.indirect.at(x,y+20)-atlas.indirect.at(x+64,y+40));
        check(difference/(32*20)>0.025,"roughness mip tiles are aliases of one image");
        for(const int topic:{10,11,12,29})check(supports_visual_experiment(topic)&&!visual_experiment_guide(topic).empty(),"missing main-app guidance");
        check(!supports_visual_experiment(13),"unsupported topic claimed by visual experiment router");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_VISUAL_EXPERIMENTS_TEST_MAIN
namespace {
// Optional manual QA output from the SAME public renderer; no assets/dependencies.
void save_preview(const std::filesystem::path& path,const emberframe::lab::Image<glm::vec3>& image) {
    const std::uint32_t stride=std::uint32_t(image.width)*4,pixel_bytes=stride*std::uint32_t(image.height);
    std::ofstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("Cannot save visual preview");
    const auto u16=[&](std::uint16_t v){stream.put(char(v));stream.put(char(v>>8));};
    const auto u32=[&](std::uint32_t v){for(int i=0;i<4;++i)stream.put(char(v>>(8*i)));};
    stream.put('B');stream.put('M');u32(54+pixel_bytes);u16(0);u16(0);u32(54);u32(40);
    u32(std::uint32_t(image.width));u32(std::uint32_t(image.height));u16(1);u16(32);u32(0);u32(pixel_bytes);u32(2835);u32(2835);u32(0);u32(0);
    for(int y=image.height-1;y>=0;--y)for(int x=0;x<image.width;++x) {
        const auto c=emberframe::lab::tone_map(image.at(x,y),1);
        stream.put(char(std::lround(std::clamp(c.b,0.0f,1.0f)*255)));stream.put(char(std::lround(std::clamp(c.g,0.0f,1.0f)*255)));
        stream.put(char(std::lround(std::clamp(c.r,0.0f,1.0f)*255)));stream.put(char(255));
    }
}
}
int main(int argc,char** argv) {
    const auto results=emberframe::lab::test_visual_experiments();int failed=0;
    for(const auto& result:results){std::cout<<(result.passed?"PASS ":"FAIL ")<<result.name<<": "<<result.detail<<'\n';failed+=!result.passed;}
    if(argc>1) {
        emberframe::lab::Scene scene;emberframe::lab::Camera camera;emberframe::lab::Settings settings;
        settings.render_width=480;settings.render_height=270;settings.samples=4;
        for(const int topic:{10,11,12,29}) {
            const auto output=emberframe::lab::run_visual_experiment(topic,scene,camera,settings);
            save_preview(std::filesystem::path(argv[1])/("visual_d"+std::to_string(topic+1)+".bmp"),output.color);
            std::cout<<"D"<<topic+1<<" preview: "<<output.cpu_ms<<" ms, "<<output.rays<<" rays\n";
        }
    }
    return failed?1:0;
}
#endif
