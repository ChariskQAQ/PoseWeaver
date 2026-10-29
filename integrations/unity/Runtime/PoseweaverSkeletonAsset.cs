using UnityEngine;
namespace Poseweaver
{
    public sealed class PoseweaverSkeletonAsset : ScriptableObject
    {
        [TextArea] public string document;
        public Texture2D[] images;
        public Shader shader;
    }
}
