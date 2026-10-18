#include "core/audio_mix.h"

#include <algorithm>
#include <cmath>

namespace rig {

std::vector<float> computePeaks(const AudioSamples& s, int perSecond) {
    std::vector<float> peaks;
    size_t frames = s.stereo.size() / 2;
    if (frames == 0 || perSecond <= 0 || s.sampleRate <= 0) return peaks;
    size_t bucket = std::max<size_t>(1, (size_t)s.sampleRate / perSecond);
    for (size_t start = 0; start < frames; start += bucket) {
        float peak = 0;
        for (size_t f = start; f < std::min(frames, start + bucket); ++f)
            peak = std::max({peak, std::fabs(s.stereo[f * 2]), std::fabs(s.stereo[f * 2 + 1])});
        peaks.push_back(std::min(peak, 1.0f));
    }
    return peaks;
}

std::vector<float> mixAudio(const std::vector<AudioPlacement>& placements, float duration, int rate) {
    size_t frames = (size_t)std::max(0.0f, std::ceil(duration * rate));
    std::vector<float> out(frames * 2, 0.0f);
    for (const AudioPlacement& p : placements) {
        if (!p.samples || p.samples->stereo.empty() || p.samples->sampleRate <= 0) continue;
        const AudioSamples& src = *p.samples;
        size_t srcFrames = src.stereo.size() / 2;
        // Constant-power-ish balance: attenuate the opposite side only.
        float bal = std::clamp(p.balance, -1.0f, 1.0f);
        float gl = p.volume * (bal > 0 ? 1 - bal : 1.0f), gr = p.volume * (bal < 0 ? 1 + bal : 1.0f);
        double step = (double)src.sampleRate / rate;
        long long first = (long long)std::floor(p.start * rate);
        for (size_t f = (size_t)std::max(0LL, first); f < frames; ++f) {
            double pos = ((long long)f - first) * step;  // source frame
            if (pos < 0) continue;
            size_t i = (size_t)pos;
            if (i >= srcFrames) break;
            double frac = pos - i;
            size_t j = std::min(i + 1, srcFrames - 1);
            float l = (float)(src.stereo[i * 2] * (1 - frac) + src.stereo[j * 2] * frac);
            float r = (float)(src.stereo[i * 2 + 1] * (1 - frac) + src.stereo[j * 2 + 1] * frac);
            out[f * 2] += l * gl;
            out[f * 2 + 1] += r * gr;
        }
    }
    for (float& v : out) v = std::clamp(v, -1.0f, 1.0f);
    return out;
}

std::vector<uint8_t> encodeWav(const std::vector<float>& stereo, int rate) {
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        out.push_back((uint8_t)v);
        out.push_back((uint8_t)(v >> 8));
    };
    uint32_t dataBytes = (uint32_t)(stereo.size() * 2);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32(36 + dataBytes);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(1);  // PCM
    u16(2);
    u32((uint32_t)rate);
    u32((uint32_t)rate * 4);
    u16(4);
    u16(16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32(dataBytes);
    for (float v : stereo) u16((uint16_t)(int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
    return out;
}

std::vector<AudioEventKey> audioEventKeys(const Skeleton& sk, const Animation& anim) {
    std::vector<AudioEventKey> out;
    for (const EventKey& k : anim.events)
        if (k.event >= 0 && k.event < (int)sk.events.size() && !sk.events[k.event].audioPath.empty()) out.push_back({k.time, k.event});
    return out;
}

} // namespace rig
