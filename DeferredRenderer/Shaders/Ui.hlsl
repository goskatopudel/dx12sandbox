cbuffer ConstantBuffer : register(b0)
{
	matrix Projection;
};

Texture2D		Image : register(t0);
SamplerState    Sampler : register(s0);

struct VOut
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
	float4 color : COLOR;
};

VOut VShader(float2 position : POSITION, float2 texcoord : TEXCOORD, float4 color : COLOR)
{
	VOut output;

	output.position = mul(float4(position, 0, 1), Projection);
	output.texcoord = texcoord;
	output.color = color;

	return output;
}

float4 PShader(float4 position : SV_POSITION, float2 texcoord : TEXCOORD, float4 color : COLOR) : SV_TARGET
{
	return color * Image.Sample(Sampler, texcoord);
}
