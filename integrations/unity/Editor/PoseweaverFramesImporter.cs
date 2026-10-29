using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace Poseweaver.Editor
{
    [ScriptedImporter(1, "pwframes")]
    public sealed class PoseweaverFramesImporter : ScriptedImporter
    {
        [Serializable] sealed class Page { public string file; public int width, height; }
        [Serializable] sealed class Frame { public int page, x, y, width, height; public float duration; }
        [Serializable] sealed class Animation { public string name; public bool loop; public float duration; public Frame[] frames; }
        [Serializable] sealed class Manifest
        {
            public string format;
            public string filter;
            public int version, frameWidth, frameHeight;
            public float fps, pixelsPerUnit, originX, originY;
            public Page[] pages;
            public Animation[] animations;
        }

        static void Require(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
        static bool Finite(float value) { return !float.IsNaN(value) && !float.IsInfinity(value); }

        public override void OnImportAsset(AssetImportContext ctx)
        {
            GameObject root = null;
            var created = new List<UnityEngine.Object>();
            try
            {
                Require(new FileInfo(ctx.assetPath).Length <= 4 * 1024 * 1024, "Manifest exceeds 4 MiB.");
                var doc = JsonUtility.FromJson<Manifest>(File.ReadAllText(ctx.assetPath));
                Require(doc != null && doc.format == "poseweaver.frames" && doc.version == 1, "Unsupported Poseweaver format/version.");
                Require(doc.filter == null || doc.filter == "linear" || doc.filter == "nearest", "Unsupported texture filter.");
                Require(doc.frameWidth > 0 && doc.frameWidth <= 4096 && doc.frameHeight > 0 && doc.frameHeight <= 4096 &&
                    Finite(doc.fps) && doc.fps >= 1 && doc.fps <= 120 && Finite(doc.pixelsPerUnit) && doc.pixelsPerUnit > 0 && doc.pixelsPerUnit <= 10000 &&
                    Finite(doc.originX) && doc.originX >= 0 && doc.originX <= doc.frameWidth &&
                    Finite(doc.originY) && doc.originY >= 0 && doc.originY <= doc.frameHeight, "Invalid dimensions, rate or origin.");
                Require(doc.pages != null && doc.pages.Length > 0 && doc.pages.Length <= 10000 &&
                    doc.animations != null && doc.animations.Length > 0 && doc.animations.Length <= 256, "Empty or oversized package.");
                string directory = Path.GetFullPath(Path.GetDirectoryName(ctx.assetPath)) + Path.DirectorySeparatorChar;
                var textures = new Texture2D[doc.pages.Length];
                for (int i = 0; i < textures.Length; ++i)
                {
                    var page = doc.pages[i];
                    Require(page != null && !string.IsNullOrEmpty(page.file) && !Path.IsPathRooted(page.file) &&
                        page.width > 0 && page.width <= 4096 && page.height > 0 && page.height <= 4096, "Invalid sprite page.");
                    string full = Path.GetFullPath(Path.Combine(directory, page.file));
                    Require(full.StartsWith(directory, StringComparison.Ordinal), "Sprite path escapes the package folder.");
                    string dependency = Path.GetDirectoryName(ctx.assetPath).Replace('\\', '/') + "/" + page.file.Replace('\\', '/');
                    ctx.DependsOnSourceAsset(dependency);
                    byte[] bytes = File.ReadAllBytes(full);
                    // Validate PNG dimensions before allowing the decoder to allocate its image.
                    Require(bytes.Length >= 24 && bytes[0] == 137 && bytes[1] == 80 && bytes[2] == 78 && bytes[3] == 71 &&
                        ReadBigEndian(bytes, 16) == page.width && ReadBigEndian(bytes, 20) == page.height, "PNG page dimensions do not match the manifest.");
                    var texture = new Texture2D(2, 2, TextureFormat.RGBA32, false);
                    created.Add(texture);
                    Require(texture.LoadImage(bytes, false), "Cannot decode sprite page.");
                    texture.name = "page_" + i;
                    texture.wrapMode = TextureWrapMode.Clamp;
                    texture.filterMode = doc.filter == "nearest" ? FilterMode.Point : FilterMode.Bilinear;
                    textures[i] = texture;
                }
                var source = ScriptableObject.CreateInstance<PoseweaverFrames>();
                created.Add(source);
                source.name = Path.GetFileNameWithoutExtension(ctx.assetPath);
                source.animations = new PoseweaverClip[doc.animations.Length];
                var names = new HashSet<string>();
                int totalFrames = 0;
                var sprites = new List<KeyValuePair<string, Sprite>>();
                for (int a = 0; a < doc.animations.Length; ++a)
                {
                    var animation = doc.animations[a];
                    Require(animation != null && !string.IsNullOrEmpty(animation.name) && names.Add(animation.name) &&
                        animation.frames != null && animation.frames.Length > 0 && Finite(animation.duration) && animation.duration > 0,
                        "Invalid or duplicate animation.");
                    totalFrames += animation.frames.Length;
                    Require(totalFrames <= 10000, "Package exceeds 10000 frames.");
                    var clip = new PoseweaverClip { name = animation.name, loop = animation.loop,
                        frames = new Sprite[animation.frames.Length], durations = new float[animation.frames.Length] };
                    double duration = 0;
                    for (int f = 0; f < clip.frames.Length; ++f)
                    {
                        var frame = animation.frames[f];
                        Require(frame != null && frame.page >= 0 && frame.page < textures.Length && frame.x >= 0 && frame.y >= 0 &&
                            frame.width == doc.frameWidth && frame.height == doc.frameHeight && Finite(frame.duration) && frame.duration > 0,
                            "Invalid frame.");
                        var texture = textures[frame.page];
                        Require(frame.x <= texture.width - frame.width && frame.y <= texture.height - frame.height, "Frame is outside its page.");
                        var sprite = Sprite.Create(texture, new Rect(frame.x, texture.height - frame.y - frame.height, frame.width, frame.height),
                            new Vector2(doc.originX / frame.width, 1 - doc.originY / frame.height), doc.pixelsPerUnit, 0, SpriteMeshType.FullRect);
                        created.Add(sprite);
                        sprite.name = animation.name + "_" + f;
                        sprites.Add(new KeyValuePair<string, Sprite>("sprite/" + animation.name + "/" + f, sprite));
                        clip.frames[f] = sprite;
                        clip.durations[f] = frame.duration;
                        duration += frame.duration;
                    }
                    Require(Math.Abs(duration - animation.duration) <= Math.Max(0.0001, animation.duration * 0.00001), "Frame durations do not match animation duration.");
                    source.animations[a] = clip;
                }
                root = new GameObject(source.name);
                var renderer = root.AddComponent<SpriteRenderer>();
                renderer.sprite = source.animations[0].frames[0];
                var player = root.AddComponent<PoseweaverPlayer>();
                player.source = source;
                player.animationName = source.animations[0].name;
                for (int i = 0; i < textures.Length; ++i) ctx.AddObjectToAsset("texture/" + i, textures[i]);
                foreach (var sprite in sprites) ctx.AddObjectToAsset(sprite.Key, sprite.Value);
                ctx.AddObjectToAsset("animations", source);
                ctx.AddObjectToAsset("main", root);
                ctx.SetMainObject(root);
            }
            catch (Exception error)
            {
                if (root != null) DestroyImmediate(root);
                foreach (var item in created) if (item != null) DestroyImmediate(item);
                ctx.LogImportError("Poseweaver: " + error.Message);
                throw;
            }
        }

        static uint ReadBigEndian(byte[] bytes, int offset)
        {
            return ((uint)bytes[offset] << 24) | ((uint)bytes[offset + 1] << 16) | ((uint)bytes[offset + 2] << 8) | bytes[offset + 3];
        }
    }
}
