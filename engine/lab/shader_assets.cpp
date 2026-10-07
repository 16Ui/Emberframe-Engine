#include "shader_assets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace emberframe::lab {
namespace {
namespace fs=std::filesystem;
[[noreturn]] void shader_error(const std::string& text) {throw std::runtime_error(text);}
void shader_require(bool condition,const std::string& text) {if(!condition)shader_error(text);}
std::string utf8(const fs::path& path) {auto s=path.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
fs::path from_utf8(std::string_view text) {return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));}
std::string unique_suffix() {
    static std::atomic<std::uint64_t> serial{0};return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1));
}
class Sha256 {
    std::array<std::uint32_t,8> state_={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<unsigned char,64> block_{};
    std::uint64_t bytes_=0;
    std::size_t used_=0;
    void compress() {
        static constexpr std::array<std::uint32_t,64> k={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::array<std::uint32_t,64> w{};
        for(std::size_t i=0;i<16;++i)for(std::size_t j=0;j<4;++j)w[i]=(w[i]<<8)|block_[4*i+j];
        // SHA-256 的消息扩展/轮函数采用无符号 32 位模加；所有编码都明确规定字节序。
        for(std::size_t i=16;i<64;++i){const auto a=w[i-15],b=w[i-2];w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));}
        auto [a,b,c,d,e,f,g,h]=state_;
        for(std::size_t i=0;i<64;++i){const auto t1=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            const auto t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        const std::array<std::uint32_t,8> working={a,b,c,d,e,f,g,h};for(std::size_t i=0;i<8;++i)state_[i]+=working[i];
    }
public:
    void update(std::string_view text) {
        shader_require(text.size()<=std::numeric_limits<std::uint64_t>::max()-bytes_,"SHA-256 input too large");bytes_+=text.size();
        for(unsigned char c:text){block_[used_++]=c;if(used_==64){compress();used_=0;}}
    }
    std::string finish() {
        const auto bits=bytes_*8;block_[used_++]=0x80;
        if(used_>56){while(used_<64)block_[used_++]=0;compress();used_=0;}
        while(used_<56)block_[used_++]=0;
        for(int i=7;i>=0;--i)block_[used_++]=static_cast<unsigned char>(bits>>(i*8));
        compress();
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto v:state_)out<<std::setw(8)<<v;return out.str();
    }
};
void hash_field(Sha256& hash,std::string_view text) {hash.update(std::to_string(text.size()));hash.update(":");hash.update(text);}
std::string read_file(const fs::path& path,std::size_t limit=64*1024*1024) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);shader_require(bool(in),"Cannot open file: "+utf8(path));const auto size=in.tellg();
    shader_require(size>=0&&std::uint64_t(size)<=limit,"File exceeds size limit: "+utf8(path));std::string text(std::size_t(size),'\0');
    in.seekg(0);in.read(text.data(),std::streamsize(text.size()));shader_require(bool(in),"File read failed: "+utf8(path));return text;
}
std::string hash_file(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);shader_require(bool(in),"Cannot hash file: "+utf8(path));Sha256 hash;std::array<char,64*1024> block{};
    while(in){in.read(block.data(),std::streamsize(block.size()));hash.update({block.data(),std::size_t(in.gcount())});}
    shader_require(in.eof(),"File hashing failed: "+utf8(path));return hash.finish();
}
void write_file(const fs::path& path,std::string_view bytes) {
    fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary|std::ios::trunc);shader_require(bool(out),"Cannot create file: "+utf8(path));
    out.write(bytes.data(),std::streamsize(bytes.size()));out.close();shader_require(bool(out),"File write failed: "+utf8(path));
}
bool identifier(std::string_view text) {
    auto letter=[](char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    return !text.empty()&&letter(text[0])&&std::all_of(text.begin()+1,text.end(),[&](char c){return letter(c)||(c>='0'&&c<='9');});
}
fs::path relative_source(const fs::path& path) {
    shader_require(!path.empty()&&!path.is_absolute()&&!path.has_root_name(),"Shader path must be relative");const auto p=path.lexically_normal();
    for(const auto& part:p)shader_require(part!=".."&&part!=".","Shader/include path escapes source directory");
    const auto name=utf8(p);shader_require(name.find('\0')==std::string::npos&&name.find('\n')==std::string::npos&&name.find('\r')==std::string::npos&&name.find(':')==std::string::npos,"Invalid shader file name");return p;
}
const char* stage_name(ShaderStage stage) {
    switch(stage){case ShaderStage::vertex:return "vert";case ShaderStage::fragment:return "frag";case ShaderStage::compute:return "comp";
    case ShaderStage::geometry:return "geom";case ShaderStage::tess_control:return "tesc";case ShaderStage::tess_evaluation:return "tese";}shader_error("Invalid shader stage");
}

// Core opcodes/decorations from the vendored Vulkan SDK SPIR-V grammar. No Vulkan
// dependency is needed to parse the word stream or compare host descriptor layouts.
struct SpvType {std::uint32_t op=0;std::vector<std::uint32_t> operands;};
struct SpvDecorations {
    bool block=false,buffer_block=false,row_major=false;
    std::optional<std::uint32_t> set,binding,offset,array_stride,matrix_stride;
};
struct SpvVariable {std::uint32_t type=0,id=0,storage=0;};
struct SpvConstant {std::uint64_t value=0;bool specialization=false;};
struct TypeLayout {std::uint64_t size=0;std::string signature;bool complete=true;};
class ReflectionReader {
    std::span<const std::uint32_t> words_;
    std::uint32_t bound_=0;
    std::unordered_map<std::uint32_t,SpvType> types_;
    std::unordered_map<std::uint32_t,SpvDecorations> decorations_;
    std::map<std::pair<std::uint32_t,std::uint32_t>,SpvDecorations> member_decorations_;
    std::unordered_map<std::uint32_t,std::string> names_;
    std::unordered_map<std::uint32_t,SpvConstant> constants_;
    std::vector<SpvVariable> variables_;
    std::set<std::uint32_t> defined_,functions_;
    SpirvReflection result_;
    void id(std::uint32_t v) const {shader_require(v>0&&v<bound_,"SPIR-V ID outside header bound");}
    void define(std::uint32_t v){id(v);shader_require(defined_.insert(v).second,"Duplicate SPIR-V result ID");}
    void incomplete(std::string reason){result_.complete=false;if(std::find(result_.limitations.begin(),result_.limitations.end(),reason)==result_.limitations.end())result_.limitations.push_back(std::move(reason));}
    static std::pair<std::string,std::size_t> literal(std::span<const std::uint32_t> w,std::size_t start) {
        std::string text;
        for(std::size_t i=start;i<w.size();++i)for(int b=0;b<4;++b) {
            const auto c=char((w[i]>>(8*b))&255u);
            if(!c){for(int k=b+1;k<4;++k)shader_require(((w[i]>>(8*k))&255u)==0,"Invalid SPIR-V string padding");return {text,i+1};}text+=c;
        }
        shader_error("Unterminated SPIR-V string");
    }
    static void decorate(SpvDecorations& d,std::uint32_t decoration,std::span<const std::uint32_t> values) {
        auto assign=[&](std::optional<std::uint32_t>& slot){shader_require(values.size()==1,"Bad SPIR-V decoration arity");shader_require(!slot||*slot==values[0],"Conflicting SPIR-V decorations");slot=values[0];};
        switch(decoration){case 2:d.block=true;break;case 3:d.buffer_block=true;break;case 4:d.row_major=true;break;
        case 6:assign(d.array_stride);break;case 7:assign(d.matrix_stride);break;case 33:assign(d.binding);break;case 34:assign(d.set);break;case 35:assign(d.offset);break;default:break;}
    }
    const SpvType& type(std::uint32_t value) const {
        const auto found=types_.find(value);shader_require(found!=types_.end(),"Referenced SPIR-V type is undefined");return found->second;
    }
    TypeLayout layout(std::uint32_t value,const SpvDecorations& member={},unsigned depth=0) {
        shader_require(depth<64,"Cyclic/deep SPIR-V type graph");const auto& t=type(value);const auto& a=t.operands;TypeLayout l;
        auto child=[&](std::uint32_t v){return layout(v,{},depth+1);};
        switch(t.op) {
        case 20:l.size=4;l.signature="bool";break;
        case 21:case 22:shader_require(!a.empty()&&a[0]>0&&a[0]<=64&&a[0]%8==0,"Invalid SPIR-V scalar width");l.size=a[0]/8;l.signature=std::to_string(t.op)+":"+std::to_string(a[0]);if(t.op==21)l.signature+=':'+std::to_string(a.at(1));break;
        case 23:{shader_require(a.size()==2&&a[1]>=2&&a[1]<=4,"Invalid SPIR-V vector");auto s=child(a[0]);l.size=s.size*a[1];l.signature="vec"+std::to_string(a[1])+"<"+s.signature+">";l.complete=s.complete;break;}
        case 24:{shader_require(a.size()==2&&a[1]>=2&&a[1]<=4,"Invalid SPIR-V matrix");const auto& column=type(a[0]);shader_require(column.op==23,"Matrix column is not a vector");auto s=child(a[0]);
            if(!member.matrix_stride){l.complete=false;l.size=s.size*a[1];}else l.size=std::uint64_t(*member.matrix_stride)*(member.row_major?column.operands.at(1):a[1]);
            l.signature="mat"+std::to_string(a[1])+":"+std::to_string(member.matrix_stride.value_or(0))+":"+std::to_string(member.row_major)+"<"+s.signature+">";l.complete=l.complete&&s.complete;break;}
        case 28:case 29:{shader_require(a.size()==(t.op==28?2u:1u),"Invalid SPIR-V array");auto s=layout(a[0],member,depth+1);const auto& d=decorations_[value];
            std::uint64_t count=0;bool known=true;if(t.op==28){auto c=constants_.find(a[1]);known=c!=constants_.end()&&!c->second.specialization;if(c!=constants_.end())count=c->second.value;}
            const auto stride=d.array_stride.value_or(std::uint32_t(s.size));
            shader_require(count<=std::numeric_limits<std::uint32_t>::max()&&(!count||stride<=std::numeric_limits<std::uint32_t>::max()/count),"SPIR-V array byte size overflows supported layout range");
            l.complete=s.complete&&d.array_stride.has_value()&&known;l.size=std::uint64_t(stride)*count;
            l.signature=(t.op==29?"runtime":"array"+std::to_string(count))+":"+std::to_string(d.array_stride.value_or(0))+"<"+s.signature+">";break;}
        case 30:{l.signature="struct{";std::uint64_t previous_end=0;
            for(std::size_t i=0;i<a.size();++i){const auto d=member_decorations_[{value,std::uint32_t(i)}];auto s=layout(a[i],d,depth+1);
                const auto offset=d.offset.value_or(std::uint32_t(previous_end));l.complete=l.complete&&s.complete&&d.offset.has_value();
                shader_require(std::uint64_t(offset)>=previous_end,"Overlapping/out-of-order SPIR-V buffer members");previous_end=std::uint64_t(offset)+s.size;l.size=std::max(l.size,previous_end);
                l.signature+=std::to_string(offset)+":"+s.signature+";";}l.signature+='}';break;}
        case 25:{l.signature="image(";for(auto v:a)l.signature+=std::to_string(v)+",";
            if(!a.empty()){auto sampled=child(a[0]);l.signature="image<"+sampled.signature+">(";for(std::size_t i=1;i<a.size();++i)l.signature+=std::to_string(a[i])+",";}l.signature+=')';break;}
        case 26:l.signature="sampler";break;
        case 27:shader_require(a.size()==1,"Invalid sampled-image type");l.signature="sampled<"+child(a[0]).signature+">";break;
        case 5341:l.signature="acceleration-structure";break;
        default:l.complete=false;l.signature="unsupported-type-"+std::to_string(t.op);break;
        }
        shader_require(l.size<=std::numeric_limits<std::uint32_t>::max(),"SPIR-V reflected layout too large");return l;
    }
public:
    explicit ReflectionReader(std::span<const std::uint32_t> words):words_(words){}
    SpirvReflection read(ShaderStage expected,std::string_view entry) {
        shader_require(words_.size()>=5&&words_.size()<=16*1024*1024,"Invalid SPIR-V module size");
        shader_require(words_[0]==0x07230203,"Invalid SPIR-V magic or unsupported byte order");
        shader_require((words_[1]&0xff0000ffu)==0&&((words_[1]>>16)&255u)==1&&((words_[1]>>8)&255u)<=6,"Unsupported SPIR-V version");
        bound_=words_[3];shader_require(bound_>0&&bound_<=16*1024*1024&&words_[4]==0,"Invalid SPIR-V header");
        bool memory_model=false,in_function=false;
        for(std::size_t offset=5;offset<words_.size();) {
            const auto count=words_[offset]>>16,op=words_[offset]&65535;
            shader_require(count>0&&count<=words_.size()-offset,"Invalid SPIR-V instruction length");const auto w=words_.subspan(offset,count);
            auto arity=[&](std::size_t minimum){shader_require(w.size()>=minimum,"Truncated SPIR-V instruction");};
            if(op==14){shader_require(count==3&&!memory_model,"Invalid/duplicate SPIR-V memory model");memory_model=true;}
            else if(op==5){arity(3);id(w[1]);names_[w[1]]=literal(w,2).first;}
            else if(op==15){arity(4);id(w[2]);const auto [name,next]=literal(w,3);for(auto i=next;i<w.size();++i)id(w[i]);
                if(w[1]<=5)result_.entry_points.push_back({name,ShaderStage(w[1]),w[2]});else incomplete("Unsupported entry-point execution model");}
            else if((op>=19&&op<=33)||op==5341){arity(2);define(w[1]);types_.emplace(w[1],SpvType{op,{w.begin()+2,w.end()}});}
            else if(op==43||op==50){arity(4);id(w[1]);define(w[2]);const auto& scalar=type(w[1]);shader_require(scalar.op==21||scalar.op==22,"Non-scalar OpConstant");
                const auto width=scalar.operands.at(0);shader_require(width<=64&&(width<=32?w.size()==4:w.size()==5),"Invalid OpConstant width");
                std::uint64_t v=w[3];if(width>32)v|=std::uint64_t(w[4])<<32;constants_[w[2]]={v,op==50};}
            else if(op==59){arity(4);id(w[1]);define(w[2]);variables_.push_back({w[1],w[2],w[3]});}
            else if(op==71){arity(3);id(w[1]);decorate(decorations_[w[1]],w[2],w.subspan(3));}
            else if(op==72){arity(4);id(w[1]);decorate(member_decorations_[{w[1],w[2]}],w[3],w.subspan(4));}
            else if(op==73||op==74||op==75||op==332)incomplete("Decoration groups/ID decorations are not expanded by basic reflection");
            else if(op==54){shader_require(count==5&&!in_function,"Invalid SPIR-V function declaration");id(w[1]);define(w[2]);functions_.insert(w[2]);in_function=true;}
            else if(op==56){shader_require(count==1&&in_function,"Invalid SPIR-V function end");in_function=false;}
            offset+=count;
        }
        shader_require(memory_model&&!in_function,"Incomplete SPIR-V module/function");std::size_t matches=0;
        for(const auto& e:result_.entry_points){shader_require(functions_.contains(e.id),"SPIR-V entry point has no function");if(e.stage==expected&&e.name==entry)++matches;}
        shader_require(matches==1,"SPIR-V entry point/stage missing or ambiguous: "+std::string(entry));
        for(const auto& variable:variables_) {
            if(variable.storage!=0&&variable.storage!=2&&variable.storage!=9&&variable.storage!=12)continue;
            const auto& pointer=type(variable.type);shader_require(pointer.op==32&&pointer.operands.size()==2&&pointer.operands[0]==variable.storage,"Invalid resource pointer/storage class");
            auto pointee=pointer.operands[1];
            if(variable.storage==9) {
                shader_require(type(pointee).op==30,"Push constant is not a struct");const auto l=layout(pointee);
                if(!l.complete)incomplete("Push-constant layout has unsupported or missing explicit layout");
                std::uint32_t first=std::numeric_limits<std::uint32_t>::max();const auto& members=type(pointee).operands;
                for(std::size_t i=0;i<members.size();++i)first=std::min(first,member_decorations_[{pointee,std::uint32_t(i)}].offset.value_or(0));
                if(members.empty())first=0;
                shader_require(l.size>=first,"Invalid push-constant range");
                result_.push_constants.push_back({first,std::uint32_t(l.size-first),shader_content_hash(l.signature)});continue;
            }
            ShaderDescriptor descriptor;const auto& d=decorations_[variable.id];
            shader_require(d.set&&d.binding,"Resource lacks explicit descriptor set/binding");descriptor.set=*d.set;descriptor.binding=*d.binding;descriptor.name=names_[variable.id];
            std::uint64_t count=1;unsigned nesting=0;
            while(type(pointee).op==28||type(pointee).op==29) {
                shader_require(++nesting<64,"Cyclic descriptor array type");const auto& a=type(pointee);
                if(a.op==29){descriptor.runtime_array=true;count=0;incomplete("Runtime descriptor arrays require external layout/specialization information");}
                else {shader_require(a.operands.size()==2,"Invalid descriptor array type");const auto c=constants_.find(a.operands[1]);
                    if(c==constants_.end()||c->second.specialization){incomplete("Specialization-sized descriptor array");count=0;}
                    else {shader_require(c->second.value>0&&c->second.value<=65536&&count<=65536/c->second.value,"Descriptor array too large");count*=c->second.value;}}
                pointee=a.operands.at(0);
            }
            descriptor.count=std::uint32_t(count);const auto& t=type(pointee);const auto& td=decorations_[pointee];
            if(variable.storage==12)descriptor.kind=ShaderDescriptorKind::storage_buffer;
            else if(variable.storage==2&&td.buffer_block)descriptor.kind=ShaderDescriptorKind::storage_buffer;
            else if(variable.storage==2&&td.block)descriptor.kind=ShaderDescriptorKind::uniform_buffer;
            else if(variable.storage==0&&t.op==26)descriptor.kind=ShaderDescriptorKind::sampler;
            else if(variable.storage==0&&t.op==27)descriptor.kind=ShaderDescriptorKind::combined_image_sampler;
            else if(variable.storage==0&&t.op==5341)descriptor.kind=ShaderDescriptorKind::acceleration_structure;
            else if(variable.storage==0&&t.op==25) {
                shader_require(t.operands.size()>=7,"Truncated image type");const auto dim=t.operands[1],sampled=t.operands[5];
                if(dim==6)descriptor.kind=ShaderDescriptorKind::input_attachment;
                else if(dim==5)descriptor.kind=sampled==2?ShaderDescriptorKind::storage_texel_buffer:ShaderDescriptorKind::uniform_texel_buffer;
                else if(sampled==1)descriptor.kind=ShaderDescriptorKind::sampled_image;
                else if(sampled==2)descriptor.kind=ShaderDescriptorKind::storage_image;
            }
            if(descriptor.kind==ShaderDescriptorKind::unknown)incomplete("Unrecognized descriptor type/storage class");
            const auto l=layout(pointee);if(!l.complete)incomplete("Descriptor block contains unsupported or missing explicit layout");
            descriptor.type_layout_hash=shader_content_hash(l.signature);descriptor.minimum_buffer_bytes=std::uint32_t(l.size);result_.descriptors.push_back(std::move(descriptor));
        }
        std::sort(result_.descriptors.begin(),result_.descriptors.end(),[](const auto& a,const auto& b){return std::tie(a.set,a.binding)<std::tie(b.set,b.binding);});
        for(std::size_t i=1;i<result_.descriptors.size();++i)shader_require(result_.descriptors[i-1].set!=result_.descriptors[i].set||result_.descriptors[i-1].binding!=result_.descriptors[i].binding,"Duplicate descriptor binding in SPIR-V module");
        shader_require(result_.push_constants.size()<=1,"Multiple push-constant blocks are unsupported");return std::move(result_);
    }
};
}

