// Shared receiving functions and the HDR blob/probe passes. Cascade math ports SkyCraft's
// SkyrimSunShadow, SetSkyrimShadows and SkyShadowProbePS (MIT, chasmlol/SkyCraft).
cbuffer Shadow : register(b3)
{
	row_major float4x4 shadowProj[2];
	float4 shadowSplits;             // xy: end distances, w: captured cascade count
	float4 shadowParams;             // xy: copied slices, z: texel size, w: depth direction
	float4 shadowForward;
	float4 shadowViewport;           // xy: bound target size, zw: inverse size
	float4 shadowControl;            // x: usable mask, y: usable cascades, z: receive, w: blob strength
	row_major float4x4 shadowInvVP;
	row_major float4x4 shadowVP;
	float4 probeFeet;
	float4 blobInfo;                 // x: count
	float4 blobCenters[32];          // camera-relative xyz, radius
};
Texture2D shadowMask : register(t2);
Texture2DArray<float> shadowMaps : register(t3);
SamplerState shadowSampler : register(s1); // linear clamp: mask can be quarter resolution
// The sun's channel of the mask (probeFeet.w: BSShadowLight maskIndex 0..3).
float MaskSun(float2 uv)
{
	float4 s = shadowMask.SampleLevel(shadowSampler, uv, 0);
	int c = (int)probeFeet.w;
	return dot(s, float4(c == 0, c == 1, c == 2, c == 3));
}

float FeetSun(float3 feet)
{
	float lit = 0;
	[unroll] for (uint k = 0; k < 8; ++k) {
		float a = 6.28318530718 * k / 8;
		float4 p = mul(shadowVP, float4(feet + float3(cos(a) * 96, sin(a) * 96, 4), 1));
		float2 uv = p.xy / p.w * float2(0.5, -0.5) + 0.5;
		lit += p.w > 0.001 && all(uv >= 0) && all(uv <= 1) ? MaskSun(uv) : 1;
	}
	return saturate(lit / 8);
}

float SkyrimSunShadow(float3 rel, float3 n, float viewZ)
{
	uint cascade = viewZ < shadowSplits.x ? 0 : 1;
	if (cascade >= (uint)shadowSplits.w || viewZ >= (cascade == 0 ? shadowSplits.x : shadowSplits.y)) return 1;
	float4 ls = mul(shadowProj[cascade], float4(rel + n * 6, 1));
	if (abs(ls.w) < 1e-6) return 1;
	ls.xyz /= ls.w;
	if (any(ls.xyz <= 0) || any(ls.xyz >= 1)) return 1;
	int size = (int)round(1 / shadowParams.z);
	int2 texel = (int2)(ls.xy * size);
	int slice = cascade == 0 ? (int)shadowParams.x : (int)shadowParams.y;
	float lit = 0;
	[unroll] for (int y = -1; y <= 1; ++y) {
		[unroll] for (int x = -1; x <= 1; ++x) {
			// Nearest texels, like SkyCraft's pointSampler; s1 stays linear for the mask.
			float d = shadowMaps.Load(int4(clamp(texel + int2(x, y), 0, size - 1), slice, 0));
			lit += shadowParams.w > 0 ? (ls.z - 0.0005 <= d ? 1 : 0) : (ls.z + 0.0005 >= d ? 1 : 0);
		}
	}
	float endDistance = shadowSplits.w > 1.5 ? shadowSplits.y : shadowSplits.x;
	return lerp(lit / 9, 1, saturate((viewZ - endDistance * 0.85) / max(endDistance * 0.15, 1)));
}

float ModelSun(float3 rel, float3 n, float4 feet)
{
	if (feet.w < 0.5 || shadowControl.z < 0.5) return 1;
	if (shadowControl.y > 0.5) return SkyrimSunShadow(rel, n, dot(rel, shadowForward.xyz));
	return shadowControl.x > 0.5 ? FeetSun(feet.xyz) : 1;
}

