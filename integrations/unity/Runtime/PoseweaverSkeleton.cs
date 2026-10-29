using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

namespace Poseweaver
{
    [Serializable] public sealed class SkeletonEvent { public string name, text, audio; public int track, integer; public float number, volume, balance; }
    [Serializable] public sealed class SkeletonBatch { public int slot, image, blend; public float[] xy, uv, color; public int[] triangles; }
    [Serializable] sealed class SkeletonReply { public bool ok; public string error; public string[] bones, skins, animations, images; public SkeletonBatch[] batches; public float[] world; public SkeletonEvent[] events; }
    [Serializable] sealed class SkeletonRequest
    {
        public string op, data, name, attachment;
        public string[] names;
        public float dt, mix = .2f, delay, x, y, angle, alpha = 1, speed = 1;
        public int track;
        public bool loop = true, position, rotate;
    }
    [DisallowMultipleComponent]
    public sealed class PoseweaverSkeleton : MonoBehaviour
    {
        public PoseweaverSkeletonAsset source;
        public string autoplay;
        public bool playing = true;
        [Min(.001f)] public float pixelsPerUnit = 100;
        public int sortingLayerID;
        public int sortingOrder;
        public event Action<SkeletonEvent> AnimationEvent;
        public string LastError { get; private set; }
        public string[] BoneNames { get; private set; }
        public string[] SkinNames { get; private set; }
        public string[] AnimationNames { get; private set; }
        IntPtr runtime;
        float[] world;
        readonly System.Collections.Generic.List<MeshFilter> parts = new System.Collections.Generic.List<MeshFilter>();
        Material[] materials;
        [DllImport("poseweaver_runtime", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr pw_create();
        [DllImport("poseweaver_runtime", CallingConvention = CallingConvention.Cdecl)] static extern void pw_destroy(IntPtr handle);
        [DllImport("poseweaver_runtime", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr pw_request(IntPtr handle, byte[] request);

        void Start() { if (runtime == IntPtr.Zero && source != null) Load(source); }
        void Update() { if (playing && runtime != IntPtr.Zero) Advance(Mathf.Min(Time.deltaTime, 1)); }
        static SkeletonReply Exchange(IntPtr handle, SkeletonRequest request)
        {
            var pointer = pw_request(handle, Encoding.UTF8.GetBytes(JsonUtility.ToJson(request) + "\0"));
            if (pointer == IntPtr.Zero) throw new InvalidOperationException("Missing native reply");
            int length = 0; while (length < 64 * 1024 * 1024 && Marshal.ReadByte(pointer, length) != 0) ++length;
            if (length == 64 * 1024 * 1024) throw new InvalidOperationException("Runtime reply exceeds 64 MiB");
            var bytes = new byte[length]; Marshal.Copy(pointer, bytes, 0, length);
            var reply = JsonUtility.FromJson<SkeletonReply>(Encoding.UTF8.GetString(bytes));
            if (reply == null) throw new InvalidOperationException("Invalid native reply");
            return reply;
        }
        public static string ValidateDocument(string document)
        {
            IntPtr handle = IntPtr.Zero;
            try { handle = pw_create(); var reply = Exchange(handle, new SkeletonRequest { op = "load", data = document }); return reply.ok ? null : reply.error; }
            catch (Exception error) { return error.Message; }
            finally { if (handle != IntPtr.Zero) pw_destroy(handle); }
        }
        SkeletonReply Call(SkeletonRequest request)
        {
            if (runtime == IntPtr.Zero) { LastError = "Load a skeleton first"; return new SkeletonReply(); }
            try
            {
                var reply = Exchange(runtime, request);
                LastError = reply.ok ? null : reply.error;
                return reply;
            }
            catch (Exception error) { LastError = error.Message; return new SkeletonReply(); }
        }
        public bool Load(PoseweaverSkeletonAsset asset)
        {
            if (asset == null || asset.document == null || asset.images == null || float.IsNaN(pixelsPerUnit) || float.IsInfinity(pixelsPerUnit) || pixelsPerUnit <= 0) { LastError = "Missing skeleton source or invalid pixels per unit"; return false; }
            foreach (var texture in asset.images) if (texture == null) { LastError = "Missing skeleton texture"; return false; }
            var shader = asset.shader != null ? asset.shader : Shader.Find("Poseweaver/Skeleton");
            if (shader == null) { LastError = "Include the Poseweaver/Skeleton shader in the player build"; return false; }
            var previous = runtime;
            try { runtime = pw_create(); }
            catch (Exception error) { LastError = error.Message; runtime = previous; return false; }
            var reply = Call(new SkeletonRequest { op = "load", data = asset.document });
            if (!reply.ok || reply.images == null || reply.images.Length != asset.images.Length) { if (reply.ok) LastError = "Skeleton texture count changed; reimport the asset"; if (runtime != IntPtr.Zero) pw_destroy(runtime); runtime = previous; return false; }
            if (!string.IsNullOrEmpty(autoplay) && Array.IndexOf(reply.animations, autoplay) < 0) { LastError = "Unknown autoplay animation: " + autoplay; pw_destroy(runtime); runtime = previous; return false; }
            if (!Call(new SkeletonRequest { op = "step", dt = 0 }).ok) { pw_destroy(runtime); runtime = previous; return false; }
            if (previous != IntPtr.Zero) pw_destroy(previous);
            source = asset; BoneNames = reply.bones; SkinNames = reply.skins; AnimationNames = reply.animations;
            ClearParts(); materials = new Material[3];
            for (int i = 0; i < 3; ++i)
            {
                var material = new Material(shader); material.SetInt("_BlendMode", i);
                material.SetInt("_SrcBlend", (int)(i == 2 ? UnityEngine.Rendering.BlendMode.DstColor : UnityEngine.Rendering.BlendMode.One));
                material.SetInt("_DstBlend", (int)(i == 1 ? UnityEngine.Rendering.BlendMode.One : UnityEngine.Rendering.BlendMode.OneMinusSrcAlpha));
                materials[i] = material;
            }
            if (!string.IsNullOrEmpty(autoplay)) Play(autoplay);
            return Advance(0);
        }
        public bool Play(string name, bool loop = true, int track = 0, float mix = .2f) { return Call(new SkeletonRequest { op = "play", name = name, loop = loop, track = track, mix = mix }).ok; }
        public bool Queue(string name, bool loop = true, int track = 0, float delay = 0, float mix = .2f) { return Call(new SkeletonRequest { op = "queue", name = name, loop = loop, track = track, delay = delay, mix = mix }).ok; }
        public bool SetSkins(params string[] names) { return Call(new SkeletonRequest { op = "skin", names = names }).ok; }
        public bool SetBonePosition(string name, Vector2 localPixels) { return Call(new SkeletonRequest { op = "bone", name = name, position = true, x = localPixels.x, y = localPixels.y }).ok; }
        public bool SetBoneRotation(string name, float degrees) { return Call(new SkeletonRequest { op = "bone", name = name, rotate = true, angle = degrees }).ok; }
        public bool ClearBone(string name) { return Call(new SkeletonRequest { op = "clear_bone", name = name }).ok; }
        public bool SetAttachment(string slot, string attachment) { return Call(new SkeletonRequest { op = "attachment", name = slot, attachment = attachment }).ok; }
        public bool ClearAttachment(string slot) { return Call(new SkeletonRequest { op = "clear_attachment", name = slot }).ok; }
        public bool SetTrackAlpha(int track, float alpha) { return Call(new SkeletonRequest { op = "alpha", track = track, alpha = alpha }).ok; }
        public bool FadeTrack(int track, float mix = .2f) { return Call(new SkeletonRequest { op = "empty", track = track, mix = mix }).ok; }
        public bool SetSpeed(float speed) { return Call(new SkeletonRequest { op = "speed", speed = speed }).ok; }
        public bool ClearTracks() { return Call(new SkeletonRequest { op = "clear" }).ok; }
        public Vector2 BonePosition(string name)
        {
            int index = BoneNames == null ? -1 : Array.IndexOf(BoneNames, name);
            return index < 0 || world == null || world.Length < (index + 1) * 6 ? Vector2.zero : new Vector2(world[index * 6 + 4], world[index * 6 + 5]);
        }
        public bool Advance(float delta)
        {
            if (float.IsNaN(pixelsPerUnit) || float.IsInfinity(pixelsPerUnit) || pixelsPerUnit <= 0) { LastError = "Invalid pixels per unit"; return false; }
            var reply = Call(new SkeletonRequest { op = "step", dt = delta }); if (!reply.ok) return false;
            world = reply.world;
            foreach (var part in parts) part.gameObject.SetActive(false);
            for (int i = 0; i < reply.batches.Length; ++i)
            {
                if (i == parts.Count)
                {
                    var child = new GameObject("Skeleton part"); child.transform.SetParent(transform, false);
                    var filter = child.AddComponent<MeshFilter>(); filter.sharedMesh = new Mesh(); filter.sharedMesh.MarkDynamic();
                    child.AddComponent<MeshRenderer>(); parts.Add(filter);
                }
                var batch = reply.batches[i]; var filterPart = parts[i]; filterPart.gameObject.SetActive(true);
                var vertices = new Vector3[batch.xy.Length / 2]; var uv = new Vector2[vertices.Length]; var colors = new Color[vertices.Length];
                var color = new Color(batch.color[0], batch.color[1], batch.color[2], batch.color[3]);
                for (int v = 0; v < vertices.Length; ++v) { vertices[v] = new Vector3(batch.xy[v * 2] / pixelsPerUnit, batch.xy[v * 2 + 1] / pixelsPerUnit, 0); uv[v] = new Vector2(batch.uv[v * 2], 1 - batch.uv[v * 2 + 1]); colors[v] = color; }
                var mesh = filterPart.sharedMesh; mesh.Clear(); mesh.indexFormat = vertices.Length > 65535 ? UnityEngine.Rendering.IndexFormat.UInt32 : UnityEngine.Rendering.IndexFormat.UInt16;
                mesh.vertices = vertices; mesh.uv = uv; mesh.colors = colors; mesh.triangles = batch.triangles; mesh.RecalculateBounds();
                var renderer = filterPart.GetComponent<MeshRenderer>(); renderer.sharedMaterial = materials[batch.blend];
                var properties = new MaterialPropertyBlock(); properties.SetTexture("_MainTex", source.images[batch.image]); renderer.SetPropertyBlock(properties);
                renderer.sortingLayerID = sortingLayerID; renderer.sortingOrder = sortingOrder + i;
            }
            if (reply.events != null) foreach (var item in reply.events) AnimationEvent?.Invoke(item);
            return true;
        }
        void ClearParts()
        {
            foreach (var part in parts) { if (part.sharedMesh != null) Release(part.sharedMesh); Release(part.gameObject); }
            parts.Clear();
            if (materials != null) foreach (var material in materials) if (material != null) Release(material);
        }
        static void Release(UnityEngine.Object item) { if (Application.isPlaying) Destroy(item); else DestroyImmediate(item); }
        void OnDestroy() { if (runtime != IntPtr.Zero) { pw_destroy(runtime); runtime = IntPtr.Zero; } ClearParts(); }
    }
}
