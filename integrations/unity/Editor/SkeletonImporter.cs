using System;
using System.IO;
using UnityEditor;
using UnityEditor.AssetImporters;
using UnityEngine;
namespace Poseweaver.Editor
{
    [ScriptedImporter(1, "pwskeleton")]
    public sealed class SkeletonImporter : ScriptedImporter
    {
        [Serializable] sealed class ImageReference { public string path; public int width, height; }
        [Serializable] sealed class Document { public ImageReference[] images; }
        public override void OnImportAsset(AssetImportContext context)
        {
            GameObject root = null;
            PoseweaverSkeletonAsset asset = null;
            try
            {
                if (new FileInfo(context.assetPath).Length > 16 * 1024 * 1024) throw new InvalidDataException("Skeleton exceeds 16 MiB");
                var text = File.ReadAllText(context.assetPath);
                var error = PoseweaverSkeleton.ValidateDocument(text);
                if (error != null) throw new InvalidDataException(error);
                var doc = JsonUtility.FromJson<Document>(text);
                if (doc == null || doc.images == null || doc.images.Length > 10000) throw new InvalidDataException("Invalid skeleton images");
                var textures = new Texture2D[doc.images.Length];
                var directory = Path.GetDirectoryName(context.assetPath).Replace('\\', '/') + "/";
                var fullDirectory = Path.GetFullPath(directory);
                for (int i = 0; i < textures.Length; ++i)
                {
                    var image = doc.images[i];
                    if (image == null || string.IsNullOrEmpty(image.path) || Path.IsPathRooted(image.path) || image.path.Contains(":") || image.path.Contains("\\")) throw new InvalidDataException("Invalid image path");
                    var full = Path.GetFullPath(Path.Combine(directory, image.path));
                    if (!full.StartsWith(fullDirectory, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Image path escapes skeleton directory");
                    var relative = directory + image.path;
                    context.DependsOnSourceAsset(relative);
                    context.DependsOnArtifact(relative);
                    textures[i] = AssetDatabase.LoadAssetAtPath<Texture2D>(relative);
                    if (textures[i] == null || textures[i].width != image.width || textures[i].height != image.height) throw new InvalidDataException("Missing or resized texture: " + relative);
                }
                asset = ScriptableObject.CreateInstance<PoseweaverSkeletonAsset>();
                asset.document = text; asset.images = textures; asset.shader = Shader.Find("Poseweaver/Skeleton");
                if (asset.shader == null) throw new InvalidDataException("Install the Poseweaver skeleton shader");
                context.AddObjectToAsset("skeleton", asset);
                root = new GameObject(Path.GetFileNameWithoutExtension(context.assetPath));
                var player = root.AddComponent<PoseweaverSkeleton>(); player.source = asset;
                context.AddObjectToAsset("prefab", root); context.SetMainObject(root);
            }
            catch (Exception error)
            {
                if (root != null) DestroyImmediate(root);
                if (asset != null) DestroyImmediate(asset);
                context.LogImportError("Poseweaver skeleton: " + error.Message);
            }
        }
    }
}
