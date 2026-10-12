// M1 world-pass probe geometry: unlit coloured boxes in Skyrim's frame (depth-tested, motion vectors).
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;      // camera-relative Skyrim units -> clip (Skyrim's worldToCam, re-based)
	row_major float4x4 prevViewProj;  // last frame's, re-based on this frame's camera
};
cbuffer Object : register(b1)
{
	float4 offset;  // xyz: camera-relative position of the box centre; w: half extent
	float4 color;
};

struct VSOut
{
	float4 pos : SV_Position;
	float4 curClip : TEXCOORD0;
	float4 prevClip : TEXCOORD1;
};

VSOut VSMain(float3 p : POSITION)
{
	VSOut o;
	const float3 rel = offset.xyz + p * offset.w;
	o.pos = mul(viewProj, float4(rel, 1.0));
	o.curClip = o.pos;
	o.prevClip = mul(prevViewProj, float4(rel, 1.0));
	return o;
}

struct PSOut
{
	float4 color : SV_Target0;
	float4 motion : SV_Target1;  // Skyrim's TAA convention: previous uv - current uv
};

PSOut PSMain(VSOut i)
{
	PSOut o;
	const float2 cur = i.curClip.xy / i.curClip.w, prev = i.prevClip.xy / i.prevClip.w;
	o.color = color;
	o.motion = float4(float2(-0.5, 0.5) * (cur - prev), 0.0, 1.0);
	return o;
}
