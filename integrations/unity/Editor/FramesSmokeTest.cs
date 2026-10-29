using System;
using UnityEditor;
using UnityEngine;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;

namespace Poseweaver.Editor
{
    // Batch entry point: -executeMethod Poseweaver.Editor.FramesSmokeTest.Run -poseweaverSample Assets/Demo/arm.pwframes
    public static class FramesSmokeTest
    {
        [Serializable] class FilterManifest { public string filter; }
        static void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Poseweaver test: " + message);
        }

        public static void Run()
        {
            string[] args = Environment.GetCommandLineArgs();
            int index = Array.IndexOf(args, "-poseweaverSample");
            Check(index >= 0 && index + 1 < args.Length, "Pass -poseweaverSample <asset path>");
            string path = args[index + 1];
            AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate | ImportAssetOptions.ForceSynchronousImport);
            var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
            Check(asset != null, "Import did not produce a GameObject");
            var instance = UnityEngine.Object.Instantiate(asset);
            PoseweaverFrames temporary = null;
            try
            {
                var player = instance.GetComponent<PoseweaverPlayer>();
                var renderer = instance.GetComponent<SpriteRenderer>();
                Check(player != null && renderer != null && player.source != null, "Missing player, renderer or source");
                var manifest = JsonUtility.FromJson<FilterManifest>(System.IO.File.ReadAllText(path));
                Check(renderer.sprite.texture.filterMode == (manifest.filter == "nearest" ? FilterMode.Point : FilterMode.Bilinear), "Wrong texture filter");
                var clip = player.source.animations[0];
                Check(clip.name == "wave" && clip.frames.Length == 30 && clip.loop, "Unexpected demo animation");
                Check(player.Play("wave") && renderer.sprite == clip.frames[0], "Play did not select first frame");
                Check(!player.Play("absent"), "Unknown animation accepted");
                player.Advance(clip.durations[0] * 1.5f);
                Check(renderer.sprite == clip.frames[1], "Wrong frame after advancing");
                player.Pause();
                var paused = renderer.sprite;
                player.Advance(0.5f);
                Check(renderer.sprite == paused && !player.IsPlaying, "Paused playback advanced");
                player.Resume();
                player.Advance(2.5f);
                Check(player.IsPlaying, "Looping playback stopped");
                var pivot = clip.frames[0].pivot;
                foreach (var frame in clip.frames)
                    Check(frame != null && frame.pivot == pivot && frame.pixelsPerUnit == 100, "Inconsistent pivot or scale");
                temporary = ScriptableObject.CreateInstance<PoseweaverFrames>();
                temporary.animations = new[] { new PoseweaverClip { name = "once", loop = false, frames = clip.frames, durations = clip.durations } };
                player.source = temporary;
                Check(player.Play("once"), "Nonlooping clip failed");
                player.Advance(10);
                Check(!player.IsPlaying && renderer.sprite == clip.frames[clip.frames.Length - 1], "Nonlooping playback did not hold last frame");
                player.Stop();
                Check(renderer.sprite == clip.frames[0], "Stop did not reset");
            }
            finally
            {
                UnityEngine.Object.DestroyImmediate(instance);
                if (temporary != null) UnityEngine.Object.DestroyImmediate(temporary);
            }
            Debug.Log("Poseweaver Unity frames: passed");
        }

        // Compile the imported prefab into a real Windows player as a second acceptance gate.
        public static void BuildWindowsPlayer()
        {
            Run();
            SkeletonSmokeTest.Run();
            string[] args = Environment.GetCommandLineArgs();
            int sample = Array.IndexOf(args, "-poseweaverSample"), output = Array.IndexOf(args, "-poseweaverBuild");
            Check(output >= 0 && output + 1 < args.Length, "Pass -poseweaverBuild <exe path>");
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(args[sample + 1]));
            var skeleton = UnityEngine.Object.Instantiate(AssetDatabase.LoadAssetAtPath<GameObject>(SkeletonSmokeTest.Sample));
            skeleton.GetComponent<PoseweaverSkeleton>().autoplay = "walk";
            var camera = new GameObject("Camera").AddComponent<Camera>();
            camera.orthographic = true; camera.orthographicSize = 4;
            camera.transform.position = new Vector3(0, 0, -10);
            const string scenePath = "Assets/PoseweaverAcceptance.unity";
            Check(EditorSceneManager.SaveScene(scene, scenePath), "Could not save acceptance scene");
            var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions {
                scenes = new[] { scenePath }, locationPathName = args[output + 1],
                target = BuildTarget.StandaloneWindows64, options = BuildOptions.Development
            });
            Check(report.summary.result == BuildResult.Succeeded, "Windows player build failed");
            Debug.Log("Poseweaver Unity Windows build: passed");
        }
    }
}
