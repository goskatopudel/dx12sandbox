// Texture2D<float>	DepthBuffer : register( t0 );
// RWTexture2D<uint> 	PagesTexture : register( u0 );

// matrix				ShadowmapMatrix;

// [numthreads( 8, 8, 1 )]
// void PreparePages( uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID )
// {
// 	float depth = DepthBuffer[DTid];

// 	float3 world_pos;

// 	float4 smpos = mul(float4(world_pos, 1), ShadowmapMatrix);
// 	smpos /= smpos.w;

// 	uint2 page;

// 	InterlockedMax(PagesTexture[page], )
// }

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
	output.texcoord.y= 1 - (float)(vertexId % 2) * 2;

	return output;
}

Texture2D<uint> 	Image : register(t0);

float4 CopyUintPS(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	uint val = Image[texcoord * 128].r;
	if(val == 0) discard;
	return val.rrrr / 32.f;
}