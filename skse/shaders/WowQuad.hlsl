// WoW particles and ribbons (M8): quads already placed in the world on the CPU (wowcore::Emitters), each with its
// colour, textured and blended by its emitter's M2 blend mode.
cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj;      // camera-relative Skyrim units -> clip
	row_major float4x4 prevViewProj;
};
cbuffer Quads : register(b1)
{
	float4 material;   // x: alpha test threshold (0: none)
};

Texture2D    tex : register(t0);
SamplerState samp : register(s0);

struct VSIn
{
	float3 pos : POSITION;   // camera-relative
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};
struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

VSOut VSMain(VSIn v)
{
	VSOut o;
	o.pos = mul(viewProj, float4(v.pos, 1.0));
	o.uv = v.uv;
	o.color = v.color;
	return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
	const float4 t = tex.Sample(samp, i.uv) * i.color;
	if (material.x > 0 && t.a < material.x) {
		discard;
	}
	return t;
}
