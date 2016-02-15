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

Texture2D<float> 	DepthBuffer : register(t0);
Texture2D<float4> 	GBufferA : register(t1);
Texture2D<float4> 	GBufferB : register(t2);
SamplerState    	Sampler : register(s0);

cbuffer Frame : register(b0) 
{
	matrix Projection;
	matrix View;
	matrix InvView;
}


float2 OctWrap( float2 v )
{
    return ( 1.0 - abs( v.yx ) ) * ( v.xy >= 0.0 ? 1.0 : -1.0 );
}
 
float3 Decode( float2 encN )
{
    encN = encN * 2.0 - 1.0;
 
    float3 n;
    n.z = 1.0 - abs( encN.x ) - abs( encN.y );
    n.xy = n.z >= 0.0 ? encN.xy : OctWrap( encN.xy );
    n = normalize( n );
    return n;
}

float linearizeDepth(float depth, float proj33, float proj43) {
	return proj43 / (depth - proj33);
}

float4 PShader_Depth(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return linearizeDepth(DepthBuffer[position.xy].x, Projection._33, Projection._43) / 500;
}

float4 PShader_Albedo(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return float4(GBufferA[position.xy].xyz, 1);
}

float4 PShader_Normals(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	uint3 bituN = GBufferB[position.xy].xyz * 255;
	float2 pN = (bituN.xy << 4) / 4095.f;
	pN += (uint2(bituN.z >> 4, bituN.z) & 15) / 4095.f;
	return float4(Decode(pN), 1);
}

float4 PShader_Roughness(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return float4(GBufferB[position.xy].www, 1);
}

float4 PShader_Metalness(float4 position : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
	return float4(GBufferA[position.xy].www, 1);
}

