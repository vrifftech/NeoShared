#include <neoshared/audio/Playback.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace neoshared::audio {
PcmView PcmView::from(std::shared_ptr<const PcmData> audio) {
    if (!audio) return {};
    PcmView view;
    view.samples=audio->samples.data();view.frames=audio->frameCount();
    view.sampleRate=audio->sampleRate;view.channels=audio->channels;
    view.owner=std::move(audio);
    return view;
}
void PcmTransport::load(std::shared_ptr<const PcmData> audio) {
    load(PcmView::from(std::move(audio)));
}
void PcmTransport::load(PcmView audio) {
    std::lock_guard<std::mutex> lock(mutex_);audio_=std::move(audio);cursor_=loopBegin_=0;
    loopEnd_=audio_.frameCount();playing_=loop_=streamed_=false;
}
void PcmTransport::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!audio_.owner || !audio_.frameCount()) throw std::runtime_error("Load audio before playback.");
    if(cursor_>=audio_.frameCount()) cursor_=loop_?loopBegin_:0;
    if(loop_ && (cursor_<loopBegin_ || cursor_>=loopEnd_)) cursor_=loopBegin_;
    playing_=true;
}
void PcmTransport::pause(){std::lock_guard<std::mutex> lock(mutex_);playing_=false;}
void PcmTransport::stop(){std::lock_guard<std::mutex> lock(mutex_);playing_=false;streamed_=false;cursor_=0;}
void PcmTransport::seek(double seconds) {
    if(!std::isfinite(seconds)) throw std::runtime_error("Playback position must be finite.");
    std::lock_guard<std::mutex> lock(mutex_);if(!audio_.owner)return;
    const auto& wave=audio_;
    cursor_=static_cast<std::size_t>(std::clamp(seconds,0.0,wave.durationSeconds())*wave.sampleRate);
    cursor_=std::min(cursor_,wave.frameCount());
}
void PcmTransport::setLoop(bool enabled,double begin,double end) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!audio_.owner || !enabled){loop_=false;return;}
    const auto& wave=audio_;
    if(!std::isfinite(begin)||!std::isfinite(end)||begin<0||end<=begin||end>wave.durationSeconds()+1e-6)
        throw std::runtime_error("Select a valid audio range before enabling looping.");
    const auto a=static_cast<std::size_t>(begin*wave.sampleRate),b=std::min(wave.frameCount(),static_cast<std::size_t>(end*wave.sampleRate));
    if(b<=a) throw std::runtime_error("Loop selection is too short.");
    loopBegin_=a;loopEnd_=b;loop_=true;
    if(playing_ && (cursor_<a || cursor_>=b))cursor_=a;
}
void PcmTransport::setVolume(float gain) {
    if(!std::isfinite(gain))throw std::invalid_argument("Invalid preview volume.");
    std::lock_guard<std::mutex> lock(mutex_);gain_=std::clamp(gain,0.f,1.f);
}
bool PcmTransport::hasStartedStreaming() const {std::lock_guard<std::mutex> lock(mutex_);return streamed_;}
bool PcmTransport::playing() const {std::lock_guard<std::mutex> lock(mutex_);return playing_;}
double PcmTransport::position() const {std::lock_guard<std::mutex> lock(mutex_);return audio_.owner?double(cursor_)/audio_.sampleRate:0;}
std::size_t PcmTransport::render(float* output,std::size_t frames) {
    if(!output && frames)throw std::invalid_argument("Null audio buffer.");
    std::lock_guard<std::mutex> lock(mutex_);const auto channels=audio_.owner?audio_.channels:1u;std::fill_n(output,frames*channels,0.f);
    if(!playing_ || !audio_.owner)return 0;
    const auto& wave=audio_;std::size_t done=0;
    while(done<frames && playing_) {
        const auto end=loop_?loopEnd_:wave.frameCount();
        if(cursor_>=end){if(loop_)cursor_=loopBegin_;else{playing_=false;break;}}
        const auto count=std::min(frames-done,end-cursor_);
        std::copy_n(wave.samples+cursor_*channels,count*channels,output+done*channels);done+=count;cursor_+=count;streamed_=true;
        if(cursor_==end){if(loop_)cursor_=loopBegin_;else playing_=false;}
    }
    for(std::size_t i=0;i<done*channels;++i)output[i]*=gain_;
    return done;
}
}
