#include "platform/audio.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include <map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::wstring widen(const std::string& s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

struct Cached {
    bool failed = false;
    rig::AudioSamples samples;
    std::vector<float> peaks;
};
} // namespace

struct AudioPlayer::Impl {
    ma_engine engine{};
    bool engineOk = false;
    std::map<std::string, Cached> cache;
    std::vector<std::unique_ptr<ma_sound>> sounds;

    Cached& load(const std::string& path) {
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        Cached& c = cache[path];
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 44100);
        ma_decoder decoder;
#ifdef _WIN32
        ma_result r = ma_decoder_init_file_w(widen(path).c_str(), &cfg, &decoder);
#else
        ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &decoder);
#endif
        if (r != MA_SUCCESS) {
            c.failed = true;
            return c;
        }
        c.samples.sampleRate = 44100;
        float chunk[4096 * 2];
        for (;;) {
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&decoder, chunk, 4096, &read) != MA_SUCCESS && read == 0) break;
            if (read == 0) break;
            c.samples.stereo.insert(c.samples.stereo.end(), chunk, chunk + read * 2);
        }
        ma_decoder_uninit(&decoder);
        c.peaks = rig::computePeaks(c.samples, 100);
        return c;
    }
};

AudioPlayer::AudioPlayer() : impl_(new Impl) {
    impl_->engineOk = ma_engine_init(nullptr, &impl_->engine) == MA_SUCCESS;
}

AudioPlayer::~AudioPlayer() {
    stopAll();
    if (impl_->engineOk) ma_engine_uninit(&impl_->engine);
}

bool AudioPlayer::deviceAvailable() const { return impl_->engineOk; }

const rig::AudioSamples* AudioPlayer::samples(const std::string& path) {
    Cached& c = impl_->load(path);
    return c.failed ? nullptr : &c.samples;
}

const std::vector<float>* AudioPlayer::peaks(const std::string& path) {
    Cached& c = impl_->load(path);
    return c.failed ? nullptr : &c.peaks;
}

void AudioPlayer::play(const std::string& path, float offset, float volume, float balance, float pitch) {
    if (!impl_->engineOk || path.empty()) return;
    auto sound = std::make_unique<ma_sound>();
    // MA_SOUND_FLAG_DECODE: the resource manager decodes once and shares the data between plays.
#ifdef _WIN32
    ma_result r = ma_sound_init_from_file_w(&impl_->engine, widen(path).c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound.get());
#else
    ma_result r = ma_sound_init_from_file(&impl_->engine, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, sound.get());
#endif
    if (r != MA_SUCCESS) return;
    ma_sound_set_volume(sound.get(), volume);
    ma_sound_set_pan(sound.get(), balance);
    ma_sound_set_pitch(sound.get(), pitch);
    if (offset > 0) ma_sound_seek_to_second(sound.get(), offset);
    ma_sound_start(sound.get());
    impl_->sounds.push_back(std::move(sound));
}

void AudioPlayer::stopAll() {
    for (auto& s : impl_->sounds) {
        ma_sound_stop(s.get());
        ma_sound_uninit(s.get());
    }
    impl_->sounds.clear();
}

void AudioPlayer::update() {
    auto& v = impl_->sounds;
    for (size_t i = 0; i < v.size();) {
        if (ma_sound_at_end(v[i].get())) {
            ma_sound_uninit(v[i].get());
            v.erase(v.begin() + i);
        } else {
            ++i;
        }
    }
}
