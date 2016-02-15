struct VOut
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

VOut VShader(uint vertexId : SV_VertexID)
{
	VOut output;
	output.position.x = (float)(vertexId / 2) * 4 - 1;
	output.position.y = (float)(vertexId % 2) * 4 - 1;
	output.position.z = 1;
	output.position.w = 1;

	output.texcoord.x = (float)(vertexId / 2) * 2;
	output.texcoord.y = 1 - (float)(vertexId % 2) * 2;

	return output;
}

TextureCube<float4> Skybox : register(t0);
SamplerState    	Sampler : register(s0);

cbuffer Frame : register(b0) 
{
	matrix Projection;
	matrix View;
	matrix InvView;
}

float4 PShader(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	const float rayX = 1./Projection._11;
	const float rayY = 1./Projection._22;
	float3 screenRay = float3(lerp( -rayX, rayX, texcoord.x ), lerp( rayY, -rayY, texcoord.y ), 1.);

	return Skybox.Sample(Sampler, mul(screenRay, transpose((float3x3)View)));
}
