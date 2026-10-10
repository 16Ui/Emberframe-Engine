#include "frame_profile.h"
#include <fstream>
#include <iomanip>
#include <sstream>

namespace emberframe::lab {
namespace {
std::string json_quote(std::string_view value) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:value) {
        if(c=='"'||c=='\\')out<<'\\'<<char(c);
        else if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
        else out<<char(c);
    }
    out<<'"';return out.str();
}
}
void decode_frame_timestamps(FrameProfile& frame,std::span<const std::uint64_t> values,
                             std::uint32_t bits,double period) {
    if(!frame.completed)throw std::logic_error("Cannot decode timestamps of an unfinished submission");
    if(bits==0||bits>64||!std::isfinite(period)||period<=0||values.size()!=2+2*frame.passes.size())
        throw std::invalid_argument("Invalid frame timestamp payload");
    const auto mask=bits==64?UINT64_MAX:(std::uint64_t(1)<<bits)-1;
    auto milliseconds=[&](std::size_t index){return double((values[index+1]-values[index])&mask)*period*1e-6;};
    frame.gpu_ms=milliseconds(0);
    for(std::size_t i=0;i<frame.passes.size();++i)frame.passes[i].gpu_ms=milliseconds(2+i*2);
    frame.timestamps_available=true;
}
std::string frame_profile_json(const FrameProfile& frame) {
    auto valid=[](double v){if(!std::isfinite(v)||v<0)throw std::invalid_argument("Invalid CPU profile duration");return v;};
    std::ostringstream out;out<<std::setprecision(12);
    out<<"{\n  \"schema\":\"emberframe.frame-profile.v1\",\n  \"serial\":"<<frame.serial
       <<",\n  \"scene_revision\":"<<frame.scene_revision<<",\n  \"completed\":"<<(frame.completed?"true":"false")
       <<",\n  \"width\":"<<frame.width<<",\n  \"height\":"<<frame.height
       <<",\n  \"bounds_cached\":"<<(frame.bounds_cached?"true":"false")
       <<",\n  \"gpu_ms\":";
    if(frame.timestamps_available&&frame.gpu_ms>=0&&std::isfinite(frame.gpu_ms))out<<frame.gpu_ms;else out<<"null";
    out<<",\n  \"cpu_ms\":{\"fence_wait\":"<<valid(frame.cpu_wait_ms)<<",\"ui\":"<<valid(frame.cpu_ui_ms)
       <<",\"prepare\":"<<valid(frame.cpu_prepare_ms)<<",\"geometry\":"<<valid(frame.cpu_geometry_ms)
       <<",\"acquire\":"<<valid(frame.cpu_acquire_ms)<<",\"record\":"<<valid(frame.cpu_record_ms)
       <<",\"submit_present\":"<<valid(frame.cpu_submit_ms)<<",\"total\":"<<valid(frame.cpu_total_ms)<<"},"
       <<"\n  \"allocated_bytes\":"<<frame.allocated_bytes<<",\n  \"upload_bytes\":"<<frame.upload_bytes
       <<",\n  \"gpu_scope_semantics\":\"inclusive; nested scopes must not be summed\",\n  \"passes\":[";
    for(std::size_t i=0;i<frame.passes.size();++i){const auto& pass=frame.passes[i];out<<(i?",\n":"\n")
        <<"    {\"name\":"<<json_quote(pass.name)<<",\"depth\":"<<pass.depth<<",\"cpu_record_ms\":"<<valid(pass.cpu_record_ms)<<",\"gpu_ms\":";
        if(frame.timestamps_available&&pass.gpu_ms>=0&&std::isfinite(pass.gpu_ms))out<<pass.gpu_ms;else out<<"null";out<<'}';}
    out<<"\n  ]\n}\n";return out.str();
}
void save_frame_profile(const FrameProfile& frame,const std::filesystem::path& path) {
    const auto text=frame_profile_json(frame);
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);if(!out||!out.write(text.data(),std::streamsize(text.size())))
        throw std::runtime_error("Could not save frame profile");
}
TestResults test_frame_profile() {
    TestResults result;
    auto test=[&](const char* name,const auto& f){try{result.push_back({name,f(),"Completed serial, wraparound and nested inclusive semantics"});}
        catch(const std::exception& e){result.push_back({name,false,e.what()});}};
    test("Frame profile timestamp wrap",[]{FrameProfile f;f.completed=true;f.passes={{"outer",0},{"child",1}};
        const std::uint64_t data[]{250,4,251,3,252,1};decode_frame_timestamps(f,data,8,1000000);
        return f.gpu_ms==10&&f.passes[0].gpu_ms==8&&f.passes[1].gpu_ms==5;});
    test("Frame profile rejects uncompleted data",[]{try{FrameProfile f;const std::uint64_t data[]{0,1};decode_frame_timestamps(f,data,64,1);}catch(const std::logic_error&){return true;}return false;});
    test("Frame profile absent timestamp is null",[]{FrameProfile f;f.passes={{"A\"\\\n",0}};const auto text=frame_profile_json(f);
        return text.find("\"gpu_ms\":null")!=std::string::npos&&text.find("A\\\"\\\\\\u000a")!=std::string::npos;});
    test("Frame profile checks query count",[]{try{FrameProfile f;f.completed=true;f.passes={{"x",0}};const std::uint64_t data[]{0,1};decode_frame_timestamps(f,data,64,1);}catch(const std::invalid_argument&){return true;}return false;});
    test("Frame profile rejects nonfinite CPU durations",[]{try{FrameProfile f;f.cpu_prepare_ms=std::numeric_limits<double>::quiet_NaN();frame_profile_json(f);}catch(const std::invalid_argument&){return true;}return false;});
    return result;
}
} // namespace emberframe::lab
