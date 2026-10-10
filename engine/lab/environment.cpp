#include "environment.h"
#include <bit>
#include <charconv>
#include <cstring>
#include <fstream>
#include <mutex>

// HDR 单独使用私有 stb 实现，不与现有 PNG/JPEG 加载器共享可变解码状态。
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace emberframe::lab {
namespace {
constexpr std::uint64_t seed=14695981039346656037ull;
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
bool finite(glm::vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
void dimensions(int width,int height) {
    require(width>0&&height>0&&width<=EnvironmentLimits::dimension&&height<=EnvironmentLimits::dimension,
            "HDR dimension exceeds 8192 or is empty");
    require(std::uint64_t(width)*height<=EnvironmentLimits::pixels,"HDR exceeds 8M pixel budget");
}
void validate_hdr_stream(std::span<const std::byte> bytes,int width,int height) {
    // stb 对截断的 flat RGBE 不总报错，某些坏 RLE 可反复读取 EOF。
    // 先完整验证编码长度/游程，之后才进入 stbi_loadf，避免坏文件挂住导入线程。
    std::size_t offset=0;
    auto line=[&] {
        const auto start=offset;
        while(offset<bytes.size()&&bytes[offset]!=std::byte{'\n'})++offset;
        require(offset<bytes.size()&&offset-start<=512&&offset<4096,"Malformed/oversized HDR header");
        std::string result(reinterpret_cast<const char*>(bytes.data()+start),offset-start);++offset;
        if(!result.empty()&&result.back()=='\r')result.pop_back();return result;
    };
    const auto magic=line();require(magic=="#?RADIANCE"||magic=="#?RGBE","Invalid HDR signature");
    bool format=false;
    for(;;){const auto record=line();if(record.empty())break;if(record=="FORMAT=32-bit_rle_rgbe")format=true;}
    require(format,"Unsupported HDR format");const auto layout=line();
    require(layout.starts_with("-Y "),"HDR must use -Y / +X orientation");
    const auto separator=layout.find(" +X ",3);require(separator!=std::string::npos,"HDR must use -Y / +X orientation");
    int h=0,w=0;const auto h_result=std::from_chars(layout.data()+3,layout.data()+separator,h);
    const auto w_result=std::from_chars(layout.data()+separator+4,layout.data()+layout.size(),w);
    require(h_result.ec==std::errc{}&&h_result.ptr==layout.data()+separator&&w_result.ec==std::errc{}&&
            w_result.ptr==layout.data()+layout.size()&&w==width&&h==height,"Invalid HDR resolution record");
    auto byte=[&] {require(offset<bytes.size(),"Truncated HDR RGBE stream");return std::to_integer<unsigned char>(bytes[offset++]);};
    const bool rle=width>=8&&bytes.size()-offset>=4&&bytes[offset]==std::byte{2}&&bytes[offset+1]==std::byte{2}&&
                   (std::to_integer<unsigned char>(bytes[offset+2])&128)==0;
    if(!rle) {
        require(bytes.size()-offset==std::size_t(width)*height*4,"Truncated/trailing flat HDR pixels");return;
    }
    for(int y=0;y<height;++y) {
        require(byte()==2&&byte()==2,"Mixed flat/RLE HDR scanlines unsupported");
        const int high=byte(),low=byte();require((high*256+low)==width,"HDR scanline width mismatch");
        for(int channel=0;channel<4;++channel) {
            int count=0;
            while(count<width) {
                const int code=byte(),run=code>128?code-128:code;
                require(run>0&&run<=width-count,"Invalid zero/oversized HDR RLE run");
                if(code>128)(void)byte();
                else {require(bytes.size()-offset>=std::size_t(run),"Truncated HDR RLE literals");offset+=run;}
                count+=run;
            }
        }
    }
    require(offset==bytes.size(),"Trailing HDR scanline data");
}
void hash_byte(std::uint64_t& hash,unsigned char b) { hash=(hash^b)*1099511628211ull; }
void hash_u32(std::uint64_t& hash,std::uint32_t value) { for(int i=0;i<4;++i)hash_byte(hash,static_cast<unsigned char>(value>>(8*i))); }
void hash_float(std::uint64_t& hash,float value) { hash_u32(hash,std::bit_cast<std::uint32_t>(value==0?0.f:value)); }
std::uint64_t bytes_hash(std::string_view bytes) { auto hash=seed;for(unsigned char b:bytes)hash_byte(hash,b);return hash; }
void append_u32(std::string& out,std::uint32_t value) { for(int i=0;i<4;++i)out.push_back(char(value>>(8*i))); }
void append_u64(std::string& out,std::uint64_t value) { for(int i=0;i<8;++i)out.push_back(char(value>>(8*i))); }
struct Reader {
    std::string_view bytes;std::size_t offset=0;
    std::uint32_t u32() {
        require(bytes.size()-offset>=4,"Truncated embedded HDR");std::uint32_t value=0;
        for(int i=0;i<4;++i)value|=std::uint32_t(static_cast<unsigned char>(bytes[offset++]))<<(8*i);return value;
    }
    std::uint64_t u64() { const auto lo=u32();return lo|(std::uint64_t(u32())<<32); }
};
Scene environment_only(const Scene& scene) {
    Scene result;result.sky_top=scene.sky_top;result.sky_bottom=scene.sky_bottom;
    result.environment_map=scene.environment_map;result.environment_intensity=scene.environment_intensity;
    result.environment_rotation=scene.environment_rotation;return result;
}
bool same_options(const IblOptions& a,const IblOptions& b) {
    return a.width==b.width&&a.height==b.height&&a.roughness_levels==b.roughness_levels&&
           a.lut_resolution==b.lut_resolution&&a.samples==b.samples;
}
}

EnvironmentMap::EnvironmentMap(Image<glm::vec3> image,std::string name):image_(std::move(image)),name_(std::move(name)) {
    dimensions(image_.width,image_.height);
    require(image_.pixels.size()==std::size_t(image_.width)*image_.height,"Invalid HDR pixel storage");
    require(name_.size()<=4096,"HDR label exceeds budget");
    fingerprint_=seed;hash_u32(fingerprint_,std::uint32_t(image_.width));hash_u32(fingerprint_,std::uint32_t(image_.height));
    for(const auto pixel:image_.pixels)for(int channel=0;channel<3;++channel) {
        const float value=pixel[channel];
        require(std::isfinite(value)&&value>=0&&value<=EnvironmentLimits::radiance,
                "HDR radiance must be finite, nonnegative and <=65504 (RGBA16F safety)");
        hash_float(fingerprint_,value);
    }
}
std::shared_ptr<const EnvironmentMap> make_environment_map(Image<glm::vec3> image,std::string name) {
    return std::shared_ptr<const EnvironmentMap>(new EnvironmentMap(std::move(image),std::move(name)));
}
std::shared_ptr<const EnvironmentMap> load_environment_hdr(std::span<const std::byte> bytes,std::string name) {
    require(!bytes.empty()&&bytes.size()<=EnvironmentLimits::file_bytes,"HDR file exceeds 32 MiB budget or is empty");
    const auto* data=reinterpret_cast<const stbi_uc*>(bytes.data());const int length=int(bytes.size());
    require(stbi_is_hdr_from_memory(data,length)!=0,"Expected Radiance .hdr RGBE data (not an LDR image)");
    int width=0,height=0,channels=0;
    require(stbi_info_from_memory(data,length,&width,&height,&channels)!=0,"Invalid Radiance HDR header");
    dimensions(width,height); // 先检查头部，避免让解码器为恶意尺寸申请巨量内存。
    validate_hdr_stream(bytes,width,height);
    const int expected_width=width,expected_height=height;
    std::unique_ptr<float,decltype(&stbi_image_free)> decoded(
        stbi_loadf_from_memory(data,length,&width,&height,&channels,3),stbi_image_free);
    require(bool(decoded),"Radiance HDR float decoding failed");
    require(width==expected_width&&height==expected_height,"HDR dimensions changed while decoding");
    Image<glm::vec3> image(width,height);
    for(std::size_t i=0;i<image.pixels.size();++i)
        image.pixels[i]={decoded.get()[3*i],decoded.get()[3*i+1],decoded.get()[3*i+2]};
    // 没有 gamma 解码：Radiance RGBE 本身就是线性辐亮度，亮度可大于 1。
    auto result=std::unique_ptr<EnvironmentMap>(new EnvironmentMap(std::move(image),std::move(name)));
    result->encoded_.assign(bytes.begin(),bytes.end());
    return std::shared_ptr<const EnvironmentMap>(std::move(result));
}
std::shared_ptr<const EnvironmentMap> load_environment_hdr(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Cannot open Radiance HDR file");
    const auto length=file.tellg();require(length>0&&std::uint64_t(length)<=EnvironmentLimits::file_bytes,"HDR file exceeds 32 MiB budget");
    std::vector<std::byte> bytes(std::size_t(length),std::byte{});file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(file),"Incomplete HDR file read");
    const auto label=path.filename().u8string();
    return load_environment_hdr(bytes,std::string(reinterpret_cast<const char*>(label.data()),label.size()));
}
void validate_environment_settings(const Scene& scene) {
    require(finite(scene.sky_top)&&finite(scene.sky_bottom),"Nonfinite analytic sky");
    require(std::isfinite(scene.environment_intensity)&&scene.environment_intensity>=0&&scene.environment_intensity<=64,
            "Environment intensity must be finite and within [0,64]");
    require(std::isfinite(scene.environment_rotation),"Nonfinite environment rotation");
}
glm::vec3 environment_lookup_direction(glm::vec3 direction,float rotation) {
    require(finite(direction)&&std::isfinite(rotation),"Nonfinite environment direction/rotation");
    direction=safe_normalize(direction,{0,1,0});
    const float angle=glm::radians(std::remainder(rotation,360.f)),c=std::cos(angle),s=std::sin(angle);
    return {c*direction.x-s*direction.z,direction.y,s*direction.x+c*direction.z};
}
glm::vec3 sample_environment_map(const EnvironmentMap& map,glm::vec3 direction) {
    require(finite(direction),"Nonfinite environment sample direction");direction=safe_normalize(direction,{0,1,0});
    const auto& image=map.image();
    float u=glm::dot(glm::vec2(direction.x,direction.z),glm::vec2(direction.x,direction.z))>1e-20f
        ?std::atan2(direction.z,direction.x)/(2*pi):0.f;u-=std::floor(u);
    const float v=std::acos(std::clamp(direction.y,-1.f,1.f))/pi;
    const float x=u*image.width-.5f,y=v*image.height-.5f;const int ix=int(std::floor(x)),iy=int(std::floor(y));
    auto texel=[&](int xx,int yy){return image.at((xx%image.width+image.width)%image.width,std::clamp(yy,0,image.height-1));};
    return glm::mix(glm::mix(texel(ix,iy),texel(ix+1,iy),x-ix),glm::mix(texel(ix,iy+1),texel(ix+1,iy+1),x-ix),y-iy);
}
std::uint64_t environment_fingerprint(const Scene& scene) {
    validate_environment_settings(scene);auto hash=seed;
    hash_u32(hash,scene.environment_map?1u:0u);
    if(scene.environment_map) {
        const auto key=scene.environment_map->fingerprint();hash_u32(hash,std::uint32_t(key));hash_u32(hash,std::uint32_t(key>>32));
        hash_float(hash,std::remainder(scene.environment_rotation,360.f));
    }else for(int i=0;i<3;++i){hash_float(hash,scene.sky_top[i]);hash_float(hash,scene.sky_bottom[i]);}
    hash_float(hash,scene.environment_intensity);return hash;
}
IblOptions scene_ibl_options(const Scene& scene) noexcept {
    IblOptions options;if(scene.environment_map){options.width=128;options.height=64;options.roughness_levels=8;}return options;
}
IblData precompute_scene_ibl(const Scene& scene,IblOptions options) {
    validate_environment_settings(scene);auto source=environment_only(scene);source.environment_intensity=1;source.environment_rotation=0;
    auto result=precompute_ibl([source](glm::vec3 d){return environment(source,d);},options);
    result.environment_intensity=scene.environment_intensity;result.environment_rotation=scene.environment_rotation;return result;
}
IblData precompute_scene_ibl(const Scene& scene) {return precompute_scene_ibl(scene,scene_ibl_options(scene));}
std::shared_ptr<const IblData> cached_scene_ibl(const Scene& scene,IblOptions options) {
    struct Entry {std::uint64_t key;IblOptions options;std::shared_ptr<const IblData> data;};
    static std::mutex mutex;static std::vector<Entry> kernels,views;const auto key=environment_fingerprint(scene);
    auto source=environment_only(scene);source.environment_intensity=1;source.environment_rotation=0;
    const auto base_key=environment_fingerprint(source);
    std::shared_ptr<const IblData> maps;
    {std::lock_guard lock(mutex);
        for(const auto& e:views)if(e.key==key&&same_options(e.options,options))return e.data;
        for(const auto& e:kernels)if(e.key==base_key&&same_options(e.options,options)){maps=e.data;break;}
    }
    // 积分不持有缓存锁，不捕获可变 Scene&；仅共享不可变图片和几个天空参数。
    if(!maps) {
        auto computed=std::make_shared<const IblData>(precompute_scene_ibl(source,options));
        std::lock_guard lock(mutex);
        for(const auto& e:kernels)if(e.key==base_key&&same_options(e.options,options)){maps=e.data;break;}
        if(!maps){maps=std::move(computed);if(kernels.size()==2)kernels.erase(kernels.begin());kernels.push_back({base_key,options,maps});}
    }
    auto view=std::make_shared<IblData>();view->shared_maps=maps;
    view->environment_intensity=scene.environment_intensity;view->environment_rotation=scene.environment_rotation;
    std::shared_ptr<const IblData> result=std::move(view);
    std::lock_guard lock(mutex);for(const auto& e:views)if(e.key==key&&same_options(e.options,options))return e.data;
    if(views.size()==4)views.erase(views.begin());views.push_back({key,options,result});return result;
}
std::shared_ptr<const IblData> cached_scene_ibl(const Scene& scene) {return cached_scene_ibl(scene,scene_ibl_options(scene));}
std::string serialize_environment(const EnvironmentMap& map) {
    const bool encoded=!map.encoded_.empty();
    std::string payload;payload.reserve(16+map.name().size()+(encoded?map.encoded_.size():map.image().pixels.size()*12));
    append_u32(payload,std::uint32_t(map.image().width));append_u32(payload,std::uint32_t(map.image().height));
    append_u32(payload,std::uint32_t(map.name().size()));payload+=map.name();
    append_u32(payload,encoded?1u:0u);
    if(encoded)payload.append(reinterpret_cast<const char*>(map.encoded_.data()),map.encoded_.size());
    else for(const auto pixel:map.image().pixels)for(int c=0;c<3;++c)append_u32(payload,std::bit_cast<std::uint32_t>(pixel[c]));
    std::string out="EFEV";append_u32(out,2);append_u64(out,payload.size());append_u64(out,bytes_hash(payload));out+=payload;return out;
}
std::shared_ptr<const EnvironmentMap> deserialize_environment(std::string_view bytes) {
    require(bytes.size()>=36&&bytes.size()<=40+4096+EnvironmentLimits::pixels*12&&bytes.substr(0,4)=="EFEV","Invalid embedded HDR envelope");
    Reader header{bytes.substr(4,20)};const auto version=header.u32();require(version==1||version==2,"Unsupported embedded HDR version");
    const auto length=header.u64(),checksum=header.u64();const auto payload=bytes.substr(24);
    require(length==payload.size()&&bytes_hash(payload)==checksum,"Embedded HDR length/checksum mismatch");
    Reader reader{payload};const auto width=reader.u32(),height=reader.u32(),name_length=reader.u32();
    require(width<=EnvironmentLimits::dimension&&height<=EnvironmentLimits::dimension,"Embedded HDR dimension exceeds budget");
    dimensions(int(width),int(height));require(name_length<=4096&&name_length<=payload.size()-reader.offset,"Invalid embedded HDR label");
    std::string name(payload.substr(reader.offset,name_length));reader.offset+=name_length;
    const auto encoding=version==2?reader.u32():0u;require(encoding<=1,"Unsupported embedded HDR encoding");
    if(encoding==1) {
        const auto encoded=payload.substr(reader.offset);
        const auto result=load_environment_hdr(std::as_bytes(std::span(encoded.data(),encoded.size())),std::move(name));
        require(result->image().width==int(width)&&result->image().height==int(height),"Embedded HDR envelope/encoded dimensions mismatch");return result;
    }
    const auto count=std::size_t(width)*height;
    require(payload.size()-reader.offset==count*12,"Embedded HDR pixel byte count mismatch");
    Image<glm::vec3> image{int(width),int(height)};
    for(auto& pixel:image.pixels)for(int c=0;c<3;++c)pixel[c]=std::bit_cast<float>(reader.u32());
    return make_environment_map(std::move(image),std::move(name));
}
} // namespace emberframe::lab
