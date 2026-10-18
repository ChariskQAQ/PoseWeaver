#include "core/anim_state.h"

#include <algorithm>
#include <cmath>

namespace rig {

namespace {
float lerp(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

void mixPose(Pose& pose, const Pose& target, float alpha) {
    if (alpha <= 0) return;
    if (alpha >= 1) {
        pose.local = target.local;
        pose.slotAttachment = target.slotAttachment;
        pose.slotColor = target.slotColor;
        pose.drawOrder = target.drawOrder;
        pose.deform = target.deform;
        pose.constraints = target.constraints;
        return;
    }
    const bool discrete = alpha >= 0.5f;
    for (size_t i = 0; i < pose.local.size() && i < target.local.size(); ++i) {
        Transform& a = pose.local[i];
        const Transform& b = target.local[i];
        a.x = lerp(a.x, b.x, alpha);
        a.y = lerp(a.y, b.y, alpha);
        a.rotation += wrapDeg(b.rotation - a.rotation) * alpha;
        a.scaleX = lerp(a.scaleX, b.scaleX, alpha);
        a.scaleY = lerp(a.scaleY, b.scaleY, alpha);
        a.shearX += wrapDeg(b.shearX - a.shearX) * alpha;
        a.shearY += wrapDeg(b.shearY - a.shearY) * alpha;
    }
    for (size_t i = 0; i < pose.slotColor.size() && i < target.slotColor.size(); ++i)
        pose.slotColor[i] = pose.slotColor[i] + (target.slotColor[i] - pose.slotColor[i]) * alpha;
    if (discrete) {
        pose.slotAttachment = target.slotAttachment;
        pose.drawOrder = target.drawOrder;
    }
    // Deform: offsets missing on one side count as zero.
    std::map<std::pair<int, int>, std::vector<Vec2>> deform;
    for (auto& [key, from] : pose.deform) {
        auto it = target.deform.find(key);
        std::vector<Vec2> v = from;
        for (size_t j = 0; j < v.size(); ++j) {
            Vec2 to = it != target.deform.end() && j < it->second.size() ? it->second[j] : Vec2{};
            v[j] = v[j] + (to - v[j]) * alpha;
        }
        deform[key] = std::move(v);
    }
    for (auto& [key, to] : target.deform) {
        if (pose.deform.count(key)) continue;
        std::vector<Vec2> v(to.size());
        for (size_t j = 0; j < v.size(); ++j) v[j] = to[j] * alpha;
        deform[key] = std::move(v);
    }
    pose.deform = std::move(deform);
    for (size_t c = 0; c < pose.constraints.size() && c < target.constraints.size(); ++c) {
        ConstraintValues& a = pose.constraints[c];
        const ConstraintValues& b = target.constraints[c];
        const float bend = a[2];
        for (int j = 0; j < 6; ++j) a[j] = lerp(a[j], b[j], alpha);
        // An IK bend direction (value 2) is +-1 and switches instead of passing through 0.
        if (std::abs(bend) == 1.0f && std::abs(b[2]) == 1.0f) a[2] = discrete ? b[2] : bend;
    }
}

void applyAnimationMix(const Skeleton& sk, const Animation& anim, float time, float alpha, Pose& pose) {
    if (alpha <= 0) return;
    Pose target = pose;
    applyAnimation(sk, anim, time, target);
    mixPose(pose, target, alpha);
}

float TrackEntry::animationTime(const Skeleton& sk) const {
    if (animation < 0 || animation >= (int)sk.animations.size()) return 0;
    float d = sk.animations[animation].duration;
    if (d <= 0) return 0;
    if (loop) return std::fmod(std::max(0.0f, trackTime), d);
    return std::clamp(trackTime, 0.0f, d);
}

void AnimationState::setMix(int from, int to, float duration) { mixes_[{from, to}] = std::max(0.0f, duration); }

float AnimationState::mixFor(int from, int to) const {
    auto it = mixes_.find({from, to});
    return it != mixes_.end() ? it->second : defaultMix;
}

AnimationState::Track& AnimationState::track(int index) {
    if (index >= (int)tracks_.size()) tracks_.resize(index + 1);
    return tracks_[index];
}

void AnimationState::start(int trackIndex, TrackEntry e) {
    Track& t = track(trackIndex);
    if (t.chain.empty() && trackIndex == 0) e.mixDuration = 0;  // nothing below track 0 to fade from
    t.chain.push_back(std::move(e));
    // Keep the crossfade chain bounded when animations are switched faster than they mix.
    if (t.chain.size() > 8) t.chain.erase(t.chain.begin(), t.chain.end() - 8);
}

void AnimationState::setAnimation(int trackIndex, int animation, bool loop) {
    if (trackIndex < 0) return;
    Track& t = track(trackIndex);
    t.queue.clear();
    TrackEntry e;
    e.animation = animation;
    e.loop = loop;
    e.mixDuration = mixFor(t.chain.empty() ? -1 : t.chain.back().animation, animation);
    start(trackIndex, e);
}

void AnimationState::addAnimation(const Skeleton& sk, int trackIndex, int animation, bool loop, float delay) {
    if (trackIndex < 0) return;
    Track& t = track(trackIndex);
    const TrackEntry* last = !t.queue.empty() ? &t.queue.back() : !t.chain.empty() ? &t.chain.back() : nullptr;
    if (!last) {
        setAnimation(trackIndex, animation, loop);
        return;
    }
    TrackEntry e;
    e.animation = animation;
    e.loop = loop;
    e.mixDuration = mixFor(last->animation, animation);
    if (delay <= 0) {
        // End of the previous animation; for one that is already looping, the end of its current cycle.
        float d = last->animation >= 0 && last->animation < (int)sk.animations.size() ? sk.animations[last->animation].duration : 0;
        if (last->loop && d > 0) d *= std::floor(last->trackTime / d) + 1;
        delay += std::max(0.0f, d - e.mixDuration);
    }
    e.delay = delay;
    t.queue.push_back(e);
}

void AnimationState::setEmptyAnimation(int trackIndex, float mixDuration) {
    if (trackIndex < 0 || trackIndex >= (int)tracks_.size() || tracks_[trackIndex].chain.empty()) return;
    Track& t = tracks_[trackIndex];
    t.queue.clear();
    TrackEntry e;
    e.animation = -1;
    e.mixDuration = std::max(0.0f, mixDuration);
    t.chain.push_back(e);
}

void AnimationState::clearTrack(int trackIndex) {
    if (trackIndex < 0 || trackIndex >= (int)tracks_.size()) return;
    tracks_[trackIndex].chain.clear();
    tracks_[trackIndex].queue.clear();
}

void AnimationState::clearTracks() {
    for (Track& t : tracks_) {
        t.chain.clear();
        t.queue.clear();
    }
}

void AnimationState::setTrackAlpha(int trackIndex, float alpha) {
    if (trackIndex >= 0) track(trackIndex).alpha = std::clamp(alpha, 0.0f, 1.0f);
}

float AnimationState::trackAlpha(int trackIndex) const {
    return trackIndex >= 0 && trackIndex < (int)tracks_.size() ? tracks_[trackIndex].alpha : 1.0f;
}

const TrackEntry* AnimationState::current(int trackIndex) const {
    if (trackIndex < 0 || trackIndex >= (int)tracks_.size() || tracks_[trackIndex].chain.empty()) return nullptr;
    return &tracks_[trackIndex].chain.back();
}

int AnimationState::mixingCount(int trackIndex) const {
    return trackIndex >= 0 && trackIndex < (int)tracks_.size() ? (int)tracks_[trackIndex].chain.size() : 0;
}

int AnimationState::queuedCount(int trackIndex) const {
    return trackIndex >= 0 && trackIndex < (int)tracks_.size() ? (int)tracks_[trackIndex].queue.size() : 0;
}

namespace {
// Events of `e` between its eventTime and trackTime (loops unrolled); advances eventTime.
void collectEvents(const Skeleton& sk, int trackIndex, TrackEntry& e, std::vector<FiredEvent>& out) {
    if (e.animation < 0 || e.animation >= (int)sk.animations.size()) return;
    const Animation& anim = sk.animations[e.animation];
    const float from = e.eventTime, to = e.trackTime;
    e.eventTime = to;
    if (to < 0 || (from >= 0 && to <= from)) return;
    auto emit = [&](float a, float b) {
        for (const EventKey* k : eventsInRange(anim, a, b)) out.push_back({trackIndex, e.animation, *k});
    };
    const float d = anim.duration;
    if (!e.loop || d <= 0) {
        if (from < d || from < 0) emit(from, std::min(to, std::max(d, 0.0f)));
        return;
    }
    int c0 = from < 0 ? 0 : (int)std::floor(from / d), c1 = (int)std::floor(to / d);
    c0 = std::max(c0, c1 - 100);  // a huge time step only replays the last cycles
    for (int c = c0; c <= c1; ++c) {
        float a = c == c0 && from >= 0 ? from - c * d : -1.0f;
        float b = c == c1 ? to - c * d : d;
        emit(a, b);
    }
}
} // namespace

std::vector<FiredEvent> AnimationState::update(const Skeleton& sk, float dt) {
    std::vector<FiredEvent> events;
    const float d = std::max(0.0f, dt * timeScale);
    for (int ti = 0; ti < (int)tracks_.size(); ++ti) {
        Track& t = tracks_[ti];
        if (t.chain.empty() && !t.queue.empty()) {
            TrackEntry e = t.queue.front();
            t.queue.erase(t.queue.begin());
            start(ti, e);
        }
        for (TrackEntry& e : t.chain) {
            e.trackTime += d;
            e.mixTime += d;
        }
        if (!t.chain.empty()) collectEvents(sk, ti, t.chain.back(), events);
        // Start queued entries whose time has come (carrying the overshoot).
        while (!t.queue.empty() && !t.chain.empty() && t.chain.back().trackTime >= t.queue.front().delay) {
            TrackEntry e = t.queue.front();
            t.queue.erase(t.queue.begin());
            float over = t.chain.back().trackTime - e.delay;
            e.trackTime = e.mixTime = over;
            start(ti, e);
            collectEvents(sk, ti, t.chain.back(), events);
        }
        // Entries hidden under a fully mixed-in later entry are done.
        for (int i = (int)t.chain.size() - 1; i > 0; --i)
            if (t.chain[i].mixFraction() >= 1.0f) {
                t.chain.erase(t.chain.begin(), t.chain.begin() + i);
                break;
            }
        if (t.chain.size() == 1 && t.chain[0].animation < 0 && t.queue.empty()) t.chain.clear();
    }
    return events;
}

void AnimationState::apply(const Skeleton& sk, Pose& pose) const {
    for (const Track& t : tracks_) {
        if (t.chain.empty() || t.alpha <= 0) continue;
        Pose mixed = pose;
        for (const TrackEntry& e : t.chain) {
            Pose target = pose;  // each entry is applied over the tracks below, then crossfaded in
            if (e.animation >= 0 && e.animation < (int)sk.animations.size())
                applyAnimation(sk, sk.animations[e.animation], e.animationTime(sk), target);
            mixPose(mixed, target, e.mixFraction());
        }
        mixPose(pose, mixed, t.alpha);
    }
}

} // namespace rig
