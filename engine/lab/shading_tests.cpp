#include "shading.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#ifdef EMBERFRAME_SHADING_TEST_MAIN
#include <iostream>
#endif

namespace emberframe::lab {
namespace {
void require(bool condition,const std::string& message) { if(!condition) throw std::runtime_error(message); }
float max_error(glm::vec3 a,glm::vec3 b) { auto e=glm::abs(a-b);return std::max({e.x,e.y,e.z}); }
bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
std::string measured(const char* name,double value) { std::ostringstream out;out<<name<<'='<<std::setprecision(7)<<value;return out.str(); }
glm::vec3 furnace(const Material& material,const Settings& settings,glm::vec3 view,int count=16384) {
    glm::dvec3 sum(0);const int half=count/2;
    for(int strategy=0;strategy<2;++strategy) for(int i=0;i<half;++i) {
        const auto u=hammersley(i,half);
        const auto sample=sample_brdf(material,{0,0,1},view,{1,0,0,1},settings,{u.x,u.y,strategy?0.75f:0.25f});
        if(sample.valid) sum+=glm::dvec3(sample.value)*double(sample.direction.z/sample.pdf);
    }
    return glm::vec3(sum/double(count));
}
}

TestResults test_shading() {
    TestResults results;
    auto test=[&](const char* name,const std::function<std::string()>& body) {
        try { results.push_back({name,true,body()}); }
        catch(const std::exception& e) { results.push_back({name,false,e.what()}); }
        catch(...) { results.push_back({name,false,"Unknown exception"}); }
    };
    test("D03 sRGB round-trip, linear interpolation and exposure",[] {
        float error=0;
        for(int i=0;i<=1000;++i) { const float x=i/1000.0f;error=std::max(error,std::abs(linear_to_srgb(srgb_to_linear(x))-x)); }
        require(error<2e-6f,measured("roundtrip_error",error));
        require(std::abs(srgb_to_linear(0.5f)-0.21404114f)<1e-6f,"sRGB decoding incorrect");
        require(max_error(tone_map({1,2,3},0),{0,0,0})==0,"Zero exposure is not black");
        auto a=tone_map({1,2,3},1),b=tone_map({1,2,3},2);
        require(finite(b)&&b.x>=a.x&&b.y>=a.y&&b.z>=a.z&&b.z<=1,"Tone-map range/monotonicity");
        require(finite(tone_map(glm::vec3(1e30f),1e30f)),"HDR tone-map overflow");
        return measured("max_roundtrip_error",error);
    });
    test("D03 sRGB Mips decode before averaging; alpha stays linear",[] {
        Texture t;t.srgb=true;t.levels.emplace_back(2,1);t.levels[0].at(0,0)={0,0,0,0.2f};t.levels[0].at(1,0)={1,1,1,0.8f};
        const auto middle=sample_texture(t,{0.5f,0.5f},FilterMode::bilinear);
        require(max_error(glm::vec3(middle),glm::vec3(0.5f))<1e-6f,"Bilinear interpolation happened in sRGB");
        build_mips(t);const auto mip=sample_texture(t,{0.5f,0.5f},FilterMode::trilinear,1);
        require(t.levels.size()==2&&max_error(glm::vec3(mip),glm::vec3(0.5f))<1e-6f,"Mip average is not linear");
        require(std::abs(mip.a-0.5f)<1e-6f,"Alpha was gamma transformed");
        require(std::abs(t.levels[1].at(0,0).r-linear_to_srgb(0.5f))<1e-6f,"Mip storage encoding incorrect");
        return measured("linear_mip",mip.r);
    });
    test("D03 NPOT area reduction and repeat seams",[] {
        Texture t;t.levels.emplace_back(3,5,glm::vec4(0));double expected=0;
        for(int y=0;y<5;++y) for(int x=0;x<3;++x) { const float v=float(x+3*y)/14;t.levels[0].at(x,y)=glm::vec4(v);expected+=v; }
        build_mips(t);const auto final=t.levels.back().at(0,0);
        require(std::abs(final.r-expected/15)<1e-6,"NPOT texels lost at borders");
        const auto seam=sample_texture(t,{0,0.23f},FilterMode::bilinear);
        const auto repeat=sample_texture(t,{-1,1.23f},FilterMode::bilinear);
        require(max_error(glm::vec3(seam),glm::vec3(repeat))<1e-6f,"Repeat addressing seam");
        return measured("NPOT_mean",final.r);
    });
    test("D03 nearest, trilinear, anisotropic footprints and constant signal",[] {
        Texture t;t.levels.emplace_back(8,8,glm::vec4(0.3f,0.6f,0.8f,0.4f));build_mips(t);
        float error=0;
        for(auto mode:{FilterMode::nearest,FilterMode::bilinear,FilterMode::trilinear,FilterMode::anisotropic})
            for(float lod:{0.0f,0.7f,3.0f}) {
                auto c=sample_texture(t,{0.31f,-0.7f},mode,lod,{0.6f,0.3f},{0.2f,0.1f});
                error=std::max(error,max_error(glm::vec3(c),{0.3f,0.6f,0.8f}));require(std::abs(c.a-0.4f)<2e-6f,"Constant alpha drift");
            }
        require(error<2e-6f,"Filtering changed constant texture");
        Texture stripes;stripes.levels.emplace_back(64,64);
        for(int y=0;y<64;++y) for(int x=0;x<64;++x) stripes.levels[0].at(x,y)=glm::vec4(float(x%2));
        build_mips(stripes);
        const float nearest=sample_texture(stripes,{0.5f/64,0.5f},FilterMode::nearest).r;
        const float aniso=sample_texture(stripes,{0.5f/64,0.5f},FilterMode::anisotropic,0,{8.0f/64,0},{0,0.5f/64}).r;
        require(nearest==0&&std::abs(aniso-0.5f)<0.03f,measured("aniso_stripe_mean",aniso));
        Texture explicit_mips;explicit_mips.levels.emplace_back(2,2,glm::vec4(0));explicit_mips.levels.emplace_back(1,1,glm::vec4(1));
        require(std::abs(sample_texture(explicit_mips,{0.4f,0.4f},FilterMode::trilinear,0.25f).r-0.25f)<1e-6f,"Trilinear LOD interpolation");
        return measured("constant_error",error)+", "+measured("stripe_mean",aniso);
    });
    test("D03 normal mip renormalization and mirrored tangent frame",[] {
        Texture t;t.levels.emplace_back(2,1);t.levels[0].at(0,0)={1,0.5f,0.5f,1};t.levels[0].at(1,0)={0.5f,0.5f,1,1};build_normal_mips(t);
        const auto n=glm::vec3(t.levels.back().at(0,0))*2.0f-1.0f;
        require(std::abs(glm::length(n)-1)<1e-6f,"Normal Mip is not normalized");
        Scene scene;scene.materials.emplace_back();scene.textures.push_back(t);scene.materials[0].normal_texture=0;
        scene.textures[0].levels[0].at(0,0)={0.5f,1,0.5f,1};SurfaceSample s;s.material=0;s.normal={0,0,1};s.uv={0.25f,0.5f};s.tangent={1,0,0,-1};
        Settings settings;settings.filter=FilterMode::nearest;auto mapped=sampled_normal(scene,s,settings);
        require(max_error(mapped,{0,-1,0})<1e-6f,"Tangent handedness was discarded");
        return measured("normal_length",glm::length(n));
    });
    test("Material factors, vertex color, MR channels and indirect-only AO",[] {
        Scene scene;scene.materials.emplace_back();auto& m=scene.materials[0];m.base_color={0.8f,0.6f,0.4f,0.5f};m.roughness=0.8f;m.metallic=0.6f;
        m.base_texture=0;m.mr_texture=1;m.ao_texture=1;m.ao_strength=0.5f;m.emissive_texture=0;m.emissive=glm::vec3(2);
        Texture base;base.srgb=true;base.levels.emplace_back(1,1,glm::vec4(0.5f,0.5f,0.5f,0.5f));scene.textures.push_back(base);
        Texture mr;mr.levels.emplace_back(1,1,glm::vec4(0.2f,0.5f,0.25f,1));scene.textures.push_back(mr);
        SurfaceSample s;s.material=0;s.vertex_color=glm::vec4(0.5f);Settings settings;const auto sampled=sample_material(scene,s,settings);
        const auto expected=glm::vec3(m.base_color)*0.5f*srgb_to_linear(0.5f);
        require(max_error(glm::vec3(sampled.base_color),expected)<1e-6f&&std::abs(sampled.base_color.a-0.125f)<1e-6f,"Material factors not multiplied exactly once");
        require(std::abs(sampled.roughness-0.4f)<1e-6f&&std::abs(sampled.metallic-0.15f)<1e-6f,"MR channels incorrect");
        require(std::abs(sample_occlusion(scene,s,settings)-0.6f)<1e-6f,"AO strength incorrect");
        require(max_error(sampled.emissive,glm::vec3(2*srgb_to_linear(0.5f)))<1e-6f,"Emissive sampling incorrect");
        return "factor/texture/vertex alpha=0.125, roughness=0.4, metallic=0.15, AO=0.6";
    });
    test("D09 uniform/cosine PDFs and Lambert white furnace",[] {
        double sphere=0,hemisphere=0,cosine=0,lambert=0;constexpr int count=8192;
        for(int i=0;i<count;++i) {
            auto u=hammersley(i,count);auto s=sample_uniform_sphere(u),h=sample_uniform_hemisphere(u),c=sample_cosine_hemisphere(u);
            sphere+=s.pdf*4*pi;hemisphere+=h.pdf*2*pi;cosine+=cosine_hemisphere_pdf(h.direction.z)*2*pi;
            lambert+=(1/pi)*c.direction.z/c.pdf;
        }
        require(std::abs(sphere/count-1)<1e-6&&std::abs(hemisphere/count-1)<1e-6&&std::abs(cosine/count-1)<1e-6,"PDF normalization failed");
        require(std::abs(lambert/count-1)<1e-6,"Lambert lost/gained energy");
        require(std::abs(mis_power(0.2f,0.7f)+mis_power(0.7f,0.2f)-1)<1e-6f&&mis_balance(0,0)==0,"MIS partition/zero PDFs");
        require(std::abs(area_to_solid_angle_pdf(0.25f,4,0.5f)-2)<1e-6f,"Area-to-solid-angle Jacobian");
        return measured("Lambert_furnace",lambert/count);
    });
    test("D09 GGX VNDF sphere PDF and null-event probability",[] {
        constexpr int count=65536;const auto v=safe_normalize(glm::vec3(0.8f,0,0.4f));double integral=0,upper=0;int valid=0;float consistency=0;
        for(int i=0;i<count;++i) {
            const auto u=hammersley(i,count);const auto s=sample_uniform_sphere(u);const float p=ggx_pdf(v,s.direction,0.45f,0.7f);
            integral+=p*4*pi;if(s.direction.z>0) upper+=p*4*pi;
            const auto g=sample_ggx(v,0.45f,0.7f,u);valid+=g.valid?1:0;
            consistency=std::max(consistency,std::abs(g.pdf-ggx_pdf(v,g.direction,0.45f,0.7f)));
        }
        integral/=count;upper/=count;const double observed=double(valid)/count;
        require(std::abs(integral-1)<0.008,measured("sphere_pdf",integral));
        require(std::abs(upper-observed)<0.008,measured("upper_pdf",upper)+", "+measured("valid_fraction",observed));
        require(consistency<1e-6f,measured("GGX_reported_pdf_error",consistency));
        return measured("sphere_pdf",integral)+", "+measured("upper_pdf",upper)+", "+measured("valid_fraction",observed);
    });
    test("D14 integrated directional albedo and Kulla-Conty white furnace",[] {
        Material m;m.metallic=1;m.base_color=glm::vec4(1);Settings settings;float worst=0;float darkest=1;
        for(float roughness:{0.4f,0.7f,1.0f}) for(float nv:{0.25f,0.6f,1.0f}) {
            m.roughness=roughness;const glm::vec3 v(std::sqrt(1-nv*nv),0,nv);
            settings.energy_compensation=false;const auto single=furnace(m,settings,v);darkest=std::min(darkest,single.x);
            const float expected=directional_albedo(default_energy_lut(),nv,roughness);
            require(std::abs(single.x-expected)<0.02f,measured("LUT_vs_quadrature_error",std::abs(single.x-expected)));
            settings.energy_compensation=true;const auto multiple=furnace(m,settings,v);worst=std::max(worst,std::abs(multiple.x-1));
        }
        require(darkest<0.5f,"Single scatter reference should lose rough-surface energy");
        require(worst<0.025f,measured("compensated_furnace_error",worst));
        return measured("maximum_energy_error",worst)+", "+measured("minimum_single_scatter",darkest);
    });
    test("D14 colored conductor energy, reciprocity and finite grazing limits",[] {
        Settings settings;Material m;m.metallic=1;m.base_color={0.95f,0.6f,0.2f,1};m.roughness=0.8f;
        const auto energy=furnace(m,settings,safe_normalize(glm::vec3(1,0,0.3f)));
        require(finite(energy)&&energy.x<=1.025f&&energy.x>energy.y&&energy.y>energy.z,"Colored furnace invalid");
        float reciprocity=0;
        for(float r:{0.02f,0.4f,1.0f}) for(float z:{0.00001f,0.1f,1.0f}) {
            m.roughness=r;const auto v=safe_normalize(glm::vec3(1,0,z)),l=safe_normalize(glm::vec3(-0.5f,0.3f,0.7f));
            const auto a=evaluate_brdf(m,{0,0,1},v,l,{1,0,0,1},settings),b=evaluate_brdf(m,{0,0,1},l,v,{1,0,0,1},settings);
            require(finite(a)&&a.x>=0,"Non-finite grazing BRDF");reciprocity=std::max(reciprocity,max_error(a,b));
        }
        require(reciprocity<2e-5f,measured("reciprocity_error",reciprocity));
        require(max_error(evaluate_brdf(m,{0,0,1},{0,0,1},{0,0,-1},{1,0,0,1},settings),glm::vec3(0))==0,"Back hemisphere contributes");
        return measured("red_energy",energy.r)+", "+measured("reciprocity_error",reciprocity);
    });
    test("D15 Disney anisotropic tangent rotation, coat and sheen",[] {
        Settings s;s.shading=ShadingMode::disney;Material m;m.metallic=1;m.roughness=0.5f;m.anisotropy=0.85f;m.base_color=glm::vec4(0.7f);
        const glm::vec3 n(0,0,1),v(0,0,1),lx=safe_normalize(glm::vec3(0.6f,0,1)),ly=safe_normalize(glm::vec3(0,0.6f,1));
        const auto x=evaluate_brdf(m,n,v,lx,{1,0,0,1},s),rot=evaluate_brdf(m,n,v,ly,{0,1,0,1},s),y=evaluate_brdf(m,n,v,ly,{1,0,0,1},s);
        require(max_error(x,rot)<1e-5f,"Rotating tangent did not rotate anisotropic lobe");
        require(std::abs(x.r-y.r)>0.02f,"Anisotropy has no measurable effect");
        m.metallic=0;m.anisotropy=0;m.base_color={0.4f,0.2f,0.1f,1};
        const auto base=evaluate_brdf(m,n,v,lx,{1,0,0,1},s);m.clearcoat=1;
        const auto coat=evaluate_brdf(m,n,v,lx,{1,0,0,1},s);require(finite(coat)&&max_error(base,coat)>1e-5f,"Clearcoat is inert");
        const auto gv=safe_normalize(glm::vec3(1,0,0.1f)),gl=safe_normalize(glm::vec3(-1,0,0.1f));m.clearcoat=0;
        const auto no_sheen=evaluate_brdf(m,n,gv,gl,{1,0,0,1},s);m.sheen=1;
        const auto sheen=evaluate_brdf(m,n,gv,gl,{1,0,0,1},s);require(sheen.r>no_sheen.r+0.01f,"Sheen is inert at grazing angles");
        return measured("anisotropy_delta",std::abs(x.r-y.r))+", "+measured("sheen_delta",sheen.r-no_sheen.r);
    });
    test("D11 SH9 constant, linear signal and irradiance convention",[] {
        const glm::vec3 c(0.2f,0.7f,1.2f);const auto constant=project_sh9([&](glm::vec3){return c;},4096);
        const auto linear=project_sh9([](glm::vec3 d){return glm::vec3(0.5f+0.2f*d.x+0.15f*d.z);},4096);
        float worst=0;
        for(auto d:{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(0,0,-1),safe_normalize(glm::vec3(1,2,3))}) {
            worst=std::max(worst,max_error(evaluate_sh9(constant,d),c));
            worst=std::max(worst,max_error(evaluate_sh9_irradiance(constant,d),c*pi));
            worst=std::max(worst,max_error(evaluate_sh9(linear,d),glm::vec3(0.5f+0.2f*d.x+0.15f*d.z)));
        }
        require(worst<0.004f,measured("SH_reconstruction_error",worst));return measured("max_SH_error",worst);
    });
    test("D12 PRT unshadowed, half-visible and fully shadowed constant environment",[] {
        const auto env=project_sh9([](glm::vec3){return glm::vec3(1);},4096);PrtOptions options;options.samples=4096;
        const auto unshadowed=bake_diffuse_prt({0,0,0},{0,0,1},{},options);
        const auto shadowed=bake_diffuse_prt({0,0,0},{0,0,1},[](glm::vec3,glm::vec3){return 0.0f;},options);
        const auto half=bake_diffuse_prt({0,0,0},{0,0,1},[](glm::vec3,glm::vec3 d){return d.x>0?1.0f:0.0f;},options);
        const auto a=evaluate_diffuse_prt(unshadowed,env,glm::vec3(0.7f)),b=evaluate_diffuse_prt(shadowed,env,glm::vec3(0.7f)),c=evaluate_diffuse_prt(half,env,glm::vec3(0.7f));
        require(max_error(a,glm::vec3(0.7f))<0.002f&&max_error(b,glm::vec3(0))==0&&max_error(c,glm::vec3(0.35f))<0.002f,"Visibility was not integrated in PRT");
        return measured("open",a.r)+", "+measured("blocked",b.r)+", "+measured("half",c.r);
    });
    test("D12 interreflection PRT colored bounce and explicit depth truncation",[] {
        const auto env=project_sh9([](glm::vec3){return glm::vec3(1);},2048);
        const PrtTraceFunction trace=[](const Ray& r)->std::optional<PrtHit> {
            if(r.origin.z<0.5f) return PrtHit{{0,0,1},{0,0,-1},{0.8f,0.3f,0.1f}};
            return std::nullopt;
        };
        const auto bounced=bake_interreflection_prt({0,0,0},{0,0,1},trace,1,{4096,1e-4f});
        const auto truncated=bake_interreflection_prt({0,0,0},{0,0,1},trace,0,{4096,1e-4f});
        const auto color=evaluate_diffuse_prt(bounced,env,glm::vec3(1));
        require(max_error(color,{0.8f,0.3f,0.1f})<0.002f,"Secondary albedo/transport not integrated");
        require(max_error(evaluate_diffuse_prt(truncated,env,glm::vec3(1)),glm::vec3(0))==0,"Depth truncation incorrectly emits light");
        return measured("bounce_red",color.r)+", "+measured("bounce_green",color.g);
    });
    test("D12 glossy 9x9 transfer has independent outgoing and incoming coefficients",[] {
        Settings settings;settings.shading=ShadingMode::toon;Material m;m.base_color=glm::vec4(0.6f);
        const auto open=bake_glossy_prt({0,0,0},{0,0,1},{1,0,0,1},m,settings,{},128,256);
        const auto closed=bake_glossy_prt({0,0,0},{0,0,1},{1,0,0,1},m,settings,[](glm::vec3,glm::vec3){return 0.0f;},8,16);
        const auto env=project_sh9([](glm::vec3){return glm::vec3(1);},2048);const auto outgoing=apply_glossy_prt(open,env),black=apply_glossy_prt(closed,env);
        // Lambert 出射半球为常数；球面 SH 的 DC 系数=2*pi*Y00*albedo。
        const float expected=2*pi*0.2820947918f*0.6f;
        require(std::abs(outgoing[0].r-expected)<0.005f,"Glossy transport matrix DC scale");
        require(glm::length(outgoing[2])>0.1f,"Outgoing-direction SH dimension missing");
        for(const auto& c:black) require(glm::length(c)==0,"Shadowed glossy matrix is not zero");
        return measured("outgoing_DC",outgoing[0].r);
    });
    test("D13 IBL constant signal, irradiance pi, BRDF LUT and seam",[] {
        const glm::vec3 color(0.4f,0.8f,1.2f);IblOptions options;options.width=12;options.height=6;options.roughness_levels=4;options.lut_resolution=8;options.samples=512;
        const auto ibl=precompute_ibl([&](glm::vec3){return color;},options);float error=0;
        for(auto d:{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(0,-1,0),safe_normalize(glm::vec3(1,0,-0.00001f))}) {
            error=std::max(error,max_error(sample_ibl_diffuse(ibl,d),color*pi));
            for(float r:{0.0f,0.4f,1.0f}) error=std::max(error,max_error(sample_ibl_specular(ibl,d,r),color));
        }
        require(error<3e-6f,measured("IBL_constant_error",error));
        float lut_error=0;
        for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
            const auto ab=ibl.brdf.at(x,y);require(ab.x>=0&&ab.y>=0&&ab.x+ab.y<=1.01f,"BRDF LUT invalid energy");
            lut_error=std::max(lut_error,std::abs(ab.x+ab.y-directional_albedo(default_energy_lut(),float(x)/7,float(y)/7)));
        }
        require(lut_error<0.025f,measured("BRDF_LUT_vs_energy_LUT",lut_error));
        return measured("constant_error",error)+", "+measured("LUT_error",lut_error);
    });
    test("D13 roughness prefilter attenuates a directional environment signal",[] {
        IblOptions o;o.width=16;o.height=8;o.roughness_levels=4;o.lut_resolution=4;o.samples=256;
        const auto ibl=precompute_ibl([](glm::vec3 d){return glm::vec3(0.5f+0.5f*d.y);},o);
        const float sharp=sample_ibl_specular(ibl,{0,1,0},0).r,rough=sample_ibl_specular(ibl,{0,1,0},1).r;
        require(sharp>rough+0.1f&&rough>0.45f,"Roughness prefilter does not convolve environment");
        const float irradiance=sample_ibl_diffuse(ibl,{0,1,0}).r;
        require(std::abs(irradiance-(0.5f*pi+pi/3))<0.045f,"Diffuse prefilter analytic linear-environment comparison");
        return measured("sharp",sharp)+", "+measured("rough",rough);
    });
    test("D16 identity LTC rectangle versus independent Lambert area quadrature",[] {
        RectangleLight light;light.center={0,0,2};light.half_u={0.7f,0,0};light.half_v={0,-0.5f,0};
        Settings settings;settings.shading=ShadingMode::toon;Material m;m.base_color=glm::vec4(1);
        const float analytic=integrate_ltc_rectangle(light,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1));
        const auto reference=integrate_rectangle_reference(light,{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},m,settings,32768);
        require(std::abs(analytic-reference.r)<0.0001f,measured("LTC_Lambert_error",std::abs(analytic-reference.r)));
        light.half_v=-light.half_v;require(integrate_ltc_rectangle(light,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1))==0,"Backside emits");
        light.two_sided=true;require(std::abs(integrate_ltc_rectangle(light,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1))-analytic)<1e-6f,"Two-sided winding incorrect");
        return measured("analytic",analytic)+", "+measured("reference",reference.r);
    });
    test("D16 LTC horizon clipping versus independent rectangle quadrature",[] {
        RectangleLight light;light.center={1,0,0.15f};light.half_u={0,0.6f,0};light.half_v={0,0,-0.7f};light.two_sided=true;
        Settings settings;settings.shading=ShadingMode::toon;Material m;m.base_color=glm::vec4(1);
        const float analytic=integrate_ltc_rectangle(light,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1));
        const auto reference=integrate_rectangle_reference(light,{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},m,settings,32768);
        require(analytic>0&&std::abs(analytic-reference.r)<0.0002f,measured("horizon_error",std::abs(analytic-reference.r)));
        light.center.z=-2;require(integrate_ltc_rectangle(light,{0,0,0},{0,0,1},{0,0,1},glm::mat3(1))==0,"Below-horizon rectangle contributes");
        return measured("horizon_analytic",analytic)+", "+measured("horizon_reference",reference.r);
    });
    test("D16 fitted LTC is finite, roughness-dependent and matches GGX reference",[] {
        LtcOptions options;options.cosine_resolution=4;options.roughness_resolution=4;options.samples=256;options.iterations=32;
        const auto lut=precompute_ltc(options);float max_fit=0;
        for(const auto& e:lut.entries.pixels) { require(glm::determinant(e.inverse)>0&&std::isfinite(e.fit_error),"Singular fitted LTC");max_fit=std::max(max_fit,e.fit_error); }
        require(std::abs(lut.entries.at(3,0).inverse[0][0]-lut.entries.at(3,3).inverse[0][0])>0.1f,"Fit is a fixed matrix stub");
        RectangleLight light;light.center={0,0,2};light.half_u={1,0,0};light.half_v={0,-1,0};
        Settings settings;settings.energy_compensation=false;Material m;m.metallic=1;m.base_color=glm::vec4(1);float worst=0;
        for(float r:{2.0f/3,1.0f}) {
            m.roughness=r;const auto approx=evaluate_ltc_rectangle(lut,light,{0,0,0},{0,0,1},{0,0,1},glm::vec3(0),glm::vec3(1),r);
            const auto selected=evaluate_rectangle_light(lut,light,{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},m,settings);
            require(max_error(selected,approx)<1e-7f,"KC-off PBR rectangle changed the calibrated LTC baseline");
            const auto reference=integrate_rectangle_reference(light,{0,0,0},{0,0,1},{0,0,1},{1,0,0,1},m,settings,32768);
            const float relative=std::abs(approx.r-reference.r)/reference.r;worst=std::max(worst,relative);
        }
        // Coarse 4x4 four-parameter fit is intentionally approximate; numerical
        // integration tests above use much tighter tolerances than shape fitting.
        require(worst<0.22f,measured("LTC_GGX_relative_error",worst));
        require(max_fit<0.6f,measured("maximum_Hellinger_error",max_fit));
        return measured("reference_relative_error",worst)+", "+measured("max_fit_objective",max_fit);
    });
    test("D16 rectangle selects actual Blinn and Disney BRDFs",[] {
        const LtcLut unused;RectangleLight light;light.center={.35f,.2f,2};light.half_u={.6f,0,0};light.half_v={0,-.4f,0};
        Material m;m.base_color={.7f,.2f,.08f,1};m.metallic=.3f;m.roughness=.7f;
        m.clearcoat=.7f;m.clearcoat_roughness=.4f;m.anisotropy=.5f;m.sheen=.6f;
        Settings s;s.energy_compensation=false;const auto view=safe_normalize(glm::vec3(.45f,.2f,1));
        glm::vec3 blinn(0),disney(0);float worst=0;
        for(const auto mode:{ShadingMode::blinn_phong,ShadingMode::disney}) {
            s.shading=mode;const auto actual=evaluate_rectangle_light(unused,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);
            const auto reference=integrate_rectangle_reference(light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s,65536);
            worst=std::max(worst,max_error(actual,reference));require(finite(actual)&&worst<1e-4f,measured("selected_BRDF_area_error",worst));
            if(mode==ShadingMode::blinn_phong)blinn=actual;else disney=actual;
        }
        require(max_error(blinn,disney)>1e-4f,"Rectangle material-model selection is inert");
        s.shading=ShadingMode::toon;const auto toon=evaluate_rectangle_light(unused,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);
        require(max_error(toon,light.radiance*glm::vec3(m.base_color)*integrate_ltc_rectangle(light,{0,0,0},{0,0,1},view,glm::mat3(1)))<1e-7f,"Toon lost analytic Lambert integration");
        return measured("independent_area_error",worst)+", "+measured("model_delta",max_error(blinn,disney));
    });
    test("D16 rectangle KC integrates only missing energy and respects model scope",[] {
        const auto lut=precompute_ltc({4,4,256,32});RectangleLight light;light.center={.35f,.1f,2};light.half_u={.55f,0,0};light.half_v={0,-.4f,0};
        Material m;m.base_color={.85f,.4f,.15f,1};m.metallic=1;m.roughness=.8f;
        Settings s;const auto view=safe_normalize(glm::vec3(.55f,.1f,1));float worst=0;
        for(const auto mode:{ShadingMode::pbr,ShadingMode::disney}) {
            s.shading=mode;m.clearcoat=mode==ShadingMode::disney?.7f:0;m.clearcoat_roughness=.4f;
            s.energy_compensation=false;const auto off=evaluate_rectangle_light(lut,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);
            const auto reference_off=integrate_rectangle_reference(light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s,65536);
            s.energy_compensation=true;const auto on=evaluate_rectangle_light(lut,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);
            const auto reference_on=integrate_rectangle_reference(light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s,65536);
            const auto delta=on-off;worst=std::max(worst,max_error(delta,reference_on-reference_off));
            require(worst<1e-4f,measured("KC_increment_area_error",worst));
            require(delta.r>delta.g&&delta.g>delta.b&&delta.b>0,"KC addition lost conductor tint or did not restore energy");
        }
        for(const auto mode:{ShadingMode::disney,ShadingMode::blinn_phong,ShadingMode::toon}) {
            s.shading=mode;m.anisotropy=.6f;s.energy_compensation=false;
            const auto off=evaluate_rectangle_light(lut,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);s.energy_compensation=true;
            const auto on=evaluate_rectangle_light(lut,light,{0,0,0},{0,0,1},view,{1,0,0,1},m,s);
            require(max_error(on,off)==0,"Isotropic GGX KC leaked into anisotropic Disney/Blinn/Toon");
        }
        return measured("independent_KC_increment_error",worst);
    });
    test("D16 rectangle tangent, lobes, emitting side and geometric scale",[] {
        const LtcLut unused;RectangleLight light;light.center={.65f,0,2};light.half_u={.2f,0,0};light.half_v={0,-.12f,0};
        Material m;m.base_color={.7f,.2f,.08f,1};m.metallic=1;m.roughness=.6f;m.anisotropy=.8f;
        Settings s;s.shading=ShadingMode::disney;s.energy_compensation=false;
        const auto evaluate=[&](const RectangleLight& rect,glm::vec4 tangent) {return evaluate_rectangle_light(unused,rect,{0,0,0},{0,0,1},{0,0,1},tangent,m,s);};
        const auto original=evaluate(light,{1,0,0,1}),rotated=evaluate(light,{0,1,0,1});
        require(max_error(original,rotated)>1e-4f,"Rectangle ignores anisotropic tangent rotation");
        m.anisotropy=-.8f;require(max_error(original,evaluate(light,{0,1,0,-1}))<1e-6f,"Anisotropy sign/tangent/handedness inconsistent");m.anisotropy=.8f;
        auto scaled=light;scaled.center*=2;scaled.half_u*=2;scaled.half_v*=2;
        require(max_error(original,evaluate(scaled,{1,0,0,1}))<1e-6f,"Area/distance squared scale invariance failed");
        auto back=light;back.half_v=-back.half_v;require(max_error(evaluate(back,{1,0,0,1}),glm::vec3(0))==0,"One-sided rectangle emits from its back");
        back.two_sided=true;require(max_error(original,evaluate(back,{1,0,0,1}))<1e-6f,"Two-sided emitter cosine/winding mismatch");
        back.half_u=glm::vec3(0);require(max_error(evaluate(back,{1,0,0,1}),glm::vec3(0))==0,"Degenerate rectangle contributes");
        require(max_error(evaluate_rectangle_light(unused,light,{0,0,0},{0,0,-1},{0,0,-1},{1,0,0,1},m,s),glm::vec3(0))==0,"Below-surface directions contribute");
        m.metallic=0;m.anisotropy=0;m.clearcoat=0;const auto bare=evaluate(light,{1,0,0,1});m.clearcoat=1;m.clearcoat_roughness=.4f;
        require(max_error(bare,evaluate(light,{1,0,0,1}))>1e-5f,"Rectangle clearcoat is inert");
        m.clearcoat=0;light.center={.5f,0,.1f};light.half_u={.01f,0,0};light.half_v={0,-.01f,0};
        const auto grazing=safe_normalize(glm::vec3(-1,0,.1f));
        const auto no_sheen=evaluate_rectangle_light(unused,light,{0,0,0},{0,0,1},grazing,{1,0,0,1},m,s);m.sheen=1;
        const auto sheen=evaluate_rectangle_light(unused,light,{0,0,0},{0,0,1},grazing,{1,0,0,1},m,s);
        require(max_error(no_sheen,sheen)>1e-5f,"Rectangle sheen is inert at grazing angles");
        return measured("tangent_delta",max_error(original,rotated))+", "+measured("sheen_delta",max_error(no_sheen,sheen));
    });
    test("Bounded precomputation rejects excessive resolutions",[] {
        bool energy=false,ibl=false,ltc=false;
        try { auto t=precompute_energy_lut({10000,2,16});(void)t; } catch(const std::invalid_argument&) { energy=true; }
        try { IblOptions o;o.width=10000;auto t=precompute_ibl([](glm::vec3){return glm::vec3(1);},o);(void)t; } catch(const std::invalid_argument&) { ibl=true; }
        try { auto t=precompute_ltc({2,2,32,10000});(void)t; } catch(const std::invalid_argument&) { ltc=true; }
        require(energy&&ibl&&ltc,"Precomputation bound not enforced");return "Energy/IBL/LTC invalid configurations rejected";
    });
    return results;
}
}

#ifdef EMBERFRAME_SHADING_TEST_MAIN
int main() {
    const auto results=emberframe::lab::test_shading();int failed=0;
    for(const auto& r:results) { std::cout<<(r.passed?"PASS ":"FAIL ")<<r.name<<": "<<r.detail<<'\n';failed+=r.passed?0:1; }
    std::cout<<results.size()-failed<<'/'<<results.size()<<" passed\n";return failed?1:0;
}
#endif
