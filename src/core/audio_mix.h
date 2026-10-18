#pragma once
// Audio helpers that need no audio device: waveform peaks, mixing event sounds into one
// track (for video export) and WAV encoding. Decoding/playback live in platform/audio.
#include "core/model.h"

#include <cstdint>

namespace rig {

// Decoded sound: interleaved stereo float samples.
struct AudioSamples {
    int sampleRate = 44100;
    std::vector<float> stereo;
    float duration() const { return sampleRate > 0 ? (float)(stereo.size() / 2) / sampleRate : 0.0f; }
};

// Max absolute amplitude per bucket (peaksPerSecond buckets per second), for drawing.
std::vector<float> computePeaks(const AudioSamples& samples, int peaksPerSecond);

struct AudioPlacement {
    const AudioSamples* samples = nullptr;
    float start = 0;     // seconds on the output timeline (may be negative: starts part-way)
    float volume = 1;
    float balance = 0;   // -1 left .. 1 right
};
// Mixes the placements into `duration` seconds of interleaved stereo at `sampleRate`
// (sources are resampled linearly; the result is clamped to [-1, 1]).
std::vector<float> mixAudio(const std::vector<AudioPlacement>& placements, float duration, int sampleRate);

// 16-bit PCM WAV file bytes.
std::vector<uint8_t> encodeWav(const std::vector<float>& stereo, int sampleRate);

// Event keys of `anim` whose event has an audio file, with their play time.
struct AudioEventKey {
    float time;
    int event;
};
std::vector<AudioEventKey> audioEventKeys(const Skeleton& sk, const Animation& anim);

} // namespace rig
