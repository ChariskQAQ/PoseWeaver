using System;
using UnityEngine;

namespace Poseweaver
{
    [DisallowMultipleComponent, RequireComponent(typeof(SpriteRenderer))]
    public sealed class PoseweaverPlayer : MonoBehaviour
    {
        public PoseweaverFrames source;
        public string animationName;
        public bool playOnStart = true;
        [Min(0)] public float speed = 1;
        public bool IsPlaying { get; private set; }
        PoseweaverClip clip;
        SpriteRenderer target;
        double[] ends;
        double time;

        void Start() { if (playOnStart) Play(animationName); }
        void Update() { Advance(Time.deltaTime); }

        public bool Play(string name)
        {
            if (source == null || source.animations == null) return false;
            var next = Array.Find(source.animations, item => item != null && item.name == name);
            if (next == null || next.frames == null || next.frames.Length == 0 || next.durations == null ||
                next.frames.Length != next.durations.Length) return false;
            var nextEnds = new double[next.frames.Length];
            double total = 0;
            for (int i = 0; i < nextEnds.Length; ++i)
            {
                float duration = next.durations[i];
                if (next.frames[i] == null || float.IsNaN(duration) || float.IsInfinity(duration) || duration <= 0) return false;
                total += duration;
                nextEnds[i] = total;
            }
            clip = next;
            ends = nextEnds;
            target = GetComponent<SpriteRenderer>();
            animationName = name;
            time = 0;
            target.sprite = clip.frames[0];
            IsPlaying = true;
            return true;
        }

        public void Pause() { IsPlaying = false; }
        public void Resume() { if (clip != null) IsPlaying = true; }
        public void Stop()
        {
            IsPlaying = false;
            time = 0;
            if (clip != null && target != null) target.sprite = clip.frames[0];
        }

        // Also useful for a game's explicit animation clock.
        public void Advance(float seconds)
        {
            if (!IsPlaying || clip == null || target == null || !IsFinite(seconds) || !IsFinite(speed) || seconds < 0 || speed < 0) return;
            time += (double)seconds * speed;
            double total = ends[ends.Length - 1];
            if (time >= total)
            {
                if (clip.loop) time %= total;
                else { time = total; IsPlaying = false; }
            }
            int frame = Array.BinarySearch(ends, time);
            frame = frame >= 0 ? frame + 1 : ~frame;
            target.sprite = clip.frames[Math.Min(frame, clip.frames.Length - 1)];
        }

        static bool IsFinite(float value) { return !float.IsNaN(value) && !float.IsInfinity(value); }
    }
}
