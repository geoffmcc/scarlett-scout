#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <onnxruntime_cxx_api.h>
#include "version.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr size_t kSamples = 160000;
std::map<std::wstring,std::wstring> args(int argc,wchar_t** argv) {
    std::map<std::wstring,std::wstring> out;
    for(int i=1;i+1<argc;i+=2) out[argv[i]]=argv[i+1];
    return out;
}
std::wstring arg(const std::map<std::wstring,std::wstring>& a,const wchar_t* k) {
    auto i=a.find(k); return i==a.end()?L"":i->second;
}
void status(const fs::path& p,const std::string& s) {
    auto t=p.wstring()+L".tmp";
    { std::ofstream f(t,std::ios::binary|std::ios::trunc); f<<s<<'\n'; f.flush(); }
    MoveFileExW(t.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
uint32_t u32(const unsigned char* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
bool wav(const fs::path& p,std::vector<float>& out) {
    std::ifstream f(p,std::ios::binary); unsigned char h[12]{};
    f.read((char*)h,12);
    if(f.gcount()!=12||std::string((char*)h,4)!="RIFF"||std::string((char*)h+8,4)!="WAVE") return false;
    uint16_t format=0,ch=0,bits=0; uint32_t rate=0; std::vector<unsigned char> pcm;
    while(f) {
        unsigned char c[8]{}; f.read((char*)c,8); if(f.gcount()!=8) break;
        uint32_t n=u32(c+4); std::string id((char*)c,4);
        if(id=="fmt ") {
            std::vector<unsigned char> b(n); f.read((char*)b.data(),n); if(b.size()<16||f.gcount()!=(std::streamsize)n)return false;
            format=b[0]|(b[1]<<8); ch=b[2]|(b[3]<<8); rate=u32(b.data()+4); bits=b[14]|(b[15]<<8);
        } else if(id=="data") { pcm.resize(n); f.read((char*)pcm.data(),n); if(f.gcount()!=(std::streamsize)n)return false; break; }
        else f.seekg(n,std::ios::cur);
        if(n&1) f.seekg(1,std::ios::cur);
    }
    if(format!=1||ch!=1||bits!=16||rate!=48000||pcm.empty()) return false;
    std::vector<float> source(pcm.size()/2);
    for(size_t i=0;i<source.size();++i) {
        int16_t v=(int16_t)(uint16_t(pcm[2*i])|(uint16_t(pcm[2*i+1])<<8));
        source[i]=float(v)/32768.f;
    }
    out.reserve((source.size()+2)/3);
    for(size_t i=0;i<source.size();i+=3) out.push_back(source[i]);
    if(out.size()>kSamples) out.erase(out.begin(),out.end()-kSamples);
    if(out.size()<kSamples) out.resize(kSamples,0.f);
    double mean=0; for(float x:out)mean+=x; mean/=out.size();
    double var=0; for(float x:out){double d=x-mean;var+=d*d;} double sd=std::max(std::sqrt(var/out.size()),1e-7);
    for(float& x:out)x=float(std::clamp((x-mean)/sd,-5.0,5.0));
    return true;
}
std::map<int,std::string> labels(const fs::path& p) {
    std::map<int,std::string> out; std::ifstream f(p); std::string s((std::istreambuf_iterator<char>(f)),{});
    auto a=s.find("\"id2label\""); if(a==std::string::npos)return out;
    a=s.find('{',a); auto b=s.find('}',a); if(a==std::string::npos||b==std::string::npos)return out;
    std::string part=s.substr(a,b-a+1); size_t pos=0;
    while((pos=part.find('"',pos))!=std::string::npos) {
        auto end=part.find('"',pos+1); if(end==std::string::npos)break;
        int id=-1; try{id=std::stoi(part.substr(pos+1,end-pos-1));}catch(...){pos=end+1;continue;}
        auto colon=part.find(':',end); auto q=part.find('"',colon); auto qe=part.find('"',q+1);
        if(q==std::string::npos||qe==std::string::npos)break;
        out[id]=part.substr(q+1,qe-q-1); pos=qe+1;
    }
    return out;
}
std::string clean(std::string s) { for(char& c:s)if(c=='|'||c=='\n'||c=='\r')c=' '; if(s.size()>400)s.resize(400);return s; }
}
int wmain() {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc); if(!argv)return 2;
    auto a=args(argc,argv); LocalFree(argv);
    fs::path runtime=arg(a,L"--runtime"), model=arg(a,L"--model"), config=arg(a,L"--config");
    fs::path inbox=arg(a,L"--inbox"), stat=arg(a,L"--status"), track=arg(a,L"--track-result");
    fs::path capture=arg(a,L"--capture-result"), stop=arg(a,L"--stop"), reset=arg(a,L"--reset");
    if(runtime.empty()||model.empty()||inbox.empty()||stat.empty()||track.empty()||capture.empty()||stop.empty())return 2;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(runtime.c_str()); SetDllDirectoryW(runtime.c_str());
    status(stat,std::string("STARTING|Scarlett Scout v")+SCARLETT_SCOUT_VERSION_A+" · loading ONNX Runtime CUDA");
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"ScarlettScoutCUDA");
        Ort::SessionOptions options; options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        OrtCUDAProviderOptions cuda{}; cuda.device_id=0;
        options.AppendExecutionProvider_CUDA(cuda);
        Ort::Session session(env,model.c_str(),options);
        Ort::AllocatorWithDefaultOptions allocator;
        if(session.GetInputCount()<1||session.GetInputCount()>2||session.GetOutputCount()<1) throw std::runtime_error("Expected one audio input and optional attention mask");
        std::vector<std::string> inputNames; std::vector<const char*> inputPtrs;
        for(size_t i=0;i<session.GetInputCount();++i){ auto n=session.GetInputNameAllocated(i,allocator); inputNames.emplace_back(n.get()); }
        for(auto& n:inputNames)inputPtrs.push_back(n.c_str());
        auto outputName=session.GetOutputNameAllocated(0,allocator); std::string outputString=outputName.get(); const char* outputPtr=outputString.c_str();
        auto inputInfo=session.GetInputTypeInfo(0).GetTensorTypeAnd、ShapeInfo();
        (void)inputInfo;
        auto labelsMap=labels(config);
        status(stat,std::string("READY|NVIDIA CUDA|Scarlett Scout v")+SCARLETT_SCOUT_VERSION_A+" · ONNX Runtime CUDA session ready");
        std::map<std::string,double> sums; std::map<std::string,unsigned> votes; unsigned windows=0;
        while(!fs::exists(stop)) {
            if(!reset.empty()&&fs::exists(reset)){sums.clear();votes.clear();windows=0;std::error_code ec;fs::remove(reset,ec);status(track,"TRACK|0|Waiting for track-analysis windows");}
            std::vector<fs::path> jobs;
            try{for(auto& e:fs::directory_iterator(inbox)){auto n=e.path().filename().wstring();if(e.is_regular_file()&&e.path().extension()==L".wav"&&(n.rfind(L"track-",0)==0||n.rfind(L"capture-",0)==0))jobs.push_back(e.path());}}catch(...){}
            std::sort(jobs.begin(),jobs.end(),[](auto&x,auto&y){return fs::last_write_time(x)<fs::last_write_time(y);});
            if(jobs.empty()){std::this_thread::sleep_for(std::chrono::milliseconds(250));continue;}
            auto job=jobs.front(); std::vector<float> audio;
            if(!wav(job,audio)){std::error_code ec;fs::rename(job,job.wstring()+L".failed",ec);status(stat,"ERROR|CUDA worker could not read mono PCM16 48 kHz job");continue;}
            std::array<int64_t,2> shape{1,(int64_t)audio.size()};
            auto mem=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
            std::vector<Ort::Value> tensors;
            auto inputType=session.GetInputTypeInfo(0).GetTensorTypeAnd、ShapeInfo().GetElementType();
            if(inputType==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                tensors.emplace_back(Ort::Value::CreateTensor<float>(mem,audio.data(),audio.size(),shape.data(),2));
            else throw std::runtime_error("CUDA worker currently requires FP32 audio input tensor");
            if(session.GetInputCount()>1){
                auto info=session.GetInputTypeInfo(1).GetTensorTypeAnd、ShapeInfo();
                auto type=info.GetElementType(); auto dims=info.GetShape(); size_t count=1;for(auto d:dims)count*=size_t(d<0?shape[1]:d);
                if(type==ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64){std::vector<int64_t> mask(count,1);tensors.emplace_back(Ort::Value::CreateTensor<int64_t>(mem,mask.data(),mask.size(),dims.data(),dims.size()));}
                else if(type==ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT){std::vector<float> mask(count,1.f);tensors.emplace_back(Ort::Value::CreateTensor<float>(mem,mask.data(),mask.size(),dims.data(),dims.size()));}
                else throw std::runtime_error("Unsupported CUDA attention-mask tensor type");
            }
            auto outputs=session.Run(Ort::RunOptions{nullptr},inputPtrs.data(),tensors.data(),tensors.size(),&outputPtr,1);
            auto info=outputs[0].GetTensorTypeAndShapeInfo(); size_t count=info.GetElementCount();
            if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)throw std::runtime_error("CUDA model output must be FP32");
            const float* scores=outputs[0].GetTensorData<float>(); float maxv=*std::max_element(scores,scores+count);
            double total=0;std::vector<double> probs(count);for(size_t i=0;i<count;++i){probs[i]=std::exp(double(scores[i]-maxv));total+=probs[i];}
            if(!(total>0)||!std::isfinite(total))throw std::runtime_error("Model returned invalid scores");
            for(auto&v:probs)v/=total;
            std::vector<size_t> order(count);for(size_t i=0;i<count;++i)order[i]=i;
            std::sort(order.begin(),order.end(),[&](size_t x,size_t y){return probs[x]>probs[y];});
            bool isTrack=job.filename().wstring().rfind(L"track-",0)==0;
            std::vector<std::pair<std::string,double>> ranked;
            for(size_t i:order){auto it=labelsMap.find((int)i);ranked.emplace_back(it==labelsMap.end()?"Class "+std::to_string(i):it->second,probs[i]*100.0);}
            if(isTrack){
                for(auto&v:ranked)sums[v.first]+=v.second;if(!ranked.empty())votes[ranked[0].first]++;++windows;
                std::vector<std::pair<std::string,double>> avg;for(auto&v:sums)avg.emplace_back(v.first,v.second/windows);
                std::sort(avg.begin(),avg.end(),[](auto&x,auto&y){return x.second>y.second;});
                std::string lead=avg.empty()?"unknown":avg[0].first;double runner=avg.size()>1?avg[1].second:0;
                unsigned vc=votes[lead];double share=100.0*vc/std::max(1u,windows);
                bool reliable=windows>=4&&share>=50&&(avg[0].second-runner)>=6&&avg[0].second>=38;
                std::ostringstream o;o<<(reliable?"TRACK|":"TRACK_UNCERTAIN|")<<windows<<"|"<<std::fixed<<std::setprecision(0)<<share;
                for(size_t i=0;i<std::min<size_t>(3,avg.size());++i)o<<"|"<<clean(avg[i].first)<<"~"<<std::setprecision(1)<<avg[i].second;
                status(track,o.str());
            } else {std::ostringstream o;o<<"CAPTURE";for(size_t i=0;i<std::min<size_t>(3,ranked.size());++i)o<<"|"<<clean(ranked[i].first)<<"~"<<std::fixed<<std::setprecision(1)<<ranked[i].second;status(capture,o.str());}
            status(stat,"READY|NVIDIA CUDA|CUDA inference completed successfully");
            std::error_code ec;fs::remove(job,ec);
        }
        status(stat,"STOPPED|NVIDIA CUDA worker stopped cleanly");
    } catch(const std::exception& e) { status(stat,std::string("ERROR|CUDA initialization/inference failed: ")+e.what()); return 3; }
    return 0;
}