std::string shader_content_hash(std::string_view bytes){Sha256 hash;hash.update(bytes);return hash.finish();}
SpirvReflection reflect_spirv(std::span<const std::uint32_t> words,ShaderStage stage,std::string_view entry){return ReflectionReader(words).read(stage,entry);}
SpirvModule read_spirv(const fs::path& path,ShaderStage stage,std::string_view entry) {
    const auto bytes=read_file(path);shader_require(bytes.size()%4==0,"SPIR-V byte length is not four-byte aligned");SpirvModule module;module.words.resize(bytes.size()/4);
    for(std::size_t i=0;i<module.words.size();++i)for(std::size_t b=0;b<4;++b)module.words[i]|=std::uint32_t(static_cast<unsigned char>(bytes[i*4+b]))<<(b*8);
    module.reflection=reflect_spirv(module.words,stage,entry);return module;
}
std::vector<ShaderSource> workbench_shader_sources() {
    return {{"mesh.vert",ShaderStage::vertex},{"fullscreen.vert",ShaderStage::vertex},{"forward.frag",ShaderStage::fragment},
            {"gbuffer.frag",ShaderStage::fragment},{"shadow.frag",ShaderStage::fragment},{"sky.frag",ShaderStage::fragment},
            {"deferred.frag",ShaderStage::fragment},{"post.frag",ShaderStage::fragment},{"cull.comp",ShaderStage::compute},{"object_cull.comp",ShaderStage::compute},
            {"gpu_shadow.vert",ShaderStage::vertex},{"gpu_shadow_check.comp",ShaderStage::compute},{"gpu_shadow_moments.comp",ShaderStage::compute},{"gpu_shadow_sat.comp",ShaderStage::compute},
            {"effects_bloom.comp",ShaderStage::compute},{"effects_compose.comp",ShaderStage::compute},{"effects_debug.comp",ShaderStage::compute},{"effects_filter.comp",ShaderStage::compute},{"effects_npr.comp",ShaderStage::compute},{"effects_occlusion.comp",ShaderStage::compute},{"effects_snapshot.comp",ShaderStage::compute},{"effects_temporal.comp",ShaderStage::compute},{"effects_trace.comp",ShaderStage::compute},{"effects_copy.comp",ShaderStage::compute},
            {"volume_inject.comp",ShaderStage::compute},{"volume_mip.comp",ShaderStage::compute},{"volume_propagate.comp",ShaderStage::compute},{"volume_rsm.comp",ShaderStage::compute},{"volume_trace.comp",ShaderStage::compute},{"volume_voxelize.comp",ShaderStage::compute},{"volume_sparse.comp",ShaderStage::compute}};
}
ShaderCompatibility compare_shader_layouts(const SpirvReflection& before,const SpirvReflection& after) {
    ShaderCompatibility result;auto mismatch=[&](std::string why){result.compatible=false;result.reasons.push_back(std::move(why));};
    if(!before.complete||!after.complete)mismatch("Basic descriptor reflection is incomplete");
    if(before.descriptors.size()!=after.descriptors.size())mismatch("Descriptor binding count changed");
    for(const auto& a:before.descriptors) {
        const auto b=std::find_if(after.descriptors.begin(),after.descriptors.end(),[&](const auto& d){return d.set==a.set&&d.binding==a.binding;});
        if(b==after.descriptors.end()){mismatch("Removed descriptor set="+std::to_string(a.set)+" binding="+std::to_string(a.binding));continue;}
        if(a.kind!=b->kind||a.count!=b->count||a.runtime_array!=b->runtime_array||a.type_layout_hash!=b->type_layout_hash||a.minimum_buffer_bytes!=b->minimum_buffer_bytes)
            mismatch("Changed descriptor type/count/layout set="+std::to_string(a.set)+" binding="+std::to_string(a.binding));
    }
    if(before.push_constants.size()!=after.push_constants.size())mismatch("Push-constant block count changed");
    else for(std::size_t i=0;i<before.push_constants.size();++i){const auto& a=before.push_constants[i];const auto& b=after.push_constants[i];
        if(a.offset!=b.offset||a.size!=b.size||a.type_layout_hash!=b.type_layout_hash)mismatch("Push-constant range/layout changed");}
    return result;
}
ShaderCompatibility compare_shader_versions(const ShaderVersion& before,const ShaderVersion& after) {
    ShaderCompatibility result;
    if(before.shaders.size()!=after.shaders.size()){result.compatible=false;result.reasons.push_back("Shader module set changed");}
    for(const auto& a:before.shaders) {
        const auto b=std::find_if(after.shaders.begin(),after.shaders.end(),[&](const auto& shader){return shader.source.file==a.source.file;});
        if(b==after.shaders.end()){result.compatible=false;result.reasons.push_back("Missing shader: "+utf8(a.source.file));continue;}
        if(a.source.stage!=b->source.stage||a.source.entry_point!=b->source.entry_point){result.compatible=false;result.reasons.push_back("Changed stage/entry point: "+utf8(a.source.file));}
        auto comparison=compare_shader_layouts(a.reflection,b->reflection);
        if(!comparison.compatible){result.compatible=false;for(auto& reason:comparison.reasons)result.reasons.push_back(utf8(a.source.file)+": "+reason);}
    }
    return result;
}

