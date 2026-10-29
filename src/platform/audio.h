#pragma once
#include "core/audio_mix.h"

#include <memory>
#include <string>

// Audio playback (miniaudio). Decoding works even without an output device; playback calls
// are ignored when no device could be opened.
class AudioPlayer {
public:
    explicit AudioPlayer(bool playback = true);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool deviceAvailable() const;
    // Decoded file (stereo float, 44.1 kHz), cached per path; null if it cannot be read.
    const rig::AudioSamples* samples(const std::string& path);
    // Waveform peaks, 100 per second (cached).
    const std::vector<float>* peaks(const std::string& path);

    // Starts `path` `offset` seconds in. volume 0..1, balance -1 (left) .. 1 (right), pitch 1 = normal.
    void play(const std::string& path, float offset, float volume, float balance, float pitch);
    void stopAll();
    void update();  // releases finished sounds

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
