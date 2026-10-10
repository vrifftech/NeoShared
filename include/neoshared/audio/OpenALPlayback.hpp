#pragma once
#include <neoshared/audio/Audio.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace neoshared::audio {
struct OpenALSourceParameters {
    float gain{1},pitch{1};
    std::array<float,3> position{};
    float minimumDistance{10},maximumDistance{20};
    bool positional{},looping{};
};
// Explicit OpenAL provider boundary. The caller supplies one provider's symbol
// resolver and retains that provider for the lifetime of this object. No audio
// device, spatializer or provider is substituted if initialization fails.
class OpenALPlayback final {
public:
    using Resolver=std::function<void*(const char*)>;
    using Buffer=std::uint32_t;
    explicit OpenALPlayback(Resolver);
    ~OpenALPlayback();
    OpenALPlayback(const OpenALPlayback&)=delete;
    OpenALPlayback& operator=(const OpenALPlayback&)=delete;
    Buffer prepare(const PcmData&);
    // Native open-stream creates its sample before admission, without
    // starting playback. Gain/mix/loop writes follow at the caller.
    void preparePausedStream(std::uint64_t source,Buffer);
    void setLooping(std::uint64_t source,bool);
    void pause(std::uint64_t source);
    // AIL_start_stream dispatches SampleOpenAL::Start, not Pause(0).
    void resume(std::uint64_t source);
    void play(std::uint64_t source,Buffer,const OpenALSourceParameters&);
    void restart(std::uint64_t source,bool looping);
    void stop(std::uint64_t);
    bool playing(std::uint64_t) const;
    void setGain(std::uint64_t,float);
    // Standard OpenAL mono panning: -1 left, +1 right, listener-relative
    // and without distance attenuation. OpenAL preserves stereo channels;
    // its core API does not spatialize stereo buffers.
    void setPan(std::uint64_t,float);
    void erase(std::uint64_t);
    void clear();
    // Host application suspension; retains source offsets and resumes only
    // the voices which this operation paused.
    void setSuspended(bool);
    void setListener(std::array<float,3> position,std::array<float,3> forward,
                     std::array<float,3> up);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neoshared::audio