namespace {
std::string log_prefix(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);if(!in)return {};std::string text(1024*1024,'\0');in.read(text.data(),std::streamsize(text.size()));text.resize(std::size_t(in.gcount()));return text;
}
bool log_exceeded(const fs::path& a,const fs::path& b,std::size_t limit) {
    std::error_code error;const auto first=fs::file_size(a,error);if(error)return false;const auto second=fs::file_size(b,error);return !error&&(first>limit||second>limit||first>limit-std::min<std::uintmax_t>(second,limit));
}
#ifdef _WIN32
struct WinHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    WinHandle()=default;explicit WinHandle(HANDLE h):value(h){}
    ~WinHandle(){if(value!=INVALID_HANDLE_VALUE&&value!=nullptr)CloseHandle(value);}
    WinHandle(const WinHandle&)=delete;WinHandle& operator=(const WinHandle&)=delete;
};
std::wstring wide_utf8(std::string_view s) {
    shader_require(s.size()<=32767&&s.find('\0')==std::string_view::npos,"Invalid/oversized compiler argument");
    if(s.empty())return {};
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    shader_require(size>0,"Invalid UTF-8 compiler argument");std::wstring w(std::size_t(size),L'\0');
    shader_require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),w.data(),size)==size,"UTF-8 conversion failed");return w;
}
std::wstring quote_argument(std::wstring_view value) {
    // CreateProcessW 接收一个命令行字符串：反斜杠只在引号前/参数结尾加倍，避免路径和宏值被拆成其他参数。
    std::wstring out=L"\"";std::size_t slashes=0;
    for(auto c:value){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;}else{out.append(slashes,L'\\');out+=c;}slashes=0;}
    out.append(slashes*2,L'\\');out+=L'\"';return out;
}
class CacheLock {
    WinHandle handle_;
public:
    CacheLock(const fs::path& path,std::chrono::milliseconds timeout) {
        const auto until=std::chrono::steady_clock::now()+timeout;
        for(;;){handle_.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle_.value!=INVALID_HANDLE_VALUE)return;
            const auto error=GetLastError();
            shader_require(error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION,"Cannot open shader cache lock: Win32 "+std::to_string(error));
            shader_require(std::chrono::steady_clock::now()<until,"Timed out waiting for another shader cache build");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    }
};
ShaderDiagnostic run_compiler(const fs::path& executable,const std::vector<std::string>& arguments,const fs::path& cwd,
                              const fs::path& logbase,std::chrono::milliseconds timeout,std::size_t output_limit) {
    ShaderDiagnostic d;d.stdout_file=logbase;d.stdout_file+=".stdout.log";d.stderr_file=logbase;d.stderr_file+=".stderr.log";fs::create_directories(logbase.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    WinHandle out(CreateFileW(d.stdout_file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    WinHandle err(CreateFileW(d.stderr_file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    WinHandle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    shader_require(out.value!=INVALID_HANDLE_VALUE&&err.value!=INVALID_HANDLE_VALUE&&input.value!=INVALID_HANDLE_VALUE,"Cannot create compiler standard streams");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow=SW_HIDE;startup.StartupInfo.hStdOutput=out.value;startup.StartupInfo.hStdError=err.value;startup.StartupInfo.hStdInput=input.value;
    SIZE_T attribute_bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&attribute_bytes);
    std::vector<std::max_align_t> attribute_storage((attribute_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    startup.lpAttributeList=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    shader_require(InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_bytes)!=0,"Cannot initialize process handle whitelist");
    struct AttributeCleanup{PPROC_THREAD_ATTRIBUTE_LIST list;~AttributeCleanup(){DeleteProcThreadAttributeList(list);}}cleanup{startup.lpAttributeList};
    std::array<HANDLE,3> inherited={out.value,err.value,input.value};
    shader_require(UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),sizeof(inherited),nullptr,nullptr)!=0,"Cannot restrict inherited compiler handles");
    std::wstring command=quote_argument(executable.native());for(const auto& a:arguments){command+=L' ';command+=quote_argument(wide_utf8(a));}
    shader_require(command.size()<32767,"Compiler command line exceeds Windows limit");
    WinHandle job(CreateJobObjectW(nullptr,nullptr));shader_require(job.value!=nullptr,"Cannot create compiler process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    shader_require(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"Cannot configure compiler process job");
    PROCESS_INFORMATION info{};const auto start=std::chrono::steady_clock::now();
    const auto launched=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
                                      CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.c_str(),&startup.StartupInfo,&info);
    if(!launched){d.stderr_text="CreateProcessW failed: Win32 "+std::to_string(GetLastError());DWORD written=0;
        WriteFile(err.value,d.stderr_text.data(),DWORD(d.stderr_text.size()),&written,nullptr);FlushFileBuffers(err.value);return d;}
    WinHandle process(info.hProcess),thread(info.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)){const auto error=GetLastError();TerminateProcess(process.value,1);WaitForSingleObject(process.value,INFINITE);shader_error("Cannot contain compiler process: Win32 "+std::to_string(error));}
    if(ResumeThread(thread.value)==DWORD(-1)){TerminateJobObject(job.value,1);WaitForSingleObject(process.value,INFINITE);shader_error("Cannot start compiler thread");}
    for(;;) {
        const auto state=WaitForSingleObject(process.value,10);
        if(state==WAIT_OBJECT_0)break;
        if(state==WAIT_FAILED){TerminateJobObject(job.value,1);WaitForSingleObject(process.value,INFINITE);shader_error("Compiler process wait failed");}
        d.timed_out=std::chrono::steady_clock::now()-start>timeout;d.output_limit_exceeded=log_exceeded(d.stdout_file,d.stderr_file,output_limit);
        if(d.timed_out||d.output_limit_exceeded){TerminateJobObject(job.value,1);WaitForSingleObject(process.value,INFINITE);break;}
    }
    DWORD exit_code=0;shader_require(GetExitCodeProcess(process.value,&exit_code)!=0,"Cannot obtain compiler exit code");d.exit_code=int(exit_code);
    d.output_limit_exceeded=d.output_limit_exceeded||log_exceeded(d.stdout_file,d.stderr_file,output_limit);
    d.elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    FlushFileBuffers(out.value);FlushFileBuffers(err.value);d.stdout_text=log_prefix(d.stdout_file);d.stderr_text=log_prefix(d.stderr_file);return d;
}
#else
struct FileHandle {int fd=-1;~FileHandle(){if(fd>=0)::close(fd);}};
class CacheLock {
    FileHandle handle_;
public:
    CacheLock(const fs::path& path,std::chrono::milliseconds timeout) {
        handle_.fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);shader_require(handle_.fd>=0,"Cannot open shader cache lock");const auto until=std::chrono::steady_clock::now()+timeout;
        while(::flock(handle_.fd,LOCK_EX|LOCK_NB)!=0){shader_require(errno==EWOULDBLOCK||errno==EAGAIN,"Cannot lock shader cache");shader_require(std::chrono::steady_clock::now()<until,"Timed out waiting for shader cache lock");std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    }
};
ShaderDiagnostic run_compiler(const fs::path& executable,const std::vector<std::string>& arguments,const fs::path& cwd,
                              const fs::path& logbase,std::chrono::milliseconds timeout,std::size_t output_limit) {
    ShaderDiagnostic d;d.stdout_file=logbase;d.stdout_file+=".stdout.log";d.stderr_file=logbase;d.stderr_file+=".stderr.log";fs::create_directories(logbase.parent_path());
    FileHandle out{::open(d.stdout_file.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600)},err{::open(d.stderr_file.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600)},input{::open("/dev/null",O_RDONLY|O_CLOEXEC)};
    shader_require(out.fd>=0&&err.fd>=0&&input.fd>=0,"Cannot create compiler standard streams");
    std::vector<std::string> strings{executable.string()};strings.insert(strings.end(),arguments.begin(),arguments.end());std::vector<char*> argv;for(auto& s:strings)argv.push_back(s.data());argv.push_back(nullptr);
    const auto start=std::chrono::steady_clock::now();const auto pid=::fork();shader_require(pid>=0,"Cannot fork compiler process");
    if(pid==0){::setpgid(0,0);if(::chdir(cwd.c_str())!=0||::dup2(out.fd,STDOUT_FILENO)<0||::dup2(err.fd,STDERR_FILENO)<0||::dup2(input.fd,STDIN_FILENO)<0)_exit(126);::execv(executable.c_str(),argv.data());_exit(127);}
    ::setpgid(pid,pid);int status=0;
    for(;;){const auto waited=::waitpid(pid,&status,WNOHANG);if(waited==pid)break;if(waited<0&&errno!=EINTR){::kill(-pid,SIGKILL);::waitpid(pid,&status,0);shader_error("Compiler wait failed");}
        d.timed_out=std::chrono::steady_clock::now()-start>timeout;d.output_limit_exceeded=log_exceeded(d.stdout_file,d.stderr_file,output_limit);
        if(d.timed_out||d.output_limit_exceeded){::kill(-pid,SIGKILL);while(::waitpid(pid,&status,0)<0&&errno==EINTR){}break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    d.exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
    d.output_limit_exceeded=d.output_limit_exceeded||log_exceeded(d.stdout_file,d.stderr_file,output_limit);d.elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    d.stdout_text=log_prefix(d.stdout_file);d.stderr_text=log_prefix(d.stderr_file);return d;
}
#endif

using SourceSnapshot=std::map<std::string,std::string>;
std::string without_comments(std::string_view text) {
    std::string out(text);bool block=false,line=false,string=false;char quote=0;
    for(std::size_t i=0;i<out.size();++i){const char c=text[i],n=i+1<text.size()?text[i+1]:'\0';
        if(line){if(c=='\n')line=false;else out[i]=' ';continue;}
        if(block){if(c=='*'&&n=='/'){out[i]=out[i+1]=' ';++i;block=false;}else if(c!='\n')out[i]=' ';continue;}
        if(string){if(c=='\\'&&i+1<text.size()){++i;continue;}if(c==quote)string=false;continue;}
        if(c=='\"'||c=='\''){string=true;quote=c;continue;}
        if(c=='/'&&n=='*'){out[i]=out[i+1]=' ';++i;block=true;}else if(c=='/'&&n=='/'){out[i]=out[i+1]=' ';++i;line=true;}}
    return out;
}
std::vector<std::string> literal_includes(std::string_view text) {
    const auto clean=without_comments(text);std::istringstream lines(clean);std::string line;std::vector<std::string> includes;
    while(std::getline(lines,line)) {
        std::size_t i=0;auto whitespace=[&]{while(i<line.size()&&(line[i]==' '||line[i]=='\t'||line[i]=='\r'))++i;};whitespace();if(i==line.size()||line[i++]!='#')continue;whitespace();
        const auto begin=i;while(i<line.size()&&((line[i]>='a'&&line[i]<='z')||line[i]=='_'))++i;
        if(std::string_view(line).substr(begin,i-begin)!="include")continue;
        whitespace();
        shader_require(i<line.size()&&(line[i]=='\"'||line[i]=='<'),"Macro or malformed #include is unsupported by the source snapshot");
        const char close=line[i++]=='\"'?'\"':'>';const auto start=i;const auto end=line.find(close,start);shader_require(end!=std::string::npos&&end>start,"Malformed #include path");
        includes.push_back(line.substr(start,end-start));i=end+1;whitespace();shader_require(i==line.size(),"Unexpected tokens after #include");
    }
    return includes;
}
void reject_source_symlinks(const fs::path& root,const fs::path& relative) {
    auto current=root;for(const auto& part:relative){current/=part;shader_require(!fs::is_symlink(fs::symlink_status(current)),"Symlinked shader inputs are unsupported: "+utf8(current));}
}
SourceSnapshot snapshot_sources(const ShaderLibraryConfig& config) {
    SourceSnapshot snapshot;std::vector<fs::path> pending;for(const auto& source:config.sources)pending.push_back(source.file);
    shader_require(fs::is_directory(config.source_dir),"Shader source directory does not exist");
    for(fs::recursive_directory_iterator it(config.source_dir),end;it!=end;++it) {
        shader_require(it.depth()<64,"Shader source directory nesting too deep");
        if(it->is_symlink()){it.disable_recursion_pending();continue;}
        if(it->is_regular_file()){const auto extension=it->path().extension();if(extension==".glsl"||extension==".glslinclude")pending.push_back(it->path().lexically_relative(config.source_dir));}
    }
    std::size_t total=0;
    while(!pending.empty()) {
        const auto relative=relative_source(pending.back());pending.pop_back();const auto key=utf8(relative);if(snapshot.contains(key))continue;
        shader_require(snapshot.size()<4096,"Shader snapshot exceeds 4096 files");reject_source_symlinks(config.source_dir,relative);
        auto content=read_file(config.source_dir/relative,64*1024*1024-total);total+=content.size();
        for(const auto& include:literal_includes(content)) {
            const auto name=from_utf8(include);shader_require(!name.is_absolute()&&!name.has_root_name(),"Absolute shader #include is not allowed");
            auto resolved=relative_source(relative.parent_path()/name);
            if(!fs::is_regular_file(config.source_dir/resolved))resolved=relative_source(name);
            shader_require(fs::is_regular_file(config.source_dir/resolved),"Shader include not found: "+include+" from "+key);pending.push_back(resolved);
        }
        snapshot.emplace(key,std::move(content));
    }
    return snapshot;
}
std::vector<std::string> compiler_options(const ShaderCompileOptions& options) {
    const std::set<std::string> vulkan={"vulkan1.0","vulkan1.1","vulkan1.2","vulkan1.3","vulkan1.4"};
    const std::set<std::string> spirv={"spirv1.0","spirv1.1","spirv1.2","spirv1.3","spirv1.4","spirv1.5","spirv1.6"};
    shader_require(vulkan.contains(options.vulkan_target)&&spirv.contains(options.spirv_target),"Unsupported Vulkan/SPIR-V target option");
    shader_require(!(options.optimize_size&&options.disable_optimization),"Conflicting shader optimization options");
    std::vector<std::string> args={"-V","--target-env",options.vulkan_target,"--target-env",options.spirv_target};
    if(options.debug_info)args.push_back("-g");
    if(options.optimize_size)args.push_back("-Os");
    if(options.disable_optimization)args.push_back("-Od");
    std::set<std::string> names;
    for(const auto& [name,value]:options.defines){shader_require(identifier(name)&&names.insert(name).second,"Invalid/duplicate shader macro name");
        shader_require(value.find_first_of("\r\n")==std::string::npos&&value.find('\0')==std::string::npos,"Shader macro contains line break/NUL");args.push_back("-D"+name+"="+value);}
    return args;
}
std::string snapshot_hash(const SourceSnapshot& snapshot) {
    Sha256 hash;hash_field(hash,"EmberFrame shader source snapshot 1");for(const auto& [path,text]:snapshot){hash_field(hash,path);hash_field(hash,text);}return hash.finish();
}
fs::path output_file(const ShaderSource& source){auto path=source.file;path+=".spv";return path;}
void validate_config(ShaderLibraryConfig& config) {
    shader_require(!config.source_dir.empty()&&!config.cache_dir.empty()&&!config.glslang.empty(),"Shader library requires source/cache/compiler paths");
    config.source_dir=fs::absolute(config.source_dir).lexically_normal();config.cache_dir=fs::absolute(config.cache_dir).lexically_normal();config.glslang=fs::absolute(config.glslang).lexically_normal();
    // 防止自己的缓存快照再次被纳入源码递归扫描，也不允许缓存覆盖源码目录。
    const auto child=config.cache_dir.lexically_relative(config.source_dir),parent=config.source_dir.lexically_relative(config.cache_dir);
    auto inside=[](const fs::path& path){return !path.empty()&&*path.begin()!="..";};
    shader_require(!inside(child)&&!inside(parent),"Shader cache and source directories must not contain each other");
    shader_require(config.compiler_timeout.count()>0&&config.compiler_timeout<=std::chrono::minutes(10)&&config.max_log_bytes>0,"Invalid compiler timeout/log limit");
    shader_require(!config.sources.empty()&&config.sources.size()<=256,"Shader source list must have 1..256 entries");
    std::set<std::string> unique;
    for(auto& source:config.sources){source.file=relative_source(source.file);auto key=utf8(source.file);for(auto& c:key)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
        shader_require(!key.starts_with("source/"),"The source/ output prefix is reserved for immutable source snapshots");
        shader_require(unique.insert(key).second,"Duplicate shader output path");shader_require(identifier(source.entry_point),"Invalid shader entry point identifier");stage_name(source.stage);}
    compiler_options(config.options);
}
void write_manifest(const fs::path& directory,const ShaderVersion& version) {
    std::ostringstream out;out<<"EmberFrameShaderCache 1\n"<<version.id<<'\n'<<version.source_hash<<'\n'<<version.compiler_hash<<'\n'<<version.shaders.size()<<'\n';
    for(const auto& shader:version.shaders)out<<std::quoted(utf8(shader.source.file))<<' '<<std::uint32_t(shader.source.stage)<<' '<<std::quoted(shader.source.entry_point)<<' '<<shader.spirv_hash<<'\n';
    write_file(directory/"manifest.txt",out.str());
}
std::shared_ptr<ShaderVersion> read_bundle(const fs::path& directory,const std::vector<ShaderSource>& sources,const std::string& expected_id={},bool baseline=false) {
    auto version=std::make_shared<ShaderVersion>();version->directory=fs::absolute(directory).lexically_normal();
    std::map<std::string,std::string> expected_hashes;
    if(!baseline) {
        std::istringstream manifest(read_file(directory/"manifest.txt",1024*1024));std::string magic;unsigned format=0;std::size_t count=0;
        manifest>>magic>>format>>version->id>>version->source_hash>>version->compiler_hash>>count;
        shader_require(manifest&&magic=="EmberFrameShaderCache"&&format==1&&version->id==expected_id&&count==sources.size(),"Invalid/incomplete shader cache manifest");
        for(std::size_t i=0;i<count;++i){std::string file,entry,hash;std::uint32_t stage=0;manifest>>std::quoted(file)>>stage>>std::quoted(entry)>>hash;
            shader_require(manifest&&file==utf8(sources[i].file)&&stage==std::uint32_t(sources[i].stage)&&entry==sources[i].entry_point&&hash.size()==64,"Shader cache manifest does not match requested module set");expected_hashes.emplace(file,hash);}
        manifest>>std::ws;shader_require(manifest.eof(),"Unexpected shader cache manifest records");
    }
    Sha256 fingerprint;hash_field(fingerprint,"EmberFrame external shader baseline 1");
    for(const auto& source:sources) {
        CompiledShader shader;shader.source=source;shader.spirv_file=version->directory/output_file(source);shader.spirv_hash=hash_file(shader.spirv_file);
        if(!baseline)shader_require(shader.spirv_hash==expected_hashes.at(utf8(source.file)),"Corrupt shader cache SPIR-V checksum: "+utf8(source.file));
        shader.reflection=read_spirv(shader.spirv_file,source.stage,source.entry_point).reflection;
        hash_field(fingerprint,utf8(source.file));hash_field(fingerprint,shader.spirv_hash);version->shaders.push_back(std::move(shader));
    }
    if(baseline)version->id="baseline-"+fingerprint.finish();
    return version;
}
}

struct ShaderLibrary::Impl {
    ShaderLibraryConfig config;
    std::mutex build_mutex;
    mutable std::mutex active_mutex;
    std::shared_ptr<const ShaderVersion> current,prior;
    std::unordered_map<std::string,std::shared_ptr<const ShaderVersion>> known;
    std::string compiler_hash,compiler_version;
    explicit Impl(ShaderLibraryConfig c):config(std::move(c)){validate_config(config);}
    void register_result(ShaderBuildResult& result) {
        std::lock_guard lock(active_mutex);
        const auto [found,inserted]=known.emplace(result.version->id,result.version);
        if(!inserted)result.version=found->second;
        if(current)result.compatibility=compare_shader_versions(*current,*result.version);
        else for(const auto& shader:result.version->shaders)if(!shader.reflection.complete){result.compatibility.compatible=false;result.compatibility.reasons.push_back("Incomplete reflection: "+utf8(shader.source.file));}
    }
};
ShaderLibrary::ShaderLibrary(ShaderLibraryConfig config):impl_(std::make_unique<Impl>(std::move(config))){}
ShaderLibrary::ShaderLibrary(fs::path source_dir,fs::path cache_dir,fs::path glslang)
    :ShaderLibrary([&]{ShaderLibraryConfig config;config.source_dir=std::move(source_dir);config.cache_dir=std::move(cache_dir);config.glslang=std::move(glslang);return config;}()){}
ShaderLibrary::~ShaderLibrary()=default;
std::shared_ptr<const ShaderVersion> ShaderLibrary::active() const {std::lock_guard lock(impl_->active_mutex);return impl_->current;}
std::shared_ptr<const ShaderVersion> ShaderLibrary::previous() const {std::lock_guard lock(impl_->active_mutex);return impl_->prior;}
void ShaderLibrary::activate(const std::shared_ptr<const ShaderVersion>& version) {
    shader_require(bool(version),"Cannot activate a null shader version");std::lock_guard lock(impl_->active_mutex);
    const auto found=impl_->known.find(version->id);shader_require(found!=impl_->known.end()&&found->second==version,"Unknown/foreign shader version");
    if(impl_->current==version)return;
    for(const auto& shader:version->shaders)shader_require(shader.reflection.complete,"Cannot activate incomplete shader reflection");
    if(impl_->current){const auto comparison=compare_shader_versions(*impl_->current,*version);shader_require(comparison.compatible,"Shader layout is incompatible with active pipelines" );}
    impl_->prior=impl_->current;impl_->current=version;
}
void ShaderLibrary::adopt_baseline(const fs::path& shader_dir) {
    auto version=read_bundle(shader_dir,impl_->config.sources,{},true);
    for(const auto& shader:version->shaders)shader_require(shader.reflection.complete,"Baseline shader reflection is incomplete");
    std::lock_guard lock(impl_->active_mutex);shader_require(!impl_->current,"Shader baseline can only be adopted before activation");impl_->known[version->id]=version;impl_->current=std::move(version);
}
ShaderBuildResult ShaderLibrary::build() {
    std::lock_guard serial(impl_->build_mutex);const auto& c=impl_->config;ShaderBuildResult result;fs::path staging,published;std::string key;
    try {
        fs::create_directories(c.cache_dir);CacheLock directory_lock(c.cache_dir/"build.lock",c.compiler_timeout);
        staging=c.cache_dir/"staging"/unique_suffix();shader_require(fs::create_directories(staging),"Cannot create unique shader build staging directory");result.log_directory=staging/"logs";
        fs::create_directories(result.log_directory);
        shader_require(fs::is_regular_file(c.glslang),"glslang executable is missing: "+utf8(c.glslang));
        const auto binary_hash=hash_file(c.glslang);
        if(impl_->compiler_hash!=binary_hash) {
            auto diagnostic=run_compiler(c.glslang,{"--version"},staging,result.log_directory/"compiler-version",c.compiler_timeout,c.max_log_bytes);
            result.diagnostics.push_back(diagnostic);shader_require(diagnostic.exit_code==0&&!diagnostic.timed_out&&!diagnostic.output_limit_exceeded,"Cannot query glslang version");
            impl_->compiler_hash=binary_hash;impl_->compiler_version=diagnostic.stdout_text+"\n"+diagnostic.stderr_text;
        }
        const auto snapshot=snapshot_sources(c);const auto sources_hash=snapshot_hash(snapshot);const auto options=compiler_options(c.options);
        Sha256 identity;hash_field(identity,"EmberFrameShaderCache 1");hash_field(identity,sources_hash);hash_field(identity,binary_hash);hash_field(identity,utf8(c.glslang));hash_field(identity,impl_->compiler_version);
        for(const auto& option:options)hash_field(identity,option);
        for(const auto& source:c.sources){hash_field(identity,utf8(source.file));hash_field(identity,stage_name(source.stage));hash_field(identity,source.entry_point);}
        key=identity.finish();published=c.cache_dir/"versions"/key;
        if(fs::exists(published)) {
            auto version=read_bundle(published,c.sources,key);shader_require(version->source_hash==sources_hash&&version->compiler_hash==binary_hash,"Shader manifest fingerprint mismatch");
            result.version=std::move(version);result.success=true;result.cache_hit=true;
            write_file(staging/"cache-hit.txt","Validated immutable shader version "+key+"\n");
            const auto hitdir=c.cache_dir/"cache_hits"/(key+"-"+unique_suffix());fs::create_directories(hitdir.parent_path());
            fs::rename(staging,hitdir);
            for(auto& d:result.diagnostics){d.stdout_file=hitdir/d.stdout_file.lexically_relative(staging);d.stderr_file=hitdir/d.stderr_file.lexically_relative(staging);}
            result.log_directory=hitdir/"logs";staging.clear();impl_->register_result(result);return result;
        }
        const auto frozen=staging/"source";
        for(const auto& [path,text]:snapshot)write_file(frozen/from_utf8(path),text);
        write_file(staging/"compiler-version.txt",impl_->compiler_version);
        auto version=std::make_shared<ShaderVersion>();version->id=key;version->source_hash=sources_hash;version->compiler_hash=binary_hash;version->directory=published;
        bool failed=false;
        for(const auto& source:c.sources) {
            auto args=options;args.push_back("-I"+utf8(frozen));args.push_back("-S");args.push_back(stage_name(source.stage));args.push_back("-e");args.push_back(source.entry_point);
            args.push_back(utf8(frozen/source.file));args.push_back("-o");const auto output=staging/output_file(source);args.push_back(utf8(output));fs::create_directories(output.parent_path());
            std::ostringstream invocation;invocation<<"Executable: "<<utf8(c.glslang)<<'\n';for(const auto& arg:args)invocation<<std::quoted(arg)<<'\n';write_file(result.log_directory/(utf8(source.file)+".argv.txt"),invocation.str());
            auto diagnostic=run_compiler(c.glslang,args,frozen,result.log_directory/source.file,c.compiler_timeout,c.max_log_bytes);diagnostic.source=source.file;++result.compiled_files;
            const bool success=diagnostic.exit_code==0&&!diagnostic.timed_out&&!diagnostic.output_limit_exceeded;result.diagnostics.push_back(std::move(diagnostic));
            if(!success){failed=true;continue;}
            try {CompiledShader shader;shader.source=source;shader.spirv_file=published/output_file(source);shader.spirv_hash=hash_file(output);shader.reflection=read_spirv(output,source.stage,source.entry_point).reflection;version->shaders.push_back(std::move(shader));}
            catch(const std::exception& error){failed=true;write_file(result.log_directory/(utf8(source.file)+".validation.log"),error.what());result.error+=utf8(source.file)+": "+error.what()+"\n";}
        }
        shader_require(!failed,"Shader compilation/validation failed; previous active version was retained. "+result.error);
        shader_require(hash_file(c.glslang)==binary_hash,"Compiler executable changed while compiling this bundle");
        write_manifest(staging,*version);fs::create_directories(published.parent_path());fs::rename(staging,published);
        for(auto& d:result.diagnostics){d.stdout_file=published/d.stdout_file.lexically_relative(staging);d.stderr_file=published/d.stderr_file.lexically_relative(staging);}
        result.log_directory=published/"logs";staging.clear();result.version=std::move(version);result.success=true;impl_->register_result(result);
    } catch(const std::exception& error) {
        result.success=false;result.version.reset();result.error=error.what();
        if(!staging.empty())try {
            write_file(staging/"failure.txt",result.error);const auto failed=c.cache_dir/"failures"/((key.empty()?"preflight":key)+"-"+unique_suffix());fs::create_directories(failed.parent_path());fs::rename(staging,failed);
            for(auto& d:result.diagnostics){d.stdout_file=failed/d.stdout_file.lexically_relative(staging);d.stderr_file=failed/d.stderr_file.lexically_relative(staging);}
            result.log_directory=failed/"logs";
        } catch(const std::exception& log_error){result.error+="; retaining staging at "+utf8(staging)+": "+log_error.what();}
    }
    return result;
}
} // namespace emberframe::lab
