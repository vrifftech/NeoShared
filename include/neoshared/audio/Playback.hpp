#pragma once
#include <neoshared/audio/Audio.hpp>
#include <mutex>
#include <string>

namespace neoshared::audio {
// The retained owner keeps these immutable interleaved samples alive through
// device callbacks, including views into another tool's audio representation.
struct PcmView {
    std::shared_ptr<const void> owner;
    const float* samples = nullptr;
    std::size_t frames = 0;
    unsigned sampleRate = 0, channels = 0;
    std::size_t frameCount() const noexcept { return frames; }
    double durationSeconds() const noexcept {
        return sampleRate ? double(frames)/sampleRate : 0.0;
    }
    static PcmView from(std::shared_ptr<const PcmData> audio);
};
// The device callback advances this cursor; UI wall-clock timers do not.
class PcmTransport {
public:
    void load(std::shared_ptr<const PcmData> audio);
    void load(PcmView audio);
    void play(); void pause(); void stop(); void seek(double seconds);
    void setLoop(bool enabled, double begin, double end);
    void setVolume(float gain);
    bool playing() const;
    bool hasStartedStreaming() const;
    double position() const;
    std::size_t render(float* output, std::size_t frames);
private:
    mutable std::mutex mutex_;
    PcmView audio_;
    std::size_t cursor_ = 0, loopBegin_ = 0, loopEnd_ = 0;
    bool playing_ = false, loop_ = false, streamed_ = false;
    float gain_ = 1.0f;
};
class AudioPlayback {
public:
    AudioPlayback(); ~AudioPlayback();
    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;
    void load(std::shared_ptr<const PcmData> audio);
    void load(PcmView audio);
    void play(); void pause(); void stop(); void seek(double seconds);
    void setLoop(bool enabled, double begin, double end);
    void setVolume(float gain);
    std::string pollError();
    bool playing() const;
    bool hasStartedStreaming() const;
    double position() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace neoshared::audio
