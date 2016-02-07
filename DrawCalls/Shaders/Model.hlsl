cbuffer Frame : register(b0)
{       
    matrix ViewProj;
}

cbuffer Object : register(b1)
{       
    matrix World;
}

struct VIn 
{
	float3 	position : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
};

struct VOut
{
	float4 	position : SV_POSITION;
	float3 	wposition : POSITION;
	float3 	normal : NORMAL;
	float2 	texcoord : TEXCOORD;
};

Texture2D ColorTex : register(t0);

VOut VShader(VIn input, uint vertexId : SV_VertexID)
{
	VOut output;

	matrix objectMatrix = World;

	float4 position = float4(input.position, 1);
	position = mul(position, objectMatrix);
	output.wposition = position.xyz;
	position = mul(position, ViewProj);
	output.position = position;
	output.normal = mul(input.normal, (float3x3) objectMatrix);
	output.texcoord = input.texcoord;
	return output;
}

void PShader(VOut interpolated, out float4 outColor : SV_TARGET0)
{
	outColor = float4(interpolated.texcoord, 0, 1) * ColorTex[uint2(0,0)];
}