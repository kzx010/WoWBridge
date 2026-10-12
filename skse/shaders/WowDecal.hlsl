// WoW's ground decals, laid on whatever Skyrim drew there as the client lays them on the ground: the selection circle
// under the target (Textures\UnitSelectTexture.blp, its alpha in the reaction's colour, turned so its bright edge faces
// the camera), and a ground spell's area under the pointer while it is placed (Interface\SpellShadow, in its own
// colours, over the ground it covers - not what stands well above or below it). A full-screen pass: each pixel's
// position from Skyrim's depth, kept where it is ground inside the circle.
cbuffer Decal : register(b0)
{
	row_major float4x4 invViewProj;  // clip -> camera-relative Skyrim units
	float4 center;                   // xyz: the target's feet, camera-relative; w: the radius
	float4 toCamera;                 // xy: level unit vector from the target towards the camera; z: 1 an area (its own
	                                 // colours, all the ground under it), 0 the selection circle
	float4 color;                    // the reaction's colour
	float4 viewport;                 // xy: the target's size in pixels
};

Texture2D<float> depthTex : register(t0);
Texture2D        tex : register(t1);
SamplerState     samp : register(s0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
	const float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
	const float  d = depthTex.Load(int3(pos.xy, 0));
	const float2 ndc = float2(pos.x / viewport.x * 2 - 1, 1 - pos.y / viewport.y * 2);
	const float4 h = mul(invViewProj, float4(ndc, d, 1));
	const float3 p = h.xyz / h.w;
	const float3 n = normalize(cross(ddx(p), ddy(p)));
	const float3 rel = p - center.xyz;
	const float  r = center.w;
	// Ground: facing up, and no higher or lower than its own slope rises from the feet over that distance (give or take
	// the ground's roughness) - which leaves out walls and legs, and the tops of paws and boots standing on it.
	// Up to about 32 degrees: steeper is a joint or a flank more often than ground.
	const float slope = sqrt(saturate(1 - n.z * n.z)) / max(abs(n.z), 0.2);
	const bool  area = toCamera.z > 0.5;
	if (area ? (abs(n.z) < 0.5 || abs(rel.z) > 0.4 * r) : (abs(n.z) < 0.85 || abs(rel.z) > length(rel.xy) * slope + 6.0)) {
		discard;
	}
	const float2 uv = float2(dot(rel.xy, float2(toCamera.y, -toCamera.x)), dot(rel.xy, toCamera.xy)) / (2 * r) + 0.5;
	if (any(uv < 0) || any(uv > 1)) {
		discard;
	}
	const float4 t = tex.SampleLevel(samp, uv, 0);
	return area ? float4(t.rgb, t.a * color.a) : float4(color.rgb, t.a * color.a);
}
