#include <neoshared/audio/OpenALPlayback.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace neoshared::audio {
namespace {
constexpr int position=0x1004,gain=0x100a,pitch=0x1003,relative=0x202,
    referenceDistance=0x1020,maximumDistance=0x1023,rolloffFactor=0x1021,looping=0x1007,
    buffer=0x1009,sourceState=0x1010,playingState=0x1012,orientation=0x100f;
template<class T>T symbol(const OpenALPlayback::Resolver& r,const char* name) {
    const auto p=r(name);
    if(!p)throw std::runtime_error(std::string("OpenAL provider is missing ")+name);
    return reinterpret_cast<T>(p);
}
}
struct OpenALPlayback::Impl {
    Resolver resolver;
    void* (*openDevice)(const char*);
    char (*closeDevice)(void*);
    void* (*createContext)(void*,const int*);
    void (*destroyContext)(void*);
    char (*makeCurrent)(void*);
    void (*genSources)(int,unsigned*);
    void (*deleteSources)(int,const unsigned*);
    void (*genBuffers)(int,unsigned*);
    void (*deleteBuffers)(int,const unsigned*);
    void (*bufferData)(unsigned,int,const void*,int,int);
    void (*sourcei)(unsigned,int,int);
    void (*sourcef)(unsigned,int,float);
    void (*source3f)(unsigned,int,float,float,float);
    void (*sourcePlay)(unsigned);
    void (*sourceStop)(unsigned);
    void (*sourcePause)(unsigned);
    void (*getSourcei)(unsigned,int,int*);
    void (*listener3f)(int,float,float,float);
    void (*listenerfv)(int,const float*);
    int (*getError)();
    void* device{};void* context{};
    std::map<std::uint64_t,unsigned> sources;
    std::vector<unsigned> buffers;
    std::set<std::uint64_t> suspendedSources;
    bool suspended{};
    explicit Impl(Resolver r):resolver(std::move(r)) {
#define LOAD(member,name) member=symbol<decltype(member)>(resolver,name)
        LOAD(openDevice,"alcOpenDevice");LOAD(closeDevice,"alcCloseDevice");
        LOAD(createContext,"alcCreateContext");LOAD(destroyContext,"alcDestroyContext");
        LOAD(makeCurrent,"alcMakeContextCurrent");LOAD(genSources,"alGenSources");
        LOAD(deleteSources,"alDeleteSources");LOAD(genBuffers,"alGenBuffers");
        LOAD(deleteBuffers,"alDeleteBuffers");LOAD(bufferData,"alBufferData");
        LOAD(sourcei,"alSourcei");LOAD(sourcef,"alSourcef");LOAD(source3f,"alSource3f");
        LOAD(sourcePlay,"alSourcePlay");LOAD(sourceStop,"alSourceStop");
        LOAD(sourcePause,"alSourcePause");
        LOAD(getSourcei,"alGetSourcei");LOAD(listener3f,"alListener3f");
        LOAD(listenerfv,"alListenerfv");LOAD(getError,"alGetError");
#undef LOAD
        device=openDevice(nullptr);
        if(!device)throw std::runtime_error("OpenAL could not open its default device");
        context=createContext(device,nullptr);
        if(!context) {closeDevice(device);device=nullptr;throw std::runtime_error("OpenAL could not create its context");}
        if(!makeCurrent(context)) {
            destroyContext(context);closeDevice(device);context=nullptr;device=nullptr;
            throw std::runtime_error("OpenAL could not make its context current");
        }
    }
    void current() const {
        if(!makeCurrent(context))throw std::runtime_error("OpenAL could not make its context current");
    }
    void check(const char* operation) const {
        const auto error=getError();
        if(error)throw std::runtime_error(std::string("OpenAL ")+operation+" failed ("+std::to_string(error)+")");
    }
    void clear() {
        for(const auto& source:sources) {sourceStop(source.second);deleteSources(1,&source.second);}
        sources.clear();
        suspendedSources.clear();
        for(auto value:buffers)deleteBuffers(1,&value);
        buffers.clear();
    }
    ~Impl() {
        if(context) {makeCurrent(context);clear();makeCurrent(nullptr);destroyContext(context);}
        if(device)closeDevice(device);
    }
};
OpenALPlayback::OpenALPlayback(Resolver r):impl_(std::make_unique<Impl>(std::move(r))) {}
OpenALPlayback::~OpenALPlayback()=default;
OpenALPlayback::Buffer OpenALPlayback::prepare(const PcmData& pcm) {
    if((pcm.channels!=1&&pcm.channels!=2)||!pcm.sampleRate)
        throw std::runtime_error("OpenAL scene samples require mono or stereo PCM and a sample rate");
    if(pcm.samples.size()>std::size_t(std::numeric_limits<int>::max())/2||
       pcm.sampleRate>unsigned(std::numeric_limits<int>::max()))
        throw std::runtime_error("OpenAL PCM exceeds its signed buffer limits");
    // The supplied Miles OpenAL bridge uploads signed 16-bit mono/stereo.
    // Decoding remains the existing shared codec boundary; this is its PCM
    // representation conversion, not a replacement spatialization algorithm.
    std::vector<std::int16_t> samples;samples.reserve(pcm.samples.size());
    for(float v:pcm.samples)samples.push_back(std::int16_t(std::clamp(v*32768.0f,-32768.0f,32767.0f)));
    auto& p=*impl_;p.current();unsigned value{};p.genBuffers(1,&value);p.check("buffer allocation");
    p.buffers.push_back(value);
    p.bufferData(value,pcm.channels==1?0x1101:0x1103,samples.data(),int(samples.size()*2),int(pcm.sampleRate));
    p.check("PCM upload");return value;
}
void OpenALPlayback::preparePausedStream(std::uint64_t id,Buffer audio) {
    auto& p=*impl_;p.current();
    // SampleOpenAL is constructed per initialized sample. Replaying a logical
    // source creates the same fresh relative-position state, not a retained
    // property from a previous resource.
    erase(id);unsigned source{};p.genSources(1,&source);p.check("source allocation");
    p.sources.emplace(id,source);
    p.source3f(source,position,0,0,0);p.sourcef(source,gain,1);p.sourcef(source,pitch,1);
    p.sourcei(source,relative,1);p.sourcei(source,buffer,int(audio));
    p.check("source initialization");
}
void OpenALPlayback::setLooping(std::uint64_t id,bool value) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end()) {p.sourcei(found->second,looping,value?1:0);p.check("source loop state");}
}
void OpenALPlayback::pause(std::uint64_t id) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end()) {p.sourcePause(found->second);p.check("source pause");}
    p.suspendedSources.erase(id);
}
void OpenALPlayback::resume(std::uint64_t id) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found==p.sources.end())return;
    p.sourcePlay(found->second);
    if(p.suspended) {p.sourcePause(found->second);p.suspendedSources.insert(id);}
    p.check("stream start");
}
void OpenALPlayback::play(std::uint64_t id,Buffer audio,const OpenALSourceParameters& value) {
    preparePausedStream(id,audio);
    auto& p=*impl_;const auto source=p.sources.at(id);
    p.sourcei(source,looping,value.looping?1:0);
    p.sourcef(source,gain,value.gain);p.sourcef(source,pitch,value.pitch);
    if(value.positional) {
        p.sourcef(source,referenceDistance,value.minimumDistance);
        p.sourcef(source,maximumDistance,value.maximumDistance);
        p.source3f(source,position,value.position[0],value.position[1],value.position[2]);
        // Miles::SampleOpenAL::Set3DPosition changes SOURCE_RELATIVE only for
        // a nonzero vector. Its zero-origin behavior is preserved.
        if(value.position[0]!=0||value.position[1]!=0||value.position[2]!=0)
            p.sourcei(source,relative,0);
    }
    p.sourcePlay(source);
    if(p.suspended) {p.sourcePause(source);p.suspendedSources.insert(id);}
    p.check("source playback");
}
void OpenALPlayback::stop(std::uint64_t id) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end())p.sourceStop(found->second);
    p.suspendedSources.erase(id);
}
void OpenALPlayback::restart(std::uint64_t id,bool loop) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found==p.sources.end())return;
    const auto source=found->second;
    // AIL_stop_sample dispatches SampleOpenAL::Pause(1), not Stop(). The
    // bound physical sample and its current gain/rate/position are retained.
    p.sourcePause(source);p.sourcei(source,looping,loop?1:0);
    p.sourcef(source,0x1026,0.0f); // SetPositionInBytes -> AL_BYTE_OFFSET
    int state{};p.getSourcei(source,sourceState,&state);
    // Native Pause(0) does not restart an AL_STOPPED sample.
    if(state!=playingState&&state!=0x1014)p.sourcePlay(source);
    if(p.suspended) {p.sourcePause(source);p.suspendedSources.insert(id);}
    p.check("retained sample restart");
}
bool OpenALPlayback::playing(std::uint64_t id) const {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found==p.sources.end())return false;
    if(p.suspendedSources.count(id))return true;
    int state{};p.getSourcei(found->second,sourceState,&state);p.check("source status");return state==playingState;
}
void OpenALPlayback::setGain(std::uint64_t id,float value) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end()) {p.sourcef(found->second,gain,value);p.check("source gain");}
}
void OpenALPlayback::setPan(std::uint64_t id,float value) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end()) {
        p.sourcei(found->second,relative,1);
        p.sourcef(found->second,rolloffFactor,0);
        p.source3f(found->second,position,value,0,-std::sqrt(1.0f-value*value));
        p.check("source pan");
    }
}
void OpenALPlayback::erase(std::uint64_t id) {
    auto& p=*impl_;p.current();const auto found=p.sources.find(id);
    if(found!=p.sources.end()) {p.sourceStop(found->second);p.deleteSources(1,&found->second);p.sources.erase(found);}
    p.suspendedSources.erase(id);
}
void OpenALPlayback::clear() {impl_->current();impl_->clear();}
void OpenALPlayback::setSuspended(bool value) {
    auto& p=*impl_;if(p.suspended==value)return;p.current();p.suspended=value;
    if(value) {
        for(const auto& source:p.sources) {
            int state{};p.getSourcei(source.second,sourceState,&state);
            if(state==playingState) {p.sourcePause(source.second);p.suspendedSources.insert(source.first);}
        }
    } else {
        for(auto id:p.suspendedSources)p.sourcePlay(p.sources.at(id));
        p.suspendedSources.clear();
    }
    p.check("application suspension");
}
void OpenALPlayback::setListener(std::array<float,3> p,std::array<float,3> forward,std::array<float,3> up) {
    auto& x=*impl_;x.current();x.listener3f(position,p[0],p[1],p[2]);
    const float axes[]{forward[0],forward[1],forward[2],up[0],up[1],up[2]};
    x.listenerfv(orientation,axes);x.check("listener update");
}
} // namespace neoshared::audio
