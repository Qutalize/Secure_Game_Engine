Shader "Range/HUD" {
    SubShader {
        Tags { "Queue"="Overlay" }
        Pass {
            Cull Off ZWrite Off ZTest Always
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            struct In { float4 vertex:POSITION; float4 color:COLOR; };
            struct Out { float4 pos:SV_POSITION; float4 color:COLOR; };
            Out vert(In v) { Out o; o.pos=UnityObjectToClipPos(v.vertex); o.color=v.color; return o; }
            float4 frag(Out i):SV_Target { return i.color; }
            ENDCG
        }
    }
}
