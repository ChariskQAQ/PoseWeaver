using System;
using UnityEngine;

namespace Poseweaver
{
    [Serializable]
    public sealed class PoseweaverClip
    {
        public string name;
        public bool loop;
        public Sprite[] frames;
        public float[] durations;
    }

    public sealed class PoseweaverFrames : ScriptableObject
    {
        public PoseweaverClip[] animations;
    }
}
