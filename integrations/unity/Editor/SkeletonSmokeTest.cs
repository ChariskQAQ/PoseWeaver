using System;
using UnityEditor;
using UnityEngine;

namespace Poseweaver.Editor
{
    public static class SkeletonSmokeTest
    {
        public const string Sample = "Assets/RuntimeDemo/robot.pwskeleton";
        static void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Poseweaver skeleton test: " + message);
        }
        public static void Run()
        {
            AssetDatabase.ImportAsset(Sample, ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(Sample);
            Check(prefab != null, "Import did not produce a prefab");
            var instance = UnityEngine.Object.Instantiate(prefab);
            try
            {
                var player = instance.GetComponent<PoseweaverSkeleton>(); player.playing = false;
                Check(player.Load(player.source), player.LastError);
                Check(player.BoneNames.Length == 15 && player.SkinNames.Length == 2 && player.AnimationNames.Length == 3, "Metadata");
                int events = 0; player.AnimationEvent += item => ++events;
                Check(player.Play("wave", false, 0, 0) && player.Advance(.5f), "Playback");
                var beforeStart = player.BonePosition("forearm_r");
                player.SendMessage("Start");
                Check(player.Advance(0) && Vector2.Distance(player.BonePosition("forearm_r"), beforeStart) < .01f, "Start must preserve an explicitly loaded character");
                Check(instance.GetComponentsInChildren<MeshRenderer>().Length == 12 && events > 0, "Meshes and events");
                Check(player.SetSkins("ember") && player.Advance(0), "Skin");
                Check(player.SetBonePosition("foot_target_l", new Vector2(65, -80)) && player.Advance(0), "IK target");
                Check(Vector2.Distance(player.BonePosition("foot_target_l"), new Vector2(65, -80)) < .01f, "IK target position");
                Check(player.ClearBone("foot_target_l"), "Clear target");
                Check(player.Play("walk", true, 0, .1f) && player.Play("wave", false, 1, .1f), "Mix tracks");
                Check(player.SetTrackAlpha(1, .5f) && player.Advance(.2f), "Track alpha");
                Check(player.Queue("idle") && player.FadeTrack(1, .1f) && player.Advance(.2f), "Queue and fade");
                Check(!player.Play("missing") && player.Advance(0), "Unknown animation rejected without losing instance");
                player.pixelsPerUnit = 0;
                Check(!player.Advance(.1f), "Invalid scale rejected");
                player.pixelsPerUnit = 100;
                Check(player.Advance(0), "Valid scale restored");
                var invalid = ScriptableObject.CreateInstance<PoseweaverSkeletonAsset>();
                try
                {
                    invalid.document = "{}"; invalid.images = player.source.images; invalid.shader = player.source.shader;
                    var previous = player.source;
                    Check(!player.Load(invalid) && player.source == previous && player.Advance(0), "Failed load preserves source and playback");
                }
                finally { UnityEngine.Object.DestroyImmediate(invalid); }
                player.autoplay = "missing";
                Check(!player.Load(player.source) && player.Advance(0), "Invalid autoplay preserves playback");
                player.autoplay = "";
            }
            finally { UnityEngine.Object.DestroyImmediate(instance); }
            Debug.Log("Poseweaver Unity skeleton: passed");
        }
    }
}
