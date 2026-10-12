// WoW 1.12 models (M2) in Skyrim's frame: skinned on the GPU, lit as the client lights a model (ambient plus one
// directional light, N.L), WoW's blend modes, depth-tested against Skyrim's scene, with motion vectors for its TAA.
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;      // camera-relative Skyrim units -> clip
	row_major float4x4 prevViewProj;  // last frame's, re-based on this frame's camera
};
cbuffer Object : register(b1)
{
	row_major float4x4 world;      // model (yards) -> camera-relative Skyrim units
	row_major float4x4 prevWorld;  // the same, last frame
	float4 lightDir;               // xyz: towards the light, world space
	float4 lightColor;
	float4 ambientColor;
	float4 material;               // x: alpha test threshold (0: none), y: 1 unlit, z: her fade (Prowl), w: flags - 1 the
	                               // face-gen tint (t1) overlaid, 2 normals as given (a Skyrim mesh: not turned to the eye),
	                               // 4 an opaque batch drawn blended at her fade, 8 menu coverage/premultiplication,
	                               // 16 translucent menu coverage (otherwise surviving fragments are opaque)
	float4 tint;                   // the batch's colour and transparency (effects; 1 for her)
	float4 uvRow0;                 // the batch's texture animation: uv' = (dot(row0, (uv, 1)), dot(row1, (uv, 1)))
	float4 uvRow1;
};
cbuffer Bones : register(b2)
{
	row_major float4x4 bones[256];
};

Texture2D    tex : register(t0);
Texture2D    tintTex : register(t1);
SamplerState samp : register(s0);

struct VSIn
{
	float3 pos : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	uint4  index : BLENDINDICES;
	float4 weight : BLENDWEIGHT;
};

struct VSOut
{
	float4 pos : SV_Position;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float4 curClip : TEXCOORD1;
	float4 prevClip : TEXCOORD2;
};

VSOut VSMain(VSIn v)
{
	float4 p = 0;
	float3 n = 0;
	float  total = 0;
	[unroll] for (int i = 0; i < 4; ++i) {
		const float w = v.weight[i];
		if (w > 0) {
			p += w * mul(bones[v.index[i]], float4(v.pos, 1.0));
			n += w * mul((float3x3)bones[v.index[i]], v.normal);
			total += w;
		}
	}
	if (total <= 0) {
		p = float4(v.pos, 1.0);
		n = v.normal;
	}
	p = float4(p.xyz, 1.0);
	VSOut o;
	o.pos = mul(viewProj, mul(world, p));
	o.curClip = o.pos;
	o.prevClip = mul(prevViewProj, mul(prevWorld, p));
	o.normal = mul((float3x3)world, n);
	o.uv = float2(dot(uvRow0.xyz, float3(v.uv, 1.0)), dot(uvRow1.xyz, float3(v.uv, 1.0)));
	return o;
}

struct PSOut
{
	float4 color : SV_Target0;
	float4 motion : SV_Target1;  // Skyrim's TAA convention: previous uv - current uv
};

PSOut PSMain(VSOut i, bool front : SV_IsFrontFace)
{
	float4 t = tex.Sample(samp, i.uv) * tint;
	const uint flags = (uint)material.w;
	if (flags & 1) {   // Skyrim's face-gen tint over the head's diffuse, as an overlay
		const float3 d = t.rgb, k = tintTex.Sample(samp, i.uv).rgb;
		t.rgb = lerp(2 * d * k, 1 - 2 * (1 - d) * (1 - k), step(0.5, d));
	}
	if (material.x > 0 && t.a < material.x) {
		discard;
	}
	float3 light = 1.0;
	if (material.y < 0.5) {
		float3 n = normalize(i.normal);
		n = (front || (flags & 2)) ? n : -n;
		light = ambientColor.rgb + lightColor.rgb * saturate(dot(n, lightDir.xyz));
	}
	PSOut o;
	o.color = float4(t.rgb * light, ((flags & 4) ? 1.0 : t.a) * (1.0 - material.z));
	// Menu targets carry coverage, with premultiplied colour. These flags are never set by the body pass.
	if (flags & 8) {
		o.color.a = (flags & 16) ? saturate(t.a) : 1.0;
		o.color.rgb *= o.color.a;
	}
	const float2 cur = i.curClip.xy / i.curClip.w, prev = i.prevClip.xy / i.prevClip.w;
	o.motion = float4(float2(-0.5, 0.5) * (cur - prev), 0.0, 1.0);
	return o;
}
