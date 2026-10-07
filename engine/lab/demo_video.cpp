// 演示导出工具：把工作台实际生成的 BMP 序列编码为 MP4，不依赖 ffmpeg。
// Media Foundation API 依据 Microsoft Sink Writer 文档；这里自行解析 BMP/转换 NV12。
// 视频供展示，不是实时录屏/帧率基准；原始 PFM/BMP 才用于数值对照。
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <propvarutil.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
constexpr DWORD first_video_stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD media_source=static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);
void checked(HRESULT value,const char* operation){if(FAILED(value)){std::ostringstream text;text<<operation<<" HRESULT=0x"<<std::hex<<unsigned(value);throw std::runtime_error(text.str());}}
struct Runtime {bool com=false,mf=false;Runtime(){checked(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM startup");com=true;auto hr=MFStartup(MF_VERSION);if(FAILED(hr)){CoUninitialize();com=false;checked(hr,"MF startup");}mf=true;}~Runtime(){if(mf)MFShutdown();if(com)CoUninitialize();}};
struct Bitmap {unsigned width=0,height=0;std::vector<unsigned char> bgra;};
Bitmap bitmap(const fs::path& path){
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot read BMP");auto bytes=file.tellg();if(bytes<54||bytes>256*1024*1024)throw std::runtime_error("BMP size outside bounds");
    file.seekg(0);std::vector<unsigned char> data(static_cast<std::size_t>(bytes));file.read(reinterpret_cast<char*>(data.data()),bytes);if(!file)throw std::runtime_error("BMP truncated");
    auto u16=[&](std::size_t n){return unsigned(data.at(n))|(unsigned(data.at(n+1))<<8);};auto u32=[&](std::size_t n){return u16(n)|(u16(n+2)<<16);};
    int width=static_cast<int>(u32(18)),height=static_cast<int>(u32(22));
    if(data[0]!='B'||data[1]!='M'||u32(14)<40||u16(26)!=1||u16(28)!=24||u32(30)!=0||width<2||width>8192||height==0||height<-8192||height>8192)throw std::runtime_error("Requires uncompressed 24-bit BMP, <=8192 per axis");
    Bitmap out;out.width=unsigned(width);out.height=unsigned(std::abs(height));if(out.width%2||out.height%2||std::uint64_t(out.width)*out.height>16777216)throw std::runtime_error("MP4 needs even dimensions, <=16M pixels");
    const auto stride=(std::size_t(width)*3+3)&~std::size_t(3),offset=std::size_t(u32(10));if(offset<54||offset>data.size()||stride*out.height>data.size()-offset)throw std::runtime_error("BMP pixel range invalid");
    out.bgra.resize(std::size_t(out.width)*out.height*4);
    for(unsigned y=0;y<out.height;++y)for(unsigned x=0;x<out.width;++x){auto src=offset+(height>0?out.height-1-y:y)*stride+x*3;auto dst=(std::size_t(y)*out.width+x)*4;out.bgra[dst]=data[src];out.bgra[dst+1]=data[src+1];out.bgra[dst+2]=data[src+2];out.bgra[dst+3]=255;}
    return out;
}
std::vector<unsigned char> nv12(const Bitmap& image){
    const auto pixels=std::size_t(image.width)*image.height;std::vector<unsigned char> out(pixels*3/2);
    auto byte=[](double value){return static_cast<unsigned char>(std::clamp(std::lround(value),0l,255l));};
    auto rgb=[&](unsigned x,unsigned y){auto p=(std::size_t(y)*image.width+x)*4;return std::array<double,3>{image.bgra[p+2]/255.,image.bgra[p+1]/255.,image.bgra[p]/255.};};
    // BT.709 limited-range YUV；输入 BMP 已是显示编码值，不再做曝光或 tone mapping。
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){auto c=rgb(x,y);out[std::size_t(y)*image.width+x]=byte(16+219*(.2126*c[0]+.7152*c[1]+.0722*c[2]));}
    for(unsigned y=0;y<image.height;y+=2)for(unsigned x=0;x<image.width;x+=2){std::array<double,3> c{};for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){auto v=rgb(x+dx,y+dy);for(int k=0;k<3;++k)c[k]+=v[k]*.25;}auto p=pixels+std::size_t(y/2)*image.width+x;out[p]=byte(128+224*(-.114572*c[0]-.385428*c[1]+.5*c[2]));out[p+1]=byte(128+224*(.5*c[0]-.454153*c[1]-.045847*c[2]));}
    return out;
}
// 同索引的真实帧左右并排，便于观察 TAA、CSM 和 LOD；不重复静态帧冒充运动。
Bitmap side_by_side(const Bitmap& left,const Bitmap& right){
    if(left.width!=right.width||left.height!=right.height||left.width>4096)throw std::runtime_error("Comparison frame sizes differ or exceed bounds");
    Bitmap out;out.width=left.width*2;out.height=left.height;out.bgra.resize(std::size_t(out.width)*out.height*4);
    const auto row=std::size_t(left.width)*4;
    for(unsigned y=0;y<out.height;++y){std::copy_n(left.bgra.data()+y*row,row,out.bgra.data()+y*row*2);std::copy_n(right.bgra.data()+y*row,row,out.bgra.data()+y*row*2+row);}return out;
}
std::vector<fs::path> frame_files(const fs::path& directory){
    std::vector<fs::path> frames;for(const auto& entry:fs::directory_iterator(directory)){auto suffix=entry.path().extension().wstring();std::transform(suffix.begin(),suffix.end(),suffix.begin(),::towlower);if(entry.is_regular_file()&&suffix==L".bmp")frames.push_back(entry.path());}
    std::sort(frames.begin(),frames.end());if(frames.empty()||frames.size()>4096)throw std::runtime_error("Requires 1..4096 BMP frames");return frames;
}
ComPtr<IMFMediaType> media_type(const GUID& format,unsigned w,unsigned h,unsigned fps){ComPtr<IMFMediaType> type;checked(MFCreateMediaType(&type),"media type");checked(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"major type");checked(type->SetGUID(MF_MT_SUBTYPE,format),"subtype");checked(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive),"progressive");checked(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,w,h),"size");checked(MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,fps,1),"rate");checked(MFSetAttributeRatio(type.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1),"aspect");checked(type->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709),"YUV matrix");checked(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235),"video range");return type;}
void verify(const fs::path& path,unsigned w,unsigned h,LONGLONG expected,LONGLONG step){
    ComPtr<IMFSourceReader> reader;checked(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"verify reader");ComPtr<IMFMediaType> native;checked(reader->GetNativeMediaType(first_video_stream,0,&native),"verify native type");unsigned rw=0,rh=0;checked(MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&rw,&rh),"verify dimensions");if(rw!=w||rh!=h)throw std::runtime_error("Encoded dimensions differ");
    PROPVARIANT duration;PropVariantInit(&duration);checked(reader->GetPresentationAttribute(media_source,MF_PD_DURATION,&duration),"verify duration");auto measured=duration.vt==VT_UI8?duration.uhVal.QuadPart:0;PropVariantClear(&duration);if(std::abs(static_cast<LONGLONG>(measured)-expected)>step*2)throw std::runtime_error("Encoded duration differs");
    // 解码只指定像素格式，不要求把导出帧率再次转换为固定 30 FPS。
    ComPtr<IMFMediaType> decoded;checked(MFCreateMediaType(&decoded),"verify media type");checked(decoded->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"verify major type");checked(decoded->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"verify subtype");checked(reader->SetCurrentMediaType(first_video_stream,nullptr,decoded.Get()),"verify decode type");
    auto read=[&](LONGLONG target){for(int n=0;n<240;++n){DWORD flags=0;LONGLONG time=0;ComPtr<IMFSample> sample;checked(reader->ReadSample(first_video_stream,0,nullptr,&flags,&time,&sample),"verify decode");if(flags&MF_SOURCE_READERF_ERROR)throw std::runtime_error("Decoder signaled an error");if(sample&&time>=target-step){DWORD length=0;checked(sample->GetTotalLength(&length),"verify sample length");if(length<static_cast<DWORD>(std::size_t(w)*h*3/2))throw std::runtime_error("Decoded frame empty");return;}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}throw std::runtime_error("Cannot decode requested video frame");};
    read(0);PROPVARIANT position;PropVariantInit(&position);position.vt=VT_I8;position.hVal.QuadPart=std::max<LONGLONG>(0,expected-step*2);checked(reader->SetCurrentPosition(GUID_NULL,position),"verify seek");read(expected-step*2);
}
}
int wmain(int argc,wchar_t** argv){
    try{
        const bool comparison=argc==6&&std::wstring_view(argv[1])==L"--compare";
        if(!comparison&&(argc<3||argc>5)){std::cerr<<"emberframe_demo_video BMP_DIRECTORY OUTPUT.mp4 [FPS=15] [REPEAT=1]\n  --compare LEFT_DIR RIGHT_DIR OUTPUT.mp4 FPS\n";return 2;}
        const fs::path directory=fs::absolute(argv[comparison?2:1]),output=fs::absolute(argv[comparison?4:2]);
        const unsigned fps=comparison?unsigned(std::stoul(argv[5])):argc>3?unsigned(std::stoul(argv[3])):15,repeat=comparison?1:argc>4?unsigned(std::stoul(argv[4])):1;
        if(fps<5||fps>60||repeat<1||repeat>60||output.extension()!=L".mp4")throw std::invalid_argument("FPS 5..60, repeat 1..60, output .mp4 required");
        auto partial=output;partial+=L".partial.mp4";if(fs::exists(output)||fs::exists(partial))throw std::runtime_error("Output exists; choose a new filename to preserve prior evidence");
        const auto frames=frame_files(directory);std::vector<fs::path> right;
        if(comparison){right=frame_files(fs::absolute(argv[3]));if(right.size()!=frames.size())throw std::runtime_error("Comparison frame counts differ");for(std::size_t i=0;i<frames.size();++i)if(frames[i].filename()!=right[i].filename())throw std::runtime_error("Comparison frame names do not align");}
        auto read_frame=[&](std::size_t i){auto left=bitmap(frames[i]);return comparison?side_by_side(left,bitmap(right[i])):left;};
        auto first=read_frame(0);Runtime runtime;ComPtr<IMFSinkWriter> writer;checked(MFCreateSinkWriterFromURL(partial.c_str(),nullptr,nullptr,&writer),"create sink writer");auto encoded=media_type(MFVideoFormat_H264,first.width,first.height,fps);checked(encoded->SetUINT32(MF_MT_AVG_BITRATE,4000000),"bit rate");DWORD stream=0;checked(writer->AddStream(encoded.Get(),&stream),"add stream");auto input=media_type(MFVideoFormat_NV12,first.width,first.height,fps);checked(writer->SetInputMediaType(stream,input.Get(),nullptr),"input type");checked(writer->BeginWriting(),"begin writing");
        std::uint64_t index=0;for(std::size_t i=0;i<frames.size();++i){auto frame=read_frame(i);if(frame.width!=first.width||frame.height!=first.height)throw std::runtime_error("BMP frame sizes differ");auto data=nv12(frame);for(unsigned n=0;n<repeat;++n){ComPtr<IMFMediaBuffer> buffer;checked(MFCreateMemoryBuffer(DWORD(data.size()),&buffer),"video buffer");BYTE* mapped=nullptr;checked(buffer->Lock(&mapped,nullptr,nullptr),"lock buffer");std::copy(data.begin(),data.end(),mapped);checked(buffer->Unlock(),"unlock buffer");checked(buffer->SetCurrentLength(DWORD(data.size())),"buffer size");ComPtr<IMFSample> sample;checked(MFCreateSample(&sample),"sample");checked(sample->AddBuffer(buffer.Get()),"sample buffer");auto time=LONGLONG(index*10000000/fps),next=LONGLONG((index+1)*10000000/fps);checked(sample->SetSampleTime(time),"sample time");checked(sample->SetSampleDuration(next-time),"sample duration");checked(writer->WriteSample(stream,sample.Get()),"write sample");++index;}}
        checked(writer->Finalize(),"finalize");writer.Reset();verify(partial,first.width,first.height,LONGLONG(index*10000000/fps),10000000/fps);fs::rename(partial,output);std::wcout<<L"Video verified: "<<output.wstring()<<L" frames="<<index<<L" duration="<<double(index)/fps<<L"s\n";return 0;
    }catch(const std::exception& error){std::cerr<<"Video export failed: "<<error.what()<<"\n";return 1;}
}
