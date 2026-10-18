#pragma once
// Animation mixing: several tracks layered on top of each other, crossfades between the animations of a
// track, queued animations and event collection (what a game runtime does with the exported data).
#include "core/pose.h"

#include <algorithm>
#include <map>

namespace rig {

// Moves `pose` towards `target` by `alpha` (0 = unchanged, 1 = target). Rotations take the short way,
// attachments / draw order / IK bend switch at alpha >= 0.5. Bone activity and skins are not touched.
void mixPose(Pose& pose, const Pose& target, float alpha);
// Applies `anim` at `time` on top of `pose` with weight `alpha`.
void applyAnimationMix(const Skeleton& sk, const Animation& anim, float time, float alpha, Pose& pose);

struct TrackEntry {
    int animation = -1;      // index into Skeleton::animations, -1 = empty (fades the track out)
    bool loop = true;
    float trackTime = 0;     // seconds since the entry started
    float mixDuration = 0;   // crossfade length from the previous entry
    float mixTime = 0;       // seconds of crossfade done
    float delay = 0;         // queued entries: start once the previous entry's trackTime reaches this
    float eventTime = -1;    // trackTime events were collected up to (-1 = none yet, so time 0 fires)

    float mixFraction() const { return mixDuration <= 0 ? 1.0f : std::min(1.0f, mixTime / mixDuration); }
    // Time inside the animation (wrapped when looping, held at the end otherwise).
    float animationTime(const Skeleton& sk) const;
};

struct FiredEvent {
    int track = 0;
    int animation = -1;
    EventKey key;
};

class AnimationState {
public:
    float defaultMix = 0.2f;
    float timeScale = 1.0f;

    // Crossfade length for a specific pair (from -1 = fading in on an empty track).
    void setMix(int from, int to, float duration);
    float mixFor(int from, int to) const;
    void clearMix(int from, int to) { mixes_.erase({from, to}); }
    const std::map<std::pair<int, int>, float>& mixes() const { return mixes_; }

    // Replaces the track's animation (crossfading from what plays now) and drops its queue.
    void setAnimation(int track, int animation, bool loop);
    // Queues an animation after the last one on the track. delay <= 0: relative to the end of the previous
    // animation (of its current cycle when looping) minus the mix duration, so the crossfade ends there.
    void addAnimation(const Skeleton& sk, int track, int animation, bool loop, float delay = 0);
    // Fades the track out to the tracks below it (or to the setup pose for track 0).
    void setEmptyAnimation(int track, float mixDuration);
    void clearTrack(int track);
    void clearTracks();

    void setTrackAlpha(int track, float alpha);
    float trackAlpha(int track) const;
    int trackCount() const { return (int)tracks_.size(); }
    // The entry that is playing (last of the crossfade chain), or null.
    const TrackEntry* current(int track) const;
    // Number of entries still being blended on the track (current included).
    int mixingCount(int track) const;
    int queuedCount(int track) const;

    // Advances time, starts queued entries, finishes crossfades; returns the events passed.
    std::vector<FiredEvent> update(const Skeleton& sk, float dt);
    // Applies every track in order on top of `pose` (normally freshly reset to setup). Call updateWorld after.
    void apply(const Skeleton& sk, Pose& pose) const;

private:
    struct Track {
        std::vector<TrackEntry> chain;  // oldest first; the last one is current
        std::vector<TrackEntry> queue;
        float alpha = 1.0f;
    };
    std::vector<Track> tracks_;
    std::map<std::pair<int, int>, float> mixes_;

    Track& track(int index);
    void start(int trackIndex, TrackEntry entry);
};

} // namespace rig
