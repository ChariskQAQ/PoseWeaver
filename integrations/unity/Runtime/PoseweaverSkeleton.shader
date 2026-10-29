Shader "Poseweaver/Skeleton"
{
    Properties { _MainTex("Texture", 2D) = "white" {} _SrcBlend("Source", Float) = 1 _DstBlend("Destination", Float) = 10 _BlendMode("Mode", Float) = 0 }
    SubShader
    {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" "IgnoreProjector"="True" }
        Cull Off ZWrite Off Blend [_SrcBlend] [_DstBlend]
        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            sampler2D _MainTex;
            struct Input { float4 vertex:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR; };
            struct Output { float4 vertex:SV_POSITION; float2 uv:TEXCOORD0; float4 color:COLOR; };
            Output vert(Input input) { Output output; output.vertex=UnityObjectToClipPos(input.vertex); output.uv=input.uv; output.color=input.color; return output; }
            fixed4 frag(Output input):SV_Target { fixed4 color=tex2D(_MainTex,input.uv)*input.color; color.rgb*=color.a; return color; }
            ENDCG
        }
    }
}