#ifndef WB_MODEL_SHADOWS
Texture2D<float> sceneDepth : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 PositionAt(float2 uv)
{
	float d = sceneDepth.Load(int3(uv * shadowViewport.xy, 0));
	float4 h = mul(shadowInvVP, float4(uv * float2(2, -2) + float2(-1, 1), d, 1));
	return h.xyz / h.w;
}
float4 BlobPS(float4 pos : SV_Position) : SV_Target
{
	// Exactly DrawDecal/WowDecal's depth reconstruction and derivative normal (DSV unbound).
	float2 uv = pos.xy * shadowViewport.zw;
	float3 p = PositionAt(uv);
	float3 n = normalize(cross(ddx(p), ddy(p)));
	float alpha = 0;
	[loop] for (uint k = 0; k < (uint)blobInfo.x; ++k) {
		float4 blob = blobCenters[k];
		float3 rel = p - blob.xyz;
		float slope = sqrt(saturate(1 - n.z * n.z)) / max(abs(n.z), 0.2);
		if (abs(n.z) >= 0.5 && abs(rel.z) <= length(rel.xy) * slope + blob.w + 12) {
			// Generated radial falloff; not an extracted 1.12 blob texture.
			float falloff = saturate(1 - dot(rel.xy, rel.xy) / (blob.w * blob.w));
			alpha = max(alpha, falloff * falloff);
		}
	}
	float lit = shadowControl.x > 0.5 ? saturate(MaskSun(uv)) : 0;
	return float4(0, 0, 0, saturate(alpha * shadowControl.w * lit));
}
float4 MaskPS(float4 pos : SV_Position) : SV_Target
{
	return float4(shadowMask.SampleLevel(shadowSampler, pos.xy * shadowViewport.zw, 0).rrr, 1);
}
static const float2 kProbe[16] = {
	float2(.1,.1), float2(.3,.1), float2(.5,.1), float2(.7,.1), float2(.9,.1),
	float2(.2,.35), float2(.5,.35), float2(.8,.35),
	float2(.2,.65), float2(.5,.65), float2(.8,.65),
	float2(.1,.9), float2(.3,.9), float2(.5,.9), float2(.7,.9), float2(.9,.9)
};
float4 MaskProbePS(float4 pos : SV_Position) : SV_Target
{
	if ((uint)pos.x == 16) return FeetSun(probeFeet.xyz).xxxx;
	return shadowMask.SampleLevel(shadowSampler, kProbe[(uint)pos.x], 0).rrrr;
}
float4 CascadeProbePS(float4 pos : SV_Position) : SV_Target
{
	// SkyCraft SkyShadowProbePS: projected depth, stored depth, v-flipped depth, cascade/outside marker.
	uint index = (uint)pos.x % 16;
	float2 uv = kProbe[index];
	float3 rel = PositionAt(uv);
	float viewZ = dot(rel, shadowForward.xyz);
	uint cascade = viewZ < shadowSplits.x ? 0 : 1;
	if ((uint)pos.x >= 16) return float4(SkyrimSunShadow(rel, float3(0,0,1), viewZ), MaskSun(uv), viewZ, 1);
	float4 ls = mul(shadowProj[cascade], float4(rel, 1));
	ls.xyz /= ls.w;
	int size = (int)round(1 / shadowParams.z);
	int slice = min(cascade, (uint)max(shadowSplits.w - 1, 0));
	float a = shadowMaps.Load(int4(clamp((int2)(ls.xy * size), 0, size - 1), slice, 0));
	float b = shadowMaps.Load(int4(clamp((int2)(float2(ls.x, 1 - ls.y) * size), 0, size - 1), slice, 0));
	bool outside = any(ls.xyz < 0) || any(ls.xyz > 1) || cascade >= (uint)shadowSplits.w;
	return float4(ls.z, a, b, cascade + (outside ? 10 : 0) + (viewZ >= shadowSplits.y ? 100 : 0));
}
#endif
